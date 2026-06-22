#include "app_state.h"

RemoteState state;
uint8_t selected_preset = 0;
bool display_flipped = false;
IdleMode idle_mode = IdleMode::kDim;
bool extended_mode = false;
bool show_info_on_first_boot = false;

lv_obj_t* main_tabs = nullptr;
lv_obj_t* presets_tab = nullptr;
lv_obj_t* fx_tab = nullptr;
lv_obj_t* preset_buttons[kExtendedPresetCount] = {};
lv_obj_t* power_button = nullptr;
lv_obj_t* power_button_label = nullptr;
lv_obj_t* brightness_label = nullptr;
lv_obj_t* mac_label = nullptr;
lv_obj_t* orientation_label = nullptr;
lv_obj_t* idle_label = nullptr;
lv_obj_t* mode_label = nullptr;
lv_obj_t* help_dialog = nullptr;

#if WLED_CYD_ENABLE_BATTERY
lv_obj_t* battery_indicator = nullptr;
lv_obj_t* battery_fill = nullptr;
lv_obj_t* battery_charge = nullptr;
#endif

lv_style_t style_screen;
lv_style_t style_topbar;
lv_style_t style_panel;
lv_style_t style_section_header;
lv_style_t style_label_muted;
lv_style_t style_button;
lv_style_t style_button_pressed;
lv_style_t style_button_checked;
lv_style_t style_slider;
lv_style_t style_slider_indicator;
lv_style_t style_knob;
