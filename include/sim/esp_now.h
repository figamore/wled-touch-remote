#pragma once

#include <cstdint>
#include <cstring>

using esp_err_t = int;

constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;

enum esp_now_send_status_t {
  ESP_NOW_SEND_SUCCESS = 0,
  ESP_NOW_SEND_FAIL = 1,
};

enum wifi_interface_t {
  WIFI_IF_STA = 0,
};

struct esp_now_peer_info_t {
  uint8_t peer_addr[6] = {};
  uint8_t channel = 0;
  bool encrypt = false;
  wifi_interface_t ifidx = WIFI_IF_STA;
};

using esp_now_send_cb_t = void (*)(const uint8_t*, esp_now_send_status_t);

inline esp_now_send_cb_t& simEspNowCallback() {
  static esp_now_send_cb_t callback = nullptr;
  return callback;
}

inline esp_err_t esp_now_init() {
  return ESP_OK;
}

inline esp_err_t esp_now_register_send_cb(esp_now_send_cb_t callback) {
  simEspNowCallback() = callback;
  return ESP_OK;
}

inline bool esp_now_is_peer_exist(const uint8_t*) {
  return true;
}

inline esp_err_t esp_now_add_peer(const esp_now_peer_info_t*) {
  return ESP_OK;
}

inline esp_err_t esp_now_send(const uint8_t* peer_addr, const uint8_t*, size_t) {
  if (simEspNowCallback()) {
    simEspNowCallback()(peer_addr, ESP_NOW_SEND_SUCCESS);
  }
  return ESP_OK;
}
