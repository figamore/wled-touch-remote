#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Station-mode Wi-Fi for the WLED JSON API transport.

namespace wifilink {

enum class Status : uint8_t {
  kNoCredentials,
  kConnecting,
  kRetrying,
  kConnected,
  kBadPassword,
  kNotFound,
  kFailed,
  kConnectionLost,
};

struct ScanResult {
  std::string ssid;
  int8_t rssi;
  bool secured;
  uint8_t channel;
};

void begin();
void loop(uint32_t now_ms);

Status status();
const char* statusName(Status status);
bool connected();
// True only while a user-visible Wi-Fi operation is actively running.
bool busy();
std::string ipAddress();
// SSID of the network currently associated with the radio; empty when offline.
std::string connectedSsid();
// Signal strength of the current association in dBm; 0 when not connected.
int rssi();

// Saved credentials; ssid() is empty when the device has never successfully
// joined a network.
std::string ssid();
bool hasCredentials();
// SSID currently being joined. It may be a new, not-yet-saved network.
std::string attemptSsid();
void saveCredentials(const char* ssid, const char* password, uint8_t channel = 0);
// Restarts a connection attempt using the currently stored credentials. This
// is primarily for the setup UI after it has shown the result of a failed join.
void retryConnection();
// Ends the setup flow's wait-for-user state and lets the normal background
// reconnect ladder resume with the active credentials.
void resumeReconnect();
void forgetCredentials();

// Scans run asynchronously; results() is valid once scanning() goes false.
// Returns true only once the request is accepted. Returns false while the
// station is associating or when the radio cannot accept the request.
bool startScan();
bool scanning();
// A scan requested while the radio is associating starts automatically once
// the association has settled.
bool scanQueued();
// True after a scan has been started during this boot, including one that
// completed with zero visible networks.
bool hasScanned();
const std::vector<ScanResult>& results();

// ESP32-P4 boards use an ESP32-C6 as their Wi-Fi coprocessor.  These are
// false/no-op on the single-chip targets.
bool hostedFirmwareUpdateAvailable();
bool updateHostedFirmware();

}  // namespace wifilink
