#pragma once

#include <cstdint>

#include "app_state.h"

// LVGL is the sole renderer.  This header deliberately exposes only the small
// hardware boundary needed by the application; no graphics-library types leak
// into the rest of the code.
void displayPrepareForBoot();
void initDisplay();
bool displayHardwareReady();
void finishDisplaySplash();
void displayRestart();
void applyDisplayRotation();
void touchActivity();
void displayUpdateIdle(uint32_t now);
uint32_t displayTakeFlushMs();
void displayClear(uint16_t rgb565 = 0);
void displaySetBrightness(uint8_t brightness);
bool displaySupportsBatteryMonitor();
void displayReadLineRgb888(uint16_t y, uint8_t* rgb, uint16_t width);

#if WLED_TOUCH_SIMULATOR
extern uint16_t sim_framebuffer[kScreenWidth * kScreenHeight];
void simulatorSetTouch(bool down, int16_t x, int16_t y);
#endif
