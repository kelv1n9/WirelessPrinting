#pragma once

#include <stddef.h>
#include <stdint.h>

template <typename Source>
class FileFeeder {
  public:
    void start(size_t bytes) {
      left = bytes;
    }

    bool spent() const {
      return left == 0;
    }

    size_t serve(Source &file, uint8_t *out, size_t room) {
      if (room > left)
        room = left;

      size_t served = 0;
      while (served < room) {
        const int got = file.read(out + served, room - served);
        if (got <= 0) {
          left = 0;
          return served;
        }
        served += got;
      }

      left -= served;
      return served;
    }

  private:
    size_t left = 0;
};
