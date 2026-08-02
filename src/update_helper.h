#pragma once

#include <cstdint>

// A small, restart-isolated UI for firmware updates. It avoids allocating the
// controller UI, WLED catalog, and color-wheel assets while TLS is active.
namespace updatehelper {
bool requested();
void request();
void begin();
void loop(uint32_t now_ms);
bool active();
}  // namespace updatehelper
