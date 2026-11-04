#pragma once
#include <deque>
#include <vector>

#include "Arduino.h"
struct HardwareSerial {
  std::deque<uint8_t> rx;
  std::vector<uint8_t> written;
  void begin(int, int, int = 0, int = 0) {}
  void setTimeout(int) {}
  size_t write(const uint8_t *data, size_t n) {
    written.insert(written.end(), data, data + n);
    return n;
  }
  void flush() {}
  size_t readBytes(uint8_t *buf, size_t n) {
    size_t i = 0;
    while (i < n && !rx.empty()) {
      buf[i++] = rx.front();
      rx.pop_front();
    }
    return i;
  }
  int read() {
    if (rx.empty()) return -1;
    int v = rx.front();
    rx.pop_front();
    return v;
  }
  int available() { return static_cast<int>(rx.size()); }
};
struct SerialT {
  void print(const char *) {}
};
static SerialT Serial;
