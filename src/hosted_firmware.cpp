#include "hosted_firmware.h"

#include "app_config.h"

#if WLED_BOARD == WLED_BOARD_JC4880P443 && !WLED_TOUCH_SIMULATOR
#include "generated/hosted_c6_firmware_asm.h"

extern "C" {
extern const uint8_t wled_hosted_c6_firmware_start[];
extern const uint8_t wled_hosted_c6_firmware_end[];
}
#endif

namespace hostedfirmware {

const uint8_t* c6Image() {
#if WLED_BOARD == WLED_BOARD_JC4880P443 && !WLED_TOUCH_SIMULATOR
  return wled_hosted_c6_firmware_start;
#else
  return nullptr;
#endif
}

size_t c6ImageSize() {
#if WLED_BOARD == WLED_BOARD_JC4880P443 && !WLED_TOUCH_SIMULATOR
  return static_cast<size_t>(wled_hosted_c6_firmware_end - wled_hosted_c6_firmware_start);
#else
  return 0;
#endif
}

}  // namespace hostedfirmware
