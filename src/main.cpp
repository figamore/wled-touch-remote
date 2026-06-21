#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <lvgl.h>
#include <Preferences.h>

#include "app_config.h"
#include "BatteryMonitor.h"
#include "generated/wled_logo_png.h"

namespace {

constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 240;
constexpr size_t kLvglBufferLines = 40;
constexpr uint8_t kWizMoteButtonOn = 1;
constexpr uint8_t kWizMoteButtonOff = 2;
constexpr uint8_t kWizMoteButtonBrightDown = 8;
constexpr uint8_t kWizMoteButtonBrightUp = 9;
constexpr uint8_t kWizMoteButtonOne = 16;
constexpr uint8_t kBasicPresetCount = 7;
constexpr uint8_t kExtendedPresetCount = 30;
constexpr uint8_t kRemoteActionFirst = 50;
constexpr uint8_t kRemoteColorFirst = 70;
constexpr uint8_t kBroadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr const char* kPrefsNamespace = "wled-cyd";
constexpr const char* kPrefsFlipKey = "flip";
constexpr const char* kPrefsIdleOffKey = "idleOff";
constexpr const char* kPrefsIdleModeKey = "idleMode";
constexpr const char* kPrefsExtendedKey = "extended";

constexpr uint32_t kColorBg = 0x0A0E13;
constexpr uint32_t kColorHeaderBar = 0x10171F;
constexpr uint32_t kColorSurface = 0x161F29;
constexpr uint32_t kColorSurfaceRaised = 0x1F2A36;
constexpr uint32_t kColorSurfacePressed = 0x2A3947;
constexpr uint32_t kColorBorder = 0x29384A;
constexpr uint32_t kColorBorderStrong = 0x3A4C5E;
constexpr uint32_t kColorText = 0xEAF2F8;
constexpr uint32_t kColorTextMuted = 0x7E93A6;
constexpr uint32_t kColorAccent = 0x22D3EE;
constexpr uint32_t kColorAccentBright = 0x67E8F9;
constexpr uint32_t kColorAccentDeep = 0x0E7490;
constexpr uint32_t kColorSelected = 0x0F766E;
constexpr uint32_t kColorSelectedBorder = 0x2DD4BF;
constexpr uint32_t kColorOk = 0x34D399;
constexpr uint32_t kColorWarn = 0xFACC15;
constexpr uint32_t kColorDanger = 0xF87171;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
#if CYD_PANEL_TYPE == CYD_PANEL_ST7789
  lgfx::Panel_ST7789 panel_;
#else
  lgfx::Panel_ILI9341 panel_;
#endif
#if CYD_TOUCH_TYPE == CYD_TOUCH_CST816S
  lgfx::Touch_CST816S touch_;
#else
  lgfx::Touch_FT5x06 touch_;
#endif

 public:
  LGFX() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = HSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 55000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = CYD_TFT_SCLK;
      cfg.pin_mosi = CYD_TFT_MOSI;
      cfg.pin_miso = CYD_TFT_MISO;
      cfg.pin_dc = CYD_TFT_DC;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }

    {
      auto cfg = panel_.config();
      cfg.pin_cs = CYD_TFT_CS;
      cfg.pin_rst = CYD_TFT_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.memory_width = 240;
      cfg.memory_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = CYD_PANEL_OFFSET_ROTATION;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = true;
      cfg.invert = CYD_PANEL_INVERT;
      cfg.rgb_order = CYD_PANEL_RGB_ORDER;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }

    {
      auto cfg = light_.config();
      cfg.pin_bl = CYD_TFT_BL;
      cfg.invert = CYD_BACKLIGHT_INVERT;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }

    {
      auto cfg = touch_.config();
      cfg.x_min = 0;
      cfg.x_max = 240;
      cfg.y_min = 0;
      cfg.y_max = 320;
      cfg.pin_int = CYD_TOUCH_INT;
      cfg.pin_rst = CYD_TOUCH_RST;
      cfg.bus_shared = false;
      cfg.offset_rotation = CYD_TOUCH_OFFSET_ROTATION;
      cfg.i2c_port = CYD_TOUCH_I2C_PORT;
      cfg.i2c_addr = CYD_TOUCH_ADDR;
      cfg.pin_sda = CYD_TOUCH_SDA;
      cfg.pin_scl = CYD_TOUCH_SCL;
      cfg.freq = 400000;
      touch_.config(cfg);
      panel_.setTouch(&touch_);
    }

    setPanel(&panel_);
  }
};

struct WizMotePacket {
  uint8_t program;
  uint8_t seq[4];
  uint8_t dt1;
  uint8_t button;
  uint8_t dt2;
  uint8_t batLevel;
  uint8_t byte10;
  uint8_t byte11;
  uint8_t byte12;
  uint8_t byte13;
} __attribute__((packed));

struct RemoteState {
  bool power = false;
  uint8_t brightness = 255;
};

struct ColorSwatch {
  const char* label;
  uint8_t button;
  uint32_t color;
  bool dark_text;
};

struct RemoteControlPair {
  const char* label;
  uint8_t down_button;
  uint8_t up_button;
};

enum class IdleMode : uint8_t {
  kDim,
  kOff,
  kAlwaysOn,
};

constexpr RemoteControlPair kFxControls[] = {
    {"Palette", kRemoteActionFirst, static_cast<uint8_t>(kRemoteActionFirst + 1)},
    {"Speed", static_cast<uint8_t>(kRemoteActionFirst + 2), static_cast<uint8_t>(kRemoteActionFirst + 3)},
    {"Intensity", static_cast<uint8_t>(kRemoteActionFirst + 4), static_cast<uint8_t>(kRemoteActionFirst + 5)},
    {"Custom Slider 1", static_cast<uint8_t>(kRemoteActionFirst + 6), static_cast<uint8_t>(kRemoteActionFirst + 7)},
};

constexpr ColorSwatch kColorSwatches[] = {
    {"Warm", kRemoteColorFirst, 0xFFC078, false},
    {"White", static_cast<uint8_t>(kRemoteColorFirst + 1), 0xFFFFFF, true},
    {"Red", static_cast<uint8_t>(kRemoteColorFirst + 2), 0xEF4444, false},
    {"Orange", static_cast<uint8_t>(kRemoteColorFirst + 3), 0xF97316, false},
    {"Yellow", static_cast<uint8_t>(kRemoteColorFirst + 4), 0xFACC15, true},
    {"Green", static_cast<uint8_t>(kRemoteColorFirst + 5), 0x22C55E, true},
    {"Cyan", static_cast<uint8_t>(kRemoteColorFirst + 6), 0x06B6D4, true},
    {"Blue", static_cast<uint8_t>(kRemoteColorFirst + 7), 0x2563EB, false},
    {"Purple", static_cast<uint8_t>(kRemoteColorFirst + 8), 0xA855F7, false},
    {"Pink", static_cast<uint8_t>(kRemoteColorFirst + 9), 0xEC4899, false},
};

