#if WLED_TOUCH_SIMULATOR

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sys/stat.h>

#include "sim_wled.h"
#include "wled_api.h"

extern void setup();
extern void loop();
extern void simulatorSetTab(uint8_t index);
extern void simulatorUseDefaultViewState();
extern void simulatorRunFrames(uint16_t frames);
extern bool simulatorSaveBmp(const char* path);
extern void simulatorSetTouch(bool down, int16_t x, int16_t y);
extern void simulatorOpenFxControls();
extern void simulatorOpenPaletteChooser();

void initBatteryMonitor() {}

bool batteryAvailable() {
  return true;
}

int batteryAdcMillivolts() {
  return 3900;
}

int batteryMillivolts() {
  return 3900;
}

int batteryLevel() {
  return 76;
}

bool batteryCharging() {
  return false;
}

namespace {

constexpr uint8_t kScreenshotTabCount = 5;

struct ScreenshotTab {
  uint8_t index;
  const char* name;
};

constexpr ScreenshotTab kScreenshotTabs[kScreenshotTabCount] = {
    {0, "power"},
    {1, "presets"},
    {2, "fx"},
    {3, "info"},
    {4, "settings"},
};

bool hasArg(int argc, char** argv, const char* expected) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], expected) == 0) {
      return true;
    }
  }
  return false;
}

void saveScreenshots() {
  mkdir("screenshots", 0755);
  simulatorUseDefaultViewState();
  simulatorRunFrames(20);

  for (const ScreenshotTab& tab : kScreenshotTabs) {
    char path[96];
    std::snprintf(path, sizeof(path), "screenshots/wled-touch-remote-%s.bmp", tab.name);
    simulatorSetTab(tab.index);
    simulatorRunFrames(30);
    simulatorSaveBmp(path);
  }

  // Colors tab scrolled down to the embedded palette browser.
  simulatorSetTab(3);
  simulatorRunFrames(30);
  for (int pass = 0; pass < 3; ++pass) {
    simulatorSetTouch(true, 300, 200);
    simulatorRunFrames(4);
    for (int i = 1; i <= 8; ++i) {
      simulatorSetTouch(true, 300, 200 - i * 17);
      simulatorRunFrames(3);
    }
    simulatorSetTouch(false, 300, 64);
    simulatorRunFrames(20);
  }
  simulatorSaveBmp("screenshots/wled-touch-remote-palettes.bmp");
}

// ── Self test ────────────────────────────────────────────────────────────────
// Drives the whole stack against the fake WLED: discovery, catalog fetch,
// palette-chooser stress scrolling (the historical freeze), command round-trips
// and web-UI push syncing. Run with: program --selftest (use `timeout`: a hang
// or abort() means a regression).

int test_failures = 0;

void expectTrue(bool ok, const char* what) {
  std::printf("[selftest] %-46s %s\n", what, ok ? "PASS" : "FAIL");
  if (!ok) {
    ++test_failures;
  }
}

bool waitUntil(const std::function<bool()>& condition, uint32_t max_frames) {
  for (uint32_t i = 0; i < max_frames; ++i) {
    loop();
    if (condition()) {
      return true;
    }
  }
  return condition();
}

void tapAt(int16_t x, int16_t y) {
  simulatorSetTouch(true, x, y);
  simulatorRunFrames(8);
  simulatorSetTouch(false, x, y);
  simulatorRunFrames(12);
}

void dragVertical(int16_t x, int16_t y_from, int16_t y_to, uint16_t steps) {
  simulatorSetTouch(true, x, y_from);
  simulatorRunFrames(4);
  for (uint16_t i = 1; i <= steps; ++i) {
    const int16_t y = y_from + (int32_t(y_to - y_from) * i) / steps;
    simulatorSetTouch(true, x, y);
    simulatorRunFrames(3);
  }
  simulatorSetTouch(false, x, y_to);
  simulatorRunFrames(20);
}

