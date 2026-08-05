#include "wled_api.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "app_state.h"
#include "wifi_link.h"

#if !WLED_TOUCH_SIMULATOR
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_idf_version.h>
#if ESP_IDF_VERSION_MAJOR >= 5
#include <esp_err.h>
#include <esp_netif.h>
#endif
#include <mdns.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#endif

namespace wled {
namespace {

constexpr size_t kMaxDevices = 6;
constexpr size_t kMaxDeviceAliasLength = 32;
constexpr size_t kMaxRequestLength = 192;
constexpr size_t kTransactionQueueSize = 8;
constexpr uint16_t kWledPort = 80;
// WLED's ESP32 WebSocket Peek stream can contain 1,024 LEDs at 25 fps.  The
// remote header renders a 64-pixel strip, so retain only those evenly spaced
// samples and never let socket draining monopolize the UI loop.
constexpr size_t kLivePreviewSamples = 64;
#if !WLED_TOUCH_SIMULATOR && WLED_BOARD == WLED_BOARD_JC4880P443
// The P4 renders a full 480x800 screen; a tab switch blocks loop() for over
// 100 ms while a 900+ LED Peek stream keeps arriving at ~100 KB/s.  The
// buffer must absorb that whole redraw without filling, or the TCP window
// closes and the stream stalls for retransmit round trips.  The larger drain
// budget clears the backlog within a few loop passes afterwards.
constexpr size_t kSocketReadBudgetBytes = 8192;
constexpr size_t kSocketRxCapacity = 16384;
constexpr uint8_t kMaxSocketFramesPerLoop = 4;
#else
constexpr size_t kSocketReadBudgetBytes = 4096;
constexpr size_t kSocketRxCapacity = 8192;
constexpr uint8_t kMaxSocketFramesPerLoop = 2;
#endif
// Keep mDNS discovery nearly continuous.  This mirrors the native WLED app's
// Bonjour browser: a 4 s multicast query followed by a 1 s pause means a
// controller that joins the network or misses one packet is found promptly.
constexpr uint32_t kDiscoverIntervalMs = 5000;
constexpr uint32_t kDiscoverQueryTimeoutMs = 4000;
constexpr uint32_t kMdnsHostResolveTimeoutMs = 1500;
constexpr uint32_t kReconnectIntervalMs = 3000;
constexpr uint32_t kNoDevicesFoundDelayMs = kDiscoverQueryTimeoutMs + 1500;
// A controller is on the local LAN; a failed TCP connect must not stall the
// display loop for three quarters of a second.  The next retry is scheduled
// normally, so a short failure here does not make recovery brittle.
constexpr uint32_t kSocketConnectTimeoutMs = 125;
constexpr uint32_t kStatePollIntervalMs = 30000;
// WLED owns only one Peek WebSocket client and silently skips clients whose
// socket cannot drain, so a short frame gap is ambiguous: congestion on a
// weak link looks identical to another viewer taking the slot — except that
// congestion resumes by itself and a takeover never does.  A gap past the
// first threshold only marks the stream stalled; the takeover verdict (and
// its log line) waits until the silence has lasted the confirm window.
constexpr uint32_t kLiveRecoverySilenceMs = 1250;
constexpr uint32_t kLiveTakeoverConfirmMs = 8000;
// Once Peek goes quiet, ask for a normal state frame. A reply proves the
// WebSocket is healthy and another viewer owns Peek; no reply identifies the
// stalled TCP receive path reported on ESP-Hosted P4/C6 boards.
constexpr uint32_t kLiveSocketProbeTimeoutMs = 2500;
// A fresh subscription is allowed extra time before the silence is treated as
// a delivery problem: WLED batches the first frame behind its render loop and
// a weak link may need TCP retransmits.
constexpr uint32_t kLiveFirstFrameGraceMs = 2500;
// A subscription that never delivered is a link or load problem, not a Peek
// takeover, so it retries on its own: 2 s doubling to 32 s, then rests until
// the user interacts.  Touch is reserved for reclaiming a taken-over stream.
constexpr uint32_t kLiveRetryBaseMs = 2000;
constexpr uint8_t kLiveRetryMaxAttempts = 5;
// While the display idles dimmed, the bar needs freshness rather than motion:
// the stream is parked on a cached frame, and each resample (a state change
// pushed from any client, a command, or the periodic poll) keeps streaming
// only this long before parking again.
constexpr uint32_t kLiveIdleSampleMs = 2000;
constexpr uint32_t kDeviceOfflineMs = 20000;
constexpr uint32_t kCommandRetryMs = 750;
constexpr uint8_t kMaxCommandAttempts = 3;
// A state frame already in the WebSocket receive buffer can describe the
// preset from just before a local tap. Keep that frame from undoing the
// optimistic selection while WLED broadcasts the command's state update.
constexpr uint32_t kPresetEchoGraceMs = 2000;

struct DeviceSlot {
  uint8_t id[6] = {};
  uint32_t ipv4 = 0;
  uint32_t lastSeen = 0;
  char alias[kMaxDeviceAliasLength + 1] = {};
  Model model;
  uint8_t pendingPreset = 0;
  uint32_t pendingPresetSince = 0;
};

enum class RequestKey : uint8_t {
  kPoll, kCatalog, kPower, kBrightness, kPreset, kPresetSave, kEffect,
  kPalette, kColor, kEffectParams, kCustom1, kCustom2, kCustom3, kLive, kRaw,
};

struct Transaction {
  char json[kMaxRequestLength] = {};
  RequestKey key = RequestKey::kRaw;
  uint32_t networkGeneration = 0;
  bool targetMainSegment = false;
  uint8_t targets = 0;
  uint8_t attempts[kMaxDevices] = {};
  uint32_t retryAt[kMaxDevices] = {};
};

Model g_model;
DeviceSlot g_devices[kMaxDevices];
size_t g_deviceCount = 0;
size_t g_focusDevice = 0;
bool g_targetAll = false;
bool g_liveWanted = false;
bool g_presetsLoaded = false;
uint8_t g_live[kLivePreviewSamples * 3] = {};
uint16_t g_liveCount = 0;
uint16_t g_liveW = 0;
uint16_t g_liveH = 0;
uint32_t g_lastLiveRx = 0;
bool g_liveParked = false;
bool g_liveIdle = false;
uint32_t g_stateRev = 0;
uint32_t g_catalogRev = 0;
uint32_t g_liveRev = 0;
uint32_t g_deviceRev = 0;
ConnectionStatus g_connectionStatus = ConnectionStatus::kNoConnection;
uint32_t g_connectionRev = 0;
uint32_t g_discoveryStartedAt = 0;
uint32_t g_connectionLostAt = 0;
bool g_wifiWasConnected = false;
// AP and STA are mutually exclusive, but both count as a usable local link.
// Keep the active SSID separately so switching between them starts a fresh
// discovery epoch even though wifilink::connected() never went false.
std::string g_activeNetworkSsid;
bool g_activeNetworkIsAccessPoint = false;
// Every queued command/probe is tagged with this value. Work from an earlier
// local network may complete after a mode change, but can never affect it.
volatile uint32_t g_networkGeneration = 1;
bool g_hadWledConnection = false;
std::string g_wledConnectionSsid;
uint32_t g_lastPoll = 0;
uint32_t g_lastDiscover = 0;
bool g_scanRequested = true;
bool g_fullScanRequested = false;
uint32_t g_nextAccessPointClientProbeAt = 0;
size_t g_nextAccessPointClientProbe = 0;
uint32_t g_lastAccessPointProbeFailure = 0;
uint32_t g_lastAccessPointProbeFailureAt = 0;
uint32_t g_nextPresetFetch = 0;
uint8_t g_presetFetchAttempts = 0;
// Bumped whenever the cached preset catalog is invalidated (preset saved,
// explicit refresh) so a fetch already in flight cannot mark the forced
// refetch as satisfied with data read before the invalidation.
uint32_t g_presetFetchToken = 0;
Transaction g_transactions[kTransactionQueueSize];
uint8_t g_transactionRead = 0;
uint8_t g_transactionWrite = 0;
uint8_t g_transactionCount = 0;

#if !WLED_TOUCH_SIMULATOR
struct HttpCommandJob { uint32_t ipv4; uint32_t networkGeneration; char payload[kMaxRequestLength]; };
struct HttpCommandResult { uint32_t networkGeneration; bool success; };
QueueHandle_t g_httpCommandJobs = nullptr;
QueueHandle_t g_httpCommandResults = nullptr;
bool g_httpCommandInFlight = false;
uint32_t g_httpCommandGeneration = 0;
uint8_t g_httpCommandTarget = 0;
char g_httpCommandPayload[kMaxRequestLength] = {};
struct AccessPointProbeJob { uint32_t ipv4; uint32_t networkGeneration; };
struct AccessPointProbeResult {
  uint32_t ipv4;
  uint32_t networkGeneration;
  bool success;
  char name[65];
};
QueueHandle_t g_accessPointProbeJobs = nullptr;
QueueHandle_t g_accessPointProbeResults = nullptr;
bool g_accessPointProbeInFlight = false;
uint32_t g_accessPointProbeAddress = 0;
uint32_t g_accessPointProbeGeneration = 0;
#endif

void deviceAliasKey(const uint8_t* id, char* key, size_t keySize) {
  if (!id || !key || keySize < 14) return;
  snprintf(key, keySize, "n%02x%02x%02x%02x%02x%02x",
           id[0], id[1], id[2], id[3], id[4], id[5]);
}

void loadDeviceAlias(DeviceSlot& device) {
  char key[14] = {};
  deviceAliasKey(device.id, key, sizeof(key));
  Preferences prefs;
  if (!key[0] || !prefs.begin(kPrefsNamespace, true)) return;
  if (prefs.isKey(key)) prefs.getString(key, device.alias, sizeof(device.alias));
  prefs.end();
}

void saveDeviceAlias(const DeviceSlot& device) {
  char key[14] = {};
  deviceAliasKey(device.id, key, sizeof(key));
  Preferences prefs;
  if (!key[0] || !prefs.begin(kPrefsNamespace, false)) return;
  if (device.alias[0]) prefs.putString(key, device.alias);
  else prefs.remove(key);
  prefs.end();
}

void saveRegistry() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) return;
  prefs.putUChar("wifiReg", 1);
  prefs.putUChar("wifiCnt", uint8_t(g_deviceCount));
  prefs.putUChar("wifiFocus", uint8_t(g_focusDevice));
  prefs.putBool("wifiAll", g_targetAll);
  for (size_t i = 0; i < g_deviceCount; ++i) {
    char addressKey[8];
    snprintf(addressKey, sizeof(addressKey), "wifi%u", unsigned(i));
    prefs.putUInt(addressKey, g_devices[i].ipv4);
#if !WLED_TOUCH_SIMULATOR
    char identityKey[8];
    snprintf(identityKey, sizeof(identityKey), "wifiM%u", unsigned(i));
    prefs.putBytes(identityKey, g_devices[i].id, sizeof(g_devices[i].id));
#endif
  }
  prefs.end();
}

void makeAddressId(uint32_t ipv4, uint8_t* id) {
  id[0] = 0x02;
  id[1] = 0x57;
  memcpy(id + 2, &ipv4, sizeof(ipv4));
}

int deviceIndexForAddress(uint32_t ipv4) {
  for (size_t i = 0; i < g_deviceCount; ++i) {
    if (g_devices[i].ipv4 == ipv4) return int(i);
  }
  return -1;
}

int deviceIndexForId(const uint8_t* id) {
  if (!id) return -1;
  for (size_t i = 0; i < g_deviceCount; ++i) {
    if (!memcmp(g_devices[i].id, id, sizeof(g_devices[i].id))) return int(i);
  }
  return -1;
}

void loadSimulatorPresets(DeviceSlot& device) {
  if (!device.model.presets.empty()) return;
  for (uint8_t id = 1; id <= kPresetSlotCount; ++id) {
    device.model.presets.push_back({id, std::string("Preset ") + std::to_string(id)});
  }
  g_presetsLoaded = true;
  g_catalogRev++;
}

DeviceSlot* rememberDevice(uint32_t ipv4, const char* name, uint32_t now,
                           const uint8_t* id = nullptr, bool nameIsFallback = false) {
  if (!ipv4) return nullptr;
  int index = deviceIndexForId(id);
  if (index < 0) index = deviceIndexForAddress(ipv4);
  if (index < 0) {
    if (g_deviceCount >= kMaxDevices) {
      Serial.printf("[WIFI] controller list full; ignoring %lu\n", static_cast<unsigned long>(ipv4));
      return nullptr;
    }
    index = int(g_deviceCount++);
    if (id) memcpy(g_devices[index].id, id, sizeof(g_devices[index].id));
    else makeAddressId(ipv4, g_devices[index].id);
    loadDeviceAlias(g_devices[index]);
    g_deviceRev++;
  }
  DeviceSlot& device = g_devices[index];
  if (id && memcmp(device.id, id, sizeof(device.id))) {
    // Older registry entries used their address as an identity.  Upgrade them
    // in place when WLED's Bonjour TXT record supplies its stable Wi-Fi MAC.
    memcpy(device.id, id, sizeof(device.id));
    if (!device.alias[0]) loadDeviceAlias(device);
  }
  device.ipv4 = ipv4;
  device.lastSeen = now;
  // A fallback name (the mDNS hostname) must never displace the friendly name
  // reported by /json info, or the two sources take turns renaming the device.
  if (name && *name && device.model.name != name &&
      (!nameIsFallback || device.model.name.empty())) {
    device.model.name = name;
    g_deviceRev++;
  }
  return &device;
}

