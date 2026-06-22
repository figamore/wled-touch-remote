#pragma once

#include "esp_now.h"

enum wifi_second_chan_t {
  WIFI_SECOND_CHAN_NONE = 0,
};

inline esp_err_t esp_wifi_set_promiscuous(bool) {
  return ESP_OK;
}

inline esp_err_t esp_wifi_set_channel(uint8_t, wifi_second_chan_t) {
  return ESP_OK;
}
