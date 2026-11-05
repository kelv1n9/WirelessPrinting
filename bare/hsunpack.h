#pragma once

#include <stdint.h>
#include <stddef.h>

// Unpacks heatshrink with an 8 bit window and a 4 bit lookahead, as the Worker packs a
// file it carries down the relay. 260 bytes of state, a byte out at a time.
class HsUnpack {
public:
  void begin() { head = 0; acc = 0; bits = 0; state = Flag; }

  template <typename Emit>
  void feed(const uint8_t *data, size_t len, Emit emit) {
    for (size_t i = 0; i < len; i++) {
      acc = (acc << 8) | data[i];
      bits += 8;
      for (;;) {
        if (state == Flag) {
          if (bits < 1) break;
          state = take(1) ? Literal : Distance;
        }
        else if (state == Literal) {
          if (bits < 8) break;
          put((uint8_t)take(8), emit);
          state = Flag;
        }
        else if (state == Distance) {
          if (bits < 8) break;
          distance = take(8) + 1;
          state = Count;
        }
        else {
          if (bits < 4) break;
          const uint16_t count = take(4) + 1;
          for (uint16_t n = 0; n < count; n++)
            put(window[(uint8_t)(head - distance)], emit);
          state = Flag;
        }
      }
    }
  }

private:
  enum State : uint8_t { Flag, Literal, Distance, Count };
  uint8_t window[256];
  uint8_t head = 0;
  uint32_t acc = 0;
  uint8_t bits = 0;
  uint16_t distance = 0;
  State state = Flag;

  uint16_t take(uint8_t n) {
    bits -= n;
    const uint16_t v = (acc >> bits) & ((1u << n) - 1);
    acc &= (1u << bits) - 1;
    return v;
  }
  template <typename Emit>
  void put(uint8_t b, Emit &emit) {
    window[head++] = b;
    emit(b);
  }
};
