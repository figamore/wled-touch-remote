#include "wled_api.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

#include "app_state.h"  // kBroadcastMac
#include "generated/wled_catalog.h"

namespace wled {
namespace {

constexpr uint8_t kMagic = 0x4E;
constexpr uint8_t kVersion = 0x01;
constexpr uint8_t kHeaderSize = 6;
constexpr uint8_t kFragSize = 244;
constexpr uint8_t kMsgRequest = 0x01;
constexpr uint8_t kMsgResponse = 0x02;
constexpr uint8_t kMsgPush = 0x03;
constexpr uint8_t kMsgDiscover = 0x04;
constexpr uint8_t kMsgLive = 0x05;
constexpr uint8_t kMsgAnnounce = 0x06;

constexpr size_t kMaxJson = 8192;
constexpr uint8_t kMaxFrags = kMaxJson / kFragSize + 1;
constexpr uint32_t kLiveReArmMs = 10000;  // refresh the live subscription well inside WLED's 30 s lease
constexpr uint32_t kHeartbeatMs = 3000;   // cheap DISCOVER presence cadence while connected
constexpr uint32_t kResyncMs = 30000;     // periodic full state poll to correct any missed pushes
constexpr uint32_t kOfflineMs = 10000;    // no rx for this long => offline (must exceed kHeartbeatMs)
constexpr uint32_t kHopDwellMs = 150;     // time spent probing each channel while hunting for WLED
constexpr uint32_t kReasmTimeoutMs = 500;
constexpr uint32_t kRequestTimeoutMs = 1500;
constexpr uint32_t kRequestRetryMs = 120;
constexpr uint8_t  kMaxChannel = 13;
constexpr uint8_t  kCatalogTries = 6;     // give up refetching the preset list after this many attempts
constexpr uint8_t  kMaxRequestAttempts = 4;
// WLED emits at most three fragments per loop and this remote drains six. Twelve callback
// slots absorb scheduling jitter without reserving an entire 8 KB message twice in DRAM.
constexpr size_t   kRxQueueSize = 12;
// A transaction stores one payload plus a target bitmap, regardless of group size. Eight
// distinct pending actions cover UI bursts while using substantially less internal DRAM.
constexpr size_t   kTransactionQueueSize = 8;
// Remote-generated requests are compact single-fragment JSON; 192 bytes also covers a safely
// escaped 32-character preset name while keeping every queue entry bounded.
constexpr size_t   kMaxRequestLength = 192;
constexpr size_t   kMaxDevices = 6;
constexpr size_t   kMaxDeviceAliasLength = 32;
constexpr size_t   kMaxLiveBytes = 4 + 256 * 3; // WLED caps live peek at 256 RGB pixels
constexpr uint8_t  kDeviceRegistryVersion = 1;
constexpr uint32_t kDiscoveryCollectMs = 500;

Model g_model;
struct DeviceSlot {
  uint8_t mac[6] = {};
  uint8_t channel = 0;
  uint8_t capabilities = 0;
  bool peerRegistered = false;
  uint32_t lastSeen = 0;
  char alias[kMaxDeviceAliasLength + 1] = {};
  Model model;
};
DeviceSlot g_devices[kMaxDevices];
size_t g_deviceCount = 0;
size_t g_focusDevice = 0;
bool g_targetAll = false;
uint32_t g_discoveryFirstReply = 0;
uint32_t g_stateRev = 0;
uint32_t g_catalogRev = 0;
uint32_t g_liveRev = 0;
uint32_t g_deviceRev = 0;
uint32_t g_lastRx = 0;
uint32_t g_lastLinkRx = 0;
uint32_t g_lastLiveRx = 0;
uint8_t g_msgId = 1;

// The Wi-Fi callback only copies frames into this bounded queue. Reassembly and model updates
// happen in loop() context, avoiding races between the high-priority Wi-Fi task and the UI.
struct RxFrame {
  uint8_t src[6] = {};
  uint8_t data[kHeaderSize + kFragSize] = {};
  uint8_t len = 0;
};

RxFrame g_rxQueue[kRxQueueSize];
uint8_t g_rxRead = 0, g_rxWrite = 0, g_rxCount = 0;
std::atomic_flag g_rxLock = ATOMIC_FLAG_INIT;
std::atomic<uint32_t> g_rxQueueDrops{0};
std::atomic<uint32_t> g_rxLockDrops{0};

uint8_t g_reasm[kMaxFrags * kFragSize + 1];
uint8_t g_curId = 0, g_curType = 0, g_curTotal = 0, g_count = 0;
uint8_t g_curMac[6] = {};
uint64_t g_flags = 0;
size_t g_len = 0;
bool g_active = false;
uint32_t g_reasmLast = 0;

// Latest decoded live frame.
uint8_t g_live[kMaxLiveBytes];
uint16_t g_liveCount = 0, g_liveW = 0, g_liveH = 0;

bool g_liveWanted = false;
uint32_t g_liveArmed = 0;

// ESP-NOW only receives on the channel the radio is tuned to, and WLED replies on its own WiFi
// channel. So we hop channels broadcasting DISCOVER until WLED answers, then lock on. This keeps the
// remote plug-and-play (no channel config) the way the send-only WizMote path was.
uint8_t g_channel = 0;
bool g_channelLocked = false;
uint8_t g_preferredChannel = 0;
bool g_preferredChannelTried = false;
uint32_t g_lastHop = 0;
uint32_t g_lastHeartbeat = 0;
bool g_discoveryRequested = false;
uint8_t g_lastDiscoveryId = 0;
uint32_t g_lastDiscoverySent = 0;
uint32_t g_lastPoll = 0;
uint32_t g_lastCatalog = 0;
uint32_t g_presetCatalogRefreshAt = 0;
uint8_t g_presetCatalogRefreshMac[6] = {};
uint8_t g_catalogTries = 0;
bool g_presetsLoaded = false;

enum class RequestKey : uint8_t {
  None,
  Poll,
  Catalog,
  CatalogRefresh,
  Power,
  Brightness,
  Preset,
  PresetSave,
  Effect,
  Palette,
  Color,
  EffectParams,
  Custom1,
  Custom2,
  Custom3,
  Live,
};

struct QueuedTransaction {
  char json[kMaxRequestLength] = {};
  RequestKey key = RequestKey::None;
  bool retryable = true;
  bool targetMainSegment = false;
  bool started = false;
  uint8_t targets = 0;
  uint8_t reconcileTargets = 0;
};

QueuedTransaction g_transactions[kTransactionQueueSize];
uint8_t g_transactionRead = 0, g_transactionWrite = 0, g_transactionCount = 0;

struct ActiveRequest {
  uint8_t target[6] = {};
  char json[kMaxRequestLength] = {};
  RequestKey key = RequestKey::None;
  bool retryable = true;
  bool active = false;
  bool awaitingResponse = false;
  uint8_t id = 0;
  uint8_t attempts = 0;
  uint32_t sentAt = 0;
  uint32_t nextAttempt = 0;
};

ActiveRequest g_request;
uint32_t g_framesDeferredForResponse = 0;

uint8_t currentRadioChannel() {
  uint8_t primary = 0;
  wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
  return esp_wifi_get_channel(&primary, &secondary) == ESP_OK ? primary : 0;
}

const char* requestKeyName(RequestKey key) {
  switch (key) {
    case RequestKey::Poll:         return "poll";
    case RequestKey::Catalog:      return "catalog";
    case RequestKey::CatalogRefresh: return "catalogRefresh";
    case RequestKey::Power:        return "power";
    case RequestKey::Brightness:   return "brightness";
    case RequestKey::Preset:       return "preset";
    case RequestKey::PresetSave:   return "presetSave";
    case RequestKey::Effect:       return "effect";
    case RequestKey::Palette:      return "palette";
    case RequestKey::Color:        return "color";
    case RequestKey::EffectParams: return "effectParams";
    case RequestKey::Custom1:      return "custom1";
    case RequestKey::Custom2:      return "custom2";
    case RequestKey::Custom3:      return "custom3";
    case RequestKey::Live:         return "live";
    default:                       return "raw";
  }
}

const char* messageTypeName(uint8_t type) {
  switch (type) {
    case kMsgRequest:  return "REQUEST";
    case kMsgResponse: return "RESPONSE";
    case kMsgPush:     return "PUSH";
    case kMsgDiscover: return "LEGACY_HELLO";
    case kMsgLive:     return "LIVE";
    case kMsgAnnounce: return "ANNOUNCE";
    default:           return "UNKNOWN";
  }
}

void resetReasm() {
  g_active = false;
  g_curTotal = g_count = 0;
  g_flags = 0;
  g_len = 0;
}

bool isBroadcastMac(const uint8_t* mac) {
  if (!mac) return true;
  for (size_t i = 0; i < 6; ++i) {
    if (mac[i] != 0xFF) return false;
  }
  return true;
}

DeviceSlot* findDevice(const uint8_t* mac) {
  if (!mac) return nullptr;
  for (size_t i = 0; i < g_deviceCount; ++i) {
    if (memcmp(g_devices[i].mac, mac, sizeof(g_devices[i].mac)) == 0) return &g_devices[i];
  }
  return nullptr;
}


// Builds a MAC-keyed NVS key that remains within ESP32 Preferences' 15-character limit.
void deviceAliasKey(const uint8_t* mac, char* key, size_t keySize) {
  if (!mac || !key || keySize < 14) return;
  snprintf(key, keySize, "n%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Loads the controller's local display alias after its MAC is discovered.
void loadDeviceAlias(DeviceSlot& device) {
  char key[14] = {};
  deviceAliasKey(device.mac, key, sizeof(key));
  Preferences prefs;
  if (!key[0] || !prefs.begin(kPrefsNamespace, true)) return;
  prefs.getString(key, device.alias, sizeof(device.alias));
  prefs.end();
}

// Persists or clears the local display alias without changing the WLED instance name.
void saveDeviceAlias(const DeviceSlot& device) {
  char key[14] = {};
  deviceAliasKey(device.mac, key, sizeof(key));
  Preferences prefs;
  if (!key[0] || !prefs.begin(kPrefsNamespace, false)) return;
  if (device.alias[0]) prefs.putString(key, device.alias);
  else prefs.remove(key);
  prefs.end();
}

bool decodeHexNibble(char value, uint8_t& nibble) {
  if (value >= '0' && value <= '9') nibble = uint8_t(value - '0');
  else if (value >= 'a' && value <= 'f') nibble = uint8_t(value - 'a' + 10);
  else if (value >= 'A' && value <= 'F') nibble = uint8_t(value - 'A' + 10);
  else return false;
  return true;
}

bool decodeDeviceRecord(const char* record, uint8_t* mac, uint8_t& channel) {
  if (!record || !mac || strnlen(record, 16) != 14) return false;
  for (size_t i = 0; i < 6; ++i) {
    uint8_t high, low;
    if (!decodeHexNibble(record[i * 2], high) || !decodeHexNibble(record[i * 2 + 1], low)) return false;
    mac[i] = uint8_t((high << 4) | low);
  }
  channel = uint8_t(atoi(record + 12));
  return channel >= 1 && channel <= kMaxChannel;
}

// Persists known MAC/channel pairs and target selection so startup can probe the useful channel first.
void saveDeviceRegistry() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) return;
  prefs.putUChar("regVer", kDeviceRegistryVersion);
  prefs.putUChar("devCount", uint8_t(g_deviceCount));
  prefs.putUChar("focus", uint8_t(g_focusDevice));
  prefs.putBool("targetAll", g_targetAll);
  for (size_t i = 0; i < g_deviceCount; ++i) {
    char key[6];
    char record[16];
    snprintf(key, sizeof(key), "dev%u", unsigned(i));
    snprintf(record, sizeof(record), "%02x%02x%02x%02x%02x%02x%02u",
             g_devices[i].mac[0], g_devices[i].mac[1], g_devices[i].mac[2],
             g_devices[i].mac[3], g_devices[i].mac[4], g_devices[i].mac[5],
             g_devices[i].channel);
    prefs.putString(key, record);
  }
  prefs.end();
}

// Loads the bounded registry without treating persisted devices as online until they announce.
void loadDeviceRegistry() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) return;
  if (prefs.getUChar("regVer", 0) != kDeviceRegistryVersion) { prefs.end(); return; }
  const uint8_t count = std::min<uint8_t>(prefs.getUChar("devCount", 0), uint8_t(kMaxDevices));
  for (uint8_t i = 0; i < count; ++i) {
    char key[6];
    char record[16] = {};
    snprintf(key, sizeof(key), "dev%u", unsigned(i));
    prefs.getString(key, record, sizeof(record));
    DeviceSlot candidate;
    if (!decodeDeviceRecord(record, candidate.mac, candidate.channel)) continue;
    g_devices[g_deviceCount++] = std::move(candidate);
  }
  g_focusDevice = std::min<size_t>(prefs.getUChar("focus", 0), g_deviceCount ? g_deviceCount - 1 : 0);
  g_targetAll = g_deviceCount > 1 && prefs.getBool("targetAll", false);
  prefs.end();
  for (size_t i = 0; i < g_deviceCount; ++i) loadDeviceAlias(g_devices[i]);
  if (g_deviceCount) {
    g_preferredChannel = g_devices[g_focusDevice].channel;
    g_model = g_devices[g_focusDevice].model;
    g_stateRev++;
    g_deviceRev++;
  }
}


