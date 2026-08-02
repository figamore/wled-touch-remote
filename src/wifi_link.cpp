#include "wifi_link.h"
#include "hosted_firmware.h"

#include <Arduino.h>
#include <Preferences.h>

#include <algorithm>
#include <cstring>

#include "app_state.h"
#include "display.h"

#if !WLED_TOUCH_SIMULATOR
#include <WiFi.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#if WLED_BOARD == WLED_BOARD_JC4880P443
#include <esp32-hal-hosted.h>
#endif
#endif

namespace wifilink {
namespace {

constexpr uint32_t kConnectTimeoutMs = 8000;
constexpr uint32_t kDisconnectSettleMs = 500;
constexpr uint32_t kFastRetryDelayMs = 1000;
constexpr uint32_t kRetryDelayMs[] = {3000, 8000, 15000};
constexpr uint32_t kScanStartRetryDelayMs = 750;

Status g_status = Status::kNoCredentials;
// Active credentials drive the radio. A setup attempt is held here until the
// station receives an IP address; only then do we copy it into the saved set.
std::string g_ssid;
std::string g_password;
uint8_t g_channel = 0;
std::string g_savedSsid;
uint32_t g_attemptStarted = 0;
uint32_t g_retryAt = 0;
uint8_t g_failures = 0;
// A network selected in setup should report its result and wait for the user,
// instead of disappearing behind an automatic reconnect loop. Established
// connections and boot-time credentials retain their normal self-healing
// reconnect behaviour.
bool g_userInitiatedConnection = false;
bool g_waitingForUserDecision = false;
// A first WPA handshake regularly fails with AUTH_EXPIRE or a handshake
// timeout even when the password is right (busy AP, missed beacon, channel
// switch mid-handshake). A setup join must retry quietly before its failure
// is presented as a verdict.
constexpr uint8_t kSetupJoinRetries = 2;
uint8_t g_setupAttempts = 0;
std::vector<ScanResult> g_results;
bool g_scanning = false;
bool g_scanPending = false;
bool g_scanStartFailureLogged = false;
uint32_t g_scanRetryAt = 0;
bool g_hasScanned = false;

void loadCredentials() {
  char ssid[kMaxSsidLength + 1] = {};
  char password[kMaxWifiPassLength + 1] = {};
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    prefs.getString(kPrefsWifiSsidKey, ssid, sizeof(ssid));
    prefs.getString(kPrefsWifiPassKey, password, sizeof(password));
    g_channel = prefs.getUChar(kPrefsWifiChannelKey, 0);
    prefs.end();
  }
  g_savedSsid = ssid;
  g_ssid = g_savedSsid;
  g_password = password;
}

void persistActiveCredentials() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putString(kPrefsWifiSsidKey, g_ssid.c_str());
    prefs.putString(kPrefsWifiPassKey, g_password.c_str());
    prefs.putUChar(kPrefsWifiChannelKey, g_channel);
    prefs.end();
  }
  g_savedSsid = g_ssid;
  Serial.printf("[WIFI] credentials saved for \"%s\"\n", g_savedSsid.c_str());
}

#if !WLED_TOUCH_SIMULATOR
// The association failure reason is the only way to tell a wrong password from
// an out-of-range network; WiFi.status() reports both as a plain disconnect.
volatile uint8_t g_disconnectReason = 0;
bool g_eventsRegistered = false;
volatile bool g_hasIp = false;
bool g_restartPending = false;
volatile bool g_ignoreNextDisconnect = false;
uint32_t g_restartRequestedAt = 0;

// Link stats the UI displays.  Sampled here, on the same cadence the rest of
// this file already talks to the driver, so the read path stays a plain memory
// read: no driver call, no allocation, no matter how often the UI redraws.
constexpr uint32_t kStatsIntervalMs = 2000;
int8_t g_rssi = 0;
std::string g_ip;
std::string g_connectedSsid;
uint32_t g_statsSampledAt = 0;

void clearLinkStats() {
  g_rssi = 0;
  g_ip.clear();
  g_connectedSsid.clear();
}

Status statusForReason(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return Status::kBadPassword;
    case WIFI_REASON_NO_AP_FOUND:
      return Status::kNotFound;
    default:
      return Status::kFailed;
  }
}

// On ESP-Hosted boards every station control call is a synchronous RPC to the
// C6 coprocessor; issued from loop() each one stalls touch and rendering for
// the RPC round trip.  A dedicated worker owns those calls, and routing them
// all through one queue preserves connect/disconnect ordering.
enum class RadioJobKind : uint8_t { kConnect, kDisconnect, kStopScan, kStartScan, kHarvestScan, kConfigureConnected, kSampleStats };
constexpr size_t kMaxScanResults = 32;
struct RadioScanNetwork {
  char ssid[kMaxSsidLength + 1] = {};
  int8_t rssi = 0;
  bool secure = false;
  uint8_t channel = 0;
};
struct RadioJob {
  RadioJobKind kind = RadioJobKind::kDisconnect;
  char ssid[kMaxSsidLength + 1] = {};
  char password[kMaxWifiPassLength + 1] = {};
  uint8_t channel = 0;
  uint32_t connectionGeneration = 0;
  uint32_t connectionAttempt = 0;
};

