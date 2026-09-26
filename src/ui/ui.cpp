#include "ui.h"
#include "tabs.h"
#include "../display.h"
#include "../settings.h"
#include "../BatteryMonitor.h"
#include "../wifi_link.h"
#include "../wled_api.h"
#include <Arduino.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace {

#if WLED_CYD_ENABLE_SHUTDOWN
constexpr uint8_t kShutdownCountdownSteps = 10;
#endif

lv_obj_t* peek_bar = nullptr;
lv_obj_t* header_target_label = nullptr;
lv_obj_t* header_connection_dot = nullptr;
lv_obj_t* header_activity_spinner = nullptr;
lv_obj_t* header_wifi_bars[4] = {nullptr, nullptr, nullptr, nullptr};
lv_obj_t* header_wifi_offline_mark = nullptr;
uint32_t header_dot_color = UINT32_MAX;
int8_t header_wifi_bars_lit = -1;
uint32_t header_wifi_color = UINT32_MAX;
int8_t header_wifi_offline = -1;
constexpr uint8_t kFxTabIndex = 2;
constexpr uint8_t kColorsTabIndex = 3;
#if WLED_CYD_ENABLE_SHUTDOWN
lv_obj_t* shutdown_overlay = nullptr;
lv_obj_t* shutdown_countdown_label = nullptr;
lv_timer_t* shutdown_timer = nullptr;
uint32_t shutdown_started_ms = 0;
bool shutdown_button_last_read_pressed = false;
uint32_t shutdown_last_tap_ms = 0;
uint32_t shutdown_last_press_edge_ms = 0;
uint32_t shutdown_last_debug_status_ms = 0;
#endif


// Re-centers the FX list whenever the user navigates back to the FX tab.
void onMainTabChanged(lv_event_t* event) {
  if (main_tabs && lv_event_get_target(event) == main_tabs &&
      lv_tabview_get_tab_act(main_tabs) == kFxTabIndex) {
    revealSelectedEffect(true);
  }
  if (main_tabs && lv_event_get_target(event) == main_tabs &&
      lv_tabview_get_tab_act(main_tabs) == kColorsTabIndex) {
    checkColorWheelEditor();
  }
}

// The header occupies the same horizontal space as the tab strip. Give every
// tab a transparent hit target there so it can be reached without needing to
// tap its small label.
void onHeaderTabClicked(lv_event_t* event) {
  if (!main_tabs) return;

  // Store an offset index so the Power tab does not need a null user-data
  // pointer (LVGL uses null to mean "no user data").
  const uintptr_t tab_id = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (tab_id == 0) return;
  const uint8_t tab_index = static_cast<uint8_t>(tab_id - 1);
  if (tab_index == lv_tabview_get_tab_act(main_tabs)) return;

  lv_tabview_set_act(main_tabs, tab_index, LV_ANIM_OFF);
  // lv_tabview_set_act() intentionally does not emit this event itself, while
  // a press on the real tab buttons does.  Keep both navigation paths alike.
  lv_event_send(main_tabs, LV_EVENT_VALUE_CHANGED, nullptr);
}

void createHeaderTabTargets(lv_obj_t* topbar, lv_obj_t* tab_buttons) {
  lv_obj_update_layout(tab_buttons);

  const auto* button_matrix = reinterpret_cast<const lv_btnmatrix_t*>(tab_buttons);
  if (!button_matrix->button_areas || button_matrix->btn_cnt == 0) return;

  lv_area_t buttons_area;
  lv_area_t topbar_area;
  lv_obj_get_coords(tab_buttons, &buttons_area);
  lv_obj_get_coords(topbar, &topbar_area);

  for (uint16_t i = 0; i < button_matrix->btn_cnt; ++i) {
    // Button-matrix areas are local to the tab strip.
    const lv_coord_t x1 = buttons_area.x1 + button_matrix->button_areas[i].x1;
    const lv_coord_t x2 = buttons_area.x1 + button_matrix->button_areas[i].x2;

    // Use the top layer so these invisible targets stay above every header
    // child. Dialogs created later on that layer still appear above them.
    lv_obj_t* hit_target = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(hit_target);
    lv_obj_set_pos(hit_target, x1, 0);
    lv_obj_set_size(hit_target, x2 - x1 + 1, topbar_area.y2 + 1);
    lv_obj_clear_flag(hit_target, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hit_target, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit_target, onHeaderTabClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1)));
  }
}


#if WLED_CYD_ENABLE_SHUTDOWN
void startShutdownUi();
#endif

#if WLED_CYD_ENABLE_SHUTDOWN && WLED_CYD_SHUTDOWN_DEBUG
const char* shutdownLevelName(bool pressed) {
  return pressed ? "LOW/pressed" : "HIGH/released";
}
#endif

void releaseShutdownKey() {
#if WLED_CYD_ENABLE_SHUTDOWN
#if WLED_CYD_SHUTDOWN_PULLUP
  pinMode(WLED_CYD_SHUTDOWN_GPIO, INPUT_PULLUP);
#else
  pinMode(WLED_CYD_SHUTDOWN_GPIO, INPUT);
#endif
#if WLED_CYD_SHUTDOWN_DEBUG
  Serial.printf("Shutdown GPIO %d released to %s\n",
                WLED_CYD_SHUTDOWN_GPIO,
                WLED_CYD_SHUTDOWN_PULLUP ? "INPUT_PULLUP" : "INPUT");
#endif
#endif
}

void holdShutdownKey() {
#if WLED_CYD_ENABLE_SHUTDOWN
#if WLED_CYD_SHUTDOWN_DEBUG
  Serial.printf("Shutdown GPIO %d driven LOW with OUTPUT_OPEN_DRAIN\n", WLED_CYD_SHUTDOWN_GPIO);
#endif
  digitalWrite(WLED_CYD_SHUTDOWN_GPIO, LOW);
  pinMode(WLED_CYD_SHUTDOWN_GPIO, OUTPUT_OPEN_DRAIN);
#endif
}

