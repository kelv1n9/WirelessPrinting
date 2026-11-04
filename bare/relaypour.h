#pragma once

#include <stdint.h>
#include <stddef.h>

// Writes all of it, however long the other side stops reading, and gives up only when
// nothing at all has gone for `patience` milliseconds. The firmware route erases a
// whole flash slot before it reads its first byte, several seconds of a full loopback,
// and a write that gave up after one short wait broke every flash through the relay.
template <typename Sink, typename Clock, typename Nap>
bool relayPourAll(Sink &sink, const uint8_t *data, size_t n, uint32_t patience, Clock now, Nap nap) {
  uint32_t lastMoved = now();
  while (n) {
    if (!sink.connected())
      return false;
    const size_t took = sink.write(data, n);
    if (took) {
      data += took;
      n -= took;
      lastMoved = now();
      continue;
    }
    if (now() - lastMoved >= patience)
      return false;
    nap();
  }
  return true;
}
