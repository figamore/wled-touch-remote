#if WLED_TOUCH_SIMULATOR

#include "sim_wled.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "generated/wled_effects.h"

namespace {

// ── Protocol framing (mirrors WLED remote.cpp) ───────────────────────────────

constexpr uint8_t kMagic = 0x4E;
constexpr uint8_t kVersion = 0x01;
constexpr uint8_t kHeaderSize = 6;
constexpr uint8_t kFragSize = 244;
constexpr uint8_t kMsgRequest = 0x01;
constexpr uint8_t kMsgResponse = 0x02;
constexpr uint8_t kMsgPush = 0x03;
constexpr uint8_t kMsgHello = 0x04;
constexpr uint8_t kMsgLive = 0x05;

constexpr uint8_t kWledMac[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
constexpr uint32_t kLiveTimeoutMs = 3000;  // {"lv":true} keepalive expiry, same as WLED
constexpr uint32_t kLiveFramePeriodMs = 45;
constexpr uint16_t kLedCount = 60;

// ── Fake instance state ──────────────────────────────────────────────────────

struct SimState {
  bool on = true;
  uint8_t bri = 140;
  int ps = -1;
  uint8_t fx = 0;
  uint8_t pal = 0;
  uint8_t sx = 128;
  uint8_t ix = 128;
  uint8_t col[3] = {255, 160, 0};
};

SimState g_state;
uint8_t g_pushId = 1;
uint32_t g_lastLvKeepalive = 0;
bool g_liveActive = false;
uint32_t g_lastLiveFrame = 0;
float g_livePhase = 0.0f;

// Outgoing messages are queued and delivered one per tick so the remote's
// single-message inbox is never clobbered by back-to-back replies.
struct PendingMessage {
  uint8_t type;
  std::vector<uint8_t> payload;
};

std::deque<PendingMessage> g_outbox;

const char* kPaletteNames[] = {
    "Default", "* Random Cycle", "* Color 1", "* Colors 1&2", "* Color Gradient", "* Colors Only",
    "Party", "Cloud", "Lava", "Ocean", "Forest", "Rainbow", "Rainbow Bands", "Sunset", "Rivendell",
    "Breeze", "Red & Blue", "Yellowout", "Analogous", "Splash", "Pastel", "Sunset 2", "Beach",
    "Vintage", "Departure", "Landscape", "Beech", "Sherbet", "Hult", "Hult 64", "Drywet", "Jul",
    "Grintage", "Rewhi", "Tertiary", "Fire", "Icefire", "Cyane", "Light Pink", "Autumn", "Magenta",
    "Magred", "Yelmag", "Yelblu", "Orange & Teal", "Tiamat", "April Night", "Orangery", "C9",
    "Sakura", "Aurora", "Atlantica", "C9 2", "C9 New", "Temperature", "Aurora 2", "Retro Clown",
    "Candy", "Toxy Reaf", "Fairy Reaf", "Semi Blue", "Pink Candy", "Red Reaf", "Aqua Flash",
    "Yelblu Hot", "Lite Light", "Red Flash", "Blink Red", "Red Shift", "Red Tide", "Candy2",
    "Traffic Light"};

struct PresetEntry {
  uint8_t id;
  const char* name;
};

const PresetEntry kPresets[] = {
    {1, "Sunset Glow"}, {2, "Party Mix"},   {3, "Ocean Waves"}, {4, "Fireplace"},
    {5, "Movie Night"}, {6, "Reading"},     {7, "Rainbow Flow"}, {8, "Night Light"},
};

std::vector<std::string> effectNames() {
  size_t maxId = 0;
  for (const WledEffectInfo& effect : kWledEffects) {
    maxId = std::max<size_t>(maxId, effect.id);
  }
  std::vector<std::string> names(maxId + 1);
  names[0] = "Solid";
  for (const WledEffectInfo& effect : kWledEffects) {
    names[effect.id] = effect.name;
  }
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i].empty()) {
      names[i] = "Mode " + std::to_string(i);
    }
  }
  return names;
}

// ── Frame delivery back into the remote ─────────────────────────────────────

void queueMessage(uint8_t type, const uint8_t* payload, size_t len) {
  PendingMessage message;
  message.type = type;
  message.payload.assign(payload, payload + len);
  g_outbox.push_back(std::move(message));
}

void queueJson(uint8_t type, const JsonDocument& doc) {
  std::string out;
  serializeJson(doc, out);
  queueMessage(type, reinterpret_cast<const uint8_t*>(out.data()), out.size());
}