struct RadioResult {
  RadioJobKind kind = RadioJobKind::kDisconnect;
  uint32_t connectionGeneration = 0;
  uint32_t connectionAttempt = 0;
  bool sleepDisabled = false;
  bool txPowerSet = false;
  int8_t txPower = 0;
  uint8_t channel = 0;
  int8_t rssi = 0;
  uint32_t rssiElapsedMs = 0;
  char ssid[kMaxSsidLength + 1] = {};
  char ip[16] = {};
  uint32_t hostedHostMajor = 0;
  uint32_t hostedHostMinor = 0;
  uint32_t hostedHostPatch = 0;
  uint32_t hostedC6Major = 0;
  uint32_t hostedC6Minor = 0;
  uint32_t hostedC6Patch = 0;
  int16_t scanCount = WIFI_SCAN_FAILED;
  uint8_t scanNetworkCount = 0;
  RadioScanNetwork scanNetworks[kMaxScanResults] = {};
};
QueueHandle_t g_radioJobs = nullptr;
QueueHandle_t g_radioResults = nullptr;
// Pressing Rescan invalidates any queued reconnect jobs.  A job already in a
// driver call cannot be preempted, but it is followed by the queued disconnect
// and scan; no additional reconnect can run ahead of the user's scan.
volatile uint32_t g_radioConnectionGeneration = 0;
volatile bool g_scanRadioJobPending = false;
volatile int8_t g_scanRadioResult = 0;  // 0 = pending, 1 = started, -1 = busy
bool g_scanHarvestPending = false;
uint32_t g_connectionAttempt = 0;
bool g_connectionConfigQueued = false;
bool g_statsJobPending = false;

// The ESP-Hosted SDIO transport to the C6 can wedge for good under sustained
// UDP load: every RPC then blocks until the 5 s esp-hosted timeout, all sends
// fail with ENOBUFS, and the disconnect event that would trigger a reconnect
// can never arrive over the dead link. A healthy link answers the RSSI RPC in
// milliseconds — even disconnected or congested — so several consecutive reads
// that blocked for seconds identify the wedge with no false positives. The
// framework cannot re-init the hosted transport in place (second init is
// broken in esp32-hal-hosted.c), so the one reliable recovery is a restart:
// boot re-resets the C6 over its reset GPIO and reconnects automatically.
constexpr uint32_t kHostedRpcTimeoutMs = 3000;
constexpr uint8_t kHostedRpcTimeoutLimit = 3;
uint8_t g_hostedRpcTimeouts = 0;

void runRadioJob(const RadioJob& job) {
  if (job.kind == RadioJobKind::kConnect) {
    if (job.connectionGeneration != g_radioConnectionGeneration) return;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    // Do not write configuration on every reconnect; the app owns its stored
    // credentials.  A remembered channel makes normal reboot reconnects quick.
    WiFi.persistent(false);
    WiFi.begin(job.ssid, job.password, job.channel);
  } else if (job.kind == RadioJobKind::kDisconnect) {
    WiFi.disconnect();
  } else if (job.kind == RadioJobKind::kStopScan) {
    esp_wifi_scan_stop();
    WiFi.scanDelete();
  } else if (job.kind == RadioJobKind::kStartScan) {
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    g_scanRadioResult = WiFi.scanNetworks(true) == WIFI_SCAN_FAILED ? -1 : 1;
    g_scanRadioJobPending = false;
  } else if (job.kind == RadioJobKind::kHarvestScan) {
    RadioResult result;
    result.kind = job.kind;
    result.scanCount = WiFi.scanComplete();
    if (result.scanCount >= 0) {
      for (int16_t i = 0; i < result.scanCount && result.scanNetworkCount < kMaxScanResults; ++i) {
        const String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;
        RadioScanNetwork& network = result.scanNetworks[result.scanNetworkCount++];
        snprintf(network.ssid, sizeof(network.ssid), "%s", ssid.c_str());
        network.rssi = int8_t(WiFi.RSSI(i));
        network.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        network.channel = uint8_t(WiFi.channel(i));
      }
      WiFi.scanDelete();
    }
    if (g_radioResults) xQueueSend(g_radioResults, &result, portMAX_DELAY);
  } else {
#if WLED_BOARD == WLED_BOARD_JC4880P443
    // Every call in this block crosses the ESP-Hosted transport to the C6.
    // Keep that round-trip out of the display/touch loop, especially while the
    // radio is still settling immediately after association.
    RadioResult result;
    result.kind = job.kind;
    result.connectionGeneration = job.connectionGeneration;
    result.connectionAttempt = job.connectionAttempt;
    if (job.kind == RadioJobKind::kConfigureConnected) {
      result.sleepDisabled = WiFi.setSleep(false);
      result.txPowerSet = WiFi.setTxPower(WIFI_POWER_19_5dBm);
      result.txPower = WiFi.getTxPower();
      hostedGetHostVersion(&result.hostedHostMajor, &result.hostedHostMinor, &result.hostedHostPatch);
      hostedGetSlaveVersion(&result.hostedC6Major, &result.hostedC6Minor, &result.hostedC6Patch);
      const String ssid = WiFi.SSID();
      snprintf(result.ssid, sizeof(result.ssid), "%s", ssid.c_str());
      result.channel = WiFi.channel();
    }
    const String ip = WiFi.localIP().toString();
    snprintf(result.ip, sizeof(result.ip), "%s", ip.c_str());
    const uint32_t rssiStarted = millis();
    result.rssi = WiFi.RSSI();
    result.rssiElapsedMs = millis() - rssiStarted;
    if (g_radioResults) xQueueSend(g_radioResults, &result, portMAX_DELAY);
#endif
  }
}