DeviceSlot* rememberDevice(const uint8_t* mac, uint32_t now_ms) {
  if (!mac || isBroadcastMac(mac)) return nullptr;
  DeviceSlot* device = findDevice(mac);
  bool registryChanged = false;
  if (!device) {
    if (g_deviceCount >= kMaxDevices) {
      Serial.printf("[ESPNOW] controller registry full capacity=%u; ignored "
                    "%02x:%02x:%02x:%02x:%02x:%02x\n",
                    unsigned(kMaxDevices), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      return nullptr;
    }
    device = &g_devices[g_deviceCount++];
    memcpy(device->mac, mac, sizeof(device->mac));
    device->channel = g_channel;
    loadDeviceAlias(*device);
    g_stateRev++;
    g_deviceRev++;
    registryChanged = true;
  }
  if (device->channel != g_channel) {
    device->channel = g_channel;
    registryChanged = true;
  }
  device->lastSeen = now_ms;
  if (registryChanged) saveDeviceRegistry();
  return device;
}

bool ensurePeerRegistered(DeviceSlot& device) {
  if (device.peerRegistered) return true;
  if (esp_now_is_peer_exist(device.mac)) {
    device.peerRegistered = true;
    return true;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, device.mac, sizeof(device.mac));
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  const esp_err_t result = esp_now_add_peer(&peer);
  if (result == ESP_OK || esp_now_is_peer_exist(device.mac)) {
    device.peerRegistered = true;
  } else {
    Serial.printf("[ESPNOW] peer registration failed result=%d ch=%u radioCh=%u\n",
                  result, g_channel, currentRadioChannel());
  }
  return device.peerRegistered;
}

void handleCompleteMessage(uint8_t type, uint8_t id, const uint8_t* src,
                           const uint8_t* payload, size_t len, uint32_t now_ms);
void completeRequest();

// Copy a frame out of the callback queue without holding the lock during parsing.
bool popRxFrame(RxFrame& frame) {
  if (g_rxLock.test_and_set(std::memory_order_acquire)) return false;
  if (!g_rxCount) {
    g_rxLock.clear(std::memory_order_release);
    return false;
  }
  frame = g_rxQueue[g_rxRead];
  g_rxRead = (g_rxRead + 1) % kRxQueueSize;
  g_rxCount--;
  g_rxLock.clear(std::memory_order_release);
  return true;
}

void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (!mac || !data || len < kHeaderSize || len > int(kHeaderSize + kFragSize)) return;
  if (data[0] != kMagic || data[1] != kVersion) return;
  if (g_rxLock.test_and_set(std::memory_order_acquire)) {
    g_rxLockDrops.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (g_rxCount < kRxQueueSize) {
    RxFrame& frame = g_rxQueue[g_rxWrite];
    memcpy(frame.src, mac, sizeof(frame.src));
    memcpy(frame.data, data, len);
    frame.len = uint8_t(len);
    g_rxWrite = (g_rxWrite + 1) % kRxQueueSize;
    g_rxCount++;
  } else g_rxQueueDrops++;
  g_rxLock.clear(std::memory_order_release);
}

// Validate and reassemble one queued frame in loop context.
void processFrame(const RxFrame& frame, uint32_t now_ms) {
  const uint8_t* mac = frame.src;
  const uint8_t* data = frame.data;
  const uint8_t len = frame.len;
  const uint8_t type = data[2];
  const uint8_t id = data[3];
  const uint8_t idx = data[4];
  const uint8_t total = data[5];
  const uint8_t plen = len - kHeaderSize;
  if (type != kMsgResponse && type != kMsgPush && type != kMsgDiscover &&
      type != kMsgLive && type != kMsgAnnounce) return;
  const bool discoveryReply = type == kMsgAnnounce || type == kMsgDiscover;
  if (!g_channelLocked && !discoveryReply) return;
  if (g_channelLocked && !discoveryReply && !findDevice(mac)) return;
  if (type == kMsgAnnounce &&
      (!g_lastDiscoverySent || id != g_lastDiscoveryId ||
       now_ms - g_lastDiscoverySent > kRequestTimeoutMs)) return;
  if (type == kMsgDiscover &&
      (!g_lastDiscoverySent || now_ms - g_lastDiscoverySent > kRequestTimeoutMs)) return;
  if (type == kMsgResponse &&
      (!g_request.active || id != g_request.id ||
       memcmp(mac, g_request.target, sizeof(g_request.target)) != 0)) return;
  if (total < 1 || total > kMaxFrags || idx >= total || plen > kFragSize) return;
  if (discoveryReply && (idx != 0 || total != 1)) return;
  if (idx < total - 1 && plen != kFragSize) return;

  if (g_active && now_ms - g_reasmLast > kReasmTimeoutMs) resetReasm();

  // A request/response exchange has priority over best-effort LIVE, PUSH, and discovery traffic.
  // Sharing one reassembly buffer otherwise lets an unrelated frame discard a partial response.
  if (g_request.active && g_request.awaitingResponse &&
      (type != kMsgResponse || id != g_request.id ||
       memcmp(mac, g_request.target, sizeof(g_request.target)) != 0)) {
    g_framesDeferredForResponse++;
    return;
  }
  // Once a state PUSH starts, let its remaining fragments finish before accepting best-effort
  // live/discovery traffic. A newer PUSH may still replace an incomplete older state update.
  if (g_active && g_curType == kMsgPush && type != kMsgPush) return;
  if (!g_active || g_curId != id || g_curType != type || g_curTotal != total ||
      (mac && memcmp(g_curMac, mac, sizeof(g_curMac)) != 0)) {
    if (g_active) {
      Serial.printf("[ESPNOW] RX replace incomplete %s id=%u fragments=%u/%u age=%lums\n",
                    messageTypeName(g_curType), g_curId, g_count, g_curTotal, now_ms - g_reasmLast);
    }
    if (idx != 0) {
      Serial.printf("[ESPNOW] RX orphan %s id=%u fragment=%u/%u ch=%u\n",
                    messageTypeName(type), id, idx + 1, total, g_channel);
      return;
    }
    g_active = true;
    g_curId = id;
    g_curType = type;
    g_curTotal = total;
    g_count = 0;
    g_flags = 0;
    g_len = 0;
    if (mac) memcpy(g_curMac, mac, sizeof(g_curMac));
  }
  g_reasmLast = now_ms;

  const uint64_t bit = uint64_t(1) << idx;
  if (g_flags & bit) return;
  memcpy(g_reasm + size_t(idx) * kFragSize, data + kHeaderSize, plen);
  g_flags |= bit;
  g_count++;
  if (idx == total - 1) g_len = size_t(idx) * kFragSize + plen;
  if (g_count < total) return;

  if (g_len <= kMaxJson) {
    if (g_curType != kMsgLive) {
      Serial.printf("[ESPNOW] RX complete %s id=%u bytes=%u fragments=%u ch=%u\n",
                    messageTypeName(g_curType), g_curId, unsigned(g_len), g_curTotal, g_channel);
    }
    handleCompleteMessage(g_curType, g_curId, g_curMac, g_reasm, g_len, now_ms);
  }
  resetReasm();
}

bool sendFrameTo(const uint8_t* mac, uint8_t type, uint8_t id, const uint8_t* payload, size_t len) {
  size_t total = len ? (len + kFragSize - 1) / kFragSize : 1;
  if (total > kMaxFrags) return false;
  uint8_t frame[kHeaderSize + kFragSize];
  frame[0] = kMagic;
  frame[1] = kVersion;
  frame[2] = type;
  frame[3] = id;
  frame[5] = uint8_t(total);
  for (size_t i = 0; i < total; i++) {
    size_t off = i * kFragSize;
    size_t chunk = len > off ? len - off : 0;
    if (chunk > kFragSize) chunk = kFragSize;
    frame[4] = uint8_t(i);
    if (chunk) memcpy(frame + kHeaderSize, payload + off, chunk);
    if (esp_now_send(mac, frame, kHeaderSize + chunk) != ESP_OK) return false;
    if (total > 1) delay(4);  // pace fragmented sends against the small TX queue
  }
  return true;
}

void clearRequests() {
  g_request = ActiveRequest{};
  g_transactionRead = g_transactionWrite = g_transactionCount = 0;
}

int deviceIndexForMac(const uint8_t* mac) {
  if (!mac) return -1;
  for (size_t i = 0; i < g_deviceCount; ++i) {
    if (memcmp(g_devices[i].mac, mac, sizeof(g_devices[i].mac)) == 0) return int(i);
  }
  return -1;
}

uint8_t onlineTargetMask() {
  if (!g_deviceCount) return 0;
  if (!g_targetAll) return uint8_t(1U << g_focusDevice);
  uint8_t mask = 0;
  for (size_t i = 0; i < g_deviceCount; ++i) {
    if (g_devices[i].channel == g_channel && g_devices[i].model.online) mask |= uint8_t(1U << i);
  }
  return mask;
}

uint8_t targetCount(uint8_t mask) {
  uint8_t count = 0;
  while (mask) { count += mask & 1U; mask >>= 1; }
  return count;
}

// Adds the destination controller's main segment ID to a compact segment command.
bool targetMainSegment(const char* json, uint8_t segmentId, char* output, size_t outputSize) {
  static constexpr char segmentPrefix[] = "\"seg\":[{";
  const char* segment = strstr(json, segmentPrefix);
  if (!segment) return false;

  const size_t insertOffset = size_t(segment - json) + sizeof(segmentPrefix) - 1;
  const int length = snprintf(output, outputSize, "%.*s\"id\":%u,%s",
                              int(insertOffset), json, unsigned(segmentId), json + insertOffset);
  return length > 0 && size_t(length) < outputSize;
}

// Stores one command once and lets the scheduler visit every target. No group fan-out can
// exhaust the queue halfway through an operation.
bool queueTransaction(const char* json, RequestKey key, bool retryable, uint8_t targets,
                      bool reconcile, bool targetMain = false) {
  if (!json || !targets) return false;
  const size_t len = strnlen(json, kMaxRequestLength);
  if (!len || len >= kMaxRequestLength) return false;

  // Coalesce gestures that have not started. The front transaction is immutable while its
  // active target is in flight so every member of a group receives the same command value.
  if (key != RequestKey::None) {
    for (uint8_t i = 0; i < g_transactionCount; i++) {
      QueuedTransaction& queued = g_transactions[(g_transactionRead + i) % kTransactionQueueSize];
      if (queued.started) continue;
      if (queued.key == key && queued.targets == targets && queued.targetMainSegment == targetMain) {
        memcpy(queued.json, json, len + 1);
        queued.retryable = retryable;
        queued.reconcileTargets = reconcile ? targets : 0;
        return true;
      }
      if (queued.key == key && queued.retryable == retryable &&
          queued.targetMainSegment == targetMain &&
          strcmp(queued.json, json) == 0) {
        queued.targets |= targets;
        if (reconcile) queued.reconcileTargets |= targets;
        return true;
      }
    }
  }
  if (g_transactionCount >= kTransactionQueueSize) {
    Serial.printf("[ESPNOW] transaction queue full key=%s targets=0x%02x depth=%u\n",
                  requestKeyName(key), targets, g_transactionCount);
    return false;
  }

  QueuedTransaction& queued = g_transactions[g_transactionWrite];
  queued = QueuedTransaction{};
  memcpy(queued.json, json, len + 1);
  queued.key = key;
  queued.retryable = retryable;
  queued.targetMainSegment = targetMain;
  queued.targets = targets;
  queued.reconcileTargets = reconcile ? targets : 0;
  g_transactionWrite = (g_transactionWrite + 1) % kTransactionQueueSize;
  g_transactionCount++;
  return true;
}

bool queueRequest(const char* json, RequestKey key, bool retryable = true,
                  const uint8_t* target = nullptr) {
  if (!target) {
    if (!g_deviceCount) return false;
    target = g_devices[g_focusDevice].mac;
  }
  const int index = deviceIndexForMac(target);
  if (index < 0) return false;
  return queueTransaction(json, key, retryable, uint8_t(1U << index), false);
}

// Enqueues one transaction for the selection; group reconciliation is generated lazily.
bool queueForTargets(const char* json, RequestKey key, bool retryable = true,
                     bool targetMain = false) {
  const uint8_t targets = onlineTargetMask();
  const bool reconcile = targetCount(targets) > 1 && key != RequestKey::Poll &&
                         key != RequestKey::Live && key != RequestKey::None;
  return queueTransaction(json, key, retryable, targets, reconcile, targetMain);
}

void completeRequest() {
  if (g_framesDeferredForResponse) {
    Serial.printf("[ESPNOW] request complete key=%s id=%u deferredFrames=%lu\n",
                  requestKeyName(g_request.key), g_request.id, g_framesDeferredForResponse);
  }
  if (g_request.key == RequestKey::PresetSave) {
    g_presetCatalogRefreshAt = millis() + 750;
    memcpy(g_presetCatalogRefreshMac, g_request.target, sizeof(g_presetCatalogRefreshMac));
  }
  g_framesDeferredForResponse = 0;
  g_request = ActiveRequest{};
}

void retryRequest(uint32_t now_ms) {
  g_request.awaitingResponse = false;
  g_request.nextAttempt = now_ms + kRequestRetryMs;
}

void markRequestTargetOffline() {
  const int index = deviceIndexForMac(g_request.target);
  if (index < 0) return;
  DeviceSlot& device = g_devices[index];
  if (device.model.online) {
    device.model.online = false;
    if (size_t(index) == g_focusDevice) g_model = device.model;
    g_stateRev++;
    g_deviceRev++;
  }
  if (g_transactionCount) {
    g_transactions[g_transactionRead].reconcileTargets &= uint8_t(~(1U << index));
  }
}

// Loads the next target from the front transaction, then lazily enters reconciliation phase.
bool startNextRequest() {
  while (g_transactionCount) {
    QueuedTransaction& transaction = g_transactions[g_transactionRead];
    const bool reconciling = transaction.targets == 0;
    uint8_t& targets = reconciling ? transaction.reconcileTargets : transaction.targets;
    if (!targets) {
      transaction = QueuedTransaction{};
      g_transactionRead = (g_transactionRead + 1) % kTransactionQueueSize;
      g_transactionCount--;
      continue;
    }
    uint8_t index = 0;
    while (!(targets & uint8_t(1U << index))) index++;
    targets &= uint8_t(~(1U << index));
    transaction.started = true;
    memcpy(g_request.target, g_devices[index].mac, sizeof(g_request.target));
    if (reconciling) {
      memcpy(g_request.json, "{\"v\":\"compact\"}", sizeof("{\"v\":\"compact\"}"));
    } else if (transaction.targetMainSegment) {
      if (!targetMainSegment(transaction.json, g_devices[index].model.mainSegmentId,
                             g_request.json, sizeof(g_request.json))) {
        Serial.printf("[ESPNOW] invalid main-segment command key=%s\n", requestKeyName(transaction.key));
        continue;
      }
    } else {
      memcpy(g_request.json, transaction.json, strlen(transaction.json) + 1);
    }
    g_request.key = reconciling ? RequestKey::Poll : transaction.key;
    g_request.retryable = reconciling ? true : transaction.retryable;
    g_request.active = true;
    g_request.id = g_msgId++;
    g_framesDeferredForResponse = 0;
    return true;
  }
  return false;
}

void pumpRequests(uint32_t now_ms) {
  if (!g_channelLocked || !g_deviceCount) return;

  if (!g_request.active) startNextRequest();
  if (!g_request.active) return;

  if (g_request.awaitingResponse) {
    if (now_ms - g_request.sentAt < kRequestTimeoutMs) return;
    Serial.printf("[ESPNOW] request timeout key=%s id=%u attempt=%u age=%lums linkAge=%lums lockedCh=%u radioCh=%u deferredFrames=%lu\n",
                  requestKeyName(g_request.key), g_request.id, g_request.attempts,
                  now_ms - g_request.sentAt, now_ms - g_lastLinkRx, g_channel,
                  currentRadioChannel(), g_framesDeferredForResponse);
    if (!g_request.retryable || g_request.attempts >= kMaxRequestAttempts) {
      if (g_request.retryable) markRequestTargetOffline();
      completeRequest();
      return;
    }
    retryRequest(now_ms);
  }
  if (now_ms < g_request.nextAttempt) return;

  const size_t len = strlen(g_request.json);
  Serial.printf("[ESPNOW] TX request key=%s id=%u attempt=%u bytes=%u lockedCh=%u radioCh=%u queued=%u\n",
                requestKeyName(g_request.key), g_request.id, g_request.attempts + 1,
                unsigned(len), g_channel, currentRadioChannel(), g_transactionCount);
  if (sendFrameTo(g_request.target, kMsgRequest, g_request.id,
                  reinterpret_cast<const uint8_t*>(g_request.json), len)) {
    if (g_request.key == RequestKey::Live) {
      completeRequest(); // subscription refresh is best-effort and must never block controls
      return;
    }
    g_request.awaitingResponse = true;
    g_request.sentAt = now_ms;
    g_request.attempts++;
  } else {
    Serial.printf("[ESPNOW] TX request immediate failure key=%s id=%u ch=%u\n",
                  requestKeyName(g_request.key), g_request.id, g_channel);
    g_request.attempts++;
    if (!g_request.retryable || g_request.attempts >= kMaxRequestAttempts) {
      if (g_request.retryable) markRequestTargetOffline();
      completeRequest();
    } else {
      g_request.nextAttempt = now_ms + kRequestRetryMs;
    }
  }
}

void setRadioChannel(uint8_t ch) {
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
}

void sendDiscovery(uint32_t now_ms) {
  g_lastDiscoveryId = g_msgId++;
  g_lastDiscoverySent = now_ms;
  sendFrameTo(kBroadcastMac, kMsgDiscover, g_lastDiscoveryId, nullptr, 0);
}

void hopChannel(uint32_t now_ms) {
  if (!g_preferredChannelTried && g_preferredChannel >= 1 && g_preferredChannel <= kMaxChannel) {
    g_channel = g_preferredChannel;
    g_preferredChannelTried = true;
  } else {
    g_channel = (g_channel % kMaxChannel) + 1;
  }
  setRadioChannel(g_channel);
  sendDiscovery(now_ms);
  Serial.printf("[ESPNOW] discovery probe ch=%u discoveryId=%u rxDrops=%lu\n",
                g_channel, g_lastDiscoveryId, g_rxQueueDrops.load(std::memory_order_relaxed));
  g_lastHop = now_ms;
}

uint32_t colorFromArray(JsonArrayConst col, uint32_t fallback) {
  if (col.isNull() || col.size() < 3) return fallback;
  uint8_t r = col[0] | 0, g = col[1] | 0, b = col[2] | 0;
  return (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

void applyState(Model& model, JsonObjectConst state, JsonObjectConst info) {
  if (!state.isNull()) {
    if (state["on"].is<bool>()) model.power = state["on"].as<bool>();
    if (state["bri"].is<int>()) model.brightness = state["bri"].as<int>();
    if (state["ps"].is<int>()) model.preset = state["ps"].as<int>();
    JsonArrayConst segs = state["seg"].as<JsonArrayConst>();
    if (!segs.isNull() && segs.size() > 0) {
      int mainSegmentId = state["mainseg"].is<int>() ? state["mainseg"].as<int>() : model.mainSegmentId;
      if (mainSegmentId < 0 || mainSegmentId > UINT8_MAX) mainSegmentId = model.mainSegmentId;

      JsonObjectConst seg;
      for (JsonObjectConst candidate : segs) {
        if (candidate["id"].is<int>() && candidate["id"].as<int>() == mainSegmentId) {
          seg = candidate;
          break;
        }
      }
      if (seg.isNull()) {
        // Older compact responses omitted segment IDs and used the array position instead.
        size_t mainIdx = size_t(mainSegmentId);
        if (mainIdx >= segs.size()) mainIdx = 0;
        seg = segs[mainIdx].as<JsonObjectConst>();
      }
      if (seg["id"].is<int>()) {
        const int segmentId = seg["id"].as<int>();
        if (segmentId >= 0 && segmentId <= UINT8_MAX) model.mainSegmentId = uint8_t(segmentId);
      } else {
        model.mainSegmentId = uint8_t(mainSegmentId);
      }
      if (seg["fx"].is<int>()) model.effect = seg["fx"].as<int>();
      if (seg["pal"].is<int>()) model.palette = seg["pal"].as<int>();
      if (seg["sx"].is<int>()) model.speed = seg["sx"].as<int>();
      if (seg["ix"].is<int>()) model.intensity = seg["ix"].as<int>();
      if (seg["c1"].is<int>()) model.custom1 = seg["c1"].as<int>();
      if (seg["c2"].is<int>()) model.custom2 = seg["c2"].as<int>();
      if (seg["c3"].is<int>()) model.custom3 = seg["c3"].as<int>();
      JsonArrayConst col = seg["col"].as<JsonArrayConst>();
      if (!col.isNull() && col.size() > 0) {
        model.color = colorFromArray(col[0].as<JsonArrayConst>(), model.color);
      }
    }
  }
  if (!info.isNull()) {
    const char* name = info["name"].as<const char*>();
    if (name) model.name = name;
  }
  model.online = true;
}

struct ParseResult {
  bool valid = false;
  int error = -1;
};

ParseResult parseInbox(DeviceSlot& device, uint8_t msgType, const uint8_t* data, size_t len) {
  ParseResult result;
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) return result;

  const bool modernAnnouncement = doc["announce"].is<JsonObject>();
  const bool legacyAnnouncement = doc["hello"].is<JsonObject>();
  const bool statePayload = doc["state"].is<JsonObject>();
  const bool catalogPayload = doc["presets"].is<JsonObject>();
  const bool resultPayload = doc["success"].is<bool>() || doc["error"].is<int>();
  if ((msgType == kMsgAnnounce && !modernAnnouncement) ||
      (msgType == kMsgDiscover && !legacyAnnouncement) ||
      (msgType == kMsgPush && !statePayload) ||
      (msgType == kMsgResponse && !statePayload && !catalogPayload && !resultPayload)) return result;

  if (modernAnnouncement || legacyAnnouncement) {
    JsonObjectConst announce = modernAnnouncement
                                   ? doc["announce"].as<JsonObjectConst>()
                                   : doc["hello"].as<JsonObjectConst>();
    if (modernAnnouncement &&
        ((!announce["proto"].is<int>() || announce["proto"].as<int>() != kVersion) ||
         (!announce["ch"].is<int>() || announce["ch"].as<int>() != g_channel))) return result;
    const char* name = announce["name"].as<const char*>();
    if (name && strnlen(name, kMaxDeviceAliasLength + 1) > kMaxDeviceAliasLength) return result;
    const bool changed = !device.model.online || (name && device.model.name != name);
    if (name) device.model.name = name;
    if (modernAnnouncement && announce["cap"].is<int>()) device.capabilities = announce["cap"].as<uint8_t>();
    device.model.online = true;
    if (changed) {
      g_stateRev++;
      g_deviceRev++;
    }
    result.valid = true;
  } else if (doc["state"].is<JsonObject>()) {
    const bool wasOffline = !device.model.online;
    applyState(device.model, doc["state"].as<JsonObjectConst>(), doc["info"].as<JsonObjectConst>());
    g_stateRev++;
    if (wasOffline) g_deviceRev++;
    result.valid = true;
  } else if (doc["presets"].is<JsonObject>()) {
    if (!device.model.online) {
      device.model.online = true;
      g_stateRev++;
      g_deviceRev++;
    }
    device.model.presets.clear();
    for (JsonPairConst kv : doc["presets"].as<JsonObjectConst>()) {
      const char* s = kv.value().as<const char*>();
      device.model.presets.push_back({uint8_t(atoi(kv.key().c_str())), s ? s : ""});
    }
    std::sort(device.model.presets.begin(), device.model.presets.end(),
              [](const PresetInfo& a, const PresetInfo& b) { return a.id < b.id; });
    g_presetsLoaded = true;
    g_catalogRev++;
    result.valid = true;
  } else if (doc["success"].is<bool>() || doc["error"].is<int>()) {
    if (!device.model.online) {
      device.model.online = true;
      g_stateRev++;
      g_deviceRev++;
    }
    result.valid = true;
    if (doc["error"].is<int>()) result.error = doc["error"].as<int>();
  }
  return result;
}

bool decodeLive(const uint8_t* data, size_t len) {
  if (len < 2 || data[0] != 'L') return false;
  size_t pos = 2;
  if (data[1] == 2) {  // 2D frame: width/height follow the version byte
    if (len < 4) return false;
    g_liveW = data[2];
    g_liveH = data[3];
    pos = 4;
  } else {
    g_liveW = 0;
    g_liveH = 0;
  }
  size_t n = (len - pos) / 3;
  if (n > sizeof(g_live) / 3) n = sizeof(g_live) / 3;
  memcpy(g_live, data + pos, n * 3);
  g_liveCount = uint16_t(n);
  g_liveRev++;
  return true;
}

void loadFallbackPresets(DeviceSlot& device) {
  device.model.presets.clear();
  for (uint8_t id = 1; id <= kExtendedPresetCount; id++)
    device.model.presets.push_back({id, std::string("Preset ") + std::to_string(id)});
  g_presetsLoaded = true;
  g_catalogRev++;
}

// Completed messages are handled synchronously by loop(), so the large reassembly buffer can
// be reused without a second 8 KB inbox and without any callback/main-loop data race.
void handleCompleteMessage(uint8_t type, uint8_t id, const uint8_t* src,
                           const uint8_t* payload, size_t len, uint32_t now_ms) {
  DeviceSlot* device = findDevice(src);
  const bool newDevice = !device;
  DeviceSlot discoveredDevice;
  if ((type == kMsgAnnounce || type == kMsgDiscover) && !device) {
    memcpy(discoveredDevice.mac, src, sizeof(discoveredDevice.mac));
    discoveredDevice.channel = g_channel;
    discoveredDevice.lastSeen = now_ms;
    device = &discoveredDevice;
  }
  if (!device) return;

  if (type == kMsgLive) {
    if (device == &g_devices[g_focusDevice] && decodeLive(payload, len)) {
      device->lastSeen = now_ms;
      g_lastLinkRx = now_ms;
      g_lastRx = now_ms;
      g_lastLiveRx = now_ms;
    }
    return;
  }

  const ParseResult result = parseInbox(*device, type, payload, len);
  if (!result.valid) return;
  device->lastSeen = now_ms;
  g_lastLinkRx = now_ms;
  if (newDevice) {
    DeviceSlot* storedDevice = rememberDevice(src, now_ms);
    if (!storedDevice) return;
    storedDevice->model = std::move(device->model);
    storedDevice->capabilities = device->capabilities;
    device = storedDevice;
  }
  if ((type == kMsgAnnounce || type == kMsgDiscover) && newDevice && g_channelLocked) {
    if (ensurePeerRegistered(*device)) {
      Serial.printf("[ESPNOW] discovered newly linked WLED %02x:%02x:%02x:%02x:%02x:%02x ch=%u devices=%u\n",
                    device->mac[0], device->mac[1], device->mac[2], device->mac[3],
                    device->mac[4], device->mac[5], g_channel, unsigned(g_deviceCount));
      queueRequest("{\"v\":\"compact\"}", RequestKey::Poll, true, device->mac);
    }
  }
  g_lastRx = now_ms;
  if (device == &g_devices[g_focusDevice]) g_model = device->model;
  if ((type == kMsgAnnounce || type == kMsgDiscover) && !g_channelLocked) {
    if (!g_discoveryFirstReply) g_discoveryFirstReply = now_ms;
  }
  if (type == kMsgResponse && g_request.active && id == g_request.id &&
      memcmp(src, g_request.target, sizeof(g_request.target)) == 0) {
    if (result.error == 3) {
      if (g_request.attempts >= kMaxRequestAttempts) completeRequest();
      else retryRequest(now_ms);
    }
    else {
      if (g_request.key == RequestKey::Catalog && result.error == 8) loadFallbackPresets(*device);
      else if (g_request.key == RequestKey::Catalog && result.error >= 8) g_presetsLoaded = true;
      completeRequest();
    }
  }
}

}  // namespace

void begin() {
  loadDeviceRegistry();
  esp_now_register_recv_cb(onRecv);
}

void loop(uint32_t now_ms) {
  static uint32_t reportedRxDrops = 0;
  static uint32_t reportedLockDrops = 0;
  RxFrame frame;
  for (uint8_t i = 0; i < 6 && popRxFrame(frame); i++) processFrame(frame, now_ms);
  now_ms = millis(); // timer comparisons use a timestamp no older than processed RX timestamps
  const uint32_t rxQueueDrops = g_rxQueueDrops.load(std::memory_order_relaxed);
  if (reportedRxDrops != rxQueueDrops) {
    Serial.printf("[ESPNOW] RX queue overflow drops=%lu depth=%u capacity=%u ch=%u\n",
                  rxQueueDrops, g_rxCount, unsigned(kRxQueueSize), g_channel);
    reportedRxDrops = rxQueueDrops;
  }
  const uint32_t rxLockDrops = g_rxLockDrops.load(std::memory_order_relaxed);
  if (reportedLockDrops != rxLockDrops) {
    Serial.printf("[ESPNOW] RX callback lock contention drops=%lu depth=%u ch=%u\n",
                  rxLockDrops, g_rxCount, g_channel);
    reportedLockDrops = rxLockDrops;
  }
  if (g_active && now_ms - g_reasmLast > kReasmTimeoutMs) {
    Serial.printf("[ESPNOW] RX reassembly timeout %s id=%u fragments=%u/%u age=%lums ch=%u\n",
                  messageTypeName(g_curType), g_curId, g_count, g_curTotal,
                  now_ms - g_reasmLast, g_channel);
    resetReasm();
  }

  for (size_t i = 0; i < g_deviceCount; ++i) {
    DeviceSlot& device = g_devices[i];
    if (device.model.online && device.lastSeen && now_ms - device.lastSeen > kOfflineMs) {
      device.model.online = false;
      if (i == g_focusDevice) g_model = device.model;
      g_stateRev++;
      g_deviceRev++;
    }
  }

  // Keep collecting staggered ANNOUNCE replies before locking the radio to this device group.
  if (!g_channelLocked && g_discoveryFirstReply &&
      now_ms - g_discoveryFirstReply >= kDiscoveryCollectMs) {
    size_t registered = 0;
    for (size_t i = 0; i < g_deviceCount; ++i) {
      if (g_devices[i].channel == g_channel && g_devices[i].model.online &&
          ensurePeerRegistered(g_devices[i])) registered++;
    }
    if (registered) {
      if (g_focusDevice >= g_deviceCount || g_devices[g_focusDevice].channel != g_channel ||
          !g_devices[g_focusDevice].model.online) {
        for (size_t i = 0; i < g_deviceCount; ++i) {
          if (g_devices[i].channel == g_channel && g_devices[i].model.online) { g_focusDevice = i; break; }
        }
        g_targetAll = false;
      }
      g_channelLocked = true;
      g_preferredChannel = g_channel;
      g_lastLinkRx = now_ms;
      g_lastHeartbeat = g_lastPoll = g_lastCatalog = now_ms;
      g_catalogTries = 1;
      g_discoveryFirstReply = 0;
      g_model = g_devices[g_focusDevice].model;
      saveDeviceRegistry();
      Serial.printf("[ESPNOW] LOCKED ch=%u devices=%u radioCh=%u rxDrops=%lu\n",
                    g_channel, unsigned(registered), currentRadioChannel(), rxQueueDrops);
      for (size_t i = 0; i < g_deviceCount; ++i) {
        if (g_devices[i].channel == g_channel && g_devices[i].model.online) {
          queueRequest("{\"v\":\"compact\"}", RequestKey::Poll, true, g_devices[i].mac);
        }
      }
      requestCatalogs();
    }
  }
  if (g_channelLocked && g_lastLinkRx && now_ms - g_lastLinkRx > kOfflineMs) {
    Serial.printf("[ESPNOW] OFFLINE linkAge=%lums completeAge=%lums lockedCh=%u radioCh=%u request=%s id=%u attempt=%u rxDrops=%lu\n",
                  now_ms - g_lastLinkRx, now_ms - g_lastRx, g_channel, currentRadioChannel(),
                  g_request.active ? requestKeyName(g_request.key) : "none",
                  g_request.id, g_request.attempts, rxQueueDrops);
    for (size_t i = 0; i < g_deviceCount; ++i) {
      if (g_devices[i].peerRegistered) esp_now_del_peer(g_devices[i].mac);
      g_devices[i].peerRegistered = false;
    }
    g_channelLocked = false;
    g_preferredChannel = g_channel;
    g_preferredChannelTried = false;
    g_discoveryFirstReply = 0;
    g_presetsLoaded = false;
    g_presetCatalogRefreshAt = 0;
    clearRequests();
    resetReasm();
  }
  if (!g_channelLocked && !g_discoveryFirstReply && now_ms - g_lastHop > kHopDwellMs) {
    hopChannel(now_ms);
  }

  if (g_channelLocked) {
    if (g_presetCatalogRefreshAt && int32_t(now_ms - g_presetCatalogRefreshAt) >= 0) {
      g_presetCatalogRefreshAt = 0;
      g_lastCatalog = now_ms;
      queueRequest("{\"get\":\"ps\"}", RequestKey::CatalogRefresh, true,
                   g_presetCatalogRefreshMac);
    }
    if (!g_request.active && !g_transactionCount &&
        (g_discoveryRequested || now_ms - g_lastHeartbeat > kHeartbeatMs)) {
      sendDiscovery(now_ms);
      g_lastHeartbeat = now_ms;
      g_discoveryRequested = false;
    }
    if (now_ms - g_lastPoll > kResyncMs) {
      poll();
      g_lastPoll = now_ms;
    }
    if (!g_presetsLoaded && g_catalogTries < kCatalogTries && now_ms - g_lastCatalog > 5000) {
      requestCatalogs();
      g_catalogTries++;
    }
  }

  if (g_channelLocked && g_liveWanted && now_ms - g_liveArmed >= kLiveReArmMs) {
    queueRequest("{\"lv\":true}", RequestKey::Live);
    g_liveArmed = now_ms;
  }
  pumpRequests(now_ms);
}

const Model& model() { return g_model; }
bool online() { return g_model.online; }
uint8_t radioChannel() { return g_channelLocked ? g_channel : 0; }
size_t deviceCount() { return g_deviceCount; }
size_t activeDeviceCount() {
  size_t count = 0;
  for (size_t i = 0; i < g_deviceCount; ++i) {
    if (g_devices[i].channel == g_channel && g_devices[i].model.online) count++;
  }
  return count;
}
DeviceInfo deviceInfo(size_t index) {
  DeviceInfo info = {};
  if (index >= g_deviceCount) return info;
  const DeviceSlot& device = g_devices[index];
  memcpy(info.mac, device.mac, sizeof(info.mac));
  info.channel = device.channel;
  info.online = device.model.online;
  info.name = device.alias[0] ? device.alias : device.model.name;
  return info;
}
size_t focusedDevice() { return g_focusDevice; }
bool targetingAll() { return g_targetAll; }

uint16_t mixedStateMask() {
  if (!g_targetAll) return kMixedNone;
  const DeviceSlot* reference = nullptr;
  uint16_t mixed = kMixedNone;
  for (size_t i = 0; i < g_deviceCount; ++i) {
    const DeviceSlot& device = g_devices[i];
    if (device.channel != g_channel || !device.model.online) continue;
    if (!reference) { reference = &device; continue; }
    if (device.model.power != reference->model.power) mixed |= kMixedPower;
    if (device.model.brightness != reference->model.brightness) mixed |= kMixedBrightness;
    if (device.model.effect != reference->model.effect) mixed |= kMixedEffect;
    if (device.model.palette != reference->model.palette) mixed |= kMixedPalette;
    if (device.model.preset != reference->model.preset) mixed |= kMixedPreset;
    if (device.model.color != reference->model.color) mixed |= kMixedColor;
  }
  return mixed;
}

void selectDevice(size_t index) {
  if (index >= g_deviceCount) return;
  if (g_channelLocked && g_devices[index].channel != g_channel) return;
  const size_t previous = g_focusDevice;
  if (g_liveWanted && previous < g_deviceCount && previous != index) {
    queueRequest("{\"lv\":false}", RequestKey::Live, true, g_devices[previous].mac);
  }
  g_focusDevice = index;
  g_targetAll = false;
  g_model = g_devices[index].model;
  g_presetsLoaded = !g_model.presets.empty();
  g_liveCount = 0;
  g_lastLiveRx = 0;
  g_stateRev++;
  g_catalogRev++;
  g_preferredChannel = g_devices[index].channel;
  saveDeviceRegistry();
  if (g_liveWanted) queueRequest("{\"lv\":true}", RequestKey::Live, true, g_devices[index].mac);
  requestCatalogs();
}

void selectAll() {
  if (!g_deviceCount) return;
  g_targetAll = true;
  g_stateRev++;
  saveDeviceRegistry();
}

// Assigns a persistent local controller name; an empty name restores WLED's own name.
void renameDevice(size_t index, const char* name) {
  if (index >= g_deviceCount || !name) return;
  const size_t length = strnlen(name, kMaxDeviceAliasLength + 1);
  if (length > kMaxDeviceAliasLength) return;
  memcpy(g_devices[index].alias, name, length);
  g_devices[index].alias[length] = '\0';
  saveDeviceAlias(g_devices[index]);
  g_stateRev++;
  g_deviceRev++;
}


// Removes an unlinked/offline controller so stale registry entries cannot exhaust capacity.
bool forgetDevice(size_t index) {
  if (index >= g_deviceCount || g_devices[index].model.online) return false;

  g_devices[index].alias[0] = '\0';
  saveDeviceAlias(g_devices[index]);
  const size_t previousCount = g_deviceCount;
  for (size_t i = index; i + 1 < g_deviceCount; ++i) g_devices[i] = std::move(g_devices[i + 1]);
  g_devices[--g_deviceCount] = DeviceSlot{};

  if (!g_deviceCount) {
    g_focusDevice = 0;
    g_targetAll = false;
    g_model = Model{};
  } else {
    if (g_focusDevice > index) g_focusDevice--;
    else if (g_focusDevice == index) g_focusDevice = std::min(index, g_deviceCount - 1);
    if (g_deviceCount < 2) g_targetAll = false;
    g_model = g_devices[g_focusDevice].model;
  }

  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    char key[6];
    snprintf(key, sizeof(key), "dev%u", unsigned(previousCount - 1));
    prefs.remove(key);
    prefs.end();
  }
  saveDeviceRegistry();
  g_stateRev++;
  g_catalogRev++;
  g_deviceRev++;
  return true;
}