void deliverMessage(const PendingMessage& message) {
  esp_now_recv_cb_t callback = simEspNowRecvCallback();
  if (!callback) return;

  static uint8_t msgId = 1;
  const uint8_t id = msgId++;
  const size_t len = message.payload.size();
  const size_t total = len ? (len + kFragSize - 1) / kFragSize : 1;
  uint8_t frame[kHeaderSize + kFragSize];
  frame[0] = kMagic;
  frame[1] = kVersion;
  frame[2] = message.type;
  frame[3] = id;
  frame[5] = static_cast<uint8_t>(total);
  for (size_t i = 0; i < total; ++i) {
    const size_t offset = i * kFragSize;
    const size_t chunk = std::min<size_t>(kFragSize, len - offset);
    frame[4] = static_cast<uint8_t>(i);
    if (chunk) memcpy(frame + kHeaderSize, message.payload.data() + offset, chunk);
    callback(kWledMac, frame, static_cast<int>(kHeaderSize + chunk));
  }
}

// ── JSON API handling ────────────────────────────────────────────────────────

void buildStateInfo(JsonDocument& doc) {
  JsonObject state = doc["state"].to<JsonObject>();
  state["on"] = g_state.on;
  state["bri"] = g_state.bri;
  state["ps"] = g_state.ps;
  state["mainseg"] = 0;
  JsonArray segs = state["seg"].to<JsonArray>();
  JsonObject seg = segs.add<JsonObject>();
  seg["id"] = 0;
  seg["fx"] = g_state.fx;
  seg["pal"] = g_state.pal;
  seg["sx"] = g_state.sx;
  seg["ix"] = g_state.ix;
  JsonArray cols = seg["col"].to<JsonArray>();
  JsonArray col0 = cols.add<JsonArray>();
  col0.add(g_state.col[0]);
  col0.add(g_state.col[1]);
  col0.add(g_state.col[2]);

  JsonObject info = doc["info"].to<JsonObject>();
  info["name"] = "Sim WLED";
  info["ver"] = "0.16.0-sim";
}

void queueStateResponse(uint8_t type) {
  JsonDocument doc;
  buildStateInfo(doc);
  queueJson(type, doc);
}

void queueStatePush() {
  queueStateResponse(kMsgPush);
}

void queueHello() {
  JsonDocument doc;
  JsonObject hello = doc["hello"].to<JsonObject>();
  hello["name"] = "Sim WLED";
  hello["mac"] = "aabbcc112233";
  hello["ver"] = 2607000;
  hello["ch"] = 6;
  queueJson(kMsgHello, doc);
}

void queueCatalog(const char* what) {
  JsonDocument doc;
  if (!strcmp(what, "fx")) {
    JsonArray effects = doc["effects"].to<JsonArray>();
    for (const std::string& name : effectNames()) {
      effects.add(name);
    }
  } else if (!strcmp(what, "pal")) {
    JsonArray palettes = doc["palettes"].to<JsonArray>();
    for (const char* name : kPaletteNames) {
      palettes.add(name);
    }
  } else if (!strcmp(what, "ps")) {
    JsonObject presets = doc["presets"].to<JsonObject>();
    for (const PresetEntry& preset : kPresets) {
      presets[std::to_string(preset.id)] = preset.name;
    }
  } else {
    doc["error"] = 9;
  }
  queueJson(kMsgResponse, doc);
}

// Returns true when the request changed state (triggers a PUSH like real WLED).
bool applyRequest(JsonObjectConst request) {
  bool changed = false;
  if (request["on"].is<bool>()) {
    g_state.on = request["on"].as<bool>();
    changed = true;
  }
  if (request["bri"].is<int>()) {
    g_state.bri = request["bri"].as<int>();
    changed = true;
  }
  if (request["ps"].is<int>()) {
    g_state.ps = request["ps"].as<int>();
    // Applying a preset flips visible state so sync bugs show up in tests.
    g_state.fx = (g_state.ps * 13) % 100;
    g_state.pal = (g_state.ps * 7) % 60;
    changed = true;
  }
  JsonArrayConst segs = request["seg"].as<JsonArrayConst>();
  if (!segs.isNull() && segs.size() > 0) {
    JsonObjectConst seg = segs[0].as<JsonObjectConst>();
    if (seg["fx"].is<int>()) { g_state.fx = seg["fx"].as<int>(); changed = true; }
    if (seg["pal"].is<int>()) { g_state.pal = seg["pal"].as<int>(); changed = true; }
    if (seg["sx"].is<int>()) { g_state.sx = seg["sx"].as<int>(); changed = true; }
    if (seg["ix"].is<int>()) { g_state.ix = seg["ix"].as<int>(); changed = true; }
    JsonArrayConst cols = seg["col"].as<JsonArrayConst>();
    if (!cols.isNull() && cols.size() > 0) {
      JsonArrayConst col0 = cols[0].as<JsonArrayConst>();
      if (!col0.isNull() && col0.size() >= 3) {
        g_state.col[0] = col0[0].as<int>();
        g_state.col[1] = col0[1].as<int>();
        g_state.col[2] = col0[2].as<int>();
        changed = true;
      }
    }
  }
  return changed;
}

