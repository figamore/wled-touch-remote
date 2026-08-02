#pragma once

// Cheap Yellow Display profiles. Auto mode passively detects known capacitive
// controllers, then uses a one-time touch setup screen for resistive boards.

#define WLED_BOARD_CYD 0
#define WLED_BOARD_JC4880P443 1

#ifndef WLED_BOARD
#define WLED_BOARD WLED_BOARD_CYD
#endif

#if WLED_BOARD == WLED_BOARD_JC4880P443
// Guition JC4880P443: ESP32-P4 with a 4.3" 480x800 ST7701S MIPI-DSI panel and
// GT911 touch. The native backend keeps the panel in portrait and maps the
// LVGL flush rectangles itself, including the 180-degree setting.
#define WLED_SCREEN_WIDTH 480
#define WLED_SCREEN_HEIGHT 800
#define WLED_LVGL_BUFFER_LINES 40
#define WLED_DISPLAY_ROTATION 0
#define WLED_DISPLAY_ROTATION_FLIPPED 2
#ifndef WLED_CYD_ENABLE_BATTERY
#define WLED_CYD_ENABLE_BATTERY 0
#endif
#endif

// CYD panels are wired portrait too, but the app runs them landscape.
#ifndef WLED_DISPLAY_ROTATION
#define WLED_DISPLAY_ROTATION 1
#endif

#ifndef WLED_DISPLAY_ROTATION_FLIPPED
#define WLED_DISPLAY_ROTATION_FLIPPED 3
#endif

#ifndef WLED_SCREEN_WIDTH
#define WLED_SCREEN_WIDTH 320
#endif

#ifndef WLED_SCREEN_HEIGHT
#define WLED_SCREEN_HEIGHT 240
#endif

#ifndef WLED_LVGL_BUFFER_LINES
#define WLED_LVGL_BUFFER_LINES 40
#endif

#ifndef WLED_CYD_ENABLE_BATTERY
#define WLED_CYD_ENABLE_BATTERY 1
#endif

#ifndef WLED_CYD_ENABLE_SHUTDOWN
#define WLED_CYD_ENABLE_SHUTDOWN 0
#endif

#ifndef WLED_CYD_SHUTDOWN_GPIO
#define WLED_CYD_SHUTDOWN_GPIO 17
#endif

#ifndef WLED_CYD_SHUTDOWN_HOLD_MS
#define WLED_CYD_SHUTDOWN_HOLD_MS 10200
#endif

#ifndef WLED_CYD_SHUTDOWN_DOUBLE_TAP_MS
#define WLED_CYD_SHUTDOWN_DOUBLE_TAP_MS 650
#endif

#ifndef WLED_CYD_SHUTDOWN_TAP_LOCKOUT_MS
#define WLED_CYD_SHUTDOWN_TAP_LOCKOUT_MS 60
#endif

#ifndef WLED_CYD_SHUTDOWN_PULLUP
#define WLED_CYD_SHUTDOWN_PULLUP 0
#endif

#ifndef WLED_CYD_SHUTDOWN_DEBUG
#define WLED_CYD_SHUTDOWN_DEBUG 0
#endif

#ifndef WLED_CYD_SHUTDOWN_DEBUG_STATUS_MS
#define WLED_CYD_SHUTDOWN_DEBUG_STATUS_MS 1000
#endif

#ifndef WLED_CYD_ENABLE_SERIAL_SCREENSHOT
#define WLED_CYD_ENABLE_SERIAL_SCREENSHOT 0
#endif

#ifndef WLED_CYD_SERIAL_BAUD
#define WLED_CYD_SERIAL_BAUD 115200
#endif

// GitHub Releases is the update channel for the device application.  These
// remain compile-time values so an image can only ever update from this
// project's release feed, never from a URL supplied by the UI or network.
#ifndef WLED_UPDATE_GITHUB_OWNER
#define WLED_UPDATE_GITHUB_OWNER "figamore"
#endif

#ifndef WLED_UPDATE_GITHUB_REPOSITORY
#define WLED_UPDATE_GITHUB_REPOSITORY "wled-touch-remote"
#endif

// On battery-capable CYDs, do not begin a flash below this level unless the
// existing battery monitor reports charging. Boards without that monitor are
// treated as externally powered and are not blocked by this check.
#ifndef WLED_UPDATE_MIN_BATTERY_LEVEL
#define WLED_UPDATE_MIN_BATTERY_LEVEL 30
#endif

#define CYD_PROFILE_AUTO 0
#define CYD_PROFILE_ST7789_CST816S 1
#define CYD_PROFILE_ILI9341_FT5X06 2
#define CYD_PROFILE_ILI9341_XPT2046 3
#define CYD_PROFILE_ST7789_XPT2046 4
#define CYD_PROFILE_ST7701_GT911 5

