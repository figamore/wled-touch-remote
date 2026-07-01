#include "tabs.h"
#include "ui.h"
#include "../BatteryMonitor.h"
#include "../wled_api.h"
#include <Arduino.h>
#include <WiFi.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "generated/version.h"
#include "generated/wled_logo_png.h"

namespace {

// ── Image descriptors for modal dialogs ──────────────────────────────────────

const lv_img_dsc_t kHelpQrImage = {
    {LV_IMG_CF_TRUE_COLOR, 0, 0, kHelpQrWidth, kHelpQrHeight},
    kHelpQrPixelCount * sizeof(kHelpQrPixels[0]),
    reinterpret_cast<const uint8_t*>(kHelpQrPixels),
};


constexpr size_t kPeekCellCount = 48;
constexpr uint32_t kPeekFreshMs = 1200;
constexpr lv_coord_t kColorWheelSize = 168;
constexpr lv_coord_t kColorSelectorSize = 18;
constexpr lv_coord_t kPaletteChooserRowHeight = 56;
constexpr lv_coord_t kPalettePreviewHeight = 10;
lv_obj_t* peek_cells[kPeekCellCount] = {};
lv_obj_t* peek_status_label = nullptr;
lv_obj_t* color_wheel = nullptr;
lv_obj_t* color_selector = nullptr;
lv_obj_t* color_preview = nullptr;
lv_obj_t* color_hex_label = nullptr;
bool color_syncing = false;
lv_color_t* color_wheel_pixels = nullptr;
lv_img_dsc_t color_wheel_image = {
    {LV_IMG_CF_TRUE_COLOR, 0, 0, kColorWheelSize, kColorWheelSize},
    kColorWheelSize * kColorWheelSize * sizeof(lv_color_t),
    nullptr,
};
bool fx_controls_pending = false;
bool palette_chooser_pending = false;
std::vector<size_t> palette_table_order;
int palette_chooser_selected = -1;

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

uint8_t colorByte(uint32_t color, uint8_t shift) {
  return static_cast<uint8_t>((color >> shift) & 0xFF);
}

uint32_t makeRgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

uint32_t blendRgb(uint32_t a, uint32_t b, uint8_t amount) {
  const uint16_t inv = 255 - amount;
  return makeRgb((colorByte(a, 16) * inv + colorByte(b, 16) * amount) / 255,
                 (colorByte(a, 8) * inv + colorByte(b, 8) * amount) / 255,
                 (colorByte(a, 0) * inv + colorByte(b, 0) * amount) / 255);
}

uint32_t hsvToRgb(uint16_t h, uint8_t s, uint8_t v) {
  h %= 360;
  if (s == 0) return makeRgb(v, v, v);

  const uint8_t region = h / 60;
  const uint16_t rem = (h % 60) * 255 / 60;
  const uint8_t p = uint16_t(v) * (255 - s) / 255;
  const uint8_t q = uint16_t(v) * (255 - uint16_t(s) * rem / 255) / 255;
  const uint8_t t = uint16_t(v) * (255 - uint16_t(s) * (255 - rem) / 255) / 255;

  switch (region) {
    case 0: return makeRgb(v, t, p);
    case 1: return makeRgb(q, v, p);
    case 2: return makeRgb(p, v, t);
    case 3: return makeRgb(p, q, v);
    case 4: return makeRgb(t, p, v);
    default: return makeRgb(v, p, q);
  }
}

void rgbToHsv(uint32_t color, uint16_t& h, uint8_t& s, uint8_t& v) {
  const uint8_t r = colorByte(color, 16);
  const uint8_t g = colorByte(color, 8);
  const uint8_t b = colorByte(color, 0);
  const uint8_t maxc = std::max(r, std::max(g, b));
  const uint8_t minc = std::min(r, std::min(g, b));
  const uint8_t delta = maxc - minc;
  v = maxc;
  s = maxc == 0 ? 0 : uint16_t(delta) * 255 / maxc;
  if (delta == 0) {
    h = 0;
  } else if (maxc == r) {
    int16_t hue = 60 * int16_t(g - b) / delta;
    h = hue < 0 ? hue + 360 : hue;
  } else if (maxc == g) {
    h = 120 + 60 * int16_t(b - r) / delta;
  } else {
    h = 240 + 60 * int16_t(r - g) / delta;
  }
}

void generateColorWheelImage() {
  static bool generated = false;
  if (generated) return;

  if (!color_wheel_pixels) {
    color_wheel_pixels = static_cast<lv_color_t*>(malloc(kColorWheelSize * kColorWheelSize * sizeof(lv_color_t)));
    if (!color_wheel_pixels) return;
    color_wheel_image.data = reinterpret_cast<const uint8_t*>(color_wheel_pixels);
  }
  generated = true;

  const float center = (kColorWheelSize - 1) / 2.0f;
  const float radius = center - 1.0f;
  const uint32_t bg = kColorBg;
  for (lv_coord_t y = 0; y < kColorWheelSize; ++y) {
    for (lv_coord_t x = 0; x < kColorWheelSize; ++x) {
      const float dx = x - center;
      const float dy = y - center;
      const float dist = std::sqrt(dx * dx + dy * dy);
      uint32_t color = bg;
      if (dist <= radius + 1.0f) {
        float hue = (std::atan2(dy, dx) + float(M_PI) / 2.0f) * 180.0f / float(M_PI);
        if (hue < 0) hue += 360.0f;
        if (hue >= 360.0f) hue -= 360.0f;
        const uint8_t sat = uint8_t(std::min(1.0f, dist / radius) * 255.0f);
        color = hsvToRgb(uint16_t(hue), sat, 255);
        if (dist > radius) {
          const uint8_t edge = uint8_t(std::min(1.0f, dist - radius) * 255.0f);
          color = blendRgb(color, bg, edge);
        }
      }
      color_wheel_pixels[y * kColorWheelSize + x] = lv_color_hex(color);
    }
  }
}

void updateColorPreview(uint32_t color) {
  if (color_preview) {
    lv_obj_set_style_bg_color(color_preview, lv_color_hex(color), LV_PART_MAIN);
  }
  if (color_hex_label) {
    lv_label_set_text_fmt(color_hex_label, "#%02X%02X%02X",
                          colorByte(color, 16), colorByte(color, 8), colorByte(color, 0));
  }
}

void setColorControls(uint32_t color) {
  color_syncing = true;
  if (color_selector) {
    uint16_t h = 0;
    uint8_t s = 0, v = 0;
    rgbToHsv(color, h, s, v);
    const float rad = ((kColorWheelSize - 1) / 2.0f - 1.0f) * (float(s) / 255.0f);
    const float theta = float(h) * float(M_PI) / 180.0f;
    const lv_coord_t x = lv_coord_t(kColorWheelSize / 2 + std::sin(theta) * rad - kColorSelectorSize / 2);
    const lv_coord_t y = lv_coord_t(kColorWheelSize / 2 - std::cos(theta) * rad - kColorSelectorSize / 2);
    lv_obj_set_pos(color_selector, x, y);
  }
  updateColorPreview(color);
  color_syncing = false;
}

bool colorFromWheelPoint(lv_obj_t* wheel, uint32_t& color) {
  lv_indev_t* indev = lv_indev_get_act();
  if (!indev || !wheel) return false;

  lv_point_t point;
  lv_indev_get_point(indev, &point);
  lv_area_t area;
  lv_obj_get_coords(wheel, &area);
  const float local_x = point.x - area.x1;
  const float local_y = point.y - area.y1;
  const float center = (kColorWheelSize - 1) / 2.0f;
  const float radius = center - 1.0f;
  const float dx = local_x - center;
  const float dy = local_y - center;
  const float dist = std::sqrt(dx * dx + dy * dy);
  if (dist > radius + 12.0f) return false;

  float hue = (std::atan2(dy, dx) + float(M_PI) / 2.0f) * 180.0f / float(M_PI);
  if (hue < 0) hue += 360.0f;
  if (hue >= 360.0f) hue -= 360.0f;
  const uint8_t sat = uint8_t(std::min(1.0f, dist / radius) * 255.0f);
  color = hsvToRgb(uint16_t(hue), sat, 255);
  return true;
}

void onColorWheel(lv_event_t* event) {
  if (color_syncing) return;
  const lv_event_code_t code = lv_event_get_code(event);
  uint32_t color = wled::model().color;
  if (!colorFromWheelPoint(lv_event_get_target(event), color)) return;
  setColorControls(color);
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    wled::setColor(colorByte(color, 16), colorByte(color, 8), colorByte(color, 0));
  }
}

