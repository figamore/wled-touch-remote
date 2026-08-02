#include "display.h"

#include <Arduino.h>
#include <lvgl.h>

#include <cstring>

#include "generated/wled_logo_png.h"
#include "generated/version.h"
#include "wled_api.h"

#if !WLED_TOUCH_SIMULATOR
#include <SPI.h>
#include <Wire.h>
#endif

#if WLED_BOARD == WLED_BOARD_JC4880P443 && !WLED_TOUCH_SIMULATOR
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#endif

#if WLED_TOUCH_SIMULATOR
uint16_t sim_framebuffer[kScreenWidth * kScreenHeight] = {};
#endif

namespace {

lv_disp_draw_buf_t draw_buf;
#if WLED_BOARD == WLED_BOARD_JC4880P443
// A full-frame LVGL buffer lets the renderer work without 40-line tiles.  Keep
// the small pair only as a safe fallback if external RAM is unavailable.
lv_color_t* p4_full_draw_buf = nullptr;
lv_color_t p4_fallback_draw_buf_1[kScreenWidth * kLvglBufferLines];
lv_color_t p4_fallback_draw_buf_2[kScreenWidth * kLvglBufferLines];
#else
lv_color_t draw_buf_1[kScreenWidth * kLvglBufferLines];
#endif

bool display_hardware_ready = false;
bool display_idle_applied = false;
bool suppress_touch_until_release = false;
bool splash_visible = false;
uint32_t splash_started_ms = 0;
uint16_t splash_text_row[kScreenWidth] = {};
uint32_t last_touch_ms = 0;
uint32_t flush_ms_accum = 0;
// Keep Eco's off delay aligned with the configured inactivity interval.
constexpr uint32_t kEcoOffDelayMs = UI_DIM_AFTER_MS;
bool eco_off_delay_pending = false;
uint32_t eco_off_started_ms = 0;
bool eco_wake_hold_pending = false;
uint32_t eco_wake_started_ms = 0;

#if WLED_TOUCH_SIMULATOR
bool sim_touch_down = false;
int16_t sim_touch_x = 0;
int16_t sim_touch_y = 0;
#elif WLED_BOARD == WLED_BOARD_JC4880P443
esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
esp_lcd_panel_io_handle_t dsi_io = nullptr;
esp_lcd_panel_handle_t dpi_panel = nullptr;
esp_ldo_channel_handle_t dsi_ldo = nullptr;
uint16_t* dsi_framebuffer = nullptr;
uint8_t gt911_address = JC4880_TOUCH_ADDR;
#else
SPIClass panel_spi(HSPI);
SPIClass touch_spi(VSPI);
enum class CydProfile : uint8_t { kSt7789Cst816s, kIli9341Ft5x06, kIli9341Xpt2046, kSt7789Xpt2046 };
CydProfile cyd_profile = CydProfile::kSt7789Cst816s;
bool cyd_is_ili9341 = false;
uint8_t cyd_panel_offset_rotation = 0;
uint8_t cyd_touch_offset_rotation = 0;
uint8_t cyd_backlight_pin = CYD_TFT_BL;
#endif

void setBacklight(uint8_t pin, uint8_t brightness) {
#if WLED_TOUCH_SIMULATOR
  (void)pin;
  (void)brightness;
#else
  static bool attached = false;
  if (!attached) {
    ledcAttach(pin, 44100, 8);
    attached = true;
  }
  ledcWrite(pin, brightness);
#endif
}

#if !WLED_TOUCH_SIMULATOR && WLED_BOARD != WLED_BOARD_JC4880P443
void spiCommand(uint8_t command, const uint8_t* data = nullptr, size_t length = 0) {
  digitalWrite(CYD_TFT_CS, LOW);
  digitalWrite(CYD_TFT_DC, LOW);
  panel_spi.transfer(command);
  if (length) {
    digitalWrite(CYD_TFT_DC, HIGH);
    panel_spi.writeBytes(data, length);
  }
  digitalWrite(CYD_TFT_CS, HIGH);
}

void spiSetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  const uint8_t column[] = {uint8_t(x0 >> 8), uint8_t(x0), uint8_t(x1 >> 8), uint8_t(x1)};
  const uint8_t row[] = {uint8_t(y0 >> 8), uint8_t(y0), uint8_t(y1 >> 8), uint8_t(y1)};
  spiCommand(0x2A, column, sizeof(column));
  spiCommand(0x2B, row, sizeof(row));
  spiCommand(0x2C);
}

void initSpiPanel() {
  pinMode(CYD_TFT_CS, OUTPUT);
  pinMode(CYD_TFT_DC, OUTPUT);
  digitalWrite(CYD_TFT_CS, HIGH);
  panel_spi.begin(CYD_TFT_SCLK, CYD_TFT_MISO, CYD_TFT_MOSI, CYD_TFT_CS);
  panel_spi.beginTransaction(SPISettings(55000000, MSBFIRST, SPI_MODE0));
  if (CYD_TFT_RST >= 0) {
    pinMode(CYD_TFT_RST, OUTPUT);
    digitalWrite(CYD_TFT_RST, LOW);
    delay(20);
    digitalWrite(CYD_TFT_RST, HIGH);
    delay(120);
  }
  spiCommand(0x01); delay(150);
  spiCommand(0x11); delay(120);
  const uint8_t colmod[] = {0x55};
  spiCommand(0x3A, colmod, 1);
  const uint8_t madctl[] = {0x00};
  spiCommand(0x36, madctl, 1);
  if (!cyd_is_ili9341) {
    const uint8_t porch[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    spiCommand(0xB2, porch, sizeof(porch));
    const uint8_t gate[] = {0x35}; spiCommand(0xB7, gate, 1);
    const uint8_t vcom[] = {0x1F}; spiCommand(0xBB, vcom, 1);
    const uint8_t power[] = {0x2C}; spiCommand(0xC0, power, 1);
    const uint8_t vrh[] = {0x01}; spiCommand(0xC3, vrh, 1);
    const uint8_t vdvs[] = {0x0F}; spiCommand(0xC4, vdvs, 1);
    const uint8_t fr[] = {0x0F}; spiCommand(0xC6, fr, 1);
  }
  spiCommand(CYD_PANEL_INVERT ? 0x21 : 0x20);
  spiCommand(0x29);
  panel_spi.endTransaction();
}

bool i2cRead(uint8_t address, uint8_t reg, uint8_t* dst, size_t length) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(int(address), int(length)) != length) return false;
  for (size_t i = 0; i < length; ++i) dst[i] = Wire.read();
  return true;
}

