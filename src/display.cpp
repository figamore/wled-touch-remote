#include "display.h"
#include <Arduino.h>
#if !WLED_TOUCH_SIMULATOR
#include <Preferences.h>
#endif
#include "generated/version.h"
#include "generated/wled_logo_png.h"

namespace {

lv_disp_draw_buf_t draw_buf;
lv_color_t draw_buf_1[kScreenWidth * kLvglBufferLines];
lv_color_t draw_buf_2[kScreenWidth * kLvglBufferLines];

bool display_idle_applied = false;
bool suppress_touch_until_release = false;
uint32_t last_touch_ms = 0;

#if !WLED_TOUCH_SIMULATOR
using HardwareProfile = LGFX::HardwareProfile;

bool loadSavedHardwareProfile(HardwareProfile& profile) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return false;
  }
  const bool has_key = prefs.isKey(kPrefsHardwareProfileKey);
  const uint8_t version = prefs.getUChar(kPrefsHardwareSetupVersionKey, 0);
  const uint8_t code = prefs.getUChar(kPrefsHardwareProfileKey, CYD_PROFILE_AUTO);
  prefs.end();
  return has_key && version == kHardwareSetupVersion && LGFX::hardwareProfileFromCode(code, profile);
}

void saveHardwareProfile(HardwareProfile profile) {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putUChar(kPrefsHardwareProfileKey, LGFX::profileInfo(profile).code);
    prefs.putUChar(kPrefsHardwareSetupVersionKey, kHardwareSetupVersion);
    prefs.end();
  }
}

void clearSavedHardwareProfile() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.remove(kPrefsHardwareProfileKey);
    prefs.remove(kPrefsHardwareSetupVersionKey);
    prefs.end();
  }
}

bool hardwareProfileResetRequested() {
  pinMode(0, INPUT_PULLUP);
  delay(20);
  return digitalRead(0) == LOW;
}

const char* setupTouchName(HardwareProfile profile) {
  return LGFX::profileInfo(profile).has_i2c_touch ? "Capacitive touch" : "Resistive touch";
}

void drawHardwareSetupPrompt(HardwareProfile profile, uint8_t attempt) {
  (void)attempt;
  const uint16_t bg = gfx.color565(4, 8, 14);
  const uint16_t panel = gfx.color565(8, 47, 73);
  const uint16_t cyan = gfx.color565(103, 232, 249);
  const uint16_t teal = gfx.color565(45, 212, 191);
  const uint16_t yellow = gfx.color565(250, 204, 21);

  gfx.fillScreen(bg);
  gfx.setTextDatum(middle_center);
  gfx.setTextSize(1);
  gfx.setTextColor(TFT_WHITE, bg);
  gfx.setFont(&lgfx::fonts::FreeSansBold18pt7b);
  gfx.drawString("Tap the center", kScreenWidth / 2, 30);

  const int16_t cx = kScreenWidth / 2;
  const int16_t cy = 128;
  gfx.fillCircle(cx, cy, 58, panel);
  gfx.drawCircle(cx, cy, 59, cyan);
  gfx.drawCircle(cx, cy, 42, cyan);
  gfx.drawCircle(cx, cy, 25, teal);
  gfx.fillCircle(cx, cy, 12, yellow);
  gfx.drawFastHLine(cx - 72, cy, 144, cyan);
  gfx.drawFastVLine(cx, cy - 72, 144, cyan);

  gfx.setTextColor(yellow, bg);
  gfx.setFont(&lgfx::fonts::FreeSansBold9pt7b);
  gfx.drawString("Hold until detected", kScreenWidth / 2, 208);

  gfx.setTextColor(gfx.color565(203, 213, 225), bg);
  gfx.setFont(&lgfx::fonts::FreeSans9pt7b);
  gfx.drawString(setupTouchName(profile), kScreenWidth / 2, 228);

  gfx.setFont(&lgfx::fonts::Font0);
}

bool waitForSetupTouch(uint32_t timeout_ms) {
  const uint32_t start_ms = millis();
  while (millis() - start_ms < timeout_ms) {
    uint16_t x = 0;
    uint16_t y = 0;
    if (gfx.getTouch(&x, &y)) {
      return true;
    }
    delay(25);
  }
  return false;
}

// Order in which profiles are tried during guided setup.
constexpr HardwareProfile kSetupCycleOrder[] = {
    HardwareProfile::kIli9341Xpt2046,
    HardwareProfile::kSt7789Xpt2046,
    HardwareProfile::kSt7789Cst816s,
    HardwareProfile::kIli9341Ft5x06,
};
constexpr uint8_t kSetupCycleCount = sizeof(kSetupCycleOrder) / sizeof(kSetupCycleOrder[0]);

