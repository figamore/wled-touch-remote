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
- `16` through `22`: presets 1 through 7
- `100` through `103`: WIZ Smart Button duplicates for on, off, brightness up, and brightness down

The stock UI sends the non-duplicate native button codes WLED handles without a `/remote.json` file.

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
