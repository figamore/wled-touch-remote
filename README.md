# WLED CYD ESP-NOW Remote

Elegant LVGL + LovyanGFX firmware for the capacitive Cheap Yellow Display ESP32-2432S028C. It gives you a touch remote for WLED's built-in ESP-NOW actions: power, brightness slider control, and presets 1-7.

## Configure

Edit `include/app_config.h` if needed:

- `CYD_PANEL_TYPE` / `CYD_TOUCH_TYPE`: default to the tested capacitive CYD profile, `ST7789 + CST816S`.
- `CYD_BACKLIGHT_INVERT`: flip this from `0` to `1` if serial boot works and the profile is right but the screen is black.
- `CYD_TFT_*` and `CYD_TOUCH_*`: display and touch pins for clone boards.
- `WLED_WIZMOTE_SCAN_CHANNELS`: broadcasts across channels 1 through 13 by default.

## WLED Pairing

WLED's built-in ESP-NOW support is WizMote-style button-code based. In WLED:

1. Open `Config -> WiFi Setup`.
2. Enable ESP-NOW remote control.
3. Flash and boot this display.
4. Tap a control.
5. Copy the `Last Seen` MAC into the `Hardware MAC` field and save.

Default WLED behavior supports power, brightness up/down, and presets 1-7. The brightness slider translates movement into native WLED brightness step packets.

The Info tab shows the ESP-NOW MAC address and includes Settings, where the display orientation can be flipped and the inactivity action can be set to dim or turn the display off. Settings are saved in ESP32 NVS and restored after reboot.

## Build and Upload

```sh
pio run -e esp32-cyd-capacitive
pio run -e esp32-cyd-capacitive -t upload
pio device monitor -b 115200
```

The startup logo is converted from `wled.png` into an RGB565 bitmap at build time. A display-sized PNG, around 320 pixels wide, avoids runtime scaling artifacts.

## Hardware Notes

The default display config targets the tested capacitive CYD layout:

- ST7789 display SPI: `SCLK=14`, `MOSI=13`, `MISO=12`, `CS=15`, `DC=2`, backlight `27`
- CST816S capacitive touch I2C: `SDA=33`, `SCL=32`, `RST=25`, address `0x15`

Some CYD sellers swap panels and touch controllers. If your board is the other common capacitive style, set `CYD_PANEL_TYPE` to `CYD_PANEL_ILI9341`, `CYD_TOUCH_TYPE` to `CYD_TOUCH_FT5X06`, `CYD_TFT_BL` to `21`, `CYD_TOUCH_INT` to `36`, `CYD_TOUCH_ADDR` to `0x38`, and `CYD_TOUCH_I2C_PORT` to `1`.

On every boot the firmware shows `wled.png` for two seconds before LVGL starts. If the serial log reaches `Display splash: WLED logo` but the panel stays black, change `CYD_BACKLIGHT_INVERT` first. If the backlight is on but there is no logo or fallback text, the LCD pinout or driver profile does not match your board.
