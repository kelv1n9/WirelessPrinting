// Dumps every byte our sender puts on the wire during a real transfer, so Marlin's own
// reference implementation can be asked whether these are the packets it would build.
#include <cstdio>
#include <cstring>
#include <string>
#include "sim/MarlinSim.h"
#include "sim/Wire.h"
#include "../ESP8266WirelessPrintAsync/BinaryTransfer.h"

struct MemorySource : ByteSource {
  std::string body;
  size_t at = 0;
  explicit MemorySource(std::string from) : body(std::move(from)) {}
  int next(uint8_t *out, const size_t max) override {
    const size_t left = body.size() - at, take = left < max ? left : max;
    memcpy(out, body.data() + at, take);
    at += take;
    return (int)take;
  }
  uint32_t size() const override { return (uint32_t)body.size(); }
};

struct Tapped : Wire {
  FILE *log;
  Tapped(MarlinSim &s, FILE *f) : Wire(s, 115200), log(f) {}
  size_t write(const uint8_t *d, const size_t n) override {
    const size_t took = Wire::write(d, n);
    if (took)
      for (size_t i = 0; i < n; i++) fputc(d[i], log);
    return took;
  }
};

int main(int argc, char **argv) {
  FILE *log = fopen(argv[1], "wb");
  MarlinSim sim;
  sim.binary = true;
  Tapped wire(sim, log);
  BinaryTransfer move;
  move.begin(&wire);

  std::string body;
  for (int i = 0; i < 300; i++) body += (char)((i * 7) & 0xFF);
  MemorySource from(body);

  move.start(&from, "Robin_nano35.bin", wire.clock);
  for (uint32_t i = 0; i < 400000 && move.running(); i++) {
    sim.tick(wire.clock);
    move.service(wire.clock);
    wire.advance();
  }
  fclose(log);
  fprintf(stderr, "%s, %u bytes written\n",
          move.phase == BinaryDone ? "done" : "failed", (unsigned)move.written);
  return move.phase == BinaryDone ? 0 : 1;
}