void radioJobTask(void*) {
  RadioJob job;
  for (;;) {
    if (xQueueReceive(g_radioJobs, &job, portMAX_DELAY) == pdTRUE) runRadioJob(job);
  }
}

bool submitRadioJob(const RadioJob& job) {
#if WLED_BOARD == WLED_BOARD_JC4880P443
  // Never let a full or wedged worker queue transfer a long C6 RPC back onto
  // the UI task. The state machine retries jobs naturally on its next pass.
  return g_radioJobs && xQueueSend(g_radioJobs, &job, 0) == pdTRUE;
#else
  runRadioJob(job);
  return true;
#endif
}

bool queueConnect() {
  RadioJob job;
  job.kind = RadioJobKind::kConnect;
  snprintf(job.ssid, sizeof(job.ssid), "%s", g_ssid.c_str());
  snprintf(job.password, sizeof(job.password), "%s", g_password.c_str());
  job.channel = g_channel;
  job.connectionGeneration = g_radioConnectionGeneration;
  return submitRadioJob(job);
}

bool queueDisconnect() {
  RadioJob job;
  job.kind = RadioJobKind::kDisconnect;
  return submitRadioJob(job);
}

bool queueStartScan() {
  RadioJob job;
  job.kind = RadioJobKind::kStartScan;
  return submitRadioJob(job);
}

bool queueStopScan() {
  RadioJob job;
  job.kind = RadioJobKind::kStopScan;
  return submitRadioJob(job);
}

bool queueHarvestScan() {
  RadioJob job;
  job.kind = RadioJobKind::kHarvestScan;
  return submitRadioJob(job);
}

#if WLED_BOARD == WLED_BOARD_JC4880P443
bool queueConnectedConfiguration() {
  RadioJob job;
  job.kind = RadioJobKind::kConfigureConnected;
  job.connectionGeneration = g_radioConnectionGeneration;
  job.connectionAttempt = g_connectionAttempt;
  return submitRadioJob(job);
}

bool queueStatsSample() {
  RadioJob job;
  job.kind = RadioJobKind::kSampleStats;
  job.connectionGeneration = g_radioConnectionGeneration;
  job.connectionAttempt = g_connectionAttempt;
  return submitRadioJob(job);
}