bool probeI2c(uint8_t address, uint8_t reg) {
  uint8_t value = 0;
  return i2cRead(address, reg, &value, 1) && value != 0 && value != 0xFF;
}

// A resistive CYD has no I2C touch controller to identify the panel family.
// These are the same ID registers used by the previous LovyanGFX backend:
// ST7789 reports 0x85 as the first RDDID byte while ILI9341 reports zero for
// RDID1. Unknown/write-only panels preserve ILI9341 as the historical fallback.
bool detectIli9341Panel() {
  pinMode(CYD_TFT_CS, OUTPUT);
  pinMode(CYD_TFT_DC, OUTPUT);
  digitalWrite(CYD_TFT_CS, HIGH);
  panel_spi.begin(CYD_TFT_SCLK, CYD_TFT_MISO, CYD_TFT_MOSI, CYD_TFT_CS);
  panel_spi.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  const auto readRegister = [](uint8_t command, uint8_t dummy_bytes, uint8_t* out, size_t length) {
    digitalWrite(CYD_TFT_CS, LOW);
    digitalWrite(CYD_TFT_DC, LOW);
    panel_spi.transfer(command);
    digitalWrite(CYD_TFT_DC, HIGH);
    while (dummy_bytes--) panel_spi.transfer(0);
    for (size_t i = 0; i < length; ++i) out[i] = panel_spi.transfer(0);
    digitalWrite(CYD_TFT_CS, HIGH);
  };
  uint8_t rdid1 = 0xFF;
  uint8_t rddid[4] = {};
  readRegister(0xDA, 0, &rdid1, 1);
  readRegister(0x04, 1, rddid, sizeof(rddid));
  panel_spi.endTransaction();
  panel_spi.end();
  Serial.printf("Display panel probe: RDID1=0x%02X RDDID=0x%02X%02X%02X%02X\n",
                rdid1, rddid[0], rddid[1], rddid[2], rddid[3]);
  return rddid[0] != 0x85 && rdid1 == 0x00;
}

bool isResistiveCyd() {
  return cyd_profile == CydProfile::kIli9341Xpt2046 || cyd_profile == CydProfile::kSt7789Xpt2046;
}

void initXpt2046() {
  // Auto detection temporarily uses the XPT2046 pins for I2C. Release that
  // bus before assigning them to SPI so touch reads cannot be held by I2C.
  Wire.end();
  pinMode(CYD_RES_TOUCH_CS, OUTPUT);
  digitalWrite(CYD_RES_TOUCH_CS, HIGH);
  pinMode(CYD_RES_TOUCH_SCLK, OUTPUT);
  digitalWrite(CYD_RES_TOUCH_SCLK, LOW);
  pinMode(CYD_RES_TOUCH_MOSI, OUTPUT);
  digitalWrite(CYD_RES_TOUCH_MOSI, LOW);
  pinMode(CYD_RES_TOUCH_MISO, INPUT_PULLUP);
  touch_spi.begin(CYD_RES_TOUCH_SCLK, CYD_RES_TOUCH_MISO, CYD_RES_TOUCH_MOSI, CYD_RES_TOUCH_CS);
}

void chooseCydProfile() {
#if CYD_HARDWARE_PROFILE == CYD_PROFILE_ILI9341_FT5X06
  cyd_profile = CydProfile::kIli9341Ft5x06;
  Wire.begin(CYD_TOUCH_SDA, CYD_TOUCH_SCL, 400000);
#elif CYD_HARDWARE_PROFILE == CYD_PROFILE_ILI9341_XPT2046
  cyd_profile = CydProfile::kIli9341Xpt2046;
#elif CYD_HARDWARE_PROFILE == CYD_PROFILE_ST7789_XPT2046
  cyd_profile = CydProfile::kSt7789Xpt2046;
#elif CYD_HARDWARE_PROFILE == CYD_PROFILE_ST7789_CST816S
  cyd_profile = CydProfile::kSt7789Cst816s;
  Wire.begin(CYD_TOUCH_SDA, CYD_TOUCH_SCL, 400000);
#else
  Wire.begin(CYD_TOUCH_SDA, CYD_TOUCH_SCL, 400000);
  if (probeI2c(CYD_ALT_TOUCH_ADDR, 0xA3)) cyd_profile = CydProfile::kIli9341Ft5x06;
  else if (probeI2c(CYD_TOUCH_ADDR, 0xA7)) cyd_profile = CydProfile::kSt7789Cst816s;
  else cyd_profile = detectIli9341Panel() ? CydProfile::kIli9341Xpt2046 : CydProfile::kSt7789Xpt2046;
#endif
  cyd_is_ili9341 = cyd_profile == CydProfile::kIli9341Ft5x06 || cyd_profile == CydProfile::kIli9341Xpt2046;
  const bool resistive = isResistiveCyd();
  cyd_backlight_pin = resistive || cyd_profile == CydProfile::kIli9341Ft5x06 ? CYD_ALT_TFT_BL : CYD_TFT_BL;
  cyd_panel_offset_rotation = cyd_profile == CydProfile::kIli9341Xpt2046 ? CYD_RES_PANEL_OFFSET_ROTATION
      : cyd_profile == CydProfile::kSt7789Xpt2046 ? CYD_RES_ST7789_PANEL_OFFSET_ROTATION : CYD_PANEL_OFFSET_ROTATION;
  cyd_touch_offset_rotation = cyd_profile == CydProfile::kSt7789Xpt2046 ? CYD_RES_ST7789_TOUCH_OFFSET_ROTATION : 0;
  Serial.printf("Display profile: %s\n", cyd_is_ili9341 ? "ILI9341" : "ST7789");
}

