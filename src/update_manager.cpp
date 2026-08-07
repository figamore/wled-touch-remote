#include "update_manager.h"

#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BatteryMonitor.h"
#include "app_config.h"
#include "display.h"
#include "wifi_link.h"
#include "generated/version.h"

#if !WLED_TOUCH_SIMULATOR
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>
#endif

namespace updater {
namespace {

// GitHub edge servers currently use the Sectigo/UserTrust, DigiCert, or Let's
// Encrypt (ISRG Root X1) chain. Release assets redirect to a separately served
// GitHub host, so keep all three roots to prevent DNS and redirect routing from
// breaking OTA verification. The updater never uses an insecure TLS mode;
// refresh these if GitHub changes CAs.
constexpr char kGithubRootCa[] = R"pem(-----BEGIN CERTIFICATE-----
MIICjzCCAhWgAwIBAgIQXIuZxVqUxdJxVt7NiYDMJjAKBggqhkjOPQQDAzCBiDEL
MAkGA1UEBhMCVVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNl
eSBDaXR5MR4wHAYDVQQKExVUaGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMT
JVVTRVJUcnVzdCBFQ0MgQ2VydGlmaWNhdGlvbiBBdXRob3JpdHkwHhcNMTAwMjAx
MDAwMDAwWhcNMzgwMTE4MjM1OTU5WjCBiDELMAkGA1UEBhMCVVMxEzARBgNVBAgT
Ck5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNleSBDaXR5MR4wHAYDVQQKExVUaGUg
VVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMTJVVTRVJUcnVzdCBFQ0MgQ2VydGlm
aWNhdGlvbiBBdXRob3JpdHkwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAAQarFRaqflo
I+d61SRvU8Za2EurxtW20eZzca7dnNYMYf3boIkDuAUU7FfO7l0/4iGzzvfUinng
o4N+LZfQYcTxmdwlkWOrfzCjtHDix6EznPO/LlxTsV+zfTJ/ijTjeXmjQjBAMB0G
A1UdDgQWBBQ64QmG1M8ZwpZ2dEl23OA1xmNjmjAOBgNVHQ8BAf8EBAMCAQYwDwYD
VR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAwNoADBlAjA2Z6EWCNzklwBBHU6+4WMB
zzuqQhFkoJ2UOQIReVx7Hfpkue4WQrO/isIJxOzksU0CMQDpKmFHjFJKS04YcPbW
RNZu9YO6bVi9JNlWSOrvxKJGgYhqOkbRqZtNyWHa0V1Xahg=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)pem";

#if WLED_BOARD == WLED_BOARD_JC4880P443
constexpr const char* kBuildTarget = "jc4880p443";
#elif WLED_BOARD == WLED_BOARD_JC8048W550C
constexpr const char* kBuildTarget = "jc8048w550c";
#else
constexpr const char* kBuildTarget = "esp32-cyd";
#endif

struct SemVer { uint32_t major = 0, minor = 0, patch = 0; };
struct ReleaseChoice {
  char version[24] = {};
  char notes[1200] = {};
  char firmware_url[360] = {};
  char sha256[65] = {};
  uint32_t size = 0;
};

Snapshot g_snapshot;
ReleaseChoice g_release;
#if !WLED_TOUCH_SIMULATOR
SemaphoreHandle_t g_lock = nullptr;
TaskHandle_t g_task = nullptr;
#endif

bool busyState(State state) {
  return state == State::kChecking || state == State::kDownloading || state == State::kVerifying ||
         state == State::kInstalling || state == State::kRestarting;
}
void copyText(char* to, size_t size, const char* from) {
  if (to && size) std::snprintf(to, size, "%s", from ? from : "");
}
void setState(State state, const char* message = nullptr) {
#if !WLED_TOUCH_SIMULATOR
  xSemaphoreTake(g_lock, portMAX_DELAY);
#endif
  g_snapshot.state = state;
  if (message) copyText(g_snapshot.message, sizeof(g_snapshot.message), message);
#if !WLED_TOUCH_SIMULATOR
  xSemaphoreGive(g_lock);
#endif
}
void setProgress(uint32_t done, uint32_t total) {
#if !WLED_TOUCH_SIMULATOR
  const uint8_t value = total ? static_cast<uint8_t>((uint64_t(done) * 100U) / total) : 0;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  g_snapshot.progress = value;
  xSemaphoreGive(g_lock);
#else
  (void)done;
  (void)total;
#endif
}
void setFailure(Failure failure, const char* message) {
#if !WLED_TOUCH_SIMULATOR
  xSemaphoreTake(g_lock, portMAX_DELAY);
#endif
  g_snapshot.state = State::kFailed;
  g_snapshot.failure = failure;
  g_snapshot.progress = 0;
  copyText(g_snapshot.message, sizeof(g_snapshot.message), message);
#if !WLED_TOUCH_SIMULATOR
  xSemaphoreGive(g_lock);
#endif
}

bool parseSemVer(const char* text, SemVer& version) {
  if (!text) return false;
  if (*text == 'v' || *text == 'V') ++text;
  uint32_t values[3] = {};
  for (uint8_t i = 0; i < 3; ++i) {
    if (*text < '0' || *text > '9') return false;
    while (*text >= '0' && *text <= '9') {
      if (values[i] > 100000000U) return false;
      values[i] = values[i] * 10 + uint32_t(*text++ - '0');
    }
    if (i != 2 && *text++ != '.') return false;
  }
  if (*text == '+') {  // build metadata does not affect precedence
    ++text;
    if (!*text) return false;
    while ((*text >= '0' && *text <= '9') || (*text >= 'A' && *text <= 'Z') ||
           (*text >= 'a' && *text <= 'z') || *text == '.' || *text == '-') ++text;
  }
  if (*text) return false;   // prerelease tags are never mistaken for stable
  version = {values[0], values[1], values[2]};
  return true;
}
int compareSemVer(const SemVer& a, const SemVer& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  return 0;
}
bool safeToInstall(char* reason, size_t reason_size) {
  if (!wifilink::connected()) {
    copyText(reason, reason_size, "Wi-Fi disconnected before the update could start.");
    return false;
  }
  if (batteryAvailable() && !batteryCharging()) {
    const int level = batteryLevel();
    if (level >= 0 && level < WLED_UPDATE_MIN_BATTERY_LEVEL) {
      std::snprintf(reason, reason_size, "Battery is below %u%%. Charge the remote before updating.", unsigned(WLED_UPDATE_MIN_BATTERY_LEVEL));
      return false;
    }
  }
  return true;
}

#if !WLED_TOUCH_SIMULATOR
void configureGithubClient(WiFiClientSecure& client) {
  client.setCACert(kGithubRootCa);
  client.setHandshakeTimeout(15);
}
bool beginGet(HTTPClient& http, WiFiClientSecure& client, const char* url) {
  configureGithubClient(client);
  if (!http.begin(client, url)) return false;
  // ArduinoJson must not consume HTTP/1.1 chunk framing from getStream() as
  // though it were part of the JSON body. HTTP/1.0 also makes GitHub close the
  // connection after the response, which is friendlier to a small ESP32.
  http.useHTTP10(true);
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "wled-touch-remote-updater");
  http.addHeader("Accept", "application/vnd.github+json");
  return true;
}
bool expectedDigest(const char* input, char* output, size_t output_size) {
  constexpr const char* prefix = "sha256:";
  if (!input || strncmp(input, prefix, strlen(prefix)) || strlen(input + strlen(prefix)) != 64) return false;
  for (const char* p = input + strlen(prefix); *p; ++p) {
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) return false;
  }
  copyText(output, output_size, input + strlen(prefix));
  for (char* p = output; *p; ++p) if (*p >= 'A' && *p <= 'F') *p = char(*p - 'A' + 'a');
  return true;
}

