# WLED Touch Remote

WLED Touch Remote is a dedicated touchscreen controller for WLED over your local Wi-Fi network. It runs on the ESP32 Cheap Yellow Display (CYD) or the Guition ESP32-P4 display and controls power, brightness, presets, colors, effects, palettes, and live LED preview through WLED's standard JSON API.

It works with current WLED firmware and requires no custom WLED build or controller-side configuration. The remote can run from 5 V or, on supported capacitive displays, from a Li-Ion battery cell. See the [bill of materials](#bill-of-materials).

> ## [▶ Try the interactive simulator in your browser for a preview](https://figamore.github.io/wled-touch-remote/simulator/jc4880p443.html)

## Contents

- [Support the project](#support-the-project)
- [Features](#features)
- [Screenshots](#screenshots)
  - [ESP32-CYD](#esp32-cyd)
  - [ESP32-P4](#esp32-p4)
- [Flash it](#flash-it)
- [Set up Wi-Fi and WLED](#set-up-wi-fi-and-wled)
- [Settings](#settings)
- [Supported hardware](#supported-hardware)
- [Bill of materials](#bill-of-materials)
- [3D-printed case](#3d-printed-case)
- [Development](#development)
- [ESP-NOW (deprecated)](#esp-now-deprecated)
- [Contributing](#contributing)

## Support the project

If you enjoy WLED Touch Remote, please consider [sponsoring its development](https://github.com/sponsors/figamore). It has taken many hours of development and testing to build, and donations help fund ongoing improvements, new features, and wider hardware support.

<p align="center">
  <img src="screenshots/case/esp32-cyd-and-esp32-p4.jpg" alt="ESP32-CYD and ESP32-P4 displays" width="720" />
</p>

## Features

- Power and brightness control
- Preset selection, creation, and renaming
- Colors, effects, palettes, speed, and intensity controls
- Live LED preview and responsive state updates from the selected controller
- Automatic discovery of WLED controllers on the local network, with manual IP-address entry when discovery is unavailable
- Optional built-in mobile hotspot for WLED setups without an existing Wi-Fi network
- Control of one or all discovered controllers
- On-device settings for screen orientation and inactivity behavior
- Web installer support for browser-based flashing

<p align="center">
  <img src="screenshots/esp32-cyd/esp32-cyd-fx.png" alt="Effects" width="620" />
</p>

## Screenshots

### ESP32-CYD

<p align="center">
  <img src="screenshots/esp32-cyd/esp32-cyd-maintab.png" alt="ESP32-CYD main control screen" width="32%" />
  <img src="screenshots/esp32-cyd/esp32-cyd-presets.png" alt="ESP32-CYD presets screen" width="32%" />
  <img src="screenshots/esp32-cyd/esp32-cyd-colorwheel.png" alt="ESP32-CYD color wheel" width="32%" />
</p>
<p align="center">
  <img src="screenshots/esp32-cyd/esp32-cyd-fx.png" alt="ESP32-CYD effects screen" width="32%" />
  <img src="screenshots/esp32-cyd/esp32-cyd-palettes.png" alt="ESP32-CYD palettes screen" width="32%" />
  <img src="screenshots/esp32-cyd/esp32-cyd-settigs.png" alt="ESP32-CYD settings screen" width="32%" />
</p>

### ESP32-P4

<p align="center">
  <img src="screenshots/esp32-p4/esp32-p4-settings.png" alt="ESP32-P4 settings screen" width="23%" />
  <img src="screenshots/esp32-p4/esp32-p4-presets.png" alt="ESP32-P4 presets screen" width="23%" />
  <img src="screenshots/esp32-p4/esp32-p4-colorwheel.png" alt="ESP32-P4 color wheel" width="23%" />
  <img src="screenshots/esp32-p4/esp32-p4-fx.png" alt="ESP32-P4 effects screen" width="23%" />
</p>
<p align="center">
  <img src="screenshots/esp32-p4/esp32-p4-wifiscan.png" alt="ESP32-P4 Wi-Fi network scan" width="30%" />
  <img src="screenshots/esp32-p4/esp32-p4-palettes.png" alt="ESP32-P4 palettes screen" width="30%" />
  <img src="screenshots/esp32-p4/esp32-p4-keyboard.png" alt="ESP32-P4 on-screen keyboard" width="30%" />
</p>

## Flash it

The easiest option is the [web installer](https://figamore.github.io/wled-touch-remote/):

1. Open the [web installer](https://figamore.github.io/wled-touch-remote/).
2. Connect your display using a data-capable USB cable.
3. Click **Install** and choose the ESP32 serial port.

## Set up Wi-Fi and WLED

1. On the remote, open **Settings → Wi-Fi**, choose your 2.4 GHz network, and enter its password.
2. The remote finds WLED instances advertised on the LAN. Open **Settings → Control Target** to choose one or control all discovered controllers.
3. If a controller is not discovered, choose **WLED IP** from the Wi-Fi screen and enter its IPv4 address.

The remote and WLED controller must be on the same routed local network.

### Mobile setups without Wi-Fi

For a portable installation, open **Settings → Mobile hotspot**, enable it, and optionally edit the displayed SSID and password.

Configure each WLED controller to join that network, then select it from **Control Target** (or enter its address with **WLED IP** if discovery is unavailable). Enabling the hotspot disconnects the remote from its saved Wi-Fi; disabling it stops the hotspot and reconnects to the saved network.

## Settings

The Settings tab lets you change:

- The controller or controllers to control
- Wi-Fi network
- Mobile hotspot name and on/off state
- Display orientation
- Inactivity behavior:
   * Always on
   * Dim
   * Display off
   * Eco: display stays on while the WLED instance is on, then turns off after 30 seconds when the WLED instance is off.
- Software updates from stable GitHub Releases

Settings are saved on the ESP32 and restored after reboot.

### Software updates

Open **Settings → Software Update → Check for Updates** to compare the installed semantic version with the newest compatible stable GitHub Release. Drafts and prereleases are ignored. The remote only accepts the application firmware asset for its own board (`esp32-cyd` or `jc4880p443`), downloads it, and verifies the GitHub-provided SHA-256 digest before committing it to the OTA partition.

Keep the remote powered and connected to Wi-Fi during installation. A download, validation, or install failure aborts the pending OTA image and leaves the currently running firmware intact.

<p align="center">
  <img src="screenshots/esp32-p4/esp32-p4-settings.png" alt="Settings" width="620" />
</p>

## Supported hardware

Supported devices:

- **Guition ESP32-P4 JC4880P443, 4.3-inch display** - highly recommended
- **Guition JC2432W328C** - recommended capacitive CYD
- **ESP32-024 and ESP32-2432S028-style resistive CYDs** - largely supported but **not recommended**

On first boot, the firmware shows a one-time touch setup screen to confirm the touch hardware.

## Bill of materials

### Cheap Yellow Display

CYDs come in capacitive and resistive versions. The capacitive version is preferred for its more responsive touch, slightly nicer display, and wider support. Only capacitive CYDs support Li-Ion batteries. Check for the `BAT` connector before buying because several variants exist.

Most resistive CYDs are also supported, but they are not recommended. There are too many resistive variants to guarantee support for all of them.

Search for `JC2432W328C` or `Capacitive CYD` on AliExpress or Amazon. Guition is the recommended brand for capacitive displays. Avoid its resistive displays because they are unlikely to be supported.

Some currently available capacitive CYDs:

- <https://www.amazon.com/DIYmalls-Touchscreen-ESP-WROOM-32-Development-JC2432W328C>
- <https://www.aliexpress.us/item/3256806545687380.html>

<p align="center">
  <img src="screenshots/capacitive-cyd.png" alt="Capacitive CYD" width="480" />
</p>

If you can source the ESP32-P4, it will provide a nicer experience due to its larger and more powerful display:
- <https://www.aliexpress.us/item/3256809431944589.html>

### Fasteners for the 3D-printed case

- **Slim version:** four M3×10 bolts
- **Battery version:** four M3×20 bolts

### Mating connectors (optional)

- CYDs usually include a JST-PB 1.25 mm four-pin connector, which you can use to add buttons.
- For battery operation, use a JST-PB 1.25 mm two-pin connector. If necessary, carefully split a four-pin connector in half.

## 3D-printed case

Optional snap-fit cases are available on MakerWorld: [FigCYD CYD case with optional battery](https://makerworld.com/en/models/2964422-figcyd-cyd-case-with-optional-battery).

An ESP32-P4 case is also available on MakerWorld: [WLED Touch Remote based on ESP32-P4](https://makerworld.com/en/models/3124737-wled-touch-remote-based-on-esp32-p4).

<p align="center">
  <img src="screenshots/case/ESP32-p4-case.jpg" alt="ESP32-P4 case" width="620" />
</p>

Choose one of the two case styles:

- **Slim case:** A clean remote without an internal battery, suited to a wall or control cabinet with continuous USB-C or 5 V power.

  <p align="center">
    <img src="screenshots/case/FigCyd-Standard.jpg" alt="Slim case" width="620" />
  </p>

- **Battery case:** A portable build with an 18650 Li-Ion cell and holder.

  <p align="center">
    <img src="screenshots/case/FigCyd-Battery-1.jpg" alt="Battery case" width="620" />
  </p>

Assembly:

1. Print the case parts from MakerWorld for the slim or battery version.
2. Press the CYD into the front shell, checking that the USB-C port, reset button, and side button line up.
3. Snap the back shell into place. Bolts are optional because the case is snap-fit, but they make it more secure.
   - **Slim:** four M3×10 bolts
   - **Battery:** four M3×20 bolts

For the slim case, you can stop here. For the battery case:

1. Remove the printed supports from the button opening, bolt holes, and battery-holder area.
2. Free the side power-button piece and make sure it moves smoothly before installing the CYD.
3. Install the 18650 holder and route the wires so they do not pinch when the case closes.

<p align="center">
  <img src="screenshots/case/Remove-button-support.jpg" alt="Case supports" width="48%" />
  <img src="screenshots/case/FigCyd-Battery-Internal.jpg" alt="Battery case interior" width="48%" />
</p>

The slim case can be powered through USB-C or through the board's `GND` and `5V` connector.

<p align="center">
  <img src="screenshots/case/FigCyd-Internal.jpg" alt="Slim case power wiring" width="620" />
</p>

### Battery operation

- Double-tap the power button to turn on.
- Hold the power button for 10 seconds to turn off.
- If you connect USB while running on battery, the device may restart. This is normal behavior for the CYD battery circuitry.
- Double-check polarity before powering the board. The case photos show the intended wiring path and board orientation.

# Development

## Build locally

Install PlatformIO, then run:

```sh
pio run -e esp32-cyd
pio run -e esp32-cyd -t upload
```

For the JC4880P443, use `pio run -e jc4880p443`. See `platformio.ini` for all available environments and `include/app_config.h` for board-specific options.

## macOS simulator

A native SDL simulator is included for screenshots and UI checks:

```sh
brew install sdl2

# 320x240 ESP32-CYD
pio run -e macos
.pio/build/macos/program

# 480x800 ESP32-P4 JC4880P443
pio run -e macos-jc4880p443
.pio/build/macos-jc4880p443/program
```

Each command opens a resizable SDL window. Click or drag in the window to
simulate touch input; close the window to exit.

## ESP-NOW (deprecated)

ESP-NOW support is deprecated in favor of standard Wi-Fi. Use Wi-Fi for new installations.

## Contributing

Issues and pull requests are welcome. Keep changes focused and touch-friendly on both the 320×240 CYD and the Guition ESP32-P4 JC4880P443 4.3-inch display.

Useful areas for contributions include:

- UI polish
- Documentation
- Hardware compatibility reports for CYD variants
- New features

See [Acknowledgements](ACKNOWLEDGEMENTS.md) for related projects and libraries that helped shape this project.