uint16_t mapResistive(uint16_t raw, int min_value, int max_value, uint16_t upper) {
  const int value = constrain(int(raw), min_value < max_value ? min_value : max_value, min_value < max_value ? max_value : min_value);
  const int scaled = (value - min_value) * int(upper) / (max_value - min_value);
  return constrain(scaled, 0, int(upper));
}

bool readXpt2046(uint16_t& x, uint16_t& y) {
  touch_spi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CYD_RES_TOUCH_CS, LOW);
  auto read = [](uint8_t cmd) { touch_spi.transfer(cmd); return uint16_t(touch_spi.transfer(0) << 8 | touch_spi.transfer(0)) >> 3; };
  const uint16_t raw_x = read(0xD0);
  const uint16_t raw_y = read(0x90);
  digitalWrite(CYD_RES_TOUCH_CS, HIGH);
  touch_spi.endTransaction();
  if (raw_x < 50 || raw_y < 50) return false;
  x = mapResistive(raw_x, CYD_RES_TOUCH_X_MIN, CYD_RES_TOUCH_X_MAX, 239);
  y = mapResistive(raw_y, CYD_RES_TOUCH_Y_MIN, CYD_RES_TOUCH_Y_MAX, 319);
  return true;
}
#endif

#if WLED_BOARD == WLED_BOARD_JC4880P443 && !WLED_TOUCH_SIMULATOR
bool p4Write(uint8_t command, const uint8_t* data, size_t len) { return dsi_io && esp_lcd_panel_io_tx_param(dsi_io, command, data, len) == ESP_OK; }
bool gt911Read(uint8_t address, uint16_t reg, uint8_t* dst, size_t length) {
  Wire.beginTransmission(address);
  Wire.write(uint8_t(reg >> 8));
  Wire.write(uint8_t(reg));
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(int(address), int(length)) != length) return false;
  for (size_t i = 0; i < length; ++i) dst[i] = Wire.read();
  return true;
}

void gt911ClearStatus() {
  const uint8_t clear[] = {0x81, 0x4E, 0};
  Wire.beginTransmission(gt911_address);
  Wire.write(clear, sizeof(clear));
  Wire.endTransmission();
}

