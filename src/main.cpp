#include <Arduino.h>
#if !WLED_TOUCH_SIMULATOR
#include <Wire.h>
#endif
#include <WiFi.h>
#include <lvgl.h>

#include "app_state.h"
#include "app_config.h"
#include "display.h"
#include "settings.h"
#include "update_manager.h"
#include "update_helper.h"
#include "wifi_link.h"
#include "BatteryMonitor.h"
#include "wled_api.h"
#include "ui/ui.h"
#include "ui/tabs.h"
#if WLED_TOUCH_SIMULATOR
#include "sim_wled.h"
#endif

#if !WLED_TOUCH_SIMULATOR && WLED_CYD_ENABLE_SERIAL_SCREENSHOT
namespace {

void writeLE16(uint8_t* out, uint16_t value) {
  out[0] = value & 0xFF;
  out[1] = (value >> 8) & 0xFF;
}

void writeLE32(uint8_t* out, uint32_t value) {
  out[0] = value & 0xFF;
  out[1] = (value >> 8) & 0xFF;
  out[2] = (value >> 16) & 0xFF;
  out[3] = (value >> 24) & 0xFF;
}

class BmpTftReadbackStream : public Stream {
 public:
  BmpTftReadbackStream()
      : row_stride_(((uint32_t)kScreenWidth * 3 + 3) & ~uint32_t(3)),
        size_(54 + row_stride_ * kScreenHeight) {
    memset(header_, 0, sizeof(header_));
    header_[0] = 'B';
    header_[1] = 'M';
    writeLE32(&header_[2], size_);
    writeLE32(&header_[10], 54);
    writeLE32(&header_[14], 40);
    writeLE32(&header_[18], kScreenWidth);
    writeLE32(&header_[22], kScreenHeight);
    writeLE16(&header_[26], 1);
    writeLE16(&header_[28], 24);
    writeLE32(&header_[34], row_stride_ * kScreenHeight);
  }

  size_t size() const { return size_; }
  void reset() { position_ = 0; loaded_y_ = -1; }

  int available() override {
    return position_ < size_ ? int(min<uint32_t>(size_ - position_, 1460)) : 0;
  }

  int read() override {
    if (position_ >= size_) {
      return -1;
    }
    return byteAt(position_++);
  }

  int peek() override {
    if (position_ >= size_) {
      return -1;
    }
    return byteAt(position_);
  }

  size_t write(uint8_t) override { return 0; }

  size_t readBytes(uint8_t* buffer, size_t length) override {
    size_t count = 0;
    while (count < length && position_ < size_) {
      buffer[count++] = byteAt(position_++);
    }
    return count;
  }

 private:
  uint8_t byteAt(uint32_t index) {
    if (index < sizeof(header_)) {
      return header_[index];
    }

    const uint32_t body_index = index - sizeof(header_);
    const uint32_t bmp_row = body_index / row_stride_;
    const uint32_t row_byte = body_index % row_stride_;
    const uint32_t active_row_bytes = uint32_t(kScreenWidth) * 3;
    if (row_byte >= active_row_bytes) {
      return 0;
    }

    const uint32_t source_y = kScreenHeight - 1 - bmp_row;
    if (loaded_y_ != int32_t(source_y)) {
      displayReadLineRgb888(source_y, row_rgb_, kScreenWidth);
      loaded_y_ = source_y;
    }

    const uint32_t x = row_byte / 3;
    const uint32_t component = row_byte % 3;
    return row_rgb_[x * 3 + component];
  }

  uint32_t position_ = 0;
  uint32_t row_stride_;
  uint32_t size_;
  int32_t loaded_y_ = -1;
  uint8_t header_[54];
  uint8_t row_rgb_[kScreenWidth * 3];
};

void writeBmpStreamToSerial(Stream& bmp, size_t size) {
  uint8_t buffer[256];
  Serial.printf("BEGIN_BMP %u\n", unsigned(size));
  while (bmp.available() > 0) {
    const size_t count = bmp.readBytes(buffer, min<size_t>(sizeof(buffer), bmp.available()));
    if (count == 0) {
      break;
    }
    Serial.write(buffer, count);
  }
  Serial.print("END_BMP\n");
  Serial.flush();
}

void sendScreenshotOverSerial() {
  lv_refr_now(nullptr);
  Serial.println("Screenshot: streaming TFT readback BMP over serial");
  static BmpTftReadbackStream bmp;
  bmp.reset();
  writeBmpStreamToSerial(bmp, bmp.size());
}

void handleSerialCommand(const char* command) {
  if (strcmp(command, "screenshot") == 0 || strcmp(command, "shot") == 0 ||
      strcmp(command, "ss") == 0 || strcmp(command, "screenshot-serial") == 0 ||
      strcmp(command, "shot-serial") == 0 || strcmp(command, "ss-serial") == 0) {
    sendScreenshotOverSerial();
  } else if (command[0] != '\0') {
    Serial.printf("Unknown command: %s\n", command);
  }
}

void pollSerialCommands() {
  static char command[48];
  static uint8_t length = 0;

  while (Serial.available() > 0) {
    const char c = Serial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      command[length] = '\0';
      handleSerialCommand(command);
      length = 0;
      continue;
    }
    if (length < sizeof(command) - 1) {
      command[length++] = c;
    }
  }
}

}  // namespace
#endif

