#include "wled_api.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <atomic>
#include <cstring>

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
constexpr uint8_t kMsgHello = 0x04;
constexpr uint8_t kMsgLive = 0x05;

constexpr size_t kMaxJson = 8192;
constexpr uint8_t kMaxFrags = kMaxJson / kFragSize + 1;
constexpr uint32_t kLiveReArmMs = 10000;  // refresh the live subscription well inside WLED's 30 s lease
constexpr uint32_t kHeartbeatMs = 3000;   // cheap HELLO keepalive cadence while connected
constexpr uint32_t kResyncMs = 30000;     // periodic full state poll to correct any missed pushes
constexpr uint32_t kOfflineMs = 10000;    // no rx for this long => offline (must exceed kHeartbeatMs)
constexpr uint32_t kHopDwellMs = 150;     // time spent probing each channel while hunting for WLED
constexpr uint32_t kReasmTimeoutMs = 500;
constexpr uint32_t kRequestTimeoutMs = 1500;
constexpr uint32_t kRequestRetryMs = 120;
constexpr uint8_t  kMaxChannel = 13;
constexpr uint8_t  kCatalogTries = 6;     // give up refetching the preset list after this many attempts
constexpr uint8_t  kMaxRequestAttempts = 4;
constexpr size_t   kRxQueueSize = kMaxFrags; // hold one maximum fragmented message
constexpr size_t   kRequestQueueSize = 6;
constexpr size_t   kMaxRequestLength = 512;

Model g_model;
uint32_t g_stateRev = 0;
uint32_t g_catalogRev = 0;
uint32_t g_liveRev = 0;
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
uint8_t g_live[kMaxJson];
uint16_t g_liveCount = 0, g_liveW = 0, g_liveH = 0;

bool g_liveWanted = false;
uint32_t g_liveArmed = 0;

// ESP-NOW only receives on the channel the radio is tuned to, and WLED replies on its own WiFi
// channel. So we hop channels broadcasting HELLO until WLED answers, then lock on. This keeps the
// remote plug-and-play (no channel config) the way the send-only WizMote path was.
uint8_t g_channel = 0;
bool g_channelLocked = false;
uint32_t g_lastHop = 0;
bool g_sawHello = false;
uint8_t g_peerMac[6] = {};
bool g_hasPeer = false;
bool g_peerRegistered = false;
uint32_t g_lastHeartbeat = 0;
uint32_t g_lastPoll = 0;
uint32_t g_lastCatalog = 0;
uint8_t g_catalogTries = 0;
bool g_presetsLoaded = false;

enum class RequestKey : uint8_t {
  None,
  Poll,
  Catalog,
  Power,
  Brightness,
  Preset,
  Effect,
  Palette,
  Color,
  EffectParams,
  Custom1,
  Custom2,
  Custom3,
  Live,
};

struct QueuedRequest {
  char json[kMaxRequestLength] = {};
  RequestKey key = RequestKey::None;
  bool retryable = true;
};

QueuedRequest g_requestQueue[kRequestQueueSize];
uint8_t g_requestRead = 0, g_requestWrite = 0, g_requestCount = 0;

