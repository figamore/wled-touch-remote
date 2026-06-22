#pragma once

#include "../app_state.h"

void createLiveTab(lv_obj_t* tab);
void createLooksTab(lv_obj_t* tab);
void rebuildPresetTab();
void createFxTab(lv_obj_t* tab);
void rebuildFxTab();
void createInfoTab(lv_obj_t* tab);
void createSettingsTab(lv_obj_t* tab);

#if WLED_CYD_ENABLE_BATTERY
void createBatteryIndicator(lv_obj_t* parent);
#endif
