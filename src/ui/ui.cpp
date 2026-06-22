#include "ui.h"
#include "tabs.h"
#include "../display.h"
#include "../espnow.h"
#include "../settings.h"
#include "../BatteryMonitor.h"
#include <Arduino.h>
#include "generated/wled_logo_png.h"

namespace {

const lv_img_dsc_t kHeaderLogoImage = {
    {LV_IMG_CF_TRUE_COLOR, 0, 0, kWledLogoHeaderWidth, kWledLogoHeaderHeight},
    kWledLogoHeaderPixelCount * sizeof(kWledLogoHeaderPixels[0]),
    reinterpret_cast<const uint8_t*>(kWledLogoHeaderPixels),
};

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

    if (i + 1 == selected_preset) {
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

// ── Event handlers ────────────────────────────────────────────────────────────

void onPower(lv_event_t*) {
  setPowerUi(!state.power);
  sendPower(state.power);
}

void onBrightness(lv_event_t* event) {
  const uint8_t previous = state.brightness;
  state.brightness = lv_slider_get_value(lv_event_get_target(event));
  if (brightness_label) {
    lv_label_set_text_fmt(brightness_label, "%u", state.brightness);
  }
  sendBrightnessDelta(static_cast<int>(state.brightness) - previous);
}

void onPreset(lv_event_t* event) {
  const uintptr_t preset = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  const uint8_t preset_number = static_cast<uint8_t>(preset);
  setSelectedPreset(preset_number);
  sendPreset(preset_number);
  setPowerUi(true);
}

void activateEffect(const WledEffectInfo* effect) {
  if (!effect) {
    return;
  }

  setSelectedEffect(effect->id);
  sendWledTouchButton(effect->button);
  setPowerUi(true);
}

void onPing(lv_event_t*) {
  sendWledTouchButton(kWledTouchButtonOn);
}

void onRestart(lv_event_t*) {
  touchActivity();
  ESP.restart();
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
  lv_obj_t* info = lv_tabview_add_tab(main_tabs, "Info");
  lv_obj_t* settings = lv_tabview_add_tab(main_tabs, "Settings");

  createLiveTab(live);
  createLooksTab(presets_tab);
  createFxTab(fx_tab);
  createInfoTab(info);
  createSettingsTab(settings);

  if (show_info_on_first_boot) {
    lv_tabview_set_act(main_tabs, kInfoTabIndex, LV_ANIM_OFF);
    markInfoTabSeen();
  }

  Serial.println("UI init: ready");
}
