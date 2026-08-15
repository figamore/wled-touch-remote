#include "tabs.h"
#include "ui.h"
#include "../BatteryMonitor.h"
#include "../display.h"
#include "../settings.h"
#include "../update_helper.h"
#include "../wifi_link.h"
#include "../wled_api.h"
#include <Arduino.h>
#include <WiFi.h>
#if !WLED_TOUCH_SIMULATOR
#include <esp_heap_caps.h>
#endif
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


// The wheel is bounded by whichever page dimension runs out first: the width
// on the large portrait panel, the height under the nested Colors tabs on a
// large landscape one.
constexpr lv_coord_t kColorWheelSize = !kLargeScreen ? 138
                                       : kPortraitScreen
                                           ? kScreenWidth - 40
                                           : kTabCardHeight - kTabButtonHeight - 24;
constexpr lv_coord_t kColorSelectorSize = uiScaled(18, 28);
constexpr lv_coord_t kPaletteRowPadTop = uiScaled(7, 10);
constexpr lv_coord_t kPaletteRowPadBottom = uiScaled(21, 30);
constexpr lv_coord_t kPaletteStripHeight = uiScaled(8, 12);
constexpr uint32_t kSliderSendIntervalMs = 150;

lv_obj_t* color_wheel = nullptr;
lv_obj_t* color_wheel_panel = nullptr;
lv_obj_t* color_selector = nullptr;
lv_obj_t* solid_color_button = nullptr;
bool color_syncing = false;
bool color_wheel_editor_created = false;
lv_color_t* color_wheel_pixels = nullptr;
lv_img_dsc_t color_wheel_image = {
    {LV_IMG_CF_TRUE_COLOR, 0, 0, kColorWheelSize, kColorWheelSize},
    kColorWheelSize * kColorWheelSize * sizeof(lv_color_t),
#if WLED_BOARD == WLED_BOARD_CYD
    reinterpret_cast<const uint8_t*>(kCydColorWheelPixels),
#else
    nullptr,
#endif
};
bool fx_controls_pending = false;
bool fx_rebuild_pending = false;
std::vector<size_t> palette_table_order;
int palette_chooser_selected = -1;
bool help_dialog_deleting = false;
lv_obj_t* preset_name_dialog = nullptr;
lv_obj_t* preset_name_input = nullptr;
uint8_t preset_name_id = 0;
lv_obj_t* preset_table = nullptr;
lv_obj_t* preset_empty_hint = nullptr;
lv_obj_t* target_dialog = nullptr;
bool target_dialog_refresh_pending = false;
lv_obj_t* controller_name_dialog = nullptr;
lv_obj_t* controller_name_input = nullptr;
size_t controller_name_index = 0;
lv_obj_t* palette_list_table = nullptr;  // palette table in Power (JC) or Colors (CYD)
lv_obj_t* fx_speed_slider = nullptr;
lv_obj_t* fx_speed_value = nullptr;
lv_obj_t* fx_intensity_slider = nullptr;
lv_obj_t* fx_intensity_value = nullptr;
lv_obj_t* fx_custom_slider[3] = {nullptr, nullptr, nullptr};
lv_obj_t* fx_custom_value[3] = {nullptr, nullptr, nullptr};
lv_obj_t* now_playing_label = nullptr;
lv_obj_t* fx_table = nullptr;
lv_obj_t* add_preset_button = nullptr;

void showTargetDialog();

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

bool wledControlsAvailable() {
  return wled::connectionStatus() == wled::ConnectionStatus::kConnected;
}

