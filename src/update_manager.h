#pragma once

#include <cstdint>

// Owns release discovery, validation, and OTA. UI only renders snapshots.
namespace updater {
enum class State : uint8_t { kIdle, kChecking, kUpToDate, kUpdateAvailable, kDownloading, kVerifying, kInstalling, kRestarting, kSuccess, kFailed };
enum class Failure : uint8_t { kNone, kNoNetwork, kServer, kInvalidRelease, kUnsafe, kDownload, kVerification, kInstall };

struct Snapshot {
  State state = State::kIdle;
  Failure failure = Failure::kNone;
  uint8_t progress = 0;
  char installed_version[24] = {};
  char available_version[24] = {};
  char release_notes[1200] = {};
  char message[160] = {};
};

void begin();
void loop(uint32_t now_ms);
Snapshot snapshot();
bool checkForUpdates();
bool installAvailableUpdate();
bool busy();
bool flashing();
}  // namespace updater
