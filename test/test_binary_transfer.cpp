// Puts a firmware image on the simulated printer's card with Marlin's own binary
// protocol and demands it arrive byte for byte. This is the path that replaces pulling
// the card out of the printer, so a single wrong byte here is a printer that will not
// start, and the only way back is the card, by hand.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "sim/MarlinSim.h"
#include "sim/Wire.h"
#include "../ESP8266WirelessPrintAsync/BinaryTransfer.h"

static int failures = 0;
static const char *scenario = "";
static void check(const bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s: %s\n", scenario, what); ++failures; }
}

struct MemorySource : ByteSource {
  std::string body;
  size_t at = 0;
  explicit MemorySource(std::string from) : body(std::move(from)) {}
  int next(uint8_t *out, const size_t max) override {
    const size_t left = body.size() - at;
    const size_t take = left < max ? left : max;
    memcpy(out, body.data() + at, take);
    at += take;
    return (int)take;
  }
  uint32_t size() const override { return (uint32_t)body.size(); }
};

struct Rig {
  MarlinSim sim;
  Wire wire;
  BinaryTransfer move;

  explicit Rig(const uint32_t baud = 115200) : wire(sim, baud) {
    sim.binary = true;
    move.begin(&wire);
  }

  // Runs until the transfer settles, returning the milliseconds it took
  uint32_t run(MemorySource &from, const char *name, const uint32_t patience = 600000,
               const bool pack = true) {
    move.start(&from, name, wire.clock, pack);
    for (uint32_t i = 0; i < patience; i++) {
      sim.tick(wire.clock);
      move.service(wire.clock);
      wire.advance();
      if (move.phase == BinaryDone || move.phase == BinaryFailed)
        return wire.clock;
    }
    return 0;
  }
};

// Something that looks like a firmware image: every byte value, including the ones that
// mean something to the ASCII protocol, and plenty of them.
static std::string image(const size_t bytes) {
  std::string out;
  out.reserve(bytes);
  for (size_t i = 0; i < bytes; i++)
    out += (char)((i * 7 + (i >> 8) * 13) & 0xFF);
  return out;
}

