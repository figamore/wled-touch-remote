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

  void putBool(const char* key, bool value) {
    writeValue(key, value ? "1" : "0");
  }

  void putUChar(const char* key, unsigned char value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%u", static_cast<unsigned>(value));
    writeValue(key, buffer);
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