// Writing the image keeps the flash cache disabled for long stretches, which
// can make the ESP-Hosted status RPC time out on a healthy C6. Without this
// the radio watchdog would take that for a dead link and restart the device
// part-way through an install.
class LinkWatchdogGuard {
 public:
  LinkWatchdogGuard() { wifilink::suspendLinkWatchdog(true); }
  ~LinkWatchdogGuard() { wifilink::suspendLinkWatchdog(false); }
};

bool fetchRelease(ReleaseChoice& result) {
  constexpr int kMaxReleaseResponseBytes = 64 * 1024;
  char url[192];
  // One release is sufficient for the stable update channel and keeps the
  // filtered JSON response comfortably within the P4's OTA memory budget.
  std::snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/releases?per_page=1", WLED_UPDATE_GITHUB_OWNER, WLED_UPDATE_GITHUB_REPOSITORY);
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginGet(http, client, url)) { setFailure(Failure::kServer, "Could not start the update request."); return false; }
  const int response = http.GET();
  if (response != HTTP_CODE_OK) {
    Serial.printf("[UPDATE] release request failed: HTTP %d\n", response);
    http.end();
    setFailure(Failure::kServer, "Could not check for updates. Please try again later.");
    return false;
  }
  // getSize() is -1 when GitHub answers chunked and sends no Content-Length;
  // that is a valid response, so only an oversized one is rejected up front.
  const int expected_size = http.getSize();
  Serial.printf("[UPDATE] release response HTTP %d, length %d\n", response, expected_size);
  if (expected_size > kMaxReleaseResponseBytes) {
    Serial.printf("[UPDATE] unexpected release response size: %d\n", expected_size);
    http.end();
    setFailure(Failure::kServer, "Unexpected update response.");
    return false;
  }

  // getString() decodes HTTP framing, which ArduinoJson must not see, and lets
  // TLS release its memory before the filtered JSON document is allocated.
  String payload = http.getString();
  http.end();
  if (payload.isEmpty() || payload.length() > size_t(kMaxReleaseResponseBytes) ||
      (expected_size > 0 && payload.length() != static_cast<size_t>(expected_size))) {
    Serial.printf("[UPDATE] incomplete release response: expected %d bytes, received %u\n",
                  expected_size, unsigned(payload.length()));
    setFailure(Failure::kServer, "The update response was interrupted. Please try again.");
    return false;
  }
  JsonDocument filter;
  JsonArray filter_array = filter.to<JsonArray>();
  JsonObject release_filter = filter_array.add<JsonObject>();
  release_filter["tag_name"] = true; release_filter["draft"] = true; release_filter["prerelease"] = true; release_filter["body"] = true;
  JsonArray assets_filter = release_filter["assets"].to<JsonArray>();
  JsonObject asset_filter = assets_filter.add<JsonObject>();
  asset_filter["name"] = true; asset_filter["size"] = true; asset_filter["digest"] = true; asset_filter["browser_download_url"] = true;
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, payload.c_str(), payload.length(),
      DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(6));
  if (error || !document.is<JsonArray>()) {
    Serial.printf("[UPDATE] release JSON parse failed: %s\n", error.c_str());
    setFailure(Failure::kServer, "Unreadable update response. Please try again later.");
    return false;
  }
  SemVer installed;
  if (!parseSemVer(kAppVersion, installed)) { setFailure(Failure::kInvalidRelease, "The installed firmware version is invalid."); return false; }
  Serial.printf("[UPDATE] installed version: %s\n", kAppVersion);
  bool newer = false, found = false;
  SemVer selected;
  for (JsonObject release : document.as<JsonArray>()) {
    // This project has no prerelease channel, so draft and prerelease releases
    // are deliberately never candidates for a device update.
    if (release["draft"].as<bool>() || release["prerelease"].as<bool>()) continue;
    const char* tag = release["tag_name"] | "";
    SemVer candidate;
    if (!parseSemVer(tag, candidate)) {
      Serial.printf("[UPDATE] ignored release with an invalid tag: %s\n", tag);
      continue;
    }
    const int comparison = compareSemVer(candidate, installed);
    Serial.printf("[UPDATE] release %s compared with %s: %d\n", tag, kAppVersion, comparison);
    if (comparison <= 0) continue;
    newer = true;
    char expected_asset[112];
    std::snprintf(expected_asset, sizeof(expected_asset), "wled-touch-remote-%s-%s-firmware.bin", tag, kBuildTarget);
    JsonObject asset;
    for (JsonObject candidate_asset : release["assets"].as<JsonArray>()) {
      if (strcmp(candidate_asset["name"] | "", expected_asset) == 0) { asset = candidate_asset; break; }
    }
    const char* asset_url = asset["browser_download_url"] | "";
    const char* digest = asset["digest"] | "";
    const uint32_t size = asset["size"] | 0U;
    char digest_hex[65] = {};
    if (!asset_url[0] || !size || !expectedDigest(digest, digest_hex, sizeof(digest_hex))) {
      Serial.printf("[UPDATE] release %s has no verified %s firmware asset\n", tag, kBuildTarget);
      continue;
    }
    if (found && compareSemVer(candidate, selected) <= 0) continue;
    found = true; selected = candidate;
    copyText(result.version, sizeof(result.version), tag[0] == 'v' ? tag + 1 : tag);
    copyText(result.notes, sizeof(result.notes), release["body"] | "No release notes provided.");
    copyText(result.firmware_url, sizeof(result.firmware_url), asset_url);
    copyText(result.sha256, sizeof(result.sha256), digest_hex); result.size = size;
  }
  if (!found) {
    if (newer) setFailure(Failure::kInvalidRelease, "A newer release has no compatible, verified firmware asset.");
    else setState(State::kUpToDate, "Your device is up to date.");
    return false;
  }
  return true;
}
void publishAvailable(const ReleaseChoice& release) {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  g_snapshot.state = State::kUpdateAvailable; g_snapshot.failure = Failure::kNone; g_snapshot.progress = 0;
  copyText(g_snapshot.available_version, sizeof(g_snapshot.available_version), release.version);
  copyText(g_snapshot.release_notes, sizeof(g_snapshot.release_notes), release.notes);
  copyText(g_snapshot.message, sizeof(g_snapshot.message), "A verified firmware update is available.");
  xSemaphoreGive(g_lock);
}
// Streams straight into the OTA partition, hashing as it goes. Used where there
// is no RAM to stage the image: one connection, no resume.
class FirmwareSink final : public Stream {
 public:
  FirmwareSink(mbedtls_sha256_context& sha, uint32_t expected_size) : sha_(sha), expected_size_(expected_size) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (!size) return 0;
    if (write_failed_ || no_network_ || size > expected_size_ - written_) {
      write_failed_ = true;
      return 0;
    }
    if (!wifilink::connected()) {
      no_network_ = true;
      return 0;
    }
    if (Update.write(const_cast<uint8_t*>(data), size) != size) {
      write_failed_ = true;
      return 0;
    }
    mbedtls_sha256_update(&sha_, data, size);
    written_ += size;
    setProgress(written_, expected_size_);
    return size;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  uint32_t written() const { return written_; }
  bool noNetwork() const { return no_network_; }
  bool writeFailed() const { return write_failed_; }