void setup() {
  displayPrepareForBoot();
  Serial.begin(WLED_CYD_SERIAL_BAUD);
  delay(100);

  initShutdownControl();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  loadSettings();
  const bool update_helper = updatehelper::requested();
  wifilink::begin();
  updater::begin();
  initDisplay();
#if WLED_CYD_ENABLE_BATTERY
  initBatteryMonitor();
#endif
  if (update_helper) {
    updatehelper::begin();
    finishDisplaySplash();
    touchActivity();
    return;
  }
  createUi();
  finishDisplaySplash();
  wled::begin();
  syncLivePeekSubscription();
  touchActivity();
}

void loop() {
#if WLED_TOUCH_SIMULATOR
  simWledTick();
#endif
  static uint32_t last_tick_ms = millis();
  const uint32_t now = millis();
  const uint32_t elapsed = now - last_tick_ms;
  last_tick_ms = now;
  lv_tick_inc(elapsed);

  wifilink::loop(now);
  updater::loop(now);
  if (updatehelper::active()) {
    updatehelper::loop(now);
    displayUpdateIdle(now);
    pollShutdownControl();
    lv_timer_handler();
    delay(1);
    return;
  }
  wled::loop(now);
  uiSyncFromModel();
  updatePeekStrip();

  displayUpdateIdle(now);
  pollShutdownControl();
  lv_timer_handler();
  // Catch up with Peek immediately after a blocking UI/SPI render, before the
  // next touch or redraw can add more receive backlog.
  wled::servicePeekSocket();
#if !WLED_TOUCH_SIMULATOR && WLED_CYD_ENABLE_SERIAL_SCREENSHOT
  pollSerialCommands();
#endif
  delay(1);
}

#if WLED_TOUCH_SIMULATOR
void simulatorSetTab(uint8_t index) {
  if (main_tabs) {
    lv_tabview_set_act(main_tabs, index, LV_ANIM_OFF);
  }
}

void simulatorUseDefaultViewState() {
  display_flipped = false;
  idle_mode = IdleMode::kDim;
  updateOrientationLabel();
  updateIdleLabel();
  applyDisplayRotation();
  displayClear();
  rebuildPresetTab();
  scheduleFxTabRebuild();
  if (lv_scr_act()) {
    lv_obj_invalidate(lv_scr_act());
  }
}

void simulatorRunFrames(uint16_t frames) {
  for (uint16_t i = 0; i < frames; ++i) {
    loop();
  }
}

static void writeLE16(FILE* file, uint16_t value) {
  fputc(value & 0xFF, file);
  fputc((value >> 8) & 0xFF, file);
}

static void writeLE32(FILE* file, uint32_t value) {
  fputc(value & 0xFF, file);
  fputc((value >> 8) & 0xFF, file);
  fputc((value >> 16) & 0xFF, file);
  fputc((value >> 24) & 0xFF, file);
}

bool simulatorSaveBmp(const char* path) {
  FILE* file = fopen(path, "wb");
  if (!file) {
    Serial.printf("Screenshot failed: %s\n", path);
    return false;
  }

  const uint32_t row_stride = ((kScreenWidth * 3 + 3) / 4) * 4;
  const uint32_t pixel_bytes = row_stride * kScreenHeight;
  const uint32_t file_size = 54 + pixel_bytes;

  fputc('B', file);
  fputc('M', file);
  writeLE32(file, file_size);
  writeLE16(file, 0);
  writeLE16(file, 0);
  writeLE32(file, 54);

  writeLE32(file, 40);
  writeLE32(file, kScreenWidth);
  writeLE32(file, kScreenHeight);
  writeLE16(file, 1);
  writeLE16(file, 24);
  writeLE32(file, 0);
  writeLE32(file, pixel_bytes);
  writeLE32(file, 2835);
  writeLE32(file, 2835);
  writeLE32(file, 0);
  writeLE32(file, 0);

  const uint8_t padding[3] = {0, 0, 0};
  const uint32_t pad_len = row_stride - kScreenWidth * 3;
  for (int y = kScreenHeight - 1; y >= 0; --y) {
    for (int x = 0; x < kScreenWidth; ++x) {
      const uint16_t pixel = sim_framebuffer[y * kScreenWidth + x];
      const uint8_t r = ((pixel >> 11) & 0x1F) * 255 / 31;
      const uint8_t g = ((pixel >> 5) & 0x3F) * 255 / 63;
      const uint8_t b = (pixel & 0x1F) * 255 / 31;
      fputc(b, file);
      fputc(g, file);
      fputc(r, file);
    }
    fwrite(padding, 1, pad_len, file);
  }

  fclose(file);
  Serial.printf("Screenshot saved: %s\n", path);
  return true;
}
#endif
