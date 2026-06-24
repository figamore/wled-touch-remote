#include <Arduino.h>
#if !WLED_TOUCH_SIMULATOR
#include <Wire.h>
#endif
#include <WiFi.h>
#include <lvgl.h>

#include "app_state.h"
#include "display.h"
#include "espnow.h"
#include "settings.h"
#include "BatteryMonitor.h"
#include "ui/ui.h"
#include "ui/tabs.h"

void setup() {
  Serial.begin(115200);
  delay(100);

  initShutdownControl();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  loadSettings();
  initDisplay();
#if WLED_CYD_ENABLE_BATTERY
  initBatteryMonitor();
#endif
  createUi();
  initEspNow();
  touchActivity();
}

void loop() {
#if WLED_TOUCH_SIMULATOR
  lgfx::Panel_sdl::loop();
#endif
  static uint32_t last_tick_ms = millis();
  const uint32_t now = millis();
  const uint32_t elapsed = now - last_tick_ms;
  last_tick_ms = now;
  lv_tick_inc(elapsed);

  displayUpdateIdle(now);
  applyPendingStatus();
  pollShutdownControl();
  lv_timer_handler();
  delay(1);
}

#if WLED_TOUCH_SIMULATOR
void simulatorSetTab(uint8_t index) {
  if (main_tabs) {
    lv_tabview_set_act(main_tabs, index, LV_ANIM_OFF);
  }
}

void simulatorSetExtendedMode(bool enabled) {
  if (extended_mode == enabled) {
    return;
  }

  extended_mode = enabled;
  updateModeLabel();
  rebuildPresetTab();
  rebuildFxTab();
}

void simulatorUseDefaultViewState() {
  display_flipped = false;
  idle_mode = IdleMode::kDim;
  extended_mode = true;
  updateOrientationLabel();
  updateIdleLabel();
  updateModeLabel();
  applyDisplayRotation();
  gfx.fillScreen(TFT_BLACK);
  rebuildPresetTab();
  rebuildFxTab();
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
