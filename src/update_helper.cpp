#include "update_helper.h"

#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>

#include "app_state.h"
#include "display.h"
#include "update_manager.h"
#include "wifi_link.h"

namespace updatehelper {
namespace {

constexpr const char* kUpdateBootKey = "updateBoot";

bool g_active = false;
bool g_check_started = false;
uint32_t g_started_at = 0;
updater::Snapshot g_seen;
lv_obj_t* g_message = nullptr;
lv_obj_t* g_message_view = nullptr;
lv_obj_t* g_primary = nullptr;
lv_obj_t* g_secondary = nullptr;
lv_obj_t* g_primary_label = nullptr;
lv_obj_t* g_secondary_label = nullptr;

void styleUpdateButton(lv_obj_t* button, bool primary) {
  constexpr lv_style_selector_t kPressed =
      static_cast<lv_style_selector_t>(LV_PART_MAIN) |
      static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
  const uint32_t background = primary ? kColorAccent : kColorSurfaceRaised;
  const uint32_t pressed = primary ? kColorAccentBright : kColorSurfacePressed;
  const uint32_t foreground = primary ? 0x062029 : kColorText;

  // This restart-isolated screen does not create the normal application UI,
  // so it cannot rely on initStyles() replacing LVGL's blue default buttons.
  lv_obj_set_style_bg_color(button, lv_color_hex(background), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(button, lv_color_hex(foreground), LV_PART_MAIN);
  lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(button,
      lv_color_hex(primary ? kColorAccentDeep : kColorBorderStrong), LV_PART_MAIN);
  lv_obj_set_style_radius(button, 9, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(pressed), kPressed);
  lv_obj_set_style_text_color(button, lv_color_hex(foreground), kPressed);
}

void restartNormal() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.remove(kUpdateBootKey);
    prefs.end();
  }
  displayRestart();
}

void setButton(lv_obj_t* button, lv_obj_t* label, const char* text, bool visible, bool enabled = true) {
  if (!button || !label) return;
  lv_label_set_text(label, text);
  if (visible) lv_obj_clear_flag(button, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
  if (enabled) lv_obj_clear_state(button, LV_STATE_DISABLED);
  else lv_obj_add_state(button, LV_STATE_DISABLED);
}

void onPrimary(lv_event_t*) {
  const updater::Snapshot update = updater::snapshot();
  if (update.state == updater::State::kUpdateAvailable) updater::installAvailableUpdate();
  else if (update.state == updater::State::kFailed) updater::checkForUpdates();
  else if (update.state == updater::State::kSuccess) displayRestart();
  else if (update.state == updater::State::kUpToDate) restartNormal();
}

void onSecondary(lv_event_t*) { restartNormal(); }

void render() {
  const updater::Snapshot update = updater::snapshot();
  if (memcmp(&update, &g_seen, sizeof(update)) == 0) return;
  g_seen = update;

  char message[460] = {};
  switch (update.state) {
    case updater::State::kChecking:
      snprintf(message, sizeof(message), "Checking for updates...");
      break;
    case updater::State::kUpdateAvailable:
      snprintf(message, sizeof(message), "Update %s is available.\n\n%s", update.available_version, update.release_notes);
      break;
    case updater::State::kDownloading:
      snprintf(message, sizeof(message), "Downloading firmware... %u%%", unsigned(update.progress));
      break;
    case updater::State::kInstalling:
      // Writing the image takes far longer than the CYD's; without a live
      // percentage the screen looks hung and invites a power cycle mid-write.
      snprintf(message, sizeof(message), "%s %u%%", update.message, unsigned(update.progress));
      break;
    case updater::State::kVerifying:
    case updater::State::kRestarting:
      snprintf(message, sizeof(message), "%s", update.message);
      break;
    case updater::State::kUpToDate:
      snprintf(message, sizeof(message), "Your device is up to date.");
      break;
    case updater::State::kFailed:
      snprintf(message, sizeof(message), "%s", update.message);
      break;
    default:
      snprintf(message, sizeof(message), "%s", update.message);
      break;
  }
  lv_label_set_text(g_message, message);
  // Each state change starts its message at the top; only this view scrolls.
  if (g_message_view) lv_obj_scroll_to_y(g_message_view, 0, LV_ANIM_OFF);

  const bool working = updater::busy();
  if (update.state == updater::State::kUpdateAvailable) {
    setButton(g_primary, g_primary_label, "Install", true);
    setButton(g_secondary, g_secondary_label, "Later", true);
  } else if (update.state == updater::State::kFailed) {
    setButton(g_primary, g_primary_label, "Retry", true, !working);
    setButton(g_secondary, g_secondary_label, "Back", true, !working);
  } else if (update.state == updater::State::kUpToDate) {
    setButton(g_primary, g_primary_label, "Back", true);
    setButton(g_secondary, g_secondary_label, "", false);
  } else if (update.state == updater::State::kSuccess) {
    setButton(g_primary, g_primary_label, "Continue", true);
    setButton(g_secondary, g_secondary_label, "", false);
  } else {
    setButton(g_primary, g_primary_label, "", false);
    setButton(g_secondary, g_secondary_label, "", false);
  }
}

}  // namespace

bool requested() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) return false;
  const bool value = prefs.getBool(kUpdateBootKey, false);
  // This flag is intentionally one-shot: an OTA-success restart must return
  // to the normal firmware, rather than re-enter this helper indefinitely.
  if (value) prefs.remove(kUpdateBootKey);
  prefs.end();
  return value;
}

