#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct CardBytes : ByteSource {
  File file;
  uint32_t held = 0, consumed = 0, readMs = 0;
  size_t at = 0, cached = 0;
  alignas(32) uint8_t buffer[4096];

  bool open(const char *path) {
    shut();
    readMs = 0;
    file = SD_MMC.open(path);
    held = file ? file.size() : 0;
    if (!held) shut();
    return bool(file) && held > 0;
  }

  void shut() {
    if (file) file.close();
    at = cached = consumed = 0;
  }

  int next(uint8_t *out, size_t max) {
    if (!file || !out || !max) return -1;
    if (consumed == held) return 0;
    if (at == cached) {
      const size_t remaining = held - consumed;
      const size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
      const uint32_t started = millis();
      const int took = file.read(buffer, want);
      readMs += uint32_t(millis() - started);
      if (took <= 0 || size_t(took) > want) {
        shut();
        return -1;
      }
      at = 0;
      cached = size_t(took);
    }
    const size_t available = cached - at;
    const size_t took = max < available ? max : available;
    memcpy(out, buffer + at, took);
    at += took;
    consumed += took;
    return int(took);
  }

  uint32_t size() const { return held; }
};
