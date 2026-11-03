#pragma once

#include <atomic>

static std::atomic<bool> otaActive{false};
static bool otaGateHeld;
static bool otaStarted;
static bool otaComplete;

static void otaRelease() {
  otaStarted = false;
  if (otaGateHeld) {
    xSemaphoreGive(tlsGate);
    otaGateHeld = false;
  }
  otaActive = false;
}

static void otaAbort() {
  otaComplete = false;
  Update.abort();
  otaRelease();
}

static void onUpload() {
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    if (otaStarted)
      otaAbort();
    otaComplete = false;
    otaActive = true;
    otaGateHeld = tlsGate && xSemaphoreTake(tlsGate, pdMS_TO_TICKS(30000)) == pdTRUE;
    if (!otaGateHeld) {
      otaAbort();
      return;
    }
    otaStarted = Update.begin(UPDATE_SIZE_UNKNOWN);
    if (!otaStarted) {
      Update.printError(Serial);
      otaAbort();
    }
  }
  else if (up.status == UPLOAD_FILE_WRITE && otaStarted) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      Update.printError(Serial);
      otaAbort();
    }
  }
  else if (up.status == UPLOAD_FILE_END && otaStarted) {
    otaComplete = Update.end(true);
    if (!otaComplete) {
      Update.printError(Serial);
      otaAbort();
    }
    else
      otaRelease();
  }
  else if (up.status == UPLOAD_FILE_ABORTED)
    otaAbort();
}