void cacheWriteback(void* address, size_t size) {
  constexpr uintptr_t kCacheLineBytes = 128;
  const uintptr_t first = reinterpret_cast<uintptr_t>(address) & ~(kCacheLineBytes - 1);
  const uintptr_t last = (reinterpret_cast<uintptr_t>(address) + size + kCacheLineBytes - 1) & ~(kCacheLineBytes - 1);
  esp_cache_msync(reinterpret_cast<void*>(first), last - first,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}

bool initP4Touch() {
  // GT911 needs a hardware reset before its I2C interface becomes reliable.
  // Its INT pin is not connected on this board, so probe both legal addresses.
  pinMode(JC4880_TOUCH_RST, OUTPUT);
  digitalWrite(JC4880_TOUCH_RST, LOW);
  delay(5);
  digitalWrite(JC4880_TOUCH_RST, HIGH);
  delay(50);
  Wire.end();
  Wire.begin(JC4880_TOUCH_SDA, JC4880_TOUCH_SCL, 400000);
  uint8_t status = 0;
  for (const uint8_t address : {uint8_t(JC4880_TOUCH_ADDR), uint8_t(0x14)}) {
    if (gt911Read(address, 0x814E, &status, 1)) {
      gt911_address = address;
      Serial.printf("GT911: found at 0x%02X\n", address);
      return true;
    }
  }
  Serial.println("GT911: not detected");
  return false;
}

bool initP4Panel() {
  esp_ldo_channel_config_t ldo = {};
  ldo.chan_id = JC4880_DSI_LDO_CHANNEL; ldo.voltage_mv = JC4880_DSI_LDO_MV;
  esp_lcd_dsi_bus_config_t bus = {};
  bus.bus_id = 0; bus.num_data_lanes = JC4880_DSI_LANES; bus.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT; bus.lane_bit_rate_mbps = JC4880_DSI_LANE_MBPS;
  esp_lcd_dbi_io_config_t dbi = {};
  dbi.virtual_channel = 0; dbi.lcd_cmd_bits = 8; dbi.lcd_param_bits = 8;
  if (esp_ldo_acquire_channel(&ldo, &dsi_ldo) != ESP_OK || esp_lcd_new_dsi_bus(&bus, &dsi_bus) != ESP_OK ||
      esp_lcd_new_panel_io_dbi(dsi_bus, &dbi, &dsi_io) != ESP_OK) return false;
  esp_lcd_dpi_panel_config_t dpi = {};
  dpi.virtual_channel = 0; dpi.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT; dpi.dpi_clock_freq_mhz = JC4880_DPI_CLOCK_MHZ;
  dpi.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565; dpi.num_fbs = 1;
  dpi.video_timing.h_size = JC4880_PANEL_WIDTH; dpi.video_timing.v_size = JC4880_PANEL_HEIGHT;
  dpi.video_timing.hsync_back_porch = JC4880_HSYNC_BACK_PORCH; dpi.video_timing.hsync_pulse_width = JC4880_HSYNC_PULSE_WIDTH; dpi.video_timing.hsync_front_porch = JC4880_HSYNC_FRONT_PORCH;
  dpi.video_timing.vsync_back_porch = JC4880_VSYNC_BACK_PORCH; dpi.video_timing.vsync_pulse_width = JC4880_VSYNC_PULSE_WIDTH; dpi.video_timing.vsync_front_porch = JC4880_VSYNC_FRONT_PORCH;
  if (esp_lcd_new_panel_dpi(dsi_bus, &dpi, &dpi_panel) != ESP_OK) return false;
  pinMode(JC4880_TFT_RST, OUTPUT); digitalWrite(JC4880_TFT_RST, LOW); delay(20); digitalWrite(JC4880_TFT_RST, HIGH); delay(120);
  // JC4880P443's ST7701S vendor sequence. This is intentionally kept here
  // beside the native DSI setup instead of hidden in a graphics library.
  auto cmd = [](uint8_t c, std::initializer_list<uint8_t> p) { return p4Write(c, p.begin(), p.size()); };
  cmd(0xFF,{0x77,0x01,0x00,0x00,0x13}); cmd(0xEF,{0x08});
  cmd(0xFF,{0x77,0x01,0x00,0x00,0x10}); cmd(0xC0,{0x63,0x00}); cmd(0xC1,{0x0D,0x02}); cmd(0xC2,{0x10,0x08}); cmd(0xCC,{0x10});
  cmd(0xB0,{0x80,0x09,0x53,0x0C,0xD0,0x07,0x0C,0x09,0x09,0x28,0x06,0xD4,0x13,0x69,0x2B,0x71});
  cmd(0xB1,{0x80,0x94,0x5A,0x10,0xD3,0x06,0x0A,0x08,0x08,0x25,0x03,0xD3,0x12,0x66,0x6A,0x0D});
  cmd(0xFF,{0x77,0x01,0x00,0x00,0x11}); cmd(0xB0,{0x5D}); cmd(0xB1,{0x58}); cmd(0xB2,{0x87}); cmd(0xB3,{0x80}); cmd(0xB5,{0x4E}); cmd(0xB7,{0x85}); cmd(0xB8,{0x21}); cmd(0xB9,{0x10,0x1F}); cmd(0xBB,{0x03}); cmd(0xBC,{0x00}); cmd(0xC1,{0x78}); cmd(0xC2,{0x78}); cmd(0xD0,{0x88});
  cmd(0xE0,{0x00,0x3A,0x02}); cmd(0xE1,{0x04,0xA0,0x00,0xA0,0x05,0xA0,0x00,0xA0,0x00,0x40,0x40}); cmd(0xE2,{0x30,0x00,0x40,0x40,0x32,0xA0,0x00,0xA0,0x00,0xA0,0x00,0xA0,0x00}); cmd(0xE3,{0x00,0x00,0x33,0x33}); cmd(0xE4,{0x44,0x44});
  cmd(0xE5,{0x09,0x2E,0xA0,0xA0,0x0B,0x30,0xA0,0xA0,0x05,0x2A,0xA0,0xA0,0x07,0x2C,0xA0,0xA0}); cmd(0xE6,{0x00,0x00,0x33,0x33}); cmd(0xE7,{0x44,0x44}); cmd(0xE8,{0x08,0x2D,0xA0,0xA0,0x0A,0x2F,0xA0,0xA0,0x04,0x29,0xA0,0xA0,0x06,0x2B,0xA0,0xA0}); cmd(0xEB,{0x00,0x00,0x4E,0x4E,0x00,0x00,0x00}); cmd(0xEC,{0x08,0x01});
  cmd(0xED,{0xB0,0x2B,0x98,0xA4,0x56,0x7F,0xFF,0xFF,0xFF,0xFF,0xF7,0x65,0x4A,0x89,0xB2,0x0B}); cmd(0xEF,{0x08,0x08,0x08,0x45,0x3F,0x54}); cmd(0xFF,{0x77,0x01,0x00,0x00,0x00});
  const uint8_t colmod[] = {0x55}; p4Write(0x3A,colmod,1); p4Write(0x11,nullptr,0); delay(120); p4Write(0x29,nullptr,0);
  if (esp_lcd_panel_init(dpi_panel) != ESP_OK || esp_lcd_dpi_panel_get_frame_buffer(dpi_panel, 1, reinterpret_cast<void**>(&dsi_framebuffer)) != ESP_OK) return false;
  initP4Touch();
  return dsi_framebuffer != nullptr;
}

bool readP4Touch(uint16_t& x, uint16_t& y) {
  uint8_t status = 0;
  if (!gt911Read(gt911_address, 0x814E, &status, 1)) return false;
  if (!(status & 0x80)) return false;
  if (!(status & 0x0F)) {
    gt911ClearStatus();
    return false;
  }
  uint8_t point[8] = {};
  if (!gt911Read(gt911_address, 0x8150, point, sizeof(point))) return false;
  gt911ClearStatus();
  // We explicitly start at 0x8150, where the first point's X low byte
  // lives. (The track ID is at 0x814F and is intentionally not read here.)
  x = point[0] | (uint16_t(point[1]) << 8);
  y = point[2] | (uint16_t(point[3]) << 8);
  return x < kScreenWidth && y < kScreenHeight;
}
#endif

void mapPhysicalToLogical(uint16_t physical_x, uint16_t physical_y, uint8_t rotation, int16_t& logical_x, int16_t& logical_y) {
#if WLED_TOUCH_SIMULATOR
  logical_x = physical_x;
  logical_y = physical_y;
#elif WLED_BOARD == WLED_BOARD_JC4880P443
  if (rotation == 2) { logical_x = kScreenWidth - 1 - physical_x; logical_y = kScreenHeight - 1 - physical_y; }
  else { logical_x = physical_x; logical_y = physical_y; }
#else
  const uint8_t r = rotation & 3;
  if (r == 1) { logical_x = physical_y; logical_y = 239 - physical_x; }
  else { logical_x = 319 - physical_y; logical_y = physical_x; }
#endif
}

enum SplashGlyph : uint8_t {
  kGlyphF, kGlyphI, kGlyphR, kGlyphM, kGlyphW, kGlyphA, kGlyphE, kGlyphV, kGlyphLowerV,
  kGlyph0, kGlyph1, kGlyph2, kGlyph3, kGlyph4, kGlyph5, kGlyph6, kGlyph7, kGlyph8, kGlyph9,
  kGlyphDot, kGlyphDash, kGlyphQuestion,
};

constexpr uint8_t kSplashGlyphRows[][7] = {
    {0x1F, 0x10, 0x1E, 0x10, 0x10, 0x10, 0x10},  // F
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},  // I
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},  // R
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},  // M
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x0A, 0x0A},  // W
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // A
    {0x1F, 0x10, 0x1E, 0x10, 0x10, 0x10, 0x1F},  // E
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},  // V
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04},  // v
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E},  // 5
    {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E},  // 9
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06},  // .
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},  // -
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},  // ?
};

