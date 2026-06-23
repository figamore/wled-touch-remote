#pragma once

// Cheap Yellow Display profiles. Auto mode passively detects known capacitive
// controllers, then uses a one-time touch setup screen for resistive boards.

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

#define CYD_PROFILE_AUTO 0
#define CYD_PROFILE_ST7789_CST816S 1
#define CYD_PROFILE_ILI9341_FT5X06 2
#define CYD_PROFILE_ILI9341_XPT2046 3

#ifndef CYD_HARDWARE_PROFILE
#define CYD_HARDWARE_PROFILE CYD_PROFILE_AUTO
#endif

#define CYD_TFT_SCLK 14
#define CYD_TFT_MOSI 13
#define CYD_TFT_MISO 12
#define CYD_TFT_CS 15
#define CYD_TFT_DC 2
#define CYD_TFT_RST -1
#define CYD_TFT_BL 27
#define CYD_BACKLIGHT_INVERT 0
#define CYD_PANEL_INVERT 0
#define CYD_PANEL_RGB_ORDER 0
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

#ifndef CYD_BOARD_CAPACITIVE
#if CYD_HARDWARE_PROFILE == CYD_PROFILE_ILI9341_XPT2046
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

// WledTouch remotes broadcast across channels so WLED can receive them whether it
// is on Wi-Fi, AP mode, or a different channel. Set to 0 to use only the channel below.
#define WLED_TOUCH_SCAN_CHANNELS 1
#define WLED_ESPNOW_CHANNEL 1

#define UI_IDLE_BRIGHTNESS 72
#define UI_ACTIVE_BRIGHTNESS 255
#define UI_DIM_AFTER_MS 30000
