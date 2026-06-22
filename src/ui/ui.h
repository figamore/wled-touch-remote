#pragma once

#include "../app_state.h"

// ── UI entry point ────────────────────────────────────────────────────────────

void createUi();

// ── Label updaters (also called from simulator) ───────────────────────────────

void updateOrientationLabel();
void updateIdleLabel();
void updateModeLabel();

// ── LVGL event callbacks (registered in tabs.cpp) ────────────────────────────

void onPower(lv_event_t* event);
void onBrightness(lv_event_t* event);
void onPreset(lv_event_t* event);
void activateEffect(const WledEffectInfo* effect);
void onPing(lv_event_t* event);
void onRestart(lv_event_t* event);
void onRemoteAction(lv_event_t* event);
void goToSettings(lv_event_t* event);
void onFlipDisplay(lv_event_t* event);
void onToggleIdleAction(lv_event_t* event);
void onToggleControlMode(lv_event_t* event);