uint8_t splashGlyphRow(char character, uint8_t row) {
  int glyph = kGlyphQuestion;
  switch (character) {
    case ' ': return 0;
    case 'F': glyph = kGlyphF; break;
    case 'I': glyph = kGlyphI; break;
    case 'R': glyph = kGlyphR; break;
    case 'M': glyph = kGlyphM; break;
    case 'W': glyph = kGlyphW; break;
    case 'A': glyph = kGlyphA; break;
    case 'E': glyph = kGlyphE; break;
    case 'V': glyph = kGlyphV; break;
    case 'v': glyph = kGlyphLowerV; break;
    case '0': glyph = kGlyph0; break;
    case '1': glyph = kGlyph1; break;
    case '2': glyph = kGlyph2; break;
    case '3': glyph = kGlyph3; break;
    case '4': glyph = kGlyph4; break;
    case '5': glyph = kGlyph5; break;
    case '6': glyph = kGlyph6; break;
    case '7': glyph = kGlyph7; break;
    case '8': glyph = kGlyph8; break;
    case '9': glyph = kGlyph9; break;
    case '.': glyph = kGlyphDot; break;
    case '-': glyph = kGlyphDash; break;
  }
  return kSplashGlyphRows[glyph][row];
}

void drawSplashTextRow(uint16_t x, uint16_t y, uint16_t width) {
#if WLED_TOUCH_SIMULATOR
  memcpy(sim_framebuffer + y * kScreenWidth + x, splash_text_row, width * sizeof(uint16_t));
#elif WLED_BOARD == WLED_BOARD_JC4880P443
  if (!display_flipped) {
    memcpy(dsi_framebuffer + y * kScreenWidth + x, splash_text_row, width * sizeof(uint16_t));
    cacheWriteback(dsi_framebuffer + y * kScreenWidth + x, width * sizeof(uint16_t));
  } else {
    uint16_t* dest = dsi_framebuffer + (kScreenHeight - 1 - y) * kScreenWidth + kScreenWidth - 1 - x;
    for (uint16_t i = 0; i < width; ++i) *dest-- = splash_text_row[i];
    const uint16_t physical_x = kScreenWidth - x - width;
    cacheWriteback(dsi_framebuffer + (kScreenHeight - 1 - y) * kScreenWidth + physical_x,
                   width * sizeof(uint16_t));
  }
#else
  panel_spi.beginTransaction(SPISettings(55000000, MSBFIRST, SPI_MODE0));
  spiSetWindow(x, y, x + width - 1, y);
  digitalWrite(CYD_TFT_CS, LOW); digitalWrite(CYD_TFT_DC, HIGH);
  panel_spi.writePixels(splash_text_row, width * sizeof(uint16_t));
  digitalWrite(CYD_TFT_CS, HIGH);
  panel_spi.endTransaction();
#endif
}

void drawSplashVersionLabel(uint16_t logo_y) {
  constexpr char kPrefix[] = "v";
  constexpr uint16_t kColor = 0x7C94;  // RGB565 equivalent of the muted UI text color.
  // Keep this as a caption beneath the logo rather than a competing title.
  const uint8_t scale = kLargeScreen ? 2 : 1;
  const uint16_t advance = 6 * scale;
  const size_t prefix_length = sizeof(kPrefix) - 1;
  const size_t version_length = strlen(kAppVersion);
  const size_t character_count = min(prefix_length + version_length, size_t((kScreenWidth + scale) / advance));
  if (character_count == 0) return;

  const uint16_t width = character_count * advance - scale;
  const uint16_t x = (kScreenWidth - width) / 2;
  const uint16_t height = 7 * scale;
  const uint16_t y = min<uint16_t>(logo_y + kWledLogoHeight + 12 * scale,
                                   kScreenHeight - height - 8 * scale);
  for (uint16_t row = 0; row < height; ++row) {
    memset(splash_text_row, 0, width * sizeof(uint16_t));
    for (size_t character_index = 0; character_index < character_count; ++character_index) {
      const char character = character_index < prefix_length ? kPrefix[character_index]
                                                               : kAppVersion[character_index - prefix_length];
      const uint8_t glyph_row = splashGlyphRow(character, row / scale);
      for (uint8_t glyph_x = 0; glyph_x < 5; ++glyph_x) {
        if (!(glyph_row & (1U << (4 - glyph_x)))) continue;
        const uint16_t pixel_x = character_index * advance + glyph_x * scale;
        for (uint8_t x_scale = 0; x_scale < scale; ++x_scale) splash_text_row[pixel_x + x_scale] = kColor;
      }
    }
    drawSplashTextRow(x, y + row, width);
  }
}

