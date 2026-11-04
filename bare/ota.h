#pragma once

#include <atomic>

extern volatile uint32_t loopBeat;

static std::atomic<bool> otaActive{false};
static bool otaGateHeld;
static bool otaStarted;
static bool otaComplete;
static String otaTrouble;         // said back in the answer, so a failed flash can be read

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
    otaTrouble = "";
    otaActive = true;
    otaGateHeld = tlsGate && xSemaphoreTake(tlsGate, pdMS_TO_TICKS(30000)) == pdTRUE;
    if (!otaGateHeld) {
      otaTrouble = "the TLS gate stayed taken for 30 s";
      otaAbort();
      return;
    }
    otaStarted = Update.begin(UPDATE_SIZE_UNKNOWN);
    if (!otaStarted) {
      otaTrouble = String("could not start: ") + Update.errorString() +
                   ", largest free block " + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      otaAbort();
    }
  }
  else if (up.status == UPLOAD_FILE_WRITE && otaStarted) {
    loopBeat = millis();          // a long upload is one call of the loop, and not a hang
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      otaTrouble = String("write failed: ") + Update.errorString();
      otaAbort();
    }
  }
  else if (up.status == UPLOAD_FILE_END && otaStarted) {
    otaComplete = Update.end(true);
    if (!otaComplete) {
      otaTrouble = String("the image was refused: ") + Update.errorString();
      otaAbort();
    }
    else
      otaRelease();
  }
  else if (up.status == UPLOAD_FILE_ABORTED) {
    otaTrouble = "the upload broke off";
    otaAbort();
  }
}