 private:
  mbedtls_sha256_context& sha_;
  const uint32_t expected_size_;
  uint32_t written_ = 0;
  bool no_network_ = false;
  bool write_failed_ = false;
};

// Accumulates the image in RAM. written() doubles as the resume offset, so a
// range request that dies part-way simply continues from where it stopped. The
// digest is taken from the finished buffer rather than inline, because a
// retried range would otherwise hash the same bytes twice.
class StagingSink final : public Stream {
 public:
  StagingSink(uint8_t* image, uint32_t total) : image_(image), total_(total) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (!size) return 0;
    if (size > total_ - written_) {
      setWriteError();
      return 0;
    }
    std::memcpy(image_ + written_, data, size);
    written_ += size;
    setProgress(written_, total_);
    return size;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  uint32_t written() const { return written_; }

 private:
  uint8_t* const image_;
  const uint32_t total_;
  uint32_t written_ = 0;
};

// Assembling the image in PSRAM buys two things the CYD does not need. The
// download never touches flash, so Update.write() cannot erase a sector -- and
// so disable the cache on both cores -- while the C6 link is carrying data.
// And the buffer's fill level is a resume point, which is what lets a failed
// range be retried without losing the megabytes already fetched. It also means
// the partition is not written until the digest verifies.
uint8_t* reserveStagingBuffer(uint32_t size) {
#if WLED_BOARD == WLED_BOARD_JC4880P443
  uint8_t* const buffer =
      static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer) {
    Serial.printf("[UPDATE] no PSRAM for a %lu-byte staging buffer (%u free); writing flash inline\n",
                  static_cast<unsigned long>(size),
                  unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  }
  return buffer;
#else
  (void)size;
  return nullptr;
#endif
}

