#pragma once

#include "../app_state.h"

void createLiveTab(lv_obj_t* tab);
void createPresetsTab(lv_obj_t* tab);
void createColorsTab(lv_obj_t* tab);
void createFxTab(lv_obj_t* tab);
void createSettingsTab(lv_obj_t* tab);
void updateColorControlsFromModel();
void updateStatusFromModel();
void rebuildPresetTab();
void rebuildFxTab();

#if WLED_CYD_ENABLE_BATTERY
void createBatteryIndicator(lv_obj_t* parent);
#endif

#if WLED_TOUCH_SIMULATOR
// Test hooks: open the modals directly so automated sim tests don't depend on
// tap coordinates.
void simulatorOpenFxControls();
void simulatorOpenPaletteChooser();
#endif
