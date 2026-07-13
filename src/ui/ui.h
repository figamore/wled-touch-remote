#pragma once

#include "../app_state.h"

// ── UI entry point ────────────────────────────────────────────────────────────

void createUi();
void initShutdownControl();
void pollShutdownControl();

// Reflect WLED state/catalogs received over the API onto the widgets. Called each loop.
void uiSyncFromModel();
void syncLivePeekSubscription();
void updatePeekStrip();

// ── Label updaters (also called from simulator) ───────────────────────────────

void updateOrientationLabel();
void updateIdleLabel();
void updateModeLabel();
void updateConnLabel();
void updateTargetLabel();

// ── LVGL event callbacks (registered in tabs.cpp) ────────────────────────────

void onPower(lv_event_t* event);
void onBrightness(lv_event_t* event);
void onPreset(lv_event_t* event);
void activateEffectId(uint8_t effect_id);
void onPing(lv_event_t* event);
void onRestart(lv_event_t* event);
void onShutdown(lv_event_t* event);
void onRemoteAction(lv_event_t* event);
void goToSettings(lv_event_t* event);
void onFlipDisplay(lv_event_t* event);
void onToggleIdleAction(lv_event_t* event);
void onToggleControlMode(lv_event_t* event);
