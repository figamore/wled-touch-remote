#include "wled_api.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <algorithm>
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
constexpr uint32_t kLiveReArmMs = 1500;   // refresh the {"lv":true} keepalive (WLED expires at 3 s)
constexpr uint32_t kHeartbeatMs = 3000;   // cheap HELLO keepalive cadence while connected
constexpr uint32_t kResyncMs = 30000;     // periodic full state poll to correct any missed pushes
constexpr uint32_t kOfflineMs = 10000;    // no rx for this long => offline (must exceed kHeartbeatMs)
constexpr uint32_t kHopDwellMs = 150;     // time spent probing each channel while hunting for WLED
constexpr uint32_t kReasmTimeoutMs = 500;
constexpr uint8_t  kMaxChannel = 13;
constexpr uint8_t  kCatalogTries = 6;     // give up refetching the preset list after this many attempts

Model g_model;
uint32_t g_stateRev = 0;
uint32_t g_catalogRev = 0;
uint32_t g_liveRev = 0;
uint32_t g_lastRx = 0;
uint32_t g_lastLiveRx = 0;
uint8_t g_msgId = 1;

// Reassembly is touched only by the (single, non-reentrant) ESP-NOW receive task. The completed
// message is handed to loop() via g_inboxReady; the receive task will not overwrite the inbox
// until loop() clears that flag, so no extra lock is needed.
uint8_t g_reasm[kMaxFrags * kFragSize + 1];
uint8_t g_curId = 0, g_curType = 0, g_curTotal = 0, g_count = 0;
uint8_t g_curMac[6] = {};
uint64_t g_flags = 0;
size_t g_len = 0;
bool g_active = false;
uint32_t g_reasmLast = 0;

struct Inbox {
  volatile bool ready = false;
  uint8_t type = 0;
  uint8_t src[6] = {};
  uint8_t data[kMaxJson + 1] = {};
  size_t len = 0;
};

Inbox g_jsonInbox;
Inbox g_liveInbox;

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
volatile bool g_sawFrame = false;
uint8_t g_peerMac[6] = {};
bool g_hasPeer = false;
bool g_peerRegistered = false;
uint32_t g_lastHeartbeat = 0;
uint32_t g_lastPoll = 0;
uint32_t g_lastCatalog = 0;
uint8_t g_catalogTries = 0;
bool g_presetsLoaded = false;

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
    return;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, g_peerMac, sizeof(g_peerMac));
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  if (esp_now_add_peer(&peer) == ESP_OK || esp_now_is_peer_exist(g_peerMac)) {
    g_peerRegistered = true;
  }
}

void storeInbox(Inbox& inbox,
                uint8_t type,
                const uint8_t* src,
                const uint8_t* payload,
                size_t len,
                bool replace) {
  if (inbox.ready && !replace) return;
  if (len > kMaxJson) return;
  if (src) memcpy(inbox.src, src, sizeof(inbox.src));
  memcpy(inbox.data, payload, len);
  inbox.data[len] = 0;
  inbox.len = len;
  inbox.type = type;
  inbox.ready = true;
}

void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < kHeaderSize || data[0] != kMagic || data[1] != kVersion) return;
  g_sawFrame = true;  // a valid frame means WLED is on this channel
  rememberPeer(mac);
  const uint8_t type = data[2];
  const uint8_t id = data[3];
  const uint8_t idx = data[4];
  const uint8_t total = data[5];
  const uint8_t plen = static_cast<uint8_t>(len) - kHeaderSize;
  if (total < 1 || total > kMaxFrags || idx >= total || plen > kFragSize) return;
  if (idx < total - 1 && plen != kFragSize) return;

  const uint32_t now = millis();
  if (g_active && now - g_reasmLast > kReasmTimeoutMs) resetReasm();

  if (!g_active || g_curId != id || g_curType != type || g_curTotal != total ||
      (mac && memcmp(g_curMac, mac, sizeof(g_curMac)) != 0)) {
    if (idx != 0) return;
    g_active = true;
    g_curId = id;
    g_curType = type;
    g_curTotal = total;
    g_count = 0;
    g_flags = 0;
    g_len = 0;
    if (mac) memcpy(g_curMac, mac, sizeof(g_curMac));
  }
  g_reasmLast = now;

  const uint64_t bit = uint64_t(1) << idx;
  if (g_flags & bit) return;
  memcpy(g_reasm + size_t(idx) * kFragSize, data + kHeaderSize, plen);
  g_flags |= bit;
  g_count++;
  if (idx == total - 1) g_len = size_t(idx) * kFragSize + plen;
  if (g_count < total) return;

  if (g_len <= kMaxJson) {
    if (g_curType == kMsgLive) {
      storeInbox(g_liveInbox, g_curType, g_curMac, g_reasm, g_len, true);
    } else {
      storeInbox(g_jsonInbox, g_curType, g_curMac, g_reasm, g_len, false);
    }
  }
  resetReasm();
}

