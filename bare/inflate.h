#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#ifdef ARDUINO
#include <rom/miniz.h>
#else
#include "miniz.h"
#endif

// Unpacks a zlib stream, as the Worker packs a file it carries down the relay: across
// the internet the bytes are the speed. The zlib wrapper is what makes it safe: the
// decoder refuses a file packed with a wider window than it is given, and checks the
// Adler-32 of everything it wrote against the one the packer put at the end. The decoder is the one in ROM. Its 11 KB of
// state are taken for one file and handed back after it. The window is the caller's,
// a power of two no smaller than the one the file was packed with, and doubles as the
// card buffer: the file comes out a window at a time from memory the card can DMA from.
class Inflate {
public:
  bool begin(uint8_t *windowAt, size_t windowSize) {
    window = windowAt;
    Window = windowSize;
    state = (tinfl_decompressor *)malloc(sizeof *state);
    if (!state)
      return false;
    tinfl_init(state);
    at = 0;
    done = bad = false;
    return true;
  }

  void end() {
    free(state);
    state = nullptr;
  }

  // Last says no more is coming. Write gets the file a window at a time.
  template <typename Write>
  bool feed(const uint8_t *data, size_t len, bool last, Write write) {
    while (!done && !bad) {
      size_t in = len, room = Window - at;
      const tinfl_status s = tinfl_decompress(state, data, &in, window, window + at, &room,
                                              TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_COMPUTE_ADLER32 |
                                                  (last ? 0 : TINFL_FLAG_HAS_MORE_INPUT));
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
  size_t Window = 0, at = 0;
  bool done = false, bad = false;
};