int runSelfTest() {
  mkdir("screenshots", 0755);
  simulatorUseDefaultViewState();

  expectTrue(waitUntil([] { return wled::online(); }, 3000), "WLED discovered via HELLO");
  // effect/palette catalogs are baked into the firmware; only presets arrive over the API
  expectTrue(waitUntil([] { return !wled::model().presets.empty(); }, 12000),
             "preset catalog loaded");

  const wled::Model& m = wled::model();
  expectTrue(m.brightness == simWledSnapshot().bri, "boot state pull: brightness");
  expectTrue(m.color == simWledSnapshot().color, "boot state pull: color");

  // Lose a direct response after WLED applies the request. The remote must retry the same
  // idempotent command, receive its matching response, then release the next queued command.
  simWledDropNextResponse();
  wled::setBrightness(91);
  wled::setEffect(42);
  expectTrue(waitUntil([] { return simWledSnapshot().fx == 42; }, 5000),
             "lost response retried; queued command released");

  // Rapid UI updates are coalesced to the newest queued value instead of filling radio queues.
  wled::setBrightness(31);
  wled::setBrightness(63);
  wled::setBrightness(127);
  expectTrue(waitUntil([] { return simWledSnapshot().bri == 127; }, 3000),
             "rapid brightness updates coalesced to latest");

  // FX controls modal + palette chooser (the freeze reproduction path).
  simulatorSetTab(2);
  simulatorRunFrames(30);
  simulatorOpenFxControls();
  simulatorRunFrames(30);
  simulatorSaveBmp("screenshots/selftest-fx-controls.bmp");
  simulatorOpenPaletteChooser();
  simulatorRunFrames(30);
  simulatorSaveBmp("screenshots/selftest-palette-chooser.bmp");

  // Stress: aggressive scrolling with flings in both directions.
  for (int i = 0; i < 10; ++i) {
    dragVertical(160, 200, 60, 6);
    dragVertical(160, 200, 55, 2);  // fast fling
  }
  for (int i = 0; i < 10; ++i) {
    dragVertical(160, 60, 200, 6);
    dragVertical(160, 55, 200, 2);
  }
  expectTrue(true, "palette list scroll stress survived");
  simulatorSaveBmp("screenshots/selftest-palette-scrolled.bmp");

  // Select a palette row and confirm the command reached WLED.
  const uint8_t palette_before = simWledSnapshot().pal;
  tapAt(160, 110);
  expectTrue(waitUntil([palette_before] { return simWledSnapshot().pal != palette_before; }, 600),
             "palette tap sent to WLED");
  expectTrue(waitUntil([] { return wled::model().palette == int(simWledSnapshot().pal); }, 600),
             "palette selection echoed to model");

  // Close the dialog (X button in the modal header).
  tapAt(292, 20);
  simulatorRunFrames(30);

  // Web-UI-style external changes must be pushed and applied promptly.
  simWledExternalChange(0);
  expectTrue(waitUntil([] { return wled::model().color == 0x28DC78; }, 600),
             "external color change pushed to display");
  simWledExternalChange(1);
  expectTrue(waitUntil([] { return wled::model().effect == 27; }, 600),
             "external effect change pushed to display");
  simWledExternalChange(2);
  expectTrue(waitUntil([] { return wled::model().brightness == 60; }, 600),
             "external brightness change pushed to display");

  simulatorSaveBmp("screenshots/selftest-final.bmp");
  std::printf("[selftest] %s (%d failure%s)\n",
              test_failures == 0 ? "ALL PASS" : "FAILED",
              test_failures,
              test_failures == 1 ? "" : "s");
  return test_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  setup();
  simulatorUseDefaultViewState();

  if (hasArg(argc, argv, "--selftest")) {
    return runSelfTest();
  }

  if (hasArg(argc, argv, "--screenshots")) {
    saveScreenshots();
    return 0;
  }

  while (true) {
    loop();
  }

  return 0;
}

#endif