enum class StatusCode : uint8_t {
  kBoot,
  kOffline,
  kSent,
  kOk,
  kNoAck,
  kSendError,
  kEspFail,
  kPeerError,
  kBroadcast,
  kReady,
};

LGFX gfx;
lv_disp_draw_buf_t draw_buf;
lv_color_t draw_buf_1[kScreenWidth * kLvglBufferLines];
lv_color_t draw_buf_2[kScreenWidth * kLvglBufferLines];

RemoteState state;
uint32_t sequence_id = 0;
bool espnow_ready = false;
bool display_flipped = false;
IdleMode idle_mode = IdleMode::kDim;
bool extended_mode = false;
bool display_idle_applied = false;
bool suppress_touch_until_release = false;
uint32_t last_touch_ms = 0;
volatile bool pending_status = false;
volatile uint8_t pending_status_code = static_cast<uint8_t>(StatusCode::kBoot);

lv_obj_t* status_dot = nullptr;
lv_obj_t* main_tabs = nullptr;
lv_obj_t* presets_tab = nullptr;
lv_obj_t* fx_tab = nullptr;
lv_obj_t* preset_buttons[kExtendedPresetCount] = {};
lv_obj_t* power_button = nullptr;
lv_obj_t* power_button_label = nullptr;
lv_obj_t* brightness_label = nullptr;
lv_obj_t* mac_label = nullptr;
lv_obj_t* orientation_label = nullptr;
lv_obj_t* idle_label = nullptr;
lv_obj_t* mode_label = nullptr;
lv_obj_t* help_dialog = nullptr;
uint8_t selected_preset = 0;

#if WLED_CYD_ENABLE_BATTERY
lv_obj_t* battery_indicator = nullptr;
lv_obj_t* battery_fill = nullptr;
lv_obj_t* battery_charge = nullptr;
#endif

lv_style_t style_screen;
lv_style_t style_topbar;
lv_style_t style_panel;
lv_style_t style_section_header;
lv_style_t style_label_muted;
lv_style_t style_button;
lv_style_t style_button_pressed;
lv_style_t style_button_checked;
lv_style_t style_slider;
lv_style_t style_slider_indicator;
lv_style_t style_knob;

const lv_img_dsc_t kHeaderLogoImage = {
    {LV_IMG_CF_TRUE_COLOR, 0, 0, kWledLogoHeaderWidth, kWledLogoHeaderHeight},
    kWledLogoHeaderPixelCount * sizeof(kWledLogoHeaderPixels[0]),
    reinterpret_cast<const uint8_t*>(kWledLogoHeaderPixels),
};

const lv_img_dsc_t kHelpQrImage = {
    {LV_IMG_CF_TRUE_COLOR, 0, 0, kHelpQrWidth, kHelpQrHeight},
    kHelpQrPixelCount * sizeof(kHelpQrPixels[0]),
    reinterpret_cast<const uint8_t*>(kHelpQrPixels),
};

const lv_img_dsc_t kRemoteJsonQrImage = {
    {LV_IMG_CF_TRUE_COLOR, 0, 0, kRemoteJsonQrWidth, kRemoteJsonQrHeight},
    kRemoteJsonQrPixelCount * sizeof(kRemoteJsonQrPixels[0]),
    reinterpret_cast<const uint8_t*>(kRemoteJsonQrPixels),
};

void rebuildPresetTab();
void rebuildFxTab();

void setStatusColor(lv_color_t color) {
  if (status_dot) {
    lv_obj_set_style_bg_color(status_dot, color, LV_PART_MAIN);
  }
}

void requestStatus(StatusCode code) {
  pending_status_code = static_cast<uint8_t>(code);
  pending_status = true;
}

void applyPendingStatus() {
  if (!pending_status) {
    return;
  }

  const auto code = static_cast<StatusCode>(pending_status_code);
  pending_status = false;

  switch (code) {
    case StatusCode::kBoot:
      setStatusColor(lv_color_hex(kColorAccent));
      break;
    case StatusCode::kOffline:
      setStatusColor(lv_color_hex(kColorDanger));
      break;
    case StatusCode::kSent:
      setStatusColor(lv_color_hex(kColorOk));
      break;
    case StatusCode::kOk:
      setStatusColor(lv_color_hex(kColorOk));
      break;
    case StatusCode::kNoAck:
      setStatusColor(lv_color_hex(0xA78BFA));
      break;
    case StatusCode::kSendError:
      setStatusColor(lv_color_hex(kColorDanger));
      break;
    case StatusCode::kEspFail:
      setStatusColor(lv_color_hex(kColorDanger));
      break;
    case StatusCode::kPeerError:
      setStatusColor(lv_color_hex(kColorDanger));
      break;
    case StatusCode::kBroadcast:
      setStatusColor(lv_color_hex(kColorOk));
      break;
    case StatusCode::kReady:
      setStatusColor(lv_color_hex(kColorOk));
      break;
  }
}

void touchActivity() {
  last_touch_ms = millis();
  display_idle_applied = false;
  gfx.setBrightness(UI_ACTIVE_BRIGHTNESS);
}

const char* idleModeName(IdleMode mode) {
  switch (mode) {
    case IdleMode::kDim:
      return "Dim";
    case IdleMode::kOff:
      return "Display Off";
    case IdleMode::kAlwaysOn:
      return "Always On";
  }
  return "Dim";
}

IdleMode nextIdleMode(IdleMode mode) {
  switch (mode) {
    case IdleMode::kDim:
      return IdleMode::kOff;
    case IdleMode::kOff:
      return IdleMode::kAlwaysOn;
    case IdleMode::kAlwaysOn:
      return IdleMode::kDim;
  }
  return IdleMode::kDim;
}

void applyDisplayRotation() {
  gfx.setRotation(display_flipped ? 3 : 1);
}

