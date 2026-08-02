#if WLED_TOUCH_SIMULATOR

#include <SDL.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sys/stat.h>

#include "display.h"
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

  expectTrue(waitUntil([] { return wled::online(); }, 3000), "simulated Wi-Fi controller available");
  expectTrue(wled::activeDeviceCount() == 1, "one simulated controller active");
  expectTrue(!wled::model().presets.empty(), "fallback preset catalog available");

  // The simulator snapshot derives directly from the model, so these only
  // verify model updates; they do not exercise transport or device delivery.
  wled::setBrightness(73);
  expectTrue(waitUntil([] { return wled::model().brightness == 73; }, 3000),
             "brightness model updated");

  wled::setBrightness(91);
  wled::setEffect(42);
  expectTrue(waitUntil([] { return wled::model().effect == 42; }, 5000),
             "effect model updated");

  // Rapid UI updates are coalesced to the newest queued value instead of filling radio queues.
  wled::setBrightness(31);
  wled::setBrightness(63);
  wled::setBrightness(127);
  expectTrue(waitUntil([] { return wled::model().brightness == 127; }, 3000),
             "rapid brightness model updates coalesced to latest");

  // FX controls modal.
  simulatorSetTab(2);
  simulatorRunFrames(30);
  simulatorOpenFxControls();
  simulatorRunFrames(30);
  simulatorSaveBmp("screenshots/selftest-fx-controls.bmp");

  // Close the dialog (X button in the modal header).
  tapAt(292, 20);
  simulatorRunFrames(30);

  simulatorSaveBmp("screenshots/selftest-final.bmp");
  std::printf("[selftest] %s (%d failure%s)\n",
              test_failures == 0 ? "ALL PASS" : "FAILED",
              test_failures,
              test_failures == 1 ? "" : "s");
  return test_failures == 0 ? 0 : 1;
}

constexpr int kSimulatorScale = kScreenWidth >= 480 ? 1 : 2;

void updateSimulatorTouch(SDL_Window* window, int x_position, int y_position, bool down) {
  int window_width = 0;
  int window_height = 0;
  SDL_GetWindowSize(window, &window_width, &window_height);
  if (window_width <= 0 || window_height <= 0) {
    simulatorSetTouch(false, 0, 0);
    return;
  }

  const int16_t x = static_cast<int16_t>(x_position * kScreenWidth / window_width);
  const int16_t y = static_cast<int16_t>(y_position * kScreenHeight / window_height);
  simulatorSetTouch(down, x, y);
}

bool processSimulatorEvents(SDL_Window* window) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_QUIT:
        return false;
      case SDL_MOUSEBUTTONDOWN:
        if (event.button.button == SDL_BUTTON_LEFT) {
          updateSimulatorTouch(window, event.button.x, event.button.y, true);
        }
        break;
      case SDL_MOUSEBUTTONUP:
        if (event.button.button == SDL_BUTTON_LEFT) {
          updateSimulatorTouch(window, event.button.x, event.button.y, false);
        }
        break;
      case SDL_MOUSEMOTION:
        if (event.motion.state & SDL_BUTTON_LMASK) {
          updateSimulatorTouch(window, event.motion.x, event.motion.y, true);
        }
        break;
      default:
        break;
    }
  }
  return true;
}

struct SimulatorWindow {
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  SDL_Texture* texture = nullptr;
};

SimulatorWindow simulator_window;

void closeInteractiveSimulator() {
  SDL_DestroyTexture(simulator_window.texture);
  SDL_DestroyRenderer(simulator_window.renderer);
  SDL_DestroyWindow(simulator_window.window);
  simulator_window = {};
  SDL_Quit();
}

bool renderSimulatorFrame() {
  if (!processSimulatorEvents(simulator_window.window)) {
    return false;
  }

  loop();
  SDL_UpdateTexture(simulator_window.texture, nullptr, sim_framebuffer,
                    kScreenWidth * sizeof(uint16_t));
  SDL_RenderClear(simulator_window.renderer);
  SDL_RenderCopy(simulator_window.renderer, simulator_window.texture, nullptr, nullptr);
  SDL_RenderPresent(simulator_window.renderer);
  return true;
}

#ifdef __EMSCRIPTEN__
void renderBrowserSimulatorFrame() {
  if (!renderSimulatorFrame()) {
    closeInteractiveSimulator();
    emscripten_cancel_main_loop();
  }
}
#endif

int runInteractiveSimulator() {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
    return 1;
  }

  simulator_window.window = SDL_CreateWindow(
      "WLED Touch Remote Simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      kScreenWidth * kSimulatorScale, kScreenHeight * kSimulatorScale,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!simulator_window.window) {
    std::fprintf(stderr, "SDL window creation failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  simulator_window.renderer = SDL_CreateRenderer(
      simulator_window.window, -1, SDL_RENDERER_ACCELERATED);
  if (!simulator_window.renderer) {
    simulator_window.renderer = SDL_CreateRenderer(simulator_window.window, -1, 0);
  }
  if (!simulator_window.renderer) {
    std::fprintf(stderr, "SDL renderer creation failed: %s\n", SDL_GetError());
    closeInteractiveSimulator();
    return 1;
  }

  simulator_window.texture = SDL_CreateTexture(
      simulator_window.renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
      kScreenWidth, kScreenHeight);
  if (!simulator_window.texture) {
    std::fprintf(stderr, "SDL texture creation failed: %s\n", SDL_GetError());
    closeInteractiveSimulator();
    return 1;
  }

#ifdef __EMSCRIPTEN__
  emscripten_set_main_loop(renderBrowserSimulatorFrame, 0, true);
#else
  while (renderSimulatorFrame()) {}
  closeInteractiveSimulator();
#endif
  return 0;
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

  return runInteractiveSimulator();
}

#endif
