#pragma once

// Heatshrink compressor whose output Marlin's heatshrink decoder reads back.
// Format: a 1 bit then 8 bits for a literal byte; a 0 bit then WINDOW_BITS bits
// of (distance - 1) then LOOKAHEAD_BITS bits of (length - 1) for a back
// reference. Bits go out most significant first, the tail is padded with zeros.
//
// WINDOW_BITS and LOOKAHEAD_BITS must equal HEATSHRINK_STATIC_WINDOW_BITS and
// HEATSHRINK_STATIC_LOOKAHEAD_BITS in the firmware on the printer. See
// Marlin/src/libs/heatshrink/heatshrink_config.h. test/test_heatshrink.cpp
// static_asserts the two against each other.
//
// Input is buffered into fixed slices inside, so the bytes that go out depend
// only on the file, never on how big a read off the card happened to be.
//
// Usage:
//   static HeatshrinkEncoder enc;            // 2 KB, do not put it on a stack
//   uint8_t out[HeatshrinkEncoder::max_output(4096)];
//   n = enc.encode(chunk, chunk_len, out, sizeof out);
//   ... last chunk ...
//   n = enc.finish(out, sizeof out);         // needs max_output(0) of room

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
  #pragma GCC push_options
  #pragma GCC optimize("O3")
#endif

class HeatshrinkEncoder {
public:
  static const uint8_t WINDOW_BITS = 8;
  static const uint8_t LOOKAHEAD_BITS = 4;

  static const uint16_t WINDOW = 1 << WINDOW_BITS;
  static const uint16_t MAX_MATCH = 1 << LOOKAHEAD_BITS;
  static const uint16_t MIN_MATCH = 2;          // 13 bits beats 2 literals at 18

  // Room an output buffer needs for a call feeding `input_len` bytes. The worst
  // case is 9 bits a byte, and up to a full buffer held back from earlier calls
  // can come out during this one.
  static constexpr size_t max_output(size_t input_len) {
    return (input_len + 2 * WINDOW) + (input_len + 2 * WINDOW) / 8 + 2;
  }

  HeatshrinkEncoder() { reset(); }

  void reset() {
    fill_ = 0; done_ = 0; acc_ = 0; nbits_ = 0;
    for (uint16_t i = 0; i < 256; i++) head_[i] = -1;
  }

  // Compresses in[0 .. in_len), returning the bytes written to out. Writes
  // nothing and returns 0 if out_cap is below max_output(in_len).
  size_t encode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap) {
    if (out_cap < max_output(in_len)) return 0;
    size_t written = 0;
    while (in_len) {
      const size_t room = 2 * WINDOW - fill_;
      const uint16_t take = in_len < room ? (uint16_t)in_len : (uint16_t)room;
      memcpy(buf_ + fill_, in, take);
      in += take; in_len -= take; fill_ += take;
      if (fill_ == 2 * WINDOW) written += flush_slice(out + written);
    }
    return written;
  }

  // Compresses whatever is still held back and pads the last byte out with
  // zeros. Needs max_output(0) of room. Returns the bytes written.
  size_t finish(uint8_t *out, size_t out_cap) {
    if (out_cap < max_output(0)) return 0;
    size_t written = flush_slice(out);
    if (nbits_) {
      out[written++] = (uint8_t)((acc_ & ((1u << nbits_) - 1)) << (8 - nbits_));
      acc_ = 0; nbits_ = 0;
    }
    return written;
  }

private:
  uint8_t buf_[2 * WINDOW];
  int16_t chain_[2 * WINDOW];
  int16_t head_[256];
  uint16_t fill_;       // bytes sitting in buf_
  uint16_t done_;       // of those, how many are already compressed
  uint32_t acc_;
  uint8_t nbits_;

  uint8_t *out_; size_t out_n_;

  void put(uint32_t bits, uint8_t count) {
    acc_ = (acc_ << count) | bits;
    nbits_ += count;
    while (nbits_ >= 8) {
      nbits_ -= 8;
      out_[out_n_++] = (uint8_t)(acc_ >> nbits_);
    }
  }

  void insert(uint16_t p) {
    const uint8_t v = buf_[p];
    chain_[p] = head_[v];
    head_[v] = (int16_t)p;
  }

  size_t flush_slice(uint8_t *out) {
    if (done_ == fill_) return 0;
    out_ = out; out_n_ = 0;
    uint16_t p = done_;
    while (p < fill_) {
      uint16_t best_len = 0, best_dist = 0;
      const uint16_t room = fill_ - p;
      const uint16_t cap = room < MAX_MATCH ? room : MAX_MATCH;
      if (cap >= MIN_MATCH) {
        for (int16_t c = head_[buf_[p]]; c >= 0 && (uint16_t)(p - c) <= WINDOW; c = chain_[c]) {
          uint16_t l = 0;
          while (l < cap && buf_[c + l] == buf_[p + l]) l++;
          if (l > best_len) { best_len = l; best_dist = p - (uint16_t)c; if (l == cap) break; }
        }
      }
      // ponytail: greedy parse, 3.1% worse than an optimal one measured on
      // sliced gcode (1.850 against 1.909). Try a lazy match at p+1 if that
      // 3% ever matters more than the lines it costs.
      if (best_len >= MIN_MATCH) {
        put(0, 1);
        put(best_dist - 1, WINDOW_BITS);
        put(best_len - 1, LOOKAHEAD_BITS);
        for (uint16_t i = 0; i < best_len; i++) insert(p + i);
        p += best_len;
      }
      else {
        put(1, 1);
        put(buf_[p], 8);
        insert(p);
        p++;
      }
    }
    done_ = fill_;
    slide();
    return out_n_;
  }

  // Keeps the last WINDOW bytes as history and drops the rest, moving every
  // position stored in the match index down with them.
  void slide() {
    if (fill_ <= WINDOW) return;
    const uint16_t shift = fill_ - WINDOW;
    memmove(buf_, buf_ + shift, WINDOW);
    memmove(chain_, chain_ + shift, WINDOW * sizeof(int16_t));
    for (uint16_t i = 0; i < 256; i++)
      head_[i] = head_[i] >= (int16_t)shift ? head_[i] - (int16_t)shift : -1;
    for (uint16_t i = 0; i < WINDOW; i++)
      chain_[i] = chain_[i] >= (int16_t)shift ? chain_[i] - (int16_t)shift : -1;
    fill_ = WINDOW;
    done_ = WINDOW;
  }
};

#if defined(__GNUC__) && !defined(__clang__)
  #pragma GCC pop_options
#endif