void request() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putBool(kUpdateBootKey, true);
    prefs.end();
  }
}

bool active() { return g_active; }

void begin() {
  g_active = true;
  lv_obj_t* screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_hex(kColorBg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t* panel = lv_obj_create(screen);
  lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(panel, uiScaled(20, 40), LV_PART_MAIN);
  lv_obj_set_style_bg_color(panel, lv_color_hex(kColorSurface), LV_PART_MAIN);
  lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  const lv_coord_t title_height = lv_font_get_line_height(UI_FONT_TITLE);
  const lv_coord_t actions_height = uiScaled(48, 64);
  const lv_coord_t gap = uiScaled(12, 20);
  const lv_coord_t message_height = kScreenHeight - 2 * uiScaled(20, 40) - title_height - actions_height - 2 * gap;

  lv_obj_t* title = lv_label_create(panel);
  lv_label_set_text(title, "Software Update");
  lv_obj_set_style_text_font(title, UI_FONT_TITLE, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  // Keep long release notes contained between the fixed title and actions.
  // The panel itself never scrolls, preventing notes from moving behind the
  // Back/Install buttons.
  g_message_view = lv_obj_create(panel);
  lv_obj_set_size(g_message_view, LV_PCT(100), message_height);
  lv_obj_align(g_message_view, LV_ALIGN_TOP_MID, 0, title_height + gap);
  lv_obj_set_style_pad_all(g_message_view, uiScaled(8, 14), LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_message_view, lv_color_hex(kColorSurfaceRaised), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_message_view, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(g_message_view, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(g_message_view, lv_color_hex(kColorBorder), LV_PART_MAIN);
  lv_obj_set_style_radius(g_message_view, 9, LV_PART_MAIN);
  lv_obj_set_scroll_dir(g_message_view, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(g_message_view, LV_SCROLLBAR_MODE_AUTO);

  g_message = lv_label_create(g_message_view);
  lv_obj_set_width(g_message, LV_PCT(100));
  lv_label_set_long_mode(g_message, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(g_message, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

  lv_obj_t* actions = lv_obj_create(panel);
  lv_obj_remove_style_all(actions);
  lv_obj_set_size(actions, LV_PCT(100), actions_height);
  lv_obj_align(actions, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
  g_secondary = lv_btn_create(actions);
  styleUpdateButton(g_secondary, false);
  lv_obj_set_size(g_secondary, uiScaled(120, 200), LV_PCT(100));
  lv_obj_add_event_cb(g_secondary, onSecondary, LV_EVENT_CLICKED, nullptr);
  g_secondary_label = lv_label_create(g_secondary);
  lv_obj_set_style_text_color(g_secondary_label, lv_color_hex(kColorText), LV_PART_MAIN);
  lv_obj_center(g_secondary_label);
  g_primary = lv_btn_create(actions);
  styleUpdateButton(g_primary, true);
  lv_obj_set_size(g_primary, uiScaled(120, 200), LV_PCT(100));
  lv_obj_add_event_cb(g_primary, onPrimary, LV_EVENT_CLICKED, nullptr);
  g_primary_label = lv_label_create(g_primary);
  lv_obj_set_style_text_color(g_primary_label, lv_color_hex(0x062029), LV_PART_MAIN);
  lv_obj_center(g_primary_label);

  g_check_started = false;
  g_started_at = millis();
  lv_label_set_text(g_message, "Connecting to Wi-Fi...");
  setButton(g_primary, g_primary_label, "", false);
  setButton(g_secondary, g_secondary_label, "Back", true);
}

void loop(uint32_t now_ms) {
  // Starting TLS before association completes wastes scarce internal RAM on
  // ESP32 CYD boards and makes the outcome needlessly timing-dependent.
  if (!g_check_started) {
    if (wifilink::connected()) {
      updater::checkForUpdates();
      g_check_started = true;
    } else {
      const bool timed_out = now_ms - g_started_at >= 20000;
      lv_label_set_text(g_message, timed_out
          ? "Wi-Fi is unavailable. Return to connect, then try again."
          : "Connecting to Wi-Fi...");
      setButton(g_primary, g_primary_label, "", false);
      setButton(g_secondary, g_secondary_label, "Back", true);
      return;
    }
  }
  render();
}

}  // namespace updatehelper
