#include "BatteryMonitor.h"

#include <Arduino.h>

#include "app_config.h"
#include "display.h"

void initBatteryMonitor() {
#if CYD_BATTERY_ADC
  if (batteryAvailable()) {
    analogSetPinAttenuation(CYD_BATTERY_ADC_PIN, ADC_11db);
  }
#endif
}

bool batteryAvailable() {
#if CYD_BATTERY_ADC && CYD_BOARD_CAPACITIVE
#if WLED_TOUCH_SIMULATOR
  return true;
#else
  return displayHardwareReady() && gfx.supportsBatteryMonitor();
#endif
#else
  return false;
#endif
}

int batteryAdcMillivolts() {
#if CYD_BATTERY_ADC
  if (!batteryAvailable()) {
    return -1;
  }
  return analogReadMilliVolts(CYD_BATTERY_ADC_PIN);
#else
  return -1;
#endif
}

int batteryMillivolts() {
#if CYD_BATTERY_ADC
  int mv = batteryAdcMillivolts();
  if (mv < 0) {
    return -1;
  }
  return (mv * CYD_BATTERY_ADC_MULTIPLIER_NUM) / CYD_BATTERY_ADC_MULTIPLIER_DEN;
#else
  return -1;
#endif
}

int batteryLevel() {
  static int cached_level = -1;
  static int smoothed_mv = -1;
  static uint32_t next_read_ms = 0;

  if (!batteryAvailable()) {
    return -1;
  }

  uint32_t now = millis();
  if (now < next_read_ms) {
    return cached_level;
  }
  next_read_ms = now + 1000;

  int millivolts = batteryMillivolts();
  if (millivolts < 3000 || millivolts > 4400) {
    return cached_level;
  }

  if (millivolts > 4250) {
    cached_level = 100;
    return cached_level;
  }

  smoothed_mv = (smoothed_mv < 3000) ? millivolts : (smoothed_mv * 3 + millivolts) / 4;
  millivolts = smoothed_mv;

  struct LevelPoint {
    int mv;
    int pct;
  };
  static constexpr LevelPoint curve[] = {
      {4050, 100},
      {3800, 75},
      {3700, 50},
      {3600, 25},
      {3300, 0},
  };

  if (millivolts >= curve[0].mv) {
    cached_level = 100;
    return cached_level;
  }
  for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
    if (millivolts >= curve[i].mv) {
      int high_mv = curve[i - 1].mv;
      int low_mv = curve[i].mv;
      int high_pct = curve[i - 1].pct;
      int low_pct = curve[i].pct;
      cached_level = low_pct + ((millivolts - low_mv) * (high_pct - low_pct)) / (high_mv - low_mv);
      return cached_level;
    }
  }

  cached_level = 0;
  return cached_level;
}

bool batteryCharging() {
  static int cached = -1;
  static uint32_t next_read_ms = 0;

  if (!batteryAvailable()) {
    return false;
  }

  uint32_t now = millis();
  if (cached < 0 || now >= next_read_ms) {
    cached = (batteryMillivolts() > 4250) ? 1 : 0;
    next_read_ms = now + 1000;
  }
  return cached == 1;
}