void processRadioResults(uint32_t now_ms) {
  RadioResult result;
  while (g_radioResults && xQueueReceive(g_radioResults, &result, 0) == pdTRUE) {
    if (result.kind == RadioJobKind::kHarvestScan) {
      g_scanHarvestPending = false;
      if (result.scanCount >= 0) {
        g_results.clear();
        for (uint8_t i = 0; i < result.scanNetworkCount; ++i) {
          const RadioScanNetwork& network = result.scanNetworks[i];
          g_results.push_back({network.ssid, network.rssi, network.secure, network.channel});
        }
        g_scanning = false;
        Serial.printf("[WIFI] scan found %u networks\n", unsigned(g_results.size()));
      } else if (result.scanCount != WIFI_SCAN_RUNNING) {
        g_scanning = false;
        Serial.println("[WIFI] scan failed");
      }
      continue;
    }
    if (result.connectionGeneration != g_radioConnectionGeneration ||
        result.connectionAttempt != g_connectionAttempt || !g_hasIp) {
      continue;  // Result belongs to an earlier association attempt.
    }
    if (result.kind == RadioJobKind::kConfigureConnected) {
      g_connectionConfigQueued = false;
      if (!result.sleepDisabled) {
        Serial.println("[WIFI] could not disable station sleep; multicast discovery may be unreliable");
      }
      if (!result.txPowerSet) {
        Serial.println("[WIFI] could not raise TX power");
      }
      Serial.printf("[WIFI] TX power %.2f dBm\n", double(result.txPower) * 0.25);
      Serial.printf("[WIFI] ESP-Hosted firmware: P4 host %lu.%lu.%lu, C6 coprocessor %lu.%lu.%lu%s\n",
                    static_cast<unsigned long>(result.hostedHostMajor),
                    static_cast<unsigned long>(result.hostedHostMinor),
                    static_cast<unsigned long>(result.hostedHostPatch),
                    static_cast<unsigned long>(result.hostedC6Major),
                    static_cast<unsigned long>(result.hostedC6Minor),
                    static_cast<unsigned long>(result.hostedC6Patch),
                    result.hostedHostMajor == result.hostedC6Major &&
                            result.hostedHostMinor == result.hostedC6Minor &&
                            result.hostedHostPatch == result.hostedC6Patch
                        ? ""
                        : " (version mismatch)");
      g_connectedSsid = result.ssid;
      if (result.channel && result.channel != g_channel) {
        g_channel = result.channel;
      }
      persistActiveCredentials();
      g_rssi = result.rssi;
      g_ip = result.ip;
      g_status = Status::kConnected;
      g_failures = 0;
      g_userInitiatedConnection = false;
      g_waitingForUserDecision = false;
      g_statsSampledAt = now_ms;
      Serial.printf("[WIFI] connected, ip=%s channel=%u rssi=%d\n",
                    g_ip.c_str(), unsigned(g_channel), int(g_rssi));
    } else if (result.kind == RadioJobKind::kSampleStats) {
      g_statsJobPending = false;
      if (result.rssi == 0 && result.rssiElapsedMs >= kHostedRpcTimeoutMs) {
        if (++g_hostedRpcTimeouts >= kHostedRpcTimeoutLimit) {
          Serial.println("[WIFI] C6 radio link unresponsive; restarting to reset the coprocessor");
          Serial.flush();
          // esp_restart()'s Wi-Fi shutdown handler would block on one more
          // doomed RPC (Req_WifiStop) before the reset; skip it.
          esp_unregister_shutdown_handler(reinterpret_cast<shutdown_handler_t>(esp_wifi_stop));
          displayRestart();
        }
      } else {
        g_hostedRpcTimeouts = 0;
        g_rssi = result.rssi;
        g_ip = result.ip;
      }
    }
  }
}
#endif

void startAttempt(uint32_t now_ms, bool retrying = false) {
  if (!g_eventsRegistered) {
    g_eventsRegistered = true;
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
      if (g_ignoreNextDisconnect) {
        g_ignoreNextDisconnect = false;
        return;
      }
      g_hasIp = false;
      g_disconnectReason = uint8_t(info.wifi_sta_disconnected.reason);
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
      g_hasIp = true;
    }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  }
  g_disconnectReason = 0;
  g_hasIp = false;
  ++g_connectionAttempt;
  g_connectionConfigQueued = false;
  g_statsJobPending = false;
  g_hostedRpcTimeouts = 0;
  g_attemptStarted = now_ms;
  g_status = retrying ? Status::kRetrying : Status::kConnecting;
  // A newly selected network takes precedence over an older setup scan.
  g_scanPending = false;
  if (g_scanning) {
    // scanDelete only frees completed scan results.  Stop an in-flight setup
    // scan explicitly so a credential submission can associate immediately.
#if WLED_BOARD == WLED_BOARD_JC4880P443
    queueStopScan();
    g_scanHarvestPending = false;
#else
    esp_wifi_scan_stop();
    WiFi.scanDelete();
#endif
    g_scanning = false;
    Serial.println("[WIFI] stopped setup scan to connect");
  }
  queueConnect();
  Serial.printf("[WIFI] connecting to \"%s\"%s\n", g_ssid.c_str(),
                g_channel ? " on remembered channel" : "");
}

void startQueuedScan(uint32_t now_ms) {
  if (!g_scanPending || g_scanning || int32_t(now_ms - g_scanRetryAt) < 0) return;
  // ESP32 cannot associate and scan at the same time.  Keep the request rather
  // than issuing a scan the driver will reject, then run it as soon as the
  // association reaches a stable state.
  if (g_restartPending || g_status == Status::kConnecting || g_status == Status::kRetrying) return;

#if WLED_BOARD == WLED_BOARD_JC4880P443
  if (g_scanRadioJobPending) return;
  if (g_scanRadioResult) {
    const int8_t result = g_scanRadioResult;
    g_scanRadioResult = 0;
    if (result > 0) {
      g_scanPending = false;
      g_scanStartFailureLogged = false;
      g_hasScanned = true;
      g_scanning = true;
      return;
    }
    g_scanRetryAt = now_ms + kScanStartRetryDelayMs;
    if (!g_scanStartFailureLogged) {
      g_scanStartFailureLogged = true;
      Serial.println("[WIFI] scan request busy; will retry");
    }
    return;
  }
  g_scanRadioResult = 0;
  g_scanRadioJobPending = true;
  if (!queueStartScan()) {
    g_scanRadioJobPending = false;
    g_scanRetryAt = now_ms + kScanStartRetryDelayMs;
  }
  return;
#else
  WiFi.mode(WIFI_STA);
  WiFi.scanDelete();
  if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) {
    g_scanRetryAt = now_ms + kScanStartRetryDelayMs;
    if (!g_scanStartFailureLogged) {
      g_scanStartFailureLogged = true;
      Serial.println("[WIFI] scan request busy; will retry");
    }
    return;
  }
  g_scanPending = false;
  g_scanStartFailureLogged = false;
  g_hasScanned = true;
  g_scanning = true;