uint32_t colorFromArray(JsonArrayConst color, uint32_t fallback) {
  if (color.isNull() || color.size() < 3) return fallback;
  return (uint32_t(color[0] | 0) << 16) | (uint32_t(color[1] | 0) << 8) | uint32_t(color[2] | 0);
}

void applyState(Model& model, JsonObjectConst state, JsonObjectConst info,
                bool preservePreset = false) {
  if (!state.isNull()) {
    if (state["on"].is<bool>()) model.power = state["on"].as<bool>();
    if (state["bri"].is<int>()) model.brightness = state["bri"].as<uint8_t>();
    if (!preservePreset && state["ps"].is<int>()) model.preset = state["ps"].as<int>();
    const int main = state["mainseg"].is<int>() ? state["mainseg"].as<int>() : model.mainSegmentId;
    JsonArrayConst segments = state["seg"].as<JsonArrayConst>();
    JsonObjectConst segment;
    for (JsonObjectConst candidate : segments) {
      if (candidate["id"].is<int>() && candidate["id"].as<int>() == main) { segment = candidate; break; }
    }
    if (segment.isNull() && !segments.isNull() && segments.size()) segment = segments[0].as<JsonObjectConst>();
    if (!segment.isNull()) {
      if (segment["id"].is<int>()) model.mainSegmentId = segment["id"].as<uint8_t>();
      if (segment["fx"].is<int>()) model.effect = segment["fx"].as<int>();
      if (segment["pal"].is<int>()) model.palette = segment["pal"].as<int>();
      if (segment["sx"].is<int>()) model.speed = segment["sx"].as<uint8_t>();
      if (segment["ix"].is<int>()) model.intensity = segment["ix"].as<uint8_t>();
      if (segment["c1"].is<int>()) model.custom1 = segment["c1"].as<uint8_t>();
      if (segment["c2"].is<int>()) model.custom2 = segment["c2"].as<uint8_t>();
      if (segment["c3"].is<int>()) model.custom3 = segment["c3"].as<uint8_t>();
      JsonArrayConst colors = segment["col"].as<JsonArrayConst>();
      if (!colors.isNull() && colors.size()) model.color = colorFromArray(colors[0].as<JsonArrayConst>(), model.color);
    }
  }
  if (!info.isNull() && info["name"].is<const char*>()) model.name = info["name"].as<const char*>();
  model.online = true;
}

bool targetMainSegment(const char* json, uint8_t segmentId, char* out, size_t outSize) {
  constexpr char prefix[] = "\"seg\":[{";
  const char* segment = strstr(json, prefix);
  if (!segment) return false;
  const size_t pos = size_t(segment - json) + sizeof(prefix) - 1;
  const int length = snprintf(out, outSize, "%.*s\"id\":%u,%s", int(pos), json, unsigned(segmentId), json + pos);
  return length > 0 && size_t(length) < outSize;
}

uint8_t onlineTargetMask() {
  if (!g_deviceCount) return 0;
  if (!g_targetAll) return uint8_t(1U << g_focusDevice);
  uint8_t mask = 0;
  for (size_t i = 0; i < g_deviceCount; ++i) if (g_devices[i].model.online) mask |= uint8_t(1U << i);
  return mask;
}

bool queueTransaction(const char* json, RequestKey key, bool targetMain = false) {
  // A local command is about to change the scene; resample a parked stream.
  g_liveParked = false;
  const uint8_t targets = onlineTargetMask();
  const size_t length = json ? strnlen(json, kMaxRequestLength) : 0;
  if (!targets || !length || length >= kMaxRequestLength) return false;
  for (uint8_t i = 0; i < g_transactionCount; ++i) {
    Transaction& queued = g_transactions[(g_transactionRead + i) % kTransactionQueueSize];
    if (queued.networkGeneration == g_networkGeneration && queued.key == key &&
        queued.targets == targets && queued.targetMainSegment == targetMain) {
      memcpy(queued.json, json, length + 1);
      return true;
    }
  }
  if (g_transactionCount == kTransactionQueueSize) {
    Serial.printf("[WIFI] command queue full; dropping %u\n", unsigned(key));
    return false;
  }
  Transaction& transaction = g_transactions[g_transactionWrite];
  transaction = Transaction{};
  memcpy(transaction.json, json, length + 1);
  transaction.key = key;
  transaction.networkGeneration = g_networkGeneration;
  transaction.targetMainSegment = targetMain;
  transaction.targets = targets;
  g_transactionWrite = (g_transactionWrite + 1) % kTransactionQueueSize;
  g_transactionCount++;
  return true;
}

void applyOptimistic(const char* json, uint8_t targets) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  for (size_t i = 0; i < g_deviceCount; ++i) {
    if (!(targets & (1U << i))) continue;
    applyState(g_devices[i].model, doc.as<JsonObjectConst>(), JsonObjectConst());
    if (i == g_focusDevice) g_model = g_devices[i].model;
  }
  g_stateRev++;
}

void updateSavedPreset(Model& model, uint8_t id, const char* name) {
  const auto preset = std::find_if(model.presets.begin(), model.presets.end(), [id](const PresetInfo& item) {
    return item.id == id;
  });
  if (preset != model.presets.end()) {
    preset->name = name;
    return;
  }
  model.presets.push_back({id, name});
  std::sort(model.presets.begin(), model.presets.end(),
            [](const PresetInfo& a, const PresetInfo& b) { return a.id < b.id; });
}

#if !WLED_TOUCH_SIMULATOR
WiFiClient g_ws;
bool g_wsConnected = false;
size_t g_wsDevice = SIZE_MAX;
bool g_wsLiveArmed = false;
bool g_wsHandshaking = false;
uint32_t g_lastSocketAttempt = 0;
// Health of the Peek subscription.  WLED grants its single Peek slot to the
// most recent {"lv":true} requester and silently skips clients whose socket
// cannot drain, so silence alone is ambiguous.  Frames that stop after having
// flowed mean another viewer holds the slot (only a takeover produces that:
// congestion resumes by itself, which the frame path uses to self-correct a
// kTakenOver verdict).  A subscription that never delivered is a link or load
// problem and retries quietly with backoff instead of on touch.
enum class LivePhase : uint8_t {
  kOff,            // not subscribed (peek disabled or socket down)
  kAwaitingFirst,  // subscribed, first frame not yet seen
  kDelivering,     // frames flowing
  kStalled,        // frame gap detected; congestion until proven otherwise
  kRetryWait,      // never delivered; automatic resubscribe scheduled
  kSuspended,      // retry budget spent; next attempt on user interaction
  kTakenOver,      // confirmed sustained silence; reclaim on interaction
};
LivePhase g_livePhase = LivePhase::kOff;
uint8_t g_liveRetryAttempts = 0;
uint32_t g_liveRetryAt = 0;
uint32_t g_liveSubscriptionStarted = 0;
uint32_t g_liveSocketProbeSentAt = 0;
bool g_liveSocketProbeAcknowledged = false;
uint8_t g_wsRx[kSocketRxCapacity] = {};
size_t g_wsRxLength = 0;
mdns_search_once_t* g_mdnsSearch = nullptr;
bool g_mdnsStarted = false;
#if ESP_IDF_VERSION_MAJOR >= 5
bool g_mdnsStaBound = false;
bool g_mdnsStaBindAttempted = false;
#endif

#if ESP_IDF_VERSION_MAJOR >= 5
// MDNS.begin() is deliberately deferred until Wi-Fi has an address. The
// Arduino wrapper does not replay the already-delivered GOT_IP event though,
// so bind the predefined STA netif explicitly. This is the supported mDNS
// lifecycle action for an interface that became available before mdns_init().
void bindMdnsToConnectedSta() {
  if (g_mdnsStaBound || g_mdnsStaBindAttempted) return;
  g_mdnsStaBindAttempted = true;
  esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!sta) {
    Serial.println("[WIFI] mDNS could not find the Wi-Fi STA netif");
    return;
  }
  const mdns_event_actions_t action = static_cast<mdns_event_actions_t>(
      MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4);
  const esp_err_t err = mdns_netif_action(sta, action);
  if (err != ESP_OK) {
    Serial.printf("[WIFI] mDNS could not bind Wi-Fi STA: %s\n", esp_err_to_name(err));
    return;
  }
  g_mdnsStaBound = true;
  Serial.println("[WIFI] mDNS bound to Wi-Fi STA after IP assignment");
}


struct MdnsBrowseCandidate {
  uint32_t ipv4 = 0;
  char hostname[65] = {};
  uint8_t mac[6] = {};
  bool hasMac = false;
};

mdns_browse_t* g_mdnsBrowse = nullptr;
QueueHandle_t g_mdnsBrowseQueue = nullptr;
uint32_t g_nextMdnsBrowseAttempt = 0;
uint32_t g_nextMdnsStartAttempt = 0;

#if WLED_BOARD == WLED_BOARD_JC4880P443
// ESP-Hosted on this board carries unicast traffic reliably, but multicast
// received by the C6 radio never reaches the P4, so DNS-SD replies sent to
// 224.0.0.251 are lost. RFC 6762 §6.7 legacy queries are built for exactly
// this client: a query sent from a source port other than 5353 must be
// answered with a unicast response addressed straight back to the querier.
constexpr uint16_t kMdnsPort = 5353;
constexpr uint16_t kLegacyMdnsSourcePort = 5359;
constexpr uint16_t kLegacySweepSourcePort = 5360;
// Group-addressed frames (multicast and broadcast) have not proven deliverable
// end to end on this radio path, so the same legacy query is also sent
// directly to every host of the local /24 (RFC 6762 §5.5 direct unicast
// queries). The periodic sweep stays gentle, while a fresh registry gets one
// faster pass on the discovery worker rather than the display/touch loop.
constexpr uint32_t kLegacySweepIntervalMs = 60000;
constexpr uint32_t kLegacySweepBatchIntervalMs = 100;
constexpr uint8_t kLegacySweepBatchSize = 3;
constexpr uint32_t kLegacyInitialSweepBatchIntervalMs = 50;
constexpr uint8_t kLegacyInitialSweepBatchSize = 8;

WiFiUDP g_legacyMdns;
bool g_legacyMdnsReady = false;
uint16_t g_legacyMdnsQueryId = 0;
uint32_t g_nextLegacyMdnsQueryAt = 0;
bool g_legacyMdnsResponseSeen = false;
bool g_legacyMdnsTxLogged = false;
uint32_t g_nextLegacySweepAt = 0;

struct LegacySweepJob {
  uint8_t local[4] = {};
  bool fast = false;
};

struct LegacySweepResult {
  uint32_t ipv4 = 0;
};

QueueHandle_t g_legacySweepJobs = nullptr;
QueueHandle_t g_legacySweepResults = nullptr;
#endif  // WLED_BOARD == WLED_BOARD_JC4880P443


struct MdnsHostResolution {
  mdns_search_once_t* search = nullptr;
  MdnsBrowseCandidate candidate;
};

MdnsHostResolution g_mdnsHostResolutions[kMaxDevices];
#endif

std::string ipToString(uint32_t address) { return std::string(IPAddress(address).toString().c_str()); }

bool isWled(uint32_t ipv4, std::string& name) {
  HTTPClient http;
  if (!http.begin(ipToString(ipv4).c_str(), kWledPort, "/json/info")) return false;
  http.setConnectTimeout(kSocketConnectTimeoutMs);
  http.setTimeout(kSocketConnectTimeoutMs);
  const bool ok = http.GET() == HTTP_CODE_OK;
  if (ok) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getStream()) && doc["ver"].is<const char*>()) {
      if (doc["name"].is<const char*>()) name = doc["name"].as<const char*>();
      http.end();
      return true;
    }
  }
  http.end();
  return false;
}

const char* mdnsTxtValue(const mdns_result_t& result, const char* key) {
  if (!key || !result.txt) return nullptr;
  for (size_t i = 0; i < result.txt_count; ++i) {
    if (result.txt[i].key && result.txt[i].value && !strcmp(result.txt[i].key, key)) {
      return result.txt[i].value;
    }
  }
  return nullptr;
}