// Pauses the receive after every kBurstBytes so the TCP window closes and the
// stack can hand its buffers back. This limits the download to roughly
// 270 KB/s on a link measured at 1.5-2 MB/s, which is a real cost; it is here
// because it keeps lwIP from holding a full window of pbufs in internal DRAM,
// which is the resource this board actually runs out of. Untested without.
constexpr uint32_t kBurstBytes = 16 * 1024;
constexpr uint32_t kBurstPauseMs = 60;
// Same purpose: LWIP_TCP_WND_DEFAULT is 65534, so without this the sender may
// have a full 64 KiB of unread data buffered in internal DRAM at any moment.
constexpr int kSocketReceiveBytes = 8 * 1024;

struct BodyResult {
  uint32_t received = 0;
  uint32_t active_ms = 0;  // to the last byte, excluding any trailing stall
  uint32_t paused_ms = 0;  // of active_ms, time spent deliberately throttling
  bool stalled = false;
  bool closed = false;
};

// Reads the body itself because HTTPClient::writeToStream() panics the device
// here. Its loop only reaches the delay(1) branch when the length is unknown;
// with a Content-Length it spins on delay(0), and vTaskDelay(0) yields solely
// to tasks of equal or higher priority, so IDLE0 never runs and the task
// watchdog fires after 5 s. An empty read is the normal case, not an edge
// case: ssl_client.cpp sets O_NONBLOCK to select() on connect and never clears
// it (unlike NetworkClient), so a read with nothing buffered returns
// MBEDTLS_ERR_SSL_WANT_READ immediately and SO_RCVTIMEO is dead config.
// NetworkClient::readBytes() does have a delay(2) for that case, but
// NetworkClientSecure::read() reports an empty socket as -1 rather than 0,
// which readBytes treats as an error and breaks on first. The CYD survives
// writeToStream() only by accident: its inline Update.write() blocks in the
// flash erase yield often enough to feed the watchdog.
BodyResult receiveBody(WiFiClientSecure& client, Stream& sink, uint32_t expected_size) {
  constexpr size_t kReadSize = 2048;
  constexpr uint32_t kStallTimeoutMs = 10000;
  constexpr uint32_t kYieldIntervalMs = 100;

  BodyResult result;
  // Off the 12 KiB task stack, which TLS already draws on heavily.
  uint8_t* const buffer = static_cast<uint8_t*>(malloc(kReadSize));
  if (!buffer) return result;

  const uint32_t started_at = millis();
  uint32_t last_data_at = started_at;
  uint32_t last_yield_at = started_at;
  uint32_t since_pause = 0;
  while (result.received < expected_size) {
    const size_t wanted = std::min<size_t>(kReadSize, expected_size - result.received);
    const int count = client.read(buffer, wanted);
    if (count > 0) {
      if (sink.write(buffer, static_cast<size_t>(count)) != static_cast<size_t>(count)) break;
      result.received += static_cast<uint32_t>(count);
      since_pause += static_cast<uint32_t>(count);
      last_data_at = millis();
      result.active_ms = last_data_at - started_at;
    } else if (!client.connected()) {
      result.closed = true;
      break;
    }
    if (since_pause >= kBurstBytes) {
      since_pause = 0;
      delay(kBurstPauseMs);
      result.paused_ms += kBurstPauseMs;
      last_yield_at = millis();
      continue;
    }
    // A blocking delay, never delay(0): only this lets IDLE0 run. It is taken
    // on every empty read and at least ten times a second while data flows.
    const uint32_t now = millis();
    if (count <= 0 || now - last_yield_at >= kYieldIntervalMs) {
      delay(1);
      last_yield_at = millis();
    }
    if (millis() - last_data_at >= kStallTimeoutMs) {
      result.stalled = true;
      break;
    }
  }
  free(buffer);
  return result;
}