#endif
}

uint32_t retryDelayMs() {
  if (g_failures == 0) return kFastRetryDelayMs;
  constexpr size_t kRetryDelayCount = sizeof(kRetryDelayMs) / sizeof(kRetryDelayMs[0]);
  const size_t index = std::min<size_t>(g_failures - 1, kRetryDelayCount - 1);
  return kRetryDelayMs[index];
}

void scheduleRetry(uint32_t now_ms, Status failure, uint8_t reason) {
  g_status = failure;
  if (g_userInitiatedConnection) {
    if (g_setupAttempts < kSetupJoinRetries) {
      ++g_setupAttempts;
      Serial.printf("[WIFI] association failed: %s (reason %u); setup retry %u of %u\n",
                    statusName(g_status), reason, unsigned(g_setupAttempts),
                    unsigned(kSetupJoinRetries));
      startAttempt(now_ms);
      return;
    }
    g_waitingForUserDecision = true;
    Serial.printf("[WIFI] association failed: %s (reason %u); awaiting setup action\n",
                  statusName(g_status), reason);
    return;
  }
  const uint32_t delayMs = retryDelayMs();
  if (g_failures != UINT8_MAX) ++g_failures;
  g_retryAt = now_ms + delayMs;
  Serial.printf("[WIFI] association failed: %s (reason %u); retrying in %lums\n",
                statusName(g_status), reason, static_cast<unsigned long>(delayMs));
}

void restartForCredentials(uint32_t now_ms) {
#if WLED_BOARD == WLED_BOARD_JC4880P443
  const bool needsDisconnect = g_hasIp || g_status == Status::kConnecting;
#else
  const bool needsDisconnect = WiFi.status() == WL_CONNECTED || g_status == Status::kConnecting;
#endif
  g_failures = 0;
  g_restartPending = false;
  // Show the new connection attempt immediately, even while the previous
  // association is being torn down in the background.
  g_status = Status::kConnecting;
  if (needsDisconnect) {
    // The driver reports a disconnect event asynchronously.  Do not let that
    // intentional event be mistaken for a failed password/AP attempt.
    g_ignoreNextDisconnect = true;
    g_restartPending = true;
    g_restartRequestedAt = now_ms;
    queueDisconnect();
    return;
  }
  startAttempt(now_ms);
}
#endif

}  // namespace

const char* statusName(Status status) {
  switch (status) {
    case Status::kNoCredentials: return "No Wi-Fi connection";
    case Status::kConnecting: return "Connecting to Wi-Fi...";
    case Status::kRetrying: return "Retrying Wi-Fi...";
    case Status::kConnected: return "Connected";
    case Status::kBadPassword: return "Wrong password";
    case Status::kNotFound: return "Network not found";
    case Status::kFailed: return "Connection failed";
    case Status::kConnectionLost: return "Connection lost";
  }
  return "Unknown";
}

void begin() {
  loadCredentials();
#if WLED_TOUCH_SIMULATOR
  if (g_ssid.empty()) {
    g_ssid = "Studio Wi-Fi";
    g_savedSsid = g_ssid;
    g_channel = 6;
  }
  g_status = Status::kConnected;
  Serial.printf("[WIFI] simulator connected to \"%s\", ip=%s\n",
                g_ssid.c_str(), ipAddress().c_str());
  return;
#endif
#if !WLED_TOUCH_SIMULATOR && WLED_BOARD == WLED_BOARD_JC4880P443
  g_radioJobs = xQueueCreate(8, sizeof(RadioJob));
  g_radioResults = xQueueCreate(4, sizeof(RadioResult));
  const bool radioTaskStarted = g_radioJobs && g_radioResults &&
                                xTaskCreate(radioJobTask, "wifiRadio", 6144, nullptr, 2, nullptr) == pdPASS;
  if (!radioTaskStarted) {
    if (g_radioJobs) vQueueDelete(g_radioJobs);
    if (g_radioResults) vQueueDelete(g_radioResults);
    g_radioJobs = nullptr;
    g_radioResults = nullptr;
    Serial.println("[WIFI] radio worker unavailable; Wi-Fi operations will retry asynchronously");
  }
#endif
  if (g_ssid.empty()) {
    g_status = Status::kNoCredentials;
    Serial.println("[WIFI] no credentials stored; open Settings -> Wi-Fi to connect");
    return;
  }
#if !WLED_TOUCH_SIMULATOR
  startAttempt(millis());
#endif
}