struct ActiveRequest : QueuedRequest {
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
    case RequestKey::Power:        return "power";
    case RequestKey::Brightness:   return "brightness";
    case RequestKey::Preset:       return "preset";
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
    case kMsgHello:    return "HELLO";
    case kMsgLive:     return "LIVE";
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

void rememberPeer(const uint8_t* mac) {
  if (!mac || isBroadcastMac(mac)) return;
  if (memcmp(g_peerMac, mac, sizeof(g_peerMac)) != 0) {
    memcpy(g_peerMac, mac, sizeof(g_peerMac));
    g_peerRegistered = false;
  }
  g_hasPeer = true;
}

void ensurePeerRegistered() {
  if (!g_hasPeer || g_peerRegistered) return;
  if (esp_now_is_peer_exist(g_peerMac)) {
    g_peerRegistered = true;
    Serial.printf("[ESPNOW] peer already registered ch=%u radioCh=%u\n", g_channel, currentRadioChannel());
    return;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, g_peerMac, sizeof(g_peerMac));
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  const esp_err_t result = esp_now_add_peer(&peer);
  if (result == ESP_OK || esp_now_is_peer_exist(g_peerMac)) {
    g_peerRegistered = true;
    Serial.printf("[ESPNOW] peer registered result=%d ch=%u radioCh=%u\n",
                  result, g_channel, currentRadioChannel());
  } else {
    Serial.printf("[ESPNOW] peer registration failed result=%d ch=%u radioCh=%u\n",
                  result, g_channel, currentRadioChannel());
  }
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
  if (type != kMsgResponse && type != kMsgPush && type != kMsgHello && type != kMsgLive) return;
  if (!g_channelLocked && type != kMsgHello) return;
  if (g_channelLocked && (!g_hasPeer || memcmp(mac, g_peerMac, sizeof(g_peerMac)) != 0)) return;
  if (total < 1 || total > kMaxFrags || idx >= total || plen > kFragSize) return;
  if (idx < total - 1 && plen != kFragSize) return;

  g_lastLinkRx = now_ms;
  if (g_active && now_ms - g_reasmLast > kReasmTimeoutMs) resetReasm();

  // A request/response exchange has priority over best-effort LIVE, PUSH, and HELLO traffic.
  // Sharing one reassembly buffer otherwise lets an unrelated frame discard a partial response.
  if (g_request.active && g_request.awaitingResponse &&
      (type != kMsgResponse || id != g_request.id)) {
    g_framesDeferredForResponse++;
    return;
  }
  // Once a state PUSH starts, let its remaining fragments finish before accepting best-effort
  // live/hello traffic. A newer PUSH may still replace an incomplete older state update.
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
  g_requestRead = g_requestWrite = g_requestCount = 0;
}

void queueRequest(const char* json, RequestKey key, bool retryable = true) {
  if (!json) return;
  const size_t len = strnlen(json, kMaxRequestLength);
  if (!len || len >= kMaxRequestLength) return;

  // Replace a queued value for the same control. This keeps slider/color gestures bounded
  // while preserving ordering between different controls.
  if (key != RequestKey::None) {
    for (uint8_t i = 0; i < g_requestCount; i++) {
      QueuedRequest& queued = g_requestQueue[(g_requestRead + i) % kRequestQueueSize];
      if (queued.key == key) {
        memcpy(queued.json, json, len + 1);
        queued.retryable = retryable;
        return;
      }
    }
  }
  if (g_requestCount >= kRequestQueueSize) return;

  QueuedRequest& queued = g_requestQueue[g_requestWrite];
  memcpy(queued.json, json, len + 1);
  queued.key = key;
  queued.retryable = retryable;
  g_requestWrite = (g_requestWrite + 1) % kRequestQueueSize;
  g_requestCount++;
}

void completeRequest() {
  if (g_framesDeferredForResponse) {
    Serial.printf("[ESPNOW] request complete key=%s id=%u deferredFrames=%lu\n",
                  requestKeyName(g_request.key), g_request.id, g_framesDeferredForResponse);
  }
  g_framesDeferredForResponse = 0;
  g_request = ActiveRequest{};
}

void retryRequest(uint32_t now_ms) {
  g_request.awaitingResponse = false;
  g_request.nextAttempt = now_ms + kRequestRetryMs;
}

void pumpRequests(uint32_t now_ms) {
  if (!g_channelLocked || !g_hasPeer || !g_peerRegistered) return;

  if (!g_request.active && g_requestCount) {
    const QueuedRequest& queued = g_requestQueue[g_requestRead];
    memcpy(g_request.json, queued.json, sizeof(g_request.json));
    g_request.key = queued.key;
    g_request.retryable = queued.retryable;
    g_request.active = true;
    g_request.id = g_msgId++;
    g_framesDeferredForResponse = 0;
    g_requestRead = (g_requestRead + 1) % kRequestQueueSize;
    g_requestCount--;
  }
  if (!g_request.active) return;

  if (g_request.awaitingResponse) {
    if (now_ms - g_request.sentAt < kRequestTimeoutMs) return;
    Serial.printf("[ESPNOW] request timeout key=%s id=%u attempt=%u age=%lums linkAge=%lums lockedCh=%u radioCh=%u deferredFrames=%lu\n",
                  requestKeyName(g_request.key), g_request.id, g_request.attempts,
                  now_ms - g_request.sentAt, now_ms - g_lastLinkRx, g_channel,
                  currentRadioChannel(), g_framesDeferredForResponse);
    if (!g_request.retryable || g_request.attempts >= kMaxRequestAttempts) {
      completeRequest();
      return;
    }
    retryRequest(now_ms);
  }
  if (now_ms < g_request.nextAttempt) return;

  const size_t len = strlen(g_request.json);
  Serial.printf("[ESPNOW] TX request key=%s id=%u attempt=%u bytes=%u lockedCh=%u radioCh=%u queued=%u\n",
                requestKeyName(g_request.key), g_request.id, g_request.attempts + 1,
                unsigned(len), g_channel, currentRadioChannel(), g_requestCount);
  if (sendFrameTo(g_peerMac, kMsgRequest, g_request.id,
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
    g_request.nextAttempt = now_ms + kRequestRetryMs;
  }
}

void setRadioChannel(uint8_t ch) {
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
}

void hopChannel(uint32_t now_ms) {
  g_channel = (g_channel % kMaxChannel) + 1;
  setRadioChannel(g_channel);
  sendFrameTo(kBroadcastMac, kMsgHello, g_msgId++, nullptr, 0);
  Serial.printf("[ESPNOW] discovery probe ch=%u helloId=%u rxDrops=%lu\n",
                g_channel, uint8_t(g_msgId - 1), g_rxQueueDrops.load(std::memory_order_relaxed));
  g_lastHop = now_ms;
}

uint32_t colorFromArray(JsonArrayConst col) {
  if (col.isNull() || col.size() < 3) return g_model.color;
  uint8_t r = col[0] | 0, g = col[1] | 0, b = col[2] | 0;
  return (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

void applyState(JsonObjectConst state, JsonObjectConst info) {
  if (!state.isNull()) {
    if (state["on"].is<bool>()) g_model.power = state["on"].as<bool>();
    if (state["bri"].is<int>()) g_model.brightness = state["bri"].as<int>();
    if (state["ps"].is<int>()) g_model.preset = state["ps"].as<int>();
    JsonArrayConst segs = state["seg"].as<JsonArrayConst>();
    if (!segs.isNull() && segs.size() > 0) {
      size_t mainIdx = state["mainseg"].is<int>() ? size_t(state["mainseg"].as<int>()) : 0;
      if (mainIdx >= segs.size()) mainIdx = 0;
      JsonObjectConst seg = segs[mainIdx].as<JsonObjectConst>();
      if (seg["fx"].is<int>()) g_model.effect = seg["fx"].as<int>();
      if (seg["pal"].is<int>()) g_model.palette = seg["pal"].as<int>();
      if (seg["sx"].is<int>()) g_model.speed = seg["sx"].as<int>();
      if (seg["ix"].is<int>()) g_model.intensity = seg["ix"].as<int>();
      if (seg["c1"].is<int>()) g_model.custom1 = seg["c1"].as<int>();
      if (seg["c2"].is<int>()) g_model.custom2 = seg["c2"].as<int>();
      if (seg["c3"].is<int>()) g_model.custom3 = seg["c3"].as<int>();
      JsonArrayConst col = seg["col"].as<JsonArrayConst>();
      if (!col.isNull() && col.size() > 0) g_model.color = colorFromArray(col[0].as<JsonArrayConst>());
    }
  }
  if (!info.isNull()) {
    const char* name = info["name"].as<const char*>();
    if (name) g_model.name = name;
  }
  g_model.online = true;
}

struct ParseResult {
  bool valid = false;
  int error = -1;
};

ParseResult parseInbox(const uint8_t* data, size_t len) {
  ParseResult result;
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) return result;

  if (doc["hello"].is<JsonObject>()) {
    JsonObjectConst hello = doc["hello"].as<JsonObjectConst>();
    const char* name = hello["name"].as<const char*>();
    const bool changed = !g_model.online || (name && g_model.name != name);
    if (name) g_model.name = name;
    g_model.online = true;
    if (changed) g_stateRev++;
    result.valid = true;
  } else if (doc["state"].is<JsonObject>()) {
    applyState(doc["state"].as<JsonObjectConst>(), doc["info"].as<JsonObjectConst>());
    g_stateRev++;
    result.valid = true;
  } else if (doc["presets"].is<JsonObject>()) {
    g_model.presets.clear();
    for (JsonPairConst kv : doc["presets"].as<JsonObjectConst>()) {
      const char* s = kv.value().as<const char*>();
      g_model.presets.push_back({uint8_t(atoi(kv.key().c_str())), s ? s : ""});
    }
    std::sort(g_model.presets.begin(), g_model.presets.end(),
              [](const PresetInfo& a, const PresetInfo& b) { return a.id < b.id; });
    g_presetsLoaded = true;
    g_catalogRev++;
    result.valid = true;
  } else if (doc["success"].is<bool>() || doc["error"].is<int>()) {
    if (!g_model.online) {
      g_model.online = true;
      g_stateRev++;
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

void loadFallbackPresets() {
  g_model.presets.clear();
  for (uint8_t id = 1; id <= kExtendedPresetCount; id++)
    g_model.presets.push_back({id, std::string("Preset ") + std::to_string(id)});
  g_presetsLoaded = true;
  g_catalogRev++;
}

// Completed messages are handled synchronously by loop(), so the large reassembly buffer can
// be reused without a second 8 KB inbox and without any callback/main-loop data race.
void handleCompleteMessage(uint8_t type, uint8_t id, const uint8_t* src,
                           const uint8_t* payload, size_t len, uint32_t now_ms) {
  if (type == kMsgLive) {
    if (decodeLive(payload, len)) {
      g_lastRx = now_ms;
      g_lastLiveRx = now_ms;
    }
    return;
  }

  const ParseResult result = parseInbox(payload, len);
  if (!result.valid) return;
  g_lastRx = now_ms;
  if (type == kMsgHello && !g_channelLocked) {
    rememberPeer(src);
    g_sawHello = g_hasPeer;
  }
  if (type == kMsgResponse && g_request.active && id == g_request.id) {
    if (result.error == 3) retryRequest(now_ms);
    else {
      if (g_request.key == RequestKey::Catalog && result.error == 8) loadFallbackPresets();
      else if (g_request.key == RequestKey::Catalog && result.error >= 8) g_presetsLoaded = true;
      completeRequest();
    }
  }
}

}  // namespace

void begin() {
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

  if (g_model.online && g_lastLinkRx && now_ms - g_lastLinkRx > kOfflineMs) {
    g_model.online = false;
    g_stateRev++;
  }

  // channel discovery: lock on once WLED answers; resume hopping if it goes silent
  if (g_sawHello) {
    g_sawHello = false;
    ensurePeerRegistered();
    if (!g_channelLocked && g_peerRegistered) {
      g_channelLocked = true;
      g_lastLinkRx = now_ms;
      Serial.printf("[ESPNOW] LOCKED ch=%u radioCh=%u peer=%02x:%02x:%02x:%02x:%02x:%02x rxDrops=%lu\n",
                    g_channel, currentRadioChannel(), g_peerMac[0], g_peerMac[1], g_peerMac[2],
                    g_peerMac[3], g_peerMac[4], g_peerMac[5], rxQueueDrops);
      g_lastHeartbeat = g_lastPoll = g_lastCatalog = now_ms;
      g_catalogTries = 1;
      poll();
      requestCatalogs();
    }
  }
  if (g_channelLocked && g_lastLinkRx && now_ms - g_lastLinkRx > kOfflineMs) {
    Serial.printf("[ESPNOW] OFFLINE linkAge=%lums completeAge=%lums lockedCh=%u radioCh=%u request=%s id=%u attempt=%u rxDrops=%lu\n",
                  now_ms - g_lastLinkRx, now_ms - g_lastRx, g_channel, currentRadioChannel(),
                  g_request.active ? requestKeyName(g_request.key) : "none",
                  g_request.id, g_request.attempts, rxQueueDrops);
    if (g_peerRegistered) esp_now_del_peer(g_peerMac);
    g_channelLocked = false;
    g_hasPeer = false;
    g_peerRegistered = false;
    g_presetsLoaded = false;
    clearRequests();
    resetReasm();
  }
  if (!g_channelLocked && now_ms - g_lastHop > kHopDwellMs) {
    hopChannel(now_ms);
  }

  if (g_channelLocked) {
    if (!g_request.active && !g_requestCount && now_ms - g_lastLinkRx > kHeartbeatMs &&
        now_ms - g_lastHeartbeat > kHeartbeatMs) {
      sendFrameTo(g_peerMac, kMsgHello, g_msgId++, nullptr, 0);
      g_lastHeartbeat = now_ms;
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
uint32_t stateRevision() { return g_stateRev; }
uint32_t catalogRevision() { return g_catalogRev; }
uint32_t liveRevision() { return g_liveRev; }
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

void poll() { queueRequest("{\"v\":\"compact\"}", RequestKey::Poll); }

void requestCatalogs() {
  g_lastCatalog = millis();
  queueRequest("{\"get\":\"ps\"}", RequestKey::Catalog);
}

void setPower(bool on) {
  queueRequest(on ? "{\"on\":true,\"v\":true}" : "{\"on\":false,\"v\":true}", RequestKey::Power);
}
void togglePower() { setPower(!g_model.power); }

void setBrightness(uint8_t bri) {
  char buf[40];
  snprintf(buf, sizeof(buf), "{\"bri\":%u,\"v\":true}", bri);
  queueRequest(buf, RequestKey::Brightness);
}

void applyPreset(uint8_t id) {
  char buf[40];
  snprintf(buf, sizeof(buf), "{\"ps\":%u,\"v\":true}", id);
  queueRequest(buf, RequestKey::Preset);
}

void setEffect(uint8_t fxId) {
  g_model.effect = fxId; // optimistic model update prevents unrelated revisions restoring stale state
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"seg\":[{\"fx\":%u}],\"v\":true}", fxId);
  queueRequest(buf, RequestKey::Effect);
}

void setPalette(uint8_t palId) {
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"seg\":[{\"pal\":%u}],\"v\":true}", palId);
  queueRequest(buf, RequestKey::Palette);
}

void setColor(uint8_t r, uint8_t g, uint8_t b) {
  g_model.color = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"seg\":[{\"col\":[[%u,%u,%u]]}],\"v\":true}", r, g, b);
  queueRequest(buf, RequestKey::Color);
}

void setEffectParams(int speed, int intensity) {
  char seg[64];
  int n = snprintf(seg, sizeof(seg), "{\"seg\":[{");
  bool comma = false;
  if (speed >= 0) { n += snprintf(seg + n, sizeof(seg) - n, "\"sx\":%d", speed); comma = true; }
  if (intensity >= 0) { n += snprintf(seg + n, sizeof(seg) - n, "%s\"ix\":%d", comma ? "," : "", intensity); }
  snprintf(seg + n, sizeof(seg) - n, "}],\"v\":true}");
  queueRequest(seg, RequestKey::EffectParams);
}

void setCustomParam(uint8_t index, uint8_t value) {
  if (index < 1 || index > 3) return;
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"seg\":[{\"c%u\":%u}],\"v\":true}", index, value);
  const RequestKey key = index == 1 ? RequestKey::Custom1 :
                         index == 2 ? RequestKey::Custom2 : RequestKey::Custom3;
  queueRequest(buf, key);
}

void sendRaw(const char* json) { queueRequest(json, RequestKey::None, false); }

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
