#pragma once

#if WLED_TOUCH_SIMULATOR

#include <cstddef>
#include <cstdint>

// Simulator-only fake WLED instance. It answers the bidirectional ESP-NOW JSON
// API (see WLED docs/espnow-json-protocol.md) so the whole remote stack —
// discovery, state, catalogs, pushes and live peek frames — runs on desktop.

// Wired into include/sim/esp_now.h: every esp_now_send() lands here.
void simWledOnOutgoingFrame(const uint8_t* mac, const uint8_t* data, size_t len);
void simWledSetSecondLinked(bool linked);

// Call once per loop(): delivers queued replies and streams live peek frames.
void simWledTick();

// Simulate a change made from the WLED web UI (state mutates + PUSH broadcast).
// kind cycles through color / effect / brightness / palette changes.
void simWledExternalChange(int kind);

// Snapshot of the fake instance's state, for test assertions.
struct SimWledSnapshot {
  bool on;
  uint8_t bri;
  uint8_t fx;
  uint8_t pal;
  uint8_t sx;
  uint8_t ix;
  uint32_t color;  // 0xRRGGBB
};

SimWledSnapshot simWledSnapshot();

// Drop one direct RESPONSE after applying its request, exercising remote timeout/retry logic.
void simWledDropNextResponse();

#endif