bool parseMacAddress(const char* text, uint8_t* mac) {
  if (!text || !mac) return false;
  uint8_t bytes[6] = {};
  uint8_t digits = 0;
  for (; *text; ++text) {
    const char c = *text;
    uint8_t value = 0;
    if (c >= '0' && c <= '9') value = uint8_t(c - '0');
    else if (c >= 'a' && c <= 'f') value = uint8_t(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') value = uint8_t(c - 'A' + 10);
    else if (c == ':' || c == '-') continue;
    else return false;
    if (digits >= 12) return false;
    if (digits & 1) bytes[digits / 2] |= value;
    else bytes[digits / 2] = uint8_t(value << 4);
    ++digits;
  }
  if (digits != 12) return false;
  memcpy(mac, bytes, sizeof(bytes));
  return true;
}

bool rememberMdnsDevice(uint32_t ipv4, const char* hostname, const uint8_t* mac,
                        bool hasMac, uint32_t now) {
  if (!ipv4) return false;
  const int addressIndex = deviceIndexForAddress(ipv4);
  const int identityIndex = hasMac ? deviceIndexForId(mac) : -1;
  const bool identityChanged = hasMac &&
                               (identityIndex < 0 || addressIndex < 0 ||
                                memcmp(g_devices[addressIndex].id, mac, 6));
  DeviceSlot* device = rememberDevice(ipv4, hostname, now, hasMac ? mac : nullptr, true);
  if (!device) return false;
  if (!device->model.online) { device->model.online = true; g_deviceRev++; }
  return addressIndex < 0 || identityChanged;
}

// The remote is the AP's DHCP server, so this is more reliable than waiting
// for a controller to send an mDNS announcement. HTTP identification runs on
// a worker; this loop task only submits one exact DHCP client at a time.
void pumpAccessPointProbeResults(uint32_t now) {
#if !WLED_TOUCH_SIMULATOR
  if (!g_accessPointProbeResults) return;
  AccessPointProbeResult result;
  while (xQueueReceive(g_accessPointProbeResults, &result, 0) == pdTRUE) {
    if (!g_accessPointProbeInFlight || result.ipv4 != g_accessPointProbeAddress ||
        result.networkGeneration != g_accessPointProbeGeneration) {
      continue;
    }
    g_accessPointProbeInFlight = false;
    if (!wifilink::accessPointActive() || result.networkGeneration != g_networkGeneration) continue;
    if (!result.success) {
      if (result.ipv4 != g_lastAccessPointProbeFailure ||
          now - g_lastAccessPointProbeFailureAt >= 10000) {
        g_lastAccessPointProbeFailure = result.ipv4;
        g_lastAccessPointProbeFailureAt = now;
        Serial.printf("[WIFI] hotspot client %s did not answer WLED /json/info\n",
                      ipToString(result.ipv4).c_str());
      }
      continue;
    }
    DeviceSlot* device = rememberDevice(result.ipv4, result.name, now);
    if (!device) continue;
    device->model.online = true;
    device->lastSeen = now;
    saveRegistry();
    g_deviceRev++;
    Serial.printf("[WIFI] WLED found from hotspot client list: %s\n",
                  ipToString(result.ipv4).c_str());
  }
#else
  (void)now;
#endif
}

void probeAccessPointClients(uint32_t now) {
  if (!wifilink::accessPointActive() || int32_t(now - g_nextAccessPointClientProbeAt) < 0) return;
 #if !WLED_TOUCH_SIMULATOR
  if (g_accessPointProbeInFlight) return;
 #endif
  const std::vector<uint32_t> clients = wifilink::accessPointClientAddresses();
  // The CYD receives exact AP client IPs from its DHCP-assignment event. Do
  // not block the display with a speculative subnet sweep while that event is
  // still pending.
  if (clients.empty()) return;

  const uint32_t address = clients[g_nextAccessPointClientProbe % clients.size()];
  ++g_nextAccessPointClientProbe;
  g_nextAccessPointClientProbeAt = now + 400;
  const int knownDevice = deviceIndexForAddress(address);
  if (!address || (knownDevice >= 0 && g_devices[knownDevice].model.online)) return;
#if !WLED_TOUCH_SIMULATOR
  if (!g_accessPointProbeJobs) return;
  const AccessPointProbeJob job{address, g_networkGeneration};
  if (xQueueSend(g_accessPointProbeJobs, &job, 0) != pdTRUE) return;
  g_accessPointProbeInFlight = true;
  g_accessPointProbeAddress = address;
  g_accessPointProbeGeneration = job.networkGeneration;
#endif
}

#if ESP_IDF_VERSION_MAJOR >= 5
void onMdnsBrowseResult(mdns_result_t* result) {
  if (!result || !result->ttl || !g_mdnsBrowseQueue) return;
  uint8_t mac[6] = {};
  const bool hasMac = parseMacAddress(mdnsTxtValue(*result, "mac"), mac);
  bool queuedAddress = false;
  uint8_t addressCount = 0;
  for (mdns_ip_addr_t* address = result->addr; address; address = address->next) {
    if (address->addr.type != ESP_IPADDR_TYPE_V4) continue;
    MdnsBrowseCandidate candidate = {};
    candidate.ipv4 = address->addr.u_addr.ip4.addr;
    candidate.hasMac = hasMac;
    if (hasMac) memcpy(candidate.mac, mac, sizeof(candidate.mac));
    if (result->hostname) {
      snprintf(candidate.hostname, sizeof(candidate.hostname), "%s", result->hostname);
    }
    xQueueSend(g_mdnsBrowseQueue, &candidate, 0);
    queuedAddress = true;
    ++addressCount;
  }
  Serial.printf("[WIFI] mDNS service event: instance=%s host=%s IPv4=%u mac=%s\n",
                result->instance_name ? result->instance_name : "?",
                result->hostname ? result->hostname : "?", unsigned(addressCount),
                hasMac ? "yes" : "no");
  if (!queuedAddress && result->hostname) {
    MdnsBrowseCandidate candidate = {};
    candidate.hasMac = hasMac;
    if (hasMac) memcpy(candidate.mac, mac, sizeof(candidate.mac));
    snprintf(candidate.hostname, sizeof(candidate.hostname), "%s", result->hostname);
    xQueueSend(g_mdnsBrowseQueue, &candidate, 0);
  }
}

void startMdnsHostResolution(const MdnsBrowseCandidate& candidate) {
  if (!candidate.hostname[0]) return;
  for (MdnsHostResolution& pending : g_mdnsHostResolutions) {
    if (pending.search && !strcmp(pending.candidate.hostname, candidate.hostname)) return;
  }
  for (MdnsHostResolution& pending : g_mdnsHostResolutions) {
    if (pending.search) continue;
    pending.candidate = candidate;
    pending.search = mdns_query_async_new(candidate.hostname, nullptr, nullptr, MDNS_TYPE_A,
                                          kMdnsHostResolveTimeoutMs, 1, nullptr);
    if (!pending.search) {
      Serial.printf("[WIFI] mDNS could not start host resolution for %s\n", candidate.hostname);
      pending = MdnsHostResolution{};
    } else {
      Serial.printf("[WIFI] mDNS resolving WLED host %s\n", candidate.hostname);
    }
    return;
  }
}

bool pumpMdnsHostResolutions(uint32_t now) {
  bool registryChanged = false;
  for (MdnsHostResolution& pending : g_mdnsHostResolutions) {
    if (!pending.search) continue;
    mdns_result_t* results = nullptr;
    uint8_t count = 0;
    if (!mdns_query_async_get_results(pending.search, 0, &results, &count)) continue;
    mdns_query_async_delete(pending.search);
    pending.search = nullptr;
    bool foundAddress = false;
    for (mdns_result_t* result = results; result; result = result->next) {
      for (mdns_ip_addr_t* address = result->addr; address; address = address->next) {
        if (address->addr.type == ESP_IPADDR_TYPE_V4) {
          foundAddress = true;
          Serial.printf("[WIFI] mDNS resolved %s to %s\n", pending.candidate.hostname,
                        IPAddress(address->addr.u_addr.ip4.addr).toString().c_str());
          registryChanged |= rememberMdnsDevice(address->addr.u_addr.ip4.addr,
                                                pending.candidate.hostname,
                                                pending.candidate.mac,
                                                pending.candidate.hasMac, now);
          break;
        }
      }
    }
    if (!foundAddress) {
      Serial.printf("[WIFI] mDNS host resolution returned no IPv4 address for %s\n",
                    pending.candidate.hostname);
    }
    mdns_query_results_free(results);
    pending = MdnsHostResolution{};
  }
  return registryChanged;
}

void pumpMdnsBrowse(uint32_t now) {
  bool registryChanged = false;
  if (g_mdnsBrowseQueue) {
    MdnsBrowseCandidate candidate;
    while (xQueueReceive(g_mdnsBrowseQueue, &candidate, 0) == pdTRUE) {
      if (candidate.ipv4) {
        registryChanged |= rememberMdnsDevice(candidate.ipv4, candidate.hostname,
                                              candidate.mac, candidate.hasMac, now);
      } else {
        startMdnsHostResolution(candidate);
      }
    }
  }
  registryChanged |= pumpMdnsHostResolutions(now);
  if (registryChanged) saveRegistry();
}

#if WLED_BOARD == WLED_BOARD_JC4880P443
bool checkLegacyMdnsSocket() {
  if (g_legacyMdnsReady) return true;
  g_legacyMdnsReady = g_legacyMdns.begin(kLegacyMdnsSourcePort) != 0;
  if (!g_legacyMdnsReady) Serial.println("[WIFI] legacy mDNS socket unavailable");
  return g_legacyMdnsReady;
}

bool sendLegacyMdnsQueryTo(const IPAddress& target) {
  const uint8_t query[] = {
      uint8_t(g_legacyMdnsQueryId >> 8), uint8_t(g_legacyMdnsQueryId),
      0x00, 0x00,              // standard query
      0x00, 0x01,              // one question
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      5, '_', 'w', 'l', 'e', 'd', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
      0x00, 0x0C,              // PTR
      0x00, 0x01,              // IN
  };
  if (!g_legacyMdns.beginPacket(target, kMdnsPort)) return false;
  g_legacyMdns.write(query, sizeof(query));
  return g_legacyMdns.endPacket() != 0;
}

void sendLegacyMdnsRound(uint32_t now, bool directOnly) {
  if (!checkLegacyMdnsSocket()) return;
  if (!g_legacyMdnsQueryId) g_legacyMdnsQueryId = uint16_t(now ^ (now >> 16)) | 1;
  bool multicastOk = true;
  bool broadcastOk = true;
  if (!directOnly) {
    multicastOk = sendLegacyMdnsQueryTo(IPAddress(224, 0, 0, 251));
    broadcastOk = sendLegacyMdnsQueryTo(WiFi.broadcastIP());
  }
  // Known controllers are refreshed with a direct unicast query, which keeps
  // them alive without depending on group-addressed delivery at all.
  for (size_t i = 0; i < g_deviceCount; ++i) {
    if (g_devices[i].ipv4) sendLegacyMdnsQueryTo(IPAddress(g_devices[i].ipv4));
  }
  if (!g_legacyMdnsTxLogged || !multicastOk || !broadcastOk) {
    g_legacyMdnsTxLogged = true;
    Serial.printf("[WIFI] legacy mDNS query round (multicast %s, broadcast %s, %u direct)\n",
                  multicastOk ? "ok" : "FAILED", broadcastOk ? "ok" : "FAILED",
                  unsigned(g_deviceCount));
  }
}

bool sendLegacySweepQuery(WiFiUDP& socket, uint16_t& queryId, const IPAddress& target) {
  const uint8_t query[] = {
      uint8_t(queryId >> 8), uint8_t(queryId),
      0x00, 0x00,              // standard query
      0x00, 0x01,              // one question
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      5, '_', 'w', 'l', 'e', 'd', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
      0x00, 0x0C,              // PTR
      0x00, 0x01,              // IN
  };
  if (!socket.beginPacket(target, kMdnsPort)) return false;
  socket.write(query, sizeof(query));
  return socket.endPacket() != 0;
}

// This socket owns a distinct source port so its unicast replies can be read
// on the worker too. Keeping the whole /24 sweep here prevents ARP and UDP
// work from stealing time from LVGL and touch handling.
void legacySweepTask(void*) {
  WiFiUDP socket;
  bool socketReady = false;
  LegacySweepJob job;
  bool active = false;
  uint8_t nextHost = 1;
  uint32_t nextBatchAt = 0;
  uint16_t queryId = uint16_t(millis() ^ (millis() >> 16)) | 1;

  for (;;) {
    // The task is normally started after Wi-Fi is up, but keep retrying if a
    // reconnect races the socket bind instead of silently disabling discovery.
    if (!socketReady) {
      socketReady = socket.begin(kLegacySweepSourcePort) != 0;
      if (!socketReady) {
        vTaskDelay(pdMS_TO_TICKS(250));
        continue;
      }
    }
    if (!active) {
      if (xQueueReceive(g_legacySweepJobs, &job, pdMS_TO_TICKS(20)) == pdTRUE) {
        active = socketReady;
        nextHost = 1;
        nextBatchAt = millis();
      }
    } else {
      // A new discovery request restarts the sweep with its current subnet.
      LegacySweepJob latest;
      if (xQueueReceive(g_legacySweepJobs, &latest, 0) == pdTRUE) {
        job = latest;
        nextHost = 1;
        nextBatchAt = millis();
      }
    }

    while (socketReady && socket.parsePacket() > 0) {
      uint8_t packet[512];
      const int length = socket.read(packet, sizeof(packet));
      if (length <= 0) continue;
      const uint32_t ipv4 = uint32_t(socket.remoteIP());
      if (ipv4 && g_legacySweepResults) {
        const LegacySweepResult result{ipv4};
        xQueueSend(g_legacySweepResults, &result, 0);
      }
    }

    const uint32_t now = millis();
    if (active && int32_t(now - nextBatchAt) >= 0) {
      const uint8_t batchSize = job.fast ? kLegacyInitialSweepBatchSize : kLegacySweepBatchSize;
      const uint32_t batchInterval = job.fast ? kLegacyInitialSweepBatchIntervalMs
                                               : kLegacySweepBatchIntervalMs;
      nextBatchAt = now + batchInterval;
      uint8_t sent = 0;
      while (sent < batchSize && nextHost <= 254) {
        const uint8_t host = nextHost++;
        if (host == job.local[3]) continue;
        sendLegacySweepQuery(socket, queryId, IPAddress(job.local[0], job.local[1], job.local[2], host));
        ++sent;
      }
      if (nextHost > 254) {
        active = false;
        Serial.println("[WIFI] background unicast mDNS sweep finished");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void startLegacySweepWorker() {
  if (g_legacySweepJobs) return;
  g_legacySweepJobs = xQueueCreate(2, sizeof(LegacySweepJob));
  g_legacySweepResults = xQueueCreate(kMaxDevices * 2, sizeof(LegacySweepResult));
  if (g_legacySweepJobs && g_legacySweepResults &&
      xTaskCreate(legacySweepTask, "wledMdns", 4096, nullptr, 1, nullptr) == pdPASS) {
    return;
  }
  if (g_legacySweepJobs) vQueueDelete(g_legacySweepJobs);
  if (g_legacySweepResults) vQueueDelete(g_legacySweepResults);
  g_legacySweepJobs = nullptr;
  g_legacySweepResults = nullptr;
  Serial.println("[WIFI] background mDNS sweep unavailable");
}

void queueLegacySweep(const IPAddress& local, bool fast) {
  if (!g_legacySweepJobs || uint32_t(local) == 0) return;
  LegacySweepJob job = {{local[0], local[1], local[2], local[3]}, fast};
  xQueueSend(g_legacySweepJobs, &job, 0);
}

bool drainLegacySweepResults(uint32_t now) {
  bool registryChanged = false;
  LegacySweepResult result;
  while (g_legacySweepResults && xQueueReceive(g_legacySweepResults, &result, 0) == pdTRUE) {
    const bool changed = rememberMdnsDevice(result.ipv4, nullptr, nullptr, false, now);
    if (changed || !g_legacyMdnsResponseSeen) {
      g_legacyMdnsResponseSeen = true;
      Serial.printf("[WIFI] background mDNS sweep response from %s\n",
                    IPAddress(result.ipv4).toString().c_str());
    }
    registryChanged |= changed;
  }
  return registryChanged;
}

int skipDnsName(const uint8_t* packet, int length, int offset) {
  while (offset >= 0 && offset < length) {
    const uint8_t label = packet[offset];
    if (!label) return offset + 1;
    if ((label & 0xC0) == 0xC0) return offset + 2 <= length ? offset + 2 : -1;
    offset += 1 + label;
  }
  return -1;
}

// Expands a possibly-compressed DNS name into out and returns the offset just
// past its encoding in the record stream, or -1 on malformed input.
int readDnsName(const uint8_t* packet, int length, int offset, char* out, size_t outSize) {
  size_t written = 0;
  int resume = -1;
  bool terminated = false;
  for (uint8_t hops = 0; hops < 8;) {
    if (offset < 0 || offset >= length) return -1;
    const uint8_t label = packet[offset];
    if (!label) {
      if (resume < 0) resume = offset + 1;
      terminated = true;
      break;
    }
    if ((label & 0xC0) == 0xC0) {
      if (offset + 1 >= length) return -1;
      if (resume < 0) resume = offset + 2;
      offset = ((label & 0x3F) << 8) | packet[offset + 1];
      ++hops;
      continue;
    }
    if (offset + 1 + label > length) return -1;
    if (written && written + 1 < outSize) out[written++] = '.';
    for (uint8_t i = 0; i < label; ++i) {
      if (written + 1 < outSize) out[written++] = char(packet[offset + 1 + i]);
    }
    offset += 1 + label;
  }
  if (!terminated) return -1;
  out[written] = '\0';
  return resume;
}

bool collectLegacyMdnsResponse(const uint8_t* packet, int length, uint32_t senderIp, uint32_t now) {
  // Anything arriving on the dedicated query socket is a response to us, so
  // only the QR bit is checked; sweep replies may straddle query-ID rounds.
  if (length < 12 || !(packet[2] & 0x80)) return false;
  const uint16_t questions = uint16_t((packet[4] << 8) | packet[5]);
  const uint32_t records = uint32_t((packet[6] << 8) | packet[7]) +
                           uint32_t((packet[8] << 8) | packet[9]) +
                           uint32_t((packet[10] << 8) | packet[11]);
  int offset = 12;
  for (uint16_t i = 0; i < questions && offset >= 0; ++i) {
    offset = skipDnsName(packet, length, offset);
    if (offset >= 0) offset += 4;
  }
  char hostname[65] = {};
  uint32_t ipv4 = 0;
  uint8_t mac[6] = {};
  bool hasMac = false;
  for (uint32_t i = 0; i < records && offset >= 0; ++i) {
    char owner[65] = {};
    offset = readDnsName(packet, length, offset, owner, sizeof(owner));
    if (offset < 0 || offset + 10 > length) break;
    const uint16_t type = uint16_t((packet[offset] << 8) | packet[offset + 1]);
    const uint16_t rdataLength = uint16_t((packet[offset + 8] << 8) | packet[offset + 9]);
    const int rdata = offset + 10;
    if (rdata + rdataLength > length) break;
    if (type == 1 && rdataLength == 4 && !ipv4) {  // A record
      ipv4 = uint32_t(packet[rdata]) | uint32_t(packet[rdata + 1]) << 8 |
             uint32_t(packet[rdata + 2]) << 16 | uint32_t(packet[rdata + 3]) << 24;
      char* suffix = strstr(owner, ".local");
      if (suffix) *suffix = '\0';
      snprintf(hostname, sizeof(hostname), "%s", owner);
    } else if (type == 16 && !hasMac) {  // TXT record: WLED publishes mac=<hex>
      int cursor = rdata;
      while (cursor < rdata + rdataLength) {
        const uint8_t entryLength = packet[cursor];
        if (!entryLength || cursor + 1 + entryLength > rdata + rdataLength) break;
        if (entryLength > 4 && !memcmp(packet + cursor + 1, "mac=", 4)) {
          char value[24] = {};
          memcpy(value, packet + cursor + 5,
                 std::min<size_t>(entryLength - 4, sizeof(value) - 1));
          hasMac = parseMacAddress(value, mac);
          if (hasMac) break;
        }
        cursor += 1 + entryLength;
      }
    }
    offset = rdata + rdataLength;
  }
  // The unicast reply itself came from the controller, so its source address
  // identifies the device even when the packet parse yields no A record.
  if (!ipv4) ipv4 = senderIp;
  if (!ipv4) return false;
  const bool changed = rememberMdnsDevice(ipv4, hostname[0] ? hostname : nullptr, mac, hasMac, now);
  if (changed || !g_legacyMdnsResponseSeen) {
    g_legacyMdnsResponseSeen = true;
    Serial.printf("[WIFI] legacy mDNS response from %s (host=%s mac=%s)\n",
                  IPAddress(ipv4).toString().c_str(), hostname[0] ? hostname : "?",
                  hasMac ? "yes" : "no");
  }
  return changed;
}

void pumpLegacyMdnsDiscovery(uint32_t now) {
  startLegacySweepWorker();
  bool registryChanged = drainLegacySweepResults(now);
  while (g_legacyMdnsReady && g_legacyMdns.parsePacket() > 0) {
    uint8_t packet[512];
    const int length = g_legacyMdns.read(packet, sizeof(packet));
    if (length > 0) {
      registryChanged |= collectLegacyMdnsResponse(packet, length,
                                                   uint32_t(g_legacyMdns.remoteIP()), now);
    }
  }
  if (registryChanged) saveRegistry();
  const bool scanRequested = g_scanRequested;
  g_scanRequested = false;
  const bool fullScanRequested = g_fullScanRequested;
  g_fullScanRequested = false;
  // A /24 sweep is 253 UDP sends plus an ARP transaction for nearly every
  // address. On ESP-Hosted that burst can exhaust the C6 transport even after
  // the worker itself has finished. Remembered controllers need only the
  // direct legacy query above; reserve a blanket sweep for an empty registry
  // or the user's explicit Scan action.
  const bool blanketDiscoveryNeeded = fullScanRequested || g_deviceCount == 0;
  if (scanRequested || int32_t(now - g_nextLegacyMdnsQueryAt) >= 0) {
    g_nextLegacyMdnsQueryAt = now + kDiscoverIntervalMs;
    sendLegacyMdnsRound(now, !blanketDiscoveryNeeded);
  }
  if (blanketDiscoveryNeeded &&
      (scanRequested || int32_t(now - g_nextLegacySweepAt) >= 0)) {
    g_nextLegacySweepAt = now + kLegacySweepIntervalMs;
    const IPAddress local = WiFi.localIP();
    const bool fast = g_deviceCount == 0;
    queueLegacySweep(local, fast);
    Serial.printf("[WIFI] background unicast mDNS sweep of %u.%u.%u.1-254 started%s\n",
                  unsigned(local[0]), unsigned(local[1]), unsigned(local[2]),
                  fast ? " (initial fast pass)" : "");
  }
}
#endif  // WLED_BOARD == WLED_BOARD_JC4880P443
#endif  // ESP_IDF_VERSION_MAJOR >= 5

// presets.json is fetched over blocking HTTP with timeouts long enough to
// freeze touch and rendering for seconds if run from loop().  The transfer
// runs on a worker task instead; it owns only its own HTTP client and the
// heap-allocated result it hands back, so every model and UI mutation stays
// on the loop task.
struct PresetFetchJob {
  uint32_t ipv4;
  uint32_t token;
};
struct PresetFetchResult {
  uint32_t ipv4 = 0;
  uint32_t token = 0;
  bool ok = false;
  std::vector<PresetInfo> presets;
};
QueueHandle_t g_presetFetchJobs = nullptr;
QueueHandle_t g_presetFetchResults = nullptr;
bool g_presetFetchInFlight = false;

// presets.json contains an entire WLED state for every preset.  Deserializing
// that full document is needlessly expensive on the CYD: this app only needs
// the top-level preset number and its display name.  Parse those incrementally
// so the catalog fetch never competes with LVGL for a large heap allocation.
class PresetJsonReader {
 public:
  explicit PresetJsonReader(Stream& input) : input_(input) {}

  int nextNonWhitespace() {
    int c;
    do {
      c = next();
    } while (c >= 0 && (c == ' ' || c == '\t' || c == '\r' || c == '\n'));
    return c;
  }

  bool expect(char expected) { return nextNonWhitespace() == expected; }

  // The opening quote has already been consumed.  A null output simply skips
  // the string, which avoids allocating for fields we do not display.
  bool readString(char* output, size_t outputSize) {
    size_t written = 0;
    if (output && outputSize) output[0] = '\0';
    for (;;) {
      int c = next();
      if (c < 0) return false;
      if (c == '"') {
        if (output && outputSize) output[std::min(written, outputSize - 1)] = '\0';
        return true;
      }
      if (c == '\\') {
        c = next();
        if (c < 0) return false;
        if (c == 'u') {
          // WLED preset names are normally UTF-8 already.  Consume escaped
          // Unicode correctly enough to preserve parser alignment; substitute
          // a marker rather than retaining a temporary decoded string.
          for (uint8_t i = 0; i < 4; ++i) if (next() < 0) return false;
          c = '?';
        } else {
          switch (c) {
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            default: break;
          }
        }
      }
      if (output && written + 1 < outputSize) output[written] = char(c);
      ++written;
    }
  }

  // The first non-whitespace character of the value has already been read.
  bool skipValue(int first, uint8_t nesting = 0) {
    if (first < 0 || nesting > 24) return false;
    if (first == '"') return readString(nullptr, 0);
    if (first == '{' || first == '[') {
      const char closer = first == '{' ? '}' : ']';
      for (;;) {
        int c = nextNonWhitespace();
        if (c == closer) return true;
        if (first == '{') {
          if (c != '"' || !readString(nullptr, 0) || !expect(':')) return false;
          c = nextNonWhitespace();
        }
        if (!skipValue(c, nesting + 1)) return false;
        c = nextNonWhitespace();
        if (c == closer) return true;
        if (c != ',') return false;
      }
    }

    // Numbers, true, false, and null end immediately before a structural
    // delimiter.  Save that delimiter for the enclosing object/array parser.
    for (;;) {
      int c = next();
      if (c < 0) return true;
      if (c == ',' || c == '}' || c == ']') {
        putBack(c);
        return true;
      }
    }
  }

 private:
  int next() {
    if (putback_ >= 0) {
      const int c = putback_;
      putback_ = -1;
      return c;
    }
    const uint32_t deadline = millis() + 5000;
    do {
      const int c = input_.read();
      if (c >= 0) return c;
      delay(1);  // This runs on the preset worker, never the UI task.
    } while (int32_t(millis() - deadline) < 0);
    return -1;
  }

  void putBack(int c) { putback_ = c; }

  Stream& input_;
  int putback_ = -1;
};

bool parsePresetObject(PresetJsonReader& reader, char* name, size_t nameSize) {
  for (;;) {
    int c = reader.nextNonWhitespace();
    if (c == '}') return true;
    if (c != '"') return false;
    char field[2] = {};
    if (!reader.readString(field, sizeof(field)) || !reader.expect(':')) return false;
    c = reader.nextNonWhitespace();
    if (field[0] == 'n' && field[1] == '\0' && c == '"') {
      if (!reader.readString(name, nameSize)) return false;
    } else if (!reader.skipValue(c)) {
      return false;
    }
    c = reader.nextNonWhitespace();
    if (c == '}') return true;
    if (c != ',') return false;
  }
}

bool parsePresetCatalog(Stream& input, std::vector<PresetInfo>& out) {
  PresetJsonReader reader(input);
  if (!reader.expect('{')) return false;
  for (;;) {
    int c = reader.nextNonWhitespace();
    if (c == '}') return true;
    if (c != '"') return false;
    char key[8] = {};
    if (!reader.readString(key, sizeof(key)) || !reader.expect(':')) return false;
    c = reader.nextNonWhitespace();
    char name[65] = {};
    if (c == '{') {
      if (!parsePresetObject(reader, name, sizeof(name))) return false;
    } else if (!reader.skipValue(c)) {
      return false;
    }
    char* end = nullptr;
    const unsigned long id = strtoul(key, &end, 10);
    if (end && *end == '\0' && id >= 1 && id <= 250) {
      out.push_back({uint8_t(id), name[0] ? name : std::string("Preset ") + std::to_string(id)});
    }
    c = reader.nextNonWhitespace();
    if (c == '}') return true;
    if (c != ',') return false;
  }
}

bool fetchPresetsBlocking(uint32_t ipv4, std::vector<PresetInfo>& out) {
  if (!ipv4) return false;
  HTTPClient http;
  if (!http.begin(ipToString(ipv4).c_str(), kWledPort, "/presets.json")) return false;
  // Generous timeouts are affordable here: this runs on the fetch worker, and
  // a marginal link needs room for several TCP retransmits before giving up.
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    if (status < 0) {
      Serial.printf("[WIFI] presets.json request failed (%s)\n",
                    HTTPClient::errorToString(status).c_str());
    } else {
      Serial.printf("[WIFI] presets.json request failed (HTTP %d)\n", status);
    }
    http.end();
    return false;
  }
  if (!parsePresetCatalog(http.getStream(), out)) {
    Serial.println("[WIFI] presets.json parse failed");
    http.end();
    return false;
  }
  http.end();
  std::sort(out.begin(), out.end(),
            [](const PresetInfo& a, const PresetInfo& b) { return a.id < b.id; });
  return true;
}

void presetFetchTask(void*) {
  PresetFetchJob job;
  for (;;) {
    if (xQueueReceive(g_presetFetchJobs, &job, portMAX_DELAY) != pdTRUE) continue;
    auto* result = new PresetFetchResult();
    result->ipv4 = job.ipv4;
    result->token = job.token;
    result->ok = fetchPresetsBlocking(job.ipv4, result->presets);
    if (xQueueSend(g_presetFetchResults, &result, portMAX_DELAY) != pdTRUE) delete result;
  }
}

void startPresetFetchWorker() {
  g_presetFetchJobs = xQueueCreate(2, sizeof(PresetFetchJob));
  g_presetFetchResults = xQueueCreate(2, sizeof(PresetFetchResult*));
  if (g_presetFetchJobs && g_presetFetchResults &&
      xTaskCreate(presetFetchTask, "wledPresets", 8192, nullptr, 1, nullptr) == pdPASS) {
    return;
  }
  if (g_presetFetchJobs) { vQueueDelete(g_presetFetchJobs); g_presetFetchJobs = nullptr; }
  if (g_presetFetchResults) { vQueueDelete(g_presetFetchResults); g_presetFetchResults = nullptr; }
  Serial.println("[WIFI] preset fetch worker unavailable; falling back to blocking fetches");
}

void httpCommandTask(void*) {
  HttpCommandJob job;
  for (;;) {
    if (xQueueReceive(g_httpCommandJobs, &job, portMAX_DELAY) != pdTRUE) continue;
    HTTPClient http;
    bool success = false;
    if (job.networkGeneration == g_networkGeneration &&
        http.begin(ipToString(job.ipv4).c_str(), kWledPort, "/json/state")) {
      http.setConnectTimeout(kSocketConnectTimeoutMs);
      http.setTimeout(kSocketConnectTimeoutMs);
      http.addHeader("Content-Type", "application/json");
      success = http.POST(String(job.payload)) == HTTP_CODE_OK;
      http.end();
    }
    const HttpCommandResult result{job.networkGeneration, success};
    xQueueSend(g_httpCommandResults, &result, portMAX_DELAY);
  }
}

void startHttpCommandWorker() {
  g_httpCommandJobs = xQueueCreate(2, sizeof(HttpCommandJob));
  g_httpCommandResults = xQueueCreate(2, sizeof(HttpCommandResult));
  if (g_httpCommandJobs && g_httpCommandResults &&
      xTaskCreate(httpCommandTask, "wledHttp", 6144, nullptr, 1, nullptr) == pdPASS) return;
  if (g_httpCommandJobs) { vQueueDelete(g_httpCommandJobs); g_httpCommandJobs = nullptr; }
  if (g_httpCommandResults) { vQueueDelete(g_httpCommandResults); g_httpCommandResults = nullptr; }
  Serial.println("[WIFI] command worker unavailable; HTTP fallback disabled");
}

void accessPointProbeTask(void*) {
  AccessPointProbeJob job;
  for (;;) {
    if (xQueueReceive(g_accessPointProbeJobs, &job, portMAX_DELAY) != pdTRUE) continue;
    AccessPointProbeResult result{};
    result.ipv4 = job.ipv4;
    result.networkGeneration = job.networkGeneration;
    if (job.networkGeneration == g_networkGeneration) {
      std::string name;
      result.success = isWled(job.ipv4, name);
      snprintf(result.name, sizeof(result.name), "%s", name.c_str());
    }
    xQueueSend(g_accessPointProbeResults, &result, portMAX_DELAY);
  }
}

void startAccessPointProbeWorker() {
  g_accessPointProbeJobs = xQueueCreate(2, sizeof(AccessPointProbeJob));
  g_accessPointProbeResults = xQueueCreate(2, sizeof(AccessPointProbeResult));
  if (g_accessPointProbeJobs && g_accessPointProbeResults &&
      xTaskCreate(accessPointProbeTask, "wledApProbe", 6144, nullptr, 1, nullptr) == pdPASS) return;
  if (g_accessPointProbeJobs) { vQueueDelete(g_accessPointProbeJobs); g_accessPointProbeJobs = nullptr; }
  if (g_accessPointProbeResults) { vQueueDelete(g_accessPointProbeResults); g_accessPointProbeResults = nullptr; }
  Serial.println("[WIFI] hotspot probe worker unavailable");
}

void applyPresetFetchResult(PresetFetchResult& result, uint32_t now) {
  const int index = deviceIndexForAddress(result.ipv4);
  const bool focused = index >= 0 && size_t(index) == g_focusDevice;
  if (result.ok && index >= 0) {
    g_devices[index].model.presets = std::move(result.presets);
    // The UI reads the focused-model snapshot rather than DeviceSlot directly.
    // Keep it in sync when the catalog arrives after the state snapshot.
    if (focused) g_model.presets = g_devices[index].model.presets;
    g_catalogRev++;
    Serial.printf("[WIFI] loaded %u presets\n", unsigned(g_devices[index].model.presets.size()));
  }
  // A result from before an invalidation or focus change may refresh the
  // cache above, but must not satisfy or delay the fetch still owed.
  if (!focused || result.token != g_presetFetchToken) return;
  if (result.ok) {
    g_presetsLoaded = true;
    g_presetFetchAttempts = 0;
  } else {
    g_presetFetchAttempts = std::min<uint8_t>(g_presetFetchAttempts + 1, 6);
    const uint32_t delayMs = 1000UL << std::min<uint8_t>(g_presetFetchAttempts, 4);
    g_nextPresetFetch = now + delayMs;
    Serial.printf("[WIFI] presets.json unavailable; retrying in %lums\n", delayMs);
  }
}

void pumpPresetFetch(uint32_t now) {
  if (g_presetFetchResults) {
    PresetFetchResult* result = nullptr;
    while (xQueueReceive(g_presetFetchResults, &result, 0) == pdTRUE) {
      g_presetFetchInFlight = false;
      applyPresetFetchResult(*result, now);
      delete result;
    }
  }
  if (g_presetFetchInFlight) return;
  if (g_focusDevice >= g_deviceCount || !g_devices[g_focusDevice].model.online ||
      g_presetsLoaded || int32_t(now - g_nextPresetFetch) < 0) return;
  const uint32_t ipv4 = g_devices[g_focusDevice].ipv4;
  if (!ipv4) return;
  if (g_presetFetchJobs) {
    const PresetFetchJob job{ipv4, g_presetFetchToken};
    if (xQueueSend(g_presetFetchJobs, &job, 0) == pdTRUE) g_presetFetchInFlight = true;
    return;
  }
  PresetFetchResult result;
  result.ipv4 = ipv4;
  result.token = g_presetFetchToken;
  result.ok = fetchPresetsBlocking(ipv4, result.presets);
  applyPresetFetchResult(result, now);
}

void applyWebsocketText(const uint8_t* payload, size_t length) {
  if (g_wsDevice >= g_deviceCount) return;
  JsonDocument doc;
  if (deserializeJson(doc, payload, length) || !doc["state"].is<JsonObject>()) return;
  DeviceSlot& device = g_devices[g_wsDevice];
  const bool wasOffline = !device.model.online;
  const uint32_t now = millis();
  if (g_liveSocketProbeSentAt) g_liveSocketProbeAcknowledged = true;
  const JsonObjectConst incomingState = doc["state"].as<JsonObjectConst>();
  bool preservePreset = false;
  if (device.pendingPreset) {
    if (incomingState["ps"].is<int>() &&
        incomingState["ps"].as<int>() == device.pendingPreset) {
      // This is WLED's response to the local choice, so future updates should
      // once again be reflected normally.
      device.pendingPreset = 0;
    } else if (now - device.pendingPresetSince < kPresetEchoGraceMs) {
      preservePreset = true;
    } else {
      // Do not mask an independently changed controller indefinitely if the
      // requested command was never acknowledged.
      device.pendingPreset = 0;
    }
  }
  applyState(device.model, incomingState, doc["info"].as<JsonObjectConst>(), preservePreset);
  device.lastSeen = now;
  if (g_wsDevice == g_focusDevice) g_model = device.model;
  g_stateRev++;
  if (wasOffline) g_deviceRev++;
  // Fresh state means the scene may no longer match the cached frame.
  g_liveParked = false;
}

bool decodeLive(const uint8_t* data, size_t length) {
  if (length < 2 || data[0] != 'L') return false;
  size_t pos = 2;
  if (data[1] == 2) {
    if (length < 4) return false;
    g_liveW = data[2]; g_liveH = data[3]; pos = 4;
  } else { g_liveW = g_liveH = 0; }
  const size_t sourceCount = (length - pos) / 3;
  const size_t count = std::min(sourceCount, kLivePreviewSamples);
  if (!count) return false;
  // Sample across the complete physical strip rather than retaining only its
  // first pixels.  This avoids copying 3 KB frames the display cannot show.
  for (size_t i = 0; i < count; ++i) {
    const size_t sourceIndex = i * sourceCount / count;
    memcpy(g_live + i * 3, data + pos + sourceIndex * 3, 3);
  }
  g_liveCount = uint16_t(count);
  g_liveRev++;
  return true;
}

void socketDisconnected(bool connectionLost = true, bool markOffline = true) {
  if (g_ws.connected()) g_ws.stop();
  const bool wasConnected = g_wsConnected;
  g_wsConnected = false;
  g_wsHandshaking = false;
  g_wsLiveArmed = false;
  g_livePhase = LivePhase::kOff;
  g_liveRetryAttempts = 0;
  g_liveRetryAt = 0;
  g_liveSubscriptionStarted = 0;
  g_liveSocketProbeSentAt = 0;
  g_liveSocketProbeAcknowledged = false;
  g_liveParked = false;
  g_wsRxLength = 0;
  if (wasConnected && connectionLost) {
    g_connectionLostAt = millis();
    Serial.println("[WIFI] WLED websocket disconnected");
  }
  if (markOffline && wasConnected && g_wsDevice < g_deviceCount &&
      g_devices[g_wsDevice].model.online) {
    g_devices[g_wsDevice].model.online = false;
    if (g_wsDevice == g_focusDevice) g_model = g_devices[g_wsDevice].model;
    g_stateRev++; g_deviceRev++;
  }
}

void setConnectionStatus(ConnectionStatus status) {
  if (g_connectionStatus == status) return;
  g_connectionStatus = status;
  ++g_connectionRev;
}

void markDevicesOffline() {
  bool changed = false;
  for (size_t i = 0; i < g_deviceCount; ++i) {
    if (!g_devices[i].model.online) continue;
    g_devices[i].model.online = false;
    changed = true;
  }
  if (!changed) return;
  if (g_focusDevice < g_deviceCount) g_model = g_devices[g_focusDevice].model;
  ++g_stateRev;
  ++g_deviceRev;
}

void updateConnectionStatus(uint32_t now) {
  if (!wifilink::connected()) {
    setConnectionStatus(ConnectionStatus::kNoConnection);
    return;
  }
#if WLED_TOUCH_SIMULATOR
  setConnectionStatus(ConnectionStatus::kConnected);
  return;
#endif
  if (g_wsConnected) {
    g_hadWledConnection = true;
    g_wledConnectionSsid = wifilink::connectedSsid();
    g_connectionLostAt = 0;
    setConnectionStatus(ConnectionStatus::kConnected);
    return;
  }
  // Saved registry entries are only candidates from an earlier network.  Do
  // not call one "found" until this Wi-Fi network has advertised it (or the
  // user has verified it by address), which sets its online flag.
  const bool focusedDeviceFound = g_focusDevice < g_deviceCount &&
                                  g_devices[g_focusDevice].model.online;
  if (g_connectionLostAt && now - g_connectionLostAt < kReconnectIntervalMs) {
    setConnectionStatus(ConnectionStatus::kConnectionLost);
    return;
  }
  if (focusedDeviceFound) {
    setConnectionStatus(ConnectionStatus::kConnecting);
    return;
  }
  // A TCP/WebSocket handshake to a remembered IP is an attempt, not proof
  // that a WLED controller exists there.
  if (g_wsHandshaking) {
    setConnectionStatus(ConnectionStatus::kConnecting);
    return;
  }
  if (g_hadWledConnection) {
    setConnectionStatus(ConnectionStatus::kReconnecting);
    return;
  }
  {
    setConnectionStatus(g_discoveryStartedAt && now - g_discoveryStartedAt >= kNoDevicesFoundDelayMs
                            ? ConnectionStatus::kNoDevicesFound
                            : ConnectionStatus::kSearching);
    return;
  }
}

bool socketSend(uint8_t opcode, const uint8_t* payload, size_t length) {
  if (!g_wsConnected || length > 512) return false;
  uint8_t header[8] = {uint8_t(0x80 | opcode), uint8_t(0x80 | (length < 126 ? length : 126))};
  size_t headerLength = 2;
  if (length >= 126) { header[2] = uint8_t(length >> 8); header[3] = uint8_t(length); headerLength = 4; }
  const uint32_t seed = millis() ^ uint32_t(length << 8);
  header[headerLength++] = uint8_t(seed); header[headerLength++] = uint8_t(seed >> 8);
  header[headerLength++] = uint8_t(seed >> 16); header[headerLength++] = uint8_t(seed >> 24);
  uint8_t frame[512];
  for (size_t i = 0; i < length; ++i) frame[i] = payload[i] ^ header[headerLength - 4 + (i & 3)];
  return g_ws.write(header, headerLength) == headerLength && g_ws.write(frame, length) == length;
}

bool socketSendText(const char* text) {
  return text && socketSend(0x1, reinterpret_cast<const uint8_t*>(text), strlen(text));
}

void pumpSocket() {
  if (!g_ws.connected()) { socketDisconnected(); return; }
  size_t readBudget = kSocketReadBudgetBytes;
  while (readBudget && g_ws.available() && g_wsRxLength < sizeof(g_wsRx)) {
    const size_t available = size_t(g_ws.available());
    const size_t freeSpace = sizeof(g_wsRx) - g_wsRxLength;
    const size_t bytesToRead = std::min(readBudget, std::min(available, freeSpace));
    const int bytesRead = g_ws.read(g_wsRx + g_wsRxLength, bytesToRead);
    if (bytesRead <= 0) break;
    g_wsRxLength += size_t(bytesRead);
    readBudget -= size_t(bytesRead);
  }
  if (!g_wsConnected) {
    if (g_wsRxLength < 4) return;
    for (size_t i = 3; i < g_wsRxLength; ++i) {
      if (g_wsRx[i - 3] == '\r' && g_wsRx[i - 2] == '\n' && g_wsRx[i - 1] == '\r' && g_wsRx[i] == '\n') {
        if (g_wsRxLength < 12 || memcmp(g_wsRx, "HTTP/1.1 101", 12) != 0) { socketDisconnected(); return; }
        const size_t remain = g_wsRxLength - (i + 1);
        memmove(g_wsRx, g_wsRx + i + 1, remain); g_wsRxLength = remain;
        g_wsConnected = true; g_wsHandshaking = false; g_wsLiveArmed = false;
        g_liveSocketProbeSentAt = 0;
        g_liveSocketProbeAcknowledged = false;
        socketSendText("{\"v\":true}");
        Serial.println("[WIFI] WLED websocket connected");
        break;
      }
    }
    return;
  }
  // Peek frames are absolute snapshots, so only the newest complete binary
  // frame in the backlog is worth decoding; everything older became stale the
  // moment it was overtaken.  Draining stale frames without per-frame decode
  // or compaction lets a redraw-induced backlog collapse in one pass instead
  // of pacing the loop at the stream's arrival rate indefinitely.
  size_t offset = 0;
  const uint8_t* livePayload = nullptr;
  size_t livePayloadLength = 0;
  uint8_t textFramesProcessed = 0;
  while (offset + 2 <= g_wsRxLength) {
    const uint8_t* frame = g_wsRx + offset;
    const uint8_t opcode = frame[0] & 0x0F;
    size_t payloadLength = frame[1] & 0x7F, headerLength = 2;
    if (payloadLength == 126) {
      if (offset + 4 > g_wsRxLength) break;
      payloadLength = (size_t(frame[2]) << 8) | frame[3];
      headerLength = 4;
    } else if (payloadLength == 127) { socketDisconnected(); return; }
    if (offset + headerLength + payloadLength > g_wsRxLength) break;
    const uint8_t* payload = frame + headerLength;
    if (opcode == 0x1) {
      // State updates must all apply, in order; keep the cap so a burst of
      // JSON parses cannot monopolize one pass.
      if (textFramesProcessed == kMaxSocketFramesPerLoop) break;
      applyWebsocketText(payload, payloadLength);
      ++textFramesProcessed;
    } else if (opcode == 0x2) {
      livePayload = payload;
      livePayloadLength = payloadLength;
    } else if (opcode == 0x8) {
      const uint16_t closeCode = payloadLength >= 2 ?
          (uint16_t(payload[0]) << 8) | payload[1] : 0;
      Serial.printf("[WIFI] WLED websocket closed (code %u)\n", unsigned(closeCode));
      socketDisconnected();
      return;
    } else if (opcode == 0x9) socketSend(0xA, payload, payloadLength);
    offset += headerLength + payloadLength;
  }
  if (livePayload && g_wsConnected) {
    // A browser-style Peek stream may use a frame form we do not render yet.
    // Its arrival still proves the subscription is alive, so do not reclaim
    // an actively delivering stream just because this decoder skipped it.
    decodeLive(livePayload, livePayloadLength);
    if (g_liveWanted && !g_liveParked) {
      g_lastLiveRx = millis();
      g_liveSocketProbeSentAt = 0;
      g_liveSocketProbeAcknowledged = false;
      if (g_livePhase != LivePhase::kDelivering) {
        // A frame can only reach this client while it owns the Peek slot,
        // so any arrival overrides a stall, takeover, or failure verdict.
        if (g_liveRetryAttempts) Serial.println("[WIFI] WLED Peek stream restored");
        g_livePhase = LivePhase::kDelivering;
        g_liveRetryAttempts = 0;
        g_liveRetryAt = 0;
      }
    }
  }
  if (offset) {
    // applyWebsocketText can tear the socket down mid-pass; never compact
    // beyond what the buffer still holds.
    if (offset >= g_wsRxLength) g_wsRxLength = 0;
    else { memmove(g_wsRx, g_wsRx + offset, g_wsRxLength - offset); g_wsRxLength -= offset; }
  }
  if (g_wsRxLength == sizeof(g_wsRx)) {
    // Never leave the receive path permanently full.  The capacity covers
    // WLED's normal state response and several Peek frames of backlog; a
    // single frame larger than the whole buffer can never complete.
    Serial.println("[WIFI] WLED websocket frame exceeds receive buffer");
    socketDisconnected();
  }
}

bool collectMdns(mdns_result_t* results, uint32_t now) {
  bool registryChanged = false;
  for (mdns_result_t* result = results; result; result = result->next) {
    uint32_t ipv4 = 0;
    for (mdns_ip_addr_t* address = result->addr; address; address = address->next) {
      if (address->addr.type == ESP_IPADDR_TYPE_V4) { ipv4 = address->addr.u_addr.ip4.addr; break; }
    }
    if (!ipv4) continue;
    uint8_t mac[6] = {};
    const bool hasMac = parseMacAddress(mdnsTxtValue(*result, "mac"), mac);
    registryChanged |= rememberMdnsDevice(ipv4, result->hostname, mac, hasMac, now);
  }
  return registryChanged;
}

void pumpDiscovery(uint32_t now) {
#if ESP_IDF_VERSION_MAJOR >= 5
  if (wifilink::accessPointActive()) return;
  pumpMdnsBrowse(now);
#if WLED_BOARD == WLED_BOARD_JC4880P443
  // The hosted radio path loses multicast replies, so the browse alone cannot
  // be relied upon here; the legacy unicast-response query does the real work.
  pumpLegacyMdnsDiscovery(now);
#else
  const bool scanRequested = g_scanRequested;
  g_scanRequested = false;
#endif
  if (!g_mdnsBrowseQueue) g_mdnsBrowseQueue = xQueueCreate(kMaxDevices * 2, sizeof(MdnsBrowseCandidate));
  if (!g_mdnsBrowse && g_mdnsBrowseQueue &&
#if WLED_BOARD == WLED_BOARD_JC4880P443
      int32_t(now - g_nextMdnsBrowseAttempt) >= 0) {
#else
      (scanRequested || int32_t(now - g_nextMdnsBrowseAttempt) >= 0)) {
#endif
    g_mdnsBrowse = mdns_browse_new("_wled", "_tcp", onMdnsBrowseResult);
    if (g_mdnsBrowse) {
      Serial.println("[WIFI] WLED mDNS browse started");
    }
    else {
      g_nextMdnsBrowseAttempt = now + kDiscoverIntervalMs;
      Serial.println("[WIFI] WLED mDNS browse could not start; retrying");
    }
  }
  return;
#else
  // ESP-IDF 4 (the original CYD environment) has no persistent browse API.
  // Its asynchronous query is retained solely for that older SDK.
  if (g_mdnsSearch) {
    mdns_result_t* results = nullptr; uint8_t count = 0;
    if (!mdns_query_async_get_results(g_mdnsSearch, 0, &results, &count)) return;
    mdns_query_async_delete(g_mdnsSearch); g_mdnsSearch = nullptr;
    const bool registryChanged = collectMdns(results, now);
    mdns_query_results_free(results);
    // Preferences::put* commits synchronously to NVS.  Discovery is periodic,
    // but the known-controller registry changes only when a new address is
    // found; writing it every pass caused a visible live-preview hitch.
    if (registryChanged) saveRegistry();
    return;
  }
  if (!g_scanRequested && now - g_lastDiscover < kDiscoverIntervalMs) return;
  g_scanRequested = false;
  g_lastDiscover = now;
  g_mdnsSearch = mdns_query_async_new(nullptr, "_wled", "_tcp", MDNS_TYPE_PTR,
                                       kDiscoverQueryTimeoutMs, kMaxDevices, nullptr);
#endif
}

void checkSocket(uint32_t now) {
  if (g_focusDevice >= g_deviceCount || !g_devices[g_focusDevice].ipv4) return;
  if (g_wsDevice == g_focusDevice && (g_wsConnected || g_wsHandshaking)) return;
  if (now - g_lastSocketAttempt < kReconnectIntervalMs) return;
  // Switching the user's selected controller is expected, not a connection
  // failure. Keep that transition out of the "connection lost" state.
  if (g_wsDevice != SIZE_MAX) socketDisconnected(false);
  g_wsDevice = g_focusDevice;
  g_lastSocketAttempt = now;
  IPAddress address(g_devices[g_focusDevice].ipv4);
  if (!g_ws.connect(address, kWledPort, kSocketConnectTimeoutMs)) return;
  const std::string host = ipToString(g_devices[g_focusDevice].ipv4);
  g_ws.printf("GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: d2xlZC1yZW1vdGU=\r\nSec-WebSocket-Version: 13\r\n\r\n", host.c_str());
  g_wsHandshaking = true;
  g_wsRxLength = 0;
  Serial.printf("[WIFI] connecting to WLED at %s\n", ipToString(g_devices[g_focusDevice].ipv4).c_str());
}

bool sendTo(size_t index, const char* payload, uint32_t networkGeneration) {
  if (index >= g_deviceCount || !g_devices[index].ipv4) return false;
  if (index == g_wsDevice && g_wsConnected) return socketSendText(payload);
  if (!g_httpCommandJobs || g_httpCommandInFlight) return false;
  HttpCommandJob job{};
  job.ipv4 = g_devices[index].ipv4;
  job.networkGeneration = networkGeneration;
  const size_t length = strnlen(payload, sizeof(job.payload));
  if (length == sizeof(job.payload)) return false;
  memcpy(job.payload, payload, length + 1);
  if (xQueueSend(g_httpCommandJobs, &job, 0) != pdTRUE) return false;
  g_httpCommandInFlight = true;
  g_httpCommandGeneration = networkGeneration;
  g_httpCommandTarget = uint8_t(index);
  memcpy(g_httpCommandPayload, payload, length + 1);
  return true;
}
#else
void setConnectionStatus(ConnectionStatus status) {
  if (g_connectionStatus == status) return;
  g_connectionStatus = status;
  ++g_connectionRev;
}

void updateConnectionStatus(uint32_t) {
  setConnectionStatus(wifilink::connected() ? ConnectionStatus::kConnected
                                              : ConnectionStatus::kNoConnection);
}

void loadSimulator() {
  DeviceSlot* device = rememberDevice(0x3201A8C0UL, "WLED", millis());
  if (!device) return;
  device->model.online = true;
  device->model.power = true;
  device->model.brightness = 140;
  device->model.effect = 0;
  device->model.palette = 0;
  device->model.color = 0xFFA000;
  loadSimulatorPresets(*device);
  g_model = device->model;
  // Seed the top-bar Peek preview with a cool blue-to-violet LED gradient.
  // The simulator has no WebSocket stream to provide one naturally.
  constexpr uint8_t kPeekPattern[][3] = {
      {20, 68, 178}, {30, 99, 208}, {48, 136, 232}, {72, 158, 238},
      {101, 130, 230}, {124, 98, 211}, {145, 79, 190},
  };
  constexpr uint16_t kPatternLength = sizeof(kPeekPattern) / sizeof(kPeekPattern[0]);
  constexpr uint16_t kPatternRepeat = 3;
  g_liveCount = kPatternLength * kPatternRepeat;
  g_liveW = g_liveCount;
  g_liveH = 1;
  for (uint16_t i = 0; i < g_liveCount; ++i) {
    const uint8_t* color = kPeekPattern[(i / kPatternRepeat) % kPatternLength];
    memcpy(g_live + i * 3, color, 3);
  }
  g_lastLiveRx = millis();
  g_liveRev++;
  g_stateRev++; g_deviceRev++;
}
#endif

void pumpTransactions(uint32_t now) {
#if !WLED_TOUCH_SIMULATOR
  // Always drain results, even after a network switch emptied the transaction
  // ring. Otherwise a stale completion could leave the single HTTP slot busy.
  if (g_httpCommandResults) {
    HttpCommandResult result;
    while (xQueueReceive(g_httpCommandResults, &result, 0) == pdTRUE) {
      if (!g_httpCommandInFlight || result.networkGeneration != g_httpCommandGeneration) continue;
      g_httpCommandInFlight = false;
      if (result.networkGeneration != g_networkGeneration || !g_transactionCount) continue;
      Transaction& transaction = g_transactions[g_transactionRead];
      if (transaction.networkGeneration != result.networkGeneration) continue;
      const size_t index = g_httpCommandTarget;
      if (index >= g_deviceCount) continue;
      const uint8_t bit = uint8_t(1U << index);
      if (result.success) {
        transaction.targets &= uint8_t(~bit);
        applyOptimistic(g_httpCommandPayload, bit);
      } else if (++transaction.attempts[index] < kMaxCommandAttempts) {
        transaction.retryAt[index] = now + kCommandRetryMs;
      } else {
        g_devices[index].model.online = false;
        g_deviceRev++;
        transaction.targets &= uint8_t(~bit);
      }
      if (!transaction.targets) {
        transaction = Transaction{};
        g_transactionRead = (g_transactionRead + 1) % kTransactionQueueSize;
        g_transactionCount--;
      }
      return;
    }
  }
  if (g_httpCommandInFlight) return;
#endif
  while (g_transactionCount) {
    Transaction& transaction = g_transactions[g_transactionRead];
    if (transaction.networkGeneration != g_networkGeneration) {
      transaction = Transaction{};
      g_transactionRead = (g_transactionRead + 1) % kTransactionQueueSize;
      g_transactionCount--;
      continue;
    }
    bool dispatched = false;
    for (size_t index = 0; index < g_deviceCount; ++index) {
      const uint8_t bit = uint8_t(1U << index);
      if (!(transaction.targets & bit) || int32_t(now - transaction.retryAt[index]) < 0) continue;
      char targeted[kMaxRequestLength];
      const char* payload = transaction.json;
      if (transaction.targetMainSegment && targetMainSegment(transaction.json, g_devices[index].model.mainSegmentId,
                                                             targeted, sizeof(targeted))) payload = targeted;
#if !WLED_TOUCH_SIMULATOR
      if (!sendTo(index, payload, transaction.networkGeneration)) {
        if (++transaction.attempts[index] < kMaxCommandAttempts) {
          transaction.retryAt[index] = now + kCommandRetryMs;
          return;
        }
        g_devices[index].model.online = false;
        g_deviceRev++;
      }
      if (g_httpCommandInFlight) return;
#endif
      transaction.targets &= uint8_t(~bit);
      applyOptimistic(payload, bit);
      dispatched = true;
      break;  // one destination per loop keeps touch and display work responsive
    }
    if (transaction.targets) return;
    transaction = Transaction{};
    g_transactionRead = (g_transactionRead + 1) % kTransactionQueueSize;
    g_transactionCount--;
    if (dispatched) return;
  }
}

}  // namespace

void begin() {
#if !WLED_TOUCH_SIMULATOR
  startPresetFetchWorker();
  startHttpCommandWorker();
  startAccessPointProbeWorker();
#endif
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    const uint8_t count = std::min<uint8_t>(prefs.getUChar("wifiCnt", 0), kMaxDevices);
    for (uint8_t i = 0; i < count; ++i) {
      char addressKey[8]; snprintf(addressKey, sizeof(addressKey), "wifi%u", unsigned(i));
      const uint32_t address = prefs.getUInt(addressKey, 0);
      uint8_t id[6] = {};
#if WLED_TOUCH_SIMULATOR
      const bool hasId = false;
#else
      char identityKey[8]; snprintf(identityKey, sizeof(identityKey), "wifiM%u", unsigned(i));
      const bool hasId = prefs.isKey(identityKey) &&
                         prefs.getBytesLength(identityKey) == sizeof(id) &&
                         prefs.getBytes(identityKey, id, sizeof(id)) == sizeof(id);
#endif
      if (address) rememberDevice(address, nullptr, 0, hasId ? id : nullptr);
    }
    if (g_deviceCount) {
      g_focusDevice = std::min<size_t>(prefs.getUChar("wifiFocus", 0), g_deviceCount - 1);
      g_targetAll = g_deviceCount > 1 && prefs.getBool("wifiAll", false);
      g_model = g_devices[g_focusDevice].model;
    }
    prefs.end();
  }
#if WLED_TOUCH_SIMULATOR
  if (!g_deviceCount) loadSimulator();
#endif
}

void loop(uint32_t now) {
#if !WLED_TOUCH_SIMULATOR
  if (!wifilink::connected()) {
    if (g_wifiWasConnected) {
      socketDisconnected();
      markDevicesOffline();
      g_connectionLostAt = now;
      g_wifiWasConnected = false;
      g_activeNetworkSsid.clear();
      g_activeNetworkIsAccessPoint = false;
    }
    updateConnectionStatus(now);
    return;
  }
  const std::string activeNetworkSsid = wifilink::connectedSsid();
  const bool activeNetworkIsAccessPoint = wifilink::accessPointActive();
  const bool networkChanged = !g_wifiWasConnected || g_activeNetworkSsid != activeNetworkSsid ||
                              g_activeNetworkIsAccessPoint != activeNetworkIsAccessPoint;
  if (networkChanged) {
    if (g_wifiWasConnected) socketDisconnected(false);
    g_wifiWasConnected = true;
    g_activeNetworkSsid = activeNetworkSsid;
    g_activeNetworkIsAccessPoint = activeNetworkIsAccessPoint;
    if (++g_networkGeneration == 0) ++g_networkGeneration;
    // The old network's targets may be unrelated devices at reused addresses.
    // Tags below also protect work that was already handed to a worker.
    g_transactionRead = 0;
    g_transactionWrite = 0;
    g_transactionCount = 0;
#if !WLED_TOUCH_SIMULATOR
    g_accessPointProbeInFlight = false;
#endif
    // Every association or mode switch begins a new discovery epoch. A
    // controller seen on the previous AP/LAN must never remain presented as
    // found on the newly active network.
    markDevicesOffline();
#if ESP_IDF_VERSION_MAJOR >= 5
    if (g_mdnsBrowseQueue) xQueueReset(g_mdnsBrowseQueue);
#endif
    g_hadWledConnection = false;
    g_connectionLostAt = 0;
    g_discoveryStartedAt = now;
    g_scanRequested = true;
    if (wifilink::accessPointActive()) {
      Serial.println("[WIFI] hotspot discovery: probing connected DHCP clients directly");
    }
  }
  // A mobile hotspot owns its client list, so no mDNS browse or subnet sweep
  // is needed there. Keep mDNS strictly for normal station Wi-Fi discovery.
  if (!wifilink::accessPointActive() && !g_mdnsStarted &&
      int32_t(now - g_nextMdnsStartAttempt) >= 0) {
    g_mdnsStarted = MDNS.begin("wled-remote");
    if (g_mdnsStarted) {
      g_scanRequested = true;
      Serial.println("[WIFI] mDNS started");
    } else {
      g_nextMdnsStartAttempt = now + kDiscoverIntervalMs;
      Serial.println("[WIFI] mDNS could not start; retrying");
    }
  }
#if ESP_IDF_VERSION_MAJOR >= 5
  if (g_mdnsStarted && !wifilink::accessPointActive()) bindMdnsToConnectedSta();
#endif
  if (g_mdnsStarted && !wifilink::accessPointActive()) pumpDiscovery(now);
  pumpAccessPointProbeResults(now);
  probeAccessPointClients(now);
  checkSocket(now);
  pumpSocket();
  if (g_wsConnected) {
    // A dimmed idle display parks after every short resample window, animated
    // or not; Display Off stops the stream entirely via setLivePeek and
    // Always On never marks the stream idle.
    if (g_liveIdle && g_wsLiveArmed && !g_liveParked &&
        now - g_liveSubscriptionStarted >= kLiveIdleSampleMs) {
      g_liveParked = true;
    }
    // A parked stream keeps g_liveWanted set (the bar shows the cached frame)
    // but releases the Peek subscription until something changes the scene.
    const bool streamDesired = g_liveWanted && !g_liveParked;
    if (streamDesired != g_wsLiveArmed) {
      socketSendText(streamDesired ? "{\"lv\":true}" : "{\"lv\":false}");
      g_wsLiveArmed = streamDesired;
      if (streamDesired) {
        g_lastLiveRx = 0;
        g_liveSubscriptionStarted = now;
        g_livePhase = LivePhase::kAwaitingFirst;
      } else {
        g_liveSubscriptionStarted = 0;
        g_livePhase = LivePhase::kOff;
        g_liveRetryAttempts = 0;
        g_liveRetryAt = 0;
        g_liveSocketProbeSentAt = 0;
        g_liveSocketProbeAcknowledged = false;
      }
    } else if (streamDesired) {
      switch (g_livePhase) {
        case LivePhase::kDelivering:
          if (now - g_lastLiveRx >= kLiveRecoverySilenceMs) {
            g_livePhase = LivePhase::kStalled;
            g_liveSocketProbeSentAt = now;
            g_liveSocketProbeAcknowledged = false;
            socketSendText("{\"v\":true}");
          }
          break;
        case LivePhase::kStalled:
#if WLED_BOARD == WLED_BOARD_JC4880P443
          if (!g_liveSocketProbeAcknowledged && g_liveSocketProbeSentAt &&
              now - g_liveSocketProbeSentAt >= kLiveSocketProbeTimeoutMs) {
            // Recover the affected TCP connection before the hosted transport
            // starves completely. Preserve the controller's online state so
            // this brief repair does not flash a false disconnect in the UI.
            Serial.println("[WIFI] WLED websocket stopped responding; reconnecting");
            socketDisconnected(false, false);
            g_lastSocketAttempt = now - kReconnectIntervalMs;
            break;
          }
#endif
          if (
#if WLED_BOARD == WLED_BOARD_JC4880P443
              g_liveSocketProbeAcknowledged &&
#endif
              now - g_lastLiveRx >= kLiveTakeoverConfirmMs) {
            // Sustained total silence is the takeover signature.  WLED does
            // not announce when the other viewer releases the slot. The state
            // probe above proves this socket itself is still healthy.
            g_livePhase = LivePhase::kTakenOver;
            g_liveSocketProbeSentAt = 0;
            g_liveSocketProbeAcknowledged = false;
            Serial.println("[WIFI] WLED Peek stream taken by another viewer; waiting for remote interaction");
          }
          break;
        case LivePhase::kAwaitingFirst:
          if (now - g_liveSubscriptionStarted >= kLiveFirstFrameGraceMs) {
            if (g_liveRetryAttempts < kLiveRetryMaxAttempts) {
              g_liveRetryAt = now + (kLiveRetryBaseMs << g_liveRetryAttempts);
              g_livePhase = LivePhase::kRetryWait;
              if (!g_liveRetryAttempts) {
                Serial.println("[WIFI] WLED Peek stream not delivering; retrying with backoff");
              }
            } else {
              g_livePhase = LivePhase::kSuspended;
              Serial.println("[WIFI] WLED Peek stream unavailable; will retry on remote interaction");
            }
          }
          break;
        case LivePhase::kRetryWait:
          if (int32_t(now - g_liveRetryAt) >= 0) {
            ++g_liveRetryAttempts;
            g_wsLiveArmed = false;  // resubscribe on the next pass
          }
          break;
        default:
          break;  // kSuspended and kTakenOver wait for user interaction
      }
    }
    if (now - g_lastPoll >= kStatePollIntervalMs) { g_lastPoll = now; socketSendText("{\"v\":true}"); }
  }
  pumpPresetFetch(now);
  for (size_t i = 0; i < g_deviceCount; ++i) {
    if (i != g_wsDevice && g_devices[i].model.online && g_devices[i].lastSeen &&
        now - g_devices[i].lastSeen > kDeviceOfflineMs) { g_devices[i].model.online = false; g_deviceRev++; }
  }
#endif
  pumpTransactions(now);
  updateConnectionStatus(now);
}

void servicePeekSocket() {
#if !WLED_TOUCH_SIMULATOR
  // A CYD redraw is synchronous SPI and can last longer than a Peek-frame
  // interval. Drain as soon as rendering returns, without rerunning mDNS,
  // command dispatch, or other work from the full network loop.
  if (g_wsConnected) pumpSocket();
#endif
}

const Model& model() { return g_model; }
bool online() { return g_model.online; }
ConnectionStatus connectionStatus() { return g_connectionStatus; }
uint32_t connectionRevision() { return g_connectionRev; }
size_t deviceCount() { return g_deviceCount; }
size_t activeDeviceCount() {
  size_t result = 0;
  for (size_t i = 0; i < g_deviceCount; ++i) if (g_devices[i].model.online) result++;
  return result;
}
DeviceInfo deviceInfo(size_t index) {
  DeviceInfo result = {};
  if (index >= g_deviceCount) return result;
  memcpy(result.mac, g_devices[index].id, sizeof(result.mac));
  result.online = g_devices[index].model.online;
  result.name = g_devices[index].alias[0] ? g_devices[index].alias : g_devices[index].model.name;
  return result;
}
size_t focusedDevice() { return g_focusDevice; }
bool targetingAll() { return g_targetAll; }
uint16_t mixedStateMask() {
  if (!g_targetAll) return kMixedNone;
  const DeviceSlot* first = nullptr; uint16_t mixed = kMixedNone;
  for (size_t i = 0; i < g_deviceCount; ++i) {
    const DeviceSlot& device = g_devices[i]; if (!device.model.online) continue;
    if (!first) { first = &device; continue; }
    if (device.model.power != first->model.power) mixed |= kMixedPower;
    if (device.model.brightness != first->model.brightness) mixed |= kMixedBrightness;
    if (device.model.effect != first->model.effect) mixed |= kMixedEffect;
    if (device.model.palette != first->model.palette) mixed |= kMixedPalette;
    if (device.model.preset != first->model.preset) mixed |= kMixedPreset;
    if (device.model.color != first->model.color) mixed |= kMixedColor;
  }
  return mixed;
}
void selectDevice(size_t index) {
  if (index >= g_deviceCount) return;
  g_focusDevice = index; g_targetAll = false; g_model = g_devices[index].model;
  g_hadWledConnection = false;
  g_connectionLostAt = 0;
  if (wifilink::connected()) setConnectionStatus(ConnectionStatus::kConnecting);
  g_presetsLoaded = !g_model.presets.empty(); g_presetFetchAttempts = 0; g_nextPresetFetch = millis();
  g_liveCount = 0; g_lastLiveRx = 0; g_liveParked = false;
  g_stateRev++; g_catalogRev++; saveRegistry();
}
void selectAll() { if (g_deviceCount) { g_targetAll = true; g_stateRev++; saveRegistry(); } }
void renameDevice(size_t index, const char* name) {
  if (index >= g_deviceCount || !name || strnlen(name, kMaxDeviceAliasLength + 1) > kMaxDeviceAliasLength) return;
  strcpy(g_devices[index].alias, name); saveDeviceAlias(g_devices[index]); g_deviceRev++;
}
bool forgetDevice(size_t index) {
  if (index >= g_deviceCount || g_devices[index].model.online) return false;
  saveDeviceAlias(g_devices[index]);
  for (size_t i = index; i + 1 < g_deviceCount; ++i) g_devices[i] = std::move(g_devices[i + 1]);
  g_devices[--g_deviceCount] = DeviceSlot{};
  if (!g_deviceCount) { g_focusDevice = 0; g_targetAll = false; g_model = Model{}; }
  else { g_focusDevice = std::min(g_focusDevice, g_deviceCount - 1); g_targetAll &= g_deviceCount > 1; g_model = g_devices[g_focusDevice].model; }
  saveRegistry(); g_stateRev++; g_catalogRev++; g_deviceRev++; return true;
}
void scanNow() {
  g_scanRequested = true;
  g_fullScanRequested = true;
  g_discoveryStartedAt = millis();
  if (wifilink::connected() && !g_deviceCount) setConnectionStatus(ConnectionStatus::kSearching);
}
bool addDeviceByAddress(const char* host) {
#if WLED_TOUCH_SIMULATOR
  (void)host; return false;
#else
  IPAddress address;
  if (!host || !address.fromString(host)) return false;
  std::string name;
  if (!isWled(uint32_t(address), name)) return false;
  DeviceSlot* device = rememberDevice(uint32_t(address), name.c_str(), millis());
  if (!device) return false;
  device->model.online = true; saveRegistry(); g_deviceRev++; return true;
#endif
}
uint32_t stateRevision() { return g_stateRev; }
uint32_t catalogRevision() { return g_catalogRev; }
uint32_t liveRevision() { return g_liveRev; }
uint32_t deviceRevision() { return g_deviceRev; }
bool commandsPending() { return g_transactionCount != 0; }
bool loading() {
#if WLED_TOUCH_SIMULATOR
  return false;
#else
  return g_presetFetchInFlight;
#endif
}
const uint8_t* liveLeds(uint16_t& count, uint16_t& width, uint16_t& height) { count = g_liveCount; width = g_liveW; height = g_liveH; return count ? g_live : nullptr; }
bool livePeekEnabled() { return g_liveWanted; }
uint32_t liveFrameAgeMs(uint32_t now) { return g_lastLiveRx ? now - g_lastLiveRx : UINT32_MAX; }
void poll() { queueTransaction("{\"v\":true}", RequestKey::kPoll); }
void requestCatalogs() {
  if (g_focusDevice >= g_deviceCount) return;
  g_devices[g_focusDevice].model.presets.clear();
  g_presetsLoaded = false;
  g_presetFetchAttempts = 0;
  g_nextPresetFetch = millis();
  g_presetFetchToken++;
  g_catalogRev++;
}
void setPower(bool on) { queueTransaction(on ? "{\"on\":true,\"v\":true}" : "{\"on\":false,\"v\":true}", RequestKey::kPower); }
void togglePower() { setPower(!g_model.power); }
void setBrightness(uint8_t value) { char json[40]; snprintf(json, sizeof(json), "{\"bri\":%u,\"v\":true}", value); queueTransaction(json, RequestKey::kBrightness); }
void applyPreset(uint8_t id) {
  char json[40];
  snprintf(json, sizeof(json), "{\"ps\":%u,\"v\":true}", id);
  if (!queueTransaction(json, RequestKey::kPreset)) return;

  // Keep the model the UI reflects aligned with the optimistic selected row
  // until WLED's state push confirms the change.
  const uint8_t targets = onlineTargetMask();
  const uint32_t now = millis();
  for (size_t i = 0; i < g_deviceCount; ++i) {
    if (targets & uint8_t(1U << i)) {
      g_devices[i].model.preset = id;
      g_devices[i].pendingPreset = id;
      g_devices[i].pendingPresetSince = now;
    }
  }
  if (g_focusDevice < g_deviceCount) g_model = g_devices[g_focusDevice].model;
  g_stateRev++;
}
void savePreset(uint8_t id, const char* name) {
  if (!id || !name || !*name) return;
  JsonDocument doc;
  doc["psave"] = id;
  doc["n"] = name;
  doc["ib"] = true;
  doc["sb"] = true;
  char json[kMaxRequestLength];
  if (serializeJson(doc, json, sizeof(json)) >= sizeof(json) ||
      !queueTransaction(json, RequestKey::kPresetSave)) return;

  // A preset save has no name in WLED's state response. Reflect the queued
  // change locally so the Presets tab updates immediately without refetching
  // presets.json.
  const uint8_t targets = onlineTargetMask();
  for (size_t i = 0; i < g_deviceCount; ++i) {
    if (targets & uint8_t(1U << i)) updateSavedPreset(g_devices[i].model, id, name);
  }
  if (g_focusDevice < g_deviceCount) g_model = g_devices[g_focusDevice].model;
  g_catalogRev++;
}
void setEffect(uint8_t id) { char json[48]; snprintf(json, sizeof(json), "{\"seg\":[{\"fx\":%u}],\"v\":true}", id); queueTransaction(json, RequestKey::kEffect, true); }
void setPalette(uint8_t id) { char json[48]; snprintf(json, sizeof(json), "{\"seg\":[{\"pal\":%u}],\"v\":true}", id); queueTransaction(json, RequestKey::kPalette, true); }
void setColor(uint8_t r, uint8_t g, uint8_t b) { char json[64]; snprintf(json, sizeof(json), "{\"seg\":[{\"col\":[[%u,%u,%u]]}],\"v\":true}", r, g, b); queueTransaction(json, RequestKey::kColor, true); }
void setEffectParams(int speed, int intensity) { char json[64]; const int n = snprintf(json, sizeof(json), "{\"seg\":[{%s%s}],\"v\":true}", speed >= 0 ? "\"sx\":" : "", speed >= 0 ? std::to_string(speed).c_str() : ""); if (intensity >= 0 && n > 0) { char value[20]; snprintf(value, sizeof(value), "%s\"ix\":%d", speed >= 0 ? "," : "", intensity); char* place = strstr(json, "}]"); if (place) { const size_t used = size_t(place - json); snprintf(json + used, sizeof(json) - used, "%s}],\"v\":true}", value); } } queueTransaction(json, RequestKey::kEffectParams, true); }
void setCustomParam(uint8_t index, uint8_t value) { if (index >= 1 && index <= 3) { char json[48]; snprintf(json, sizeof(json), "{\"seg\":[{\"c%u\":%u}],\"v\":true}", index, value); queueTransaction(json, index == 1 ? RequestKey::kCustom1 : index == 2 ? RequestKey::kCustom2 : RequestKey::kCustom3, true); } }
void sendRaw(const char* json) { queueTransaction(json, RequestKey::kRaw); }
void setLivePeek(bool on) {
  g_liveWanted = on;
  if (!on) {
    g_liveCount = 0;
    g_lastLiveRx = 0;
    g_liveParked = false;
#if !WLED_TOUCH_SIMULATOR
    g_liveSubscriptionStarted = 0;
    g_livePhase = LivePhase::kOff;
    g_liveRetryAttempts = 0;
    g_liveRetryAt = 0;
    g_liveSocketProbeSentAt = 0;
    g_liveSocketProbeAcknowledged = false;
#endif
    g_liveRev++;
  }
}

void setLivePeekIdle(bool idle) {
  if (g_liveIdle == idle) return;
  g_liveIdle = idle;
  // The user is looking again; resume the continuous stream immediately.
  if (!idle) g_liveParked = false;
}

void resumeLivePeekOnInteraction() {
#if !WLED_TOUCH_SIMULATOR
  if (!g_liveWanted) return;
  // Let loop() perform the write; touch callbacks run from LVGL and should
  // only signal intent, never perform socket I/O themselves.  Touch acts only
  // on the two states that wait for the user: a stream another viewer took,
  // and a stream whose automatic retry budget ran out.
  g_liveParked = false;
  g_liveSocketProbeSentAt = 0;
  g_liveSocketProbeAcknowledged = false;
  if (g_livePhase == LivePhase::kTakenOver) {
    g_wsLiveArmed = false;
    g_liveRetryAttempts = 0;
    Serial.println("[WIFI] remote interaction; reclaiming WLED Peek stream");
  } else if (g_livePhase == LivePhase::kSuspended) {
    g_wsLiveArmed = false;
    g_liveRetryAttempts = 0;
    Serial.println("[WIFI] remote interaction; retrying WLED Peek stream");
  }
#endif
}

}  // namespace wled