void drawDisplaySplash() {
  if (!display_hardware_ready || kWledLogoPixelCount != kWledLogoWidth * kWledLogoHeight ||
      kWledLogoWidth > kScreenWidth || kWledLogoHeight > kScreenHeight) return;

  displayClear(0);
  const uint16_t x0 = (kScreenWidth - kWledLogoWidth) / 2;
  const uint16_t y0 = (kScreenHeight - kWledLogoHeight) / 2;
#if WLED_TOUCH_SIMULATOR
  for (uint16_t y = 0; y < kWledLogoHeight; ++y) {
    memcpy(sim_framebuffer + (y0 + y) * kScreenWidth + x0,
           kWledLogoPixels + y * kWledLogoWidth, kWledLogoWidth * sizeof(uint16_t));
  }
#elif WLED_BOARD == WLED_BOARD_JC4880P443
  if (!display_flipped) {
    for (uint16_t y = 0; y < kWledLogoHeight; ++y) {
      memcpy(dsi_framebuffer + (y0 + y) * kScreenWidth + x0,
             kWledLogoPixels + y * kWledLogoWidth, kWledLogoWidth * sizeof(uint16_t));
    }
  } else {
    // The P4 panel uses a software framebuffer. Match flushDisplay's
    // 180-degree transform so direct-rendered boot art follows the UI.
    for (uint16_t y = 0; y < kWledLogoHeight; ++y) {
      const uint16_t* source = kWledLogoPixels + y * kWledLogoWidth;
      uint16_t* dest = dsi_framebuffer + (kScreenHeight - 1 - (y0 + y)) * kScreenWidth
                       + kScreenWidth - 1 - x0;
      for (uint16_t x = 0; x < kWledLogoWidth; ++x) *dest-- = *source++;
    }
  }
  cacheWriteback(dsi_framebuffer + y0 * kScreenWidth,
                 size_t(kWledLogoHeight) * kScreenWidth * sizeof(uint16_t));
#else
  panel_spi.beginTransaction(SPISettings(55000000, MSBFIRST, SPI_MODE0));
  spiSetWindow(x0, y0, x0 + kWledLogoWidth - 1, y0 + kWledLogoHeight - 1);
  digitalWrite(CYD_TFT_CS, LOW); digitalWrite(CYD_TFT_DC, HIGH);
  panel_spi.writePixels(kWledLogoPixels, kWledLogoPixelCount * sizeof(uint16_t));
  digitalWrite(CYD_TFT_CS, HIGH);
  panel_spi.endTransaction();
#endif
  drawSplashVersionLabel(y0);
  splash_started_ms = millis();
  splash_visible = true;
}

void flushDisplay(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
  if (!display_hardware_ready) { lv_disp_flush_ready(disp); return; }
  const int32_t width = area->x2 - area->x1 + 1;
  const int32_t height = area->y2 - area->y1 + 1;
  const uint32_t started = millis();
#if WLED_TOUCH_SIMULATOR
  for (int32_t y = 0; y < height; ++y) for (int32_t x = 0; x < width; ++x) sim_framebuffer[(area->y1 + y) * kScreenWidth + area->x1 + x] = color_p[y * width + x].full;
#elif WLED_BOARD == WLED_BOARD_JC4880P443
  const bool flip = display_flipped;
  if (!flip) {
    // LVGL supplies unflipped rows contiguously, so let the optimized memory
    // copy path handle them instead of doing a scalar store per pixel.
    for (int32_t y = 0; y < height; ++y) {
      memcpy(dsi_framebuffer + (area->y1 + y) * kScreenWidth + area->x1,
             color_p + y * width, size_t(width) * sizeof(*color_p));
    }
  } else {
    // A 180-degree transform has to reverse both axes. Keep the inner loop
    // pointer-only and unrolled: the previous form recalculated four offsets
    // for every pixel, which is visible during large flipped redraws.
    for (int32_t y = 0; y < height; ++y) {
      const lv_color_t* source = color_p + y * width;
      uint16_t* dest = dsi_framebuffer + (kScreenHeight - 1 - (area->y1 + y)) * kScreenWidth
                       + kScreenWidth - 1 - area->x1;
      int32_t remaining = width;
      while (remaining >= 8) {
        dest[0] = source[0].full; dest[-1] = source[1].full;
        dest[-2] = source[2].full; dest[-3] = source[3].full;
        dest[-4] = source[4].full; dest[-5] = source[5].full;
        dest[-6] = source[6].full; dest[-7] = source[7].full;
        source += 8;
        dest -= 8;
        remaining -= 8;
      }
      while (remaining--) *dest-- = source++->full;
    }
  }
  const uint16_t py0 = flip ? kScreenHeight - 1 - area->y2 : area->y1;
  cacheWriteback(dsi_framebuffer + py0 * kScreenWidth, size_t(height) * kScreenWidth * sizeof(uint16_t));
#else
  panel_spi.beginTransaction(SPISettings(55000000, MSBFIRST, SPI_MODE0));
  spiSetWindow(area->x1, area->y1, area->x2, area->y2);
  digitalWrite(CYD_TFT_CS, LOW); digitalWrite(CYD_TFT_DC, HIGH);
  // This API is specifically for RGB565 framebuffer data and performs the
  // ESP32 SPI packing in bulk. Do not pre-swap the LVGL words here.
  panel_spi.writePixels(color_p, size_t(width) * height * sizeof(*color_p));
  digitalWrite(CYD_TFT_CS, HIGH);
  panel_spi.endTransaction();
#endif
  flush_ms_accum += millis() - started;
  lv_disp_flush_ready(disp);
}