void drawSplash() {
  gfx.fillScreen(TFT_BLACK);

  const bool can_draw_logo = kWledLogoPixelCount == kWledLogoWidth * kWledLogoHeight &&
                             kWledLogoWidth <= kScreenWidth &&
                             kWledLogoHeight <= kScreenHeight;
  if (can_draw_logo) {
    const int32_t x = (kScreenWidth - kWledLogoWidth) / 2;
    const int32_t y = (kScreenHeight - kWledLogoHeight) / 2;
    gfx.pushImage(x,
                  y,
                  kWledLogoWidth,
                  kWledLogoHeight,
                  reinterpret_cast<const lgfx::rgb565_t*>(kWledLogoPixels));
  }

  if (!can_draw_logo) {
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.setTextDatum(middle_center);
    gfx.setTextSize(3);
    gfx.drawString("WLED", kScreenWidth / 2, kScreenHeight / 2);
  }

  delay(UI_SPLASH_MS);
  gfx.fillScreen(TFT_BLACK);
}

void loadSettings() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    display_flipped = prefs.getBool(kPrefsFlipKey, false);
    if (prefs.isKey(kPrefsIdleModeKey)) {
      const uint8_t saved_idle_mode = prefs.getUChar(kPrefsIdleModeKey, static_cast<uint8_t>(IdleMode::kDim));
      idle_mode = saved_idle_mode <= static_cast<uint8_t>(IdleMode::kAlwaysOn)
                      ? static_cast<IdleMode>(saved_idle_mode)
                      : IdleMode::kDim;
    } else {
      idle_mode = prefs.getBool(kPrefsIdleOffKey, false) ? IdleMode::kOff : IdleMode::kDim;
    }
    extended_mode = prefs.getBool(kPrefsExtendedKey, false);
    prefs.end();
  }
  Serial.printf("Display orientation: %s\n", display_flipped ? "flipped" : "normal");
  Serial.printf("Display idle action: %s\n", idleModeName(idle_mode));
  Serial.printf("Control mode: %s\n", extended_mode ? "extended" : "basic");
}

void saveSettings() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putBool(kPrefsFlipKey, display_flipped);
    prefs.putUChar(kPrefsIdleModeKey, static_cast<uint8_t>(idle_mode));
    prefs.putBool(kPrefsExtendedKey, extended_mode);
    prefs.end();
  }
}

void flushDisplay(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
  const int32_t width = area->x2 - area->x1 + 1;
  const int32_t height = area->y2 - area->y1 + 1;

  gfx.startWrite();
  gfx.setAddrWindow(area->x1, area->y1, width, height);
  gfx.writePixels(reinterpret_cast<lgfx::rgb565_t*>(color_p), width * height);
  gfx.endWrite();

  lv_disp_flush_ready(disp);
}

void readTouch(lv_indev_drv_t*, lv_indev_data_t* data) {
  uint16_t x = 0;
  uint16_t y = 0;
  if (gfx.getTouch(&x, &y)) {
    if ((display_idle_applied && idle_mode == IdleMode::kOff) || suppress_touch_until_release) {
      suppress_touch_until_release = true;
      data->state = LV_INDEV_STATE_REL;
      touchActivity();
      return;
    }

    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
    touchActivity();
  } else {
    suppress_touch_until_release = false;
    data->state = LV_INDEV_STATE_REL;
  }
}