bool readShutdownButtonPressed() {
#if WLED_CYD_ENABLE_SHUTDOWN
  return digitalRead(WLED_CYD_SHUTDOWN_GPIO) == LOW;
#else
  return false;
#endif
}

void syncShutdownButtonState(const char* reason) {
#if WLED_CYD_ENABLE_SHUTDOWN
  const bool pressed = readShutdownButtonPressed();
  shutdown_button_last_read_pressed = pressed;
  const uint32_t now = millis();
  shutdown_last_tap_ms = 0;
  shutdown_last_press_edge_ms = 0;
  shutdown_last_debug_status_ms = now;
#if WLED_CYD_SHUTDOWN_DEBUG
  Serial.printf("Shutdown GPIO state sync (%s): %s\n", reason, shutdownLevelName(pressed));
#endif
#endif
}

void handleShutdownTap(uint32_t now) {
#if WLED_CYD_ENABLE_SHUTDOWN
  if (shutdown_last_press_edge_ms != 0 &&
      now - shutdown_last_press_edge_ms < WLED_CYD_SHUTDOWN_TAP_LOCKOUT_MS) {
#if WLED_CYD_SHUTDOWN_DEBUG
    Serial.printf("Shutdown GPIO press pulse ignored: lockout delta=%ums\n",
                  now - shutdown_last_press_edge_ms);
#endif
    shutdown_last_press_edge_ms = now;
    return;
  }

  shutdown_last_press_edge_ms = now;

  if (shutdown_last_tap_ms != 0 &&
      now - shutdown_last_tap_ms <= WLED_CYD_SHUTDOWN_DOUBLE_TAP_MS) {
#if WLED_CYD_SHUTDOWN_DEBUG
    Serial.printf("Shutdown GPIO double tap detected: delta=%ums\n", now - shutdown_last_tap_ms);
#endif
    shutdown_last_tap_ms = 0;
    touchActivity();
    startShutdownUi();
    return;
  }

  shutdown_last_tap_ms = now;
#if WLED_CYD_SHUTDOWN_DEBUG
  Serial.printf("Shutdown GPIO first tap at %ums\n", now);
#endif
#endif
}

void closeShutdownOverlay() {
#if WLED_CYD_ENABLE_SHUTDOWN
  if (shutdown_timer) {
    lv_timer_del(shutdown_timer);
    shutdown_timer = nullptr;
  }
  if (shutdown_overlay) {
    lv_obj_del(shutdown_overlay);
    shutdown_overlay = nullptr;
  }
  shutdown_countdown_label = nullptr;
#endif
}

void abortShutdown(lv_event_t*) {
#if WLED_CYD_ENABLE_SHUTDOWN
  releaseShutdownKey();
  delay(5);
  syncShutdownButtonState("abort");
  closeShutdownOverlay();
#endif
}

void updateShutdownCountdown(lv_timer_t*) {
#if WLED_CYD_ENABLE_SHUTDOWN
  if (!shutdown_countdown_label) {
    return;
  }

  const uint32_t elapsed_ms = millis() - shutdown_started_ms;
  const uint32_t step_ms = WLED_CYD_SHUTDOWN_HOLD_MS / kShutdownCountdownSteps;
  if (elapsed_ms < WLED_CYD_SHUTDOWN_HOLD_MS) {
    uint8_t count = (WLED_CYD_SHUTDOWN_HOLD_MS - elapsed_ms + step_ms - 1) / step_ms;
    if (count > kShutdownCountdownSteps) {
      count = kShutdownCountdownSteps;
    }
    if (count < 1) {
      count = 1;
    }
    lv_label_set_text_fmt(shutdown_countdown_label, "%u", count);
    return;
  }

  lv_label_set_text(shutdown_countdown_label, "...");
#endif
}