bool sendFrameTo(const uint8_t* mac, uint8_t type, const uint8_t* payload, size_t len) {
  size_t total = len ? (len + kFragSize - 1) / kFragSize : 1;
  if (total > kMaxFrags) return false;
  uint8_t id = g_msgId++;
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

bool sendFrame(uint8_t type, const uint8_t* payload, size_t len) {
  ensurePeerRegistered();
  const uint8_t* target = (g_hasPeer && g_peerRegistered) ? g_peerMac : kBroadcastMac;
  return sendFrameTo(target, type, payload, len);
}

void sendJson(const char* json) {
  sendFrame(kMsgRequest, reinterpret_cast<const uint8_t*>(json), strlen(json));
}

void setRadioChannel(uint8_t ch) {
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
}

void hopChannel(uint32_t now_ms) {
  g_channel = (g_channel % kMaxChannel) + 1;
  setRadioChannel(g_channel);
  sendFrameTo(kBroadcastMac, kMsgHello, nullptr, 0);
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

void parseInbox(const uint8_t* data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) return;

  if (doc["hello"].is<JsonObject>()) {
    JsonObjectConst hello = doc["hello"].as<JsonObjectConst>();
    const char* name = hello["name"].as<const char*>();
    if (name) g_model.name = name;
    g_model.online = true;
    g_stateRev++;
  } else if (doc["state"].is<JsonObject>()) {
    applyState(doc["state"].as<JsonObjectConst>(), doc["info"].as<JsonObjectConst>());
    g_stateRev++;
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
  } else if (doc["success"].is<bool>() || doc["error"].is<int>()) {
    if (!g_model.online) {
      g_model.online = true;
      g_stateRev++;
    }
  }
}

void decodeLive(const uint8_t* data, size_t len) {
  if (len < 2 || data[0] != 'L') return;
  size_t pos = 2;
  if (data[1] == 2) {  // 2D frame: width/height follow the version byte
    if (len < 4) return;
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
}

}  // namespace

void begin() {
  esp_now_register_recv_cb(onRecv);
}

void loop(uint32_t now_ms) {
  if (g_active && now_ms - g_reasmLast > kReasmTimeoutMs) resetReasm();

  if (g_liveInbox.ready) {
    decodeLive(g_liveInbox.data, g_liveInbox.len);
    g_lastRx = now_ms;
    g_lastLiveRx = now_ms;
    g_liveInbox.ready = false;
  }

  if (g_jsonInbox.ready) {
    parseInbox(g_jsonInbox.data, g_jsonInbox.len);
    g_lastRx = now_ms;
    g_jsonInbox.ready = false;
  }

  if (g_model.online && now_ms - g_lastRx > kOfflineMs) {
    g_model.online = false;
    g_stateRev++;
  }

  // channel discovery: lock on once WLED answers; resume hopping if it goes silent
  if (g_sawFrame) {
    g_sawFrame = false;
    ensurePeerRegistered();
    if (!g_channelLocked) {
      g_channelLocked = true;
      g_lastHeartbeat = g_lastPoll = g_lastCatalog = now_ms;
      g_catalogTries = 1;
      poll();
      requestCatalogs();
    }
  }
  if (g_channelLocked && now_ms - g_lastRx > kOfflineMs) {
    g_channelLocked = false;  // lost WLED; resume hunting
  }
  if (!g_channelLocked && now_ms - g_lastHop > kHopDwellMs) {
    hopChannel(now_ms);
  }

  if (g_channelLocked) {
    if (now_ms - g_lastHeartbeat > kHeartbeatMs) {
      sendFrame(kMsgHello, nullptr, 0);  // cheap keepalive: refreshes rx + WLED's push presence
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
    sendJson("{\"lv\":true}");
    g_liveArmed = now_ms;
  }
}

const Model& model() { return g_model; }
bool online() { return g_model.online; }
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

void poll() { sendJson("{\"v\":true}"); }

void requestCatalogs() {
  g_lastCatalog = millis();
  sendJson("{\"get\":\"ps\"}");
}

void setPower(bool on) { sendJson(on ? "{\"on\":true,\"v\":true}" : "{\"on\":false,\"v\":true}"); }
void togglePower() { sendJson("{\"on\":\"t\",\"v\":true}"); }

void setBrightness(uint8_t bri) {
  char buf[40];
  snprintf(buf, sizeof(buf), "{\"bri\":%u,\"v\":true}", bri);
  sendJson(buf);
}

void applyPreset(uint8_t id) {
  char buf[40];
  snprintf(buf, sizeof(buf), "{\"ps\":%u,\"v\":true}", id);
  sendJson(buf);
}

void setEffect(uint8_t fxId) {
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"seg\":[{\"fx\":%u}],\"v\":true}", fxId);
  sendJson(buf);
}

void setPalette(uint8_t palId) {
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"seg\":[{\"pal\":%u}],\"v\":true}", palId);
  sendJson(buf);
}

void setColor(uint8_t r, uint8_t g, uint8_t b) {
  g_model.color = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"seg\":[{\"col\":[[%u,%u,%u]]}],\"v\":true}", r, g, b);
  sendJson(buf);
}

void setEffectParams(int speed, int intensity) {
  char seg[64];
  int n = snprintf(seg, sizeof(seg), "{\"seg\":[{");
  bool comma = false;
  if (speed >= 0) { n += snprintf(seg + n, sizeof(seg) - n, "\"sx\":%d", speed); comma = true; }
  if (intensity >= 0) { n += snprintf(seg + n, sizeof(seg) - n, "%s\"ix\":%d", comma ? "," : "", intensity); }
  snprintf(seg + n, sizeof(seg) - n, "}],\"v\":true}");
  sendJson(seg);
}

void setCustomParam(uint8_t index, uint8_t value) {
  if (index < 1 || index > 3) return;
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"seg\":[{\"c%u\":%u}],\"v\":true}", index, value);
  sendJson(buf);
}

void sendRaw(const char* json) { sendJson(json); }

void setLivePeek(bool on) {
  g_liveWanted = on;
  g_liveArmed = 0;
  if (on) {
    if (g_channelLocked) sendJson("{\"lv\":true}");
  }
  else {
    if (g_channelLocked) sendJson("{\"lv\":false}");
    g_liveCount = 0;
    g_lastLiveRx = 0;
    g_liveRev++;
  }
}

}  // namespace wled