HardwareProfile profileAtIndex(uint8_t index) {
  return kSetupCycleOrder[index % kSetupCycleCount];
}

uint8_t profileIndex(HardwareProfile profile) {
  for (uint8_t i = 0; i < kSetupCycleCount; ++i) {
    if (kSetupCycleOrder[i] == profile) {
      return i;
    }
  }
  return 0;
}

HardwareProfile runGuidedHardwareSetup(HardwareProfile first_profile) {
  uint8_t attempt = 1;
  uint8_t first_index = profileIndex(first_profile);

  pinMode(CYD_TFT_BL, OUTPUT);
  digitalWrite(CYD_TFT_BL, CYD_BACKLIGHT_INVERT ? LOW : HIGH);
  pinMode(CYD_ALT_TFT_BL, OUTPUT);
  digitalWrite(CYD_ALT_TFT_BL, CYD_BACKLIGHT_INVERT ? LOW : HIGH);

  gfx.init();
  applyDisplayRotation();
  gfx.setBrightness(UI_ACTIVE_BRIGHTNESS);

  while (true) {
    for (uint8_t i = 0; i < kSetupCycleCount; ++i) {
      const HardwareProfile profile = profileAtIndex(first_index + i);
      Serial.printf("Hardware setup: trying %s\n", LGFX::profileInfo(profile).profile_name);
      gfx.setTouchProfile(profile);
      drawHardwareSetupPrompt(profile, attempt);
      if (waitForSetupTouch(2500)) {
        const uint16_t detected_bg = gfx.color565(7, 12, 18);
        gfx.fillScreen(detected_bg);
        gfx.setTextDatum(middle_center);
        gfx.setTextSize(1);
        gfx.setTextColor(gfx.color565(52, 211, 153), detected_bg);
        gfx.setFont(&lgfx::fonts::FreeSansBold18pt7b);
        gfx.drawString("Touch detected", kScreenWidth / 2, 104);
        gfx.setTextColor(TFT_WHITE, detected_bg);
        gfx.setFont(&lgfx::fonts::FreeSans9pt7b);
        gfx.drawString("Saving setup", kScreenWidth / 2, 140);
        gfx.setFont(&lgfx::fonts::Font0);
        delay(700);
        return profile;
      }
      ++attempt;
    }
  }
}
#endif

void flushDisplay(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
  const int32_t width = area->x2 - area->x1 + 1;
  const int32_t height = area->y2 - area->y1 + 1;

#if WLED_TOUCH_SIMULATOR
  for (int32_t y = 0; y < height; ++y) {
    const int32_t dst_y = area->y1 + y;
    if (dst_y < 0 || dst_y >= kScreenHeight) {
      continue;
    }
    for (int32_t x = 0; x < width; ++x) {
      const int32_t dst_x = area->x1 + x;
      if (dst_x < 0 || dst_x >= kScreenWidth) {
        continue;
      }
      sim_framebuffer[dst_y * kScreenWidth + dst_x] = color_p[y * width + x].full;
    }
  }
#endif

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

}  // namespace

LGFX gfx;

#if WLED_TOUCH_SIMULATOR
uint16_t sim_framebuffer[kScreenWidth * kScreenHeight] = {};
#endif

void touchActivity() {
  last_touch_ms = millis();
  display_idle_applied = false;
  gfx.setBrightness(UI_ACTIVE_BRIGHTNESS);
}

void applyDisplayRotation() {
#if WLED_TOUCH_SIMULATOR
  gfx.setRotation(0);
#else
  gfx.setRotation(display_flipped ? 3 : 1);
#endif
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

    gfx.setTextColor(gfx.color565(150, 158, 170), TFT_BLACK);
    gfx.setTextDatum(middle_center);
    gfx.setTextSize(1);
    const String version = String("Firmware v") + kAppVersion;
    gfx.drawString(version.c_str(),
                   kScreenWidth / 2,
                   y + kWledLogoHeight + 18);
  }

  if (!can_draw_logo) {
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.setTextDatum(middle_center);
    gfx.setTextSize(3);
    gfx.drawString("WLED", kScreenWidth / 2, kScreenHeight / 2);
    gfx.setTextColor(gfx.color565(150, 158, 170), TFT_BLACK);
    gfx.setTextSize(1);
    const String version = String("v") + kAppVersion;
    gfx.drawString(version.c_str(), kScreenWidth / 2, kScreenHeight / 2 + 34);
  }

  delay(UI_SPLASH_MS);
  gfx.fillScreen(TFT_BLACK);
}

