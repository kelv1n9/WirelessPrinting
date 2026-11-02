#pragma once
#include <string>
#include "Arduino.h"

// Only what the guard actually touches. The digest arithmetic itself belongs to the
// library and is not what these tests are about: the branches are.
struct FakeHeader {
  String v;
  const String &value() const { return v; }
};

class AsyncWebServerRequest {
public:
  std::string apiKey;          // what the client sent, if anything
  std::string offeredPassword; // what the client would answer a digest challenge with
  bool hasApiKey = false;
  bool challenged = false;     // the guard asked it to log in
  FakeHeader header;

  bool hasHeader(const char *name) const {
    return hasApiKey && std::string(name) == "X-Api-Key";
  }
  FakeHeader *getHeader(const char *) { header.v = String(apiKey); return &header; }
  bool authenticate(const char *, const char *password, const char * = nullptr) {
    return !offeredPassword.empty() && offeredPassword == std::string(password ? password : "");
  }
  void requestAuthentication(const char * = nullptr) { challenged = true; }
};

typedef bool (*ArAuthGuard)(AsyncWebServerRequest *request);
extern ArAuthGuard asyncAuthGuard;
