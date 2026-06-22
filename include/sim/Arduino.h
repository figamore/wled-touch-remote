#pragma once

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using String = std::string;

#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW 0
#endif
#ifndef INPUT
#define INPUT 0
#endif
#ifndef OUTPUT
#define OUTPUT 1
#endif

template <typename T>
T min(T a, T b) {
  return std::min(a, b);
}

template <typename T>
T max(T a, T b) {
  return std::max(a, b);
}

inline uint32_t millis() {
  static const auto start = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
}

inline void delay(uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

class SerialClass {
 public:
  void begin(uint32_t) {}

  void print(const char* text) {
    std::fputs(text, stdout);
  }

  void println(const char* text) {
    std::fputs(text, stdout);
    std::fputc('\n', stdout);
  }

  void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
  }
};

inline SerialClass Serial;

class EspClass {
 public:
  void restart() {
    std::puts("Simulator restart requested");
    std::exit(0);
  }
};

inline EspClass ESP;

inline int analogRead(int) {
  return 0;
}
