#pragma once

#include <cstddef>
#include <cstdint>

// The matching ESP-Hosted C6 firmware is linked into P4 builds. Single-chip
// targets return an empty image.
namespace hostedfirmware {

const uint8_t* c6Image();
size_t c6ImageSize();

}  // namespace hostedfirmware
