#pragma once

#include "app_config.h"
#include <cstdint>
#include <lvgl.h>

// ── Layout ──────────────────────────────────────────────────────────────────

constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 240;
constexpr int kTopBarHeight = 30;
constexpr int kTabButtonHeight = 30;
constexpr int kPagePadding = 8;
constexpr int kTabCardHeight = kScreenHeight - kTopBarHeight - kTabButtonHeight - (2 * kPagePadding);
constexpr size_t kLvglBufferLines = 40;

// ── Protocol ────────────────────────────────────────────────────────────────

constexpr uint8_t kWledTouchButtonOn = 1;
constexpr uint8_t kWledTouchButtonOff = 2;
constexpr uint8_t kWledTouchButtonBrightDown = 8;
constexpr uint8_t kWledTouchButtonBrightUp = 9;
constexpr uint8_t kWledTouchButtonOne = 16;
constexpr uint8_t kBasicPresetCount = 7;
constexpr uint8_t kExtendedPresetCount = 30;
constexpr uint8_t kRemoteActionFirst = 50;
constexpr uint8_t kRemoteColorFirst = 70;
constexpr uint8_t kInfoTabIndex = 3;
constexpr uint8_t kSettingsTabIndex = 4;
constexpr uint8_t kBroadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Preferences keys ────────────────────────────────────────────────────────

constexpr const char* kPrefsNamespace = "wled-cyd";
constexpr const char* kPrefsFlipKey = "flip";
constexpr const char* kPrefsIdleOffKey = "idleOff";
constexpr const char* kPrefsIdleModeKey = "idleMode";
constexpr const char* kPrefsExtendedKey = "extended";
constexpr const char* kPrefsInfoSeenKey = "infoSeen";

// ── Colors ──────────────────────────────────────────────────────────────────

constexpr uint32_t kColorBg = 0x0A0E13;
constexpr uint32_t kColorHeaderBar = 0x10171F;
constexpr uint32_t kColorSurface = 0x161F29;
constexpr uint32_t kColorSurfaceRaised = 0x1F2A36;
constexpr uint32_t kColorSurfacePressed = 0x2A3947;
constexpr uint32_t kColorBorder = 0x29384A;
constexpr uint32_t kColorBorderStrong = 0x3A4C5E;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorTextMuted = 0x7E93A6;
constexpr uint32_t kColorAccent = 0x22D3EE;
constexpr uint32_t kColorAccentBright = 0x67E8F9;
constexpr uint32_t kColorAccentDeep = 0x0E7490;
constexpr uint32_t kColorSelected = 0x0F766E;
constexpr uint32_t kColorSelectedBorder = 0x2DD4BF;
constexpr uint32_t kColorOk = 0x34D399;
constexpr uint32_t kColorWarn = 0xFACC15;
constexpr uint32_t kColorDanger = 0xF87171;
constexpr uint32_t kColorBatteryOk = 0x34C759;

// ── Types ───────────────────────────────────────────────────────────────────

struct WledTouchPacket {
  uint8_t program;
  uint8_t seq[4];
  uint8_t dt1;
  uint8_t button;
  uint8_t dt2;
  uint8_t batLevel;
  uint8_t byte10;
  uint8_t byte11;
  uint8_t byte12;
  uint8_t byte13;
} __attribute__((packed));

struct RemoteState {
  bool power = false;
  uint8_t brightness = 255;
};

struct ColorSwatch {
  const char* label;
  uint8_t button;
  uint32_t color;
  bool dark_text;
};

struct RemoteControlPair {
  const char* label;
  uint8_t down_button;
  uint8_t up_button;
};

enum class IdleMode : uint8_t {
  kDim,
  kOff,
  kAlwaysOn,
};

enum class StatusCode : uint8_t {
  kBoot,
  kOffline,
  kSent,
  kOk,
  kNoAck,
  kSendError,
  kEspFail,
  kPeerError,
  kBroadcast,
  kReady,
};

// ── Data tables ─────────────────────────────────────────────────────────────

constexpr RemoteControlPair kFxControls[] = {
    {"Palette", kRemoteActionFirst, static_cast<uint8_t>(kRemoteActionFirst + 1)},
    {"Speed", static_cast<uint8_t>(kRemoteActionFirst + 2), static_cast<uint8_t>(kRemoteActionFirst + 3)},
    {"Intensity", static_cast<uint8_t>(kRemoteActionFirst + 4), static_cast<uint8_t>(kRemoteActionFirst + 5)},
    {"Custom Slider 1", static_cast<uint8_t>(kRemoteActionFirst + 6), static_cast<uint8_t>(kRemoteActionFirst + 7)},
};

constexpr ColorSwatch kColorSwatches[] = {
    {"Warm", kRemoteColorFirst, 0xFFC078, false},
    {"White", static_cast<uint8_t>(kRemoteColorFirst + 1), 0xFFFFFF, true},
    {"Red", static_cast<uint8_t>(kRemoteColorFirst + 2), 0xEF4444, false},
    {"Orange", static_cast<uint8_t>(kRemoteColorFirst + 3), 0xF97316, false},
    {"Yellow", static_cast<uint8_t>(kRemoteColorFirst + 4), 0xFACC15, true},
    {"Green", static_cast<uint8_t>(kRemoteColorFirst + 5), 0x22C55E, true},
    {"Cyan", static_cast<uint8_t>(kRemoteColorFirst + 6), 0x06B6D4, true},
    {"Blue", static_cast<uint8_t>(kRemoteColorFirst + 7), 0x2563EB, false},
    {"Purple", static_cast<uint8_t>(kRemoteColorFirst + 8), 0xA855F7, false},
    {"Pink", static_cast<uint8_t>(kRemoteColorFirst + 9), 0xEC4899, false},
};

// ── Shared mutable state ─────────────────────────────────────────────────────

extern RemoteState state;
extern uint8_t selected_preset;
extern bool display_flipped;
extern IdleMode idle_mode;
extern bool extended_mode;
extern bool show_info_on_first_boot;

// ── LVGL widget handles ──────────────────────────────────────────────────────

extern lv_obj_t* main_tabs;
extern lv_obj_t* presets_tab;
extern lv_obj_t* fx_tab;
extern lv_obj_t* preset_buttons[kExtendedPresetCount];
extern lv_obj_t* power_button;
extern lv_obj_t* power_button_label;
extern lv_obj_t* brightness_label;
extern lv_obj_t* mac_label;
extern lv_obj_t* orientation_label;
extern lv_obj_t* idle_label;
extern lv_obj_t* mode_label;
extern lv_obj_t* help_dialog;

#if WLED_CYD_ENABLE_BATTERY
extern lv_obj_t* battery_indicator;
extern lv_obj_t* battery_fill;
extern lv_obj_t* battery_charge;
#endif

// ── LVGL styles ──────────────────────────────────────────────────────────────

extern lv_style_t style_screen;
extern lv_style_t style_topbar;
extern lv_style_t style_panel;
extern lv_style_t style_section_header;
extern lv_style_t style_label_muted;
extern lv_style_t style_button;
extern lv_style_t style_button_pressed;
extern lv_style_t style_button_checked;
extern lv_style_t style_slider;
extern lv_style_t style_slider_indicator;
extern lv_style_t style_knob;
