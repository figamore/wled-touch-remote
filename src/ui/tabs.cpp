#include "tabs.h"
#include "ui.h"
#include "../BatteryMonitor.h"
#include <WiFi.h>
#include <cstring>
#include "generated/version.h"
#include "generated/wled_logo_png.h"

namespace {

// ── Image descriptors for modal dialogs ──────────────────────────────────────

const lv_img_dsc_t kHelpQrImage = {
    {LV_IMG_CF_TRUE_COLOR, 0, 0, kHelpQrWidth, kHelpQrHeight},
    kHelpQrPixelCount * sizeof(kHelpQrPixels[0]),
    reinterpret_cast<const uint8_t*>(kHelpQrPixels),
};

const lv_img_dsc_t kRemoteJsonQrImage = {
    {LV_IMG_CF_TRUE_COLOR, 0, 0, kRemoteJsonQrWidth, kRemoteJsonQrHeight},
    kRemoteJsonQrPixelCount * sizeof(kRemoteJsonQrPixels[0]),
    reinterpret_cast<const uint8_t*>(kRemoteJsonQrPixels),
};

// ── Widget helpers ────────────────────────────────────────────────────────────

void addLabel(lv_obj_t* parent, const char* text, lv_coord_t width = LV_SIZE_CONTENT) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_add_style(label, &style_section_header, LV_PART_MAIN);
  lv_obj_set_width(label, width);
}

