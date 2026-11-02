#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

extern uint32_t testClock;
inline uint32_t millis() { return testClock; }

struct String {
  std::string s;
  String() {}
  String(const char *v) : s(v ? v : "") {}
  String(const std::string &v) : s(v) {}
  String(int v) { char b[24]; snprintf(b, sizeof b, "%d", v); s = b; }
  String(unsigned v) { char b[24]; snprintf(b, sizeof b, "%u", v); s = b; }
  const char *c_str() const { return s.c_str(); }
  String operator+(const String &o) const { return String(s + o.s); }
  String &operator+=(char c) { s += c; return *this; }
  String &operator+=(const String &o) { s += o.s; return *this; }
  bool operator==(const String &o) const { return s == o.s; }
  bool operator!=(const String &o) const { return s != o.s; }
  bool operator==(const char *o) const { return s == (o ? o : ""); }
  bool operator!=(const char *o) const { return s != (o ? o : ""); }
  unsigned length() const { return (unsigned)s.size(); }
  void reserve(unsigned n) { s.reserve(n); }
};

// The hardware random generator. Deterministic here, so a failure can be repeated.
// Xorshift rather than a linear congruential generator, whose low bits cycle so short
// that a modulo by thirty two would hand back the same few characters forever.
inline uint32_t esp_random() {
  static uint32_t state = 0x13579bdf;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}
inline String operator+(const char *a, const String &b) { return String(std::string(a) + b.s); }

// Enough FreeRTOS for CommandQueue: the host runs it on one thread, so the lock is a no-op
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFF
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (SemaphoreHandle_t)1; }
inline int xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
