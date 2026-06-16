#pragma once

// Cheap Yellow Display capacitive profiles.
// The default matches the tested CYD capacitive profile that works on
// JC2432W328C-style boards: ST7789 + CST816S + backlight on GPIO27.

#define CYD_PANEL_ILI9341 1
#define CYD_PANEL_ST7789 2
#define CYD_TOUCH_FT5X06 1
#define CYD_TOUCH_CST816S 2

#define CYD_PANEL_TYPE CYD_PANEL_ST7789
#define CYD_TOUCH_TYPE CYD_TOUCH_CST816S

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

#define UI_DISPLAY_SELF_TEST_MS 1200

// WizMote remotes broadcast across channels so WLED can receive them whether it
// is on Wi-Fi, AP mode, or a different channel. Set to 0 to use only the channel below.
#define WLED_WIZMOTE_SCAN_CHANNELS 1
#define WLED_ESPNOW_CHANNEL 1

#define UI_IDLE_BRIGHTNESS 72
#define UI_ACTIVE_BRIGHTNESS 255
#define UI_DIM_AFTER_MS 30000