void styleButton(lv_obj_t* btn, bool checkable = false) {
  lv_obj_add_style(btn, &style_button, LV_PART_MAIN);
  lv_obj_add_style(btn, &style_button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
  if (checkable) {
    lv_obj_add_style(btn, &style_button_checked, LV_PART_MAIN | LV_STATE_CHECKED);
  }
}

lv_obj_t* createPanel(lv_obj_t* parent) {
  lv_obj_t* panel = lv_obj_create(parent);
  lv_obj_add_style(panel, &style_panel, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  return panel;
}

void configurePageScroll(lv_obj_t* page, bool enabled) {
  lv_obj_set_scroll_dir(page, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(page, enabled ? LV_SCROLLBAR_MODE_AUTO : LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);

  lv_obj_set_style_bg_color(page, lv_color_hex(kColorAccent), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(page, LV_OPA_50, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(page, 4, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_right(page, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(page, 2, LV_PART_SCROLLBAR);

  if (enabled) {
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
  } else {
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
  }
}

lv_obj_t* createSlider(lv_obj_t* parent,
                       int min,
                       int max,
                       int value,
                       lv_event_cb_t cb,
                       lv_obj_t** value_label) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, 38);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(row, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);

  lv_obj_t* slider = lv_slider_create(row);
  lv_slider_set_range(slider, min, max);
  lv_slider_set_value(slider, value, LV_ANIM_OFF);
  lv_obj_set_size(slider, 190, 12);
  lv_obj_add_flag(slider, LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_clear_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_style(slider, &style_slider, LV_PART_MAIN);
  lv_obj_add_style(slider, &style_slider_indicator, LV_PART_INDICATOR);
  lv_obj_add_style(slider, &style_knob, LV_PART_KNOB);
  lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, nullptr);

  *value_label = lv_label_create(row);
  lv_obj_set_width(*value_label, 34);
  lv_obj_clear_flag(*value_label, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_label_set_text_fmt(*value_label, "%d", value);
  lv_obj_set_style_text_align(*value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

  return slider;
}

lv_obj_t* createRemoteButton(lv_obj_t* parent,
                             const char* text,
                             lv_coord_t width,
                             lv_coord_t height,
                             uint8_t button) {
  lv_obj_t* btn = lv_btn_create(parent);
  styleButton(btn);
  lv_obj_set_size(btn, width, height);
  lv_obj_add_event_cb(btn,
                      onRemoteAction,
                      LV_EVENT_CLICKED,
                      reinterpret_cast<void*>(static_cast<uintptr_t>(button)));

  lv_obj_t* label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return btn;
}

void createControlPairRow(lv_obj_t* parent,
                          const char* label_text,
                          uint8_t down_button,
                          uint8_t up_button,
                          const char* down_text,
                          const char* up_text) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 48);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* label = lv_label_create(row);
  lv_obj_set_width(label, 118);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_label_set_text(label, label_text);
  lv_obj_add_style(label, &style_label_muted, LV_PART_MAIN);

  createRemoteButton(row, down_text, 78, 44, down_button);
  createRemoteButton(row, up_text, 78, 44, up_button);
}

void createToggleRow(lv_obj_t* parent,
                     const char* label_text,
                     uint8_t button) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 42);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* label = lv_label_create(row);
  lv_obj_set_width(label, 176);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_label_set_text(label, label_text);
  lv_obj_add_style(label, &style_label_muted, LV_PART_MAIN);

  createRemoteButton(row, "Toggle", 82, 38, button);
}

void createColorSwatches(lv_obj_t* parent) {
  lv_obj_t* swatches = lv_obj_create(parent);
  lv_obj_remove_style_all(swatches);
  lv_obj_set_size(swatches, LV_PCT(100), 272);
  lv_obj_set_flex_flow(swatches, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(swatches, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(swatches, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_column(swatches, 8, LV_PART_MAIN);

  for (const ColorSwatch& swatch : kColorSwatches) {
    lv_obj_t* btn = createRemoteButton(swatches, swatch.label, 132, 48, swatch.button);
    const lv_color_t color = lv_color_hex(swatch.color);
    lv_obj_set_style_bg_color(btn, color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_darken(color, LV_OPA_30),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, lv_color_hex(swatch.dark_text ? kColorBg : 0xFFFFFF), LV_PART_MAIN);
  }
}

// ── Dialog helpers ────────────────────────────────────────────────────────────

void closeHelpDialog(lv_event_t*) {
  if (help_dialog) {
    lv_obj_del(help_dialog);
    help_dialog = nullptr;
  }
}

lv_obj_t* beginInfoModal(const char* title_text) {
  help_dialog = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(help_dialog);
  lv_obj_set_size(help_dialog, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(help_dialog, lv_color_hex(kColorBg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(help_dialog, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(help_dialog, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(help_dialog, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* header = lv_obj_create(help_dialog);
  lv_obj_remove_style_all(header);
  lv_obj_set_size(header, LV_PCT(100), 40);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_left(header, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_right(header, 8, LV_PART_MAIN);
  lv_obj_set_style_border_color(header, lv_color_hex(kColorBorder), LV_PART_MAIN);
  lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
  lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(header);
  lv_label_set_text(title, title_text);
  lv_obj_set_width(title, 224);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(kColorAccent), LV_PART_MAIN);

  lv_obj_t* close = lv_btn_create(header);
  styleButton(close);
  lv_obj_set_size(close, 40, 30);
  lv_obj_add_event_cb(close, closeHelpDialog, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* close_label = lv_label_create(close);
  lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
  lv_obj_center(close_label);

  lv_obj_t* content = lv_obj_create(help_dialog);
  lv_obj_remove_style_all(content);
  lv_obj_set_size(content, 296, 184);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(content, 8, LV_PART_MAIN);
  lv_obj_add_flag(content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
  return content;
}

void addQrCode(lv_obj_t* parent, const lv_img_dsc_t* src, lv_coord_t w, lv_coord_t h) {
  lv_obj_t* qr = lv_img_create(parent);
  lv_img_set_src(qr, src);
  lv_obj_set_size(qr, w, h);
  lv_obj_set_style_outline_color(qr, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_outline_width(qr, 3, LV_PART_MAIN);
  lv_obj_set_style_outline_opa(qr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_outline_pad(qr, 0, LV_PART_MAIN);
}

void openHelpDialog(lv_event_t*) {
  if (help_dialog) {
    return;
  }

  lv_obj_t* content = beginInfoModal("Help");

  lv_obj_t* text_col = lv_obj_create(content);
  lv_obj_remove_style_all(text_col);
  lv_obj_set_size(text_col, 100, LV_PCT(100));
  lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(text_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(text_col, 10, LV_PART_MAIN);
  lv_obj_clear_flag(text_col, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* instructions = lv_label_create(text_col);
  lv_obj_set_width(instructions, 100);
  lv_label_set_long_mode(instructions, LV_LABEL_LONG_WRAP);
  lv_label_set_text(instructions,
                    "Scan for\n"
                    "setup help,\n"
                    "usage tips,\n"
                    "and project\n"
                    "instructions.");
  lv_obj_add_style(instructions, &style_label_muted, LV_PART_MAIN);

  lv_obj_t* version = lv_label_create(text_col);
  lv_obj_set_width(version, 100);
  lv_label_set_long_mode(version, LV_LABEL_LONG_WRAP);
  lv_label_set_text_fmt(version,
                        "Firmware v%s\n"
                        "Copyright Figamore 2026",
                        kAppVersion);
  lv_obj_add_style(version, &style_label_muted, LV_PART_MAIN);
  lv_obj_set_style_text_font(version, &lv_font_montserrat_12, LV_PART_MAIN);

  addQrCode(content, &kHelpQrImage, kHelpQrWidth, kHelpQrHeight);
}

void openRemoteJsonHelpDialog(lv_event_t*) {
  if (help_dialog) {
    return;
  }

  lv_obj_t* content = beginInfoModal("Extended FX");

  lv_obj_t* instructions = lv_label_create(content);
  lv_obj_set_width(instructions, 100);
  lv_label_set_long_mode(instructions, LV_LABEL_LONG_WRAP);
  lv_label_set_text(instructions,
                    "Upload\n"
                    "remote.json\n"
                    "to WLED:\n\n"
                    "1. Open\n"
                    "wled.local/edit\n"
                    "2. Upload file\n"
                    "3. Enable\n"
                    "Extended");
  lv_obj_add_style(instructions, &style_label_muted, LV_PART_MAIN);

  addQrCode(content, &kRemoteJsonQrImage, kRemoteJsonQrWidth, kRemoteJsonQrHeight);
}

const char* nextEffectControlLabel(const char*& labels, char* buffer, size_t buffer_size) {
  if (!labels || !*labels || buffer_size == 0) {
    return "";
  }

  size_t len = 0;
  while (labels[len] && labels[len] != '|') {
    len++;
  }

  const size_t copy_len = len < buffer_size - 1 ? len : buffer_size - 1;
  memcpy(buffer, labels, copy_len);
  buffer[copy_len] = '\0';
  labels += labels[len] == '|' ? len + 1 : len;
  return buffer;
}

void addEffectPairIfEnabled(lv_obj_t* parent,
                            const WledEffectInfo& effect,
                            uint16_t mask,
                            const char*& labels,
                            const char* fallback,
                            uint8_t down_button,
                            uint8_t up_button) {
  if (!(effect.controls & mask)) {
    return;
  }

  char label[32];
  const char* text = nextEffectControlLabel(labels, label, sizeof(label));
  createControlPairRow(parent,
                       *text ? text : fallback,
                       down_button,
                       up_button,
                       LV_SYMBOL_MINUS,
                       LV_SYMBOL_PLUS);
}

void addEffectToggleIfEnabled(lv_obj_t* parent,
                              const WledEffectInfo& effect,
                              uint16_t mask,
                              const char*& labels,
                              const char* fallback,
                              uint8_t button) {
  if (!(effect.controls & mask)) {
    return;
  }

  char label[32];
  const char* text = nextEffectControlLabel(labels, label, sizeof(label));
  createToggleRow(parent, *text ? text : fallback, button);
}

void openEffectSettings(const WledEffectInfo* effect) {
  if (help_dialog) {
    return;
  }

  if (!effect) {
    return;
  }

  lv_obj_t* content = beginInfoModal(effect->name);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(content, 6, LV_PART_MAIN);
  lv_obj_clear_flag(content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  configurePageScroll(content, true);

  if (!effect->controls) {
    lv_obj_t* none = lv_label_create(content);
    lv_obj_set_width(none, LV_PCT(100));
    lv_label_set_long_mode(none, LV_LABEL_LONG_WRAP);
    lv_label_set_text(none, "This WLED effect does not expose extra controls.");
    lv_obj_set_style_text_align(none, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_style(none, &style_label_muted, LV_PART_MAIN);
    return;
  }

  const char* labels = effect->labels;
  addEffectPairIfEnabled(content, *effect, kFxControlSpeed, labels,
                         "Effect speed", kRemoteSpeedDown, kRemoteSpeedUp);
  addEffectPairIfEnabled(content, *effect, kFxControlIntensity, labels,
                         "Effect intensity", kRemoteIntensityDown, kRemoteIntensityUp);
  addEffectPairIfEnabled(content, *effect, kFxControlCustom1, labels,
                         "Custom 1", kRemoteCustom1Down, kRemoteCustom1Up);
  addEffectPairIfEnabled(content, *effect, kFxControlCustom2, labels,
                         "Custom 2", kRemoteCustom2Down, kRemoteCustom2Up);
  addEffectPairIfEnabled(content, *effect, kFxControlCustom3, labels,
                         "Custom 3", kRemoteCustom3Down, kRemoteCustom3Up);
  addEffectToggleIfEnabled(content, *effect, kFxControlOption1, labels,
                           "Option 1", kRemoteOption1Toggle);
  addEffectToggleIfEnabled(content, *effect, kFxControlOption2, labels,
                           "Option 2", kRemoteOption2Toggle);
  addEffectToggleIfEnabled(content, *effect, kFxControlOption3, labels,
                           "Option 3", kRemoteOption3Toggle);
  addEffectPairIfEnabled(content, *effect, kFxControlPalette, labels,
                         "Palette", kRemotePaletteDown, kRemotePaletteUp);
}

void onEffectTableClicked(lv_event_t* event) {
  lv_obj_t* table = lv_event_get_target(event);
  uint16_t row = LV_TABLE_CELL_NONE;
  uint16_t col = LV_TABLE_CELL_NONE;
  lv_table_get_selected_cell(table, &row, &col);
  if (row == LV_TABLE_CELL_NONE || col == LV_TABLE_CELL_NONE || row >= kWledEffectCount) {
    return;
  }

  const WledEffectInfo& effect = kWledEffects[row];
  if (col == 1 && effect.id == selected_effect_id) {
    openEffectSettings(&effect);
  } else {
    activateEffect(&effect);
    lv_obj_invalidate(table);
  }
}

void onEffectTableDrawPart(lv_event_t* event) {
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(event);
  if (!dsc || !lv_obj_draw_part_check_type(dsc, &lv_table_class, LV_TABLE_DRAW_PART_CELL)) {
    return;
  }

  const uint16_t row = dsc->id / 2;
  const uint16_t col = dsc->id % 2;
  if (row >= kWledEffectCount) {
    return;
  }

  const bool selected = kWledEffects[row].id == selected_effect_id;

  if (dsc->rect_dsc) {
    dsc->rect_dsc->radius = 6;
    dsc->rect_dsc->border_width = 0;
    dsc->rect_dsc->border_color = lv_color_hex(kColorBorder);
    if (selected) {
      dsc->rect_dsc->bg_color = lv_color_hex(kColorSelected);
      dsc->rect_dsc->border_color = lv_color_hex(kColorSelectedBorder);
    } else if (row % 2) {
      dsc->rect_dsc->bg_color = lv_color_hex(kColorSurfaceRaised);
    } else {
      dsc->rect_dsc->bg_color = lv_color_hex(kColorSurface);
    }
  }

  if (dsc->label_dsc) {
    dsc->label_dsc->color = col == 1 ? lv_color_hex(kColorAccent) : lv_color_hex(kColorText);
    dsc->label_dsc->align = col == 1 ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT;
    if (col == 1 && !selected) {
      dsc->label_dsc->opa = LV_OPA_TRANSP;
    }
  }
}

// ── Settings row helper ───────────────────────────────────────────────────────

lv_obj_t* createSettingsRow(lv_obj_t* parent,
                            const char* name,
                            lv_event_cb_t cb,
                            bool checkable,
                            lv_obj_t** value_out) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 38);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* name_label = lv_label_create(row);
  lv_obj_set_width(name_label, 148);
  lv_obj_add_style(name_label, &style_label_muted, LV_PART_MAIN);
  lv_label_set_text(name_label, name);

  lv_obj_t* pill = lv_btn_create(row);
  styleButton(pill, checkable);
  lv_obj_set_size(pill, 118, 34);
  lv_obj_add_event_cb(pill, cb, LV_EVENT_CLICKED, nullptr);

  *value_out = lv_label_create(pill);
  lv_obj_center(*value_out);
  return pill;
}

#if WLED_CYD_ENABLE_BATTERY
// ── Battery widget ────────────────────────────────────────────────────────────

lv_color_t batteryColor(int level) {
  if (level >= 50) {
    return lv_color_hex(kColorBatteryOk);
  }
  if (level >= 25) {
    return lv_color_hex(kColorWarn);
  }
  return lv_color_hex(kColorDanger);
}

void updateBatteryIndicator() {
  if (!battery_indicator || !battery_fill || !battery_charge) {
    return;
  }

  int level = batteryLevel();
  if (level < 0) {
    lv_obj_add_flag(battery_indicator, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  lv_obj_clear_flag(battery_indicator, LV_OBJ_FLAG_HIDDEN);

  int fill_w = 3;
  if (level >= 75) {
    fill_w = 18;
  } else if (level >= 50) {
    fill_w = 11;
  } else if (level >= 25) {
    fill_w = 7;
  }

  bool charging = batteryCharging();
  lv_obj_set_width(battery_fill, fill_w);
  lv_obj_set_style_bg_color(battery_fill, charging ? lv_color_hex(kColorBg) : batteryColor(level), LV_PART_MAIN);

  if (charging) {
    lv_obj_clear_flag(battery_charge, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(battery_charge, LV_OBJ_FLAG_HIDDEN);
  }
}

void updateBatteryTimer(lv_timer_t*) {
  updateBatteryIndicator();
}
#endif

}  // namespace

// ── Public tab functions ──────────────────────────────────────────────────────

void createLiveTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, false);

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), 158);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);

  power_button = lv_btn_create(panel);
  styleButton(power_button, true);
  lv_obj_set_size(power_button, LV_PCT(100), 54);
  lv_obj_add_flag(power_button, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_add_event_cb(power_button, onPower, LV_EVENT_CLICKED, nullptr);

  lv_obj_set_style_bg_grad_color(power_button, lv_color_hex(kColorAccentBright),
                                 LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_style_bg_grad_dir(power_button, LV_GRAD_DIR_VER,
                               LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(power_button, lv_color_hex(0x062029),
                              LV_PART_MAIN | LV_STATE_CHECKED);

  power_button_label = lv_label_create(power_button);
  lv_label_set_text(power_button_label, LV_SYMBOL_POWER "  Power Off");
  lv_obj_set_style_text_font(power_button_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_center(power_button_label);

  addLabel(panel, "Brightness");
  createSlider(panel, 1, 255, state.brightness, onBrightness, &brightness_label);
}

void createLooksTab(lv_obj_t* tab) {
  for (lv_obj_t*& preset_button : preset_buttons) {
    preset_button = nullptr;
  }

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, extended_mode);

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), extended_mode ? 540 : kTabCardHeight);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel,
                        extended_mode ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);

  if (extended_mode) {
    lv_obj_t* preset_list = lv_obj_create(panel);
    lv_obj_remove_style_all(preset_list);
    lv_obj_set_size(preset_list, LV_PCT(100), 214);
    lv_obj_set_flex_flow(preset_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(preset_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(preset_list, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(preset_list, 6, LV_PART_MAIN);
    lv_obj_clear_flag(preset_list, LV_OBJ_FLAG_SCROLLABLE);

    for (uintptr_t i = 1; i <= kExtendedPresetCount; ++i) {
      lv_obj_t* btn = lv_btn_create(preset_list);
      styleButton(btn, true);
      lv_obj_set_size(btn, 62, 38);
      lv_obj_add_event_cb(btn, onPreset, LV_EVENT_CLICKED, reinterpret_cast<void*>(i));
      preset_buttons[i - 1] = btn;
      if (i == selected_preset) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
      }

      lv_obj_t* label = lv_label_create(btn);
      lv_label_set_text_fmt(label, "%u", static_cast<unsigned>(i));
      lv_obj_center(label);
    }

    addLabel(panel, "Colors");
    createColorSwatches(panel);
    return;
  }

  lv_obj_t* rows = lv_obj_create(panel);
  lv_obj_remove_style_all(rows);
  lv_obj_set_size(rows, LV_PCT(100), 88);
  lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(rows, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(rows, 10, LV_PART_MAIN);

  lv_obj_t* row_one = lv_obj_create(rows);
  lv_obj_remove_style_all(row_one);
  lv_obj_set_size(row_one, LV_PCT(100), 38);
  lv_obj_set_flex_flow(row_one, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row_one, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row_one, 8, LV_PART_MAIN);

  lv_obj_t* row_two = lv_obj_create(rows);
  lv_obj_remove_style_all(row_two);
  lv_obj_set_size(row_two, LV_PCT(100), 38);
  lv_obj_set_flex_flow(row_two, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row_two, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row_two, 8, LV_PART_MAIN);

  for (uintptr_t i = 1; i <= kBasicPresetCount; ++i) {
    lv_obj_t* parent = i <= 4 ? row_one : row_two;
    lv_obj_t* btn = lv_btn_create(parent);
    styleButton(btn, true);
    lv_obj_set_size(btn, 64, 38);
    lv_obj_add_event_cb(btn, onPreset, LV_EVENT_CLICKED, reinterpret_cast<void*>(i));
    preset_buttons[i - 1] = btn;
    if (i == selected_preset) {
      lv_obj_add_state(btn, LV_STATE_CHECKED);
    }

    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text_fmt(label, "%u", static_cast<unsigned>(i));
    lv_obj_center(label);
  }
}

void rebuildPresetTab() {
  if (!presets_tab) {
    return;
  }
  lv_obj_clean(presets_tab);
  createLooksTab(presets_tab);
}

void createFxTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, false);

  if (!extended_mode) {
    lv_obj_t* panel = createPanel(tab);
    lv_obj_set_size(panel, LV_PCT(100), kTabCardHeight);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, 6, LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Extended Mode Off");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);

    lv_obj_t* hint = lv_label_create(panel);
    lv_label_set_text(hint, "Enable it in Settings for FX controls");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_style(hint, &style_label_muted, LV_PART_MAIN);

    lv_obj_t* settings = lv_btn_create(panel);
    styleButton(settings);
    lv_obj_set_size(settings, LV_PCT(100), 32);
    lv_obj_add_event_cb(settings, goToSettings, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* settings_label = lv_label_create(settings);
    lv_label_set_text(settings_label, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_center(settings_label);

    lv_obj_t* learn_more = lv_btn_create(panel);
    styleButton(learn_more);
    lv_obj_set_size(learn_more, LV_PCT(100), 32);
    lv_obj_add_event_cb(learn_more, openRemoteJsonHelpDialog, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* learn_more_label = lv_label_create(learn_more);
    lv_label_set_text(learn_more_label, LV_SYMBOL_LIST "  Learn More");
    lv_obj_center(learn_more_label);
    return;
  }

  lv_obj_t* fx_panel = createPanel(tab);
  lv_obj_set_size(fx_panel, LV_PCT(100), kTabCardHeight);
  lv_obj_set_flex_flow(fx_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(fx_panel, 8, LV_PART_MAIN);

  lv_obj_t* table = lv_table_create(fx_panel);
  lv_obj_set_width(table, LV_PCT(100));
  lv_obj_set_flex_grow(table, 1);
  lv_table_set_col_cnt(table, 2);
  lv_table_set_row_cnt(table, kWledEffectCount);
  lv_table_set_col_width(table, 0, 232);
  lv_table_set_col_width(table, 1, 38);
  lv_obj_set_scroll_dir(table, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(table, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_add_flag(table, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                         LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                         LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(table, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(table, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(table, lv_color_hex(kColorAccent), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(table, LV_OPA_50, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(table, 4, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(table, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_left(table, 10, LV_PART_ITEMS);
  lv_obj_set_style_pad_right(table, 8, LV_PART_ITEMS);
  lv_obj_set_style_pad_top(table, 8, LV_PART_ITEMS);
  lv_obj_set_style_pad_bottom(table, 8, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(table, lv_color_hex(kColorText), LV_PART_ITEMS);
  lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
  lv_obj_set_style_border_color(table, lv_color_hex(kColorBorder), LV_PART_ITEMS);

  for (size_t i = 0; i < kWledEffectCount; ++i) {
    lv_table_set_cell_value(table, i, 0, kWledEffects[i].name);
    lv_table_add_cell_ctrl(table, i, 0, LV_TABLE_CELL_CTRL_TEXT_CROP);
    lv_table_set_cell_value(table, i, 1, LV_SYMBOL_SETTINGS);
  }
  lv_obj_add_event_cb(table, onEffectTableClicked, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(table, onEffectTableDrawPart, LV_EVENT_DRAW_PART_BEGIN, nullptr);
}

void rebuildFxTab() {
  if (!fx_tab) {
    return;
  }
  lv_obj_clean(fx_tab);
  createFxTab(fx_tab);
}

void createInfoTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, false);

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), 156);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);

  addLabel(panel, "WLED ESP-NOW MAC");

  mac_label = lv_label_create(panel);
  lv_label_set_text(mac_label, WiFi.macAddress().c_str());
  lv_obj_set_width(mac_label, LV_PCT(100));
  lv_obj_set_style_text_align(mac_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_font(mac_label, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(mac_label, lv_color_hex(kColorAccent), LV_PART_MAIN);

  lv_obj_t* hint = lv_label_create(panel);
  lv_label_set_text(hint, "Enter this in WLED Linked MACs");
  lv_obj_set_width(hint, LV_PCT(100));
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_add_style(hint, &style_label_muted, LV_PART_MAIN);

  lv_obj_t* actions = lv_obj_create(panel);
  lv_obj_remove_style_all(actions);
  lv_obj_set_size(actions, LV_PCT(100), 38);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* ping = lv_btn_create(actions);
  styleButton(ping);
  lv_obj_set_size(ping, 84, 36);
  lv_obj_add_event_cb(ping, onPing, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* label = lv_label_create(ping);
  lv_label_set_text(label, LV_SYMBOL_WIFI "  Ping");
  lv_obj_center(label);

  lv_obj_t* help = lv_btn_create(actions);
  styleButton(help);
  lv_obj_set_size(help, 84, 36);
  lv_obj_add_event_cb(help, openHelpDialog, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* help_label = lv_label_create(help);
  lv_label_set_text(help_label, LV_SYMBOL_LIST "  Help");
  lv_obj_center(help_label);

  lv_obj_t* restart = lv_btn_create(actions);
  styleButton(restart);
  lv_obj_set_size(restart, 84, 36);
  lv_obj_add_event_cb(restart, onRestart, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* restart_label = lv_label_create(restart);
  lv_label_set_text(restart_label, LV_SYMBOL_POWER "  Restart");
  lv_obj_center(restart_label);
}

void createSettingsTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, false);

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), 156);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 8, LV_PART_MAIN);

  createSettingsRow(panel, "Orientation", onFlipDisplay, false, &orientation_label);
  updateOrientationLabel();

  createSettingsRow(panel, "Inactivity", onToggleIdleAction, false, &idle_label);
  updateIdleLabel();

  lv_obj_t* mode_pill = createSettingsRow(panel, "Remote Mode", onToggleControlMode, true, &mode_label);
  if (extended_mode) {
    lv_obj_add_state(mode_pill, LV_STATE_CHECKED);
  }
  updateModeLabel();
}

#if WLED_CYD_ENABLE_BATTERY
void createBatteryIndicator(lv_obj_t* parent) {
  if (!batteryAvailable()) {
    return;
  }

  battery_indicator = lv_obj_create(parent);
  lv_obj_remove_style_all(battery_indicator);
  lv_obj_set_size(battery_indicator, 30, 18);
  lv_obj_clear_flag(battery_indicator, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* body = lv_obj_create(battery_indicator);
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, 22, 14);
  lv_obj_align(body, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_border_width(body, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(body, lv_color_hex(kColorText), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

  battery_fill = lv_obj_create(body);
  lv_obj_remove_style_all(battery_fill);
  lv_obj_set_size(battery_fill, 3, 10);
  lv_obj_align(battery_fill, LV_ALIGN_TOP_LEFT, 1, 1);
  lv_obj_set_style_bg_opa(battery_fill, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(battery_fill, lv_color_hex(kColorDanger), LV_PART_MAIN);
  lv_obj_clear_flag(battery_fill, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* nub = lv_obj_create(battery_indicator);
  lv_obj_remove_style_all(nub);
  lv_obj_set_size(nub, 4, 6);
  lv_obj_align(nub, LV_ALIGN_LEFT_MID, 22, 0);
  lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(nub, lv_color_hex(kColorText), LV_PART_MAIN);
  lv_obj_clear_flag(nub, LV_OBJ_FLAG_SCROLLABLE);

  battery_charge = lv_label_create(battery_indicator);
  lv_label_set_text(battery_charge, LV_SYMBOL_CHARGE);
  lv_obj_set_style_text_font(battery_charge, &lv_font_montserrat_10, LV_PART_MAIN);
  lv_obj_set_style_text_color(battery_charge, lv_color_hex(kColorBatteryOk), LV_PART_MAIN);
  lv_obj_align(battery_charge, LV_ALIGN_CENTER, -4, 0);

  updateBatteryIndicator();
  lv_timer_create(updateBatteryTimer, 1000, nullptr);
}
#endif
