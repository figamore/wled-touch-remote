#include "display.h"
#include <Arduino.h>
#include "generated/version.h"
#include "generated/wled_logo_png.h"

namespace {

lv_disp_draw_buf_t draw_buf;
lv_color_t draw_buf_1[kScreenWidth * kLvglBufferLines];
lv_color_t draw_buf_2[kScreenWidth * kLvglBufferLines];

bool display_idle_applied = false;
bool suppress_touch_until_release = false;
uint32_t last_touch_ms = 0;

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
    gfx.drawString(String("Firmware v") + kAppVersion,
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
    gfx.drawString(String("v") + kAppVersion, kScreenWidth / 2, kScreenHeight / 2 + 34);
  }

  delay(UI_SPLASH_MS);
  gfx.fillScreen(TFT_BLACK);
}

void initDisplay() {
  Serial.println("Display init: starting LovyanGFX");
#if WLED_TOUCH_SIMULATOR
  lgfx::Panel_sdl::setup();
  Serial.println("Display profile: simulator SDL");
#else
  gfx.autoConfigure();
  Serial.printf("Display profile: %s%s\n",
                gfx.profileName(),
#if CYD_HARDWARE_PROFILE == CYD_PROFILE_AUTO
                gfx.profileDetected() ? " (auto detected)" : " (auto fallback)"
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
                gfx.hardwareProfile() == LGFX::HardwareProfile::kIli9341Ft5x06 ? CYD_ALT_TFT_BL : CYD_TFT_BL,
#endif
                CYD_BACKLIGHT_INVERT);
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

void displayUpdateIdle(uint32_t now) {
  if (idle_mode != IdleMode::kAlwaysOn && !display_idle_applied && now - last_touch_ms > UI_DIM_AFTER_MS) {
    display_idle_applied = true;
    gfx.setBrightness(idle_mode == IdleMode::kOff ? 0 : UI_IDLE_BRIGHTNESS);
  }
}
