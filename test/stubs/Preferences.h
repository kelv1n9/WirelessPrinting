#pragma once
#include <map>
#include <string>
#include "Arduino.h"

// Survives a simulated reboot, like the real thing
inline std::map<std::string, std::string> &prefsStore() {
  static std::map<std::string, std::string> store;
  return store;
}

class Preferences {
public:
  bool begin(const char *, bool = false) { return true; }
  void end() {}
  String getString(const char *key, const String &fallback) {
    auto it = prefsStore().find(key);
    return it == prefsStore().end() ? fallback : String(it->second);
  }
  size_t putString(const char *key, const String &value) {
    prefsStore()[key] = value.s;
    return value.s.size();
  }
  bool getBool(const char *key, bool fallback) {
    auto it = prefsStore().find(key);
    return it == prefsStore().end() ? fallback : it->second == "1";
  }
  size_t putBool(const char *key, bool value) {
    prefsStore()[key] = value ? "1" : "0";
    return 1;
  }
};
