#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

enum { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload {
  int status = UPLOAD_FILE_START;
  uint8_t buf[8]{};
  size_t currentSize = sizeof buf;
};
struct {
  HTTPUpload up;
  HTTPUpload &upload() { return up; }
} server;

static std::mutex gate;
static auto tlsGate = &gate;
static constexpr int pdTRUE = 1;
static constexpr int pdMS_TO_TICKS(int ms) { return ms; }
static constexpr size_t UPDATE_SIZE_UNKNOWN = size_t(-1);
static int Serial;
static std::atomic<int> takes{0};
static int gives;
static bool gateTimeout;
static int xSemaphoreTake(std::mutex *, int);
static void xSemaphoreGive(std::mutex *);

struct {
  bool beginOK = true, writeOK = true, endOK = true, running = false;
  int begins = 0, writes = 0, ends = 0, aborts = 0;
  bool begin(size_t size) {
    assert(size == UPDATE_SIZE_UNKNOWN && !running);
    ++begins;
    return running = beginOK;
  }
  size_t write(uint8_t *, size_t size) {
    assert(running);
    ++writes;
    return writeOK ? size : size - 1;
  }
  bool end(bool remaining) {
    assert(running && remaining);
    ++ends;
    if (endOK) running = false;
    return endOK;
  }
  void abort() { ++aborts; running = false; }
  void printError(int) {}
} Update;

volatile uint32_t loopBeat = 0;
static uint32_t millis() { return 1; }
#include "../bare/ota.h"

static int xSemaphoreTake(std::mutex *mutex, int wait) {
  assert(otaActive && wait == pdMS_TO_TICKS(30000));
  if (gateTimeout) return 0;
  ++takes;
  mutex->lock();
  return pdTRUE;
}

static void xSemaphoreGive(std::mutex *mutex) {
  assert(otaGateHeld);
  ++gives;
  mutex->unlock();
}

static void event(int status) {
  server.up.status = status;
  onUpload();
}

static void released(bool complete) {
  assert(!otaActive && !otaStarted && !otaGateHeld);
  assert(otaComplete == complete && !Update.running);
  assert(takes == gives);
}

static void success() {
  event(UPLOAD_FILE_START);
  assert(otaActive && otaStarted && otaGateHeld && !otaComplete);
  event(UPLOAD_FILE_WRITE);
  event(UPLOAD_FILE_END);
  released(true);
}

int main() {
  assert(!otaComplete);
  event(UPLOAD_FILE_WRITE);
  event(UPLOAD_FILE_END);
  released(false);
  assert(Update.begins == 0 && Update.writes == 0 && Update.ends == 0);

  std::atomic<bool> tlsReady{false};
  std::thread existingTLS([&] {
    gate.lock();
    tlsReady = true;
    while (!takes.load()) std::this_thread::yield();
    assert(otaActive && Update.begins == 0);
    gate.unlock();
  });
  while (!tlsReady.load()) std::this_thread::yield();
  event(UPLOAD_FILE_START);
  existingTLS.join();
  event(UPLOAD_FILE_WRITE);
  event(UPLOAD_FILE_ABORTED);
  released(false);
  assert(Update.aborts == 1 && Update.ends == 0);
  success();

  Update.beginOK = false;
  event(UPLOAD_FILE_START);
  released(false);
  event(UPLOAD_FILE_END);
  released(false);
  Update.beginOK = true;
  success();

  event(UPLOAD_FILE_START);
  Update.writeOK = false;
  event(UPLOAD_FILE_WRITE);
  released(false);
  const int ends = Update.ends, writes = Update.writes;
  event(UPLOAD_FILE_WRITE);
  event(UPLOAD_FILE_END);
  assert(Update.ends == ends && Update.writes == writes);
  released(false);
  Update.writeOK = true;
  success();

  event(UPLOAD_FILE_START);
  Update.endOK = false;
  event(UPLOAD_FILE_END);
  released(false);
  Update.endOK = true;
  success();

  event(UPLOAD_FILE_ABORTED);
  event(UPLOAD_FILE_ABORTED);
  released(false);
  event(UPLOAD_FILE_START);
  event(UPLOAD_FILE_START);
  event(UPLOAD_FILE_ABORTED);
  released(false);

  tlsGate = nullptr;
  event(UPLOAD_FILE_START);
  released(false);
  tlsGate = &gate;
  success();
  gateTimeout = true;
  const int begins = Update.begins, releases = gives;
  event(UPLOAD_FILE_START);
  event(UPLOAD_FILE_WRITE);
  event(UPLOAD_FILE_END);
  released(false);
  assert(Update.begins == begins && gives == releases);
  gateTimeout = false;
  success();
  puts("OTA recovery and TLS serialization passed");
}