void onColorSwatch(lv_event_t* event) {
  const uint32_t c = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  setColorControls(c);
  wled::setColor((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

void createColorSwatches(lv_obj_t* parent) {
  const uint32_t current = wled::model().color;
  generateColorWheelImage();

  lv_obj_t* editor = lv_obj_create(parent);
  lv_obj_remove_style_all(editor);
  lv_obj_set_size(editor, LV_PCT(100), 214);
  lv_obj_set_flex_flow(editor, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(editor, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(editor, 8, LV_PART_MAIN);
  lv_obj_clear_flag(editor, LV_OBJ_FLAG_SCROLLABLE);

  if (color_wheel_image.data) {
    color_wheel = lv_obj_create(editor);
    lv_obj_remove_style_all(color_wheel);
    lv_obj_set_size(color_wheel, kColorWheelSize, kColorWheelSize);
    lv_obj_add_flag(color_wheel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_clear_flag(color_wheel, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(color_wheel, onColorWheel, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(color_wheel, onColorWheel, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(color_wheel, onColorWheel, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(color_wheel, onColorWheel, LV_EVENT_PRESS_LOST, nullptr);

    lv_obj_t* wheel_img = lv_img_create(color_wheel);
    lv_img_set_src(wheel_img, &color_wheel_image);
    lv_obj_set_size(wheel_img, kColorWheelSize, kColorWheelSize);
    lv_obj_clear_flag(wheel_img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    color_selector = lv_obj_create(color_wheel);
    lv_obj_remove_style_all(color_selector);
    lv_obj_set_size(color_selector, kColorSelectorSize, kColorSelectorSize);
    lv_obj_set_style_radius(color_selector, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(color_selector, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(color_selector, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_color(color_selector, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(color_selector, 3, LV_PART_MAIN);
    lv_obj_set_style_outline_color(color_selector, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_outline_width(color_selector, 2, LV_PART_MAIN);
    lv_obj_set_style_outline_opa(color_selector, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(color_selector, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  }

  lv_obj_t* chip_row = lv_obj_create(editor);
  lv_obj_remove_style_all(chip_row);
  lv_obj_set_size(chip_row, LV_PCT(100), 36);
  lv_obj_set_flex_flow(chip_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(chip_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(chip_row, 10, LV_PART_MAIN);
  lv_obj_clear_flag(chip_row, LV_OBJ_FLAG_SCROLLABLE);

  color_preview = lv_obj_create(chip_row);
  lv_obj_remove_style_all(color_preview);
  lv_obj_set_size(color_preview, 54, 30);
  lv_obj_set_style_bg_opa(color_preview, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(color_preview, 6, LV_PART_MAIN);
  lv_obj_set_style_border_width(color_preview, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(color_preview, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_border_opa(color_preview, LV_OPA_30, LV_PART_MAIN);

  color_hex_label = lv_label_create(chip_row);
  lv_obj_set_width(color_hex_label, 100);
  lv_obj_set_style_text_font(color_hex_label, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(color_hex_label, lv_color_hex(kColorAccent), LV_PART_MAIN);

  setColorControls(current);

  lv_obj_t* swatches = lv_obj_create(parent);
  lv_obj_remove_style_all(swatches);
  lv_obj_set_size(swatches, LV_PCT(100), 160);
  lv_obj_set_flex_flow(swatches, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(swatches, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(swatches, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_column(swatches, 8, LV_PART_MAIN);

  for (const ColorSwatch& swatch : kColorSwatches) {
    lv_obj_t* btn = lv_btn_create(swatches);
    styleButton(btn);
    lv_obj_set_size(btn, 132, 48);
    lv_obj_add_event_cb(btn, onColorSwatch, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(swatch.color)));
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, swatch.label);
    lv_obj_center(label);

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


void onFxSpeed(lv_event_t* event) {
  wled::setEffectParams(lv_slider_get_value(lv_event_get_target(event)), -1);
}

void onFxIntensity(lv_event_t* event) {
  wled::setEffectParams(-1, lv_slider_get_value(lv_event_get_target(event)));
}

void openFxControls();

uint32_t gradientColor(const uint32_t* stops, size_t count, uint8_t pos) {
  if (!stops || count == 0) return kColorBg;
  if (count == 1 || pos == 0) return stops[0];
  if (pos == 255) return stops[count - 1];
  const uint16_t scaled = uint16_t(pos) * (count - 1);
  const size_t left = scaled / 255;
  const size_t right = std::min(left + 1, count - 1);
  return blendRgb(stops[left], stops[right], scaled % 255);
}

uint32_t palettePreviewColor(uint16_t palette, uint8_t pos) {
  const uint32_t c1 = wled::model().color;
  const uint32_t c2 = 0x0066FF;
  const uint32_t c3 = 0xFFFFFF;

  static const uint32_t kParty[] = {
      0x9B00D5, 0xBD00B8, 0xDA0092, 0xF3005C, 0xF45500, 0xDC8F00, 0xD5B400, 0xD5D500,
      0xD59B00, 0xEF6600, 0xF90044, 0xE10086, 0xC400B0, 0xA300CF, 0x7600E8, 0x0032FC};
  static const uint32_t kRainbow[] = {
      0xFF0000, 0xEB7000, 0xD59B00, 0xD5BA00, 0xD5D500, 0x9CEB00, 0x00FF00, 0x00EB70,
      0x00D59B, 0x009CD4, 0x0000FF, 0x7000EB, 0x9B00D5, 0xBA00BB, 0xD5009B, 0xEB0072};
  static const uint32_t kRainbowStripe[] = {
      0xFF0000, 0x050505, 0xD59B00, 0x050505, 0xD5D500, 0x050505, 0x00FF00, 0x050505,
      0x00D59B, 0x050505, 0x0000FF, 0x050505, 0x9B00D5, 0x050505, 0xD5009B, 0x050505};
  static const uint32_t kCloud[] = {0x1E2A66, 0x6076B8, 0xDDE8FF, 0xFFFFFF, 0x879BE1, 0x2C397D};
  static const uint32_t kLava[] = {0x050000, 0x3A0000, 0x940000, 0xFF3A00, 0xFFB000, 0xFFFFFF};
  static const uint32_t kOcean[] = {0x001030, 0x003C7A, 0x007EA7, 0x00C2C7, 0xB4FFF4};
  static const uint32_t kForest[] = {0x001A00, 0x064B16, 0x16882B, 0x6BBF3A, 0xD9F99D};
  static const uint32_t kSunset[] = {0x22001E, 0x71115A, 0xE04447, 0xFF8A00, 0xFFD166};
  static const uint32_t kRivendell[] = {0x001A24, 0x0B5C63, 0x7CBF9E, 0xE8E8C6, 0x5B7C99};
  static const uint32_t kBreeze[] = {0x00283C, 0x0F7890, 0x6BE7D8, 0xD7FFF7, 0x4EA3FF};
  static const uint32_t kRedBlue[] = {0xFF0000, 0x150018, 0x001DFF, 0x110021, 0xFF0040};
  static const uint32_t kAnalogous[] = {0x2B00FF, 0x7C00FF, 0xD000B8, 0xFF005A, 0xFF1A1A};
  static const uint32_t kSplash[] = {0x001020, 0x00D7FF, 0x0040FF, 0xF000FF, 0xFF3D81};
  static const uint32_t kPastel[] = {0xFFB7C5, 0xFFD6A5, 0xFDFFB6, 0xCAFFBF, 0x9BF6FF, 0xBDB2FF};
  static const uint32_t kVintage[] = {0x2C1608, 0x6B2E16, 0xB85C38, 0xF4D58D, 0x7D5A50};
  static const uint32_t kDeparture[] = {0x000B2E, 0x00296B, 0x00B4D8, 0x90E0EF, 0xF72585};
  static const uint32_t kLandscape[] = {0x02111B, 0x064635, 0x519259, 0xF0BB62, 0xC84B31};
  static const uint32_t kSherbet[] = {0xFF006E, 0xFB5607, 0xFFBE0B, 0x3A86FF, 0x8338EC};
  static const uint32_t kFire[] = {0x000000, 0x440000, 0xB00000, 0xFF6A00, 0xFFF000};
  static const uint32_t kIce[] = {0x000018, 0x003B78, 0x00C2FF, 0xD7FFFF, 0xFFFFFF};
  static const uint32_t kApril[] = {0x001024, 0x00B8C8, 0x001024, 0x2CE43A, 0x101040, 0xFFB02E, 0x1A0B2E, 0xFF3E74};
  static const uint32_t kAurora[] = {0x003833, 0x006B22, 0x00E436, 0x25BEBE, 0x004466};
  static const uint32_t kAtlantica[] = {0x0037A6, 0x0080FF, 0x00C2A8, 0x00A85A, 0x2ED06E};
  static const uint32_t kAquaFlash[] = {0x102B2B, 0x5BE7E7, 0xC7FFE8, 0xFFFF7A, 0xC7FFE8, 0x5BE7E7, 0x102B2B};
  static const uint32_t kPinkCandy[] = {0x32002F, 0xFF4FB8, 0xFFFFFF, 0xFF9EE2, 0x7A1FA2};
  static const uint32_t kTraffic[] = {0xFF0000, 0xFF0000, 0xFFD000, 0xFFD000, 0x00CC33, 0x00CC33};

  switch (palette) {
    case 0:
    case 6: return gradientColor(kParty, sizeof(kParty) / sizeof(kParty[0]), pos);
    case 1: return hsvToRgb(pos * 360 / 255, 210, 255);
    case 2: return c1;
    case 3: { const uint32_t stops[] = {c1, c1, c2, c2}; return gradientColor(stops, 4, pos); }
    case 4: { const uint32_t stops[] = {c3, c2, c1}; return gradientColor(stops, 3, pos); }
    case 5: { const uint32_t stops[] = {c1, c1, c1, c2, c2, c2, c3, c3, c3, c1}; return gradientColor(stops, 10, pos); }
    case 7: return gradientColor(kCloud, sizeof(kCloud) / sizeof(kCloud[0]), pos);
    case 8: return gradientColor(kLava, sizeof(kLava) / sizeof(kLava[0]), pos);
    case 9: return gradientColor(kOcean, sizeof(kOcean) / sizeof(kOcean[0]), pos);
    case 10: return gradientColor(kForest, sizeof(kForest) / sizeof(kForest[0]), pos);
    case 11: return gradientColor(kRainbow, sizeof(kRainbow) / sizeof(kRainbow[0]), pos);
    case 12: return gradientColor(kRainbowStripe, sizeof(kRainbowStripe) / sizeof(kRainbowStripe[0]), pos);
    case 13:
    case 21: return gradientColor(kSunset, sizeof(kSunset) / sizeof(kSunset[0]), pos);
    case 14: return gradientColor(kRivendell, sizeof(kRivendell) / sizeof(kRivendell[0]), pos);
    case 15: return gradientColor(kBreeze, sizeof(kBreeze) / sizeof(kBreeze[0]), pos);
    case 16: return gradientColor(kRedBlue, sizeof(kRedBlue) / sizeof(kRedBlue[0]), pos);
    case 18: return gradientColor(kAnalogous, sizeof(kAnalogous) / sizeof(kAnalogous[0]), pos);
    case 19: return gradientColor(kSplash, sizeof(kSplash) / sizeof(kSplash[0]), pos);
    case 20: return gradientColor(kPastel, sizeof(kPastel) / sizeof(kPastel[0]), pos);
    case 23:
    case 32: return gradientColor(kVintage, sizeof(kVintage) / sizeof(kVintage[0]), pos);
    case 24: return gradientColor(kDeparture, sizeof(kDeparture) / sizeof(kDeparture[0]), pos);
    case 25:
    case 26: return gradientColor(kLandscape, sizeof(kLandscape) / sizeof(kLandscape[0]), pos);
    case 27:
    case 57:
    case 70: return gradientColor(kSherbet, sizeof(kSherbet) / sizeof(kSherbet[0]), pos);
    case 35:
    case 39:
    case 66:
    case 67:
    case 68:
    case 69: return gradientColor(kFire, sizeof(kFire) / sizeof(kFire[0]), pos);
    case 36:
    case 37:
    case 43:
    case 54:
    case 60:
    case 64:
    case 65: return gradientColor(kIce, sizeof(kIce) / sizeof(kIce[0]), pos);
    case 46: return gradientColor(kApril, sizeof(kApril) / sizeof(kApril[0]), pos);
    case 50:
    case 55: return gradientColor(kAurora, sizeof(kAurora) / sizeof(kAurora[0]), pos);
    case 51: return gradientColor(kAtlantica, sizeof(kAtlantica) / sizeof(kAtlantica[0]), pos);
    case 63: return gradientColor(kAquaFlash, sizeof(kAquaFlash) / sizeof(kAquaFlash[0]), pos);
    case 61: return gradientColor(kPinkCandy, sizeof(kPinkCandy) / sizeof(kPinkCandy[0]), pos);
    case 71: return gradientColor(kTraffic, sizeof(kTraffic) / sizeof(kTraffic[0]), pos);
    default:
      return hsvToRgb((uint16_t(pos) + palette * 23) % 360, 210, 240);
  }
}

bool paletteNameLess(const std::vector<std::string>& palettes, size_t left, size_t right) {
  return palettes[left] < palettes[right];
}

std::vector<size_t> paletteDisplayOrder(const std::vector<std::string>& palettes) {
  std::vector<size_t> order;
  order.reserve(palettes.size());
  if (!palettes.empty()) {
    order.push_back(0);
  }
  for (size_t i = 1; i < palettes.size(); ++i) {
    order.push_back(i);
  }
  std::sort(order.begin() + (order.empty() ? 0 : 1), order.end(),
            [&palettes](size_t left, size_t right) {
              return paletteNameLess(palettes, left, right);
            });
  return order;
}

const char* paletteNameOrFallback(const std::vector<std::string>& palettes, size_t id) {
  if (id < palettes.size() && !palettes[id].empty()) {
    return palettes[id].c_str();
  }
  return "Palette";
}

void drawPalettePreviewArea(lv_draw_ctx_t* draw_ctx, const lv_area_t& coords, uint16_t palette);

void drawPalettePreview(lv_event_t* event) {
  lv_draw_ctx_t* draw_ctx = lv_event_get_draw_ctx(event);
  if (!draw_ctx) return;

  lv_obj_t* strip = lv_event_get_target(event);
  const uintptr_t palette = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  lv_area_t coords;
  lv_obj_get_coords(strip, &coords);
  drawPalettePreviewArea(draw_ctx, coords, static_cast<uint16_t>(palette));
}

void drawPalettePreviewArea(lv_draw_ctx_t* draw_ctx, const lv_area_t& coords, uint16_t palette) {
  const lv_coord_t width = lv_area_get_width(&coords);
  if (width <= 0) return;

  lv_draw_rect_dsc_t rect;
  lv_draw_rect_dsc_init(&rect);
  rect.bg_opa = LV_OPA_COVER;
  rect.border_width = 0;
  rect.radius = 0;

  lv_area_t col = coords;
  for (lv_coord_t x = 0; x < width; ++x) {
    const uint8_t pos = width == 1 ? 0 : uint8_t((uint32_t(x) * 255U) / uint32_t(width - 1));
    rect.bg_color = lv_color_hex(palettePreviewColor(palette, pos));
    col.x1 = coords.x1 + x;
    col.x2 = col.x1;
    lv_draw_rect(draw_ctx, &rect, &col);
  }
}

lv_obj_t* createPalettePreview(lv_obj_t* parent, uint16_t palette, lv_coord_t height = kPalettePreviewHeight) {
  lv_obj_t* strip = lv_obj_create(parent);
  lv_obj_remove_style_all(strip);
  lv_obj_set_size(strip, LV_PCT(100), height);
  lv_obj_set_style_radius(strip, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(strip, lv_color_hex(kColorBg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(strip, true, LV_PART_MAIN);
  lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(strip, drawPalettePreview, LV_EVENT_DRAW_MAIN_END,
                      reinterpret_cast<void*>(static_cast<uintptr_t>(palette)));
  return strip;
}

void onPaletteTableClicked(lv_event_t* event) {
  lv_obj_t* table = lv_event_get_target(event);
  uint16_t row = LV_TABLE_CELL_NONE;
  uint16_t col = LV_TABLE_CELL_NONE;
  lv_table_get_selected_cell(table, &row, &col);
  if (row == LV_TABLE_CELL_NONE || row >= palette_table_order.size()) return;

  const size_t palette_id = palette_table_order[row];
  if (palette_id > 255) return;
  palette_chooser_selected = static_cast<int>(palette_id);
  wled::setPalette(static_cast<uint8_t>(palette_id));
  lv_obj_invalidate(table);
}

void onPaletteTableDrawPart(lv_event_t* event) {
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(event);
  if (!dsc || !lv_obj_draw_part_check_type(dsc, &lv_table_class, LV_TABLE_DRAW_PART_CELL)) return;

  const uint16_t row = dsc->id;
  if (row >= palette_table_order.size()) return;

  const bool selected = static_cast<int>(palette_table_order[row]) == palette_chooser_selected;
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_DRAW_PART_BEGIN) {
    if (dsc->rect_dsc) {
      dsc->rect_dsc->radius = 8;
      dsc->rect_dsc->border_width = selected ? 1 : 0;
      dsc->rect_dsc->border_color = lv_color_hex(kColorSelectedBorder);
      dsc->rect_dsc->bg_color = lv_color_hex(selected ? kColorSelected : (row % 2 ? kColorSurfaceRaised : kColorSurface));
    }
    if (dsc->label_dsc) {
      dsc->label_dsc->align = LV_TEXT_ALIGN_CENTER;
      dsc->label_dsc->color = lv_color_hex(selected ? 0xFFFFFF : kColorText);
    }
    return;
  }

  if (code != LV_EVENT_DRAW_PART_END || !dsc->draw_ctx || !dsc->draw_area) return;

  lv_area_t preview = *dsc->draw_area;
  preview.x1 += 9;
  preview.x2 -= 9;
  preview.y1 = preview.y2 - 11;
  preview.y2 -= 3;
  if (preview.x1 <= preview.x2 && preview.y1 <= preview.y2) {
    drawPalettePreviewArea(dsc->draw_ctx, preview, static_cast<uint16_t>(palette_table_order[row]));
  }
}

lv_obj_t* beginPaletteModal(const char* title_text) {
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
  lv_obj_set_size(content, 304, 190);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -5);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(content, 8, LV_PART_MAIN);
  configurePageScroll(content, true);
  return content;
}

void openPaletteChooser() {
  if (help_dialog) return;

  const wled::Model& m = wled::model();
  palette_table_order = paletteDisplayOrder(m.palettes);
  palette_chooser_selected = m.palette;

  lv_obj_t* content = beginPaletteModal("Choose Palette");
  if (m.palettes.empty()) {
    lv_obj_t* hint = lv_label_create(content);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "Palette list not loaded yet.");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_style(hint, &style_label_muted, LV_PART_MAIN);
    return;
  }

  lv_obj_t* table = lv_table_create(content);
  lv_obj_set_width(table, LV_PCT(100));
  lv_obj_set_flex_grow(table, 1);
  lv_table_set_col_cnt(table, 1);
  lv_table_set_row_cnt(table, palette_table_order.size());
  lv_table_set_col_width(table, 0, 296);
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
  lv_obj_set_style_min_height(table, kPaletteChooserRowHeight, LV_PART_ITEMS);
  lv_obj_set_style_pad_left(table, 10, LV_PART_ITEMS);
  lv_obj_set_style_pad_right(table, 10, LV_PART_ITEMS);
  lv_obj_set_style_pad_top(table, 9, LV_PART_ITEMS);
  lv_obj_set_style_pad_bottom(table, 20, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(table, lv_color_hex(kColorText), LV_PART_ITEMS);
  lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);

  uint16_t selected_row = LV_TABLE_CELL_NONE;
  for (size_t row = 0; row < palette_table_order.size(); ++row) {
    const size_t id = palette_table_order[row];
    lv_table_set_cell_value(table, row, 0, paletteNameOrFallback(m.palettes, id));
    lv_table_add_cell_ctrl(table, row, 0, LV_TABLE_CELL_CTRL_TEXT_CROP);
    if (static_cast<int>(id) == palette_chooser_selected) {
      selected_row = static_cast<uint16_t>(row);
    }
  }
  lv_obj_add_event_cb(table, onPaletteTableClicked, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(table, onPaletteTableDrawPart, LV_EVENT_DRAW_PART_BEGIN, nullptr);
  lv_obj_add_event_cb(table, onPaletteTableDrawPart, LV_EVENT_DRAW_PART_END, nullptr);

  if (selected_row != LV_TABLE_CELL_NONE) {
    lv_obj_scroll_to_y(table, selected_row * kPaletteChooserRowHeight, LV_ANIM_OFF);
  }
}

void openPaletteChooserAsync(void*) {
  palette_chooser_pending = false;
  closeHelpDialog(nullptr);
  openPaletteChooser();
}

void schedulePaletteChooserOpen(lv_event_t*) {
  if (palette_chooser_pending) return;
  palette_chooser_pending = true;
  lv_async_call(openPaletteChooserAsync, nullptr);
}

void openFxControlsAsync(void*) {
  fx_controls_pending = false;
  openFxControls();
}

void scheduleFxControlsOpen() {
  if (fx_controls_pending) return;
  fx_controls_pending = true;
  lv_async_call(openFxControlsAsync, nullptr);
}

// Generic per-effect controls (speed, intensity, palette) that apply to any effect over the
// API, replacing the old remote.json button-mapped controls.
void openFxControls() {
  if (help_dialog) {
    return;
  }

  lv_obj_t* content = beginInfoModal("FX Controls");
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(content, 8, LV_PART_MAIN);
  lv_obj_clear_flag(content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  configurePageScroll(content, true);

  const wled::Model& m = wled::model();
  lv_obj_t* value_label = nullptr;

  addLabel(content, "Speed");
  createSlider(content, 0, 255, m.speed, onFxSpeed, &value_label);
  addLabel(content, "Intensity");
  createSlider(content, 0, 255, m.intensity, onFxIntensity, &value_label);

  addLabel(content, "Palette");
  lv_obj_t* palette_btn = lv_btn_create(content);
  styleButton(palette_btn, m.palette >= 0);
  lv_obj_set_size(palette_btn, LV_PCT(100), 48);
  lv_obj_add_event_cb(palette_btn, schedulePaletteChooserOpen, LV_EVENT_CLICKED, nullptr);
  if (m.palette >= 0) {
    lv_obj_add_state(palette_btn, LV_STATE_CHECKED);
  }

  lv_obj_t* palette_label = lv_label_create(palette_btn);
  lv_obj_set_width(palette_label, LV_PCT(88));
  lv_label_set_long_mode(palette_label, LV_LABEL_LONG_DOT);
  if (m.palette >= 0 && static_cast<size_t>(m.palette) < m.palettes.size()) {
    lv_label_set_text(palette_label, paletteNameOrFallback(m.palettes, static_cast<size_t>(m.palette)));
  } else {
    lv_label_set_text(palette_label, "Choose Palette");
  }
  lv_obj_align(palette_label, LV_ALIGN_CENTER, 0, -6);

  const uint16_t preview_id = m.palette >= 0 ? static_cast<uint16_t>(m.palette) : 0;
  lv_obj_t* preview = createPalettePreview(palette_btn, preview_id, 8);
  lv_obj_align(preview, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void onEffectTableClicked(lv_event_t* event) {
  lv_obj_t* table = lv_event_get_target(event);
  uint16_t row = LV_TABLE_CELL_NONE;
  uint16_t col = LV_TABLE_CELL_NONE;
  lv_table_get_selected_cell(table, &row, &col);
  const size_t count = wled::model().effects.size();
  if (row == LV_TABLE_CELL_NONE || col == LV_TABLE_CELL_NONE || row >= count) {
    return;
  }

  if (col == 1 && row == selected_effect_id) {
    scheduleFxControlsOpen();
  } else {
    activateEffectId(static_cast<uint8_t>(row));
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
  if (row >= wled::model().effects.size()) {
    return;
  }

  const bool selected = row == selected_effect_id;

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
  brightness_slider = createSlider(panel, 1, 255, state.brightness, onBrightness, &brightness_label);

  lv_obj_t* peek_panel = createPanel(tab);
  lv_obj_set_size(peek_panel, LV_PCT(100), 70);
  lv_obj_set_flex_flow(peek_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(peek_panel, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(peek_panel, 8, LV_PART_MAIN);
  lv_obj_t* peek_header = lv_obj_create(peek_panel);
  lv_obj_remove_style_all(peek_header);
  lv_obj_set_size(peek_header, LV_PCT(100), 18);
  lv_obj_set_flex_flow(peek_header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(peek_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(peek_header, LV_OBJ_FLAG_SCROLLABLE);

  addLabel(peek_header, "Live");
  peek_status_label = lv_label_create(peek_header);
  lv_obj_set_width(peek_status_label, 148);
  lv_label_set_long_mode(peek_status_label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(peek_status_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_obj_set_style_text_font(peek_status_label, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_add_style(peek_status_label, &style_label_muted, LV_PART_MAIN);
  lv_label_set_text(peek_status_label, "Waiting");

  lv_obj_t* strip = lv_obj_create(peek_panel);
  lv_obj_remove_style_all(strip);
  lv_obj_set_size(strip, LV_PCT(100), 18);
  lv_obj_set_style_bg_color(strip, lv_color_hex(kColorBg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(strip, 4, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(strip, true, LV_PART_MAIN);
  lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
  lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
  for (size_t i = 0; i < kPeekCellCount; ++i) {
    peek_cells[i] = lv_obj_create(strip);
    lv_obj_remove_style_all(peek_cells[i]);
    lv_obj_set_width(peek_cells[i], 0);
    lv_obj_set_flex_grow(peek_cells[i], 1);
    lv_obj_set_height(peek_cells[i], LV_PCT(100));
    lv_obj_set_style_bg_opa(peek_cells[i], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(peek_cells[i], lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_clear_flag(peek_cells[i], LV_OBJ_FLAG_SCROLLABLE);
  }
}

void updatePeekStrip() {
  if (!peek_cells[0]) return;
  static uint32_t seen = UINT32_MAX;
  static bool seen_fresh = false;
  static bool seen_online = false;
  const uint32_t rev = wled::liveRevision();
  const uint32_t age = wled::liveFrameAgeMs(millis());
  const bool fresh = age != UINT32_MAX && age <= kPeekFreshMs;
  const bool online = wled::online();
  if (rev == seen && fresh == seen_fresh && online == seen_online) return;
  seen = rev;
  seen_fresh = fresh;
  seen_online = online;

  uint16_t count = 0, w = 0, h = 0;
  const uint8_t* leds = wled::liveLeds(count, w, h);
  if (peek_status_label) {
    if (!online) {
      lv_label_set_text(peek_status_label, "Searching");
    } else if (!wled::livePeekEnabled()) {
      lv_label_set_text(peek_status_label, "Paused");
    } else if (!fresh || !leds || !count) {
      lv_label_set_text(peek_status_label, "Waiting");
    } else {
      lv_label_set_text(peek_status_label, "Live");
    }
  }
  for (size_t i = 0; i < kPeekCellCount; ++i) {
    uint32_t color = kColorBg;
    if (fresh && leds && count) {
      const size_t idx = i * count / kPeekCellCount;
      const uint8_t* p = leds + idx * 3;
      color = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
    }
    lv_obj_set_style_bg_color(peek_cells[i], lv_color_hex(color), LV_PART_MAIN);
  }
}

void updateColorControlsFromModel() {
  if (!color_preview) return;
  setColorControls(wled::model().color);
}

void createLooksTab(lv_obj_t* tab) {
  for (lv_obj_t*& preset_button : preset_buttons) {
    preset_button = nullptr;
  }

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, true);

  const std::vector<wled::PresetInfo>& presets = wled::model().presets;

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), 540);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);

  addLabel(panel, "Presets");
  if (presets.empty()) {
    lv_obj_t* hint = lv_label_create(panel);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "Waiting for WLED. Add this remote's MAC in WLED, then tap Ping on the Info tab.");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_style(hint, &style_label_muted, LV_PART_MAIN);
  } else {
    lv_obj_t* list = lv_obj_create(panel);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    const size_t shown = std::min<size_t>(presets.size(), kExtendedPresetCount);
    for (size_t i = 0; i < shown; ++i) {
      const uint8_t id = presets[i].id;
      lv_obj_t* btn = lv_btn_create(list);
      styleButton(btn, true);
      lv_obj_set_size(btn, LV_PCT(100), 40);
      lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
      lv_obj_add_event_cb(btn, onPreset, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
      preset_buttons[i] = btn;
      if (id == selected_preset) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
      }

      lv_obj_t* label = lv_label_create(btn);
      lv_obj_set_width(label, LV_PCT(100));
      lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
      if (presets[i].name.empty()) {
        lv_label_set_text_fmt(label, "Preset %u", static_cast<unsigned>(id));
      } else {
        lv_label_set_text(label, presets[i].name.c_str());
      }
      lv_obj_center(label);
    }
  }

  addLabel(panel, "Colors");
  createColorSwatches(panel);
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

  const std::vector<std::string>& effects = wled::model().effects;
  if (effects.empty()) {
    lv_obj_t* panel = createPanel(tab);
    lv_obj_set_size(panel, LV_PCT(100), kTabCardHeight);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, 6, LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Effects loading");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);

    lv_obj_t* hint = lv_label_create(panel);
    lv_label_set_text(hint, "Add this remote's MAC in WLED, then tap Ping on the Info tab.");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_style(hint, &style_label_muted, LV_PART_MAIN);
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
  lv_table_set_row_cnt(table, effects.size());
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

  for (size_t i = 0; i < effects.size(); ++i) {
    lv_table_set_cell_value(table, i, 0, effects[i].c_str());
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

  conn_label = lv_label_create(panel);
  lv_obj_set_width(conn_label, LV_PCT(100));
  lv_label_set_long_mode(conn_label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(conn_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(conn_label, "Searching for WLED...");
  lv_obj_set_style_text_color(conn_label, lv_color_hex(kColorWarn), LV_PART_MAIN);
  updateConnLabel();

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

  lv_obj_t* power_action = lv_btn_create(actions);
  styleButton(power_action);
#if WLED_CYD_ENABLE_SHUTDOWN
  lv_obj_set_size(power_action, 98, 36);
  lv_obj_add_event_cb(power_action, onShutdown, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* power_action_label = lv_label_create(power_action);
  lv_label_set_text(power_action_label, LV_SYMBOL_POWER "  Shutdown");
#else
  lv_obj_set_size(power_action, 84, 36);
  lv_obj_add_event_cb(power_action, onRestart, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* power_action_label = lv_label_create(power_action);
  lv_label_set_text(power_action_label, LV_SYMBOL_POWER "  Restart");
#endif
  lv_obj_center(power_action_label);
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