void handleRequest(const uint8_t* payload, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) {
    JsonDocument error;
    error["error"] = 9;
    queueJson(kMsgResponse, error);
    return;
  }

  if (doc["get"].is<const char*>()) {
    queueCatalog(doc["get"].as<const char*>());
    return;
  }

  if (doc["lv"].is<bool>()) {
    g_liveActive = doc["lv"].as<bool>();
    g_lastLvKeepalive = millis();
    JsonDocument ok;
    ok["success"] = true;
    queueJson(kMsgResponse, ok);
    return;
  }

  const bool changed = applyRequest(doc.as<JsonObjectConst>());
  if (doc["v"].as<bool>()) {
    queueStateResponse(kMsgResponse);
  } else {
    JsonDocument ok;
    ok["success"] = true;
    queueJson(kMsgResponse, ok);
  }
  if (changed) {
    queueStatePush();
  }
}

// ── Live peek frame synthesis ────────────────────────────────────────────────

void hsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
  const float c = v * s;
  const float hp = h / 60.0f;
  const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
  float rf = 0, gf = 0, bf = 0;
  if (hp < 1) { rf = c; gf = x; }
  else if (hp < 2) { rf = x; gf = c; }
  else if (hp < 3) { gf = c; bf = x; }
  else if (hp < 4) { gf = x; bf = c; }
  else if (hp < 5) { rf = x; bf = c; }
  else { rf = c; bf = x; }
  const float m = v - c;
  r = static_cast<uint8_t>((rf + m) * 255.0f);
  g = static_cast<uint8_t>((gf + m) * 255.0f);
  b = static_cast<uint8_t>((bf + m) * 255.0f);
}

void queueLiveFrame() {
  uint8_t payload[2 + kLedCount * 3];
  payload[0] = 'L';
  payload[1] = 1;
  const float bri = g_state.on ? g_state.bri / 255.0f : 0.0f;
  for (uint16_t i = 0; i < kLedCount; ++i) {
    uint8_t r = 0, g = 0, b = 0;
    if (bri > 0.0f) {
      if (g_state.fx == 0) {
        // Solid: primary colour everywhere.
        r = static_cast<uint8_t>(g_state.col[0] * bri);
        g = static_cast<uint8_t>(g_state.col[1] * bri);
        b = static_cast<uint8_t>(g_state.col[2] * bri);
      } else {
        // Anything else: scrolling rainbow whose rate follows the speed slider.
        const float hue = std::fmod(g_livePhase + i * (360.0f / kLedCount), 360.0f);
        hsvToRgb(hue, 1.0f, bri, r, g, b);
      }
    }
    payload[2 + i * 3 + 0] = r;
    payload[2 + i * 3 + 1] = g;
    payload[2 + i * 3 + 2] = b;
  }
  queueMessage(kMsgLive, payload, sizeof(payload));
}

}  // namespace

void simWledOnOutgoingFrame(const uint8_t*, const uint8_t* data, size_t len) {
  if (len < kHeaderSize || data[0] != kMagic || data[1] != kVersion) {
    return;  // WizMote packets and other traffic are not for the JSON API
  }

  const uint8_t type = data[2];
  const uint8_t index = data[4];
  const uint8_t total = data[5];
  if (index != 0 || total != 1) {
    return;  // remote requests are single-fragment; ignore anything else
  }

  const uint8_t* payload = data + kHeaderSize;
  const size_t payloadLen = len - kHeaderSize;

  if (type == kMsgHello) {
    queueHello();
  } else if (type == kMsgRequest) {
    handleRequest(payload, payloadLen);
  }
}

void simWledTick() {
  if (!g_outbox.empty()) {
    deliverMessage(g_outbox.front());
    g_outbox.pop_front();
  }

  const uint32_t now = millis();
  if (g_liveActive && now - g_lastLvKeepalive > kLiveTimeoutMs) {
    g_liveActive = false;
  }
  if (g_liveActive && now - g_lastLiveFrame >= kLiveFramePeriodMs) {
    g_lastLiveFrame = now;
    g_livePhase = std::fmod(g_livePhase + (g_state.sx / 255.0f) * 14.0f + 1.0f, 360.0f);
    queueLiveFrame();
  }
}

SimWledSnapshot simWledSnapshot() {
  SimWledSnapshot snapshot;
  snapshot.on = g_state.on;
  snapshot.bri = g_state.bri;
  snapshot.fx = g_state.fx;
  snapshot.pal = g_state.pal;
  snapshot.sx = g_state.sx;
  snapshot.ix = g_state.ix;
  snapshot.color = (uint32_t(g_state.col[0]) << 16) | (uint32_t(g_state.col[1]) << 8) | g_state.col[2];
  return snapshot;
}

void simWledExternalChange(int kind) {
  switch (kind % 4) {
    case 0:
      g_state.col[0] = 40;
      g_state.col[1] = 220;
      g_state.col[2] = 120;
      break;
    case 1:
      g_state.fx = 27;  // Android
      break;
    case 2:
      g_state.bri = 60;
      break;
    default:
      g_state.pal = 11;  // Rainbow
      break;
  }
  queueStatePush();
}

#endif
