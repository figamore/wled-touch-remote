#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <lvgl.h>
#include <Preferences.h>

#include "app_config.h"
#include "generated/wled_logo_png.h"

namespace {

constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 240;
constexpr size_t kLvglBufferLines = 20;
constexpr uint8_t kWizMoteButtonOn = 1;
constexpr uint8_t kWizMoteButtonOff = 2;
constexpr uint8_t kWizMoteButtonBrightDown = 8;
constexpr uint8_t kWizMoteButtonBrightUp = 9;
constexpr uint8_t kWizMoteButtonOne = 16;
constexpr uint8_t kWizMotePresetCount = 7;
constexpr uint8_t kBroadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr const char* kPrefsNamespace = "wled-cyd";
constexpr const char* kPrefsFlipKey = "flip";
constexpr const char* kPrefsIdleOffKey = "idleOff";

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
bool idle_display_off = false;
bool display_idle_applied = false;
bool suppress_touch_until_release = false;
uint32_t last_touch_ms = 0;
volatile bool pending_status = false;
volatile uint8_t pending_status_code = static_cast<uint8_t>(StatusCode::kBoot);

lv_obj_t* status_label = nullptr;
lv_obj_t* status_dot = nullptr;
lv_obj_t* power_button = nullptr;
lv_obj_t* power_button_label = nullptr;
lv_obj_t* brightness_label = nullptr;
lv_obj_t* mac_label = nullptr;
lv_obj_t* settings_dialog = nullptr;
lv_obj_t* orientation_label = nullptr;
lv_obj_t* idle_label = nullptr;

lv_style_t style_screen;
lv_style_t style_topbar;
lv_style_t style_panel;
lv_style_t style_label_muted;
lv_style_t style_button;
lv_style_t style_button_checked;
lv_style_t style_slider;
lv_style_t style_slider_indicator;
lv_style_t style_knob;

const lv_img_dsc_t kHeaderLogoImage = {
    {LV_IMG_CF_TRUE_COLOR, 0, 0, kWledLogoHeaderWidth, kWledLogoHeaderHeight},
    kWledLogoHeaderPixelCount * sizeof(kWledLogoHeaderPixels[0]),
    reinterpret_cast<const uint8_t*>(kWledLogoHeaderPixels),
};

void setStatusDirect(const char* text, lv_color_t color) {
  if (status_label) {
    lv_label_set_text(status_label, text);
  }
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
      setStatusDirect("boot", lv_color_hex(0x38BDF8));
      break;
    case StatusCode::kOffline:
      setStatusDirect("offline", lv_color_hex(0xF87171));
      break;
    case StatusCode::kSent:
      setStatusDirect("sent", lv_color_hex(0x34D399));
      break;
    case StatusCode::kOk:
      setStatusDirect("ok", lv_color_hex(0x34D399));
      break;
    case StatusCode::kNoAck:
      setStatusDirect("no ack", lv_color_hex(0xA78BFA));
      break;
    case StatusCode::kSendError:
      setStatusDirect("send err", lv_color_hex(0xF87171));
      break;
    case StatusCode::kEspFail:
      setStatusDirect("esp fail", lv_color_hex(0xF87171));
      break;
    case StatusCode::kPeerError:
      setStatusDirect("peer err", lv_color_hex(0xF87171));
      break;
    case StatusCode::kBroadcast:
      setStatusDirect("broadcast", lv_color_hex(0x34D399));
      break;
    case StatusCode::kReady:
      setStatusDirect("ready", lv_color_hex(0x34D399));
      break;
  }
}

void touchActivity() {
  last_touch_ms = millis();
  display_idle_applied = false;
  gfx.setBrightness(UI_ACTIVE_BRIGHTNESS);
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
    idle_display_off = prefs.getBool(kPrefsIdleOffKey, false);
    prefs.end();
  }
  Serial.printf("Display orientation: %s\n", display_flipped ? "flipped" : "normal");
  Serial.printf("Display idle action: %s\n", idle_display_off ? "off" : "dim");
}