void readTouch(lv_indev_drv_t*, lv_indev_data_t* data) {
  uint16_t physical_x = 0, physical_y = 0;
  bool down = false;
#if WLED_TOUCH_SIMULATOR
  down = sim_touch_down; physical_x = sim_touch_x; physical_y = sim_touch_y;
#elif WLED_BOARD == WLED_BOARD_JC4880P443
  down = readP4Touch(physical_x, physical_y);
#else
  const bool resistive = isResistiveCyd();
  if (resistive) down = readXpt2046(physical_x, physical_y);
  else if (cyd_profile == CydProfile::kIli9341Ft5x06) { uint8_t p[5]; down = i2cRead(CYD_ALT_TOUCH_ADDR, 0x02, p, 5) && (p[0] & 0x0F); if (down) { physical_x = ((p[1]&0x0F)<<8)|p[2]; physical_y=((p[3]&0x0F)<<8)|p[4]; } }
  else {
    // CST816S begins its report at 0x02: count, X high/low, Y high/low.
    // This matches the controller's native read layout exactly.
    uint8_t p[5];
    down = i2cRead(CYD_TOUCH_ADDR, 0x02, p, sizeof(p)) && (p[0] & 0x0F);
    if (down) {
      physical_x = ((p[1] & 0x0F) << 8) | p[2];
      physical_y = ((p[3] & 0x0F) << 8) | p[4];
    }
  }
#endif
  if (down) {
#if WLED_TOUCH_SIMULATOR
    data->point.x = physical_x;
    data->point.y = physical_y;
#elif WLED_BOARD == WLED_BOARD_JC4880P443
    mapPhysicalToLogical(physical_x, physical_y, display_flipped ? 2 : 0, data->point.x, data->point.y);
#else
    mapPhysicalToLogical(physical_x, physical_y, (display_flipped ? WLED_DISPLAY_ROTATION_FLIPPED : WLED_DISPLAY_ROTATION) + cyd_touch_offset_rotation, data->point.x, data->point.y);
#endif
    if (!suppress_touch_until_release) touchActivity();
  } else suppress_touch_until_release = false;
  data->state = down && !suppress_touch_until_release ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}
}  // namespace

#if WLED_TOUCH_SIMULATOR
void simulatorSetTouch(bool down, int16_t x, int16_t y) { sim_touch_down = down; sim_touch_x = x; sim_touch_y = y; }
#endif

void touchActivity() {
  last_touch_ms = millis();
  const bool waking_from_backlight_off =
      display_idle_applied && (idle_mode == IdleMode::kOff || idle_mode == IdleMode::kEco);
  // Once Eco has switched the backlight off, a touch wakes it for the regular
  // inactivity period instead of immediately applying Eco's shorter off delay.
  if (idle_mode == IdleMode::kEco && (display_idle_applied || eco_wake_hold_pending)) {
    eco_wake_hold_pending = true;
    eco_wake_started_ms = last_touch_ms;
  }
  // A wake touch must be released before LVGL sees another press, preventing
  // the control under the user's finger from activating accidentally.
  if (waking_from_backlight_off) suppress_touch_until_release = true;
  display_idle_applied = false;
  displaySetBrightness(UI_ACTIVE_BRIGHTNESS);
}
void applyDisplayRotation() {
  suppress_touch_until_release = true;
#if !WLED_TOUCH_SIMULATOR && WLED_BOARD != WLED_BOARD_JC4880P443
  const uint8_t rotation = ((display_flipped ? WLED_DISPLAY_ROTATION_FLIPPED : WLED_DISPLAY_ROTATION) + cyd_panel_offset_rotation) & 3;
  // Match the controller's complete MADCTL rotation table. In particular,
  // the horizontal/vertical refresh-order bits matter on CYD ST7789/ILI9341
  // panels; omitting them produces the subtly wrong color scan seen after
  // replacing the previous panel backend.
  static constexpr uint8_t kMadctlRotation[] = {0x00, 0x64, 0xD4, 0xB0};
  const uint8_t madctl = kMadctlRotation[rotation] | (CYD_PANEL_RGB_ORDER ? 0x00 : 0x08);
  panel_spi.beginTransaction(SPISettings(55000000, MSBFIRST, SPI_MODE0));
  spiCommand(0x36, &madctl, 1);
  panel_spi.endTransaction();
#endif
}
void displaySetBrightness(uint8_t brightness) {
#if !WLED_TOUCH_SIMULATOR && WLED_BOARD == WLED_BOARD_JC4880P443
  setBacklight(JC4880_TFT_BL, brightness);
#elif !WLED_TOUCH_SIMULATOR
  setBacklight(cyd_backlight_pin, brightness);
#else
  (void)brightness;
#endif
}

void displayPrepareForBoot() {
#if !WLED_TOUCH_SIMULATOR && WLED_BOARD == WLED_BOARD_JC4880P443
  pinMode(JC4880_TFT_BL, OUTPUT);
  digitalWrite(JC4880_TFT_BL, LOW);
#elif !WLED_TOUCH_SIMULATOR
  // Auto detection has not selected the board profile yet, so hold both CYD
  // backlight possibilities inactive until a complete first frame is ready.
  pinMode(CYD_TFT_BL, OUTPUT);
  digitalWrite(CYD_TFT_BL, CYD_BACKLIGHT_INVERT ? HIGH : LOW);
  if (CYD_ALT_TFT_BL != CYD_TFT_BL) {
    pinMode(CYD_ALT_TFT_BL, OUTPUT);
    digitalWrite(CYD_ALT_TFT_BL, CYD_BACKLIGHT_INVERT ? HIGH : LOW);
  }
#endif
}

void displayRestart() {
  // Do not leave the illuminated panel showing its undefined reset contents
  // while the ESP32 restarts and rebuilds the first framebuffer.
  displaySetBrightness(0);
  ESP.restart();
}

void displayClear(uint16_t rgb565) {
  if (!display_hardware_ready) return;
#if WLED_TOUCH_SIMULATOR
  for (uint32_t i = 0; i < uint32_t(kScreenWidth) * kScreenHeight; ++i) sim_framebuffer[i] = rgb565;
#elif WLED_BOARD == WLED_BOARD_JC4880P443
  for (uint32_t i = 0; i < uint32_t(kScreenWidth) * kScreenHeight; ++i) dsi_framebuffer[i] = rgb565;
  cacheWriteback(dsi_framebuffer, size_t(kScreenWidth) * kScreenHeight * sizeof(uint16_t));
#else
  panel_spi.beginTransaction(SPISettings(55000000, MSBFIRST, SPI_MODE0));
  spiSetWindow(0, 0, kScreenWidth - 1, kScreenHeight - 1);
  digitalWrite(CYD_TFT_CS, LOW); digitalWrite(CYD_TFT_DC, HIGH);
  for (uint32_t i = 0; i < uint32_t(kScreenWidth) * kScreenHeight; ++i) panel_spi.write16(rgb565);
  digitalWrite(CYD_TFT_CS, HIGH);
  panel_spi.endTransaction();
#endif
}