// One connection per call, torn down on return. Appends to sink, which tracks
// the resume offset itself, so a short result is a resumable partial success.
bool fetchRange(const char* url, StagingSink& sink, uint32_t length) {
  const uint32_t offset = sink.written();
  char range[48];
  std::snprintf(range, sizeof(range), "bytes=%lu-%lu", static_cast<unsigned long>(offset),
                static_cast<unsigned long>(offset + length - 1));

  WiFiClientSecure client;
  HTTPClient http;
  if (!beginGet(http, client, url)) return false;
  http.addHeader("Range", range);
  const int response = http.GET();
  // 206 is the contract. A 200 means Range was ignored and the whole body is
  // coming, which is only usable when nothing has been stored yet.
  if (response != HTTP_CODE_PARTIAL_CONTENT && !(response == HTTP_CODE_OK && offset == 0)) {
    Serial.printf("[UPDATE] range %s refused: HTTP %d\n", range, response);
    http.end();
    return false;
  }

  // Only settable now: HTTPClient owns the connect, so there is no socket to
  // configure until the response headers are in.
  const int receive_bytes = kSocketReceiveBytes;
  client.setSocketOption(SOL_SOCKET, SO_RCVBUF, &receive_bytes, sizeof(receive_bytes));

  const BodyResult body = receiveBody(client, sink, length);
  http.end();
  // Report the wire time with the deliberate throttle pauses taken out, so a
  // genuinely slow link stays distinguishable from a healthy, throttled one.
  const uint32_t wire_ms = body.active_ms > body.paused_ms ? body.active_ms - body.paused_ms : 0;
  // Internal free and largest-block are logged per range because internal DRAM
  // is what this path runs out of, and a TLS session is rebuilt for each one.
  // Exhaustion here surfaces as "esp-aes: Failed to allocate memory for start
  // alignment buffer", DNS failures and refused connects -- all of which read
  // as a dead radio link unless these two numbers say otherwise.
  Serial.printf("[UPDATE] range %s: %lu bytes, %lu ms wire + %lu ms throttle (%lu KB/s)%s%s, "
                "%lu total, %u internal free / %u largest\n",
                range, static_cast<unsigned long>(body.received),
                static_cast<unsigned long>(wire_ms), static_cast<unsigned long>(body.paused_ms),
                static_cast<unsigned long>(wire_ms ? body.received / wire_ms : 0),
                body.stalled ? ", STALLED" : "", body.closed ? ", CLOSED" : "",
                static_cast<unsigned long>(sink.written()),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
  return body.received == length;
}

// A retried chunk resumes from sink.written(), so progress is never lost as
// long as some attempt moves forward; the attempt budget only resets on one
// that completes.
bool downloadStagedImage(const char* url, StagingSink& sink, uint32_t size) {
  constexpr uint32_t kChunkBytes = 128 * 1024;
  constexpr uint8_t kMaxAttempts = 4;
  constexpr uint32_t kRetryDelayMs = 2000;
  constexpr uint32_t kSettleDelayMs = 100;

  uint8_t attempts = 0;
  while (sink.written() < size) {
    if (!wifilink::connected()) return false;
    const uint32_t length = std::min<uint32_t>(kChunkBytes, size - sink.written());
    if (fetchRange(url, sink, length)) {
      attempts = 0;
      delay(kSettleDelayMs);
      continue;
    }
    if (++attempts >= kMaxAttempts) return false;
    Serial.printf("[UPDATE] retrying from %lu bytes (attempt %u)\n",
                  static_cast<unsigned long>(sink.written()), unsigned(attempts + 1));
    delay(kRetryDelayMs);
  }
  return true;
}

// Hashing 3 MiB in one mbedtls call would hold the CPU far too long.
void hashImage(mbedtls_sha256_context& sha, const uint8_t* image, uint32_t size) {
  constexpr uint32_t kChunkSize = 64 * 1024;
  for (uint32_t offset = 0; offset < size; offset += kChunkSize) {
    mbedtls_sha256_update(&sha, image + offset, std::min<uint32_t>(kChunkSize, size - offset));
    delay(1);
  }
}

// Update.write() programs flash with the cache disabled, so feed it in chunks
// and yield in between: the idle task must keep running or the task watchdog
// panics part-way through the install.
bool writeStagedImage(const uint8_t* image, uint32_t size) {
  constexpr uint32_t kChunkSize = 32 * 1024;
  for (uint32_t offset = 0; offset < size; offset += kChunkSize) {
    const uint32_t count = std::min<uint32_t>(kChunkSize, size - offset);
    if (Update.write(const_cast<uint8_t*>(image) + offset, count) != count) return false;
    setProgress(offset + count, size);
    delay(1);
  }
  return true;
}

void downloadInstall(const ReleaseChoice& release) {
  // Scoped here rather than in installTask(): that task ends in vTaskDelete(),
  // which never unwinds the stack, so a guard held there would never release.
  LinkWatchdogGuard watchdog_guard;
  char reason[160] = {};
  if (!safeToInstall(reason, sizeof(reason))) { setFailure(wifilink::connected() ? Failure::kUnsafe : Failure::kNoNetwork, reason); return; }
  setState(State::kDownloading, "Downloading firmware...");
  // ESP-Hosted takes its receive buffers from internal DRAM, so log that too:
  // exhaustion there would explain a transport that dies under bulk receive.
  Serial.printf("[UPDATE] requesting %lu-byte firmware (%u internal, %u PSRAM free)\n",
                static_cast<unsigned long>(release.size),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  uint8_t* const staging = reserveStagingBuffer(release.size);
  mbedtls_sha256_context sha; mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0);
  uint8_t digest[32] = {};

  // Only the inline path has an open OTA handle to unwind. mbedtls_sha256_free
  // zeroizes and is safe on a context already finished below.
  const auto fail = [staging, &sha](Failure failure, const char* message) {
    if (!staging) Update.abort();
    mbedtls_sha256_free(&sha);
    heap_caps_free(staging);
    setFailure(failure, message);
  };

  if (staging) {
    StagingSink sink(staging, release.size);
    const bool complete = downloadStagedImage(release.firmware_url, sink, release.size);
    if (!complete || sink.written() != release.size) {
      Serial.printf("[UPDATE] firmware transfer incomplete: expected %lu bytes, received %lu\n",
                    static_cast<unsigned long>(release.size),
                    static_cast<unsigned long>(sink.written()));
      fail(wifilink::connected() ? Failure::kDownload : Failure::kNoNetwork,
           wifilink::connected() ? "The firmware download failed before it was complete."
                                 : "Wi-Fi disconnected while downloading the update.");
      return;
    }
    hashImage(sha, staging, release.size);
  } else {
    WiFiClientSecure client; HTTPClient http;
    if (!beginGet(http, client, release.firmware_url)) { fail(Failure::kDownload, "Could not start the firmware download."); return; }
    const int response = http.GET(), length = http.getSize();
    Serial.printf("[UPDATE] firmware response HTTP %d, length %d\n", response, length);
    if (response != HTTP_CODE_OK || length <= 0 || uint32_t(length) != release.size) { http.end(); fail(Failure::kDownload, "The firmware download was incomplete or unexpectedly sized."); return; }
    if (!Update.begin(release.size, U_FLASH)) { http.end(); fail(Failure::kInstall, "Not enough update space is available on this device."); return; }
    FirmwareSink sink(sha, release.size);
    const uint32_t transferred = receiveBody(client, sink, release.size).received;
    http.end();
    if (sink.noNetwork()) { fail(Failure::kNoNetwork, "Wi-Fi disconnected while downloading the update."); return; }
    if (sink.writeFailed()) { fail(Failure::kInstall, "Writing the firmware update to flash failed."); return; }
    if (transferred != release.size || sink.written() != release.size) {
      Serial.printf("[UPDATE] firmware transfer incomplete: expected %lu bytes, received %lu\n",
                    static_cast<unsigned long>(release.size),
                    static_cast<unsigned long>(transferred));
      fail(Failure::kDownload, "The firmware download failed before it was complete."); return;
    }
  }
  mbedtls_sha256_finish(&sha, digest); mbedtls_sha256_free(&sha);
  Serial.println("[UPDATE] firmware download complete");
  setState(State::kVerifying, "Verifying firmware...");
  char actual[65] = {}; for (size_t i = 0; i < sizeof(digest); ++i) std::snprintf(actual + i * 2, 3, "%02x", digest[i]);
  if (strcmp(actual, release.sha256)) { fail(Failure::kVerification, "Firmware verification failed. Your current software is unchanged."); return; }
  Serial.println("[UPDATE] firmware digest verified");

  // A staged image reaches flash only here, with the TLS socket already closed:
  // nothing is left on the radio link to lose while the cache is off, and the
  // partition is never touched until the download verifies.
  setState(State::kInstalling, "Installing verified firmware...");
  setProgress(0, release.size);
  if (staging && !Update.begin(release.size, U_FLASH)) { heap_caps_free(staging); setFailure(Failure::kInstall, "Not enough update space is available on this device."); return; }
  const bool installed = (!staging || writeStagedImage(staging, release.size)) && Update.end(true);
  heap_caps_free(staging);
  if (!installed) { Update.abort(); setFailure(Failure::kInstall, "Installation could not finish. Your current software is unchanged."); return; }
  Serial.println("[UPDATE] firmware installed successfully");
  setState(State::kSuccess, "Firmware installed successfully.");
}
void checkTask(void*) {
  ReleaseChoice choice;
  if (fetchRelease(choice)) {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_release = choice;
    xSemaphoreGive(g_lock);
    publishAvailable(choice);
  }
  xSemaphoreTake(g_lock, portMAX_DELAY); g_task = nullptr; xSemaphoreGive(g_lock); vTaskDelete(nullptr);
}
void installTask(void*) {
  xSemaphoreTake(g_lock, portMAX_DELAY); const ReleaseChoice choice = g_release; xSemaphoreGive(g_lock);
  downloadInstall(choice);
  xSemaphoreTake(g_lock, portMAX_DELAY); g_task = nullptr; xSemaphoreGive(g_lock); vTaskDelete(nullptr);
}
#endif
}  // namespace

