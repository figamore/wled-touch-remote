#pragma once

#include <LovyanGFX.hpp>
#include "app_state.h"

#if WLED_TOUCH_SIMULATOR
#include <SDL.h>
#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>
#else
#include <Wire.h>
#endif

// ── LGFX driver class ────────────────────────────────────────────────────────

#if WLED_TOUCH_SIMULATOR
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_sdl panel_;

 public:
  LGFX() {
    auto cfg = panel_.config();
    cfg.panel_width = kScreenWidth;
    cfg.panel_height = kScreenHeight;
    cfg.memory_width = kScreenWidth;
    cfg.memory_height = kScreenHeight;
    cfg.offset_x = 0;
    cfg.offset_y = 0;
    cfg.offset_rotation = 0;
    panel_.config(cfg);
    panel_.setWindowTitle("WLED Touch Remote");
    panel_.setScaling(2, 2);
    setPanel(&panel_);
  }
};
#else
class LGFX : public lgfx::LGFX_Device {
 public:
  enum class HardwareProfile : uint8_t {
    kSt7789Cst816s,
    kIli9341Ft5x06,
    kIli9341Xpt2046,
    kSt7789Xpt2046,
  };

  struct ProfileInfo {
    HardwareProfile profile;
    uint8_t code;  // persisted CYD_PROFILE_* code
    const char* panel_name;
    const char* touch_name;
    const char* profile_name;
    uint8_t backlight_pin;
    bool is_ili9341;
    bool has_i2c_touch;
    bool supports_battery;
    uint8_t panel_offset_rotation;
    uint8_t touch_offset_rotation;
  };

  static const ProfileInfo* profileTable(uint8_t& count) {
    static constexpr ProfileInfo kProfiles[] = {
        {HardwareProfile::kSt7789Cst816s, CYD_PROFILE_ST7789_CST816S, "ST7789", "CST816S",
         "ST7789 + CST816S", CYD_TFT_BL, false, true, true, CYD_PANEL_OFFSET_ROTATION, CYD_TOUCH_OFFSET_ROTATION},
        {HardwareProfile::kIli9341Ft5x06, CYD_PROFILE_ILI9341_FT5X06, "ILI9341", "FT5x06",
         "ILI9341 + FT5x06", CYD_ALT_TFT_BL, true, true, true, CYD_PANEL_OFFSET_ROTATION, CYD_TOUCH_OFFSET_ROTATION},
        {HardwareProfile::kIli9341Xpt2046, CYD_PROFILE_ILI9341_XPT2046, "ILI9341", "XPT2046",
         "ILI9341 + XPT2046", CYD_RES_TFT_BL, true, false, false, CYD_RES_PANEL_OFFSET_ROTATION,
         CYD_RES_TOUCH_OFFSET_ROTATION},
        {HardwareProfile::kSt7789Xpt2046, CYD_PROFILE_ST7789_XPT2046, "ST7789", "XPT2046",
         "ST7789 + XPT2046", CYD_RES_TFT_BL, false, false, false, CYD_RES_ST7789_PANEL_OFFSET_ROTATION,
         CYD_RES_ST7789_TOUCH_OFFSET_ROTATION},
    };
    count = sizeof(kProfiles) / sizeof(kProfiles[0]);
    return kProfiles;
  }

  static const ProfileInfo& profileInfo(HardwareProfile profile) {
    uint8_t count = 0;
    return profileTable(count)[static_cast<uint8_t>(profile)];
  }

 private:
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Panel_ST7789 panel_st7789_;
  lgfx::Panel_ILI9341 panel_ili9341_;
  lgfx::Touch_CST816S touch_cst816s_;
  lgfx::Touch_FT5x06 touch_ft5x06_;
  lgfx::Touch_XPT2046 touch_xpt2046_;
  HardwareProfile profile_ = HardwareProfile::kSt7789Cst816s;
  bool profile_detected_ = false;