void loop(uint32_t now_ms) {
#if !WLED_TOUCH_SIMULATOR
#if WLED_BOARD == WLED_BOARD_JC4880P443
  processRadioResults(now_ms);
#endif
  if (g_restartPending) {
    // A disconnected station may not emit an event.  Waiting briefly handles
    // both that case and the normal asynchronous disconnect without racing it.
    if (!g_ignoreNextDisconnect || now_ms - g_restartRequestedAt >= kDisconnectSettleMs) {
      g_restartPending = false;
      g_ignoreNextDisconnect = false;
      startAttempt(now_ms);
    }
    return;
  }

  if (g_scanning) {
#if WLED_BOARD == WLED_BOARD_JC4880P443
    if (!g_scanHarvestPending) g_scanHarvestPending = queueHarvestScan();
#else
    const int16_t count = WiFi.scanComplete();
    if (count >= 0) {
      g_results.clear();
      for (int16_t i = 0; i < count; ++i) {
        // Hidden networks report an empty SSID and cannot be joined by name.
        if (WiFi.SSID(i).isEmpty()) continue;
        g_results.push_back({WiFi.SSID(i).c_str(), int8_t(WiFi.RSSI(i)),
                             WiFi.encryptionType(i) != WIFI_AUTH_OPEN,
                             uint8_t(WiFi.channel(i))});
      }
      WiFi.scanDelete();
      g_scanning = false;
      Serial.printf("[WIFI] scan found %u networks\n", unsigned(g_results.size()));
    } else if (count != WIFI_SCAN_RUNNING) {
      g_scanning = false;
      Serial.println("[WIFI] scan failed");
    }
#endif
  }

  startQueuedScan(now_ms);

  // A user-requested scan takes precedence over automatic reconnect.  In
  // particular, do not let a retry start another association between two
  // attempts to start the scan; that would keep the radio permanently busy
  // and prevent choosing a replacement network.
  if (g_scanPending) return;

  if (g_ssid.empty()) {
    g_status = Status::kNoCredentials;
    return;
  }

  // On ESP-Hosted, station status is already reported through the Wi-Fi event
  // callback. Querying it again from this loop would be another C6 RPC.
#if WLED_BOARD == WLED_BOARD_JC4880P443
  const bool up = g_hasIp;
#else
  const bool up = g_hasIp && WiFi.status() == WL_CONNECTED;
#endif
  if (up) {
#if WLED_BOARD == WLED_BOARD_JC4880P443
    if (g_status != Status::kConnected) {
      if (!g_connectionConfigQueued) {
        g_connectionConfigQueued = true;
        if (!queueConnectedConfiguration()) g_connectionConfigQueued = false;
      }
      return;
    }
    if (now_ms - g_statsSampledAt >= kStatsIntervalMs && !g_statsJobPending) {
      g_statsJobPending = queueStatsSample();
      if (g_statsJobPending) g_statsSampledAt = now_ms;
    }
    return;
#else
    if (g_status != Status::kConnected) {
      // Keep the station awake after association so multicast DNS traffic is
      // not filtered between DTIM beacons (the ESP32 equivalent of Android's
      // Wi-Fi multicast lock used by WLED Native).
      if (!WiFi.setSleep(false)) {
        Serial.println("[WIFI] could not disable station sleep; multicast discovery may be unreliable");
      }
      // Pin the radio to its maximum TX power. This strengthens only the
      // uplink (commands, TCP ACKs) — the Peek downlink's strength is set by
      // the AP, not the remote.
      if (!WiFi.setTxPower(WIFI_POWER_19_5dBm)) {
        Serial.println("[WIFI] could not raise TX power");
      }
      Serial.printf("[WIFI] TX power %.2f dBm\n", double(WiFi.getTxPower()) * 0.25);
      g_status = Status::kConnected;
      g_failures = 0;
      g_userInitiatedConnection = false;
      g_waitingForUserDecision = false;
      g_connectedSsid = WiFi.SSID().c_str();
      const uint8_t connectedChannel = WiFi.channel();
      if (connectedChannel && connectedChannel != g_channel) {
        g_channel = connectedChannel;
      }
      persistActiveCredentials();
      Serial.printf("[WIFI] connected, ip=%s channel=%u rssi=%d\n",
                    WiFi.localIP().toString().c_str(), unsigned(g_channel), WiFi.RSSI());
      g_statsSampledAt = now_ms - kStatsIntervalMs;  // sample immediately below
    }
    if (now_ms - g_statsSampledAt >= kStatsIntervalMs) {
      g_statsSampledAt = now_ms;
      g_rssi = WiFi.RSSI();
      g_ip = WiFi.localIP().toString().c_str();
    }
    return;
#endif
  }

  clearLinkStats();

  // A requested setup scan owns the radio until it completes.  Do not start a
  // retrying association in the same loop and force the scan to be cancelled.
  if (g_scanning) return;

  if (g_status == Status::kConnecting) {
    // Association failures normally arrive immediately.  Waiting for the full
    // timeout only obscures a bad password or unavailable AP.
    const uint8_t reason = g_disconnectReason;
    if (reason) {
      const Status failure = statusForReason(reason);
      // The router may have changed channel.  Retry a full-channel join once
      // before reporting the saved network as unavailable.
      if (failure == Status::kNotFound && g_channel) {
        Serial.println("[WIFI] remembered channel missed; retrying all channels");
        g_channel = 0;
        startAttempt(now_ms);
        return;
      }
      queueDisconnect();
      scheduleRetry(now_ms, failure, reason);
      return;
    }
    if (now_ms - g_attemptStarted > kConnectTimeoutMs) {
      queueDisconnect();
      scheduleRetry(now_ms, Status::kFailed, reason);
    }
    return;
  }

  // Covers both a failed attempt and a connection dropping after being up.
  const bool settled = g_status == Status::kFailed || g_status == Status::kBadPassword ||
                       g_status == Status::kNotFound || g_status == Status::kConnectionLost;
  if (!settled) {
    // This is the only path reached after an established station connection
    // disappears without a new association failure reason.
    scheduleRetry(now_ms, Status::kConnectionLost, 0);
    return;
  }
  if (g_waitingForUserDecision) return;
  if (int32_t(now_ms - g_retryAt) >= 0) startAttempt(now_ms, true);
#else
  (void)now_ms;
  g_status = g_ssid.empty() ? Status::kNoCredentials : Status::kConnected;
#endif
}