void scanNow() { g_discoveryRequested = true; }
uint32_t stateRevision() { return g_stateRev; }
uint32_t catalogRevision() { return g_catalogRev; }
uint32_t liveRevision() { return g_liveRev; }
uint32_t deviceRevision() { return g_deviceRev; }
bool livePeekEnabled() { return g_liveWanted; }

const uint8_t* liveLeds(uint16_t& ledCount, uint16_t& width, uint16_t& height) {
  ledCount = g_liveCount;
  width = g_liveW;
  height = g_liveH;
  return g_liveCount ? g_live : nullptr;
}

uint32_t liveFrameAgeMs(uint32_t now_ms) {
  if (!g_lastLiveRx) return UINT32_MAX;
  return now_ms - g_lastLiveRx;
}

void poll() { queueForTargets("{\"v\":\"compact\"}", RequestKey::Poll); }

void requestCatalogs() {
  g_lastCatalog = millis();
  queueRequest("{\"get\":\"ps\"}", RequestKey::Catalog);
}

void setPower(bool on) {
  queueForTargets(on ? "{\"on\":true,\"v\":true}" : "{\"on\":false,\"v\":true}", RequestKey::Power);
}
void togglePower() { setPower(!g_model.power); }

void setBrightness(uint8_t bri) {
  char buf[40];
  snprintf(buf, sizeof(buf), "{\"bri\":%u,\"v\":true}", bri);
  queueForTargets(buf, RequestKey::Brightness);
}

