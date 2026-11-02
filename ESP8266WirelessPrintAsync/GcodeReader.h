#pragma once

#include <stddef.h>
#include <stdint.h>

#define GCODE_BLOCK_BYTES 2048
#define GCODE_LINE_BYTES  320

// A block is taken at once and the lines come out of it. The source is a template
// parameter so the same code can be run against a plain array on a desktop, which is
// where the carriage return below was caught.
class GcodeReader {
  public:
    void rewind() {
      held = taken = 0;
      text[0] = 0;
    }

    bool blockSpent() const {
      return taken == held;
    }

    const char *line() const {
      return text;
    }

    // Fills the line and says how many bytes of the file that took, the newline
    // included, so the position the progress is worked out from stays right.
    template <typename Source>
    size_t nextLine(Source &file) {
      size_t at = 0, consumed = 0;

      for (;;) {
        if (taken == held) {
          taken = 0;
          const int got = file.read(block, GCODE_BLOCK_BYTES);
          held = got > 0 ? (size_t)got : 0;
          if (held == 0)
            break;
        }
        const char ch = (char)block[taken++];
        ++consumed;
        if (ch == '\n')
          break;
        if (at < GCODE_LINE_BYTES - 1)
          text[at++] = ch;
      }
      while (at && text[at - 1] == '\r')   // A file written on Windows ends every line
        --at;                              // with one, and it would land past the checksum
      text[at] = 0;

      return consumed;
    }

  private:
    uint8_t block[GCODE_BLOCK_BYTES] __attribute__((aligned(32)));
    size_t held = 0, taken = 0;
    char text[GCODE_LINE_BYTES];
};