Status status() { return g_status; }
bool connected() { return g_status == Status::kConnected; }
bool busy() {
#if WLED_TOUCH_SIMULATOR
  return false;
#else
  return g_scanning || g_scanPending || g_restartPending || g_status == Status::kConnecting ||
         g_status == Status::kRetrying;
#endif
}

std::string ipAddress() {
#if WLED_TOUCH_SIMULATOR
  return connected() ? "192.168.1.50" : "";
#else
  return connected() ? g_ip : std::string();
#endif
}

std::string connectedSsid() {
#if WLED_TOUCH_SIMULATOR
  return connected() ? g_ssid : std::string();
#else
  return connected() ? g_connectedSsid : std::string();
#endif
}

int rssi() {
#if WLED_TOUCH_SIMULATOR
  return connected() ? -52 : 0;
#else
  return connected() ? g_rssi : 0;
#endif
}

std::string ssid() { return g_savedSsid; }
bool hasCredentials() { return !g_savedSsid.empty(); }
std::string attemptSsid() { return g_ssid; }

void saveCredentials(const char* ssid, const char* password, uint8_t channel) {
  if (!ssid) return;
  const char* requestedPassword = password ? password : "";
  const size_t ssidLength = strnlen(ssid, kMaxSsidLength + 1);
  const size_t passwordLength = strnlen(requestedPassword, kMaxWifiPassLength + 1);
  g_ssid.assign(ssid, std::min(ssidLength, kMaxSsidLength));
  g_password.assign(requestedPassword, std::min(passwordLength, kMaxWifiPassLength));
  g_channel = channel;
  if (ssidLength > kMaxSsidLength || passwordLength > kMaxWifiPassLength) {
    Serial.println("[WIFI] credentials truncated to supported length");
  }

  Serial.printf("[WIFI] trying credentials for \"%s\"\n", g_ssid.c_str());

  g_userInitiatedConnection = true;
  g_waitingForUserDecision = false;
  g_setupAttempts = 0;

#if !WLED_TOUCH_SIMULATOR
  restartForCredentials(millis());
#else
  persistActiveCredentials();
  g_status = Status::kConnected;
#endif
}

void retryConnection() {
  if (g_ssid.empty()) return;
  g_userInitiatedConnection = true;
  g_waitingForUserDecision = false;
  g_setupAttempts = 0;
#if !WLED_TOUCH_SIMULATOR
  restartForCredentials(millis());
#else
  g_status = Status::kConnected;
  g_userInitiatedConnection = false;
#endif
}

void resumeReconnect() {
  g_userInitiatedConnection = false;
  g_waitingForUserDecision = false;
  g_setupAttempts = 0;
#if !WLED_TOUCH_SIMULATOR
  if (!g_ssid.empty()) g_retryAt = millis();
#endif
}

