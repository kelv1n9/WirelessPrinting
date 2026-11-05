#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#ifdef ARDUINO
#include <rom/miniz.h>
#include <esp_heap_caps.h>
#else
#include "miniz.h"
#endif

// Unpacks raw deflate with a window of at most 4 KB, as the Worker packs a file it
// carries down the relay: a third fewer bytes than heatshrink, and across the internet
// the bytes are the speed. The decoder is the one in ROM. Its 11 KB of state and the
// window are taken for one file and handed back after it. The window doubles as the
// card buffer, so the file comes out 4 KB at a time from memory the card can DMA from.
class Inflate {
public:
  static const size_t Window = 4096;

  bool begin() {
    state = (tinfl_decompressor *)malloc(sizeof *state);
#ifdef ARDUINO
    window = (uint8_t *)heap_caps_malloc(Window, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
#else
    window = (uint8_t *)malloc(Window);
#endif
    if (!state || !window) {
      end();
      return false;
    }
    tinfl_init(state);
    at = 0;
    done = bad = false;
    return true;
  }

  void end() {
    free(state);
    free(window);
    state = nullptr;
    window = nullptr;
  }

  // Last says no more is coming. Write gets the file a window at a time.
  template <typename Write>
  bool feed(const uint8_t *data, size_t len, bool last, Write write) {
    while (!done && !bad) {
      size_t in = len, room = Window - at;
      const tinfl_status s = tinfl_decompress(state, data, &in, window, window + at, &room,
                                              last ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
      data += in;
      len -= in;
      at += room;
      if (at == Window || (s == TINFL_STATUS_DONE && at)) {
        write(window, at);
        at = 0;
      }
      if (s < 0 || (!in && !room && s != TINFL_STATUS_DONE && s != TINFL_STATUS_NEEDS_MORE_INPUT))
        bad = true;
      else if (s == TINFL_STATUS_DONE)
        done = true;
      else if (s == TINFL_STATUS_NEEDS_MORE_INPUT)
        break;
    }
    return !bad;
  }

  bool finished() const { return done; }

private:
  tinfl_decompressor *state = nullptr;
  uint8_t *window = nullptr;
  size_t at = 0;
  bool done = false, bad = false;
};
