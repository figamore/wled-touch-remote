#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Bidirectional WLED control over the ESP-NOW JSON API (see the WLED firmware doc
// docs/espnow-json-protocol.md). The remote sends JSON state commands and {"get":...} catalog
// requests, and receives {"state","info"} responses/pushes plus binary live LED peek frames.
// All model updates happen in loop() context, so UI code may read the model without locking.

namespace wled {

struct PresetInfo {
  uint8_t id;
  std::string name;
};

struct Model {
  bool online = false;
  bool power = false;
  uint8_t brightness = 128;
  uint8_t mainSegmentId = 0;
  int effect = -1;          // current segment effect id, -1 = unknown
  int palette = -1;
  int preset = -1;          // current preset id, -1 = none
  uint8_t speed = 128;
  uint8_t intensity = 128;
  uint8_t custom1 = 128;    // per-effect custom sliders (seg c1/c2/c3)
  uint8_t custom2 = 128;
  uint8_t custom3 = 16;
  uint32_t color = 0xFFFFFF; // primary colour of the main segment, 0xRRGGBB
  std::string name;          // WLED instance name (info.name)
  // Effect and palette catalogs are baked in (generated/wled_catalog.h); only presets,
  // which are per-instance, are fetched over the API.
  std::vector<PresetInfo> presets;
};

struct DeviceInfo {
  uint8_t mac[6];
  uint8_t channel;
  bool online;
  std::string name;
};

enum MixedState : uint16_t {
  kMixedNone = 0,
  kMixedPower = 1U << 0,
  kMixedBrightness = 1U << 1,
  kMixedEffect = 1U << 2,
  kMixedPalette = 1U << 3,
  kMixedPreset = 1U << 4,
  kMixedColor = 1U << 5,
};

void begin();
void loop(uint32_t now_ms);

const Model& model();
bool online();
uint8_t radioChannel(); // discovered WLED channel, or 0 while searching
size_t deviceCount();
size_t activeDeviceCount();
DeviceInfo deviceInfo(size_t index);
size_t focusedDevice();
bool targetingAll();
uint16_t mixedStateMask();
void selectDevice(size_t index);
void selectAll();
void renameDevice(size_t index, const char* name);
bool forgetDevice(size_t index);
void scanNow();

// Monotonic counters; UI compares against its last-seen value to know when to refresh.
uint32_t stateRevision();
uint32_t catalogRevision();
uint32_t liveRevision();
uint32_t deviceRevision();

// Latest live peek frame as RGB triples. Returns nullptr until a frame arrives.
const uint8_t* liveLeds(uint16_t& ledCount, uint16_t& width, uint16_t& height);
bool livePeekEnabled();
uint32_t liveFrameAgeMs(uint32_t now_ms);

// Commands.
void poll();
void requestCatalogs();
void setPower(bool on);
void togglePower();
void setBrightness(uint8_t bri);
void applyPreset(uint8_t id);
void savePreset(uint8_t id, const char* name);
void setEffect(uint8_t fxId);
void setPalette(uint8_t palId);
void setColor(uint8_t r, uint8_t g, uint8_t b);
void setEffectParams(int speed, int intensity); // -1 leaves a field unchanged
void setCustomParam(uint8_t index, uint8_t value); // seg c1/c2/c3 (index 1-3)
void sendRaw(const char* json);
void setLivePeek(bool on);

}  // namespace wled
