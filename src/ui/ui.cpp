#include "ui.h"
#include "tabs.h"
#include "../display.h"
#include "../espnow.h"
#include "../settings.h"
#include "../BatteryMonitor.h"
#include "../wled_api.h"
#include <Arduino.h>
#include <algorithm>
#include "generated/wled_logo_png.h"

namespace {

const lv_img_dsc_t kHeaderLogoImage = {
    {LV_IMG_CF_TRUE_COLOR, 0, 0, kWledLogoHeaderWidth, kWledLogoHeaderHeight},
    kWledLogoHeaderPixelCount * sizeof(kWledLogoHeaderPixels[0]),
    reinterpret_cast<const uint8_t*>(kWledLogoHeaderPixels),
};

constexpr uint32_t kPeekFreshMs = 1200;
#if WLED_CYD_ENABLE_SHUTDOWN
constexpr uint8_t kShutdownCountdownSteps = 10;
#endif

lv_obj_t* peek_bar = nullptr;
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
  lv_obj_set_style_text_font(shutdown_countdown_label, &lv_font_montserrat_48, LV_PART_MAIN);
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

  for (uint8_t i = 0; i < kExtendedPresetCount; ++i) {
    if (!preset_buttons[i]) {
      continue;
    }

    const uint8_t id = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(preset_buttons[i])));
    if (id == selected_preset) {
      lv_obj_add_state(preset_buttons[i], LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(preset_buttons[i], LV_STATE_CHECKED);
    }
  }
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
  lv_style_set_text_font(&style_section_header, &lv_font_montserrat_16);
  lv_style_set_text_letter_space(&style_section_header, 1);

  lv_style_init(&style_label_muted);
  lv_style_set_text_color(&style_label_muted, lv_color_hex(kColorText));
  lv_style_set_text_font(&style_label_muted, &lv_font_montserrat_14);

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
  lv_style_set_pad_all(&style_knob, 5);
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

void updateModeLabel() {
  if (mode_label) {
    lv_label_set_text(mode_label, extended_mode ? "Extended" : "Basic");
  }
}

void updateConnLabel() {
  if (!conn_label) return;
  const wled::Model& m = wled::model();
  if (m.online) {
    lv_label_set_text_fmt(conn_label, LV_SYMBOL_OK " %s", m.name.empty() ? "Connected" : m.name.c_str());
    lv_obj_set_style_text_color(conn_label, lv_color_hex(kColorOk), LV_PART_MAIN);
  } else {
    lv_label_set_text(conn_label, "Searching for WLED...");
    lv_obj_set_style_text_color(conn_label, lv_color_hex(kColorWarn), LV_PART_MAIN);
  }
}

// ── Live state reflection ─────────────────────────────────────────────────────

void uiSyncFromModel() {
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

  const wled::Model& m = wled::model();
  if (state.power != m.power) setPowerUi(m.power);

  if (state.brightness != m.brightness &&
      !(brightness_slider && lv_obj_has_state(brightness_slider, LV_STATE_PRESSED))) {
    state.brightness = m.brightness;
    if (brightness_slider) lv_slider_set_value(brightness_slider, m.brightness, LV_ANIM_OFF);
    if (brightness_label) lv_label_set_text_fmt(brightness_label, "%u", m.brightness);
  }

  updateColorControlsFromModel();

  if (m.effect >= 0 && static_cast<uint8_t>(m.effect) != selected_effect_id) {
    setSelectedEffect(static_cast<uint8_t>(m.effect));
    if (fx_tab) lv_obj_invalidate(fx_tab);
  }

  setSelectedPreset(m.preset > 0 ? static_cast<uint8_t>(m.preset) : 0);
  updateStatusFromModel();
}

// ── Event handlers ────────────────────────────────────────────────────────────

void onPower(lv_event_t*) {
  setPowerUi(!state.power);
  wled::setPower(state.power);
}

void onBrightness(lv_event_t* event) {
  static uint32_t last_send_ms = 0;
  state.brightness = lv_slider_get_value(lv_event_get_target(event));
  if (brightness_label) {
    lv_label_set_text_fmt(brightness_label, "%u", state.brightness);
  }
  const lv_event_code_t code = lv_event_get_code(event);
  const uint32_t now = millis();
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || now - last_send_ms >= 150) {
    last_send_ms = now;
    wled::setBrightness(state.brightness);
  }
}

void onPreset(lv_event_t* event) {
  const uintptr_t preset = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  const uint8_t preset_number = static_cast<uint8_t>(preset);
  setSelectedPreset(preset_number);
  wled::applyPreset(preset_number);
}

void activateEffectId(uint8_t effect_id) {
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

void onPing(lv_event_t*) {
  wled::poll();
  wled::requestCatalogs();
}

void onRestart(lv_event_t*) {
  touchActivity();
  ESP.restart();
}

void onShutdown(lv_event_t*) {
  touchActivity();
  startShutdownUi();
}

void onRemoteAction(lv_event_t* event) {
  const uintptr_t button = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  sendWledTouchButton(static_cast<uint8_t>(button));
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
  gfx.fillScreen(TFT_BLACK);
  updateOrientationLabel();
  lv_obj_invalidate(lv_scr_act());
}

void onToggleIdleAction(lv_event_t*) {
  idle_mode = nextIdleMode(idle_mode);
  saveSettings();
  updateIdleLabel();
  touchActivity();
}

void onToggleControlMode(lv_event_t* event) {
  extended_mode = !extended_mode;
  saveSettings();
  updateModeLabel();
  lv_obj_t* target = lv_event_get_target(event);
  if (target) {
    if (extended_mode) {
      lv_obj_add_state(target, LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(target, LV_STATE_CHECKED);
    }
  }
  rebuildPresetTab();
  rebuildFxTab();
  touchActivity();
}

// ── UI entry point ────────────────────────────────────────────────────────────

void syncLivePeekSubscription() {
  wled::setLivePeek(true);
}

void updatePeekStrip() {
  if (!peek_bar) return;
  static uint32_t seen_rev = UINT32_MAX;
  static bool seen_visible = false;

  const uint32_t age = wled::liveFrameAgeMs(millis());
  const bool visible = wled::online() && age != UINT32_MAX && age <= kPeekFreshMs;
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
  if (rev != seen_rev) {
    seen_rev = rev;
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

  lv_obj_t* title = lv_img_create(topbar);
  lv_img_set_src(title, &kHeaderLogoImage);
  lv_obj_set_size(title, kWledLogoHeaderWidth, kWledLogoHeaderHeight);

  peek_bar = lv_obj_create(topbar);
  lv_obj_remove_style_all(peek_bar);
  lv_obj_set_size(peek_bar, 170, 14);
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
  lv_obj_set_size(status, batteryAvailable() ? 30 : 0, 22);
#else
  lv_obj_set_size(status, 0, 22);
#endif
  lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(status, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

#if WLED_CYD_ENABLE_BATTERY
  createBatteryIndicator(status);
#endif

  main_tabs = lv_tabview_create(root, LV_DIR_TOP, kTabButtonHeight);
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
  lv_obj_t* colors = lv_tabview_add_tab(main_tabs, "Colors");
  lv_obj_t* settings = lv_tabview_add_tab(main_tabs, LV_SYMBOL_SETTINGS);

  createLiveTab(live);
  createPresetsTab(presets_tab);
  createFxTab(fx_tab);
  createColorsTab(colors);
  createSettingsTab(settings);

  if (show_info_on_first_boot) {
    lv_tabview_set_act(main_tabs, kInfoTabIndex, LV_ANIM_OFF);
    markInfoTabSeen();
  }

  Serial.println("UI init: ready");
}