#ifndef CYD_HARDWARE_PROFILE
#if WLED_BOARD == WLED_BOARD_JC4880P443
#define CYD_HARDWARE_PROFILE CYD_PROFILE_ST7701_GT911
#else
#define CYD_HARDWARE_PROFILE CYD_PROFILE_AUTO
#endif
#endif

#if WLED_BOARD == WLED_BOARD_JC4880P443

// Panel is driven over MIPI-DSI, so there are no SPI pins; only the reset and
// backlight lines are GPIOs. Timings match the panel's 480x800 ST7701S module.
#define JC4880_TFT_RST 5
#define JC4880_TFT_BL 23
#define JC4880_PANEL_WIDTH 480
#define JC4880_PANEL_HEIGHT 800
#define JC4880_DSI_LANES 2
#define JC4880_DSI_LANE_MBPS 500
#define JC4880_DSI_LDO_CHANNEL 3
#define JC4880_DSI_LDO_MV 2500
#define JC4880_DPI_CLOCK_MHZ 34
#define JC4880_HSYNC_BACK_PORCH 42
#define JC4880_HSYNC_PULSE_WIDTH 12
#define JC4880_HSYNC_FRONT_PORCH 42
#define JC4880_VSYNC_BACK_PORCH 8
#define JC4880_VSYNC_PULSE_WIDTH 2
#define JC4880_VSYNC_FRONT_PORCH 166

#define JC4880_TOUCH_SDA 7
#define JC4880_TOUCH_SCL 8
#define JC4880_TOUCH_RST 3
#define JC4880_TOUCH_INT -1
#define JC4880_TOUCH_ADDR 0x5D
#define JC4880_TOUCH_I2C_PORT 0

#define CYD_BOARD_CAPACITIVE 1

#else

#define CYD_TFT_SCLK 14
#define CYD_TFT_MOSI 13
#define CYD_TFT_MISO 12
#define CYD_TFT_CS 15
#define CYD_TFT_DC 2
#define CYD_TFT_RST -1
#define CYD_TFT_BL 27
#define CYD_BACKLIGHT_INVERT 0
#define CYD_PANEL_INVERT 0
// The stock CYD panels use RGB component order. The BGR MADCTL setting swaps
// red and blue (noticeable in the color wheel and WLED live preview).
#define CYD_PANEL_RGB_ORDER 1
#define CYD_PANEL_OFFSET_ROTATION 0

#define CYD_TOUCH_SDA 33
#define CYD_TOUCH_SCL 32
#define CYD_TOUCH_INT -1
#define CYD_TOUCH_RST 25
#define CYD_TOUCH_ADDR 0x15
#define CYD_TOUCH_I2C_PORT 0
#define CYD_TOUCH_OFFSET_ROTATION 0

#define CYD_ALT_TFT_BL 21
#define CYD_ALT_TOUCH_INT 36
#define CYD_ALT_TOUCH_ADDR 0x38
#define CYD_ALT_TOUCH_I2C_PORT 1

#define CYD_RES_TFT_BL 21
#define CYD_RES_PANEL_OFFSET_ROTATION 2
#define CYD_RES_ST7789_PANEL_OFFSET_ROTATION 0
#define CYD_RES_TOUCH_X_MIN 300
#define CYD_RES_TOUCH_X_MAX 3900
#define CYD_RES_TOUCH_Y_MIN 3700
#define CYD_RES_TOUCH_Y_MAX 200
#define CYD_RES_TOUCH_INT -1
#define CYD_RES_TOUCH_SPI_HOST -1
#define CYD_RES_TOUCH_SCLK 25
#define CYD_RES_TOUCH_MOSI 32
#define CYD_RES_TOUCH_MISO 39
#define CYD_RES_TOUCH_CS 33
#define CYD_RES_TOUCH_OFFSET_ROTATION 0
#define CYD_RES_ST7789_TOUCH_OFFSET_ROTATION 2

#endif

#ifndef CYD_BOARD_CAPACITIVE
#if CYD_HARDWARE_PROFILE == CYD_PROFILE_ILI9341_XPT2046 || CYD_HARDWARE_PROFILE == CYD_PROFILE_ST7789_XPT2046
#define CYD_BOARD_CAPACITIVE 0
#else
#define CYD_BOARD_CAPACITIVE 1
#endif
#endif

#define CYD_BATTERY_ADC (WLED_CYD_ENABLE_BATTERY && CYD_BOARD_CAPACITIVE)
#define CYD_BATTERY_ADC_PIN 39
#define CYD_BATTERY_ADC_MULTIPLIER_NUM 1534
#define CYD_BATTERY_ADC_MULTIPLIER_DEN 1000

#define UI_SPLASH_MS 1000

#define UI_IDLE_BRIGHTNESS 72
#define UI_ACTIVE_BRIGHTNESS 255
#define UI_DIM_AFTER_MS 30000