void setControlEnabled(lv_obj_t* control, bool enabled) {
  if (!control) return;
  if (enabled == !lv_obj_has_state(control, LV_STATE_DISABLED)) return;
  if (enabled) lv_obj_clear_state(control, LV_STATE_DISABLED);
  else lv_obj_add_state(control, LV_STATE_DISABLED);
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

// The P4's 480 px-wide panel leaves the Wi-Fi chooser's labels looking small
// beside its generously sized touch rows.  Keep the compact screen unchanged,
// while using the already-available header face for this one dense dialog.
void styleWifiMenuLabel(lv_obj_t* label) {
  if (kLargeScreen) lv_obj_set_style_text_font(label, UI_FONT_HEADER, LV_PART_MAIN);
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

void drawBrightnessIcon(lv_event_t* event) {
  lv_draw_ctx_t* draw_ctx = lv_event_get_draw_ctx(event);
  if (!draw_ctx) return;

  lv_area_t coords;
  lv_obj_get_coords(lv_event_get_target(event), &coords);
  const lv_coord_t center_x = (coords.x1 + coords.x2) / 2;
  const lv_coord_t center_y = (coords.y1 + coords.y2) / 2;
  const lv_coord_t extent = std::min(lv_area_get_width(&coords), lv_area_get_height(&coords));
  const lv_coord_t radius = std::max<lv_coord_t>(4, (extent - 8) / 3);
  const lv_coord_t ray_start = radius + 2;
  const lv_coord_t ray_end = extent / 2 - 1;

  lv_draw_line_dsc_t ray;
  lv_draw_line_dsc_init(&ray);
  ray.color = lv_color_hex(kColorTextMuted);
  ray.width = uiScaled(1, 2);
  ray.opa = LV_OPA_COVER;
  ray.round_start = 1;
  ray.round_end = 1;

  const auto draw_ray = [&](int x1, int y1, int x2, int y2) {
    const lv_point_t start = {static_cast<lv_coord_t>(x1), static_cast<lv_coord_t>(y1)};
    const lv_point_t end = {static_cast<lv_coord_t>(x2), static_cast<lv_coord_t>(y2)};
    lv_draw_line(draw_ctx, &ray, &start, &end);
  };

  const lv_coord_t diagonal_start = std::max<lv_coord_t>(2, ray_start * 7 / 10);
  const lv_coord_t diagonal_end = std::max<lv_coord_t>(2, ray_end * 7 / 10);
  draw_ray(center_x, center_y - ray_start, center_x, center_y - ray_end);
  draw_ray(center_x + ray_start, center_y, center_x + ray_end, center_y);
  draw_ray(center_x, center_y + ray_start, center_x, center_y + ray_end);
  draw_ray(center_x - ray_start, center_y, center_x - ray_end, center_y);
  draw_ray(center_x + diagonal_start, center_y - diagonal_start,
           center_x + diagonal_end, center_y - diagonal_end);
  draw_ray(center_x + diagonal_start, center_y + diagonal_start,
           center_x + diagonal_end, center_y + diagonal_end);
  draw_ray(center_x - diagonal_start, center_y + diagonal_start,
           center_x - diagonal_end, center_y + diagonal_end);
  draw_ray(center_x - diagonal_start, center_y - diagonal_start,
           center_x - diagonal_end, center_y - diagonal_end);

  lv_area_t disc = {static_cast<lv_coord_t>(center_x - radius),
                    static_cast<lv_coord_t>(center_y - radius),
                    static_cast<lv_coord_t>(center_x + radius),
                    static_cast<lv_coord_t>(center_y + radius)};
  lv_draw_rect_dsc_t fill;
  lv_draw_rect_dsc_init(&fill);
  fill.bg_color = lv_color_hex(kColorTextMuted);
  fill.bg_opa = LV_OPA_COVER;
  fill.border_width = 0;
  fill.radius = LV_RADIUS_CIRCLE;
  lv_draw_rect(draw_ctx, &fill, &disc);

  // Cut the left half back to the panel colour, then redraw the outline for a
  // crisp half-filled sun that stays legible at the compact display size.
  lv_area_t left_half = disc;
  left_half.x2 = center_x - 1;
  lv_draw_rect_dsc_t cutout;
  lv_draw_rect_dsc_init(&cutout);
  cutout.bg_color = lv_color_hex(kColorSurface);
  cutout.bg_opa = LV_OPA_COVER;
  cutout.border_width = 0;
  cutout.radius = 0;
  lv_draw_rect(draw_ctx, &cutout, &left_half);

  lv_draw_rect_dsc_t outline;
  lv_draw_rect_dsc_init(&outline);
  outline.bg_opa = LV_OPA_TRANSP;
  outline.border_color = lv_color_hex(kColorTextMuted);
  outline.border_opa = LV_OPA_COVER;
  outline.border_width = uiScaled(1, 2);
  outline.radius = LV_RADIUS_CIRCLE;
  lv_draw_rect(draw_ctx, &outline, &disc);
}

lv_obj_t* createBrightnessIcon(lv_obj_t* parent) {
  lv_obj_t* icon = lv_obj_create(parent);
  lv_obj_remove_style_all(icon);
  lv_obj_set_size(icon, uiScaled(24, 32), uiScaled(24, 32));
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(icon, drawBrightnessIcon, LV_EVENT_DRAW_MAIN, nullptr);
  return icon;
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
  lv_obj_set_height(row, uiScaled(38, 56));
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, uiScaled(8, 12), LV_PART_MAIN);
  lv_obj_set_style_pad_left(row, name ? 0 : 10, LV_PART_MAIN);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);

  if (name) {
    lv_obj_t* name_label = lv_label_create(row);
    lv_obj_set_width(name_label, uiScaled(82, 140));
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_add_style(name_label, &style_label_muted, LV_PART_MAIN);
    lv_label_set_text(name_label, name);
  }

  lv_obj_t* slider = lv_slider_create(row);
  lv_slider_set_range(slider, min, max);
  lv_slider_set_value(slider, value, LV_ANIM_OFF);
  lv_obj_set_size(slider, name ? 140 : 190, uiScaled(12, 18));
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
  lv_obj_set_width(*value_label, uiScaled(34, 56));
  lv_obj_clear_flag(*value_label, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_label_set_text_fmt(*value_label, "%d", value);
  // Slider rows are also used inside top-level dialogs, which do not inherit
  // the screen text style. Keep the numeric value readable on dark surfaces.
  lv_obj_set_style_text_color(*value_label, lv_color_hex(kColorText), LV_PART_MAIN);
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
  // The CYD's 138px wheel is generated at build time and memory-mapped from
  // flash. Large P4 builds retain their full-resolution PSRAM-backed image.
  if (color_wheel_image.data) return;

  const size_t bytes = kColorWheelSize * kColorWheelSize * sizeof(lv_color_t);
#if !WLED_TOUCH_SIMULATOR && WLED_BOARD_HAS_PSRAM
  color_wheel_pixels = static_cast<lv_color_t*>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
  color_wheel_pixels = static_cast<lv_color_t*>(malloc(bytes));
#endif
  if (!color_wheel_pixels) return;
  color_wheel_image.data = reinterpret_cast<const uint8_t*>(color_wheel_pixels);

  static const uint8_t kBayer[4][4] = {
      {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  const float center = (kColorWheelSize - 1) / 2.0f;
  const float radius = center - 1.5f;
  const float bg_r = float((kColorSurface >> 16) & 0xFF);
  const float bg_g = float((kColorSurface >> 8) & 0xFF);
  const float bg_b = float(kColorSurface & 0xFF);

  for (lv_coord_t y = 0; y < kColorWheelSize; ++y) {
    for (lv_coord_t x = 0; x < kColorWheelSize; ++x) {
      float r, g, b;
      wheelSampleRgb(x + 0.5f - center, y + 0.5f - center, radius, bg_r, bg_g, bg_b, r, g, b);
      const float dither = kBayer[y & 3][x & 3] / 16.0f - 0.46875f;
      const int red = std::min(255, std::max(0, int(r + dither * 8.0f + 0.5f)));
      const int green = std::min(255, std::max(0, int(g + dither * 4.0f + 0.5f)));
      const int blue = std::min(255, std::max(0, int(b + dither * 8.0f + 0.5f)));
      color_wheel_pixels[y * kColorWheelSize + x] = lv_color_make(red, green, blue);
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

// Once a drag starts on the disc, project points outside its edge back onto
// the rim so selection remains continuous under a wandering finger.
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
  if (!wledControlsAvailable()) return;
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
  if (!wledControlsAvailable()) return;
  activateEffectId(0);
  lv_obj_add_state(lv_event_get_target(event), LV_STATE_CHECKED);
}


void createColorWheelEditor(lv_obj_t* parent) {
  const uint32_t current = wled::model().color;
  generateColorWheelImage();

  lv_obj_t* editor = lv_obj_create(parent);
  lv_obj_remove_style_all(editor);
  lv_obj_set_size(editor, LV_PCT(100), LV_PCT(100));
  // The near-full-width wheel leaves no room beside it on the large portrait
  // panel, so the Solid button moves underneath.
  lv_obj_set_flex_flow(editor, kLargeScreen && kPortraitScreen ? LV_FLEX_FLOW_COLUMN
                                                               : LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(editor, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(editor, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_row(editor, 18, LV_PART_MAIN);
  lv_obj_clear_flag(editor, LV_OBJ_FLAG_SCROLLABLE);

  if (color_wheel_image.data) {
    color_wheel = lv_obj_create(editor);
    lv_obj_remove_style_all(color_wheel);
    lv_obj_set_size(color_wheel, kColorWheelSize, kColorWheelSize);
    lv_obj_add_flag(color_wheel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
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
    lv_obj_set_style_bg_opa(color_selector, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(color_selector, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(color_selector, 3, LV_PART_MAIN);
    lv_obj_set_style_outline_color(color_selector, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_outline_width(color_selector, 2, LV_PART_MAIN);
    lv_obj_set_style_outline_opa(color_selector, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(color_selector, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  } else {
    lv_obj_t* error = lv_label_create(editor);
    lv_label_set_text(error, "Color wheel unavailable: memory allocation failed.");
    lv_obj_set_width(error, LV_PCT(90));
    lv_label_set_long_mode(error, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(error, &style_label_muted, LV_PART_MAIN);
  }

  solid_color_button = lv_btn_create(editor);
  styleButton(solid_color_button, true);
  lv_obj_set_size(solid_color_button, uiScaled(88, 200), uiScaled(44, 60));
  lv_obj_add_event_cb(solid_color_button, onSolidColor, LV_EVENT_CLICKED, nullptr);
  if (wled::model().effect == 0) lv_obj_add_state(solid_color_button, LV_STATE_CHECKED);

  lv_obj_t* solid_label = lv_label_create(solid_color_button);
  lv_label_set_text(solid_label, "Solid");
  lv_obj_center(solid_label);

  setColorControls(current);
  updateWledControlAvailability();
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
// The 146px keyboard suits the CYD's 240px-tall screen, but a taller panel needs
// proportionally bigger keys to stay tappable rather than the same strip of pixels.
constexpr lv_coord_t kKeyboardHeight = kLargeScreen ? kScreenHeight * 2 / 5 : 146;
constexpr lv_coord_t kDialogInputHeight = kLargeScreen ? 64 : 40;
// The P4 has enough vertical room for a more substantial modal header, which
// also gives its close control a comfortably large touch target.
constexpr lv_coord_t kDialogHeaderHeight = uiScaled(40, 68);
constexpr lv_coord_t kDialogWidth = kScreenWidth - 24;
constexpr lv_btnmatrix_ctrl_t kTextKey = LV_BTNMATRIX_CTRL_POPOVER | 1;
constexpr lv_btnmatrix_ctrl_t kWideTextKey = LV_BTNMATRIX_CTRL_POPOVER | 2;
constexpr lv_btnmatrix_ctrl_t kKeyboardSpacer = LV_BTNMATRIX_CTRL_HIDDEN | 1;

const char* kCompactKeyboardLowerMap[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "\t", "a", "s", "d", "f", "g", "h", "j", "k", "l", "\t", "\n",
    LV_SYMBOL_UP, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "123", " ", ".", LV_SYMBOL_RIGHT, "",
};

const char* kCompactKeyboardUpperMap[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "\t", "A", "S", "D", "F", "G", "H", "J", "K", "L", "\t", "\n",
    LV_SYMBOL_UP, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "123", " ", ".", LV_SYMBOL_RIGHT, "",
};

const char* kCompactKeyboardSymbolMap[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
    "#+=", ".", ",", "?", "!", "'", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", " ", ".", LV_SYMBOL_RIGHT, "",
};

const char* kCompactKeyboardMoreSymbolMap[] = {
    "[", "]", "{", "}", "#", "%", "^", "*", "+", "=", "\n",
    "_", "\\", "|", "~", "<", ">", "`", ";", ":", "\"", "\n",
    "123", ".", ",", "?", "!", "'", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", " ", ".", LV_SYMBOL_RIGHT, "",
};

const lv_btnmatrix_ctrl_t kCompactKeyboardTextCtrlMap[] = {
    kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey,
    kKeyboardSpacer, kWideTextKey, kWideTextKey, kWideTextKey, kWideTextKey, kWideTextKey,
    kWideTextKey, kWideTextKey, kWideTextKey, kWideTextKey, kKeyboardSpacer,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 3, kWideTextKey, kWideTextKey, kWideTextKey, kWideTextKey,
    kWideTextKey, kWideTextKey, kWideTextKey, LV_KEYBOARD_CTRL_BTN_FLAGS | 3,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, 8, kWideTextKey, LV_KEYBOARD_CTRL_BTN_FLAGS | 4,
};

const lv_btnmatrix_ctrl_t kCompactKeyboardSymbolCtrlMap[] = {
    kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey,
    kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey, kTextKey,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, kWideTextKey, kWideTextKey, kWideTextKey, kWideTextKey,
    kWideTextKey, LV_KEYBOARD_CTRL_BTN_FLAGS | 3,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, 8, kWideTextKey, LV_KEYBOARD_CTRL_BTN_FLAGS | 4,
};

void onDialogKeyboardInput(lv_event_t* event) {
  if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;

  lv_obj_t* keyboard = lv_event_get_target(event);
  const uint16_t button_id = lv_btnmatrix_get_selected_btn(keyboard);
  if (button_id == LV_BTNMATRIX_BTN_NONE) return;
  const char* key = lv_keyboard_get_btn_text(keyboard, button_id);
  lv_obj_t* textarea = lv_keyboard_get_textarea(keyboard);
  if (!key || !textarea) return;

  if (strcmp(key, LV_SYMBOL_UP) == 0) {
    const lv_keyboard_mode_t mode = lv_keyboard_get_mode(keyboard);
    lv_keyboard_set_mode(keyboard, mode == LV_KEYBOARD_MODE_TEXT_UPPER
                                        ? LV_KEYBOARD_MODE_TEXT_LOWER
                                        : LV_KEYBOARD_MODE_TEXT_UPPER);
  } else if (strcmp(key, LV_SYMBOL_DOWN) == 0) {
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  } else if (strcmp(key, "123") == 0) {
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_SPECIAL);
  } else if (strcmp(key, "#+=") == 0) {
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
  } else if (strcmp(key, "ABC") == 0) {
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  } else if (strcmp(key, LV_SYMBOL_OK) == 0) {
    lv_event_send(textarea, LV_EVENT_READY, nullptr);
  } else if (strcmp(key, LV_SYMBOL_CLOSE) == 0 || strcmp(key, LV_SYMBOL_KEYBOARD) == 0) {
    lv_event_send(textarea, LV_EVENT_CANCEL, nullptr);
  } else if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
    lv_textarea_del_char(textarea);
  } else if (strcmp(key, LV_SYMBOL_LEFT) == 0) {
    lv_textarea_cursor_left(textarea);
  } else if (strcmp(key, LV_SYMBOL_RIGHT) == 0) {
    if (lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_NUMBER) {
      lv_textarea_cursor_right(textarea);
    } else {
      lv_event_send(textarea, LV_EVENT_READY, nullptr);
    }
  } else if (strcmp(key, LV_SYMBOL_NEW_LINE) == 0 || strcmp(key, "Enter") == 0) {
    lv_textarea_add_char(textarea, '\n');
    if (lv_textarea_get_one_line(textarea)) lv_event_send(textarea, LV_EVENT_READY, nullptr);
  } else if (strcmp(key, "+/-") == 0) {
    const uint16_t cursor = lv_textarea_get_cursor_pos(textarea);
    const char* text = lv_textarea_get_text(textarea);
    if (text[0] == '-') {
      lv_textarea_set_cursor_pos(textarea, 1);
      lv_textarea_del_char(textarea);
      lv_textarea_add_char(textarea, '+');
      lv_textarea_set_cursor_pos(textarea, cursor);
    } else if (text[0] == '+') {
      lv_textarea_set_cursor_pos(textarea, 1);
      lv_textarea_del_char(textarea);
      lv_textarea_add_char(textarea, '-');
      lv_textarea_set_cursor_pos(textarea, cursor);
    } else {
      lv_textarea_set_cursor_pos(textarea, 0);
      lv_textarea_add_char(textarea, '-');
      lv_textarea_set_cursor_pos(textarea, cursor + 1);
    }
  } else {
    lv_textarea_add_text(textarea, key);
  }
}

void enableNumericKeyboardPopovers(lv_obj_t* keyboard) {
  lv_btnmatrix_set_btn_ctrl_all(keyboard, LV_BTNMATRIX_CTRL_POPOVER);
  // Keep mode, confirmation, editing, and cursor controls free of popovers.
  constexpr uint16_t kActionButtonIds[] = {3, 7, 11, 12, 15, 16};
  for (uint16_t id : kActionButtonIds) {
    lv_btnmatrix_clear_btn_ctrl(keyboard, id, LV_BTNMATRIX_CTRL_POPOVER);
  }
}

lv_obj_t* createDialogKeyboard(lv_obj_t* parent, lv_obj_t* textarea) {
  lv_obj_t* keyboard = lv_keyboard_create(parent);
  // Use the compact layout's arrow keys for case switching instead of LVGL's
  // literal "ABC"/"abc" labels.
  lv_obj_remove_event_cb(keyboard, lv_keyboard_def_event_cb);
  lv_obj_add_event_cb(keyboard, onDialogKeyboardInput, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_set_size(keyboard, LV_PCT(100), kKeyboardHeight);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  if (kLargeScreen) {
    lv_obj_set_style_text_font(keyboard, UI_FONT_TITLE, LV_PART_ITEMS);
    lv_obj_set_style_text_font(textarea, UI_FONT_TITLE, LV_PART_MAIN);
  }
  lv_keyboard_set_textarea(keyboard, textarea);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, kCompactKeyboardLowerMap,
                      kCompactKeyboardTextCtrlMap);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, kCompactKeyboardUpperMap,
                      kCompactKeyboardTextCtrlMap);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL, kCompactKeyboardSymbolMap,
                      kCompactKeyboardSymbolCtrlMap);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1, kCompactKeyboardMoreSymbolMap,
                      kCompactKeyboardSymbolCtrlMap);
  lv_keyboard_set_popovers(keyboard, true);
  lv_obj_add_state(textarea, LV_STATE_FOCUSED);
  return keyboard;
}

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
  lv_obj_set_size(header, LV_PCT(100), kDialogHeaderHeight);
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
  lv_obj_set_width(title, kScreenWidth - uiScaled(96, 120));
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(title, UI_FONT_TITLE, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(kColorAccent), LV_PART_MAIN);

  lv_obj_t* close = lv_btn_create(header);
  styleButton(close);
  lv_obj_set_size(close, uiScaled(40, 76), uiScaled(30, 56));
  lv_obj_add_event_cb(close, on_close, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* close_label = lv_label_create(close);
  lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
  if (kLargeScreen) lv_obj_set_style_text_font(close_label, UI_FONT_HEADER, LV_PART_MAIN);
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
  lv_obj_set_size(content, kDialogWidth, kScreenHeight - topInset - kDialogHeaderHeight - 16);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(content, 8, LV_PART_MAIN);
  lv_obj_add_flag(content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
  return content;
}

#if WLED_BOARD == WLED_BOARD_JC4880P443
// ESP-Hosted exposes the C6 update image that matches the P4's installed
// Arduino runtime. Keep the update opt-in and make its restart effect explicit.
lv_obj_t* c6_update_dialog = nullptr;
lv_obj_t* c6_update_message = nullptr;
lv_obj_t* c6_update_button = nullptr;
lv_obj_t* c6_update_button_label = nullptr;

void onC6UpdateDialogDeleted(lv_event_t*) {
  c6_update_dialog = nullptr;
  c6_update_message = nullptr;
  c6_update_button = nullptr;
  c6_update_button_label = nullptr;
}

void closeC6UpdateDialog(lv_event_t*) {
  if (c6_update_dialog) lv_obj_del_async(c6_update_dialog);
}

void runC6UpdateTimer(lv_timer_t*) {
  if (wifilink::updateHostedFirmware()) {
    return;  // The P4 restarts immediately after the C6 image is activated.
  }

  if (!c6_update_dialog || !c6_update_message) return;
  lv_label_set_text(c6_update_message,
                    "Update failed. Restart the remote before retrying.\n"
                    "See the serial log for the exact error.");
  if (c6_update_button) lv_obj_clear_state(c6_update_button, LV_STATE_DISABLED);
  if (c6_update_button_label) lv_label_set_text(c6_update_button_label, "Retry");
}

void startC6Update(lv_event_t*) {
  if (!c6_update_dialog || !c6_update_message) return;
  lv_label_set_text(c6_update_message,
                    "Updating the bundled matching C6 radio firmware…\n\n"
                    "Keep the remote powered. It will restart when complete.");
  if (c6_update_button) lv_obj_add_state(c6_update_button, LV_STATE_DISABLED);
  // Let LVGL present the warning before the official OTA routine takes
  // exclusive use of the Wi-Fi transport.
  lv_timer_t* timer = lv_timer_create(runC6UpdateTimer, 50, nullptr);
  lv_timer_set_repeat_count(timer, 1);
}

void openC6UpdateDialog(lv_event_t*) {
  if (c6_update_dialog || !wifilink::hostedFirmwareUpdateAvailable()) return;

  c6_update_dialog = createDialogShell("Update C6 radio", closeC6UpdateDialog);
  lv_obj_add_event_cb(c6_update_dialog, onC6UpdateDialogDeleted, LV_EVENT_DELETE, nullptr);

  lv_obj_t* content = lv_obj_create(c6_update_dialog);
  lv_obj_remove_style_all(content);
  lv_obj_set_size(content, std::min<lv_coord_t>(kScreenWidth - 24, 420),
                  kLargeScreen ? 250 : 184);
  lv_obj_align(content, LV_ALIGN_CENTER, 0, 24);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(content, 8, LV_PART_MAIN);
  lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

  c6_update_message = lv_label_create(content);
  lv_obj_set_width(c6_update_message, LV_PCT(100));
  lv_label_set_long_mode(c6_update_message, LV_LABEL_LONG_WRAP);
  lv_label_set_text(c6_update_message,
                    "Update the ESP32-C6 Wi-Fi coprocessor to the firmware\n"
                    "that matches this P4 build?\n\n"
                    "Wi-Fi will disconnect and the remote will restart.");
  lv_obj_set_style_text_align(c6_update_message, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_add_style(c6_update_message, &style_label_muted, LV_PART_MAIN);

  lv_obj_t* actions = lv_obj_create(content);
  lv_obj_remove_style_all(actions);
  lv_obj_set_size(actions, LV_PCT(100), 60);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* cancel = lv_btn_create(actions);
  styleButton(cancel);
  lv_obj_set_size(cancel, 180, 56);
  lv_obj_add_event_cb(cancel, closeC6UpdateDialog, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* cancel_label = lv_label_create(cancel);
  lv_label_set_text(cancel_label, "Cancel");
  lv_obj_center(cancel_label);

  c6_update_button = lv_btn_create(actions);
  styleButton(c6_update_button);
  lv_obj_set_size(c6_update_button, 180, 56);
  lv_obj_add_event_cb(c6_update_button, startC6Update, LV_EVENT_CLICKED, nullptr);
  c6_update_button_label = lv_label_create(c6_update_button);
  lv_label_set_text(c6_update_button_label, "Update");
  lv_obj_center(c6_update_button_label);
}
#endif


void onTargetDialogDeleted(lv_event_t*) {
  target_dialog = nullptr;
  if (target_dialog_refresh_pending) {
    target_dialog_refresh_pending = false;
    lv_async_call([](void*) { showTargetDialog(); }, nullptr);
  }
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
  lv_obj_set_size(controller_name_input, kDialogWidth, kDialogInputHeight);
  lv_obj_align(controller_name_input, LV_ALIGN_TOP_MID, 0, kDialogHeaderHeight + 6);
  lv_textarea_set_one_line(controller_name_input, true);
  lv_textarea_set_max_length(controller_name_input, 32);
  lv_textarea_set_placeholder_text(controller_name_input, "Controller name");
  lv_textarea_set_text(controller_name_input, device.name.c_str());
  lv_obj_add_event_cb(controller_name_input, onControllerNameInput, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(controller_name_input, onControllerNameInput, LV_EVENT_CANCEL, nullptr);

  createDialogKeyboard(controller_name_dialog, controller_name_input);
}

// ── Wi-Fi setup ───────────────────────────────────────────────────────────────

lv_obj_t* wifi_label = nullptr;
lv_timer_t* wifi_refresh_timer = nullptr;

void updateWifiLabel() {
  if (!wifi_label) return;
  const std::string network = wifilink::ssid();
  lv_label_set_text(wifi_label, network.empty() ? "Set up" : network.c_str());
}

lv_obj_t* wifi_dialog = nullptr;
lv_obj_t* wifi_list = nullptr;
lv_obj_t* wifi_status_label = nullptr;
lv_obj_t* wifi_activity_spinner = nullptr;
std::string wifi_list_connected_ssid;
lv_obj_t* wifi_password_dialog = nullptr;
lv_obj_t* wifi_password_input = nullptr;
lv_obj_t* wifi_join_dialog = nullptr;
lv_obj_t* wifi_join_heading = nullptr;
lv_obj_t* wifi_join_detail = nullptr;
lv_obj_t* wifi_join_spinner = nullptr;
lv_obj_t* wifi_join_actions = nullptr;
lv_timer_t* wifi_join_refresh_timer = nullptr;
std::string wifi_pending_ssid;
uint8_t wifi_pending_channel = 0;

enum class WifiJoinView : uint8_t { kUninitialized, kJoining, kConnected, kBadPassword, kNotFound, kFailed };
WifiJoinView wifi_join_view = WifiJoinView::kUninitialized;

void showWifiDialog();
void rebuildWifiList();
void showWifiJoinDialog();
void showWifiPasswordDialog(const char* ssid, bool secured, uint8_t channel);
void onWifiJoinRefreshTick(lv_timer_t* timer);

void updateWifiStatusLabel() {
  if (!wifi_status_label) return;
  if (wifi_activity_spinner) {
    if (wifilink::busy()) lv_obj_clear_flag(wifi_activity_spinner, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(wifi_activity_spinner, LV_OBJ_FLAG_HIDDEN);
  }
  const std::string ip = wifilink::ipAddress();
  if (wifilink::scanning()) {
    lv_label_set_text(wifi_status_label, "Searching for Wi-Fi networks...");
  } else if (wifilink::scanQueued()) {
    lv_label_set_text(wifi_status_label, "Waiting to scan for Wi-Fi networks...");
  } else if (ip.empty()) {
    lv_label_set_text(wifi_status_label, wifilink::statusName(wifilink::status()));
  } else {
    lv_label_set_text_fmt(wifi_status_label, "Connected  %s", ip.c_str());
  }
}

// The scan is asynchronous and the association state changes on its own, so the
// open dialog polls instead of waiting for an event.
void onWifiRefreshTick(lv_timer_t*) {
  static bool was_scanning = false;
  const bool scanning = wifilink::scanning();
  if (was_scanning && !scanning) rebuildWifiList();
  was_scanning = scanning;
  const std::string connected_ssid = wifilink::connectedSsid();
  if (connected_ssid != wifi_list_connected_ssid) {
    wifi_list_connected_ssid = connected_ssid;
    rebuildWifiList();
  }
  updateWifiStatusLabel();
  updateTargetLabel();  // Keeps the top bar in sync with a queued/manual scan.
}

void onWifiDialogDeleted(lv_event_t*) {
  if (wifi_refresh_timer) {
    lv_timer_del(wifi_refresh_timer);
    wifi_refresh_timer = nullptr;
  }
  wifi_dialog = nullptr;
  wifi_list = nullptr;
  wifi_status_label = nullptr;
  wifi_activity_spinner = nullptr;
  wifi_list_connected_ssid.clear();
}

void closeWifiDialog(lv_event_t*) {
  if (wifi_dialog) lv_obj_del_async(wifi_dialog);
}

void onWifiPasswordDialogDeleted(lv_event_t*) {
  wifi_password_dialog = nullptr;
  wifi_password_input = nullptr;
}

void closeWifiPasswordDialog(lv_event_t*) {
  if (wifi_password_dialog) lv_obj_del_async(wifi_password_dialog);
}

void onWifiJoinDialogDeleted(lv_event_t*) {
  if (wifi_join_refresh_timer) {
    lv_timer_del(wifi_join_refresh_timer);
    wifi_join_refresh_timer = nullptr;
  }
  wifi_join_dialog = nullptr;
  wifi_join_heading = nullptr;
  wifi_join_detail = nullptr;
  wifi_join_spinner = nullptr;
  wifi_join_actions = nullptr;
}

bool wifiJoinNeedsDecision() {
  const wifilink::Status status = wifilink::status();
  return status == wifilink::Status::kBadPassword || status == wifilink::Status::kNotFound ||
         status == wifilink::Status::kFailed || status == wifilink::Status::kConnectionLost;
}

void closeWifiJoinDialog(lv_event_t*) {
  // Dismissing a failure card hands the network back to the normal background
  // reconnect ladder; only the explicit card actions keep setup semantics.
  if (wifi_join_dialog && wifiJoinNeedsDecision()) wifilink::resumeReconnect();
  if (wifi_join_dialog) lv_obj_del_async(wifi_join_dialog);
}

lv_obj_t* createWifiJoinAction(const char* text, lv_event_cb_t callback) {
  lv_obj_t* button = lv_btn_create(wifi_join_actions);
  styleButton(button);
  lv_obj_set_height(button, uiScaled(38, 58));
  lv_obj_set_flex_grow(button, 1);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return button;
}

void onWifiJoinContinue(lv_event_t*) {
  closeWifiJoinDialog(nullptr);
  if (main_tabs) lv_tabview_set_act(main_tabs, kSettingsTabIndex, LV_ANIM_ON);
}

void onWifiJoinRetry(lv_event_t*) {
  wifilink::retryConnection();
}

void onWifiJoinRetypePassword(lv_event_t*) {
  if (wifi_join_dialog) lv_obj_del_async(wifi_join_dialog);
  showWifiPasswordDialog(wifi_pending_ssid.c_str(), true, wifi_pending_channel);
}

void onWifiJoinChooseNetwork(lv_event_t*) {
  if (wifi_join_dialog) lv_obj_del_async(wifi_join_dialog);
  showWifiDialog();
  wifilink::startScan();
}

void updateWifiJoinDialog() {
  if (!wifi_join_dialog || !wifi_join_heading || !wifi_join_detail || !wifi_join_actions) return;

  const wifilink::Status status = wifilink::status();
  WifiJoinView view = WifiJoinView::kJoining;
  if (status == wifilink::Status::kConnected) view = WifiJoinView::kConnected;
  else if (status == wifilink::Status::kBadPassword) view = WifiJoinView::kBadPassword;
  else if (status == wifilink::Status::kNotFound) view = WifiJoinView::kNotFound;
  else if (status == wifilink::Status::kFailed || status == wifilink::Status::kConnectionLost ||
           status == wifilink::Status::kNoCredentials) view = WifiJoinView::kFailed;

  if (view == wifi_join_view) return;
  wifi_join_view = view;
  lv_obj_clean(wifi_join_actions);

  if (wifi_join_spinner) {
    if (view == WifiJoinView::kJoining) lv_obj_clear_flag(wifi_join_spinner, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(wifi_join_spinner, LV_OBJ_FLAG_HIDDEN);
  }

  switch (view) {
    case WifiJoinView::kConnected:
      lv_label_set_text(wifi_join_heading, "Connected");
      lv_label_set_text_fmt(wifi_join_detail, "Joined %s. Wi-Fi is ready.", wifilink::connectedSsid().c_str());
      lv_obj_set_style_text_color(wifi_join_heading, lv_color_hex(kColorOk), LV_PART_MAIN);
      updateWifiLabel();
      createWifiJoinAction("Continue", onWifiJoinContinue);
      break;
    case WifiJoinView::kBadPassword:
      lv_label_set_text(wifi_join_heading, "Incorrect password");
      lv_label_set_text_fmt(wifi_join_detail, "The password for %s was not accepted.", wifi_pending_ssid.c_str());
      lv_obj_set_style_text_color(wifi_join_heading, lv_color_hex(kColorDanger), LV_PART_MAIN);
      createWifiJoinAction("Re-enter password", onWifiJoinRetypePassword);
      createWifiJoinAction("Choose network", onWifiJoinChooseNetwork);
      break;
    case WifiJoinView::kNotFound:
      lv_label_set_text(wifi_join_heading, "Network not found");
      lv_label_set_text_fmt(wifi_join_detail, "%s is not available. Check that it is nearby and powered on.",
                            wifi_pending_ssid.c_str());
      lv_obj_set_style_text_color(wifi_join_heading, lv_color_hex(kColorDanger), LV_PART_MAIN);
      createWifiJoinAction("Try again", onWifiJoinRetry);
      createWifiJoinAction("Choose network", onWifiJoinChooseNetwork);
      break;
    case WifiJoinView::kFailed:
      lv_label_set_text(wifi_join_heading, "Could not join network");
      lv_label_set_text_fmt(wifi_join_detail, "Couldn't connect to %s. Try again or choose another network.",
                            wifi_pending_ssid.c_str());
      lv_obj_set_style_text_color(wifi_join_heading, lv_color_hex(kColorDanger), LV_PART_MAIN);
      createWifiJoinAction("Try again", onWifiJoinRetry);
      createWifiJoinAction("Choose network", onWifiJoinChooseNetwork);
      break;
    case WifiJoinView::kJoining:
      lv_label_set_text(wifi_join_heading, "Joining Wi-Fi...");
      lv_label_set_text_fmt(wifi_join_detail, "Connecting to %s. This can take a few seconds.",
                            wifi_pending_ssid.c_str());
      lv_obj_set_style_text_color(wifi_join_heading, lv_color_hex(kColorAccent), LV_PART_MAIN);
      break;
  }
}

void onWifiJoinRefreshTick(lv_timer_t*) {
  updateWifiJoinDialog();
}

void showWifiJoinDialog() {
  if (wifi_join_dialog) return;
  // Force the first refresh to render the current state, including the fast
  // simulator path where the association completes before this dialog opens.
  wifi_join_view = WifiJoinView::kUninitialized;
  wifi_join_dialog = createDialogShell("Wi-Fi", closeWifiJoinDialog);
  lv_obj_add_event_cb(wifi_join_dialog, onWifiJoinDialogDeleted, LV_EVENT_DELETE, nullptr);

  wifi_join_heading = lv_label_create(wifi_join_dialog);
  lv_obj_set_width(wifi_join_heading, kDialogWidth - 24);
  lv_label_set_long_mode(wifi_join_heading, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(wifi_join_heading, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_font(wifi_join_heading, UI_FONT_HEADER, LV_PART_MAIN);
  lv_obj_align(wifi_join_heading, LV_ALIGN_TOP_MID, 0, kDialogHeaderHeight + uiScaled(20, 34));

  wifi_join_detail = lv_label_create(wifi_join_dialog);
  lv_obj_set_width(wifi_join_detail, kDialogWidth - 32);
  lv_label_set_long_mode(wifi_join_detail, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(wifi_join_detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_add_style(wifi_join_detail, &style_label_muted, LV_PART_MAIN);
  lv_obj_align(wifi_join_detail, LV_ALIGN_TOP_MID, 0, kDialogHeaderHeight + uiScaled(48, 82));

  wifi_join_spinner = lv_spinner_create(wifi_join_dialog, 900, 70);
  lv_obj_set_size(wifi_join_spinner, uiScaled(34, 52), uiScaled(34, 52));
  lv_obj_align(wifi_join_spinner, LV_ALIGN_CENTER, 0, uiScaled(12, 26));
  lv_obj_set_style_arc_width(wifi_join_spinner, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_width(wifi_join_spinner, 3, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(wifi_join_spinner, lv_color_hex(kColorBorder), LV_PART_MAIN);
  lv_obj_set_style_arc_color(wifi_join_spinner, lv_color_hex(kColorAccent), LV_PART_INDICATOR);

  wifi_join_actions = lv_obj_create(wifi_join_dialog);
  lv_obj_remove_style_all(wifi_join_actions);
  lv_obj_set_size(wifi_join_actions, kDialogWidth, uiScaled(42, 62));
  lv_obj_align(wifi_join_actions, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_set_flex_flow(wifi_join_actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(wifi_join_actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(wifi_join_actions, 8, LV_PART_MAIN);
  lv_obj_clear_flag(wifi_join_actions, LV_OBJ_FLAG_SCROLLABLE);

  updateWifiJoinDialog();
  // Poll for the dialog's whole life: a driver-side retry can still land after
  // a failure card is shown, and the card must follow it to Connected.
  wifi_join_refresh_timer = lv_timer_create(onWifiJoinRefreshTick, 500, nullptr);
}

void onWifiPasswordInput(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_CANCEL) {
    closeWifiPasswordDialog(nullptr);
    return;
  }
  if (code != LV_EVENT_READY || !wifi_password_input) return;
  wifilink::saveCredentials(wifi_pending_ssid.c_str(), lv_textarea_get_text(wifi_password_input),
                            wifi_pending_channel);
  closeWifiPasswordDialog(nullptr);
  closeWifiDialog(nullptr);
  updateWifiLabel();
  if (!wifilink::accessPointEnabled()) showWifiJoinDialog();
}

// Same on-screen keyboard pattern as the controller/preset name dialogs.
void showWifiPasswordDialog(const char* ssid, bool secured, uint8_t channel) {
  if (wifi_password_dialog || !ssid) return;
  wifi_pending_ssid = ssid;
  wifi_pending_channel = channel;

  if (!secured) {
    wifilink::saveCredentials(ssid, "", channel);
    closeWifiDialog(nullptr);
    updateWifiLabel();
    if (!wifilink::accessPointEnabled()) showWifiJoinDialog();
    return;
  }

  wifi_password_dialog = createDialogShell(ssid, closeWifiPasswordDialog);
  lv_obj_add_event_cb(wifi_password_dialog, onWifiPasswordDialogDeleted, LV_EVENT_DELETE, nullptr);

  wifi_password_input = lv_textarea_create(wifi_password_dialog);
  lv_obj_set_size(wifi_password_input, kDialogWidth, kDialogInputHeight);
  lv_obj_align(wifi_password_input, LV_ALIGN_TOP_MID, 0, kDialogHeaderHeight + 6);
  lv_textarea_set_one_line(wifi_password_input, true);
  lv_textarea_set_password_mode(wifi_password_input, false);
  lv_textarea_set_max_length(wifi_password_input, kMaxWifiPassLength);
  lv_textarea_set_placeholder_text(wifi_password_input, "Wi-Fi password");
  lv_obj_add_event_cb(wifi_password_input, onWifiPasswordInput, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(wifi_password_input, onWifiPasswordInput, LV_EVENT_CANCEL, nullptr);

  createDialogKeyboard(wifi_password_dialog, wifi_password_input);
}

void onWifiNetworkPicked(lv_event_t* event) {
  const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  const auto& networks = wifilink::results();
  if (index >= networks.size()) return;
  showWifiPasswordDialog(networks[index].ssid.c_str(), networks[index].secured, networks[index].channel);
}

lv_obj_t* manual_ip_dialog = nullptr;
lv_obj_t* manual_ip_input = nullptr;

void onManualIpDialogDeleted(lv_event_t*) {
  manual_ip_dialog = nullptr;
  manual_ip_input = nullptr;
}

void closeManualIpDialog(lv_event_t*) {
  if (manual_ip_dialog) lv_obj_del_async(manual_ip_dialog);
}

void onManualIpInput(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_CANCEL) {
    closeManualIpDialog(nullptr);
    return;
  }
  if (code != LV_EVENT_READY || !manual_ip_input) return;
  const char* host = lv_textarea_get_text(manual_ip_input);
  if (wled::addDeviceByAddress(host)) {
    closeManualIpDialog(nullptr);
    closeWifiDialog(nullptr);
  } else {
    lv_textarea_set_placeholder_text(manual_ip_input, "No WLED at that address");
    lv_textarea_set_text(manual_ip_input, "");
  }
}

// WLED only advertises mDNS when an mDNS name is set in its settings, so an
// address can always be entered by hand.
void onWifiManualIp(lv_event_t*) {
  if (manual_ip_dialog) return;
  manual_ip_dialog = createDialogShell("WLED address", closeManualIpDialog);
  lv_obj_add_event_cb(manual_ip_dialog, onManualIpDialogDeleted, LV_EVENT_DELETE, nullptr);

  manual_ip_input = lv_textarea_create(manual_ip_dialog);
  lv_obj_set_size(manual_ip_input, kDialogWidth, kDialogInputHeight);
  lv_obj_align(manual_ip_input, LV_ALIGN_TOP_MID, 0, kDialogHeaderHeight + 6);
  lv_textarea_set_one_line(manual_ip_input, true);
  lv_textarea_set_max_length(manual_ip_input, 15);
  lv_textarea_set_accepted_chars(manual_ip_input, "0123456789.");
  lv_textarea_set_placeholder_text(manual_ip_input, "192.168.1.50");
  lv_obj_add_event_cb(manual_ip_input, onManualIpInput, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(manual_ip_input, onManualIpInput, LV_EVENT_CANCEL, nullptr);

  lv_obj_t* keyboard = createDialogKeyboard(manual_ip_dialog, manual_ip_input);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
  enableNumericKeyboardPopovers(keyboard);
}

void onWifiRescan(lv_event_t*) {
  if (wifilink::startScan()) {
    updateWifiStatusLabel();
    rebuildWifiList();
  } else {
    updateWifiStatusLabel();
  }
}

void onWifiForget(lv_event_t*) {
  wifilink::forgetCredentials();
  updateWifiLabel();
  closeWifiDialog(nullptr);
}

// Networks are few enough to render as plain rows; no recycling needed.
void rebuildWifiList() {
  if (!wifi_list) return;
  lv_obj_clean(wifi_list);

  const auto& networks = wifilink::results();
  if (networks.empty()) {
    lv_obj_t* empty = lv_label_create(wifi_list);
    lv_obj_add_style(empty, &style_label_muted, LV_PART_MAIN);
    styleWifiMenuLabel(empty);
    lv_label_set_text(empty, wifilink::scanning()
                                  ? "Scanning..."
                                  : (wifilink::hasScanned() ? "No networks found"
                                                            : "Tap Rescan to find networks"));
    return;
  }

  for (size_t i = 0; i < networks.size(); ++i) {
    // lv_btn_create applies the default theme's eight button styles before
    // styleButton() adds ours. Recreating a scan list on the 320 KiB CYD can
    // fragment the heap until one of those reallocations fails inside LVGL.
    // A clickable label is sufficient for a network row and needs only its
    // text plus our two shared styles.
    lv_obj_t* row = lv_label_create(wifi_list);
    styleButton(row, false);
    styleWifiMenuLabel(row);
    lv_obj_set_size(row, LV_PCT(100), uiScaled(44, 60));
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_left(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(row, onWifiNetworkPicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(uintptr_t(i)));

    // A checkmark must mean an active association, never merely credentials
    // saved from a failed attempt.
    const bool connected = wifilink::connected() &&
                           networks[i].ssid == wifilink::connectedSsid();
    lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
    lv_label_set_text_fmt(row, "%s%s%s  %ddBm", connected ? LV_SYMBOL_OK " " : "",
                          networks[i].ssid.c_str(),
                          networks[i].secured ? "  " LV_SYMBOL_WIFI : "", networks[i].rssi);
    lv_obj_set_style_pad_top(row, uiScaled(12, 18), LV_PART_MAIN);
  }
}

void showWifiDialog() {
  if (wifi_dialog) return;
  wifi_dialog = createDialogShell("Wi-Fi", closeWifiDialog);
  lv_obj_add_event_cb(wifi_dialog, onWifiDialogDeleted, LV_EVENT_DELETE, nullptr);

  const lv_coord_t status_y = kDialogHeaderHeight + 8;
  const lv_coord_t actions_y = status_y + uiScaled(24, 34);
  const lv_coord_t action_height = uiScaled(34, 52);
  const lv_coord_t list_y = actions_y + action_height + 12;

  wifi_status_label = lv_label_create(wifi_dialog);
  lv_obj_add_style(wifi_status_label, &style_label_muted, LV_PART_MAIN);
  styleWifiMenuLabel(wifi_status_label);
  lv_obj_set_width(wifi_status_label, kDialogWidth - uiScaled(28, 42));
  lv_label_set_long_mode(wifi_status_label, LV_LABEL_LONG_DOT);
  lv_obj_align(wifi_status_label, LV_ALIGN_TOP_LEFT, 12, status_y);

  wifi_activity_spinner = lv_spinner_create(wifi_dialog, 800, 70);
  lv_obj_set_size(wifi_activity_spinner, uiScaled(16, 24), uiScaled(16, 24));
  lv_obj_align(wifi_activity_spinner, LV_ALIGN_TOP_RIGHT, -12, status_y);
  lv_obj_set_style_arc_width(wifi_activity_spinner, 2, LV_PART_MAIN);
  lv_obj_set_style_arc_width(wifi_activity_spinner, 2, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(wifi_activity_spinner, lv_color_hex(kColorBorder), LV_PART_MAIN);
  lv_obj_set_style_arc_color(wifi_activity_spinner, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
  lv_obj_add_flag(wifi_activity_spinner, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* actions = lv_obj_create(wifi_dialog);
  lv_obj_remove_style_all(actions);
  lv_obj_set_size(actions, kDialogWidth, action_height + 4);
  lv_obj_align(actions, LV_ALIGN_TOP_MID, 0, actions_y);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(actions, 6, LV_PART_MAIN);

  const uint8_t action_count = wifilink::hasCredentials() ? 3 : 2;
  const lv_coord_t action_width =
      (kDialogWidth - (action_count - 1) * 6) / action_count;

  lv_obj_t* rescan = lv_btn_create(actions);
  styleButton(rescan, false);
  lv_obj_set_size(rescan, action_width, action_height);
  lv_obj_add_event_cb(rescan, onWifiRescan, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* rescan_label = lv_label_create(rescan);
  lv_label_set_text(rescan_label, LV_SYMBOL_REFRESH " Rescan");
  styleWifiMenuLabel(rescan_label);
  lv_obj_center(rescan_label);

  lv_obj_t* manual = lv_btn_create(actions);
  styleButton(manual, false);
  lv_obj_set_size(manual, action_width, action_height);
  lv_obj_add_event_cb(manual, onWifiManualIp, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* manual_label = lv_label_create(manual);
  lv_label_set_text(manual_label, "WLED IP");
  styleWifiMenuLabel(manual_label);
  lv_obj_center(manual_label);

  if (wifilink::hasCredentials()) {
    lv_obj_t* forget = lv_btn_create(actions);
    styleButton(forget, false);
    lv_obj_set_size(forget, action_width, action_height);
    lv_obj_add_event_cb(forget, onWifiForget, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* forget_label = lv_label_create(forget);
    lv_label_set_text(forget_label, "Forget");
    styleWifiMenuLabel(forget_label);
    lv_obj_center(forget_label);
  }

  wifi_list = lv_obj_create(wifi_dialog);
  lv_obj_remove_style_all(wifi_list);
  lv_obj_set_size(wifi_list, kDialogWidth, kScreenHeight - list_y - 12);
  lv_obj_align(wifi_list, LV_ALIGN_TOP_MID, 0, list_y);
  lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(wifi_list, 6, LV_PART_MAIN);

  // A scan and station association contend for the same radio.  Only scan
  // automatically while setting up a device with no saved credentials.
  if (!wifilink::hasCredentials()) wifilink::startScan();
  wifi_list_connected_ssid = wifilink::connectedSsid();
  rebuildWifiList();
  updateWifiStatusLabel();
  wifi_refresh_timer = lv_timer_create(onWifiRefreshTick, 500, nullptr);
}

void openWifiDialog(lv_event_t*) {
  showWifiDialog();
}

// ── Mobile access point ─────────────────────────────────────────────────────

lv_obj_t* access_point_label = nullptr;
lv_obj_t* access_point_dialog = nullptr;
lv_obj_t* access_point_switch = nullptr;
lv_obj_t* access_point_name_value = nullptr;
lv_obj_t* access_point_password_value = nullptr;
lv_obj_t* access_point_editor_dialog = nullptr;
lv_obj_t* access_point_editor_input = nullptr;

enum class AccessPointEdit : uintptr_t { kName, kPassword };
AccessPointEdit access_point_editing = AccessPointEdit::kName;

void updateAccessPointLabel() {
  if (!access_point_label) return;
  if (wifilink::accessPointActive()) {
    lv_label_set_text(access_point_label, wifilink::accessPointName().c_str());
  } else if (wifilink::accessPointEnabled()) {
    lv_label_set_text(access_point_label, "Starting...");
  } else {
    lv_label_set_text(access_point_label, "Off");
  }
}

void onAccessPointDialogDeleted(lv_event_t*) {
  access_point_dialog = nullptr;
  access_point_switch = nullptr;
  access_point_name_value = nullptr;
  access_point_password_value = nullptr;
}

void closeAccessPointDialog(lv_event_t*) {
  if (access_point_dialog) lv_obj_del_async(access_point_dialog);
}

void saveAccessPointEnabledState() {
  if (!access_point_switch) return;
  wifilink::setAccessPoint(lv_obj_has_state(access_point_switch, LV_STATE_CHECKED),
                           wifilink::accessPointName().c_str(),
                           wifilink::accessPointPassword().c_str());
  updateAccessPointLabel();
}

void onAccessPointSwitchChanged(lv_event_t*) {
  saveAccessPointEnabledState();
}

void updateAccessPointDialogValues() {
  if (access_point_name_value) {
    lv_label_set_text(access_point_name_value, wifilink::accessPointName().c_str());
  }
  if (access_point_password_value) {
    lv_label_set_text(access_point_password_value, wifilink::accessPointPassword().c_str());
  }
}

void onAccessPointEditorDeleted(lv_event_t*) {
  access_point_editor_dialog = nullptr;
  access_point_editor_input = nullptr;
}

void closeAccessPointEditor(lv_event_t*) {
  if (access_point_editor_dialog) lv_obj_del_async(access_point_editor_dialog);
}

void rejectAccessPointEditor(const char* reason) {
  if (!access_point_editor_input) return;
  // Match the existing manual-IP validation feedback: keep the dialog open
  // and make the correction needed visible in the input itself.
  lv_textarea_set_text(access_point_editor_input, "");
  lv_textarea_set_placeholder_text(access_point_editor_input, reason);
}

void onAccessPointEditorInput(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_CANCEL) {
    closeAccessPointEditor(nullptr);
    return;
  }
  if (code != LV_EVENT_READY || !access_point_editor_input) return;

  const char* value = lv_textarea_get_text(access_point_editor_input);
  const size_t length = strnlen(value, kMaxWifiPassLength + 1);
  if (access_point_editing == AccessPointEdit::kName && !length) {
    rejectAccessPointEditor("Hotspot SSID cannot be empty");
    return;
  }
  if (access_point_editing == AccessPointEdit::kPassword &&
      (length < 8 || length > kMaxWifiPassLength)) {
    rejectAccessPointEditor("Password must be 8-63 characters");
    return;
  }

  bool saved = false;
  if (access_point_editing == AccessPointEdit::kName) {
    saved = wifilink::setAccessPoint(wifilink::accessPointEnabled(), value,
                                     wifilink::accessPointPassword().c_str());
  } else {
    saved = wifilink::setAccessPoint(wifilink::accessPointEnabled(), wifilink::accessPointName().c_str(), value);
  }
  if (!saved) {
    rejectAccessPointEditor("Could not save hotspot settings");
    return;
  }
  updateAccessPointLabel();
  updateAccessPointDialogValues();
  closeAccessPointEditor(nullptr);
}

void showAccessPointEditor(AccessPointEdit edit) {
  if (access_point_editor_dialog) return;
  access_point_editing = edit;
  access_point_editor_dialog = createDialogShell(edit == AccessPointEdit::kName ? "Hotspot SSID" : "Hotspot password",
                                                  closeAccessPointEditor);
  lv_obj_add_event_cb(access_point_editor_dialog, onAccessPointEditorDeleted, LV_EVENT_DELETE, nullptr);

  access_point_editor_input = lv_textarea_create(access_point_editor_dialog);
  lv_obj_set_size(access_point_editor_input, kDialogWidth, kDialogInputHeight);
  lv_obj_align(access_point_editor_input, LV_ALIGN_TOP_MID, 0, kDialogHeaderHeight + 8);
  lv_textarea_set_one_line(access_point_editor_input, true);
  const bool editingName = edit == AccessPointEdit::kName;
  lv_textarea_set_max_length(access_point_editor_input, editingName ? kMaxSsidLength : kMaxWifiPassLength);
  lv_textarea_set_text(access_point_editor_input,
                       editingName ? wifilink::accessPointName().c_str()
                                   : wifilink::accessPointPassword().c_str());
  lv_obj_add_event_cb(access_point_editor_input, onAccessPointEditorInput, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(access_point_editor_input, onAccessPointEditorInput, LV_EVENT_CANCEL, nullptr);
  createDialogKeyboard(access_point_editor_dialog, access_point_editor_input);
}

void onAccessPointEdit(lv_event_t* event) {
  showAccessPointEditor(static_cast<AccessPointEdit>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
}

void createAccessPointValueRow(lv_obj_t* parent, const char* name, lv_obj_t** value_out,
                               AccessPointEdit edit, lv_coord_t y) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, kDialogWidth - 24, uiScaled(34, 52));
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_border_color(row, lv_color_hex(kColorBorder), LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_left(row, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_right(row, 4, LV_PART_MAIN);

  lv_obj_t* name_label = lv_label_create(row);
  lv_label_set_text(name_label, name);
  lv_obj_add_style(name_label, &style_label_muted, LV_PART_MAIN);

  *value_out = lv_label_create(row);
  lv_obj_set_width(*value_out, uiScaled(122, 260));
  lv_label_set_long_mode(*value_out, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(*value_out, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_obj_set_style_text_color(*value_out, lv_color_hex(kColorText), LV_PART_MAIN);

  lv_obj_t* edit_button = lv_btn_create(row);
  styleButton(edit_button);
  lv_obj_set_size(edit_button, uiScaled(32, 48), uiScaled(26, 40));
  lv_obj_add_event_cb(edit_button, onAccessPointEdit, LV_EVENT_CLICKED,
                      reinterpret_cast<void*>(static_cast<uintptr_t>(edit)));
  lv_obj_t* edit_label = lv_label_create(edit_button);
  lv_label_set_text(edit_label, LV_SYMBOL_EDIT);
  lv_obj_center(edit_label);
}

void openAccessPointDialog(lv_event_t*) {
  if (access_point_dialog) return;
  access_point_dialog = createDialogShell("Mobile hotspot", closeAccessPointDialog);
  lv_obj_add_event_cb(access_point_dialog, onAccessPointDialogDeleted, LV_EVENT_DELETE, nullptr);

  // The row is taller than the switch so the extended click area below has room
  // to grow inside it; a hit area larger than the parent would be clipped away.
  lv_obj_t* enabled_row = lv_obj_create(access_point_dialog);
  lv_obj_remove_style_all(enabled_row);
  lv_obj_set_size(enabled_row, kDialogWidth - 24, uiScaled(48, 72));
  lv_obj_align(enabled_row, LV_ALIGN_TOP_MID, 0, kDialogHeaderHeight + uiScaled(16, 26));
  lv_obj_set_flex_flow(enabled_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(enabled_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(enabled_row, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_right(enabled_row, 4, LV_PART_MAIN);
  lv_obj_clear_flag(enabled_row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* enabled_label = lv_label_create(enabled_row);
  lv_label_set_text(enabled_label, "Enable local Wi-Fi");
  lv_obj_set_style_text_font(enabled_label, UI_FONT_HEADER, LV_PART_MAIN);
  lv_obj_set_style_text_color(enabled_label, lv_color_hex(kColorText), LV_PART_MAIN);

  access_point_switch = lv_switch_create(enabled_row);
  lv_obj_set_size(access_point_switch, uiScaled(60, 92), uiScaled(32, 50));
  lv_obj_set_ext_click_area(access_point_switch, uiScaled(8, 11));
  if (wifilink::accessPointEnabled()) lv_obj_add_state(access_point_switch, LV_STATE_CHECKED);
  lv_obj_add_event_cb(access_point_switch, onAccessPointSwitchChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  createAccessPointValueRow(access_point_dialog, "SSID", &access_point_name_value, AccessPointEdit::kName,
                            kDialogHeaderHeight + uiScaled(72, 114));
  createAccessPointValueRow(access_point_dialog, "Password", &access_point_password_value,
                            AccessPointEdit::kPassword, kDialogHeaderHeight + uiScaled(112, 178));
  updateAccessPointDialogValues();
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
  refreshTargetDialog();
}

// Presents every WLED discovered on the local network.  Opens even when nothing
// has been found yet, since this is the only place a rescan can be triggered.
void showTargetDialog() {
  if (target_dialog) return;
  target_dialog = createDialogShell("Control WLED", closeTargetDialog);
  lv_obj_add_event_cb(target_dialog, onTargetDialogDeleted, LV_EVENT_DELETE, nullptr);

  lv_obj_t* content = lv_obj_create(target_dialog);
  lv_obj_remove_style_all(content);
  lv_obj_set_size(content, kDialogWidth, kScreenHeight - kDialogHeaderHeight - 16);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(content, 6, LV_PART_MAIN);
  configurePageScroll(content, true);

  lv_obj_t* scanButton = lv_btn_create(content);
  styleButton(scanButton);
  lv_obj_set_size(scanButton, LV_PCT(100), uiScaled(36, 52));
  lv_obj_add_event_cb(scanButton, onTargetScan, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* scanLabel = lv_label_create(scanButton);
  lv_label_set_text(scanLabel, LV_SYMBOL_REFRESH " Scan for linked controllers");
  lv_obj_center(scanLabel);

  if (!wled::deviceCount()) {
    lv_obj_t* hint = lv_label_create(content);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    if (!wifilink::connected()) {
      lv_label_set_text(hint, "No Wi-Fi connection. Connect Wi-Fi before searching for WLED devices.");
    } else if (wled::connectionStatus() == wled::ConnectionStatus::kSearching) {
      lv_label_set_text(hint, "Searching for WLED devices on this Wi-Fi network...");
    } else {
      lv_label_set_text(hint,
                        "Searching for WLED devices. You can also add one by IP from the Wi-Fi screen.");
    }
    lv_obj_add_style(hint, &style_label_muted, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorTextMuted), LV_PART_MAIN);
  }

  if (wled::deviceCount() > 1) {
    lv_obj_t* allButton = lv_btn_create(content);
    styleButton(allButton, true);
    lv_obj_set_size(allButton, LV_PCT(100), uiScaled(38, 56));
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
    lv_obj_set_size(row, LV_PCT(100), uiScaled(38, 56));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* button = lv_btn_create(row);
    styleButton(button, true);
    lv_obj_set_height(button, uiScaled(38, 56));
    lv_obj_set_flex_grow(button, 1);
    if (!device.online) lv_obj_add_state(button, LV_STATE_DISABLED);
    if (!wled::targetingAll() && wled::focusedDevice() == i) lv_obj_add_state(button, LV_STATE_CHECKED);
    lv_obj_add_event_cb(button, onTargetSelected, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1)));

    char text[64];
    const char* name = device.name.empty() ? "WLED" : device.name.c_str();
    snprintf(text, sizeof(text), "%s", name);
    lv_obj_t* label = lv_label_create(button);
    lv_obj_set_width(label, uiScaled(116, 230));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* connection = lv_label_create(button);
    lv_obj_set_width(connection, uiScaled(58, 90));
    lv_label_set_long_mode(connection, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(connection, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    const wled::ConnectionStatus link = wled::connectionStatus();
    const bool focused = !wled::targetingAll() && wled::focusedDevice() == i;
    const char* connectionText = device.online ? "Online" : "Offline";
    if (focused && link == wled::ConnectionStatus::kConnecting) connectionText = "Connecting";
    else if (focused && link == wled::ConnectionStatus::kReconnecting) connectionText = "Reconnecting";
    else if (focused && link == wled::ConnectionStatus::kConnectionLost) connectionText = "Offline";
    lv_label_set_text(connection, connectionText);
    lv_obj_set_style_text_color(connection,
                                lv_color_hex(device.online && (!focused || link != wled::ConnectionStatus::kConnectionLost)
                                                 ? kColorOk
                                                 : kColorDanger), LV_PART_MAIN);
    lv_obj_align(connection, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t* rename = lv_btn_create(row);
    styleButton(rename);
    lv_obj_set_size(rename, uiScaled(42, 64), uiScaled(38, 56));
    lv_obj_add_event_cb(rename, device.online ? onTargetRename : onTargetForget, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    lv_obj_t* renameLabel = lv_label_create(rename);
    lv_label_set_text(renameLabel, device.online ? LV_SYMBOL_EDIT : LV_SYMBOL_TRASH);
    lv_obj_center(renameLabel);
  }
}



// WLED accepts preset IDs from 1 through 250; adding presets must not inherit
// the old fixed-row UI limit.
uint8_t firstFreePresetSlot() {
  const std::vector<wled::PresetInfo>& presets = wled::model().presets;
  for (uint16_t candidate = 1; candidate <= 250; ++candidate) {
    const uint8_t id = static_cast<uint8_t>(candidate);
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
  preset_name_id = 0;
}

void closePresetNameDialog(lv_event_t*) {
  if (preset_name_dialog) lv_obj_del_async(preset_name_dialog);
}

// Saves the current WLED state when the keyboard confirms a non-empty preset name.
// For an existing preset, its state is applied before this is queued so a rename
// keeps the preset contents intact.
void onPresetNameInput(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_CANCEL) {
    closePresetNameDialog(nullptr);
    return;
  }
  if (code != LV_EVENT_READY || !preset_name_input) return;
  if (!wledControlsAvailable()) {
    closePresetNameDialog(nullptr);
    return;
  }

  const char* name = lv_textarea_get_text(preset_name_input);
  const uint8_t id = preset_name_id ? preset_name_id : firstFreePresetSlot();
  if (!id || !name || !name[0]) return;
  wled::savePreset(id, name);
  closePresetNameDialog(nullptr);
}

// Opens a compact text-entry dialog; the keyboard's checkmark saves the current state.
void openPresetNameDialog(uint8_t id = 0, const char* existingName = nullptr) {
  if (!wledControlsAvailable() || preset_name_dialog || (!id && !firstFreePresetSlot())) return;

  preset_name_id = id;
  preset_name_dialog = createDialogShell(id ? "Rename preset" : "Add preset", closePresetNameDialog);
  lv_obj_add_event_cb(preset_name_dialog, onPresetNameDialogDeleted, LV_EVENT_DELETE, nullptr);

  preset_name_input = lv_textarea_create(preset_name_dialog);
  lv_obj_set_size(preset_name_input, kDialogWidth, kDialogInputHeight);
  lv_obj_align(preset_name_input, LV_ALIGN_TOP_MID, 0, kDialogHeaderHeight + 6);
  lv_textarea_set_one_line(preset_name_input, true);
  lv_textarea_set_max_length(preset_name_input, 32);
  lv_textarea_set_placeholder_text(preset_name_input, "Preset name");
  if (existingName && existingName[0]) lv_textarea_set_text(preset_name_input, existingName);
  lv_obj_add_event_cb(preset_name_input, onPresetNameInput, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(preset_name_input, onPresetNameInput, LV_EVENT_CANCEL, nullptr);

  createDialogKeyboard(preset_name_dialog, preset_name_input);
}

void openNewPresetNameDialog(lv_event_t*) {
  openPresetNameDialog();
}

void openRenamePresetNameDialog(uint8_t id) {
  if (!wledControlsAvailable() || preset_name_dialog) return;
  const std::vector<wled::PresetInfo>& presets = wled::model().presets;
  const auto preset = std::find_if(presets.begin(), presets.end(), [id](const wled::PresetInfo& item) {
    return item.id == id;
  });
  if (id == 0 || preset == presets.end()) return;

  // WLED has no rename-only API: psave stores the active state under the
  // supplied name. Queue this preset first, then the save from the dialog;
  // transactions are processed FIFO, retaining the preset's saved state.
  wled::applyPreset(id);
  openPresetNameDialog(id, preset->name.c_str());
}


lv_obj_t* addQrCode(lv_obj_t* parent, const lv_img_dsc_t* src, lv_coord_t w, lv_coord_t h) {
  lv_obj_t* qr = lv_img_create(parent);
  lv_img_set_src(qr, src);
  // Keep the image at its native resolution. The P4's LVGL zoom path can
  // sample past this embedded bitmap, producing repeated QR tiles.
  lv_img_set_zoom(qr, LV_IMG_ZOOM_NONE);
  lv_img_set_antialias(qr, false);
  lv_obj_set_size(qr, w, h);
  lv_obj_set_style_outline_color(qr, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_outline_width(qr, 3, LV_PART_MAIN);
  lv_obj_set_style_outline_opa(qr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_outline_pad(qr, 0, LV_PART_MAIN);
  return qr;
}

void openHelpDialog(lv_event_t*) {
  if (help_dialog) {
    return;
  }

  lv_obj_t* content = beginInfoModal("Help");
  if (!content) return;

  if (kLargeScreen) {
    // Place the QR at the visual center, with the supporting labels anchored
    // above and below it instead of treating all three as a single flex group.
    lv_obj_set_layout(content, 0);

    lv_obj_t* instructions = lv_label_create(content);
    lv_obj_set_width(instructions, LV_PCT(100));
    lv_label_set_long_mode(instructions, LV_LABEL_LONG_WRAP);
    lv_label_set_text(instructions,
                      "Scan the code for setup help,\n"
                      "usage tips, and project instructions.");
    lv_obj_add_style(instructions, &style_label_muted, LV_PART_MAIN);
    lv_obj_set_style_text_font(instructions, UI_FONT_BIG, LV_PART_MAIN);
    lv_obj_set_style_text_align(instructions, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(instructions, LV_ALIGN_TOP_MID, 0, 28);

    lv_obj_t* version = lv_label_create(content);
    lv_obj_set_width(version, LV_PCT(100));
    lv_label_set_text_fmt(version,
                          "Firmware v%s\n"
                          "Copyright Figamore 2026",
                          kAppVersion);
    lv_obj_add_style(version, &style_label_muted, LV_PART_MAIN);
    lv_obj_set_style_text_font(version, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(version, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_obj_t* qr = addQrCode(content, &kHelpQrImage, kHelpQrWidth, kHelpQrHeight);
    lv_obj_center(qr);
    return;
  }

  lv_obj_t* text_col = lv_obj_create(content);
  lv_obj_remove_style_all(text_col);
  lv_obj_set_size(text_col, uiScaled(100, 200), LV_PCT(100));
  lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(text_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(text_col, 10, LV_PART_MAIN);
  lv_obj_clear_flag(text_col, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* instructions = lv_label_create(text_col);
  lv_obj_set_width(instructions, uiScaled(100, 200));
  lv_label_set_long_mode(instructions, LV_LABEL_LONG_WRAP);
  lv_label_set_text(instructions,
                    "Scan for\n"
                    "setup help,\n"
                    "usage tips,\n"
                    "and project\n"
                    "instructions.");
  lv_obj_add_style(instructions, &style_label_muted, LV_PART_MAIN);

  lv_obj_t* version = lv_label_create(text_col);
  lv_obj_set_width(version, uiScaled(100, 200));
  lv_label_set_long_mode(version, LV_LABEL_LONG_WRAP);
  lv_label_set_text_fmt(version,
                        "Firmware v%s\n"
                        "Copyright Figamore 2026",
                        kAppVersion);
  lv_obj_add_style(version, &style_label_muted, LV_PART_MAIN);
  lv_obj_set_style_text_font(version, UI_FONT_SMALL, LV_PART_MAIN);

  addQrCode(content, &kHelpQrImage, kHelpQrWidth, kHelpQrHeight);
}


void onFxSpeed(lv_event_t* event) {
  if (!wledControlsAvailable()) return;
  static uint32_t last_send_ms = 0;
  const int value = lv_slider_get_value(lv_event_get_target(event));
  if (fx_speed_value) lv_label_set_text_fmt(fx_speed_value, "%d", value);
  if (shouldSendSliderValue(lv_event_get_code(event), last_send_ms)) {
    wled::setEffectParams(value, -1);
  }
}

void onFxIntensity(lv_event_t* event) {
  if (!wledControlsAvailable()) return;
  static uint32_t last_send_ms = 0;
  const int value = lv_slider_get_value(lv_event_get_target(event));
  if (fx_intensity_value) lv_label_set_text_fmt(fx_intensity_value, "%d", value);
  if (shouldSendSliderValue(lv_event_get_code(event), last_send_ms)) {
    wled::setEffectParams(-1, value);
  }
}

void onFxCustom(lv_event_t* event) {
  if (!wledControlsAvailable()) return;
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

  const lv_coord_t segments = std::min<lv_coord_t>(width, uiScaled(36, 56));
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
  if (!wledControlsAvailable()) return;
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
  strip.y2 -= uiScaled(8, 10);
  strip.y1 = strip.y2 - (kPaletteStripHeight - 1);
  if (strip.x2 <= strip.x1 || strip.y2 <= strip.y1) return;
  drawPalettePreviewArea(dsc->draw_ctx, strip, static_cast<uint16_t>(palette_table_order[row]));
}

// Builds the palette list. The parent page scrolls around the static table.
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
  if (!wledControlsAvailable()) return;
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

  updateWledControlAvailability();

}

// ── Scrub strip ──────────────────────────────────────────────────────────────
// LVGL 8 scrollbars are display-only, and a ~190-row effect list is dozens of
// flings end to end.  Pressing in a strip along the right edge and dragging
// maps the finger's position onto the whole scroll range instead, like
// dragging a phone's scroll indicator.  A press in the strip without movement
// still behaves as a normal tap so the gear column keeps working.
constexpr lv_coord_t kScrubStripWidth = 18;
constexpr lv_coord_t kScrubMoveThreshold = 4;
lv_obj_t* scrub_target = nullptr;
bool scrub_moved = false;
lv_coord_t scrub_press_y = 0;

bool scrubConsumedGesture() { return scrub_target && scrub_moved; }

void onScrubStrip(lv_event_t* event) {
  lv_obj_t* obj = lv_event_get_target(event);
  lv_indev_t* indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t point;
  lv_indev_get_point(indev, &point);
  const lv_event_code_t code = lv_event_get_code(event);
  lv_area_t area;
  lv_obj_get_coords(obj, &area);
  if (code == LV_EVENT_PRESSED) {
    if (point.x < area.x2 - kScrubStripWidth) return;
    scrub_target = obj;
    scrub_moved = false;
    scrub_press_y = point.y;
    // Keep LVGL's own drag-to-scroll out of the way for this press.
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  } else if (code == LV_EVENT_PRESSING) {
    if (scrub_target != obj) return;
    if (!scrub_moved && LV_ABS(point.y - scrub_press_y) < kScrubMoveThreshold) return;
    scrub_moved = true;
    const lv_coord_t height = lv_area_get_height(&area);
    if (height <= 0) return;
    const lv_coord_t range = lv_obj_get_scroll_top(obj) + lv_obj_get_scroll_bottom(obj);
    lv_coord_t offset = point.y - area.y1;
    if (offset < 0) offset = 0;
    if (offset > height) offset = height;
    lv_obj_scroll_to_y(obj, int32_t(range) * offset / height, LV_ANIM_OFF);
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (scrub_target != obj) return;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    scrub_target = nullptr;
    scrub_moved = false;
  }
}

void attachScrubStrip(lv_obj_t* scrollable) {
  lv_obj_add_event_cb(scrollable, onScrubStrip, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(scrollable, onScrubStrip, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(scrollable, onScrubStrip, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(scrollable, onScrubStrip, LV_EVENT_PRESS_LOST, nullptr);
}

void onEffectTableClicked(lv_event_t* event) {
  if (!wledControlsAvailable()) return;
  // The table reports a cell "click" on release even after a scrub drag.
  if (scrubConsumedGesture()) return;
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
  lv_obj_set_size(row, LV_PCT(100), uiScaled(38, 58));
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* name_label = lv_label_create(row);
  lv_obj_set_width(name_label, uiScaled(148, 220));
  lv_obj_add_style(name_label, &style_label_muted, LV_PART_MAIN);
  lv_label_set_text(name_label, name);

  lv_obj_t* pill = lv_btn_create(row);
  styleButton(pill, checkable);
  lv_obj_set_size(pill, uiScaled(118, 190), uiScaled(34, 52));
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
  target_dialog_refresh_pending = true;
  lv_obj_del_async(target_dialog);
}

void checkColorWheelEditor() {
  if (!color_wheel_editor_created && color_wheel_panel) {
    color_wheel_editor_created = true;
    createColorWheelEditor(color_wheel_panel);
  }
}

void updateWledControlAvailability() {
  const bool available = wledControlsAvailable();
  setControlEnabled(power_button, available);
  setControlEnabled(brightness_slider, available);
  setControlEnabled(color_wheel, available);
  setControlEnabled(solid_color_button, available);
  setControlEnabled(palette_list_table, available);
  setControlEnabled(fx_table, available);
  setControlEnabled(fx_speed_slider, available);
  setControlEnabled(fx_intensity_slider, available);
  for (lv_obj_t* slider : fx_custom_slider) setControlEnabled(slider, available);
  setControlEnabled(preset_table, available);
  // Reapply the capacity rule when reconnecting; no connection always wins.
  setControlEnabled(add_preset_button, available && firstFreePresetSlot());
}

void createLiveTab(lv_obj_t* tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  // The larger portrait panel can keep palettes with the primary power controls.
  configurePageScroll(tab, WLED_BOARD == WLED_BOARD_JC4880P443);

  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100),
                  WLED_BOARD == WLED_BOARD_JC4880P443 ? LV_SIZE_CONTENT : kTabCardHeight);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);

  power_button = lv_btn_create(panel);
  styleButton(power_button, true);
  lv_obj_set_size(power_button, LV_PCT(100), uiScaled(58, 88));
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
  lv_obj_set_style_text_font(power_button_label, UI_FONT_BIG, LV_PART_MAIN);
  lv_obj_center(power_button_label);

  brightness_slider =
      createLabeledSlider(panel, nullptr, 1, 255, state.brightness, onBrightness, &brightness_label);
  lv_obj_t* brightness_row = lv_obj_get_parent(brightness_slider);
  lv_obj_set_style_pad_left(brightness_row, 0, LV_PART_MAIN);
  lv_obj_t* brightness_icon = createBrightnessIcon(brightness_row);
  lv_obj_move_to_index(brightness_icon, 0);
  lv_obj_set_width(brightness_label, uiScaled(44, 64));
  lv_label_set_long_mode(brightness_label, LV_LABEL_LONG_CLIP);
  lv_label_set_text_fmt(brightness_label, "%u%%", brightnessPercent(state.brightness));

  now_playing_label = lv_label_create(panel);
  lv_obj_set_width(now_playing_label, LV_PCT(100));
  lv_label_set_long_mode(now_playing_label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(now_playing_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_add_style(now_playing_label, &style_label_muted, LV_PART_MAIN);
  lv_obj_set_style_text_color(now_playing_label, lv_color_hex(kColorTextMuted), LV_PART_MAIN);
  lv_label_set_text(now_playing_label, "");

#if WLED_BOARD == WLED_BOARD_JC4880P443
  addLabel(panel, "Palettes", LV_PCT(100));
  palette_table_order = paletteDisplayOrder();
  palette_chooser_selected = wled::model().palette;
  palette_list_table = buildPaletteTable(panel, kScreenWidth - 52);
#endif

  updateWledControlAvailability();
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
    if (m.online && effect[0] && palette[0]) {
      lv_label_set_text_fmt(now_playing_label, "%s  •  %s", effect, palette);
    } else if (m.online && effect[0]) {
      lv_label_set_text(now_playing_label, effect);
    } else if (!m.online) {
      switch (wled::connectionStatus()) {
        case wled::ConnectionStatus::kNoConnection:
          lv_label_set_text(now_playing_label, "No Wi-Fi connection");
          break;
        case wled::ConnectionStatus::kSearching:
          lv_label_set_text(now_playing_label, "Searching for WLED devices...");
          break;
        case wled::ConnectionStatus::kNoDevicesFound:
          lv_label_set_text(now_playing_label, "Searching for WLED...");
          break;
        case wled::ConnectionStatus::kConnectionLost:
          lv_label_set_text(now_playing_label, "WLED connection lost");
          break;
        case wled::ConnectionStatus::kConnecting:
        case wled::ConnectionStatus::kReconnecting:
          lv_label_set_text(now_playing_label, "Connecting to WLED...");
          break;
        case wled::ConnectionStatus::kConnected:
          lv_label_set_text(now_playing_label, "");
          break;
      }
    } else {
      lv_label_set_text(now_playing_label, "");
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

void onPresetTableClicked(lv_event_t* event) {
  if (!wledControlsAvailable()) return;
  lv_obj_t* table = lv_event_get_target(event);
  uint16_t row = LV_TABLE_CELL_NONE;
  uint16_t col = LV_TABLE_CELL_NONE;
  lv_table_get_selected_cell(table, &row, &col);
  const std::vector<wled::PresetInfo>& presets = wled::model().presets;
  if (row == LV_TABLE_CELL_NONE || col == LV_TABLE_CELL_NONE || row >= presets.size()) return;

  const wled::PresetInfo& preset = presets[row];
  if (col == 1) {
    openRenamePresetNameDialog(preset.id);
  } else {
    selected_preset = preset.id;
    lv_obj_invalidate(table);
    wled::applyPreset(preset.id);
  }
}

void onPresetTableDrawPart(lv_event_t* event) {
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(event);
  if (!dsc || !lv_obj_draw_part_check_type(dsc, &lv_table_class, LV_TABLE_DRAW_PART_CELL)) return;

  const uint16_t row = dsc->id / 2;
  const uint16_t col = dsc->id % 2;
  const std::vector<wled::PresetInfo>& presets = wled::model().presets;
  if (row >= presets.size()) return;

  const bool selected = presets[row].id == selected_preset;
  if (dsc->rect_dsc) {
    dsc->rect_dsc->radius = 6;
    dsc->rect_dsc->border_width = 0;
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
  }
}

void refreshPresetTable() {
  if (!preset_table || !preset_empty_hint) return;
  const std::vector<wled::PresetInfo>& presets = wled::model().presets;
  if (presets.empty()) {
    // LVGL 8 creates a table with one empty cell. Shrinking it to zero rows
    // dereferences that null cell when LV_USE_USER_DATA is enabled.
    lv_label_set_text(preset_empty_hint, wled::online()
                                        ? "Loading presets from WLED…"
                                        : "No WLED controller reachable. Open Settings to check the connection.");
    lv_obj_clear_flag(preset_empty_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(preset_table, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_table_set_row_cnt(preset_table, presets.size());
    for (size_t row = 0; row < presets.size(); ++row) {
      const wled::PresetInfo& preset = presets[row];
      if (preset.name.empty()) lv_table_set_cell_value_fmt(preset_table, row, 0, "Preset %u", unsigned(preset.id));
      else lv_table_set_cell_value(preset_table, row, 0, preset.name.c_str());
      lv_table_add_cell_ctrl(preset_table, row, 0, LV_TABLE_CELL_CTRL_TEXT_CROP);
      lv_table_set_cell_value(preset_table, row, 1, LV_SYMBOL_EDIT);
    }
    lv_obj_add_flag(preset_empty_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(preset_table, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_invalidate(preset_table);
  updateWledControlAvailability();
}

void createPresetsTab(lv_obj_t* tab) {
  for (size_t i = 0; i < kPresetSlotCount; ++i) preset_buttons[i] = nullptr;
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(tab, 8, LV_PART_MAIN);
  // Keep the add action with the preset rows instead of reserving permanent
  // space for it at the top of the page.
  configurePageScroll(tab, true);
  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);
  preset_empty_hint = lv_label_create(panel);
  lv_obj_set_width(preset_empty_hint, LV_PCT(100));
  lv_label_set_long_mode(preset_empty_hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(preset_empty_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_add_style(preset_empty_hint, &style_label_muted, LV_PART_MAIN);
  preset_table = lv_table_create(panel);
  lv_obj_set_size(preset_table, LV_PCT(100), LV_SIZE_CONTENT);
  lv_table_set_col_cnt(preset_table, 2);
  const lv_coord_t edit_col_width = uiScaled(42, 62);
  lv_table_set_col_width(preset_table, 0, kScreenWidth - 52 - edit_col_width);
  lv_table_set_col_width(preset_table, 1, edit_col_width);
  lv_obj_set_scrollbar_mode(preset_table, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(preset_table, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                                     LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_add_flag(preset_table, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(preset_table, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(preset_table, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(preset_table, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(preset_table, lv_color_hex(kColorAccent), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(preset_table, LV_OPA_50, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(preset_table, 4, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(preset_table, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_left(preset_table, uiScaled(10, 14), LV_PART_ITEMS);
  lv_obj_set_style_pad_right(preset_table, 8, LV_PART_ITEMS);
  lv_obj_set_style_pad_top(preset_table, uiScaled(8, 14), LV_PART_ITEMS);
  lv_obj_set_style_pad_bottom(preset_table, uiScaled(8, 14), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(preset_table, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(preset_table, lv_color_hex(kColorText), LV_PART_ITEMS);
  lv_obj_set_style_border_width(preset_table, 0, LV_PART_ITEMS);
  lv_obj_add_event_cb(preset_table, onPresetTableClicked, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(preset_table, onPresetTableDrawPart, LV_EVENT_DRAW_PART_BEGIN, nullptr);
  add_preset_button = lv_btn_create(panel);
  styleButton(add_preset_button);
  lv_obj_set_size(add_preset_button, LV_PCT(100), uiScaled(38, 54));
  lv_obj_add_event_cb(add_preset_button, openNewPresetNameDialog, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* addLabel = lv_label_create(add_preset_button);
  lv_label_set_text(addLabel, LV_SYMBOL_PLUS "  Add preset");
  lv_obj_center(addLabel);
  refreshPresetTable();
}

void createColorsTab(lv_obj_t* tab) {
  lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
  configurePageScroll(tab, false);

#if WLED_BOARD == WLED_BOARD_JC4880P443
  // Palettes live below the Power controls on the large portrait display, so
  // Colors can be a direct, full-page color-wheel editor.
  color_wheel_panel = createPanel(tab);
  lv_obj_set_size(color_wheel_panel, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(color_wheel_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(color_wheel_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(color_wheel_panel, 2, LV_PART_MAIN);
#else
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
  palette_list_table = buildPaletteTable(palette_panel, kScreenWidth - 52);
  updateWledControlAvailability();

  lv_obj_t* wheel_page = lv_tabview_add_tab(color_tabs, "Color Wheel");
  lv_obj_set_style_pad_all(wheel_page, 2, LV_PART_MAIN);
  configurePageScroll(wheel_page, false);

  color_wheel_panel = createPanel(wheel_page);
  lv_obj_set_size(color_wheel_panel, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(color_wheel_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(color_wheel_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(color_wheel_panel, 2, LV_PART_MAIN);
#endif
}

void rebuildPresetTab() {
  if (presets_tab) refreshPresetTable();
}

void refreshPresetSelection() {
  if (preset_table) lv_obj_invalidate(preset_table);
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
  lv_obj_set_size(chips, LV_PCT(100), uiScaled(30, 48));
  lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
  for (uintptr_t i = 0; i < 4; ++i) {
    lv_obj_t* chip = lv_btn_create(chips);
    styleButton(chip, true);
    lv_obj_set_size(chip, uiScaled(66, 102), uiScaled(28, 46));
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
  // Fill the panel: screen minus tab/panel padding, borders, and scrollbar slack.
  const lv_coord_t gear_col_width = uiScaled(38, 60);
  lv_table_set_col_width(table, 0, kScreenWidth - 50 - gear_col_width);
  lv_table_set_col_width(table, 1, gear_col_width);
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
  lv_obj_set_style_pad_left(table, uiScaled(10, 14), LV_PART_ITEMS);
  lv_obj_set_style_pad_right(table, 8, LV_PART_ITEMS);
  lv_obj_set_style_pad_top(table, uiScaled(8, 14), LV_PART_ITEMS);
  lv_obj_set_style_pad_bottom(table, uiScaled(8, 14), LV_PART_ITEMS);
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
  attachScrubStrip(table);
  lv_obj_add_event_cb(table, onEffectTableDrawPart, LV_EVENT_DRAW_PART_BEGIN, nullptr);

  revealSelectedEffect(false, false);
  updateWledControlAvailability();
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
    scheduleFxTabRebuild();
    return;
  }

  lv_obj_update_layout(fx_table);
  // default font line + item pads
  const lv_coord_t rowHeight = lv_font_get_line_height(lv_font_default()) + 2 * uiScaled(8, 14);
  const size_t row = static_cast<size_t>(selected - fx_table_order.begin());
  lv_coord_t target = static_cast<lv_coord_t>(row) * rowHeight - uiScaled(48, 80);
  if (target < 0) target = 0;
  lv_obj_scroll_to_y(fx_table, target, animated ? LV_ANIM_ON : LV_ANIM_OFF);
}


static lv_timer_t* conn_refresh_timer = nullptr;

// ── Legacy in-app updater (kept out of the firmware build) ───────────────────
// Updates now use the restart-isolated helper below, so the normal controller
// screen and its large widgets are never resident while TLS is running.
#if 0

lv_obj_t* firmware_update_dialog = nullptr;
lv_obj_t* firmware_update_content = nullptr;
lv_obj_t* firmware_update_icon = nullptr;
lv_obj_t* firmware_update_spinner = nullptr;
lv_obj_t* firmware_update_message = nullptr;
lv_obj_t* firmware_update_notes = nullptr;
lv_obj_t* firmware_update_primary = nullptr;
lv_obj_t* firmware_update_secondary = nullptr;
lv_obj_t* firmware_update_primary_label = nullptr;
lv_obj_t* firmware_update_secondary_label = nullptr;
bool firmware_update_confirming = false;
updater::Snapshot firmware_update_seen;

void updateFirmwareDialog();

void onFirmwareUpdateDialogDeleted(lv_event_t*) {
  firmware_update_dialog = nullptr;
  firmware_update_content = nullptr;
  firmware_update_icon = nullptr;
  firmware_update_spinner = nullptr;
  firmware_update_message = nullptr;
  firmware_update_notes = nullptr;
  firmware_update_primary = nullptr;
  firmware_update_secondary = nullptr;
  firmware_update_primary_label = nullptr;
  firmware_update_secondary_label = nullptr;
  firmware_update_confirming = false;
  memset(&firmware_update_seen, 0, sizeof(firmware_update_seen));
}

void closeFirmwareUpdateDialog(lv_event_t*) {
  if (firmware_update_dialog && !updater::flashing()) lv_obj_del_async(firmware_update_dialog);
}

void onFirmwareUpdatePrimary(lv_event_t*) {
  const updater::Snapshot update = updater::snapshot();
  if (update.state == updater::State::kUpdateAvailable) {
    if (firmware_update_confirming) {
      firmware_update_confirming = false;
      updater::installAvailableUpdate();
    } else {
      firmware_update_confirming = true;
    }
  } else if (update.state == updater::State::kFailed) {
    // A failed install retains the verified release metadata, so it can retry
    // the download. Other errors repeat the lightweight release check.
    if (update.failure == updater::Failure::kDownload || update.failure == updater::Failure::kVerification ||
        update.failure == updater::Failure::kInstall || update.failure == updater::Failure::kUnsafe) {
      updater::installAvailableUpdate();
    } else {
      updater::checkForUpdates();
    }
  } else if (update.state == updater::State::kUpToDate) {
    closeFirmwareUpdateDialog(nullptr);
  }
  updateFirmwareDialog();
}

void onFirmwareUpdateSecondary(lv_event_t*) {
  if (firmware_update_confirming) {
    firmware_update_confirming = false;
    updateFirmwareDialog();
  } else {
    closeFirmwareUpdateDialog(nullptr);
  }
}

void setFirmwareAction(lv_obj_t* button, lv_obj_t* label, const char* text, bool visible, bool enabled = true) {
  if (!button || !label) return;
  lv_label_set_text(label, text);
  if (visible) lv_obj_clear_flag(button, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
  setControlEnabled(button, enabled);
}

void updateFirmwareDialog() {
  if (!firmware_update_dialog) return;
  const updater::Snapshot update = updater::snapshot();
  if (memcmp(&update, &firmware_update_seen, sizeof(update)) == 0 &&
      !(firmware_update_confirming && update.state == updater::State::kUpdateAvailable)) return;
  firmware_update_seen = update;

  const bool working = update.state == updater::State::kChecking || updater::flashing();
  if (firmware_update_spinner) {
    if (working) lv_obj_clear_flag(firmware_update_spinner, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(firmware_update_spinner, LV_OBJ_FLAG_HIDDEN);
  }
  if (firmware_update_icon) {
    if (update.state == updater::State::kUpToDate) {
      lv_label_set_text(firmware_update_icon, LV_SYMBOL_OK);
      lv_obj_set_style_text_color(firmware_update_icon, lv_color_hex(kColorOk), LV_PART_MAIN);
      lv_obj_clear_flag(firmware_update_icon, LV_OBJ_FLAG_HIDDEN);
    } else if (update.state == updater::State::kFailed) {
      lv_label_set_text(firmware_update_icon, LV_SYMBOL_WARNING);
      lv_obj_set_style_text_color(firmware_update_icon, lv_color_hex(kColorDanger), LV_PART_MAIN);
      lv_obj_clear_flag(firmware_update_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(firmware_update_icon, LV_OBJ_FLAG_HIDDEN);
    }
  }

  char message[420] = {};
  if (firmware_update_confirming) {
    snprintf(message, sizeof(message),
             "Install version %s now?\n\nKeep the remote powered and connected to Wi-Fi. Navigation will be disabled until it restarts.",
             update.available_version);
  } else if (update.state == updater::State::kUpdateAvailable) {
    snprintf(message, sizeof(message), "Installed: %s\nAvailable: %s\n\n%s", update.installed_version,
             update.available_version, update.message);
  } else if (update.state == updater::State::kDownloading) {
    snprintf(message, sizeof(message), "Downloading firmware... %u%%", unsigned(update.progress));
  } else {
    snprintf(message, sizeof(message), "%s", update.message);
  }
  if (firmware_update_message) lv_label_set_text(firmware_update_message, message);

  const bool show_notes = update.state == updater::State::kUpdateAvailable && !firmware_update_confirming &&
                          update.release_notes[0];
  if (firmware_update_notes) {
    if (show_notes) {
      char notes[sizeof(update.release_notes) + 24];
      snprintf(notes, sizeof(notes), "Release notes\n%s", update.release_notes);
      lv_label_set_text(firmware_update_notes, notes);
      lv_obj_clear_flag(firmware_update_notes, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(firmware_update_notes, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (firmware_update_confirming) {
    setFirmwareAction(firmware_update_primary, firmware_update_primary_label, "Install", true);
    setFirmwareAction(firmware_update_secondary, firmware_update_secondary_label, "Cancel", true);
  } else if (update.state == updater::State::kUpdateAvailable) {
    setFirmwareAction(firmware_update_primary, firmware_update_primary_label, "Install", true);
    setFirmwareAction(firmware_update_secondary, firmware_update_secondary_label, "Later", true);
  } else if (update.state == updater::State::kFailed) {
    setFirmwareAction(firmware_update_primary, firmware_update_primary_label, "Retry", true);
    setFirmwareAction(firmware_update_secondary, firmware_update_secondary_label, "Close", true);
  } else if (update.state == updater::State::kUpToDate) {
    setFirmwareAction(firmware_update_primary, firmware_update_primary_label, "Done", true);
    setFirmwareAction(firmware_update_secondary, firmware_update_secondary_label, "", false);
  } else {
    setFirmwareAction(firmware_update_primary, firmware_update_primary_label, "", false);
    setFirmwareAction(firmware_update_secondary, firmware_update_secondary_label, "", false);
  }

  // The dialog covers the entire input layer. During the flash its close
  // affordance is disabled as a second guard against navigation.
  lv_obj_t* header = lv_obj_get_child(firmware_update_dialog, 0);
  if (header) setControlEnabled(lv_obj_get_child(header, 1), !updater::flashing());
}

void openFirmwareUpdateDialog(lv_event_t*) {
  updatehelper::request();
  displayRestart();
  return;

  if (firmware_update_dialog) return;
  firmware_update_confirming = false;
  firmware_update_dialog = createDialogShell("Software Update", closeFirmwareUpdateDialog);
  lv_obj_add_event_cb(firmware_update_dialog, onFirmwareUpdateDialogDeleted, LV_EVENT_DELETE, nullptr);

  firmware_update_content = lv_obj_create(firmware_update_dialog);
  lv_obj_remove_style_all(firmware_update_content);
  lv_obj_set_size(firmware_update_content, kDialogWidth, kScreenHeight - kDialogHeaderHeight - 16);
  lv_obj_align(firmware_update_content, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_flex_flow(firmware_update_content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(firmware_update_content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(firmware_update_content, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_row(firmware_update_content, 8, LV_PART_MAIN);
  // Release notes can be longer than a CYD screen; retain the standard page
  // scrolling behaviour instead of truncating the useful part of a release.
  lv_obj_set_scroll_dir(firmware_update_content, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(firmware_update_content, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_add_flag(firmware_update_content, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR);

  firmware_update_icon = lv_label_create(firmware_update_content);
  lv_obj_set_style_text_font(firmware_update_icon, UI_FONT_BIG, LV_PART_MAIN);
  lv_obj_add_flag(firmware_update_icon, LV_OBJ_FLAG_HIDDEN);
  firmware_update_spinner = lv_spinner_create(firmware_update_content, 800, 70);
  lv_obj_set_size(firmware_update_spinner, uiScaled(38, 56), uiScaled(38, 56));
  firmware_update_message = lv_label_create(firmware_update_content);
  lv_obj_set_width(firmware_update_message, LV_PCT(100));
  lv_label_set_long_mode(firmware_update_message, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(firmware_update_message, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_add_style(firmware_update_message, &style_label_muted, LV_PART_MAIN);
  firmware_update_notes = lv_label_create(firmware_update_content);
  lv_obj_set_width(firmware_update_notes, LV_PCT(100));
  lv_label_set_long_mode(firmware_update_notes, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(firmware_update_notes, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_obj_add_style(firmware_update_notes, &style_label_muted, LV_PART_MAIN);

  lv_obj_t* actions = lv_obj_create(firmware_update_content);
  lv_obj_remove_style_all(actions);
  lv_obj_set_size(actions, LV_PCT(100), uiScaled(42, 60));
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  firmware_update_secondary = lv_btn_create(actions);
  styleButton(firmware_update_secondary);
  lv_obj_set_size(firmware_update_secondary, uiScaled(116, 190), uiScaled(38, 56));
  lv_obj_add_event_cb(firmware_update_secondary, onFirmwareUpdateSecondary, LV_EVENT_CLICKED, nullptr);
  firmware_update_secondary_label = lv_label_create(firmware_update_secondary);
  lv_obj_center(firmware_update_secondary_label);
  firmware_update_primary = lv_btn_create(actions);
  styleButton(firmware_update_primary);
  lv_obj_set_size(firmware_update_primary, uiScaled(116, 190), uiScaled(38, 56));
  lv_obj_add_event_cb(firmware_update_primary, onFirmwareUpdatePrimary, LV_EVENT_CLICKED, nullptr);
  firmware_update_primary_label = lv_label_create(firmware_update_primary);
  lv_obj_center(firmware_update_primary_label);

  updater::checkForUpdates();
  updateFirmwareDialog();
}

void updateFirmwareUpdateUi() {
  updateFirmwareDialog();
}
#endif

// ── Device firmware update hand-off ─────────────────────────────────────────

void openFirmwareUpdateDialog(lv_event_t*) {
  updatehelper::request();
  displayRestart();
}

void updateFirmwareUpdateUi() {}

// Wi-Fi and WLED state move on their own, so the panel polls rather than relying
// on the model revision, which only advances while a controller is answering.
// Only while the panel is actually on screen: nothing else reads these labels.
static void onConnRefreshTick(lv_timer_t*) {
  if (main_tabs && lv_tabview_get_tab_act(main_tabs) != kSettingsTabIndex) return;
  updateConnLabel();
  updateAccessPointLabel();
}

// Two lines say what the remote is talking to and over what link; the rows below
// are the way to change either, so neither fact needs repeating elsewhere.
void createConnectionPanel(lv_obj_t* tab) {
  lv_obj_t* panel = createPanel(tab);
  lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 6, LV_PART_MAIN);

  conn_label = lv_label_create(panel);
  lv_obj_set_width(conn_label, LV_PCT(100));
  lv_label_set_long_mode(conn_label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(conn_label, UI_FONT_HEADER, LV_PART_MAIN);

  conn_detail_label = lv_label_create(panel);
  lv_obj_set_width(conn_detail_label, LV_PCT(100));
  lv_label_set_long_mode(conn_detail_label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(conn_detail_label, UI_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(conn_detail_label, lv_color_hex(kColorTextMuted), LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(conn_detail_label, 4, LV_PART_MAIN);

  updateConnLabel();

  createSettingsRow(panel, "Controlling", openTargetDialog, false, &target_label);
  updateTargetLabel();

  createSettingsRow(panel, "Wi-Fi network", openWifiDialog, false, &wifi_label);
  updateWifiLabel();

  createSettingsRow(panel, "Mobile hotspot", openAccessPointDialog, false, &access_point_label);
  updateAccessPointLabel();

  if (!conn_refresh_timer) {
    conn_refresh_timer = lv_timer_create(onConnRefreshTick, 2000, nullptr);
  }
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

  addLabel(panel, "Device");

  createSettingsRow(panel, "Orientation", onFlipDisplay, false, &orientation_label);
  updateOrientationLabel();

  createSettingsRow(panel, "Inactivity", onToggleIdleAction, false, &idle_label);
  updateIdleLabel();

  addLabel(panel, "Software Update");
  lv_obj_t* update_version = lv_label_create(panel);
  lv_obj_set_width(update_version, LV_PCT(100));
  lv_label_set_text_fmt(update_version, "Current firmware: %s", kAppVersion);
  lv_obj_add_style(update_version, &style_label_muted, LV_PART_MAIN);

  lv_obj_t* check_update = lv_btn_create(panel);
  styleButton(check_update);
  lv_obj_set_size(check_update, LV_PCT(100), uiScaled(38, 56));
  lv_obj_add_event_cb(check_update, openFirmwareUpdateDialog, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* check_update_label = lv_label_create(check_update);
  lv_label_set_text(check_update_label, LV_SYMBOL_REFRESH "  Check for Updates");
  lv_obj_center(check_update_label);

#if WLED_BOARD == WLED_BOARD_JC4880P443
  lv_obj_t* c6_update_label = nullptr;
  lv_obj_t* c6_update = createSettingsRow(panel, "C6 Wi-Fi radio", openC6UpdateDialog, false,
                                           &c6_update_label);
  if (wifilink::hostedFirmwareUpdateAvailable()) {
    lv_label_set_text(c6_update_label, "Update");
  } else {
    lv_label_set_text(c6_update_label, "Current");
    lv_obj_add_state(c6_update, LV_STATE_DISABLED);
  }
#endif

  lv_obj_t* actions = lv_obj_create(panel);
  lv_obj_remove_style_all(actions);
  lv_obj_set_size(actions, LV_PCT(100), uiScaled(38, 58));
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* help = lv_btn_create(actions);
  styleButton(help);
  lv_obj_set_size(help, uiScaled(128, 200), uiScaled(36, 54));
  lv_obj_add_event_cb(help, openHelpDialog, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* help_label = lv_label_create(help);
  lv_label_set_text(help_label, LV_SYMBOL_LIST "  Help");
  lv_obj_center(help_label);

  lv_obj_t* power_action = lv_btn_create(actions);
  styleButton(power_action);
  lv_obj_set_size(power_action, uiScaled(128, 200), uiScaled(36, 54));
  lv_obj_t* power_action_label = lv_label_create(power_action);
#if WLED_CYD_ENABLE_SHUTDOWN
  lv_obj_add_event_cb(power_action, onShutdown, LV_EVENT_CLICKED, nullptr);
  lv_label_set_text(power_action_label, LV_SYMBOL_POWER "  Shutdown");
#else
  lv_obj_add_event_cb(power_action, onRestart, LV_EVENT_CLICKED, nullptr);
  lv_label_set_text(power_action_label, LV_SYMBOL_POWER "  Restart");
#endif
  lv_obj_center(power_action_label);
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
