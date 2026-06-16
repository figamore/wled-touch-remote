# ESP-NOW WLED Protocol

This firmware defaults to WLED's native ESP-NOW remote protocol: WizMote-style button packets.

WLED setup:

1. In WLED, open `Config -> WiFi Setup`.
2. Enable ESP-NOW remote control.
3. Flash and boot this display.
4. Tap any control on the display.
5. Copy the display MAC from WLED's `Last Seen` field into WLED's `Hardware MAC` field.
6. Save.

The display prints its station MAC on boot, too:

```text
CYD station MAC: 88:57:21:2E:E9:D8
ESP-NOW protocol: WLED native WizMote
```

## Default WLED Buttons

These work with WLED's built-in WizMote behavior:

- `1`: on
- `2`: off
- `8`: brightness down
- `9`: brightness up
- `16` through `19`: presets 1 through 4

The display also sends stable custom button codes for richer UI controls. Add a `remote.json` file to WLED if you want those custom codes to run exact JSON commands.

## Packet Shape

The native packet is the same 13-byte shape used by WizMote-style remotes:

```cpp
struct WizMotePacket {
  uint8_t program;    // 0x91 for On, 0x81 for all other buttons
  uint8_t seq[4];     // little-endian sequence number
  uint8_t dt1;        // 0x20
  uint8_t button;     // button code
  uint8_t dt2;        // 0x01
  uint8_t batLevel;   // 90
  uint8_t byte10;
  uint8_t byte11;
  uint8_t byte12;
  uint8_t byte13;
};
```

The firmware broadcasts each button packet across channels 1 through 13 by default, matching the behavior of common WLED ESP-NOW remotes.

## JSON Bridge Mode

`include/app_config.h` still has a `WLED_PROTOCOL_JSON_BRIDGE` mode for a custom bridge or usermod that receives raw WLED JSON over ESP-NOW. Stock WLED ESP-NOW remote support does not use that JSON transport.
