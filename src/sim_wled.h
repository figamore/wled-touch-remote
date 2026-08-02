#pragma once

#if WLED_TOUCH_SIMULATOR

#include <cstddef>
#include <cstdint>

// Simulator-only view of the Wi-Fi client model. Hardware networking is not
// emulated by the desktop display harness.
void simWledSetSecondLinked(bool linked);

// Call once per loop() to advance simulator hooks.
void simWledTick();

// Reserved for simulator UI tests.
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

void simWledDropNextResponse();

#endif
