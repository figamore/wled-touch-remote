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

// Holds off the periodic ESP-Hosted status RPC and the unresponsive-link
// recovery restart it drives. An OTA install keeps the flash cache disabled for
// long stretches, which starves the SDIO host driver and makes those RPCs time
// out even though the C6 is healthy; restarting there would abort the install
// mid-write. This is a no-op on single-chip boards.
void suspendLinkWatchdog(bool suspended);

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

// Optional local Wi-Fi network for mobile WLED installations. It is a WPA2
// 2.4 GHz access point; WLED controllers join it directly and receive an IP
// address from the remote. Enabling it pauses station Wi-Fi until disabled.
bool accessPointEnabled();
bool accessPointActive();
std::string accessPointName();
std::string accessPointPassword();
// DHCP-assigned IPv4 addresses of clients currently joined to the remote's
// access point. This is empty while hotspot mode is inactive.
std::vector<uint32_t> accessPointClientAddresses();
// Returns false without changing the saved configuration when the SSID or
// WPA2 password is invalid.
bool setAccessPoint(bool enabled, const char* name, const char* password);

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
