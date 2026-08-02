#include "update_manager.h"

#include <Arduino.h>
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
  const int expected_size = http.getSize();
  if (expected_size <= 0 || expected_size > kMaxReleaseResponseBytes) {
    Serial.printf("[UPDATE] unexpected release response size: %d\n", expected_size);
    http.end();
    setFailure(Failure::kServer, "Unexpected update response.");
    return false;
  }

  // HTTPClient decodes transfer framing in getString(); getStream() exposes
  // the raw socket and can make ArduinoJson report IncompleteInput. Buffering
  // this small, bounded response also lets TLS release its memory before the
  // filtered JSON document is allocated.
  String payload = http.getString();
  http.end();
  if (payload.length() != static_cast<size_t>(expected_size)) {
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
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_snapshot.progress = static_cast<uint8_t>((uint64_t(written_) * 100U) / expected_size_);
    xSemaphoreGive(g_lock);
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
void downloadInstall(const ReleaseChoice& release) {
  char reason[160] = {};
  if (!safeToInstall(reason, sizeof(reason))) { setFailure(wifilink::connected() ? Failure::kUnsafe : Failure::kNoNetwork, reason); return; }
  setState(State::kDownloading, "Downloading firmware...");
  WiFiClientSecure client; HTTPClient http;
  if (!beginGet(http, client, release.firmware_url)) { setFailure(Failure::kDownload, "Could not start the firmware download."); return; }
  const int response = http.GET(), length = http.getSize();
  if (response != HTTP_CODE_OK || length <= 0 || uint32_t(length) != release.size) { http.end(); setFailure(Failure::kDownload, "The firmware download was incomplete or unexpectedly sized."); return; }
  if (!Update.begin(release.size, U_FLASH)) { http.end(); setFailure(Failure::kInstall, "Not enough update space is available on this device."); return; }
  mbedtls_sha256_context sha; mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0);
  FirmwareSink sink(sha, release.size);
  const int transferred = http.writeToStream(&sink);
  http.end(); uint8_t digest[32] = {}; mbedtls_sha256_finish(&sha, digest); mbedtls_sha256_free(&sha);
  if (sink.noNetwork()) { Update.abort(); setFailure(Failure::kNoNetwork, "Wi-Fi disconnected while downloading the update."); return; }
  if (sink.writeFailed()) { Update.abort(); setFailure(Failure::kInstall, "Writing the firmware update to flash failed."); return; }
  if (transferred != static_cast<int>(release.size) || sink.written() != release.size) {
    Serial.printf("[UPDATE] firmware transfer incomplete: expected %lu bytes, received %d / wrote %lu\n",
                  static_cast<unsigned long>(release.size), transferred,
                  static_cast<unsigned long>(sink.written()));
    Update.abort(); setFailure(Failure::kDownload, "The firmware download failed before it was complete."); return;
  }
  setState(State::kVerifying, "Verifying firmware...");
  char actual[65] = {}; for (size_t i = 0; i < sizeof(digest); ++i) std::snprintf(actual + i * 2, 3, "%02x", digest[i]);
  if (strcmp(actual, release.sha256)) { Update.abort(); setFailure(Failure::kVerification, "Firmware verification failed. Your current software is unchanged."); return; }
  setState(State::kInstalling, "Installing verified firmware...");
  if (!Update.end(true)) { Update.abort(); setFailure(Failure::kInstall, "Installation could not finish. Your current software is unchanged."); return; }
  setState(State::kSuccess, "Firmware installed successfully.");
  vTaskDelay(pdMS_TO_TICKS(150));
  setState(State::kRestarting, "Update installed. Restarting the remote...");
  vTaskDelay(pdMS_TO_TICKS(700)); displayRestart();
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
