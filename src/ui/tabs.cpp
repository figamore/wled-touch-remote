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
#include <strings.h>
#include <string>
#include <vector>
#include "generated/version.h"
#include "generated/wled_catalog.h"
#include "generated/wled_logo_png.h"

namespace {

// ── Image descriptors for modal dialogs ──────────────────────────────────────

const lv_img_dsc_t kHelpQrImage = {
    {LV_IMG_CF_TRUE_COLOR, 0, 0, kHelpQrWidth, kHelpQrHeight},
    kHelpQrPixelCount * sizeof(kHelpQrPixels[0]),
    reinterpret_cast<const uint8_t*>(kHelpQrPixels),
};


constexpr lv_coord_t kColorWheelSize = 138;
constexpr lv_coord_t kColorSelectorSize = 18;
constexpr lv_coord_t kPaletteRowPadTop = 7;
constexpr lv_coord_t kPaletteRowPadBottom = 21;
constexpr uint32_t kSliderSendIntervalMs = 150;

lv_obj_t* color_wheel = nullptr;
lv_obj_t* color_selector = nullptr;
lv_obj_t* solid_color_button = nullptr;
bool color_syncing = false;
lv_color_t* color_wheel_pixels = nullptr;
lv_img_dsc_t color_wheel_image = {
    {LV_IMG_CF_TRUE_COLOR, 0, 0, kColorWheelSize, kColorWheelSize},
    kColorWheelSize * kColorWheelSize * sizeof(lv_color_t),
    nullptr,
};
bool fx_controls_pending = false;
bool fx_rebuild_pending = false;
std::vector<size_t> palette_table_order;
int palette_chooser_selected = -1;
bool help_dialog_deleting = false;
lv_obj_t* preset_name_dialog = nullptr;
lv_obj_t* preset_name_input = nullptr;
lv_obj_t* target_dialog = nullptr;
lv_obj_t* controller_name_dialog = nullptr;
lv_obj_t* controller_name_input = nullptr;
size_t controller_name_index = 0;
lv_obj_t* palette_list_table = nullptr;  // table embedded in the Colors tab
lv_obj_t* fx_speed_slider = nullptr;
lv_obj_t* fx_speed_value = nullptr;
lv_obj_t* fx_intensity_slider = nullptr;
lv_obj_t* fx_intensity_value = nullptr;
lv_obj_t* fx_custom_slider[3] = {nullptr, nullptr, nullptr};
lv_obj_t* fx_custom_value[3] = {nullptr, nullptr, nullptr};
lv_obj_t* now_playing_label = nullptr;
lv_obj_t* fx_table = nullptr;

// FX tab category filter: effects are tagged 1D/2D/audio in the baked catalog.
enum class FxFilter : uint8_t { kAll, k1D, k2D, kSound };
FxFilter fx_filter = FxFilter::kAll;
std::vector<uint16_t> fx_table_order;  // table row -> effect id

bool fxMatchesFilter(uint8_t flags, FxFilter filter) {
  switch (filter) {
    case FxFilter::k1D: return flags & kFxFlag1D;
    case FxFilter::k2D: return flags & kFxFlag2D;
    case FxFilter::kSound: return flags & (kFxFlagAudioVolume | kFxFlagAudioFreq);
    default: return true;
  }
}

// Solid stays pinned first (like the WLED UI); everything else alphabetical.
void buildFxTableOrder() {
  fx_table_order.clear();
  for (size_t id = 0; id < kWledFxCount; ++id) {
    if (!kWledFx[id].name || !kWledFx[id].name[0]) continue;
    if (!fxMatchesFilter(kWledFx[id].flags, fx_filter)) continue;
    fx_table_order.push_back(static_cast<uint16_t>(id));
  }
  std::sort(fx_table_order.begin(), fx_table_order.end(), [](uint16_t a, uint16_t b) {
    if (a == 0 || b == 0) return a == 0;
    return strcasecmp(kWledFx[a].name, kWledFx[b].name) < 0;
  });
}

// '|'-separated slider labels from the catalog: speed|intensity|c1|c2|c3|o1|o2|o3.
void fxSliderLabels(uint16_t fx_id, std::string out[8]) {
  for (int i = 0; i < 8; ++i) out[i].clear();
  if (fx_id >= kWledFxCount || !kWledFx[fx_id].sliders) return;
  const char* p = kWledFx[fx_id].sliders;
  int slot = 0;
  while (*p && slot < 8) {
    const char* sep = strchr(p, '|');
    if (!sep) {
      out[slot] = p;
      break;
    }
    out[slot].assign(p, sep - p);
    p = sep + 1;
    slot++;
  }
}

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

lv_obj_t* createLabeledSlider(lv_obj_t* parent,
                              const char* name,
                              int min,
                              int max,
                              int value,
                              lv_event_cb_t cb,
                              lv_obj_t** value_label,
                              void* user_data = nullptr) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, 38);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_left(row, name ? 0 : 10, LV_PART_MAIN);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);

  if (name) {
    lv_obj_t* name_label = lv_label_create(row);
    lv_obj_set_width(name_label, 72);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_add_style(name_label, &style_label_muted, LV_PART_MAIN);
    lv_label_set_text(name_label, name);
  }

  lv_obj_t* slider = lv_slider_create(row);
  lv_slider_set_range(slider, min, max);
  lv_slider_set_value(slider, value, LV_ANIM_OFF);
  lv_obj_set_size(slider, name ? 150 : 190, 12);
  lv_obj_set_flex_grow(slider, 1);
  lv_obj_add_flag(slider, LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_clear_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_style(slider, &style_slider, LV_PART_MAIN);
  lv_obj_add_style(slider, &style_slider_indicator, LV_PART_INDICATOR);
  lv_obj_add_style(slider, &style_knob, LV_PART_KNOB);
  lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, user_data);
  lv_obj_add_event_cb(slider, cb, LV_EVENT_RELEASED, user_data);
  lv_obj_add_event_cb(slider, cb, LV_EVENT_PRESS_LOST, user_data);

  *value_label = lv_label_create(row);
  lv_obj_set_width(*value_label, 34);
  lv_obj_clear_flag(*value_label, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_label_set_text_fmt(*value_label, "%d", value);
  lv_obj_set_style_text_align(*value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

  return slider;
}

// Rate-limits slider commands while dragging; the release always goes through
// so the final value is never lost.
bool shouldSendSliderValue(lv_event_code_t code, uint32_t& last_send_ms) {
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) return true;
  const uint32_t now = millis();
  if (now - last_send_ms < kSliderSendIntervalMs) return false;
  last_send_ms = now;
  return true;
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

void wheelSampleRgb(float dx, float dy, float radius, float bg_r, float bg_g, float bg_b,
                    float& r, float& g, float& b) {
  const float dist = std::sqrt(dx * dx + dy * dy);
  if (dist > radius) {
    r = bg_r;
    g = bg_g;
    b = bg_b;
    return;
  }

  float hue = (std::atan2(dy, dx) + float(M_PI) / 2.0f) * (180.0f / float(M_PI));
  if (hue < 0.0f) hue += 360.0f;
  const float sat = std::min(1.0f, dist / radius);

  const float hp = hue / 60.0f;
  const float x = 1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f);
  float pr = 0, pg = 0, pb = 0;
  if (hp < 1)      { pr = 1; pg = x; }
  else if (hp < 2) { pr = x; pg = 1; }
  else if (hp < 3) { pg = 1; pb = x; }
  else if (hp < 4) { pg = x; pb = 1; }
  else if (hp < 5) { pr = x; pb = 1; }
  else             { pr = 1; pb = x; }
  r = (1.0f - sat + sat * pr) * 255.0f;
  g = (1.0f - sat + sat * pg) * 255.0f;
  b = (1.0f - sat + sat * pb) * 255.0f;
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

  // 2x2 supersampling anti-aliases the rim; ordered dithering hides the RGB565
  // banding that made the gradients look stepped. Blends into the panel surface
  // colour the wheel actually sits on.
  static const uint8_t kBayer[4][4] = {
      {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  const float center = (kColorWheelSize - 1) / 2.0f;
  const float radius = center - 1.5f;
  const float bg_r = float((kColorSurface >> 16) & 0xFF);
  const float bg_g = float((kColorSurface >> 8) & 0xFF);
  const float bg_b = float(kColorSurface & 0xFF);

  for (lv_coord_t y = 0; y < kColorWheelSize; ++y) {
    for (lv_coord_t x = 0; x < kColorWheelSize; ++x) {
      float r_sum = 0, g_sum = 0, b_sum = 0;
      for (int sub = 0; sub < 4; ++sub) {
        const float sx = (sub & 1) ? 0.75f : 0.25f;
        const float sy = (sub & 2) ? 0.75f : 0.25f;
        float r, g, b;
        wheelSampleRgb(x + sx - center, y + sy - center, radius, bg_r, bg_g, bg_b, r, g, b);
        r_sum += r;
        g_sum += g;
        b_sum += b;
      }
      const float dither = kBayer[y & 3][x & 3] / 16.0f - 0.46875f;
      const int r = std::min(255, std::max(0, int(r_sum / 4.0f + dither * 8.0f + 0.5f)));
      const int g = std::min(255, std::max(0, int(g_sum / 4.0f + dither * 4.0f + 0.5f)));
      const int b = std::min(255, std::max(0, int(b_sum / 4.0f + dither * 8.0f + 0.5f)));
      color_wheel_pixels[y * kColorWheelSize + x] = lv_color_make(r, g, b);
    }
  }
}

void setColorControls(uint32_t color) {
  color_syncing = true;
  if (color_selector) {
    uint16_t h = 0;
    uint8_t s = 0, v = 0;
    rgbToHsv(color, h, s, v);
    const float rad = ((kColorWheelSize - 1) / 2.0f - 1.5f) * (float(s) / 255.0f);
    const float theta = float(h) * float(M_PI) / 180.0f;
    const lv_coord_t x = lv_coord_t(kColorWheelSize / 2 + std::sin(theta) * rad - kColorSelectorSize / 2);
    const lv_coord_t y = lv_coord_t(kColorWheelSize / 2 - std::cos(theta) * rad - kColorSelectorSize / 2);
    lv_obj_set_pos(color_selector, x, y);
    lv_obj_set_style_bg_color(color_selector, lv_color_hex(color), LV_PART_MAIN);
  }
  color_syncing = false;
}

// clamp=false requires the touch to start on the wheel; clamp=true (while a drag is
// being tracked) projects any point onto the wheel so the finger can wander past the
// rim without the selection jumping or going dead.
bool colorFromWheelPoint(lv_obj_t* wheel, uint32_t& color, bool clamp) {
  lv_indev_t* indev = lv_indev_get_act();
  if (!indev || !wheel) return false;

  lv_point_t point;
  lv_indev_get_point(indev, &point);
  lv_area_t area;
  lv_obj_get_coords(wheel, &area);
  const float local_x = point.x - area.x1;
  const float local_y = point.y - area.y1;
  const float center = (kColorWheelSize - 1) / 2.0f;
  const float radius = center - 1.5f;
  const float dx = local_x - center;
  const float dy = local_y - center;
  float dist = std::sqrt(dx * dx + dy * dy);
  if (!clamp && dist > radius + 12.0f) return false;
  if (dist > radius) dist = radius;

  float hue = (std::atan2(dy, dx) + float(M_PI) / 2.0f) * 180.0f / float(M_PI);
  if (hue < 0) hue += 360.0f;
  if (hue >= 360.0f) hue -= 360.0f;
  const uint8_t sat = uint8_t(std::min(1.0f, dist / radius) * 255.0f);
  color = hsvToRgb(uint16_t(hue), sat, 255);
  return true;
}

void onColorWheel(lv_event_t* event) {
  if (color_syncing) return;
  static uint32_t last_send_ms = 0;
  static bool tracking = false;
  const lv_event_code_t code = lv_event_get_code(event);

  if (code == LV_EVENT_PRESSED) tracking = false;
  uint32_t color = wled::model().color;
  if (!colorFromWheelPoint(lv_event_get_target(event), color, tracking)) return;
  tracking = code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST;

  setColorControls(color);
  if (shouldSendSliderValue(code, last_send_ms)) {
    wled::setColor(colorByte(color, 16), colorByte(color, 8), colorByte(color, 0));
  }
}


// Switches to WLED's Solid effect without changing the color selected on the wheel.
void onSolidColor(lv_event_t* event) {
  activateEffectId(0);
  lv_obj_add_state(lv_event_get_target(event), LV_STATE_CHECKED);
}


void createColorWheelEditor(lv_obj_t* parent) {
  const uint32_t current = wled::model().color;
  generateColorWheelImage();

  lv_obj_t* editor = lv_obj_create(parent);
  lv_obj_remove_style_all(editor);
  lv_obj_set_size(editor, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(editor, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(editor, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(editor, 18, LV_PART_MAIN);
  lv_obj_clear_flag(editor, LV_OBJ_FLAG_SCROLLABLE);

  if (color_wheel_image.data) {
    color_wheel = lv_obj_create(editor);
    lv_obj_remove_style_all(color_wheel);
    lv_obj_set_size(color_wheel, kColorWheelSize, kColorWheelSize);
    lv_obj_add_flag(color_wheel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    // no scroll chaining in either direction: a swipe on the wheel picks a colour,
    // it must never scroll the page underneath (that made the wheel clip and jump)
    lv_obj_clear_flag(color_wheel, LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE);
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
    lv_obj_set_style_bg_opa(color_selector, LV_OPA_COVER, LV_PART_MAIN);  // filled with the picked colour
    lv_obj_set_style_border_color(color_selector, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(color_selector, 3, LV_PART_MAIN);
    lv_obj_set_style_outline_color(color_selector, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_outline_width(color_selector, 2, LV_PART_MAIN);
    lv_obj_set_style_outline_opa(color_selector, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(color_selector, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  }

  
  solid_color_button = lv_btn_create(editor);
  styleButton(solid_color_button, true);
  lv_obj_set_size(solid_color_button, 88, 44);
  lv_obj_add_event_cb(solid_color_button, onSolidColor, LV_EVENT_CLICKED, nullptr);
  if (wled::model().effect == 0) lv_obj_add_state(solid_color_button, LV_STATE_CHECKED);

  lv_obj_t* solid_label = lv_label_create(solid_color_button);
  lv_label_set_text(solid_label, "Solid");
  lv_obj_center(solid_label);
  

  setColorControls(current);
}

// ── Dialog helpers ────────────────────────────────────────────────────────────

void onHelpDialogDeleted(lv_event_t*) {
  help_dialog = nullptr;
  help_dialog_deleting = false;
  fx_speed_slider = nullptr;
  fx_speed_value = nullptr;
  fx_intensity_slider = nullptr;
  fx_intensity_value = nullptr;
  for (int i = 0; i < 3; ++i) {
    fx_custom_slider[i] = nullptr;
    fx_custom_value[i] = nullptr;
  }
}

void closeHelpDialogTimer(lv_timer_t*) {
  if (help_dialog) {
    lv_obj_del_async(help_dialog);
  }
}

void closeHelpDialog(lv_event_t*) {
  if (!help_dialog || help_dialog_deleting) return;
  help_dialog_deleting = true;
  lv_timer_t* timer = lv_timer_create(closeHelpDialogTimer, 30, nullptr);
  lv_timer_set_repeat_count(timer, 1);
}

// Overlay with a title header and close button; content goes below the header.
lv_obj_t* createDialogShell(const char* title_text, lv_event_cb_t on_close, lv_coord_t topInset = 0) {
  lv_obj_t* dialog = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(dialog);
  lv_obj_set_size(dialog, LV_PCT(100), kScreenHeight - topInset);
  lv_obj_align(dialog, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(dialog, lv_color_hex(kColorBg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(dialog, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* header = lv_obj_create(dialog);
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
  lv_obj_add_event_cb(close, on_close, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* close_label = lv_label_create(close);
  lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
  lv_obj_center(close_label);

  return dialog;
}

lv_obj_t* beginInfoModal(const char* title_text, bool preserveTopBar = false) {
  if (help_dialog_deleting) return nullptr;

  help_dialog_deleting = false;
  const lv_coord_t topInset = preserveTopBar ? kTopBarHeight : 0;
  help_dialog = createDialogShell(title_text, closeHelpDialog, topInset);
  lv_obj_add_event_cb(help_dialog, onHelpDialogDeleted, LV_EVENT_DELETE, nullptr);

  lv_obj_t* content = lv_obj_create(help_dialog);
  lv_obj_remove_style_all(content);
  lv_obj_set_size(content, 296, preserveTopBar ? 154 : 184);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(content, 8, LV_PART_MAIN);
  lv_obj_add_flag(content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
  return content;
}


void onTargetDialogDeleted(lv_event_t*) {
  target_dialog = nullptr;
}

void closeTargetDialog(lv_event_t*) {
  if (target_dialog) lv_obj_del_async(target_dialog);
}

void onControllerNameDialogDeleted(lv_event_t*) {
  controller_name_dialog = nullptr;
  controller_name_input = nullptr;
}

void closeControllerNameDialog(lv_event_t*) {
  if (controller_name_dialog) lv_obj_del_async(controller_name_dialog);
}

// Stores a local alias when the keyboard confirms; clearing the field restores WLED's name.
void onControllerNameInput(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_CANCEL) {
    closeControllerNameDialog(nullptr);
    return;
  }
  if (code != LV_EVENT_READY || !controller_name_input) return;
  wled::renameDevice(controller_name_index, lv_textarea_get_text(controller_name_input));
  updateTargetLabel();
  closeControllerNameDialog(nullptr);
}

// Opens the same compact on-screen keyboard pattern used for preset names.
void showControllerNameDialog(size_t index) {
  if (controller_name_dialog || index >= wled::deviceCount()) return;
  controller_name_index = index;
  const wled::DeviceInfo device = wled::deviceInfo(index);
  controller_name_dialog = createDialogShell("Rename controller", closeControllerNameDialog);
  lv_obj_add_event_cb(controller_name_dialog, onControllerNameDialogDeleted, LV_EVENT_DELETE, nullptr);

  controller_name_input = lv_textarea_create(controller_name_dialog);
  lv_obj_set_size(controller_name_input, 296, 40);
  lv_obj_align(controller_name_input, LV_ALIGN_TOP_MID, 0, 46);
  lv_textarea_set_one_line(controller_name_input, true);
  lv_textarea_set_max_length(controller_name_input, 32);
  lv_textarea_set_placeholder_text(controller_name_input, "Controller name");
  lv_textarea_set_text(controller_name_input, device.name.c_str());
  lv_obj_add_event_cb(controller_name_input, onControllerNameInput, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(controller_name_input, onControllerNameInput, LV_EVENT_CANCEL, nullptr);

  lv_obj_t* keyboard = lv_keyboard_create(controller_name_dialog);
  lv_obj_set_size(keyboard, LV_PCT(100), 146);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(keyboard, controller_name_input);
  lv_obj_add_state(controller_name_input, LV_STATE_FOCUSED);
}

void onTargetRename(lv_event_t* event) {
  const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  closeTargetDialog(nullptr);
  showControllerNameDialog(index);
}

void onTargetForget(lv_event_t* event) {
  const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (wled::forgetDevice(index)) closeTargetDialog(nullptr);
}

void onTargetSelected(lv_event_t* event) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (value == 0) wled::selectAll();
  else wled::selectDevice(value - 1);
  updateTargetLabel();
  updateConnLabel();
  closeTargetDialog(nullptr);
}

void onTargetScan(lv_event_t*) {
  wled::scanNow();
}

// Presents every WLED found on the locked channel plus a reliable unicast "All" target.
void showTargetDialog() {
  if (target_dialog || !wled::deviceCount()) return;
  target_dialog = createDialogShell("Control WLED", closeTargetDialog);
  lv_obj_add_event_cb(target_dialog, onTargetDialogDeleted, LV_EVENT_DELETE, nullptr);

  lv_obj_t* content = lv_obj_create(target_dialog);
  lv_obj_remove_style_all(content);
  lv_obj_set_size(content, 296, 184);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(content, 6, LV_PART_MAIN);
  configurePageScroll(content, true);

  lv_obj_t* scanButton = lv_btn_create(content);
  styleButton(scanButton);
  lv_obj_set_size(scanButton, LV_PCT(100), 36);
  lv_obj_add_event_cb(scanButton, onTargetScan, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* scanLabel = lv_label_create(scanButton);
  lv_label_set_text(scanLabel, LV_SYMBOL_REFRESH " Scan for linked controllers");
  lv_obj_center(scanLabel);

  if (wled::deviceCount() > 1) {
    lv_obj_t* allButton = lv_btn_create(content);
    styleButton(allButton, true);
    lv_obj_set_size(allButton, LV_PCT(100), 38);
    if (wled::targetingAll()) lv_obj_add_state(allButton, LV_STATE_CHECKED);
    lv_obj_add_event_cb(allButton, onTargetSelected, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* label = lv_label_create(allButton);
    lv_label_set_text_fmt(label, "All controllers (%u online)", unsigned(wled::activeDeviceCount()));
    lv_obj_center(label);
  }

  for (size_t i = 0; i < wled::deviceCount(); ++i) {
    const wled::DeviceInfo device = wled::deviceInfo(i);
    lv_obj_t* row = lv_obj_create(content);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 38);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* button = lv_btn_create(row);
    styleButton(button, true);
    lv_obj_set_height(button, 38);
    lv_obj_set_flex_grow(button, 1);
    if (device.channel != wled::radioChannel() || !device.online) lv_obj_add_state(button, LV_STATE_DISABLED);
    if (!wled::targetingAll() && wled::focusedDevice() == i) lv_obj_add_state(button, LV_STATE_CHECKED);
    lv_obj_add_event_cb(button, onTargetSelected, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1)));

    char text[64];
    const char* name = device.name.empty() ? "WLED" : device.name.c_str();
    snprintf(text, sizeof(text), "%s  %02X%02X", name, device.mac[4], device.mac[5]);
    lv_obj_t* label = lv_label_create(button);
    lv_obj_set_width(label, 116);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* connection = lv_label_create(button);
    lv_obj_set_width(connection, 58);
    lv_label_set_long_mode(connection, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(connection, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_text(connection, device.online ? "Online" : "Offline");
    lv_obj_set_style_text_color(connection,
                                lv_color_hex(device.online ? kColorOk : kColorDanger), LV_PART_MAIN);
    lv_obj_align(connection, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t* rename = lv_btn_create(row);
    styleButton(rename);
    lv_obj_set_size(rename, 42, 38);
    lv_obj_add_event_cb(rename, device.online ? onTargetRename : onTargetForget, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    lv_obj_t* renameLabel = lv_label_create(rename);
    lv_label_set_text(renameLabel, device.online ? LV_SYMBOL_EDIT : LV_SYMBOL_TRASH);
    lv_obj_center(renameLabel);
  }
}



// Returns the first preset slot represented by the remote, or zero when all are occupied.
uint8_t firstFreePresetSlot() {
  const std::vector<wled::PresetInfo>& presets = wled::model().presets;
  for (uint8_t id = 1; id <= kExtendedPresetCount; ++id) {
    const bool used = std::any_of(presets.begin(), presets.end(), [id](const wled::PresetInfo& preset) {
      return preset.id == id;
    });
    if (!used) return id;
  }
  return 0;
}

void onPresetNameDialogDeleted(lv_event_t*) {
  preset_name_dialog = nullptr;
  preset_name_input = nullptr;
}

void closePresetNameDialog(lv_event_t*) {
  if (preset_name_dialog) lv_obj_del_async(preset_name_dialog);
}

// Saves the current WLED state when the keyboard confirms a non-empty preset name.
void onPresetNameInput(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_CANCEL) {
    closePresetNameDialog(nullptr);
    return;
  }
  if (code != LV_EVENT_READY || !preset_name_input) return;

  const char* name = lv_textarea_get_text(preset_name_input);
  const uint8_t id = firstFreePresetSlot();
  if (!id || !name || !name[0]) return;
  wled::savePreset(id, name);
  closePresetNameDialog(nullptr);
}

// Opens a compact text-entry dialog; the keyboard's checkmark saves the current state.
void openPresetNameDialog(lv_event_t*) {
  if (preset_name_dialog || !firstFreePresetSlot()) return;

  preset_name_dialog = createDialogShell("Add preset", closePresetNameDialog);
  lv_obj_add_event_cb(preset_name_dialog, onPresetNameDialogDeleted, LV_EVENT_DELETE, nullptr);

  preset_name_input = lv_textarea_create(preset_name_dialog);
  lv_obj_set_size(preset_name_input, 296, 40);
  lv_obj_align(preset_name_input, LV_ALIGN_TOP_MID, 0, 46);
  lv_textarea_set_one_line(preset_name_input, true);
  lv_textarea_set_max_length(preset_name_input, 32);
  lv_textarea_set_placeholder_text(preset_name_input, "Preset name");
  lv_obj_add_event_cb(preset_name_input, onPresetNameInput, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(preset_name_input, onPresetNameInput, LV_EVENT_CANCEL, nullptr);

  lv_obj_t* keyboard = lv_keyboard_create(preset_name_dialog);
  lv_obj_set_size(keyboard, LV_PCT(100), 146);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(keyboard, preset_name_input);
  lv_obj_add_state(preset_name_input, LV_STATE_FOCUSED);
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
  if (!content) return;

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
  static uint32_t last_send_ms = 0;
  const int value = lv_slider_get_value(lv_event_get_target(event));
  if (fx_speed_value) lv_label_set_text_fmt(fx_speed_value, "%d", value);
  if (shouldSendSliderValue(lv_event_get_code(event), last_send_ms)) {
    wled::setEffectParams(value, -1);
  }
}

void onFxIntensity(lv_event_t* event) {
  static uint32_t last_send_ms = 0;
  const int value = lv_slider_get_value(lv_event_get_target(event));
  if (fx_intensity_value) lv_label_set_text_fmt(fx_intensity_value, "%d", value);
  if (shouldSendSliderValue(lv_event_get_code(event), last_send_ms)) {
    wled::setEffectParams(-1, value);
  }
}

void onFxCustom(lv_event_t* event) {
  static uint32_t last_send_ms = 0;
  const uintptr_t idx = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));  // 1-3
  if (idx < 1 || idx > 3) return;
  const int value = lv_slider_get_value(lv_event_get_target(event));
  if (fx_custom_value[idx - 1]) lv_label_set_text_fmt(fx_custom_value[idx - 1], "%d", value);
  if (shouldSendSliderValue(lv_event_get_code(event), last_send_ms)) {
    wled::setCustomParam(static_cast<uint8_t>(idx), static_cast<uint8_t>(value));
  }
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

// Preview colours come from generated/wled_catalog.h (sampled from the actual WLED
// palette data); ids 0-5 derive from the current segment colours at runtime.
uint32_t palettePreviewColor(uint16_t palette, uint8_t pos) {
  const uint32_t c1 = wled::model().color;
  const uint32_t c2 = 0x0066FF;  // stand-ins: the remote only tracks the primary colour
  const uint32_t c3 = 0xFFFFFF;

  switch (palette) {
    case 1: return hsvToRgb(pos * 360 / 255, 210, 255);  // * Random Cycle
    case 2: return c1;
    case 3: { const uint32_t stops[] = {c1, c1, c2, c2}; return gradientColor(stops, 4, pos); }
    case 4: { const uint32_t stops[] = {c3, c2, c1}; return gradientColor(stops, 3, pos); }
    case 5: { const uint32_t stops[] = {c1, c1, c1, c2, c2, c2, c3, c3, c3, c1}; return gradientColor(stops, 10, pos); }
    default: break;
  }

  size_t idx = palette < kWledPaletteDynamicCount ? 0  // Default renders like Party
                                                  : palette - kWledPaletteDynamicCount;
  if (idx >= kWledPaletteStopsCount) return hsvToRgb((uint16_t(pos) + palette * 23) % 360, 210, 240);

  uint32_t stops[8];
  for (int i = 0; i < 8; ++i) {
    stops[i] = makeRgb(kWledPaletteStops[idx][i][0], kWledPaletteStops[idx][i][1],
                       kWledPaletteStops[idx][i][2]);
  }
  return gradientColor(stops, 8, pos);
}

// "Default" stays pinned first; everything else alphabetical by baked name.
std::vector<size_t> paletteDisplayOrder() {
  std::vector<size_t> order;
  order.reserve(kWledPaletteCount);
  for (size_t i = 0; i < kWledPaletteCount; ++i) {
    order.push_back(i);
  }
  std::sort(order.begin() + 1, order.end(), [](size_t left, size_t right) {
    return strcasecmp(kWledPaletteNames[left], kWledPaletteNames[right]) < 0;
  });
  return order;
}

const char* paletteNameOrFallback(size_t id) {
  if (id < kWledPaletteCount && kWledPaletteNames[id][0]) {
    return kWledPaletteNames[id];
  }
  return "Palette";
}

void drawPalettePreviewArea(lv_draw_ctx_t* draw_ctx, const lv_area_t& coords, uint16_t palette);

void drawPalettePreviewArea(lv_draw_ctx_t* draw_ctx, const lv_area_t& coords, uint16_t palette) {
  const lv_coord_t width = lv_area_get_width(&coords);
  if (width <= 0) return;

  const lv_coord_t segments = std::min<lv_coord_t>(width, 36);
  lv_draw_rect_dsc_t rect;
  lv_draw_rect_dsc_init(&rect);
  rect.bg_opa = LV_OPA_COVER;
  rect.border_width = 0;
  rect.radius = 0;

  for (lv_coord_t i = 0; i < segments; ++i) {
    const uint8_t pos = segments == 1 ? 0 : uint8_t((uint32_t(i) * 255U) / uint32_t(segments - 1));
    rect.bg_color = lv_color_hex(palettePreviewColor(palette, pos));
    lv_area_t col = coords;
    col.x1 = coords.x1 + (int32_t(i) * width) / segments;
    col.x2 = coords.x1 + (int32_t(i + 1) * width) / segments - 1;
    if (i == segments - 1) col.x2 = coords.x2;
    lv_draw_rect(draw_ctx, &rect, &col);
  }
}

void onPaletteTableClicked(lv_event_t* event) {
  lv_obj_t* table = lv_event_get_target(event);
  uint16_t row = LV_TABLE_CELL_NONE;
  uint16_t col = LV_TABLE_CELL_NONE;
  lv_table_get_selected_cell(table, &row, &col);
  if (row == LV_TABLE_CELL_NONE || row >= palette_table_order.size()) return;

  const size_t id = palette_table_order[row];
  if (id > 255) return;
  palette_chooser_selected = static_cast<int>(id);
  wled::setPalette(static_cast<uint8_t>(id));
  if (palette_list_table) lv_obj_invalidate(palette_list_table);
}

void onPaletteTableDrawPart(lv_event_t* event) {
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(event);
  if (!dsc || !lv_obj_draw_part_check_type(dsc, &lv_table_class, LV_TABLE_DRAW_PART_CELL)) {
    return;
  }
  const uint16_t row = dsc->id;
  if (row >= palette_table_order.size()) {
    return;
  }

  const bool selected = static_cast<int>(palette_table_order[row]) == palette_chooser_selected;
  if (dsc->rect_dsc) {
    dsc->rect_dsc->radius = 8;
    dsc->rect_dsc->border_width = 0;
    if (selected) {
      dsc->rect_dsc->bg_color = lv_color_hex(kColorSelected);
    } else if (row % 2) {
      dsc->rect_dsc->bg_color = lv_color_hex(kColorSurfaceRaised);
    } else {
      dsc->rect_dsc->bg_color = lv_color_hex(kColorSurface);
    }
  }
  if (dsc->label_dsc) {
    dsc->label_dsc->align = LV_TEXT_ALIGN_CENTER;
    dsc->label_dsc->color = lv_color_hex(kColorText);
  }
}

void onPaletteTableDrawEnd(lv_event_t* event) {
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(event);
  if (!dsc || !lv_obj_draw_part_check_type(dsc, &lv_table_class, LV_TABLE_DRAW_PART_CELL)) {
    return;
  }
  const uint16_t row = dsc->id;
  if (row >= palette_table_order.size() || !dsc->draw_area || !dsc->draw_ctx) {
    return;
  }

  lv_area_t strip = *dsc->draw_area;
  strip.x1 += 14;
  strip.x2 -= 14;
  strip.y2 -= 8;
  strip.y1 = strip.y2 - 7;
  if (strip.x2 <= strip.x1 || strip.y2 <= strip.y1) return;
  drawPalettePreviewArea(dsc->draw_ctx, strip, static_cast<uint16_t>(palette_table_order[row]));
}

// Builds the Colors-tab palette list. The parent page scrolls around the static table.
lv_obj_t* buildPaletteTable(lv_obj_t* parent, lv_coord_t col_width) {
  if (palette_table_order.empty()) palette_table_order = paletteDisplayOrder();

  lv_obj_t* table = lv_table_create(parent);
  lv_table_set_col_cnt(table, 1);
  lv_table_set_row_cnt(table, palette_table_order.size());
  lv_table_set_col_width(table, 0, col_width);
  lv_obj_set_size(table, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_scrollbar_mode(table, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(table, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                           LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_add_flag(table, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(table, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(table, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(table, lv_color_hex(kColorAccent), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(table, LV_OPA_50, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(table, 4, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(table, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_left(table, 10, LV_PART_ITEMS);
  lv_obj_set_style_pad_right(table, 10, LV_PART_ITEMS);
  lv_obj_set_style_pad_top(table, kPaletteRowPadTop, LV_PART_ITEMS);
  lv_obj_set_style_pad_bottom(table, kPaletteRowPadBottom, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(table, lv_color_hex(kColorText), LV_PART_ITEMS);
  lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);

  for (size_t i = 0; i < palette_table_order.size(); ++i) {
    lv_table_set_cell_value(table, i, 0, paletteNameOrFallback(palette_table_order[i]));
    lv_table_add_cell_ctrl(table, i, 0, LV_TABLE_CELL_CTRL_TEXT_CROP);
  }
  lv_obj_add_event_cb(table, onPaletteTableClicked, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(table, onPaletteTableDrawPart, LV_EVENT_DRAW_PART_BEGIN, nullptr);
  lv_obj_add_event_cb(table, onPaletteTableDrawEnd, LV_EVENT_DRAW_PART_END, nullptr);
  return table;
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

// Per-effect controls driven by the baked catalog: only the sliders this effect uses.
void openFxControls() {
  if (help_dialog) {
    return;
  }

  const wled::Model& m = wled::model();
  const char* title = "Effect";
  if (selected_effect_id < kWledFxCount && kWledFx[selected_effect_id].name) {
    title = kWledFx[selected_effect_id].name;
  }
  std::string labels[8];
  fxSliderLabels(selected_effect_id, labels);

  
  // Leave the application top bar exposed so live Peek remains visible while editing.
  lv_obj_t* content = beginInfoModal(title, true);
  
  if (!content) return;
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(content, 10, LV_PART_MAIN);
  lv_obj_clear_flag(content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_set_scroll_dir(content, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM);

  if (!labels[0].empty()) {
    const char* name = labels[0] == "Effect speed" ? "Speed" : labels[0].c_str();
    fx_speed_slider = createLabeledSlider(content, name, 0, 255, m.speed, onFxSpeed, &fx_speed_value);
  }
  if (!labels[1].empty()) {
    const char* name = labels[1] == "Effect intensity" ? "Intensity" : labels[1].c_str();
    fx_intensity_slider =
        createLabeledSlider(content, name, 0, 255, m.intensity, onFxIntensity, &fx_intensity_value);
  }
  const uint8_t custom_values[3] = {m.custom1, m.custom2, m.custom3};
  for (uintptr_t i = 0; i < 3; ++i) {
    if (labels[2 + i].empty()) continue;
    fx_custom_slider[i] = createLabeledSlider(content, labels[2 + i].c_str(), 0, 255,
                                              custom_values[i], onFxCustom, &fx_custom_value[i],
                                              reinterpret_cast<void*>(i + 1));
  }

}

void onEffectTableClicked(lv_event_t* event) {
  lv_obj_t* table = lv_event_get_target(event);
  uint16_t row = LV_TABLE_CELL_NONE;
  uint16_t col = LV_TABLE_CELL_NONE;
  lv_table_get_selected_cell(table, &row, &col);
  if (row == LV_TABLE_CELL_NONE || col == LV_TABLE_CELL_NONE || row >= fx_table_order.size()) {
    return;
  }

  const uint16_t fx_id = fx_table_order[row];
  if (col == 1 && fx_id == selected_effect_id) {
    scheduleFxControlsOpen();
  } else {
    activateEffectId(static_cast<uint8_t>(fx_id));
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
  if (row >= fx_table_order.size()) {
    return;
  }

  const bool selected = fx_table_order[row] == selected_effect_id;

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

void openTargetDialog(lv_event_t*) {
  showTargetDialog();
}

void refreshTargetDialog() {
  if (!target_dialog || controller_name_dialog) return;
  lv_obj_del(target_dialog);
  target_dialog = nullptr;
  showTargetDialog();
}

void createLiveTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, false);

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), kTabCardHeight);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);

  power_button = lv_btn_create(panel);
  styleButton(power_button, true);
  lv_obj_set_size(power_button, LV_PCT(100), 58);
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

  brightness_slider =
      createLabeledSlider(panel, "Brightness", 1, 255, state.brightness, onBrightness, &brightness_label);

  now_playing_label = lv_label_create(panel);
  lv_obj_set_width(now_playing_label, LV_PCT(100));
  lv_label_set_long_mode(now_playing_label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(now_playing_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_add_style(now_playing_label, &style_label_muted, LV_PART_MAIN);
  lv_obj_set_style_text_color(now_playing_label, lv_color_hex(kColorTextMuted), LV_PART_MAIN);
  lv_label_set_text(now_playing_label, "");
}

void updateColorControlsFromModel() {
  if (!color_wheel) return;
  if (color_wheel && lv_obj_has_state(color_wheel, LV_STATE_PRESSED)) return;
  setColorControls(wled::model().color);
}

// Reflects state pushes onto the open dialogs and the now-playing line, so
// changes made in the WLED web UI appear immediately wherever they are visible.
void updateStatusFromModel() {
  const wled::Model& m = wled::model();

  if (solid_color_button) {
    if (m.effect == 0) {
      lv_obj_add_state(solid_color_button, LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(solid_color_button, LV_STATE_CHECKED);
    }
  }

  if (now_playing_label) {
    const char* effect = m.effect >= 0 && static_cast<size_t>(m.effect) < kWledFxCount &&
                                 kWledFx[m.effect].name
                             ? kWledFx[m.effect].name
                             : "";
    const char* palette = m.palette >= 0 && static_cast<size_t>(m.palette) < kWledPaletteCount
                              ? kWledPaletteNames[m.palette]
                              : "";
    if (effect[0] && palette[0]) {
      lv_label_set_text_fmt(now_playing_label, "%s  •  %s", effect, palette);
    } else if (effect[0]) {
      lv_label_set_text(now_playing_label, effect);
    } else {
      lv_label_set_text(now_playing_label, m.online ? "" : "Searching for WLED...");
    }
  }

  if (fx_speed_slider && !lv_obj_has_state(fx_speed_slider, LV_STATE_PRESSED)) {
    lv_slider_set_value(fx_speed_slider, m.speed, LV_ANIM_OFF);
    if (fx_speed_value) lv_label_set_text_fmt(fx_speed_value, "%u", m.speed);
  }
  if (fx_intensity_slider && !lv_obj_has_state(fx_intensity_slider, LV_STATE_PRESSED)) {
    lv_slider_set_value(fx_intensity_slider, m.intensity, LV_ANIM_OFF);
    if (fx_intensity_value) lv_label_set_text_fmt(fx_intensity_value, "%u", m.intensity);
  }
  const uint8_t custom_values[3] = {m.custom1, m.custom2, m.custom3};
  for (int i = 0; i < 3; ++i) {
    if (!fx_custom_slider[i] || lv_obj_has_state(fx_custom_slider[i], LV_STATE_PRESSED)) continue;
    lv_slider_set_value(fx_custom_slider[i], custom_values[i], LV_ANIM_OFF);
    if (fx_custom_value[i]) lv_label_set_text_fmt(fx_custom_value[i], "%u", custom_values[i]);
  }
  if (palette_chooser_selected != m.palette) {
    palette_chooser_selected = m.palette;
    if (palette_list_table) lv_obj_invalidate(palette_list_table);
  }
}

void createPresetsTab(lv_obj_t* tab) {
  for (lv_obj_t*& preset_button : preset_buttons) {
    preset_button = nullptr;
  }

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, true);

  const std::vector<wled::PresetInfo>& presets = wled::model().presets;

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);

  
  lv_obj_t* addPreset = lv_btn_create(panel);
  styleButton(addPreset);
  lv_obj_set_size(addPreset, LV_PCT(100), 38);
  lv_obj_add_event_cb(addPreset, openPresetNameDialog, LV_EVENT_CLICKED, nullptr);
  if (!firstFreePresetSlot()) lv_obj_add_state(addPreset, LV_STATE_DISABLED);

  lv_obj_t* addPresetLabel = lv_label_create(addPreset);
  lv_label_set_text(addPresetLabel, LV_SYMBOL_PLUS "  Add preset");
  lv_obj_center(addPresetLabel);
  

  if (presets.empty()) {
    lv_obj_t* hint = lv_label_create(panel);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "Waiting for WLED. Add this remote's MAC in WLED, then tap Ping in Settings.");
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
}

void createColorsTab(lv_obj_t* tab) {
  lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
  configurePageScroll(tab, false);

  
  // Palettes is first so it is the default view; each child owns its vertical scrolling.
  lv_obj_t* color_tabs = lv_tabview_create(tab, LV_DIR_TOP, kTabButtonHeight);
  lv_obj_set_size(color_tabs, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(color_tabs, lv_color_hex(kColorBg), LV_PART_MAIN);
  lv_obj_set_style_border_width(color_tabs, 0, LV_PART_MAIN);

  lv_obj_t* tab_buttons = lv_tabview_get_tab_btns(color_tabs);
  lv_obj_set_style_bg_color(tab_buttons, lv_color_hex(kColorSurface), LV_PART_MAIN);
  lv_obj_set_style_border_width(tab_buttons, 0, LV_PART_MAIN);
  lv_obj_set_style_text_color(tab_buttons, lv_color_hex(kColorTextMuted), LV_PART_MAIN);
  lv_obj_set_style_text_color(tab_buttons, lv_color_hex(kColorAccent), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(tab_buttons, lv_color_hex(kColorAccent), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(tab_buttons, 2, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_side(tab_buttons, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS | LV_STATE_CHECKED);

  lv_obj_t* palettes_page = lv_tabview_add_tab(color_tabs, "Palettes");
  lv_obj_set_style_pad_all(palettes_page, 8, LV_PART_MAIN);
  configurePageScroll(palettes_page, true);

  lv_obj_t* palette_panel = createPanel(palettes_page);
  lv_obj_set_size(palette_panel, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(palette_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(palette_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_all(palette_panel, 12, LV_PART_MAIN);
  palette_table_order = paletteDisplayOrder();
  palette_chooser_selected = wled::model().palette;
  palette_list_table = buildPaletteTable(palette_panel, 268);

  lv_obj_t* wheel_page = lv_tabview_add_tab(color_tabs, "Color Wheel");
  lv_obj_set_style_pad_all(wheel_page, 2, LV_PART_MAIN);
  configurePageScroll(wheel_page, false);

  lv_obj_t* wheel_panel = createPanel(wheel_page);
  lv_obj_set_size(wheel_panel, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(wheel_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(wheel_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(wheel_panel, 2, LV_PART_MAIN);
  createColorWheelEditor(wheel_panel);
  
}

void rebuildPresetTab() {
  if (!presets_tab) {
    return;
  }
  lv_obj_clean(presets_tab);
  createPresetsTab(presets_tab);
}

void rebuildFxTabAsync(void*) {
  fx_rebuild_pending = false;
  rebuildFxTab();
}

void scheduleFxTabRebuild() {
  if (fx_rebuild_pending) return;
  fx_rebuild_pending = true;
  lv_async_call(rebuildFxTabAsync, nullptr);
}

void onFxFilterChip(lv_event_t* event) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  const FxFilter filter = static_cast<FxFilter>(value);
  if (filter == fx_filter) {
    // keep the active chip checked even when re-tapped
    lv_obj_add_state(lv_event_get_target(event), LV_STATE_CHECKED);
    return;
  }
  fx_filter = filter;
  scheduleFxTabRebuild();  // rebuilding now would delete the chip mid-event
}

void createFxTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  configurePageScroll(tab, false);

  buildFxTableOrder();

  lv_obj_t* fx_panel = createPanel(tab);
  lv_obj_set_size(fx_panel, LV_PCT(100), kTabCardHeight);
  lv_obj_set_flex_flow(fx_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(fx_panel, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(fx_panel, 8, LV_PART_MAIN);

  static const char* kChipNames[] = {"All", "1D", "2D", "Sound"};
  lv_obj_t* chips = lv_obj_create(fx_panel);
  lv_obj_remove_style_all(chips);
  lv_obj_set_size(chips, LV_PCT(100), 30);
  lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
  for (uintptr_t i = 0; i < 4; ++i) {
    lv_obj_t* chip = lv_btn_create(chips);
    styleButton(chip, true);
    lv_obj_set_size(chip, 66, 28);
    if (static_cast<FxFilter>(i) == fx_filter) lv_obj_add_state(chip, LV_STATE_CHECKED);
    lv_obj_add_event_cb(chip, onFxFilterChip, LV_EVENT_CLICKED, reinterpret_cast<void*>(i));
    lv_obj_t* label = lv_label_create(chip);
    lv_label_set_text(label, kChipNames[i]);
    lv_obj_center(label);
  }

  lv_obj_t* table = lv_table_create(fx_panel);
  fx_table = table;
  lv_obj_set_width(table, LV_PCT(100));
  lv_obj_set_flex_grow(table, 1);
  lv_table_set_col_cnt(table, 2);
  lv_table_set_row_cnt(table, fx_table_order.size());
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

  for (size_t i = 0; i < fx_table_order.size(); ++i) {
    lv_table_set_cell_value(table, i, 0, kWledFx[fx_table_order[i]].name);
    lv_table_add_cell_ctrl(table, i, 0, LV_TABLE_CELL_CTRL_TEXT_CROP);
    lv_table_set_cell_value(table, i, 1, LV_SYMBOL_SETTINGS);
  }
  lv_obj_add_event_cb(table, onEffectTableClicked, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(table, onEffectTableDrawPart, LV_EVENT_DRAW_PART_BEGIN, nullptr);

  revealSelectedEffect(false, false);
}

void rebuildFxTab() {
  if (!fx_tab) {
    return;
  }
  fx_table = nullptr;
  lv_obj_clean(fx_tab);
  createFxTab(fx_tab);
}


// Keeps the active effect visible after remote state changes and tab navigation.
void revealSelectedEffect(bool animated, bool showAllIfFiltered) {
  if (!fx_table) return;

  auto selected = std::find(fx_table_order.begin(), fx_table_order.end(), selected_effect_id);
  if (selected == fx_table_order.end()) {
    if (!showAllIfFiltered || fx_filter == FxFilter::kAll) return;
    fx_filter = FxFilter::kAll;
    rebuildFxTab();
    return;
  }

  lv_obj_update_layout(fx_table);
  const lv_coord_t rowHeight = 16 + 16;  // montserrat_14 line + item pads
  const size_t row = static_cast<size_t>(selected - fx_table_order.begin());
  lv_coord_t target = static_cast<lv_coord_t>(row) * rowHeight - 48;
  if (target < 0) target = 0;
  lv_obj_scroll_to_y(fx_table, target, animated ? LV_ANIM_ON : LV_ANIM_OFF);
}


void createConnectionPanel(lv_obj_t* tab) {
  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
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
  configurePageScroll(tab, true);

  createConnectionPanel(tab);

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 8, LV_PART_MAIN);

  createSettingsRow(panel, "Control Target", openTargetDialog, false, &target_label);
  updateTargetLabel();

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

#if WLED_TOUCH_SIMULATOR
void simulatorOpenFxControls() {
  openFxControls();
}

#endif

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