esp_err_t sendWizMoteButton(uint8_t button_code) {
  if (!espnow_ready) {
    requestStatus(StatusCode::kOffline);
    return ESP_ERR_INVALID_STATE;
  }

  sequence_id++;
  WizMotePacket packet = {
      static_cast<uint8_t>(button_code == kWizMoteButtonOn ? 0x91 : 0x81),
      {static_cast<uint8_t>(sequence_id),
       static_cast<uint8_t>(sequence_id >> 8),
       static_cast<uint8_t>(sequence_id >> 16),
       static_cast<uint8_t>(sequence_id >> 24)},
      0x20,
      button_code,
      0x01,
      90,
      0,
      0,
      0,
      0,
  };

  esp_err_t last_result = ESP_OK;

#if WLED_WIZMOTE_SCAN_CHANNELS
  for (uint8_t channel = 1; channel <= 13; channel++) {
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    last_result = esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    delay(3);
  }
  esp_wifi_set_channel(WLED_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
#else
  last_result = esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
#endif

  requestStatus(last_result == ESP_OK ? StatusCode::kSent : StatusCode::kSendError);
  Serial.printf("WizMote button sent: %u result=%d\n", button_code, last_result);
  return last_result;
}

void sendPower(bool power) {
  sendWizMoteButton(power ? kWizMoteButtonOn : kWizMoteButtonOff);
}

void setPowerUi(bool power) {
  state.power = power;
  if (power_button) {
    if (state.power) {
      lv_obj_add_state(power_button, LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(power_button, LV_STATE_CHECKED);
    }
  }
  if (power_button_label) {
    lv_label_set_text(power_button_label,
                      state.power ? LV_SYMBOL_POWER "  Power On" : LV_SYMBOL_POWER "  Power Off");
  }
}

void sendBrightnessDelta(int delta) {
  const uint8_t button = delta > 0 ? kWizMoteButtonBrightUp : kWizMoteButtonBrightDown;
  const uint8_t repeats = min<uint8_t>(6, max<uint8_t>(1, abs(delta) / 25));
  for (uint8_t i = 0; i < repeats; i++) {
    sendWizMoteButton(button);
    delay(10);
  }
}

void sendPreset(uint8_t preset) {
  const uint8_t max_preset = extended_mode ? kExtendedPresetCount : kBasicPresetCount;
  if (preset < 1 || preset > max_preset) {
    return;
  }
  sendWizMoteButton(kWizMoteButtonOne + preset - 1);
}

void onPower(lv_event_t* event) {
  setPowerUi(!state.power);
  sendPower(state.power);
}

void onBrightness(lv_event_t* event) {
  const uint8_t previous = state.brightness;
  state.brightness = lv_slider_get_value(lv_event_get_target(event));
  if (brightness_label) {
    lv_label_set_text_fmt(brightness_label, "%u", state.brightness);
  }
  sendBrightnessDelta(static_cast<int>(state.brightness) - previous);
}

void setSelectedPreset(uint8_t preset) {
  selected_preset = preset;

  for (uint8_t i = 0; i < kExtendedPresetCount; ++i) {
    if (!preset_buttons[i]) {
      continue;
    }

    if (i + 1 == selected_preset) {
      lv_obj_add_state(preset_buttons[i], LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(preset_buttons[i], LV_STATE_CHECKED);
    }
  }
}

void onPreset(lv_event_t* event) {
  const uintptr_t preset = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  const uint8_t preset_number = static_cast<uint8_t>(preset);
  setSelectedPreset(preset_number);
  sendPreset(preset_number);
  setPowerUi(true);
}

void onPing(lv_event_t*) {
  sendWizMoteButton(kWizMoteButtonOn);
}

void onRemoteAction(lv_event_t* event) {
  const uintptr_t button = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  sendWizMoteButton(static_cast<uint8_t>(button));
}

void goToSettings(lv_event_t*) {
  if (main_tabs) {
    lv_tabview_set_act(main_tabs, 4, LV_ANIM_ON);
  }
}

#if WLED_CYD_ENABLE_BATTERY
lv_color_t batteryColor(int level) {
  if (level >= 50) {
    return lv_color_hex(kColorOk);
  }
  if (level >= 25) {
    return lv_color_hex(kColorWarn);
  }
  return lv_color_hex(kColorDanger);
}

void updateBatteryIndicator() {
  if (!battery_indicator || !battery_fill || !battery_charge) {
    return;
  }

  int level = batteryLevel();
  if (level < 0) {
    lv_obj_add_flag(battery_indicator, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  lv_obj_clear_flag(battery_indicator, LV_OBJ_FLAG_HIDDEN);

  int fill_w = 2;
  if (level >= 75) {
    fill_w = 18;
  } else if (level >= 50) {
    fill_w = 9;
  } else if (level >= 25) {
    fill_w = 4;
  }

  bool charging = batteryCharging();
  lv_obj_set_width(battery_fill, fill_w);
  lv_obj_set_style_bg_color(battery_fill, charging ? lv_color_hex(kColorBg) : batteryColor(level), LV_PART_MAIN);

  if (charging) {
    lv_obj_clear_flag(battery_charge, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(battery_charge, LV_OBJ_FLAG_HIDDEN);
  }
}

void updateBatteryTimer(lv_timer_t*) {
  updateBatteryIndicator();
}

void createBatteryIndicator(lv_obj_t* parent) {
  if (!batteryAvailable()) {
    return;
  }

  battery_indicator = lv_obj_create(parent);
  lv_obj_remove_style_all(battery_indicator);
  lv_obj_set_size(battery_indicator, 28, 18);
  lv_obj_clear_flag(battery_indicator, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* body = lv_obj_create(battery_indicator);
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, 20, 13);
  lv_obj_align(body, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_border_width(body, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(body, lv_color_hex(kColorText), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

  battery_fill = lv_obj_create(body);
  lv_obj_remove_style_all(battery_fill);
  lv_obj_set_size(battery_fill, 2, 11);
  lv_obj_align(battery_fill, LV_ALIGN_TOP_LEFT, 1, 1);
  lv_obj_set_style_bg_opa(battery_fill, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(battery_fill, lv_color_hex(kColorDanger), LV_PART_MAIN);
  lv_obj_clear_flag(battery_fill, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* nub = lv_obj_create(battery_indicator);
  lv_obj_remove_style_all(nub);
  lv_obj_set_size(nub, 4, 7);
  lv_obj_align(nub, LV_ALIGN_LEFT_MID, 20, 0);
  lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(nub, lv_color_hex(kColorText), LV_PART_MAIN);
  lv_obj_clear_flag(nub, LV_OBJ_FLAG_SCROLLABLE);

  battery_charge = lv_label_create(battery_indicator);
  lv_label_set_text(battery_charge, LV_SYMBOL_CHARGE);
  lv_obj_set_style_text_font(battery_charge, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(battery_charge, lv_color_hex(kColorOk), LV_PART_MAIN);
  lv_obj_align(battery_charge, LV_ALIGN_CENTER, -2, 0);

  updateBatteryIndicator();
  lv_timer_create(updateBatteryTimer, 1000, nullptr);
}
#endif

void updateOrientationLabel() {
  if (orientation_label) {
    lv_label_set_text(orientation_label, display_flipped ? "Flipped" : "Normal");
  }
}

void updateIdleLabel() {
  if (idle_label) {
    lv_label_set_text(idle_label, idleModeName(idle_mode));
  }
}

void updateModeLabel() {
  if (mode_label) {
    lv_label_set_text(mode_label, extended_mode ? "Extended" : "Basic");
  }
}

void onFlipDisplay(lv_event_t*) {
  display_flipped = !display_flipped;
  saveSettings();
  applyDisplayRotation();
  gfx.fillScreen(TFT_BLACK);
  updateOrientationLabel();
  lv_obj_invalidate(lv_scr_act());
}

void onToggleIdleAction(lv_event_t*) {
  idle_mode = nextIdleMode(idle_mode);
  saveSettings();
  updateIdleLabel();
  touchActivity();
}

void onToggleControlMode(lv_event_t* event) {
  extended_mode = !extended_mode;
  saveSettings();
  updateModeLabel();
  lv_obj_t* target = lv_event_get_target(event);
  if (target) {
    if (extended_mode) {
      lv_obj_add_state(target, LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(target, LV_STATE_CHECKED);
    }
  }
  rebuildPresetTab();
  rebuildFxTab();
  touchActivity();
}

void addLabel(lv_obj_t* parent, const char* text, lv_coord_t width = LV_SIZE_CONTENT) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_add_style(label, &style_section_header, LV_PART_MAIN);
  lv_obj_set_width(label, width);
}

void styleButton(lv_obj_t* btn, bool checkable = false) {
  lv_obj_add_style(btn, &style_button, LV_PART_MAIN);
  lv_obj_add_style(btn, &style_button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
  if (checkable) {
    lv_obj_add_style(btn, &style_button_checked, LV_PART_MAIN | LV_STATE_CHECKED);
  }
}

lv_obj_t* createPanel(lv_obj_t* parent) {
  lv_obj_t* panel = lv_obj_create(parent);
  lv_obj_add_style(panel, &style_panel, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  return panel;
}

void configurePageScroll(lv_obj_t* page, bool enabled) {
  lv_obj_set_scroll_dir(page, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(page, enabled ? LV_SCROLLBAR_MODE_AUTO : LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);

  lv_obj_set_style_bg_color(page, lv_color_hex(kColorAccent), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(page, LV_OPA_50, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(page, 4, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_right(page, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(page, 2, LV_PART_SCROLLBAR);

  if (enabled) {
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
  } else {
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
  }
}

void closeHelpDialog(lv_event_t*) {
  if (help_dialog) {
    lv_obj_del(help_dialog);
    help_dialog = nullptr;
  }
}

lv_obj_t* beginInfoModal(const char* title_text) {
  help_dialog = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(help_dialog);
  lv_obj_set_size(help_dialog, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(help_dialog, lv_color_hex(kColorBg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(help_dialog, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(help_dialog, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(help_dialog, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* header = lv_obj_create(help_dialog);
  lv_obj_remove_style_all(header);
  lv_obj_set_size(header, LV_PCT(100), 40);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_left(header, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_right(header, 8, LV_PART_MAIN);
  lv_obj_set_style_border_color(header, lv_color_hex(kColorBorder), LV_PART_MAIN);
  lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
  lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(header);
  lv_label_set_text(title, title_text);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(kColorAccent), LV_PART_MAIN);

  lv_obj_t* close = lv_btn_create(header);
  styleButton(close);
  lv_obj_set_size(close, 40, 30);
  lv_obj_add_event_cb(close, closeHelpDialog, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* close_label = lv_label_create(close);
  lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
  lv_obj_center(close_label);

  lv_obj_t* content = lv_obj_create(help_dialog);
  lv_obj_remove_style_all(content);
  lv_obj_set_size(content, 296, 184);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(content, 8, LV_PART_MAIN);
  lv_obj_add_flag(content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
  return content;
}

void addQrCode(lv_obj_t* parent, const lv_img_dsc_t* src, lv_coord_t w, lv_coord_t h) {
  lv_obj_t* qr = lv_img_create(parent);
  lv_img_set_src(qr, src);
  lv_obj_set_size(qr, w, h);
  lv_obj_set_style_outline_color(qr, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_outline_width(qr, 3, LV_PART_MAIN);
  lv_obj_set_style_outline_opa(qr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_outline_pad(qr, 0, LV_PART_MAIN);
}

void openHelpDialog(lv_event_t*) {
  if (help_dialog) {
    return;
  }

  lv_obj_t* content = beginInfoModal("Help");

  lv_obj_t* instructions = lv_label_create(content);
  lv_obj_set_width(instructions, 100);
  lv_label_set_long_mode(instructions, LV_LABEL_LONG_WRAP);
  lv_label_set_text(instructions,
                    "Scan for\n"
                    "setup help,\n"
                    "usage tips,\n"
                    "and project\n"
                    "instructions.");
  lv_obj_add_style(instructions, &style_label_muted, LV_PART_MAIN);

  addQrCode(content, &kHelpQrImage, kHelpQrWidth, kHelpQrHeight);
}

void openRemoteJsonHelpDialog(lv_event_t*) {
  if (help_dialog) {
    return;
  }

  lv_obj_t* content = beginInfoModal("Extended FX");

  lv_obj_t* instructions = lv_label_create(content);
  lv_obj_set_width(instructions, 100);
  lv_label_set_long_mode(instructions, LV_LABEL_LONG_WRAP);
  lv_label_set_text(instructions,
                    "Upload\n"
                    "remote.json\n"
                    "to WLED:\n\n"
                    "1. Open\n"
                    "wled.local/edit\n"
                    "2. Upload file\n"
                    "3. Enable\n"
                    "Extended");
  lv_obj_add_style(instructions, &style_label_muted, LV_PART_MAIN);

  addQrCode(content, &kRemoteJsonQrImage, kRemoteJsonQrWidth, kRemoteJsonQrHeight);
}

lv_obj_t* createSlider(lv_obj_t* parent,
                       int min,
                       int max,
                       int value,
                       lv_event_cb_t cb,
                       lv_obj_t** value_label) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, 38);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(row, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);

  lv_obj_t* slider = lv_slider_create(row);
  lv_slider_set_range(slider, min, max);
  lv_slider_set_value(slider, value, LV_ANIM_OFF);
  lv_obj_set_size(slider, 190, 12);
  lv_obj_add_flag(slider, LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_clear_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_style(slider, &style_slider, LV_PART_MAIN);
  lv_obj_add_style(slider, &style_slider_indicator, LV_PART_INDICATOR);
  lv_obj_add_style(slider, &style_knob, LV_PART_KNOB);
  lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, nullptr);

  *value_label = lv_label_create(row);
  lv_obj_set_width(*value_label, 34);
  lv_obj_clear_flag(*value_label, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_label_set_text_fmt(*value_label, "%d", value);
  lv_obj_set_style_text_align(*value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

  return slider;
}

void createLiveTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, false);

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), 158);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);

  power_button = lv_btn_create(panel);
  styleButton(power_button, true);
  lv_obj_set_size(power_button, LV_PCT(100), 54);
  lv_obj_add_flag(power_button, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_add_event_cb(power_button, onPower, LV_EVENT_CLICKED, nullptr);

  lv_obj_set_style_bg_grad_color(power_button, lv_color_hex(kColorAccentBright),
                                 LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_style_bg_grad_dir(power_button, LV_GRAD_DIR_VER,
                               LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(power_button, lv_color_hex(0x062029),
                              LV_PART_MAIN | LV_STATE_CHECKED);

  power_button_label = lv_label_create(power_button);
  lv_label_set_text(power_button_label, LV_SYMBOL_POWER "  Power Off");
  lv_obj_set_style_text_font(power_button_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_center(power_button_label);

  addLabel(panel, "Brightness");
  createSlider(panel, 1, 255, state.brightness, onBrightness, &brightness_label);

}

lv_obj_t* createRemoteButton(lv_obj_t* parent,
                             const char* text,
                             lv_coord_t width,
                             lv_coord_t height,
                             uint8_t button) {
  lv_obj_t* btn = lv_btn_create(parent);
  styleButton(btn);
  lv_obj_set_size(btn, width, height);
  lv_obj_add_event_cb(btn,
                      onRemoteAction,
                      LV_EVENT_CLICKED,
                      reinterpret_cast<void*>(static_cast<uintptr_t>(button)));

  lv_obj_t* label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return btn;
}

void createControlPairRow(lv_obj_t* parent,
                          const char* label_text,
                          uint8_t down_button,
                          uint8_t up_button,
                          const char* down_text,
                          const char* up_text) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 48);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* label = lv_label_create(row);
  lv_obj_set_width(label, 118);
  lv_label_set_text(label, label_text);
  lv_obj_add_style(label, &style_label_muted, LV_PART_MAIN);

  createRemoteButton(row, down_text, 78, 44, down_button);
  createRemoteButton(row, up_text, 78, 44, up_button);
}

void createColorSwatches(lv_obj_t* parent) {
  lv_obj_t* swatches = lv_obj_create(parent);
  lv_obj_remove_style_all(swatches);
  lv_obj_set_size(swatches, LV_PCT(100), 272);
  lv_obj_set_flex_flow(swatches, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(swatches, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(swatches, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_column(swatches, 8, LV_PART_MAIN);

  for (const ColorSwatch& swatch : kColorSwatches) {
    lv_obj_t* btn = createRemoteButton(swatches, swatch.label, 132, 48, swatch.button);
    const lv_color_t color = lv_color_hex(swatch.color);
    lv_obj_set_style_bg_color(btn, color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_darken(color, LV_OPA_30),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, lv_color_hex(swatch.dark_text ? kColorBg : 0xFFFFFF), LV_PART_MAIN);
  }
}

void createLooksTab(lv_obj_t* tab) {
  for (lv_obj_t*& preset_button : preset_buttons) {
    preset_button = nullptr;
  }

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, extended_mode);

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), extended_mode ? 282 : 138);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);

  if (extended_mode) {
    lv_obj_t* preset_list = lv_obj_create(panel);
    lv_obj_remove_style_all(preset_list);
    lv_obj_set_size(preset_list, LV_PCT(100), 252);
    lv_obj_set_flex_flow(preset_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(preset_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(preset_list, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(preset_list, 6, LV_PART_MAIN);
    lv_obj_clear_flag(preset_list, LV_OBJ_FLAG_SCROLLABLE);

    for (uintptr_t i = 1; i <= kExtendedPresetCount; ++i) {
      lv_obj_t* btn = lv_btn_create(preset_list);
      styleButton(btn, true);
      lv_obj_set_size(btn, 48, 34);
      lv_obj_add_event_cb(btn, onPreset, LV_EVENT_CLICKED, reinterpret_cast<void*>(i));
      preset_buttons[i - 1] = btn;
      if (i == selected_preset) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
      }

      lv_obj_t* label = lv_label_create(btn);
      lv_label_set_text_fmt(label, "%u", static_cast<unsigned>(i));
      lv_obj_center(label);
    }

    return;
  }

  lv_obj_t* rows = lv_obj_create(panel);
  lv_obj_remove_style_all(rows);
  lv_obj_set_size(rows, LV_PCT(100), 88);
  lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rows, 10, LV_PART_MAIN);

  lv_obj_t* row_one = lv_obj_create(rows);
  lv_obj_remove_style_all(row_one);
  lv_obj_set_size(row_one, LV_PCT(100), 38);
  lv_obj_set_flex_flow(row_one, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row_one, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* row_two = lv_obj_create(rows);
  lv_obj_remove_style_all(row_two);
  lv_obj_set_size(row_two, LV_PCT(100), 38);
  lv_obj_set_flex_flow(row_two, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row_two, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row_two, 8, LV_PART_MAIN);

  for (uintptr_t i = 1; i <= kBasicPresetCount; ++i) {
    lv_obj_t* parent = i <= 4 ? row_one : row_two;
    lv_obj_t* btn = lv_btn_create(parent);
    styleButton(btn, true);
    lv_obj_set_size(btn, 64, 38);
    lv_obj_add_event_cb(btn, onPreset, LV_EVENT_CLICKED, reinterpret_cast<void*>(i));
    preset_buttons[i - 1] = btn;
    if (i == selected_preset) {
      lv_obj_add_state(btn, LV_STATE_CHECKED);
    }

    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text_fmt(label, "%u", static_cast<unsigned>(i));
    lv_obj_center(label);
  }
}

void rebuildPresetTab() {
  if (!presets_tab) {
    return;
  }
  lv_obj_clean(presets_tab);
  createLooksTab(presets_tab);
}

void createFxTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, extended_mode);

  if (!extended_mode) {
    lv_obj_t* panel = createPanel(tab);
    lv_obj_set_size(panel, LV_PCT(100), 180);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, 8, LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Extended Mode Off");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);

    lv_obj_t* hint = lv_label_create(panel);
    lv_label_set_text(hint, "Enable it in Settings for FX controls");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_style(hint, &style_label_muted, LV_PART_MAIN);

    lv_obj_t* settings = lv_btn_create(panel);
    styleButton(settings);
    lv_obj_set_size(settings, LV_PCT(100), 34);
    lv_obj_add_event_cb(settings, goToSettings, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* settings_label = lv_label_create(settings);
    lv_label_set_text(settings_label, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_center(settings_label);

    lv_obj_t* learn_more = lv_btn_create(panel);
    styleButton(learn_more);
    lv_obj_set_size(learn_more, LV_PCT(100), 34);
    lv_obj_add_event_cb(learn_more, openRemoteJsonHelpDialog, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* learn_more_label = lv_label_create(learn_more);
    lv_label_set_text(learn_more_label, LV_SYMBOL_LIST "  Learn More");
    lv_obj_center(learn_more_label);
    return;
  }

  lv_obj_t* fx_panel = createPanel(tab);
  lv_obj_set_size(fx_panel, LV_PCT(100), 584);
  lv_obj_set_flex_flow(fx_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(fx_panel, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_row(fx_panel, 8, LV_PART_MAIN);

  addLabel(fx_panel, "Look Controls");
  for (const RemoteControlPair& control : kFxControls) {
    createControlPairRow(fx_panel,
                         control.label,
                         control.down_button,
                         control.up_button,
                         LV_SYMBOL_MINUS,
                         LV_SYMBOL_PLUS);
  }

  addLabel(fx_panel, "Colors");
  createColorSwatches(fx_panel);
}

void rebuildFxTab() {
  if (!fx_tab) {
    return;
  }
  lv_obj_clean(fx_tab);
  createFxTab(fx_tab);
}

void createInfoTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, false);

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), 156);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);

  addLabel(panel, "WLED ESP-NOW MAC");

  mac_label = lv_label_create(panel);
  lv_label_set_text(mac_label, WiFi.macAddress().c_str());
  lv_obj_set_width(mac_label, LV_PCT(100));
  lv_obj_set_style_text_align(mac_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_font(mac_label, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(mac_label, lv_color_hex(kColorAccent), LV_PART_MAIN);

  lv_obj_t* hint = lv_label_create(panel);
  lv_label_set_text(hint, "Enter this in WLED Hardware MAC");
  lv_obj_set_width(hint, LV_PCT(100));
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_add_style(hint, &style_label_muted, LV_PART_MAIN);

  lv_obj_t* actions = lv_obj_create(panel);
  lv_obj_remove_style_all(actions);
  lv_obj_set_size(actions, LV_PCT(100), 38);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* ping = lv_btn_create(actions);
  styleButton(ping);
  lv_obj_set_size(ping, 118, 36);
  lv_obj_add_event_cb(ping, onPing, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* label = lv_label_create(ping);
  lv_label_set_text(label, LV_SYMBOL_WIFI "  Ping");
  lv_obj_center(label);

  lv_obj_t* help = lv_btn_create(actions);
  styleButton(help);
  lv_obj_set_size(help, 118, 36);
  lv_obj_add_event_cb(help, openHelpDialog, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* help_label = lv_label_create(help);
  lv_label_set_text(help_label, LV_SYMBOL_LIST "  Help");
  lv_obj_center(help_label);
}

lv_obj_t* createSettingsRow(lv_obj_t* parent,
                            const char* name,
                            lv_event_cb_t cb,
                            bool checkable,
                            lv_obj_t** value_out) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 38);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* name_label = lv_label_create(row);
  lv_obj_set_width(name_label, 148);
  lv_obj_add_style(name_label, &style_label_muted, LV_PART_MAIN);
  lv_label_set_text(name_label, name);

  lv_obj_t* pill = lv_btn_create(row);
  styleButton(pill, checkable);
  lv_obj_set_size(pill, 118, 34);
  lv_obj_add_event_cb(pill, cb, LV_EVENT_CLICKED, nullptr);

  *value_out = lv_label_create(pill);
  lv_obj_center(*value_out);
  return pill;
}

void createSettingsTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, false);

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), 156);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 8, LV_PART_MAIN);

  createSettingsRow(panel, "Orientation", onFlipDisplay, false, &orientation_label);
  updateOrientationLabel();

  createSettingsRow(panel, "Inactivity", onToggleIdleAction, false, &idle_label);
  updateIdleLabel();

  lv_obj_t* mode_pill = createSettingsRow(panel, "Remote Mode", onToggleControlMode, true, &mode_label);
  if (extended_mode) {
    lv_obj_add_state(mode_pill, LV_STATE_CHECKED);
  }
  updateModeLabel();
}

void initStyles() {
  static const lv_style_prop_t kButtonTransProps[] = {
      LV_STYLE_BG_COLOR, LV_STYLE_BG_OPA, LV_STYLE_BORDER_COLOR,
      LV_STYLE_TEXT_COLOR,
      static_cast<lv_style_prop_t>(0)};
  static lv_style_transition_dsc_t button_trans;
  lv_style_transition_dsc_init(&button_trans, kButtonTransProps,
                               lv_anim_path_ease_out, 150, 0, nullptr);

  lv_style_init(&style_screen);
  lv_style_set_bg_color(&style_screen, lv_color_hex(kColorBg));
  lv_style_set_text_color(&style_screen, lv_color_hex(kColorText));

  lv_style_init(&style_topbar);
  lv_style_set_bg_color(&style_topbar, lv_color_hex(kColorHeaderBar));
  lv_style_set_bg_opa(&style_topbar, LV_OPA_COVER);
  lv_style_set_border_width(&style_topbar, 0);
  lv_style_set_radius(&style_topbar, 0);
  lv_style_set_pad_left(&style_topbar, 10);
  lv_style_set_pad_right(&style_topbar, 10);

  lv_style_init(&style_panel);
  lv_style_set_bg_color(&style_panel, lv_color_hex(kColorSurface));
  lv_style_set_bg_opa(&style_panel, LV_OPA_COVER);
  lv_style_set_border_width(&style_panel, 1);
  lv_style_set_border_color(&style_panel, lv_color_hex(kColorBorder));
  lv_style_set_radius(&style_panel, 12);

  lv_style_init(&style_section_header);
  lv_style_set_text_color(&style_section_header, lv_color_hex(kColorText));
  lv_style_set_text_font(&style_section_header, &lv_font_montserrat_16);
  lv_style_set_text_letter_space(&style_section_header, 1);

  lv_style_init(&style_label_muted);
  lv_style_set_text_color(&style_label_muted, lv_color_hex(kColorText));
  lv_style_set_text_font(&style_label_muted, &lv_font_montserrat_14);

  lv_style_init(&style_button);
  lv_style_set_bg_color(&style_button, lv_color_hex(kColorSurfaceRaised));
  lv_style_set_bg_opa(&style_button, LV_OPA_COVER);
  lv_style_set_border_width(&style_button, 1);
  lv_style_set_border_color(&style_button, lv_color_hex(kColorBorderStrong));
  lv_style_set_radius(&style_button, 9);
  lv_style_set_text_color(&style_button, lv_color_hex(kColorText));
  lv_style_set_transition(&style_button, &button_trans);

  lv_style_init(&style_button_pressed);
  lv_style_set_bg_color(&style_button_pressed, lv_color_hex(kColorSurfacePressed));
  lv_style_set_border_color(&style_button_pressed, lv_color_hex(kColorAccent));

  lv_style_init(&style_button_checked);
  lv_style_set_bg_color(&style_button_checked, lv_color_hex(kColorSelected));
  lv_style_set_border_color(&style_button_checked, lv_color_hex(kColorSelectedBorder));
  lv_style_set_text_color(&style_button_checked, lv_color_hex(0xFFFFFF));

  lv_style_init(&style_slider);
  lv_style_set_bg_color(&style_slider, lv_color_hex(0x223040));
  lv_style_set_bg_opa(&style_slider, LV_OPA_COVER);
  lv_style_set_radius(&style_slider, LV_RADIUS_CIRCLE);

  lv_style_init(&style_slider_indicator);
  lv_style_set_bg_color(&style_slider_indicator, lv_color_hex(kColorAccentDeep));
  lv_style_set_bg_grad_color(&style_slider_indicator, lv_color_hex(kColorAccent));
  lv_style_set_bg_grad_dir(&style_slider_indicator, LV_GRAD_DIR_HOR);
  lv_style_set_radius(&style_slider_indicator, LV_RADIUS_CIRCLE);

  lv_style_init(&style_knob);
  lv_style_set_bg_color(&style_knob, lv_color_hex(0xFFFFFF));
  lv_style_set_border_color(&style_knob, lv_color_hex(kColorAccent));
  lv_style_set_border_width(&style_knob, 2);
  lv_style_set_pad_all(&style_knob, 5);
}

void createUi() {
  Serial.println("UI init: building screen");
  initStyles();

  lv_obj_t* screen = lv_scr_act();
  lv_obj_add_style(screen, &style_screen, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* root = lv_obj_create(screen);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

  lv_obj_t* topbar = lv_obj_create(root);
  lv_obj_add_style(topbar, &style_topbar, LV_PART_MAIN);
  lv_obj_set_size(topbar, LV_PCT(100), 34);
  lv_obj_set_flex_flow(topbar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(topbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollbar_mode(topbar, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* title = lv_img_create(topbar);
  lv_img_set_src(title, &kHeaderLogoImage);
  lv_obj_set_size(title, kWledLogoHeaderWidth, kWledLogoHeaderHeight);

  lv_obj_t* status = lv_obj_create(topbar);
  lv_obj_remove_style_all(status);
#if WLED_CYD_ENABLE_BATTERY
  lv_obj_set_size(status, batteryAvailable() ? 44 : 16, 22);
#else
  lv_obj_set_size(status, 16, 22);
#endif
  lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(status, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(status, 6, LV_PART_MAIN);

  status_dot = lv_obj_create(status);
  lv_obj_remove_style_all(status_dot);
  lv_obj_set_size(status_dot, 10, 10);
  lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(status_dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(status_dot, lv_color_hex(kColorAccent), LV_PART_MAIN);

#if WLED_CYD_ENABLE_BATTERY
  createBatteryIndicator(status);
#endif

  main_tabs = lv_tabview_create(root, LV_DIR_TOP, 30);
  lv_obj_set_size(main_tabs, LV_PCT(100), kScreenHeight - 34);
  lv_obj_set_style_bg_color(main_tabs, lv_color_hex(kColorBg), LV_PART_MAIN);
  lv_obj_set_style_border_width(main_tabs, 0, LV_PART_MAIN);

  lv_obj_t* tab_btns = lv_tabview_get_tab_btns(main_tabs);
  lv_obj_set_style_bg_color(tab_btns, lv_color_hex(kColorHeaderBar), LV_PART_MAIN);
  lv_obj_set_style_border_width(tab_btns, 0, LV_PART_MAIN);
  lv_obj_set_style_text_color(tab_btns, lv_color_hex(kColorTextMuted), LV_PART_MAIN);
  lv_obj_set_style_text_color(tab_btns, lv_color_hex(kColorAccent), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(tab_btns, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(tab_btns, lv_color_hex(kColorAccent), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(tab_btns, 3, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_side(tab_btns, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(tab_btns, lv_color_hex(kColorSurfaceRaised), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(tab_btns, LV_OPA_50, LV_PART_ITEMS | LV_STATE_PRESSED);

  lv_obj_t* live = lv_tabview_add_tab(main_tabs, "Power");
  presets_tab = lv_tabview_add_tab(main_tabs, "Presets");
  fx_tab = lv_tabview_add_tab(main_tabs, "FX");
  lv_obj_t* info = lv_tabview_add_tab(main_tabs, "Info");
  lv_obj_t* settings = lv_tabview_add_tab(main_tabs, "Settings");

  createLiveTab(live);
  createLooksTab(presets_tab);
  createFxTab(fx_tab);
  createInfoTab(info);
  createSettingsTab(settings);
  Serial.println("UI init: ready");
}

void onEspNowSent(const uint8_t*, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    requestStatus(StatusCode::kOk);
  } else {
    requestStatus(StatusCode::kNoAck);
  }
}

void initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (mac_label) {
    lv_label_set_text(mac_label, WiFi.macAddress().c_str());
  }

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(WLED_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    espnow_ready = false;
    requestStatus(StatusCode::kEspFail);
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(onEspNowSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kBroadcastMac, sizeof(kBroadcastMac));
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;

  if (!esp_now_is_peer_exist(peer.peer_addr)) {
    esp_err_t add_result = esp_now_add_peer(&peer);
    if (add_result != ESP_OK) {
      espnow_ready = false;
      requestStatus(StatusCode::kPeerError);
      Serial.printf("ESP-NOW peer add failed: %d\n", add_result);
      return;
    }
  }

  espnow_ready = true;
  requestStatus(StatusCode::kBroadcast);
  Serial.printf("CYD station MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("ESP-NOW channel: %u\n", WLED_ESPNOW_CHANNEL);
  Serial.println("ESP-NOW protocol: WLED native WizMote");
  Serial.println("Pair this MAC in WLED Config -> WiFi Setup -> ESP-NOW remote.");
}

void initDisplay() {
  Serial.println("Display init: starting LovyanGFX");
  Serial.printf("Display profile: panel=%s touch=%s\n",
#if CYD_PANEL_TYPE == CYD_PANEL_ST7789
                "ST7789",
#else
                "ILI9341",
#endif
#if CYD_TOUCH_TYPE == CYD_TOUCH_CST816S
                "CST816S"
#else
                "FT5x06"
#endif
  );
  Serial.printf("TFT pins: sclk=%d mosi=%d miso=%d cs=%d dc=%d rst=%d bl=%d blInvert=%d\n",
                CYD_TFT_SCLK,
                CYD_TFT_MOSI,
                CYD_TFT_MISO,
                CYD_TFT_CS,
                CYD_TFT_DC,
                CYD_TFT_RST,
                CYD_TFT_BL,
                CYD_BACKLIGHT_INVERT);
  Serial.printf("Touch pins: sda=%d scl=%d rst=%d int=%d addr=0x%02X i2cPort=%d\n",
                CYD_TOUCH_SDA,
                CYD_TOUCH_SCL,
                CYD_TOUCH_RST,
                CYD_TOUCH_INT,
                CYD_TOUCH_ADDR,
                CYD_TOUCH_I2C_PORT);
  const bool display_ok = gfx.init();
  Serial.printf("Display init: %s\n", display_ok ? "ok" : "failed");
  applyDisplayRotation();
  gfx.setBrightness(UI_ACTIVE_BRIGHTNESS);

#if UI_SPLASH_MS > 0
  Serial.println("Display splash: WLED logo");
  drawSplash();
#endif

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, draw_buf_1, draw_buf_2, kScreenWidth * kLvglBufferLines);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = kScreenWidth;
  disp_drv.ver_res = kScreenHeight;
  disp_drv.flush_cb = flushDisplay;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = readTouch;
  indev_drv.scroll_limit = 6;
  indev_drv.scroll_throw = 8;
  lv_indev_drv_register(&indev_drv);
}

}

void setup() {
  Serial.begin(115200);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  loadSettings();
  initDisplay();
#if WLED_CYD_ENABLE_BATTERY
  initBatteryMonitor();
#endif
  createUi();
  initEspNow();
  last_touch_ms = millis();
}

void loop() {
  static uint32_t last_tick_ms = millis();
  const uint32_t now = millis();
  const uint32_t elapsed = now - last_tick_ms;
  last_tick_ms = now;
  lv_tick_inc(elapsed);

  if (idle_mode != IdleMode::kAlwaysOn && !display_idle_applied && now - last_touch_ms > UI_DIM_AFTER_MS) {
    display_idle_applied = true;
    gfx.setBrightness(idle_mode == IdleMode::kOff ? 0 : UI_IDLE_BRIGHTNESS);
  }

  applyPendingStatus();
  lv_timer_handler();
  delay(1);
}