void forgetCredentials() {
  g_ssid.clear();
  g_password.clear();
  g_channel = 0;
  g_savedSsid.clear();
  g_userInitiatedConnection = false;
  g_waitingForUserDecision = false;
  g_setupAttempts = 0;
#if !WLED_TOUCH_SIMULATOR
  g_restartPending = false;
#endif
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.remove(kPrefsWifiSsidKey);
    prefs.remove(kPrefsWifiPassKey);
    prefs.remove(kPrefsWifiChannelKey);
    prefs.end();
  }
#if !WLED_TOUCH_SIMULATOR
  queueDisconnect();
  clearLinkStats();
#endif
  g_status = Status::kNoCredentials;
}

bool startScan() {
  if (g_scanning || g_scanPending) return true;
#if WLED_TOUCH_SIMULATOR
  g_results = {{"Home-WiFi", -48, true, 6}, {"Home-WiFi-5G", -61, true, 11}, {"Guest", -77, false, 1}};
  g_hasScanned = true;
  return true;
#else
  if (g_restartPending || g_status == Status::kConnecting || g_status == Status::kRetrying) return false;
  const uint32_t now = millis();
  if (!connected()) {
    // Cancel a scheduled reconnect before scanning.  A queued radio job is a
    // real prerequisite for accepting this request on ESP-Hosted boards.
    g_radioConnectionGeneration = g_radioConnectionGeneration + 1;
    if (!queueDisconnect()) return false;
    g_hasIp = false;
    g_disconnectReason = 0;
    g_status = Status::kFailed;
    g_scanRetryAt = now + kDisconnectSettleMs;
  } else {
    g_scanRetryAt = now;
  }
  g_scanPending = true;
  g_scanStartFailureLogged = false;
  return true;
#endif
}

bool scanning() { return g_scanning; }
bool scanQueued() { return g_scanPending; }
bool hasScanned() { return g_hasScanned; }
const std::vector<ScanResult>& results() { return g_results; }

bool hostedFirmwareUpdateAvailable() {
#if !WLED_TOUCH_SIMULATOR && WLED_BOARD == WLED_BOARD_JC4880P443
  return hostedIsInitialized() && hostedHasUpdate();
#else
  return false;
#endif
}

bool updateHostedFirmware() {
#if !WLED_TOUCH_SIMULATOR && WLED_BOARD == WLED_BOARD_JC4880P443
  if (!hostedIsInitialized()) {
    Serial.println("[WIFI] C6 update unavailable: ESP-Hosted is not initialized");
    return false;
  }
  if (!hostedHasUpdate()) {
    Serial.println("[WIFI] C6 firmware already matches the P4 host");
    return false;
  }

  // This uses ESP-Hosted's official OTA transport, but feeds it the exact C6
  // image that shipped with this P4 runtime.  The old C6 therefore never has
  // to perform DNS or HTTPS before its own update can repair networking.
  const uint8_t* image = hostedfirmware::c6Image();
  const size_t imageSize = hostedfirmware::c6ImageSize();
  if (!image || imageSize == 0) {
    Serial.println("[WIFI] C6 update image is missing from this P4 build");
    return false;
  }

  Serial.printf("[WIFI] starting bundled C6 firmware update (%lu bytes)\n",
                static_cast<unsigned long>(imageSize));
  if (!hostedBeginUpdate()) {
    Serial.println("[WIFI] C6 update could not start");
    return false;
  }

  constexpr size_t kC6UpdateChunkSize = 2048;
  size_t written = 0;
  uint8_t nextProgress = 25;
  while (written < imageSize) {
    const size_t chunk = std::min(kC6UpdateChunkSize, imageSize - written);
    if (!hostedWriteUpdate(const_cast<uint8_t*>(image + written), chunk)) {
      Serial.printf("[WIFI] C6 update write failed after %lu of %lu bytes; restart before retrying\n",
                    static_cast<unsigned long>(written), static_cast<unsigned long>(imageSize));
      return false;
    }
    written += chunk;
    const uint8_t progress = static_cast<uint8_t>((written * 100U) / imageSize);
    if (progress >= nextProgress) {
      Serial.printf("[WIFI] C6 update %u%%\n", static_cast<unsigned>(progress));
      nextProgress = static_cast<uint8_t>(nextProgress + 25);
    }
    delay(1);
  }

  if (!hostedEndUpdate()) {
    Serial.println("[WIFI] C6 update finalization failed; restart before retrying");
    return false;
  }
  if (!hostedActivateUpdate()) {
    Serial.println("[WIFI] C6 update activation failed; restart before retrying");
    return false;
  }

  // Activation resets the C6, so restart the P4 immediately into a clean
  // ESP-Hosted session rather than leaving sockets bound to the old radio.
  Serial.println("[WIFI] C6 update activated; restarting the P4");
  Serial.flush();
  displayRestart();
  return true;  // displayRestart() does not return on hardware.
#else
  return false;
#endif
}

}  // namespace wifilink
