#pragma once

#include "app_config.h"
#include <cstdint>
#include <lvgl.h>

// ── Layout ──────────────────────────────────────────────────────────────────

constexpr int kScreenWidth = WLED_SCREEN_WIDTH;
constexpr int kScreenHeight = WLED_SCREEN_HEIGHT;

// The JC4880P443 panel is ~1.5x the pixel density of a CYD, so every touch
// target and font gets its own large-screen size instead of the CYD pixels.
constexpr bool kLargeScreen = WLED_SCREEN_WIDTH >= 480;
constexpr lv_coord_t uiScaled(lv_coord_t compact, lv_coord_t large) {
  return kLargeScreen ? large : compact;
}

// WLED brightness is represented on the wire as 0–255; the UI presents the
// equivalent rounded percentage without changing that protocol value.
constexpr uint8_t brightnessPercent(uint8_t brightness) {
  return static_cast<uint8_t>((uint16_t(brightness) * 100U + 127U) / 255U);
}

constexpr int kTopBarHeight = uiScaled(30, 44);
constexpr int kTabButtonHeight = uiScaled(30, 52);
constexpr int kPagePadding = 8;
constexpr int kTabCardHeight = kScreenHeight - kTopBarHeight - kTabButtonHeight - (2 * kPagePadding);
constexpr size_t kLvglBufferLines = WLED_LVGL_BUFFER_LINES;

// Font roles; the large-screen sizes only exist in the P4 build (lv_conf.h).
#if WLED_SCREEN_WIDTH >= 480
#define UI_FONT_SMALL (&lv_font_montserrat_16)
#define UI_FONT_BODY (&lv_font_montserrat_18)
#define UI_FONT_HEADER (&lv_font_montserrat_24)
#define UI_FONT_TITLE (&lv_font_montserrat_24)
#define UI_FONT_BIG (&lv_font_montserrat_28)
#else
#define UI_FONT_SMALL (&lv_font_montserrat_12)
#define UI_FONT_BODY (&lv_font_montserrat_14)
#define UI_FONT_HEADER (&lv_font_montserrat_16)
#define UI_FONT_TITLE (&lv_font_montserrat_18)
#define UI_FONT_BIG (&lv_font_montserrat_20)
#endif

// ── Protocol ────────────────────────────────────────────────────────────────

constexpr uint8_t kWledTouchButtonOne = 16;
constexpr uint8_t kPresetSlotCount = 20;
constexpr uint8_t kInfoTabIndex = 4;
constexpr uint8_t kSettingsTabIndex = 4;

// ── Preferences keys ────────────────────────────────────────────────────────

constexpr const char* kPrefsNamespace = "wled-cyd";
constexpr const char* kPrefsFlipKey = "flip";
constexpr const char* kPrefsIdleOffKey = "idleOff";
constexpr const char* kPrefsIdleModeKey = "idleMode";
constexpr const char* kPrefsInfoSeenKey = "infoSeen";
constexpr const char* kPrefsHardwareProfileKey = "hwProfile";
constexpr const char* kPrefsHardwareSetupVersionKey = "hwSetupVer";
constexpr uint8_t kHardwareSetupVersion = 4;
constexpr const char* kPrefsWifiSsidKey = "wifiSsid";
constexpr const char* kPrefsWifiPassKey = "wifiPass";
// The last known channel lets the station try the AP before doing a full
// 2.4 GHz scan on the next boot.  It is only a hint: wifi_link falls back to
// an all-channel attempt if the router has moved it.
constexpr const char* kPrefsWifiChannelKey = "wifiChan";

constexpr size_t kMaxSsidLength = 32;
constexpr size_t kMaxWifiPassLength = 63;

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

enum class IdleMode : uint8_t {
  kDim,
  kOff,
  kAlwaysOn,
  kEco,
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

// ── Shared mutable state ─────────────────────────────────────────────────────

extern RemoteState state;
extern uint8_t selected_preset;
extern uint8_t selected_effect_id;
extern bool display_flipped;
extern IdleMode idle_mode;
extern bool show_info_on_first_boot;

// ── LVGL widget handles ──────────────────────────────────────────────────────

extern lv_obj_t* main_tabs;
extern lv_obj_t* presets_tab;
extern lv_obj_t* fx_tab;
extern lv_obj_t* preset_buttons[kPresetSlotCount];
extern lv_obj_t* power_button;
extern lv_obj_t* power_button_label;
extern lv_obj_t* brightness_label;
extern lv_obj_t* brightness_slider;
extern lv_obj_t* conn_label;
extern lv_obj_t* conn_detail_label;
extern lv_obj_t* target_label;
extern lv_obj_t* orientation_label;
extern lv_obj_t* idle_label;
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