void initDisplay() {
  Serial.println("Display init: native LVGL backend");
#if WLED_TOUCH_SIMULATOR
  display_hardware_ready = true;
#elif WLED_BOARD == WLED_BOARD_JC4880P443
  display_hardware_ready = initP4Panel();
#else
  chooseCydProfile();
  if (isResistiveCyd()) initXpt2046();
  initSpiPanel(); display_hardware_ready = true;
#endif
  applyDisplayRotation();
  if (UI_SPLASH_MS > 0) drawDisplaySplash();
  else displayClear();
  if (display_hardware_ready) displaySetBrightness(UI_ACTIVE_BRIGHTNESS);
  lv_init();
#if WLED_BOARD == WLED_BOARD_JC4880P443 && !WLED_TOUCH_SIMULATOR
  constexpr size_t p4_full_frame_pixels = size_t(kScreenWidth) * kScreenHeight;
  p4_full_draw_buf = static_cast<lv_color_t*>(
      heap_caps_malloc(p4_full_frame_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (p4_full_draw_buf) {
    lv_disp_draw_buf_init(&draw_buf, p4_full_draw_buf, nullptr, p4_full_frame_pixels);
    Serial.printf("LVGL: using one full-frame PSRAM buffer (%u bytes, %u bytes free)\n",
                  unsigned(p4_full_frame_pixels * sizeof(lv_color_t)),
                  unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  } else {
    Serial.println("LVGL: full-frame PSRAM buffer unavailable; using two 40-line buffers");
    lv_disp_draw_buf_init(&draw_buf, p4_fallback_draw_buf_1, p4_fallback_draw_buf_2,
                          kScreenWidth * kLvglBufferLines);
  }
#elif WLED_BOARD == WLED_BOARD_JC4880P443
  lv_disp_draw_buf_init(&draw_buf, p4_fallback_draw_buf_1, p4_fallback_draw_buf_2,
                        kScreenWidth * kLvglBufferLines);
#else
  lv_disp_draw_buf_init(&draw_buf, draw_buf_1, nullptr, kScreenWidth * kLvglBufferLines);
#endif
  static lv_disp_drv_t disp_drv; lv_disp_drv_init(&disp_drv); disp_drv.hor_res = kScreenWidth; disp_drv.ver_res = kScreenHeight; disp_drv.flush_cb = flushDisplay; disp_drv.draw_buf = &draw_buf; lv_disp_drv_register(&disp_drv);
  static lv_indev_drv_t indev_drv; lv_indev_drv_init(&indev_drv); indev_drv.type = LV_INDEV_TYPE_POINTER; indev_drv.read_cb = readTouch; indev_drv.scroll_limit = 6; indev_drv.scroll_throw = 8; lv_indev_drv_register(&indev_drv);
}

void finishDisplaySplash() {
  if (!splash_visible) return;
  while (millis() - splash_started_ms < UI_SPLASH_MS) delay(1);
  // Replace the direct-rendered logo in one synchronous handoff once LVGL's
  // first screen exists, avoiding an intervening black frame.
  lv_refr_now(nullptr);
  splash_visible = false;
}
bool displayHardwareReady() { return display_hardware_ready; }
bool displaySupportsBatteryMonitor() { return WLED_BOARD != WLED_BOARD_JC4880P443; }
uint32_t displayTakeFlushMs() { const uint32_t total = flush_ms_accum; flush_ms_accum = 0; return total; }
void displayUpdateIdle(uint32_t now) {
  // Always On deliberately bypasses the inactivity timer.  In particular, it
  // must not fall through to the generic dim path below.
  if (idle_mode == IdleMode::kAlwaysOn) {
    eco_off_delay_pending = false;
    eco_wake_hold_pending = false;
    return;
  }

  if (idle_mode == IdleMode::kEco) {
    // Eco restores full brightness as soon as WLED turns on. When it turns
    // off, keep the display visible for the inactivity interval before
    // powering the backlight down.
    if (wled::model().power) {
      eco_off_delay_pending = false;
      eco_wake_hold_pending = false;
      if (display_idle_applied) {
        display_idle_applied = false;
        displaySetBrightness(UI_ACTIVE_BRIGHTNESS);
      }
    } else if (eco_wake_hold_pending && now - eco_wake_started_ms < UI_DIM_AFTER_MS) {
      return;
    } else if (!eco_off_delay_pending) {
      eco_off_delay_pending = true;
      eco_wake_hold_pending = false;
      eco_off_started_ms = now;
    } else if (!display_idle_applied && now - eco_off_started_ms >= kEcoOffDelayMs) {
      eco_wake_hold_pending = false;
      display_idle_applied = true;
      displaySetBrightness(0);
    }
    return;
  }

  eco_off_delay_pending = false;
  eco_wake_hold_pending = false;
  if (!display_idle_applied && now - last_touch_ms > UI_DIM_AFTER_MS) {
    display_idle_applied = true;
    displaySetBrightness(idle_mode == IdleMode::kOff ? 0 : UI_IDLE_BRIGHTNESS);
  }
}
void displayReadLineRgb888(uint16_t y, uint8_t* rgb, uint16_t width) { for (uint16_t x = 0; x < width; ++x) { uint16_t p = 0;
#if WLED_TOUCH_SIMULATOR
  p = sim_framebuffer[y * kScreenWidth + x];
#elif WLED_BOARD == WLED_BOARD_JC4880P443
  p = dsi_framebuffer[y * kScreenWidth + x];
#endif
  rgb[x * 3] = ((p >> 11) & 31) * 255 / 31; rgb[x * 3 + 1] = ((p >> 5) & 63) * 255 / 63; rgb[x * 3 + 2] = (p & 31) * 255 / 31; } }