void applyPreset(uint8_t id) {
  char buf[40];
  snprintf(buf, sizeof(buf), "{\"ps\":%u,\"v\":true}", id);
  queueForTargets(buf, RequestKey::Preset);
}


// Saves WLED's current state to a preset and refreshes the instance-specific preset catalog.
void savePreset(uint8_t id, const char* name) {
  if (id < 1 || id > 250 || !name || !name[0]) return;

  JsonDocument doc;
  doc["psave"] = id;
  doc["n"] = name;
  doc["ib"] = true;
  doc["sb"] = true;
  char buf[kMaxRequestLength];
  const size_t len = serializeJson(doc, buf, sizeof(buf));
  if (!len || len >= sizeof(buf)) return;

  queueRequest(buf, RequestKey::PresetSave);
}


void setEffect(uint8_t fxId) {
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"seg\":[{\"fx\":%u}],\"v\":true}", fxId);
  if (!queueForTargets(buf, RequestKey::Effect, true, true)) return;
  g_model.effect = fxId; // optimistic model update prevents unrelated revisions restoring stale state
  if (g_targetAll) {
    for (size_t i = 0; i < g_deviceCount; ++i) {
      if (g_devices[i].channel == g_channel && g_devices[i].model.online)
        g_devices[i].model.effect = fxId;
    }
  } else if (g_deviceCount) g_devices[g_focusDevice].model.effect = fxId;
}

