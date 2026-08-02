#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

class Preferences {
 public:
  bool begin(const char* ns, bool read_only = false) {
    namespace_ = ns ? ns : "prefs";
    read_only_ = read_only;
    mkdir(".sim-prefs", 0755);
    return true;
  }

  void end() {}

  bool isKey(const char* key) {
    FILE* file = std::fopen(pathFor(key).c_str(), "rb");
    if (!file) {
      return false;
    }
    std::fclose(file);
    return true;
  }

  bool getBool(const char* key, bool default_value = false) {
    std::string value = readValue(key);
    if (value.empty()) {
      return default_value;
    }
    return value == "1" || value == "true";
  }

  unsigned char getUChar(const char* key, unsigned char default_value = 0) {
    std::string value = readValue(key);
    if (value.empty()) {
      return default_value;
    }
    return static_cast<unsigned char>(std::strtoul(value.c_str(), nullptr, 10));
  }

  unsigned int getUInt(const char* key, unsigned int default_value = 0) {
    std::string value = readValue(key);
    return value.empty() ? default_value : static_cast<unsigned int>(std::strtoul(value.c_str(), nullptr, 10));
  }

  size_t getString(const char* key, char* value, size_t max_len) {
    if (!value || !max_len) return 0;
    const std::string stored = readValue(key);
    const size_t len = stored.size() < max_len - 1 ? stored.size() : max_len - 1;
    std::memcpy(value, stored.data(), len);
    value[len] = '\0';
    return len;
  }

  void putBool(const char* key, bool value) {
    writeValue(key, value ? "1" : "0");
  }

  void putUChar(const char* key, unsigned char value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%u", static_cast<unsigned>(value));
    writeValue(key, buffer);
  }

  void putUInt(const char* key, unsigned int value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%u", value);
    writeValue(key, buffer);
  }

  size_t putString(const char* key, const char* value) {
    if (!value) return 0;
    writeValue(key, value);
    return std::strlen(value);
  }

  bool remove(const char* key) {
    if (read_only_) return false;
    return std::remove(pathFor(key).c_str()) == 0;
  }

 private:
  std::string namespace_;
  bool read_only_ = false;

  std::string pathFor(const char* key) const {
    return ".sim-prefs/" + namespace_ + "-" + (key ? key : "key");
  }

  std::string readValue(const char* key) {
    FILE* file = std::fopen(pathFor(key).c_str(), "rb");
    if (!file) {
      return {};
    }
    char buffer[64] = {};
    const size_t len = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    return std::string(buffer, len);
  }

  void writeValue(const char* key, const char* value) {
    if (read_only_) {
      return;
    }
    FILE* file = std::fopen(pathFor(key).c_str(), "wb");
    if (!file) {
      return;
    }
    std::fwrite(value, 1, std::strlen(value), file);
    std::fclose(file);
  }
};