  struct TouchProfile {
    int16_t pin_int;
    int16_t pin_rst;
    uint8_t i2c_port;
    uint8_t i2c_addr;
  };

  static TouchProfile cst816sTouchProfile() {
    return {
        CYD_TOUCH_INT,
        CYD_TOUCH_RST,
        CYD_TOUCH_I2C_PORT,
        CYD_TOUCH_ADDR,
    };
  }

  static TouchProfile ft5x06TouchProfile() {
    return {
        CYD_ALT_TOUCH_INT,
        CYD_TOUCH_RST,
        CYD_ALT_TOUCH_I2C_PORT,
        CYD_ALT_TOUCH_ADDR,
    };
  }

  static TwoWire& i2cBus(uint8_t port) {
    return port == 1 ? Wire1 : Wire;
  }

  static void resetTouchPin(int16_t pin_rst, uint16_t settle_ms) {
    if (pin_rst < 0) {
      return;
    }
    pinMode(pin_rst, OUTPUT);
    digitalWrite(pin_rst, LOW);
    delay(10);
    digitalWrite(pin_rst, HIGH);
    delay(settle_ms);
  }

  static bool readI2cRegister(const TouchProfile& profile,
                              uint8_t reg,
                              uint8_t* data,
                              size_t length,
                              bool stop_after_write = false) {
    TwoWire& bus = i2cBus(profile.i2c_port);
    bus.end();
    delay(1);
    bus.begin(CYD_TOUCH_SDA, CYD_TOUCH_SCL, 400000);
    bus.beginTransmission(profile.i2c_addr);
    bus.write(reg);
    if (bus.endTransmission(stop_after_write) != 0) {
      bus.end();
      return false;
    }

    const uint8_t read = bus.requestFrom(static_cast<int>(profile.i2c_addr),
                                         static_cast<int>(length));
    if (read != length) {
      bus.end();
      return false;
    }

    for (size_t i = 0; i < length; ++i) {
      data[i] = bus.read();
    }
    bus.end();
    return true;
  }