int main() {
  scenario = "a small image lands whole";
  {
    Rig rig;
    const std::string body = image(1000);
    MemorySource from(body);
    const uint32_t ms = rig.run(from, "Robin_nano35.bin");
    check(rig.move.phase == BinaryDone, "it finished");
    if (rig.move.trouble) printf("  said: %s\n", rig.move.trouble);
    check(rig.sim.card.count("Robin_nano35.bin") == 1, "the printer has the file");
    check(rig.sim.card["Robin_nano35.bin"] == body, "and it is byte for byte the same");
    check(rig.move.written == body.size(), "the module counted every byte");
    check(!rig.sim.binaryMode, "the printer is back to reading gcode");
    printf("  %u bytes in %u ms, %u packets\n",
           (unsigned)body.size(), ms, (unsigned)rig.move.packets);
  }

  scenario = "every byte value survives, nothing is treated as text";
  {
    Rig rig;
    std::string body;
    for (int i = 0; i < 256; i++) body += (char)i;
    body += body;                      // and again, so a lost newline would show
    MemorySource from(body);
    rig.run(from, "ALL.BIN");
    check(rig.move.phase == BinaryDone, "it finished");
    check(rig.sim.card["ALL.BIN"] == body, "all 256 values came through unchanged");
  }

  scenario = "an image the size of the real firmware lands whole";
  {
    Rig rig(250000);
    const std::string body = image(293860);      // what the printer's build actually weighs
    MemorySource from(body);
    const uint32_t ms = rig.run(from, "Robin_nano35.bin", 2000000);
    check(rig.move.phase == BinaryDone, "it finished");
    check(rig.sim.card["Robin_nano35.bin"] == body, "byte for byte");
    printf("  %u bytes in %u ms at 250000, %u packets, %u resends\n",
           (unsigned)body.size(), ms, (unsigned)rig.move.packets, (unsigned)rig.move.resends);
  }

  scenario = "a damaged wire costs time, never correctness";
  {
    Rig rig;
    rig.sim.binaryCorruptEvery = 700;            // a flipped bit every so often
    const std::string body = image(8000);
    MemorySource from(body);
    rig.run(from, "NOISY.BIN", 2000000);
    check(rig.move.phase == BinaryDone, "it still finished");
    if (rig.move.trouble) printf("  said: %s\n", rig.move.trouble);
    check(rig.sim.card["NOISY.BIN"] == body, "and what landed is right");
    check(rig.sim.binaryResendsAsked > 0, "the printer did ask for repeats");
    printf("  %u repeats asked over %u packets\n",
           (unsigned)rig.sim.binaryResendsAsked, (unsigned)rig.move.packets);
  }

  scenario = "bytes that never arrive are noticed and asked for again";
  {
    Rig rig;
    rig.sim.binaryDropEvery = 900;
    const std::string body = image(8000);
    MemorySource from(body);
    rig.run(from, "LOSSY.BIN", 4000000);
    check(rig.move.phase == BinaryDone, "it still finished");
    if (rig.move.trouble) printf("  said: %s\n", rig.move.trouble);
    check(rig.sim.card["LOSSY.BIN"] == body, "and what landed is right");
    printf("  %u repeats asked\n", (unsigned)rig.sim.binaryResendsAsked);
  }

  scenario = "a printer built without the protocol is not left half open";
  {
    Rig rig;
    rig.sim.binary = false;                      // the firmware in the printer today
    const std::string body = image(500);
    MemorySource from(body);
    rig.run(from, "NOPE.BIN", 200000);
    check(rig.move.phase == BinaryFailed, "it gave up");
    check(rig.sim.card.count("NOPE.BIN") == 0, "and wrote nothing");
    check(rig.sim.card.count("B1") == 0, "and left no stray file called B1 behind");
    check(rig.move.trouble && strstr(rig.move.trouble, "no binary transfer"),
          "and said why, rather than blaming silence");
    if (rig.move.trouble) printf("  said: %s\n", rig.move.trouble);
  }

  scenario = "a printer with no card refuses before a byte is sent";
  {
    Rig rig;
    rig.sim.cardPresent = false;
    const std::string body = image(500);
    MemorySource from(body);
    rig.run(from, "NOCARD.BIN", 200000);
    check(rig.move.phase == BinaryFailed, "it gave up");
    check(rig.move.written == 0, "nothing was counted as written");
    check(rig.move.trouble && strstr(rig.move.trouble, "open the file"),
          "and blamed the file, not the feature: this printer has the protocol");
    if (rig.move.trouble) printf("  said: %s\n", rig.move.trouble);
  }

  scenario = "stopping half way leaves nothing behind on the card";
  {
    Rig rig;
    const std::string body = image(20000);
    MemorySource from(body);
    rig.move.start(&from, "HALF.BIN", rig.wire.clock);
    for (uint32_t i = 0; i < 400000; i++) {
      rig.sim.tick(rig.wire.clock);
      rig.move.service(rig.wire.clock);
      rig.wire.advance();
      if (rig.move.written > 4000) break;
    }
    check(rig.move.phase == BinaryWriting, "it was in the middle of the file");
    rig.move.stop();
    for (uint32_t i = 0; i < 200000; i++) {       // the loop keeps servicing it
      rig.sim.tick(rig.wire.clock);
      rig.move.service(rig.wire.clock);
      rig.wire.advance();
      if (!rig.move.running()) break;
    }
    check(rig.move.phase == BinaryFailed, "it ended as a failure, not a success");
    check(rig.sim.card.count("HALF.BIN") == 0, "the printer threw the stub away");
    check(!rig.sim.binaryOpen, "and has no file open");
  }

  scenario = "the printer tells us how much it can take and we believe it";
  {
    Rig rig;
    rig.sim.binaryPayload = 32;                  // a firmware built with a smaller buffer
    const std::string body = image(2000);
    MemorySource from(body);
    rig.run(from, "SMALL.BIN", 2000000);
    check(rig.move.phase == BinaryDone, "it finished");
    check(rig.move.theirPayload == 32, "the module took the printer's word for it");
    check(rig.sim.card["SMALL.BIN"] == body, "byte for byte");
  }

  scenario = "an empty file is still a file";
  {
    Rig rig;
    MemorySource from(std::string(""));
    rig.run(from, "EMPTY.BIN", 200000);
    check(rig.move.phase == BinaryDone, "it finished");
    check(rig.sim.card.count("EMPTY.BIN") == 1, "and the printer has it");
    check(rig.sim.card["EMPTY.BIN"].empty(), "empty, as asked");
  }

  scenario = "a printer card error closes packet mode and removes the partial file";
  {
    Rig rig;
    rig.sim.binaryIoErrorAfter = 4096;
    rig.sim.binaryWriteDelayMs = 2;
    const std::string body = image(10000);
    MemorySource from(body);
    rig.run(from, "BROKEN.BIN", 200000, false);
    check(rig.move.phase == BinaryFailed, "the sender reports failure");
    check(rig.move.trouble && strstr(rig.move.trouble, "could not write"), "the card error is reported");
    check(!rig.sim.binaryMode, "the printer is back in command mode");
    check(!rig.sim.binaryOpen, "the printer closed the file");
    check(rig.sim.card.count("BROKEN.BIN") == 0, "the partial file was removed");
  }

#ifdef MARLIN_SIM_HEATSHRINK
  scenario = "a sliced file crosses a fast serial link without changing a byte";
  {
    std::ifstream file("cold_cube_head.gcode", std::ios::binary);
    const std::string sample((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    check(!sample.empty(), "the real gcode sample is present");
    if (!sample.empty()) {
      std::string body;
      body.reserve(sample.size() * 25);
      for (int i = 0; i < 25; i++) body += sample;
      Rig rig(2000000);
      rig.sim.binaryPayload = 1024;
      rig.sim.rxBufferSize = 1280;
      rig.sim.binaryWriteDelayMs = 2;
      rig.sim.stallEvery = 50;
      rig.sim.stallFor = 5;
      MemorySource from(body);
      const uint32_t ms = rig.run(from, "FAST.GCO");
      check(rig.move.phase == BinaryDone, "the transfer finishes");
      check(rig.sim.card["FAST.GCO"] == body, "the printer card matches the original");
      check(rig.sim.dropped == 0, "the UART ring did not overflow");
      check(rig.move.resends == 0, "the printer requested no repeats");
      const uint32_t bytesPerSecond = ms ? (uint32_t)(body.size() * 1000 / ms) : 0;
      check(bytesPerSecond >= 200000, "the model reaches 200 kB/s");
      printf("  sliced file: %u bytes in %u ms, %u kB/s\n",
             (unsigned)body.size(), ms, bytesPerSecond / 1000);
    }
  }
#endif

  printf(failures ? "\n%d checks failed\n" : "\nall checks passed\n", failures);
  return failures != 0;
}
