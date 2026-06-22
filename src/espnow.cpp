#include "espnow.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

namespace {

bool espnow_ready = false;
uint32_t sequence_id = 0;
volatile bool pending_status = false;
volatile uint8_t pending_status_code = static_cast<uint8_t>(StatusCode::kBoot);

void setStatusColor(lv_color_t) {}

void onEspNowSent(const uint8_t*, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    requestStatus(StatusCode::kOk);
  } else {
    requestStatus(StatusCode::kNoAck);
  }
}

}  // namespace

void requestStatus(StatusCode code) {
  pending_status_code = static_cast<uint8_t>(code);
  pending_status = true;
}

void applyPendingStatus() {
  if (!pending_status) {
    return;
  }

  const auto code = static_cast<StatusCode>(pending_status_code);
  pending_status = false;

  switch (code) {
    case StatusCode::kBoot:
      setStatusColor(lv_color_hex(kColorAccent));
      break;
    case StatusCode::kOffline:
      setStatusColor(lv_color_hex(kColorDanger));
      break;
    case StatusCode::kSent:
      setStatusColor(lv_color_hex(kColorOk));
      break;
    case StatusCode::kOk:
      setStatusColor(lv_color_hex(kColorOk));
      break;
    case StatusCode::kNoAck:
      setStatusColor(lv_color_hex(0xA78BFA));
      break;
    case StatusCode::kSendError:
      setStatusColor(lv_color_hex(kColorDanger));
      break;
    case StatusCode::kEspFail:
      setStatusColor(lv_color_hex(kColorDanger));
      break;
    case StatusCode::kPeerError:
      setStatusColor(lv_color_hex(kColorDanger));
      break;
    case StatusCode::kBroadcast:
      setStatusColor(lv_color_hex(kColorOk));
      break;
    case StatusCode::kReady:
      setStatusColor(lv_color_hex(kColorOk));
      break;
  }
}

esp_err_t sendWledTouchButtonRaw(uint8_t button_code) {
  if (!espnow_ready) {
    requestStatus(StatusCode::kOffline);
    return ESP_ERR_INVALID_STATE;
  }

  sequence_id++;
  WledTouchPacket packet = {
      static_cast<uint8_t>(button_code == kWledTouchButtonOn ? 0x91 : 0x81),
      {static_cast<uint8_t>(sequence_id),
       static_cast<uint8_t>(sequence_id >> 8),
       static_cast<uint8_t>(sequence_id >> 16),
       static_cast<uint8_t>(sequence_id >> 24)},
      0x20,
      button_code,
      0x01,
      90,
      0,
      0,
      0,
      0,
  };

  esp_err_t last_result = ESP_OK;

#if WLED_TOUCH_SCAN_CHANNELS
  for (uint8_t channel = 1; channel <= 13; channel++) {
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    last_result = esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    delay(3);
  }
  esp_wifi_set_channel(WLED_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
#else
  last_result = esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
#endif

  requestStatus(last_result == ESP_OK ? StatusCode::kSent : StatusCode::kSendError);
  Serial.printf("WledTouch button sent: %u result=%d\n", button_code, last_result);
  return last_result;
}

void sendWledTouchButton(uint8_t button_code) {
  sendWledTouchButtonRaw(button_code);
}

void sendPower(bool power) {
  sendWledTouchButton(power ? kWledTouchButtonOn : kWledTouchButtonOff);
}

void sendBrightnessDelta(int delta) {
  const uint8_t button = delta > 0 ? kWledTouchButtonBrightUp : kWledTouchButtonBrightDown;
  const uint8_t repeats = min<uint8_t>(6, max<uint8_t>(1, abs(delta) / 25));
  for (uint8_t i = 0; i < repeats; i++) {
    sendWledTouchButton(button);
    delay(10);
  }
}

void sendPreset(uint8_t preset) {
  const uint8_t max_preset = extended_mode ? kExtendedPresetCount : kBasicPresetCount;
  if (preset < 1 || preset > max_preset) {
    return;
  }
  sendWledTouchButton(kWledTouchButtonOne + preset - 1);
}

void initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (mac_label) {
    lv_label_set_text(mac_label, WiFi.macAddress().c_str());
  }

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(WLED_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    espnow_ready = false;
    requestStatus(StatusCode::kEspFail);
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(onEspNowSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kBroadcastMac, sizeof(kBroadcastMac));
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;

  if (!esp_now_is_peer_exist(peer.peer_addr)) {
    esp_err_t add_result = esp_now_add_peer(&peer);
    if (add_result != ESP_OK) {
      espnow_ready = false;
      requestStatus(StatusCode::kPeerError);
      Serial.printf("ESP-NOW peer add failed: %d\n", add_result);
      return;
    }
  }

  espnow_ready = true;
  requestStatus(StatusCode::kBroadcast);
  Serial.printf("CYD station MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("ESP-NOW channel: %u\n", WLED_ESPNOW_CHANNEL);
  Serial.println("Pair this MAC in WLED Config -> WiFi Setup -> ESP-NOW remote.");
}