void startShutdownUi() {
#if WLED_CYD_ENABLE_SHUTDOWN
  if (shutdown_overlay) {
    return;
  }

  shutdown_started_ms = millis();
  holdShutdownKey();

  shutdown_overlay = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(shutdown_overlay);
  lv_obj_set_size(shutdown_overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(shutdown_overlay, lv_color_hex(kColorBg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(shutdown_overlay, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_flex_flow(shutdown_overlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(shutdown_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(shutdown_overlay, 18, LV_PART_MAIN);
  lv_obj_clear_flag(shutdown_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_move_foreground(shutdown_overlay);

  lv_obj_t* title = lv_label_create(shutdown_overlay);
  lv_label_set_text(title, "Shutting down");
  lv_obj_set_style_text_color(title, lv_color_hex(kColorAccent), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);

  shutdown_countdown_label = lv_label_create(shutdown_overlay);
  lv_label_set_text(shutdown_countdown_label, "10");
  lv_obj_set_style_text_color(shutdown_countdown_label, lv_color_hex(kColorText), LV_PART_MAIN);
#if LV_FONT_MONTSERRAT_48
  lv_obj_set_style_text_font(shutdown_countdown_label, &lv_font_montserrat_48, LV_PART_MAIN);
#else
  lv_obj_set_style_text_font(shutdown_countdown_label, &lv_font_montserrat_20, LV_PART_MAIN);
#endif
  lv_obj_set_style_text_align(shutdown_countdown_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_size(shutdown_countdown_label, LV_PCT(100), 64);

  lv_obj_t* abort = lv_btn_create(shutdown_overlay);
  lv_obj_add_style(abort, &style_button, LV_PART_MAIN);
  lv_obj_add_style(abort, &style_button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_size(abort, 104, 38);
  lv_obj_add_event_cb(abort, abortShutdown, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* abort_label = lv_label_create(abort);
  lv_label_set_text(abort_label, "Abort");
  lv_obj_center(abort_label);

  shutdown_timer = lv_timer_create(updateShutdownCountdown, 100, nullptr);
  updateShutdownCountdown(nullptr);
#endif
}

// Live peek: renders the latest LED frame streamed from WLED into the top bar,
// the same way the web UI's Peek strip works.
void drawPeekBar(lv_event_t* event) {
  lv_draw_ctx_t* draw_ctx = lv_event_get_draw_ctx(event);
  if (!draw_ctx) return;

  uint16_t count = 0, width = 0, height = 0;
  const uint8_t* leds = wled::liveLeds(count, width, height);
  if (!leds || !count) return;

  lv_area_t coords;
  lv_obj_get_coords(lv_event_get_target(event), &coords);
  coords.x1 += 3;
  coords.x2 -= 3;
  coords.y1 += 3;
  coords.y2 -= 3;
  const lv_coord_t strip_width = lv_area_get_width(&coords);
  if (strip_width <= 0) return;

  const lv_coord_t segments = std::min<lv_coord_t>(strip_width, std::min<uint16_t>(count, 64));
  lv_draw_rect_dsc_t rect;
  lv_draw_rect_dsc_init(&rect);
  rect.bg_opa = LV_OPA_COVER;
  rect.border_width = 0;
  rect.radius = 0;

  for (lv_coord_t i = 0; i < segments; ++i) {
    const size_t led = size_t(i) * count / segments;
    const uint8_t* p = leds + led * 3;
    rect.bg_color = lv_color_make(p[0], p[1], p[2]);
    lv_area_t seg = coords;
    seg.x1 = coords.x1 + (int32_t(i) * strip_width) / segments;
    seg.x2 = coords.x1 + (int32_t(i + 1) * strip_width) / segments - 1;
    if (i == segments - 1) seg.x2 = coords.x2;
    lv_draw_rect(draw_ctx, &rect, &seg);
  }
}

void setPowerUi(bool power) {
  state.power = power;
  if (power_button) {
    if (state.power) {
      lv_obj_add_state(power_button, LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(power_button, LV_STATE_CHECKED);
    }
  }
  if (power_button_label) {
    lv_label_set_text(power_button_label,
                      state.power ? LV_SYMBOL_POWER "  Power On" : LV_SYMBOL_POWER "  Power Off");
  }
}

void setSelectedPreset(uint8_t preset) {
  selected_preset = preset;
  refreshPresetSelection();
}

void setSelectedEffect(uint8_t effect_id) {
  selected_effect_id = effect_id;
}

void initStyles() {
  static const lv_style_prop_t kButtonTransProps[] = {
      LV_STYLE_BG_COLOR, LV_STYLE_BG_OPA, LV_STYLE_BORDER_COLOR,
      LV_STYLE_TEXT_COLOR,
      static_cast<lv_style_prop_t>(0)};
  static lv_style_transition_dsc_t button_trans;
  lv_style_transition_dsc_init(&button_trans, kButtonTransProps,
                               lv_anim_path_ease_out, 150, 0, nullptr);

  lv_style_init(&style_screen);
  lv_style_set_bg_color(&style_screen, lv_color_hex(kColorBg));
  lv_style_set_text_color(&style_screen, lv_color_hex(kColorText));

  lv_style_init(&style_topbar);
  lv_style_set_bg_color(&style_topbar, lv_color_hex(kColorHeaderBar));
  lv_style_set_bg_opa(&style_topbar, LV_OPA_COVER);
  lv_style_set_border_width(&style_topbar, 0);
  lv_style_set_radius(&style_topbar, 0);
  lv_style_set_pad_left(&style_topbar, 10);
  lv_style_set_pad_right(&style_topbar, 10);

  lv_style_init(&style_panel);
  lv_style_set_bg_color(&style_panel, lv_color_hex(kColorSurface));
  lv_style_set_bg_opa(&style_panel, LV_OPA_COVER);
  lv_style_set_border_width(&style_panel, 1);
  lv_style_set_border_color(&style_panel, lv_color_hex(kColorBorder));
  lv_style_set_radius(&style_panel, 12);

  lv_style_init(&style_section_header);
  lv_style_set_text_color(&style_section_header, lv_color_hex(kColorText));
  lv_style_set_text_font(&style_section_header, UI_FONT_HEADER);
  lv_style_set_text_letter_space(&style_section_header, 1);

  lv_style_init(&style_label_muted);
  lv_style_set_text_color(&style_label_muted, lv_color_hex(kColorText));
  lv_style_set_text_font(&style_label_muted, UI_FONT_BODY);

  lv_style_init(&style_button);
  lv_style_set_bg_color(&style_button, lv_color_hex(kColorSurfaceRaised));
  lv_style_set_bg_opa(&style_button, LV_OPA_COVER);
  lv_style_set_border_width(&style_button, 1);
  lv_style_set_border_color(&style_button, lv_color_hex(kColorBorderStrong));
  lv_style_set_radius(&style_button, 9);
  lv_style_set_text_color(&style_button, lv_color_hex(kColorText));
  lv_style_set_transition(&style_button, &button_trans);

  lv_style_init(&style_button_pressed);
  lv_style_set_bg_color(&style_button_pressed, lv_color_hex(kColorSurfacePressed));
  lv_style_set_border_color(&style_button_pressed, lv_color_hex(kColorAccent));

  lv_style_init(&style_button_checked);
  lv_style_set_bg_color(&style_button_checked, lv_color_hex(kColorSelected));
  lv_style_set_border_color(&style_button_checked, lv_color_hex(kColorSelectedBorder));
  lv_style_set_text_color(&style_button_checked, lv_color_hex(0xFFFFFF));

  lv_style_init(&style_slider);
  lv_style_set_bg_color(&style_slider, lv_color_hex(0x223040));
  lv_style_set_bg_opa(&style_slider, LV_OPA_COVER);
  lv_style_set_radius(&style_slider, LV_RADIUS_CIRCLE);

  lv_style_init(&style_slider_indicator);
  lv_style_set_bg_color(&style_slider_indicator, lv_color_hex(kColorAccentDeep));
  lv_style_set_bg_grad_color(&style_slider_indicator, lv_color_hex(kColorAccent));
  lv_style_set_bg_grad_dir(&style_slider_indicator, LV_GRAD_DIR_HOR);
  lv_style_set_radius(&style_slider_indicator, LV_RADIUS_CIRCLE);

  lv_style_init(&style_knob);
  lv_style_set_bg_color(&style_knob, lv_color_hex(0xFFFFFF));
  lv_style_set_border_color(&style_knob, lv_color_hex(kColorAccent));
  lv_style_set_border_width(&style_knob, 2);
  lv_style_set_pad_all(&style_knob, uiScaled(5, 9));
}

}  // namespace

// ── Label updaters ────────────────────────────────────────────────────────────

void updateOrientationLabel() {
  if (orientation_label) {
    lv_label_set_text(orientation_label, display_flipped ? "Flipped" : "Normal");
  }
}

void updateIdleLabel() {
  if (idle_label) {
    lv_label_set_text(idle_label, idleModeName(idle_mode));
  }
}

// Signal strength in words; dBm alone means nothing to most people.
static const char* signalQuality(int rssi) {
  if (rssi >= -55) return "Excellent";
  if (rssi >= -67) return "Good";
  if (rssi >= -75) return "Fair";
  return "Weak";
}

// The panel refreshes on a timer, so only touch a label when its text changed;
// LVGL invalidates on every set regardless of whether anything moved.
static void setLabelTextIfChanged(lv_obj_t* label, const char* text) {
  const char* current = lv_label_get_text(label);
  if (current && strcmp(current, text) == 0) return;
  lv_label_set_text(label, text);
}

// Headline says what the remote is controlling; the second line carries the link
// facts behind it.  A Wi-Fi problem outranks anything WLED-side, since nothing
// downstream can work without it.
void updateConnLabel() {
  if (!conn_label || !conn_detail_label) return;

  const wifilink::Status wifi_status = wifilink::status();
  const wled::Model& m = wled::model();
  char headline[64];
  char detail[80];
  uint32_t color = kColorWarn;

  if (!wifilink::connected()) {
    switch (wifi_status) {
      case wifilink::Status::kNoCredentials:
        snprintf(headline, sizeof(headline), "No Wi-Fi connection");
        snprintf(detail, sizeof(detail), "Choose a Wi-Fi network to get started");
        color = kColorDanger;
        break;
      case wifilink::Status::kConnecting:
        snprintf(headline, sizeof(headline), "Connecting to Wi-Fi...");
        snprintf(detail, sizeof(detail), "Joining %s", wifilink::attemptSsid().c_str());
        break;
      case wifilink::Status::kRetrying:
        snprintf(headline, sizeof(headline), "Retrying Wi-Fi...");
        snprintf(detail, sizeof(detail), "Trying %s again", wifilink::attemptSsid().c_str());
        break;
      case wifilink::Status::kNotFound:
        snprintf(headline, sizeof(headline), "Wi-Fi network not found");
        snprintf(detail, sizeof(detail), "%s is unavailable. Retrying shortly", wifilink::attemptSsid().c_str());
        color = kColorDanger;
        break;
      case wifilink::Status::kBadPassword:
        snprintf(headline, sizeof(headline), "Wi-Fi connection failed");
        snprintf(detail, sizeof(detail), "Check the password for %s", wifilink::attemptSsid().c_str());
        color = kColorDanger;
        break;
      case wifilink::Status::kConnectionLost:
        snprintf(headline, sizeof(headline), "Wi-Fi connection lost");
        snprintf(detail, sizeof(detail), "Reconnecting to %s...", wifilink::attemptSsid().c_str());
        color = kColorDanger;
        break;
      default:
        snprintf(headline, sizeof(headline), "Wi-Fi connection failed");
        snprintf(detail, sizeof(detail), "Retrying %s...", wifilink::attemptSsid().c_str());
        color = kColorDanger;
        break;
    }
  } else {
    const int rssi = wifilink::rssi();
    snprintf(detail, sizeof(detail), "%s  %d dBm  %s", signalQuality(rssi), rssi,
             wifilink::ipAddress().c_str());
    switch (wled::connectionStatus()) {
      case wled::ConnectionStatus::kSearching:
        snprintf(headline, sizeof(headline), "Searching for WLED devices...");
        snprintf(detail, sizeof(detail), "Looking on the current Wi-Fi network");
        break;
      case wled::ConnectionStatus::kNoDevicesFound:
        snprintf(headline, sizeof(headline), "Searching for WLED...");
        snprintf(detail, sizeof(detail), "Searching this Wi-Fi network");
        color = kColorWarn;
        break;
      case wled::ConnectionStatus::kConnecting:
        {
          const bool found = wled::deviceCount() &&
                             wled::deviceInfo(wled::focusedDevice()).online;
          if (found) {
            snprintf(headline, sizeof(headline), "WLED found - connecting...");
            snprintf(detail, sizeof(detail), "Connecting to %s", m.name.empty() ? "WLED" : m.name.c_str());
          } else {
            snprintf(headline, sizeof(headline), "Checking saved device...");
            snprintf(detail, sizeof(detail), "Verifying the saved address");
          }
        }
        break;
      case wled::ConnectionStatus::kConnectionLost:
        snprintf(headline, sizeof(headline), "WLED connection lost");
        snprintf(detail, sizeof(detail), "Reconnecting to %s...", m.name.empty() ? "WLED" : m.name.c_str());
        color = kColorDanger;
        break;
      case wled::ConnectionStatus::kReconnecting:
        snprintf(headline, sizeof(headline), "Reconnecting to WLED...");
        snprintf(detail, sizeof(detail), "Trying %s again", m.name.empty() ? "WLED" : m.name.c_str());
        break;
      case wled::ConnectionStatus::kNoConnection:
        snprintf(headline, sizeof(headline), "No connection");
        snprintf(detail, sizeof(detail), "Connect Wi-Fi to find WLED devices");
        color = kColorDanger;
        break;
      case wled::ConnectionStatus::kConnected:
        break;
    }
    if (wled::connectionStatus() == wled::ConnectionStatus::kConnected &&
        wled::targetingAll() && wled::activeDeviceCount()) {
      snprintf(headline, sizeof(headline), "All controllers, %u online",
               unsigned(wled::activeDeviceCount()));
      color = kColorOk;
    } else if (wled::connectionStatus() == wled::ConnectionStatus::kConnected && m.online) {
      snprintf(headline, sizeof(headline), "Connected to %s", m.name.empty() ? "WLED" : m.name.c_str());
      color = kColorOk;
    }
  }

  setLabelTextIfChanged(conn_label, headline);
  setLabelTextIfChanged(conn_detail_label, detail);
  lv_obj_set_style_text_color(conn_label, lv_color_hex(color), LV_PART_MAIN);
}


// Updates the compact top-bar target name and summarizes connection health with a status dot.
void setHeaderDotColor(uint32_t color) {
  if (!header_connection_dot || color == header_dot_color) return;
  header_dot_color = color;
  lv_obj_set_style_bg_color(header_connection_dot, lv_color_hex(color), LV_PART_MAIN);
}

void setHiddenIfChanged(lv_obj_t* object, bool hidden) {
  if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == hidden) return;
  if (hidden) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
}

void updateHeaderTarget() {
  if (!header_target_label || !header_connection_dot) return;

  uint32_t dotColor = kColorDanger;
  if (!wifilink::connected()) {
    if (wifilink::scanning() || wifilink::scanQueued()) {
      lv_label_set_text(header_target_label, "Wi-Fi scan");
      dotColor = kColorWarn;
      setHeaderDotColor(dotColor);
      return;
    }
    const wifilink::Status status = wifilink::status();
    switch (status) {
      case wifilink::Status::kNoCredentials:
        lv_label_set_text(header_target_label, "No Wi-Fi");
        break;
      case wifilink::Status::kConnecting:
        lv_label_set_text(header_target_label, "Wi-Fi...");
        dotColor = kColorWarn;
        break;
      case wifilink::Status::kRetrying:
      case wifilink::Status::kNotFound:
      case wifilink::Status::kFailed:
      case wifilink::Status::kConnectionLost:
        lv_label_set_text(header_target_label, "Wi-Fi retry");
        dotColor = kColorWarn;
        break;
      case wifilink::Status::kBadPassword:
        lv_label_set_text(header_target_label, "Wi-Fi password");
        break;
      case wifilink::Status::kConnected:
        // connected() above already handles this state.
        break;
    }
  } else if (wled::connectionStatus() == wled::ConnectionStatus::kNoDevicesFound) {
    lv_label_set_text(header_target_label, "Searching");
    dotColor = kColorWarn;
  } else if (wled::connectionStatus() == wled::ConnectionStatus::kSearching) {
    lv_label_set_text(header_target_label, "Searching");
    dotColor = kColorWarn;
  } else if (wled::connectionStatus() == wled::ConnectionStatus::kReconnecting) {
    lv_label_set_text(header_target_label, "Reconnecting");
    dotColor = kColorWarn;
  } else if (wled::connectionStatus() == wled::ConnectionStatus::kConnectionLost) {
    lv_label_set_text(header_target_label, "WLED offline");
  } else if (wled::connectionStatus() == wled::ConnectionStatus::kConnecting) {
    lv_label_set_text(header_target_label, "Connecting");
    dotColor = kColorWarn;
  } else if (wled::targetingAll()) {
    lv_label_set_text(header_target_label, "Multi");
    const size_t online = wled::activeDeviceCount();
    size_t expected = 0;
    for (size_t i = 0; i < wled::deviceCount(); ++i) {
      expected++;
    }
    if (online && online == expected) dotColor = kColorOk;
    else if (online) dotColor = kColorWarn;
  } else {
    const wled::DeviceInfo device = wled::deviceInfo(wled::focusedDevice());
    if (!device.name.empty()) lv_label_set_text(header_target_label, device.name.c_str());
    else lv_label_set_text(header_target_label, "WLED");
    if (device.online) dotColor = kColorOk;
  }
  setHeaderDotColor(dotColor);
}

// One quiet top-bar spinner covers connection work, state loading, and queued
// commands. It is hidden for settled errors and after work completes.
void updateActivityIndicator() {
  if (!header_activity_spinner || !header_connection_dot) return;
  const bool wifiBusy = wifilink::busy();
  const wled::ConnectionStatus wledStatus = wled::connectionStatus();
  const bool wledBusy = wifilink::connected() &&
                        (wledStatus == wled::ConnectionStatus::kSearching ||
                         wledStatus == wled::ConnectionStatus::kNoDevicesFound ||
                         wledStatus == wled::ConnectionStatus::kConnecting ||
                         wledStatus == wled::ConnectionStatus::kReconnecting);
  const bool active = wifiBusy || wledBusy || wled::loading() || wled::commandsPending();
  setHiddenIfChanged(header_activity_spinner, !active);
  setHiddenIfChanged(header_connection_dot, active);
}

void updateWifiIndicator() {
  if (!header_wifi_bars[0] || !header_wifi_offline_mark) return;

  uint32_t color = kColorDanger;
  uint8_t bars = 0;
  bool offline = false;
  if (wifilink::connected()) {
    const int rssi = wifilink::rssi();
    if (rssi >= -55) bars = 4;
    else if (rssi >= -67) bars = 3;
    else if (rssi >= -75) bars = 2;
    else bars = 1;
    color = bars >= 3 ? kColorOk : (bars == 2 ? kColorWarn : kColorDanger);
  } else if (wifilink::busy()) {
    bars = 1;
    color = kColorWarn;
  } else {
    offline = true;
  }
  if (bars != header_wifi_bars_lit || color != header_wifi_color) {
    for (uint8_t i = 0; i < 4; ++i) {
      const bool lit = i < bars;
      const bool was_lit = i < header_wifi_bars_lit;
      // Unlit bars keep their muted style unless their state changed. Updating
      // an LVGL style with the same value still invalidates that object.
      if (header_wifi_bars_lit >= 0 && lit == was_lit && (!lit || color == header_wifi_color)) continue;
      lv_obj_set_style_bg_color(header_wifi_bars[i], lv_color_hex(lit ? color : kColorTextMuted), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(header_wifi_bars[i], lit ? LV_OPA_COVER : LV_OPA_70, LV_PART_MAIN);
    }
    header_wifi_bars_lit = bars;
    header_wifi_color = color;
  }
  if (offline != header_wifi_offline) {
    header_wifi_offline = offline;
    setHiddenIfChanged(header_wifi_offline_mark, !offline);
  }
}

// Shows the focused WLED or current multi-device target in the Settings target row.
void updateTargetLabel() {
  updateHeaderTarget();
  if (target_label) {
    if (wled::connectionStatus() != wled::ConnectionStatus::kConnected) {
      lv_label_set_text(target_label, "Scan");
    } else if (wled::targetingAll()) {
      lv_label_set_text_fmt(target_label, "All (%u)", unsigned(wled::activeDeviceCount()));
    } else {
      const wled::DeviceInfo device = wled::deviceInfo(wled::focusedDevice());
      if (!device.name.empty()) lv_label_set_text(target_label, device.name.c_str());
      else lv_label_set_text_fmt(target_label, "%02X%02X", device.mac[4], device.mac[5]);
    }
  }
}


// ── Live state reflection ─────────────────────────────────────────────────────

void uiSyncFromModel() {
  updateFirmwareUpdateUi();
  updateActivityIndicator();
  updateWifiIndicator();
  static uint32_t seen_connection = UINT32_MAX;
  const uint32_t connection_rev = wled::connectionRevision();
  if (connection_rev != seen_connection) {
    seen_connection = connection_rev;
    updateConnLabel();
    updateTargetLabel();
    refreshTargetDialog();
    updateStatusFromModel();
    updateWledControlAvailability();
  }
  
  static uint32_t seen_devices = UINT32_MAX;
  const uint32_t drev = wled::deviceRevision();
  if (drev != seen_devices) {
    seen_devices = drev;
    updateTargetLabel();
    refreshTargetDialog();
  }
  

  static uint32_t seen_catalog = UINT32_MAX;
  const uint32_t crev = wled::catalogRevision();
  if (crev != seen_catalog) {
    seen_catalog = crev;
    rebuildPresetTab();  // effect/palette catalogs are baked in; only presets arrive at runtime
    updateStatusFromModel();
  }

  static uint32_t seen_state = UINT32_MAX;
  const uint32_t rev = wled::stateRevision();
  if (rev == seen_state) return;
  seen_state = rev;

  updateConnLabel();
  updateTargetLabel();

  const wled::Model& m = wled::model();
  const uint16_t mixed = wled::mixedStateMask();
  if (mixed & wled::kMixedPower) {
    if (power_button) lv_obj_clear_state(power_button, LV_STATE_CHECKED);
    if (power_button_label) lv_label_set_text(power_button_label, LV_SYMBOL_POWER "  Mixed");
  } else if (state.power != m.power || wled::targetingAll()) {
    setPowerUi(m.power);
  }

  if (state.brightness != m.brightness &&
      !(brightness_slider && lv_obj_has_state(brightness_slider, LV_STATE_PRESSED))) {
    state.brightness = m.brightness;
    if (brightness_slider) lv_slider_set_value(brightness_slider, m.brightness, LV_ANIM_OFF);
    if (brightness_label) {
      if (mixed & wled::kMixedBrightness) lv_label_set_text(brightness_label, "Mixed");
      else lv_label_set_text_fmt(brightness_label, "%u%%", brightnessPercent(m.brightness));
    }
  } else if (brightness_label && (mixed & wled::kMixedBrightness) &&
             !(brightness_slider && lv_obj_has_state(brightness_slider, LV_STATE_PRESSED))) {
    lv_label_set_text(brightness_label, "Mixed");
  }

  updateColorControlsFromModel();

  if (m.effect >= 0 && static_cast<uint8_t>(m.effect) != selected_effect_id) {
    setSelectedEffect(static_cast<uint8_t>(m.effect));
    if (fx_tab) lv_obj_invalidate(fx_tab);
    revealSelectedEffect(true);
  }

  setSelectedPreset(m.preset > 0 ? static_cast<uint8_t>(m.preset) : 0);
  updateStatusFromModel();
}

// ── Event handlers ────────────────────────────────────────────────────────────

void onPower(lv_event_t*) {
  if (wled::connectionStatus() != wled::ConnectionStatus::kConnected) return;
  const bool turnOn = (wled::mixedStateMask() & wled::kMixedPower) || !state.power;
  setPowerUi(turnOn);
  wled::setPower(state.power);
}

void onBrightness(lv_event_t* event) {
  if (wled::connectionStatus() != wled::ConnectionStatus::kConnected) return;
  static uint32_t last_send_ms = 0;
  state.brightness = lv_slider_get_value(lv_event_get_target(event));
  if (brightness_label) {
    lv_label_set_text_fmt(brightness_label, "%u%%", brightnessPercent(state.brightness));
  }
  const lv_event_code_t code = lv_event_get_code(event);
  const uint32_t now = millis();
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || now - last_send_ms >= 150) {
    last_send_ms = now;
    wled::setBrightness(state.brightness);
  }
}

void onPreset(lv_event_t* event) {
  if (wled::connectionStatus() != wled::ConnectionStatus::kConnected) return;
  const uintptr_t preset = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  const uint8_t preset_number = static_cast<uint8_t>(preset);
  setSelectedPreset(preset_number);
  wled::applyPreset(preset_number);
}

void activateEffectId(uint8_t effect_id) {
  if (wled::connectionStatus() != wled::ConnectionStatus::kConnected) return;
  setSelectedEffect(effect_id);
  wled::setEffect(effect_id);
}

void initShutdownControl() {
#if WLED_CYD_ENABLE_SHUTDOWN
  releaseShutdownKey();
  delay(5);
  syncShutdownButtonState("init");
#if WLED_CYD_SHUTDOWN_DEBUG
  Serial.printf("Shutdown GPIO debug: gpio=%d active=LOW pullup=%u lockout=%ums double_tap=%ums hold=%ums\n",
                WLED_CYD_SHUTDOWN_GPIO,
                WLED_CYD_SHUTDOWN_PULLUP,
                WLED_CYD_SHUTDOWN_TAP_LOCKOUT_MS,
                WLED_CYD_SHUTDOWN_DOUBLE_TAP_MS,
                WLED_CYD_SHUTDOWN_HOLD_MS);
#endif
#endif
}

void pollShutdownControl() {
#if WLED_CYD_ENABLE_SHUTDOWN
  if (shutdown_overlay) {
    return;
  }

  const uint32_t now = millis();
  const bool pressed = readShutdownButtonPressed();
#if WLED_CYD_SHUTDOWN_DEBUG
  if (now - shutdown_last_debug_status_ms >= WLED_CYD_SHUTDOWN_DEBUG_STATUS_MS) {
    shutdown_last_debug_status_ms = now;
    const uint32_t tap_age = shutdown_last_tap_ms == 0 ? 0 : now - shutdown_last_tap_ms;
    Serial.printf("Shutdown GPIO status: raw=%s last_tap_age=%ums\n",
                  shutdownLevelName(pressed),
                  tap_age);
  }
#endif

  if (pressed != shutdown_button_last_read_pressed) {
    shutdown_button_last_read_pressed = pressed;
#if WLED_CYD_SHUTDOWN_DEBUG
    Serial.printf("Shutdown GPIO raw transition: %s at %ums\n",
                  shutdownLevelName(pressed),
                  now);
#endif
    if (pressed) {
      handleShutdownTap(now);
    }
    return;
  }
#endif
}

void onRestart(lv_event_t*) {
  touchActivity();
  displayRestart();
}

void onShutdown(lv_event_t*) {
  touchActivity();
  startShutdownUi();
}

void goToSettings(lv_event_t*) {
  if (main_tabs) {
    lv_tabview_set_act(main_tabs, kSettingsTabIndex, LV_ANIM_ON);
  }
}

void onFlipDisplay(lv_event_t*) {
  display_flipped = !display_flipped;
  saveSettings();
  applyDisplayRotation();
  displayClear();
  updateOrientationLabel();
  lv_obj_invalidate(lv_scr_act());
}

void onToggleIdleAction(lv_event_t*) {
  idle_mode = nextIdleMode(idle_mode);
  saveSettings();
  updateIdleLabel();
  touchActivity();
}

// ── UI entry point ────────────────────────────────────────────────────────────

void syncLivePeekSubscription() {
  wled::setLivePeek(true);
}

void updatePeekStrip() {
  if (!peek_bar) return;
  constexpr uint32_t kPeekRefreshIntervalMs = 50;  // 20 fps is smooth in a 14 px strip.
  static uint32_t seen_rev = UINT32_MAX;
  static uint32_t last_refresh_ms = 0;
  static bool seen_visible = false;
  static bool received_frame = false;

  const uint32_t now = millis();
  if (wled::liveFrameAgeMs(now) != UINT32_MAX) received_frame = true;
  if (!wled::livePeekEnabled()) received_frame = false;
  const bool visible = wled::online() && wled::livePeekEnabled() && received_frame;
  if (visible != seen_visible) {
    seen_visible = visible;
    if (visible) {
      lv_obj_clear_flag(peek_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(peek_bar, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (!visible) return;

  const uint32_t rev = wled::liveRevision();
  if (rev != seen_rev && now - last_refresh_ms >= kPeekRefreshIntervalMs) {
    seen_rev = rev;
    last_refresh_ms = now;
    lv_obj_invalidate(peek_bar);
  }
}

void createUi() {
  Serial.println("UI init: building screen");
  initStyles();

  lv_obj_t* screen = lv_scr_act();
  lv_obj_add_style(screen, &style_screen, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* root = lv_obj_create(screen);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

  lv_obj_t* topbar = lv_obj_create(root);
  lv_obj_add_style(topbar, &style_topbar, LV_PART_MAIN);
  lv_obj_set_size(topbar, LV_PCT(100), kTopBarHeight);
  lv_obj_set_flex_flow(topbar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(topbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollbar_mode(topbar, LV_SCROLLBAR_MODE_OFF);

  
  lv_obj_t* target = lv_obj_create(topbar);
  lv_obj_remove_style_all(target);
  // The P4 preview needs priority in the wide header; long controller names
  // stay readable here but truncate sooner when necessary.
  lv_obj_set_size(target, uiScaled(96, 130), uiScaled(30, 40));
  lv_obj_set_flex_flow(target, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(target, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(target, 6, LV_PART_MAIN);
  lv_obj_clear_flag(target, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  header_connection_dot = lv_obj_create(target);
  lv_obj_remove_style_all(header_connection_dot);
  lv_obj_clear_flag(header_connection_dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(header_connection_dot, uiScaled(9, 13), uiScaled(9, 13));
  lv_obj_set_style_radius(header_connection_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(header_connection_dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(header_connection_dot, lv_color_hex(kColorDanger), LV_PART_MAIN);

  header_activity_spinner = lv_spinner_create(target, 800, 70);
  lv_obj_clear_flag(header_activity_spinner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(header_activity_spinner, uiScaled(11, 16), uiScaled(11, 16));
  lv_obj_set_style_arc_width(header_activity_spinner, 2, LV_PART_MAIN);
  lv_obj_set_style_arc_width(header_activity_spinner, 2, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(header_activity_spinner, lv_color_hex(kColorBorder), LV_PART_MAIN);
  lv_obj_set_style_arc_color(header_activity_spinner, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
  lv_obj_add_flag(header_activity_spinner, LV_OBJ_FLAG_HIDDEN);

  header_target_label = lv_label_create(target);
  lv_obj_clear_flag(header_target_label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_width(header_target_label, uiScaled(78, 100));
  lv_label_set_long_mode(header_target_label, LV_LABEL_LONG_DOT);
  lv_label_set_text(header_target_label, "Searching");
  lv_obj_set_style_text_font(header_target_label, UI_FONT_BODY, LV_PART_MAIN);
  lv_obj_set_style_text_color(header_target_label, lv_color_hex(kColorText), LV_PART_MAIN);
  

  peek_bar = lv_obj_create(topbar);
  lv_obj_remove_style_all(peek_bar);
  // The compact CYD needs a capped preview width to preserve room for the
  // controller and status controls.
  lv_obj_set_size(peek_bar, uiScaled(130, 250), uiScaled(14, 22));
#if WLED_SCREEN_WIDTH >= 480
  // On the large panels, let the preview consume every pixel left between the
  // target selector and status icons instead of holding it to 250 px.
  lv_obj_set_flex_grow(peek_bar, 1);
#endif
  lv_obj_set_style_bg_color(peek_bar, lv_color_hex(kColorBg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(peek_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(peek_bar, lv_color_hex(kColorBorder), LV_PART_MAIN);
  lv_obj_set_style_border_width(peek_bar, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(peek_bar, 5, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(peek_bar, true, LV_PART_MAIN);
  lv_obj_clear_flag(peek_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(peek_bar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(peek_bar, drawPeekBar, LV_EVENT_DRAW_MAIN_END, nullptr);

  lv_obj_t* status = lv_obj_create(topbar);
  lv_obj_remove_style_all(status);
#if WLED_CYD_ENABLE_BATTERY
  lv_obj_set_size(status, uiScaled(24, 38) + (batteryAvailable() ? uiScaled(30, 46) : 0), 22);
#else
  lv_obj_set_size(status, uiScaled(24, 38), 22);
#endif
  lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(status, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(status, 4, LV_PART_MAIN);

  lv_obj_t* wifi = lv_obj_create(status);
  lv_obj_remove_style_all(wifi);
  lv_obj_set_size(wifi, uiScaled(22, 30), 22);
  lv_obj_clear_flag(wifi, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  for (uint8_t i = 0; i < 4; ++i) {
    header_wifi_bars[i] = lv_obj_create(wifi);
    lv_obj_remove_style_all(header_wifi_bars[i]);
    lv_obj_set_size(header_wifi_bars[i], 4, 5 + i * 4);
    lv_obj_set_style_radius(header_wifi_bars[i], 1, LV_PART_MAIN);
    lv_obj_align(header_wifi_bars[i], LV_ALIGN_BOTTOM_LEFT, 1 + i * 5, -2);
  }

  header_wifi_offline_mark = lv_label_create(wifi);
  lv_label_set_text(header_wifi_offline_mark, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_font(header_wifi_offline_mark, UI_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(header_wifi_offline_mark, lv_color_hex(kColorDanger), LV_PART_MAIN);
  lv_obj_align(header_wifi_offline_mark, LV_ALIGN_BOTTOM_RIGHT, 1, 2);
  lv_obj_add_flag(header_wifi_offline_mark, LV_OBJ_FLAG_HIDDEN);

#if WLED_CYD_ENABLE_BATTERY
  createBatteryIndicator(status);
#endif

  main_tabs = lv_tabview_create(root, LV_DIR_TOP, kTabButtonHeight);
  lv_obj_add_event_cb(main_tabs, onMainTabChanged, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_set_size(main_tabs, LV_PCT(100), kScreenHeight - kTopBarHeight);
  lv_obj_set_style_bg_color(main_tabs, lv_color_hex(kColorBg), LV_PART_MAIN);
  lv_obj_set_style_border_width(main_tabs, 0, LV_PART_MAIN);

  lv_obj_t* tab_btns = lv_tabview_get_tab_btns(main_tabs);
  lv_obj_set_style_bg_color(tab_btns, lv_color_hex(kColorHeaderBar), LV_PART_MAIN);
  lv_obj_set_style_border_width(tab_btns, 0, LV_PART_MAIN);
  lv_obj_set_style_text_color(tab_btns, lv_color_hex(kColorTextMuted), LV_PART_MAIN);
  lv_obj_set_style_text_color(tab_btns, lv_color_hex(kColorAccent), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(tab_btns, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(tab_btns, lv_color_hex(kColorAccent), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(tab_btns, 3, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_side(tab_btns, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(tab_btns, lv_color_hex(kColorSurfaceRaised), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(tab_btns, LV_OPA_50, LV_PART_ITEMS | LV_STATE_PRESSED);

  lv_obj_t* live = lv_tabview_add_tab(main_tabs, "Power");
  presets_tab = lv_tabview_add_tab(main_tabs, "Presets");
  fx_tab = lv_tabview_add_tab(main_tabs, "FX");
#if WLED_BOARD == WLED_BOARD_JC4880P443
  lv_obj_t* colors = lv_tabview_add_tab(main_tabs, "Colors");
#else
  lv_obj_t* colors = lv_tabview_add_tab(main_tabs, "Colors");
#endif
  lv_obj_t* settings = lv_tabview_add_tab(main_tabs, LV_SYMBOL_SETTINGS);

  createLiveTab(live);
  createPresetsTab(presets_tab);
  createFxTab(fx_tab);
  createColorsTab(colors);
  createSettingsTab(settings);

  createHeaderTabTargets(topbar, tab_btns);

  if (show_info_on_first_boot) {
    lv_tabview_set_act(main_tabs, kInfoTabIndex, LV_ANIM_OFF);
    markInfoTabSeen();
  }

  Serial.println("UI init: ready");
}
