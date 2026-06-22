#if WLED_TOUCH_SIMULATOR

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

extern void setup();
extern void loop();
extern void simulatorSetTab(uint8_t index);
extern void simulatorUseBasicViewState();
extern void simulatorRunFrames(uint16_t frames);
extern bool simulatorSaveBmp(const char* path);

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
  simulatorUseBasicViewState();
  simulatorRunFrames(20);

  for (const ScreenshotTab& tab : kScreenshotTabs) {
    char path[96];
    std::snprintf(path, sizeof(path), "screenshots/wled-touch-remote-%s.bmp", tab.name);
    simulatorSetTab(tab.index);
    simulatorRunFrames(30);
    simulatorSaveBmp(path);
  }
}

}  // namespace

int main(int argc, char** argv) {
  setup();

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
