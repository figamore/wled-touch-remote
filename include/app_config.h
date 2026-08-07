#pragma once

// Cheap Yellow Display profiles. Auto mode passively detects known capacitive
// controllers, then uses a one-time touch setup screen for resistive boards.

#define WLED_BOARD_CYD 0
#define WLED_BOARD_JC4880P443 1
#define WLED_BOARD_JC8048W550C 2

#ifndef WLED_BOARD
#define WLED_BOARD WLED_BOARD_CYD
#endif

// Panel transport and touch controller per board.  The rest of the code asks
// for a capability rather than repeating the board comparison.
#define WLED_PANEL_SPI (WLED_BOARD == WLED_BOARD_CYD)
#define WLED_PANEL_DSI (WLED_BOARD == WLED_BOARD_JC4880P443)
#define WLED_PANEL_RGB (WLED_BOARD == WLED_BOARD_JC8048W550C)
#define WLED_TOUCH_GT911 (WLED_PANEL_DSI || WLED_PANEL_RGB)
#define WLED_BOARD_HAS_PSRAM (WLED_PANEL_DSI || WLED_PANEL_RGB)

#if WLED_BOARD == WLED_BOARD_JC8048W550C
// Jingcai JC8048W550C (also sold as Sunton ESP32-8048S050): ESP32-S3 with a 5"
// 800x480 ST7262 RGB panel and GT911 touch.  This panel is wired landscape, so
// the app runs it unrotated and mirrors the framebuffer for the flipped view.
#define WLED_SCREEN_WIDTH 800
#define WLED_SCREEN_HEIGHT 480
// Twenty lines is 32 KiB per buffer; two of those still fit in internal RAM
// beside Wi-Fi and TLS, which is what keeps LVGL rendering out of PSRAM.
#define WLED_LVGL_BUFFER_LINES 20
#define WLED_DISPLAY_ROTATION 0
#define WLED_DISPLAY_ROTATION_FLIPPED 2
#ifndef WLED_CYD_ENABLE_BATTERY
#define WLED_CYD_ENABLE_BATTERY 0
#endif
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
#define CYD_PROFILE_ST7262_GT911 6

#ifndef CYD_HARDWARE_PROFILE
#if WLED_BOARD == WLED_BOARD_JC4880P443
#define CYD_HARDWARE_PROFILE CYD_PROFILE_ST7701_GT911
#elif WLED_BOARD == WLED_BOARD_JC8048W550C
#define CYD_HARDWARE_PROFILE CYD_PROFILE_ST7262_GT911
#else
#define CYD_HARDWARE_PROFILE CYD_PROFILE_AUTO
#endif
#endif

#if WLED_BOARD == WLED_BOARD_JC8048W550C

// The ST7262 is a plain RGB receiver: no command bus and no init sequence, so
// only the parallel timing pins, the backlight and the GT911 are configured.
#define JC8048_TFT_BL 2
#define JC8048_PANEL_WIDTH 800
#define JC8048_PANEL_HEIGHT 480
#define JC8048_RGB_PCLK_HZ (16 * 1000 * 1000)
#define JC8048_RGB_PCLK_ACTIVE_NEG 1
#define JC8048_HSYNC_PULSE_WIDTH 4
#define JC8048_HSYNC_BACK_PORCH 8
#define JC8048_HSYNC_FRONT_PORCH 8
#define JC8048_VSYNC_PULSE_WIDTH 4
#define JC8048_VSYNC_BACK_PORCH 8
#define JC8048_VSYNC_FRONT_PORCH 8
#define JC8048_RGB_IO_HSYNC 39
#define JC8048_RGB_IO_VSYNC 41
#define JC8048_RGB_IO_DE 40
#define JC8048_RGB_IO_PCLK 42
#define JC8048_RGB_IO_DISP -1
// 16-bit RGB565 bus: B0-B4, G0-G5, R0-R4 in that order.
#define JC8048_RGB_DATA_PINS \
  { 8, 3, 46, 9, 1, 5, 6, 7, 15, 16, 4, 45, 48, 47, 21, 14 }
// The LCD DMA reads these internal-RAM buffers instead of the PSRAM frame
// buffer; without them the S3 drops pixels and the image drifts sideways.
#define JC8048_RGB_BOUNCE_LINES 10

// The GT911's reset and interrupt lines are not broken out on this board, so
// its I2C address is whatever the controller latched at power-up.
#define GT911_TOUCH_SDA 19
#define GT911_TOUCH_SCL 20
#define GT911_TOUCH_RST -1
#define GT911_TOUCH_INT -1
#define GT911_TOUCH_ADDR 0x5D

#define CYD_BOARD_CAPACITIVE 1

#elif WLED_BOARD == WLED_BOARD_JC4880P443

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

// The GT911 driver is shared with the JC8048W550C, which wires it elsewhere.
#define GT911_TOUCH_SDA JC4880_TOUCH_SDA
#define GT911_TOUCH_SCL JC4880_TOUCH_SCL
#define GT911_TOUCH_RST JC4880_TOUCH_RST
#define GT911_TOUCH_INT JC4880_TOUCH_INT
#define GT911_TOUCH_ADDR JC4880_TOUCH_ADDR

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
