#pragma once

#include <string>

enum wifi_mode_t {
  WIFI_STA = 1,
};

class WiFiClass {
 public:
  void mode(wifi_mode_t) {}
  void setSleep(bool) {}

  std::string macAddress() const {
    return "24:6F:28:AA:BB:CC";
  }
};

inline WiFiClass WiFi;