void begin() {
#if !WLED_TOUCH_SIMULATOR
  g_lock = xSemaphoreCreateMutex();
#endif
  std::memset(&g_snapshot, 0, sizeof(g_snapshot));
  g_snapshot.state = State::kIdle;
  copyText(g_snapshot.installed_version, sizeof(g_snapshot.installed_version), kAppVersion);
}
void loop(uint32_t) { /* workers keep the UI loop responsive */ }
Snapshot snapshot() {
  Snapshot value;
#if !WLED_TOUCH_SIMULATOR
  xSemaphoreTake(g_lock, portMAX_DELAY);
#endif
  value = g_snapshot;
#if !WLED_TOUCH_SIMULATOR
  xSemaphoreGive(g_lock);
#endif
  return value;
}
bool busy() { return busyState(snapshot().state); }
bool flashing() { const State state = snapshot().state; return state == State::kDownloading || state == State::kVerifying || state == State::kInstalling || state == State::kRestarting; }
bool checkForUpdates() {
  if (!wifilink::connected()) { setFailure(Failure::kNoNetwork, "Wi-Fi is unavailable. Connect to Wi-Fi and try again."); return false; }
#if WLED_TOUCH_SIMULATOR
  setFailure(Failure::kServer, "Update checks are unavailable in the simulator."); return false;
#else
  xSemaphoreTake(g_lock, portMAX_DELAY);
  if (g_task || busyState(g_snapshot.state)) { xSemaphoreGive(g_lock); return false; }
  g_snapshot.state = State::kChecking; g_snapshot.failure = Failure::kNone; g_snapshot.progress = 0;
  copyText(g_snapshot.message, sizeof(g_snapshot.message), "Checking for updates...");
  const bool started = xTaskCreate(checkTask, "releaseCheck", 10240, nullptr, 1, &g_task) == pdPASS;
  if (!started) { g_snapshot.state = State::kFailed; g_snapshot.failure = Failure::kServer; copyText(g_snapshot.message, sizeof(g_snapshot.message), "Could not start the update check."); }
  xSemaphoreGive(g_lock); return started;
#endif
}
bool installAvailableUpdate() {
#if WLED_TOUCH_SIMULATOR
  setFailure(Failure::kServer, "Firmware installation is unavailable in the simulator."); return false;
#else
  xSemaphoreTake(g_lock, portMAX_DELAY);
  const bool retrying_install = g_snapshot.state == State::kFailed && g_release.firmware_url[0];
  if (g_task || (g_snapshot.state != State::kUpdateAvailable && !retrying_install) || !g_release.firmware_url[0]) { xSemaphoreGive(g_lock); return false; }
  const bool started = xTaskCreate(installTask, "firmwareOta", 12288, nullptr, 1, &g_task) == pdPASS;
  if (!started) { g_snapshot.state = State::kFailed; g_snapshot.failure = Failure::kInstall; copyText(g_snapshot.message, sizeof(g_snapshot.message), "Could not start the firmware update."); }
  xSemaphoreGive(g_lock); return started;
#endif
}
}  // namespace updater