void saveSettings() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putBool(kPrefsFlipKey, display_flipped);
    prefs.putBool(kPrefsIdleOffKey, idle_display_off);
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
    if (display_idle_applied || suppress_touch_until_release) {
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
    lv_label_set_text(power_button_label, state.power ? "Power On" : "Power Off");
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
  if (preset < 1 || preset > kWizMotePresetCount) {
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

void onPreset(lv_event_t* event) {
  const uintptr_t preset = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  sendPreset(static_cast<uint8_t>(preset));
  setPowerUi(true);
}

void onPing(lv_event_t*) {
  sendWizMoteButton(kWizMoteButtonOn);
}

void closeSettingsDialog(lv_event_t*) {
  if (settings_dialog) {
    lv_obj_del(settings_dialog);
    settings_dialog = nullptr;
    orientation_label = nullptr;
    idle_label = nullptr;
  }
}

void updateOrientationLabel() {
  if (orientation_label) {
    lv_label_set_text(orientation_label, display_flipped ? "Orientation: Flipped" : "Orientation: Normal");
  }
}

void updateIdleLabel() {
  if (idle_label) {
    lv_label_set_text(idle_label, idle_display_off ? "Inactivity: Display Off" : "Inactivity: Dim");
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
  idle_display_off = !idle_display_off;
  saveSettings();
  updateIdleLabel();
  touchActivity();
}

void addLabel(lv_obj_t* parent, const char* text, lv_coord_t width = LV_SIZE_CONTENT) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_add_style(label, &style_label_muted, LV_PART_MAIN);
  lv_obj_set_width(label, width);
}

lv_obj_t* createPanel(lv_obj_t* parent) {
  lv_obj_t* panel = lv_obj_create(parent);
  lv_obj_add_style(panel, &style_panel, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
  return panel;
}

void openSettingsDialog(lv_event_t*) {
  if (settings_dialog) {
    return;
  }

  settings_dialog = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(settings_dialog);
  lv_obj_set_size(settings_dialog, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(settings_dialog, lv_color_hex(0x020617), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(settings_dialog, LV_OPA_70, LV_PART_MAIN);

  lv_obj_t* panel = createPanel(settings_dialog);
  lv_obj_set_size(panel, 280, 186);
  lv_obj_center(panel);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(panel);
  lv_label_set_text(title, "Settings");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);

  orientation_label = lv_label_create(panel);
  lv_obj_add_style(orientation_label, &style_label_muted, LV_PART_MAIN);
  updateOrientationLabel();

  idle_label = lv_label_create(panel);
  lv_obj_add_style(idle_label, &style_label_muted, LV_PART_MAIN);
  updateIdleLabel();

  lv_obj_t* row_one = lv_obj_create(panel);
  lv_obj_remove_style_all(row_one);
  lv_obj_set_size(row_one, LV_PCT(100), 38);
  lv_obj_set_flex_flow(row_one, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row_one, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* flip = lv_btn_create(row_one);
  lv_obj_add_style(flip, &style_button, LV_PART_MAIN);
  lv_obj_set_size(flip, 118, 34);
  lv_obj_add_event_cb(flip, onFlipDisplay, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* flip_label = lv_label_create(flip);
  lv_label_set_text(flip_label, "Flip");
  lv_obj_center(flip_label);

  lv_obj_t* idle = lv_btn_create(row_one);
  lv_obj_add_style(idle, &style_button, LV_PART_MAIN);
  lv_obj_set_size(idle, 118, 34);
  lv_obj_add_event_cb(idle, onToggleIdleAction, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* idle_button_label = lv_label_create(idle);
  lv_label_set_text(idle_button_label, "Idle Mode");
  lv_obj_center(idle_button_label);

  lv_obj_t* close = lv_btn_create(panel);
  lv_obj_add_style(close, &style_button, LV_PART_MAIN);
  lv_obj_set_size(close, LV_PCT(100), 34);
  lv_obj_add_event_cb(close, closeSettingsDialog, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* close_label = lv_label_create(close);
  lv_label_set_text(close_label, "Close");
  lv_obj_center(close_label);
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
  lv_obj_set_height(row, 30);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);

  lv_obj_t* slider = lv_slider_create(row);
  lv_slider_set_range(slider, min, max);
  lv_slider_set_value(slider, value, LV_ANIM_OFF);
  lv_obj_set_size(slider, 190, 8);
  lv_obj_add_style(slider, &style_slider, LV_PART_MAIN);
  lv_obj_add_style(slider, &style_slider_indicator, LV_PART_INDICATOR);
  lv_obj_add_style(slider, &style_knob, LV_PART_KNOB);
  lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, nullptr);

  *value_label = lv_label_create(row);
  lv_obj_set_width(*value_label, 34);
  lv_label_set_text_fmt(*value_label, "%d", value);
  lv_obj_set_style_text_align(*value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

  return slider;
}

void createLiveTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), 156);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 12, LV_PART_MAIN);

  power_button = lv_btn_create(panel);
  lv_obj_add_style(power_button, &style_button, LV_PART_MAIN);
  lv_obj_add_style(power_button, &style_button_checked, LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_size(power_button, LV_PCT(100), 50);
  lv_obj_add_flag(power_button, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_add_event_cb(power_button, onPower, LV_EVENT_CLICKED, nullptr);

  power_button_label = lv_label_create(power_button);
  lv_label_set_text(power_button_label, "Power Off");
  lv_obj_set_style_text_font(power_button_label, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_center(power_button_label);

  addLabel(panel, "Brightness");
  createSlider(panel, 1, 255, state.brightness, onBrightness, &brightness_label);

}

void createLooksTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), 156);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);

  addLabel(panel, "Presets");

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

  for (uintptr_t i = 1; i <= kWizMotePresetCount; ++i) {
    lv_obj_t* parent = i <= 4 ? row_one : row_two;
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_add_style(btn, &style_button, LV_PART_MAIN);
    lv_obj_set_size(btn, 64, 38);
    lv_obj_add_event_cb(btn, onPreset, LV_EVENT_CLICKED, reinterpret_cast<void*>(i));

    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text_fmt(label, "%u", static_cast<unsigned>(i));
    lv_obj_center(label);
  }
}

void createInfoTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);

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
  lv_obj_set_style_text_color(mac_label, lv_color_hex(0x38BDF8), LV_PART_MAIN);

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
  lv_obj_add_style(ping, &style_button, LV_PART_MAIN);
  lv_obj_set_size(ping, 118, 34);
  lv_obj_add_event_cb(ping, onPing, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* label = lv_label_create(ping);
  lv_label_set_text(label, "Ping WLED");
  lv_obj_center(label);

  lv_obj_t* settings = lv_btn_create(actions);
  lv_obj_add_style(settings, &style_button, LV_PART_MAIN);
  lv_obj_set_size(settings, 118, 34);
  lv_obj_add_event_cb(settings, openSettingsDialog, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* settings_label = lv_label_create(settings);
  lv_label_set_text(settings_label, "Settings");
  lv_obj_center(settings_label);
}

void initStyles() {
  lv_style_init(&style_screen);
  lv_style_set_bg_color(&style_screen, lv_color_hex(0x0B1014));
  lv_style_set_text_color(&style_screen, lv_color_hex(0xEAF2F8));

  lv_style_init(&style_topbar);
  lv_style_set_bg_color(&style_topbar, lv_color_hex(0x111821));
  lv_style_set_bg_opa(&style_topbar, LV_OPA_COVER);
  lv_style_set_border_width(&style_topbar, 0);
  lv_style_set_radius(&style_topbar, 0);
  lv_style_set_pad_left(&style_topbar, 10);
  lv_style_set_pad_right(&style_topbar, 10);

  lv_style_init(&style_panel);
  lv_style_set_bg_color(&style_panel, lv_color_hex(0x18212B));
  lv_style_set_bg_opa(&style_panel, LV_OPA_COVER);
  lv_style_set_border_width(&style_panel, 1);
  lv_style_set_border_color(&style_panel, lv_color_hex(0x263342));
  lv_style_set_radius(&style_panel, 8);

  lv_style_init(&style_label_muted);
  lv_style_set_text_color(&style_label_muted, lv_color_hex(0x8FA3B8));
  lv_style_set_text_font(&style_label_muted, &lv_font_montserrat_12);

  lv_style_init(&style_button);
  lv_style_set_bg_color(&style_button, lv_color_hex(0x22303C));
  lv_style_set_bg_opa(&style_button, LV_OPA_COVER);
  lv_style_set_border_width(&style_button, 1);
  lv_style_set_border_color(&style_button, lv_color_hex(0x344556));
  lv_style_set_radius(&style_button, 7);
  lv_style_set_shadow_width(&style_button, 0);
  lv_style_set_text_color(&style_button, lv_color_hex(0xEAF2F8));

  lv_style_init(&style_button_checked);
  lv_style_set_bg_color(&style_button_checked, lv_color_hex(0x0F766E));
  lv_style_set_border_color(&style_button_checked, lv_color_hex(0x2DD4BF));
  lv_style_set_text_color(&style_button_checked, lv_color_hex(0xFFFFFF));

  lv_style_init(&style_slider);
  lv_style_set_bg_color(&style_slider, lv_color_hex(0x2A3846));
  lv_style_set_bg_opa(&style_slider, LV_OPA_COVER);
  lv_style_set_radius(&style_slider, 4);

  lv_style_init(&style_slider_indicator);
  lv_style_set_bg_color(&style_slider_indicator, lv_color_hex(0x38BDF8));
  lv_style_set_radius(&style_slider_indicator, 4);

  lv_style_init(&style_knob);
  lv_style_set_bg_color(&style_knob, lv_color_hex(0xEAF2F8));
  lv_style_set_border_color(&style_knob, lv_color_hex(0x38BDF8));
  lv_style_set_border_width(&style_knob, 2);
  lv_style_set_pad_all(&style_knob, 4);
}

void createUi() {
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
  lv_obj_set_size(status, 92, 22);
  lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(status, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(status, 6, LV_PART_MAIN);

  status_dot = lv_obj_create(status);
  lv_obj_remove_style_all(status_dot);
  lv_obj_set_size(status_dot, 9, 9);
  lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(status_dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(status_dot, lv_color_hex(0x38BDF8), LV_PART_MAIN);

  status_label = lv_label_create(status);
  lv_label_set_text(status_label, "boot");
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xCBD5E1), LV_PART_MAIN);

  lv_obj_t* tabs = lv_tabview_create(root, LV_DIR_TOP, 30);
  lv_obj_set_size(tabs, LV_PCT(100), kScreenHeight - 34);
  lv_obj_set_style_bg_color(tabs, lv_color_hex(0x0B1014), LV_PART_MAIN);
  lv_obj_set_style_border_width(tabs, 0, LV_PART_MAIN);

  lv_obj_t* tab_btns = lv_tabview_get_tab_btns(tabs);
  lv_obj_set_style_bg_color(tab_btns, lv_color_hex(0x111821), LV_PART_MAIN);
  lv_obj_set_style_text_color(tab_btns, lv_color_hex(0x9DB3C7), LV_PART_MAIN);
  lv_obj_set_style_text_color(tab_btns, lv_color_hex(0xFFFFFF), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(tab_btns, lv_color_hex(0x2563EB), LV_PART_ITEMS | LV_STATE_CHECKED);

  lv_obj_t* live = lv_tabview_add_tab(tabs, "Power");
  lv_obj_t* looks = lv_tabview_add_tab(tabs, "Presets");
  lv_obj_t* info = lv_tabview_add_tab(tabs, "Info");

  createLiveTab(live);
  createLooksTab(looks);
  createInfoTab(info);
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
  lv_indev_drv_register(&indev_drv);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  loadSettings();
  initDisplay();
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

  if (!display_idle_applied && now - last_touch_ms > UI_DIM_AFTER_MS) {
    display_idle_applied = true;
    gfx.setBrightness(idle_display_off ? 0 : UI_IDLE_BRIGHTNESS);
  }

  applyPendingStatus();
  lv_timer_handler();
  delay(5);
}