void initDisplay() {
  Serial.println("Display init: starting LovyanGFX");
  bool display_already_initialized = false;
#if WLED_TOUCH_SIMULATOR
  lgfx::Panel_sdl::setup();
  Serial.println("Display profile: simulator SDL");
#else
#if CYD_HARDWARE_PROFILE == CYD_PROFILE_AUTO
  if (hardwareProfileResetRequested()) {
    Serial.println("Display profile: BOOT held, clearing saved hardware profile");
    clearSavedHardwareProfile();
  }

  HardwareProfile saved_profile;
  if (loadSavedHardwareProfile(saved_profile)) {
    gfx.setHardwareProfile(saved_profile);
    Serial.println("Display profile: loaded saved hardware profile");
  } else {
    gfx.autoConfigure();
    const bool has_capacitive_hint = gfx.profileDetected();
    const HardwareProfile first_profile = gfx.hardwareProfile();
    if (!has_capacitive_hint) {
      gfx.setHardwareProfile(first_profile);
    }
    Serial.printf("Display profile: touch setup required%s\n",
                  has_capacitive_hint ? " (capacitive hint found)" : "");
    const HardwareProfile selected_profile = runGuidedHardwareSetup(first_profile);
    saveHardwareProfile(selected_profile);
    gfx.setHardwareProfile(selected_profile);
    gfx.init();
    applyDisplayRotation();
    gfx.setBrightness(UI_ACTIVE_BRIGHTNESS);
    display_already_initialized = true;
  }
#else
  gfx.autoConfigure();
#endif
  Serial.printf("Display profile: %s%s\n",
                gfx.profileName(),
#if CYD_HARDWARE_PROFILE == CYD_PROFILE_AUTO
                display_already_initialized ? " (touch setup)" : (gfx.profileDetected() ? " (auto)" : " (fallback)")
#else
                " (manual)"
#endif
  );
  Serial.printf("Display drivers: panel=%s touch=%s\n", gfx.panelName(), gfx.touchName());
#endif
  Serial.printf("TFT pins: sclk=%d mosi=%d miso=%d cs=%d dc=%d rst=%d bl=%d blInvert=%d\n",
                CYD_TFT_SCLK,
                CYD_TFT_MOSI,
                CYD_TFT_MISO,
                CYD_TFT_CS,
                CYD_TFT_DC,
                CYD_TFT_RST,
#if WLED_TOUCH_SIMULATOR
                CYD_TFT_BL,
#else
                gfx.backlightPin(),
#endif
                CYD_BACKLIGHT_INVERT);
#if !WLED_TOUCH_SIMULATOR
  if (!gfx.hasI2cTouch()) {
    Serial.printf("Touch pins: sclk=%d mosi=%d miso=%d cs=%d int=%d spiHost=%d\n",
                  CYD_RES_TOUCH_SCLK,
                  CYD_RES_TOUCH_MOSI,
                  CYD_RES_TOUCH_MISO,
                  CYD_RES_TOUCH_CS,
                  CYD_RES_TOUCH_INT,
                  CYD_RES_TOUCH_SPI_HOST);
  } else
#endif
  {
    Serial.printf("Touch pins: sda=%d scl=%d rst=%d int=%d addr=0x%02X i2cPort=%d\n",
                  CYD_TOUCH_SDA,
                  CYD_TOUCH_SCL,
                  CYD_TOUCH_RST,
#if WLED_TOUCH_SIMULATOR
                  CYD_TOUCH_INT,
                  CYD_TOUCH_ADDR,
                  CYD_TOUCH_I2C_PORT
#else
                  gfx.hardwareProfile() == LGFX::HardwareProfile::kIli9341Ft5x06 ? CYD_ALT_TOUCH_INT : CYD_TOUCH_INT,
                  gfx.hardwareProfile() == LGFX::HardwareProfile::kIli9341Ft5x06 ? CYD_ALT_TOUCH_ADDR : CYD_TOUCH_ADDR,
                  gfx.hardwareProfile() == LGFX::HardwareProfile::kIli9341Ft5x06 ? CYD_ALT_TOUCH_I2C_PORT : CYD_TOUCH_I2C_PORT
#endif
    );
  }
  const bool display_ok = display_already_initialized || gfx.init();
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

void displayUpdateIdle(uint32_t now) {
  if (idle_mode != IdleMode::kAlwaysOn && !display_idle_applied && now - last_touch_ms > UI_DIM_AFTER_MS) {
    display_idle_applied = true;
    gfx.setBrightness(idle_mode == IdleMode::kOff ? 0 : UI_IDLE_BRIGHTNESS);
  }
}