void setPalette(uint8_t palId) {
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"seg\":[{\"pal\":%u}],\"v\":true}", palId);
  queueForTargets(buf, RequestKey::Palette, true, true);
}

void setColor(uint8_t r, uint8_t g, uint8_t b) {
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"seg\":[{\"col\":[[%u,%u,%u]]}],\"v\":true}", r, g, b);
  if (!queueForTargets(buf, RequestKey::Color, true, true)) return;
  g_model.color = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
  if (g_targetAll) {
    for (size_t i = 0; i < g_deviceCount; ++i) {
      if (g_devices[i].channel == g_channel && g_devices[i].model.online)
        g_devices[i].model.color = g_model.color;
    }
  } else if (g_deviceCount) g_devices[g_focusDevice].model.color = g_model.color;
}

void setEffectParams(int speed, int intensity) {
  char seg[64];
  int n = snprintf(seg, sizeof(seg), "{\"seg\":[{");
  bool comma = false;
  if (speed >= 0) { n += snprintf(seg + n, sizeof(seg) - n, "\"sx\":%d", speed); comma = true; }
  if (intensity >= 0) { n += snprintf(seg + n, sizeof(seg) - n, "%s\"ix\":%d", comma ? "," : "", intensity); }
  snprintf(seg + n, sizeof(seg) - n, "}],\"v\":true}");
  queueForTargets(seg, RequestKey::EffectParams, true, true);
}

void setCustomParam(uint8_t index, uint8_t value) {
  if (index < 1 || index > 3) return;
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"seg\":[{\"c%u\":%u}],\"v\":true}", index, value);
  const RequestKey key = index == 1 ? RequestKey::Custom1 :
                         index == 2 ? RequestKey::Custom2 : RequestKey::Custom3;
  queueForTargets(buf, key, true, true);
}

void sendRaw(const char* json) { queueForTargets(json, RequestKey::None, false); }

void setLivePeek(bool on) {
  g_liveWanted = on;
  g_liveArmed = 0;
  if (on) {
    if (g_channelLocked) queueRequest("{\"lv\":true}", RequestKey::Live);
  }
  else {
    if (g_channelLocked) queueRequest("{\"lv\":false}", RequestKey::Live);
    g_liveCount = 0;
    g_lastLiveRx = 0;
    g_liveRev++;
  }
}

}  // namespace wled