  static bool hasNonZeroByte(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
      if (data[i] != 0x00 && data[i] != 0xFF) {
        return true;
      }
    }
    return false;
  }

  static bool probeFt5x06() {
    const TouchProfile profile = ft5x06TouchProfile();
    resetTouchPin(profile.pin_rst, 50);
    uint8_t data[6] = {};
    return readI2cRegister(profile, 0xA3, data, sizeof(data), true) && data[5] != 0x00 && data[5] != 0xFF;
  }

  static bool probeCst816s() {
    const TouchProfile profile = cst816sTouchProfile();
    resetTouchPin(profile.pin_rst, 50);
    uint8_t data[3] = {};
    return readI2cRegister(profile, 0xA7, data, sizeof(data), true) && hasNonZeroByte(data, sizeof(data));
  }

  static void prepareResistiveTouchPins() {
    Wire.end();
    Wire1.end();
    delay(1);

    pinMode(CYD_RES_TOUCH_CS, OUTPUT);
    digitalWrite(CYD_RES_TOUCH_CS, HIGH);

    if (CYD_RES_TOUCH_SPI_HOST < 0) {
      pinMode(CYD_RES_TOUCH_SCLK, OUTPUT);
      digitalWrite(CYD_RES_TOUCH_SCLK, LOW);
      pinMode(CYD_RES_TOUCH_MOSI, OUTPUT);
      digitalWrite(CYD_RES_TOUCH_MOSI, LOW);
      pinMode(CYD_RES_TOUCH_MISO, INPUT_PULLUP);
    }
  }

  static uint32_t readPanelRegister(lgfx::IBus& bus,
                                    uint_fast16_t cmd,
                                    uint8_t dummy_bits,
                                    uint8_t read_count) {
    constexpr uint_fast8_t kDataBits = 8;
    pinMode(CYD_TFT_CS, OUTPUT);
    digitalWrite(CYD_TFT_CS, HIGH);

    bus.beginTransaction();
    bus.writeCommand(0, kDataBits);
    bus.wait();

    digitalWrite(CYD_TFT_CS, LOW);
    bus.writeCommand(cmd, kDataBits);
    bus.beginRead(dummy_bits);

    uint32_t result = 0;
    for (uint8_t i = 0; i < read_count; ++i) {
      result |= ((bus.readData(kDataBits) >> (kDataBits - 8)) & 0xFF) << (i * 8);
    }

    bus.endTransaction();
    digitalWrite(CYD_TFT_CS, HIGH);
    return result;
  }

  static bool detectIli9341Panel() {
    lgfx::Bus_SPI detect_bus;
    auto cfg = detect_bus.config();
    cfg.spi_host = HSPI_HOST;
    cfg.spi_mode = 0;
    cfg.freq_write = 8000000;
    cfg.freq_read = 8000000;
    cfg.spi_3wire = false;
    cfg.use_lock = true;
    cfg.dma_channel = 0;
    cfg.pin_sclk = CYD_TFT_SCLK;
    cfg.pin_mosi = CYD_TFT_MOSI;
    cfg.pin_miso = CYD_TFT_MISO;
    cfg.pin_dc = CYD_TFT_DC;
    detect_bus.config(cfg);
    detect_bus.init();

    const uint32_t id1 = readPanelRegister(detect_bus, 0xDA, 0, 1);
    const uint32_t id = readPanelRegister(detect_bus, 0x04, 1, 4);
    detect_bus.release();

    // On these CYD families, ST7789 reports 0x85 in RDDID while ILI9341
    // reports 0x00 for RDID1.
    Serial.printf("Display panel probe: RDID1=0x%02lX RDDID=0x%08lX\n",
                  static_cast<unsigned long>(id1 & 0xFF),
                  static_cast<unsigned long>(id));
    if ((id & 0xFF) == 0x85) {
      return false;
    }
    return (id1 & 0xFF) == 0x00;
  }

  static HardwareProfile resistiveProfileForPanel() {
    return detectIli9341Panel() ? HardwareProfile::kIli9341Xpt2046 : HardwareProfile::kSt7789Xpt2046;
  }

  void configureBus() {
    auto cfg = bus_.config();
    cfg.spi_host = HSPI_HOST;
    cfg.spi_mode = 0;
    cfg.freq_write = 55000000;
    cfg.freq_read = 16000000;
    cfg.spi_3wire = false;
    cfg.use_lock = true;
    cfg.dma_channel = SPI_DMA_CH_AUTO;
    cfg.pin_sclk = CYD_TFT_SCLK;
    cfg.pin_mosi = CYD_TFT_MOSI;
    cfg.pin_miso = CYD_TFT_MISO;
    cfg.pin_dc = CYD_TFT_DC;
    bus_.config(cfg);
  }

  template <typename Panel>
  void configurePanel(Panel& panel) {
    auto cfg = panel.config();
    cfg.pin_cs = CYD_TFT_CS;
    cfg.pin_rst = CYD_TFT_RST;
    cfg.pin_busy = -1;
    cfg.panel_width = 240;
    cfg.panel_height = 320;
    cfg.memory_width = 240;
    cfg.memory_height = 320;
    cfg.offset_x = 0;
    cfg.offset_y = 0;
    cfg.offset_rotation = profileInfo(profile_).panel_offset_rotation;
    cfg.dummy_read_pixel = 8;
    cfg.dummy_read_bits = 1;
    cfg.readable = true;
    cfg.invert = CYD_PANEL_INVERT;
    cfg.rgb_order = CYD_PANEL_RGB_ORDER;
    cfg.dlen_16bit = false;
    cfg.bus_shared = false;
    panel.config(cfg);
  }

  void configureLight(uint8_t pin_bl) {
    auto cfg = light_.config();
    cfg.pin_bl = pin_bl;
    cfg.invert = CYD_BACKLIGHT_INVERT;
    cfg.freq = 44100;
    cfg.pwm_channel = 7;
    light_.config(cfg);
  }

  template <typename Touch>
  void configureTouch(Touch& touch, const TouchProfile& profile) {
    auto cfg = touch.config();
    cfg.x_min = 0;
    cfg.x_max = 240;
    cfg.y_min = 0;
    cfg.y_max = 320;
    cfg.pin_int = profile.pin_int;
    cfg.pin_rst = profile.pin_rst;
    cfg.bus_shared = false;
    cfg.offset_rotation = profileInfo(profile_).touch_offset_rotation;
    cfg.i2c_port = profile.i2c_port;
    cfg.i2c_addr = profile.i2c_addr;
    cfg.pin_sda = CYD_TOUCH_SDA;
    cfg.pin_scl = CYD_TOUCH_SCL;
    cfg.freq = 400000;
    touch.config(cfg);
  }

  bool detectFt5x06() {
    configureTouch(touch_ft5x06_, ft5x06TouchProfile());
    return touch_ft5x06_.init();
  }

  void configureResistiveTouch(HardwareProfile profile) {
    prepareResistiveTouchPins();

    auto cfg = touch_xpt2046_.config();
    cfg.x_min = CYD_RES_TOUCH_X_MIN;
    cfg.x_max = CYD_RES_TOUCH_X_MAX;
    cfg.y_min = CYD_RES_TOUCH_Y_MIN;
    cfg.y_max = CYD_RES_TOUCH_Y_MAX;
    cfg.pin_int = CYD_RES_TOUCH_INT;
    cfg.bus_shared = false;
    cfg.spi_host = CYD_RES_TOUCH_SPI_HOST;
    cfg.pin_sclk = CYD_RES_TOUCH_SCLK;
    cfg.pin_mosi = CYD_RES_TOUCH_MOSI;
    cfg.pin_miso = CYD_RES_TOUCH_MISO;
    cfg.pin_cs = CYD_RES_TOUCH_CS;
    cfg.offset_rotation = profileInfo(profile).touch_offset_rotation;
    touch_xpt2046_.config(cfg);
  }

  HardwareProfile detectProfile() {
    profile_detected_ = true;
#if CYD_HARDWARE_PROFILE == CYD_PROFILE_ST7789_CST816S
    return HardwareProfile::kSt7789Cst816s;
#elif CYD_HARDWARE_PROFILE == CYD_PROFILE_ILI9341_FT5X06
    return HardwareProfile::kIli9341Ft5x06;
#elif CYD_HARDWARE_PROFILE == CYD_PROFILE_ILI9341_XPT2046
    return HardwareProfile::kIli9341Xpt2046;
#elif CYD_HARDWARE_PROFILE == CYD_PROFILE_ST7789_XPT2046
    return HardwareProfile::kSt7789Xpt2046;
#else
    if (detectFt5x06()) {
      return HardwareProfile::kIli9341Ft5x06;
    }
    if (probeCst816s()) {
      return HardwareProfile::kSt7789Cst816s;
    }
    profile_detected_ = false;
    return resistiveProfileForPanel();
#endif
  }

  void applyProfile(HardwareProfile profile) {
    profile_ = profile;
    configureBus();

    if (profile_ == HardwareProfile::kIli9341Ft5x06) {
      configurePanel(panel_ili9341_);
      configureLight(profileInfo(profile_).backlight_pin);
      configureTouch(touch_ft5x06_, ft5x06TouchProfile());
      panel_ili9341_.setBus(&bus_);
      panel_ili9341_.setLight(&light_);
      panel_ili9341_.setTouch(&touch_ft5x06_);
      setPanel(&panel_ili9341_);
      return;
    }

    if (profile_ == HardwareProfile::kIli9341Xpt2046) {
      configurePanel(panel_ili9341_);
      configureLight(profileInfo(profile_).backlight_pin);
      configureResistiveTouch(profile_);
      panel_ili9341_.setBus(&bus_);
      panel_ili9341_.setLight(&light_);
      panel_ili9341_.setTouch(&touch_xpt2046_);
      setPanel(&panel_ili9341_);
      return;
    }

    if (profile_ == HardwareProfile::kSt7789Xpt2046) {
      configurePanel(panel_st7789_);
      configureLight(profileInfo(profile_).backlight_pin);
      configureResistiveTouch(profile_);
      panel_st7789_.setBus(&bus_);
      panel_st7789_.setLight(&light_);
      panel_st7789_.setTouch(&touch_xpt2046_);
      setPanel(&panel_st7789_);
      return;
    }

    configurePanel(panel_st7789_);
    configureLight(profileInfo(profile_).backlight_pin);
    configureTouch(touch_cst816s_, cst816sTouchProfile());
    panel_st7789_.setBus(&bus_);
    panel_st7789_.setLight(&light_);
    panel_st7789_.setTouch(&touch_cst816s_);
    setPanel(&panel_st7789_);
  }

 public:
  LGFX() {
    applyProfile(profile_);
  }

  void autoConfigure() {
    applyProfile(detectProfile());
  }

  void setHardwareProfile(HardwareProfile profile) {
    profile_detected_ = true;
    applyProfile(profile);
  }

  bool setTouchProfile(HardwareProfile profile) {
    if (profile == HardwareProfile::kIli9341Xpt2046 || profile == HardwareProfile::kSt7789Xpt2046) {
      configureResistiveTouch(profile);
      getPanel()->setTouch(&touch_xpt2046_);
    } else if (profile == HardwareProfile::kIli9341Ft5x06) {
      configureTouch(touch_ft5x06_, ft5x06TouchProfile());
      getPanel()->setTouch(&touch_ft5x06_);
    } else {
      configureTouch(touch_cst816s_, cst816sTouchProfile());
      getPanel()->setTouch(&touch_cst816s_);
    }
    const bool touch_init_ok = getPanel()->initTouch();
    Serial.printf("Hardware setup: touch driver init for %s: %s\n",
                  profileInfo(profile).profile_name,
                  touch_init_ok ? "ok" : "failed");
    return touch_init_ok;
  }

  uint8_t hardwareProfileCode() const {
    return profileInfo(profile_).code;
  }

  static bool hardwareProfileFromCode(uint8_t code, HardwareProfile& profile) {
    uint8_t count = 0;
    const ProfileInfo* table = profileTable(count);
    for (uint8_t i = 0; i < count; ++i) {
      if (table[i].code == code) {
        profile = table[i].profile;
        return true;
      }
    }
    return false;
  }

  HardwareProfile hardwareProfile() const {
    return profile_;
  }

  bool profileDetected() const {
    return profile_detected_;
  }

  bool hasI2cTouch() const {
    return profileInfo(profile_).has_i2c_touch;
  }

  bool isIli9341() const {
    return profileInfo(profile_).is_ili9341;
  }

  bool supportsBatteryMonitor() const {
    return profileInfo(profile_).supports_battery;
  }

  uint8_t backlightPin() const {
    return profileInfo(profile_).backlight_pin;
  }

  const char* panelName() const {
    return profileInfo(profile_).panel_name;
  }

  const char* touchName() const {
    return profileInfo(profile_).touch_name;
  }

  const char* profileName() const {
    return profileInfo(profile_).profile_name;
  }
};
#endif

// ── Display instance ─────────────────────────────────────────────────────────

extern LGFX gfx;

#if WLED_TOUCH_SIMULATOR
extern uint16_t sim_framebuffer[kScreenWidth * kScreenHeight];
#endif

// ── Display functions ─────────────────────────────────────────────────────────

void initDisplay();
bool displayHardwareReady();
void drawSplash();
void applyDisplayRotation();
void touchActivity();
void displayUpdateIdle(uint32_t now);
