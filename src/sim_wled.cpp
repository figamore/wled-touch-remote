#if WLED_TOUCH_SIMULATOR

#include "sim_wled.h"

#include "wled_api.h"

// The desktop simulator exercises the same Wi-Fi API client state machine as
// the device. Network I/O is intentionally not emulated here; it remains a
// display/input harness and never carries a second wireless protocol.

void simWledSetSecondLinked(bool) {}
void simWledTick() {}
void simWledExternalChange(int) {}
void simWledDropNextResponse() {}

SimWledSnapshot simWledSnapshot() {
  const wled::Model& model = wled::model();
  return {model.power, model.brightness, uint8_t(model.effect < 0 ? 0 : model.effect),
          uint8_t(model.palette < 0 ? 0 : model.palette), model.speed, model.intensity, model.color};
}

#endif
