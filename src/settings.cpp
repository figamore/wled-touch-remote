#include "settings.h"
#include <Arduino.h>
#include <Preferences.h>

const char* idleModeName(IdleMode mode) {
  switch (mode) {
    case IdleMode::kDim:
      return "Dim";
    case IdleMode::kOff:
      return "Display Off";
    case IdleMode::kAlwaysOn:
      return "Always On";
    case IdleMode::kEco:
      return "Eco";
  }
  return "Dim";
}

IdleMode nextIdleMode(IdleMode mode) {
  switch (mode) {
    case IdleMode::kDim:
      return IdleMode::kOff;
    case IdleMode::kOff:
      return IdleMode::kAlwaysOn;
    case IdleMode::kAlwaysOn:
      return IdleMode::kEco;
    case IdleMode::kEco:
      return IdleMode::kDim;
  }
  return IdleMode::kDim;
}

void loadSettings() {
  bool info_seen = false;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    display_flipped = prefs.getBool(kPrefsFlipKey, false);
    if (prefs.isKey(kPrefsIdleModeKey)) {
      const uint8_t saved_idle_mode = prefs.getUChar(kPrefsIdleModeKey, static_cast<uint8_t>(IdleMode::kDim));
      idle_mode = saved_idle_mode <= static_cast<uint8_t>(IdleMode::kEco)
                      ? static_cast<IdleMode>(saved_idle_mode)
                      : IdleMode::kDim;
    } else {
      idle_mode = prefs.getBool(kPrefsIdleOffKey, false) ? IdleMode::kOff : IdleMode::kDim;
    }
    info_seen = prefs.getBool(kPrefsInfoSeenKey, false);
    prefs.end();
  }
  show_info_on_first_boot = !info_seen;
#if WLED_TOUCH_SIMULATOR
  // Browser and desktop simulators do not retain device preferences reliably
  // between sessions. Start at the primary Power view instead of treating every
  // launch as a first boot and selecting the index shared by the Settings tab.
  show_info_on_first_boot = false;
#endif
  Serial.printf("Display orientation: %s\n", display_flipped ? "flipped" : "normal");
  Serial.printf("Display idle action: %s\n", idleModeName(idle_mode));
}

void markInfoTabSeen() {
  if (!show_info_on_first_boot) {
    return;
  }

  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putBool(kPrefsInfoSeenKey, true);
    prefs.end();
  }
  show_info_on_first_boot = false;
}

void saveSettings() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putBool(kPrefsFlipKey, display_flipped);
    prefs.putUChar(kPrefsIdleModeKey, static_cast<uint8_t>(idle_mode));
    prefs.end();
  }
}
