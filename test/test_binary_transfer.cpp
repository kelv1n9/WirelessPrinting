// Puts a firmware image on the simulated printer's card with Marlin's own binary
// protocol and demands it arrive byte for byte. This is the path that replaces pulling
// the card out of the printer, so a single wrong byte here is a printer that will not
// start, and the only way back is the card, by hand.

#include <cstdio>
#include <algorithm>
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

struct PipelineWire : Wire {
  using Wire::Wire;
  std::deque<uint8_t> pending;
  std::string answer;
  unsigned peak = 0, writeCalls = 0, dropWriteAt = 0, corruptWriteAt = 0;
  bool shutWithPending = false;

  size_t write(const uint8_t *data, size_t len) override {
    const bool frame = len >= 8 && data[0] == 0xAD && data[1] == 0xB5;
    const bool writing = frame && data[3] == 0x13;
    size_t took;
    if (writing && ++writeCalls == dropWriteAt)
      took = len;
    else if (writing && writeCalls == corruptWriteAt) {
      std::vector<uint8_t> damaged(data, data + len);
      damaged.back() ^= 1;
      took = Wire::write(damaged.data(), len);
    }
    else
      took = Wire::write(data, len);
    if (took == len && writing) {
      if (std::find(pending.begin(), pending.end(), data[2]) == pending.end())
        pending.push_back(data[2]);
      if (pending.size() > peak) peak = pending.size();
    }
    if (took == len && frame && data[3] == 0x12 && !pending.empty())
      shutWithPending = true;
    return took;
  }

  int read() override {
    const int ch = Wire::read();
    if (ch < 0) return ch;
    if (ch == '\n' || ch == '\r') {
      unsigned seq;
      char extra;
      if (sscanf(answer.c_str(), "ok%u%c", &seq, &extra) == 1 && seq <= 255) {
        const auto found = std::find(pending.begin(), pending.end(), uint8_t(seq));
        if (found != pending.end()) pending.erase(pending.begin(), found + 1);
      }
      else if (sscanf(answer.c_str(), "ss%u", &seq) == 1 && seq <= 255 && !pending.empty()) {
        const auto found = std::find(pending.begin(), pending.end(), uint8_t(seq));
        if (found != pending.end()) pending.erase(pending.begin(), found);
        else if (uint8_t(pending.back() + 1) == seq) pending.clear();
      }
      answer.clear();
    }
    else
      answer += char(ch);
    return ch;
  }
};

struct Rig {
  MarlinSim sim;
  PipelineWire wire;
  BinaryTransfer move;

  explicit Rig(const uint32_t baud = 115200) : wire(sim, baud) {
    sim.binary = true;
    wire.txRoom = 4096;
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

static std::string noise(const size_t bytes) {
  std::string out(bytes, '\0');
  uint32_t state = 0x93471;
  for (char &c : out) {
    state = state * 1664525U + 1013904223U;
    c = char(state >> 24);
  }
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

  for (const bool loseAck : {false, true}) {
    scenario = loseAck ? "lost binary exit acknowledgement" : "lost binary exit packet";
    Rig rig;
    rig.sim.binaryCloseDrops = loseAck ? 0 : 1;
    rig.sim.binaryCloseAckDrops = loseAck ? 1 : 0;
    const std::string body = image(1000);
    MemorySource from(body);
    rig.run(from, "EXIT.BIN", 20000, false);
    check(rig.move.phase == BinaryDone, "the completed file is confirmed");
    check(!rig.sim.binaryMode, "ordinary commands work after completion");
    check(rig.move.resends == 1, "the missing exit response caused a retry");
    check(rig.sim.card["EXIT.BIN"] == body, "the completed file remains intact");
  }

  scenario = "source failure while preparing ahead closes the partial file";
  {
    struct BrokenSource : MemorySource {
      using MemorySource::MemorySource;
      int next(uint8_t *out, size_t max) override {
        return at >= 8192 ? -1 : MemorySource::next(out, max);
      }
    } from(image(30000));
    Rig rig;
    rig.sim.binaryWriteDelayMs = 10;
    rig.run(from, "SOURCE.BIN", 100000);
    check(rig.move.phase == BinaryFailed, "a read error cannot complete a transfer");
    check(!rig.sim.binaryMode && !rig.sim.binaryOpen, "the wire and file are released");
    check(!rig.sim.card.count("SOURCE.BIN"), "the partial file is removed");
  }

  for (const unsigned payload : {96U, 1024U}) {
    for (const bool packed : {false, true}) {
      for (unsigned fault = 0; fault < 9; ++fault) {
        scenario = "window two retains exact bytes through wrap and faults";
        Rig rig(2000000);
        rig.sim.binaryWindow = 2;
        rig.sim.binaryPayload = payload;
        rig.sim.rxBufferSize = 2304;
        rig.sim.binaryWriteDelayMs = 3;
        if (fault == 1) rig.wire.dropWriteAt = 1;
        if (fault == 2) rig.wire.dropWriteAt = 2;
        if (fault == 3) rig.wire.corruptWriteAt = 1;
        if (fault == 4) rig.wire.corruptWriteAt = 2;
        if (fault == 5) rig.sim.binaryWriteAckDropAt = 1;
        if (fault == 6) rig.sim.binaryWriteAckDrops = 2;
        if (fault == 7) rig.sim.binaryWriteAckCorruptEvery = 37;
        if (fault == 8) { rig.sim.stallEvery = 50; rig.sim.stallFor = 15; }
        const auto bytes = noise(payload * 270 + 37);
        MemorySource from(bytes);
        const auto elapsed = rig.run(from, "WINDOW.BIN", 200000, packed);
        if (rig.move.phase != BinaryDone)
          printf("  payload %u packed %d fault %u phase %u trouble %s\n", payload, packed, fault,
                 unsigned(rig.move.phase), rig.move.trouble ? rig.move.trouble : "none");
        check(elapsed && rig.move.phase == BinaryDone, "window two finishes within its bound");
        check(rig.move.windowUsed == 2, "QUERY enabled window two");
        check(rig.wire.peak == 2, "exactly two WRITE sequences were outstanding at peak");
        check(!rig.wire.shutWithPending, "all WRITE acknowledgements precede FILE CLOSE");
        check(rig.sim.card["WINDOW.BIN"] == bytes, "no source bytes were lost or duplicated");
        check(rig.sim.binaryWritePackets > 256, "WRITE sequence crossed modulo 256");
        check(rig.sim.binaryWritePackets == (rig.move.sent + payload - 1) / payload,
              "each unique WRITE was dispatched exactly once");
        check(!rig.sim.binaryMode && !rig.sim.binaryOpen && !rig.sim.dropped, "the wire closes without overflow");
        check(rig.move.resends <= BINARY_MAX_RESENDS, "recovery stayed bounded");
        if (fault == 5) check(rig.move.resends == 0, "a later cumulative ACK covers the missing older ACK");
        if (!fault) check(!rig.move.resends, "a clean window needs no recovery");
#ifdef MARLIN_SIM_HEATSHRINK
        check(rig.move.packing == packed, "requested packed data used Marlin's real decoder");
#endif
      }
    }
  }

  for (const uint8_t cap : {uint8_t(1), uint8_t(2)}) for (const uint8_t limit : {uint8_t(1), uint8_t(2)}) {
    scenario = "negotiated window respects both endpoints";
    Rig rig(2000000);
    rig.sim.binaryWindow = cap;
    rig.sim.binaryPayload = 1024;
    rig.sim.rxBufferSize = 2304;
    rig.sim.binaryWriteDelayMs = 10;
    rig.move.windowLimit = limit;
    MemorySource from(noise(20000));
    rig.run(from, "LIMIT.BIN", 100000, false);
    const auto used = std::min(cap, limit);
    check(rig.move.phase == BinaryDone && rig.move.windowUsed == used, "the agreed window is the smaller cap");
    check(rig.wire.peak == used && rig.sim.card["LIMIT.BIN"] == from.body, "fallback preserves bytes and bound");
  }

  for (const unsigned payload : {96U, 1024U}) for (unsigned lost = 254; lost <= 257; ++lost) {
    for (unsigned fault = 0; fault < 3; ++fault) {
      scenario = "recovery and cumulative ACK cross the sequence wrap";
      Rig rig(2000000);
      rig.sim.binaryWindow = 2;
      rig.sim.binaryPayload = payload;
      rig.sim.rxBufferSize = 2304;
      rig.sim.binaryWriteDelayMs = 2;
      if (fault == 0) rig.wire.dropWriteAt = lost;
      if (fault == 1) rig.wire.corruptWriteAt = lost;
      if (fault == 2) rig.sim.binaryWriteAckDropAt = lost;
      MemorySource from(noise(payload * 270 + 1));
      rig.run(from, "WRAP.BIN", 100000, false);
      check(rig.move.phase == BinaryDone && rig.sim.card["WRAP.BIN"] == from.body, "wrap recovery is byte exact");
      check(rig.wire.peak == 2 && !rig.wire.shutWithPending && !rig.sim.dropped, "wrap preserves the window and close bound");
    }
  }

  for (const unsigned length : {0U, 1U, 1024U, 1025U}) {
    scenario = "empty and final partial windows drain before close";
    Rig rig(2000000);
    rig.sim.binaryWindow = 2;
    rig.sim.binaryPayload = 1024;
    rig.sim.rxBufferSize = 2304;
    rig.sim.binaryWriteDelayMs = 15;
    MemorySource from(noise(length));
    rig.run(from, "TAIL.BIN", 100000, false);
    check(rig.move.phase == BinaryDone && rig.sim.card["TAIL.BIN"] == from.body, "final window preserves every byte");
    check(!rig.wire.shutWithPending && !rig.sim.binaryMode, "close waits for the last SD acknowledgement");
  }

  for (unsigned fault = 0; fault < 5; ++fault) {
    scenario = "two pending writes are released on errors and cancellation";
    struct Source : MemorySource {
      using MemorySource::MemorySource;
      bool broken = false;
      int next(uint8_t *out, size_t max) override {
        return broken && at >= 8192 ? -1 : MemorySource::next(out, max);
      }
    } from(noise(40000));
    Rig rig(2000000);
    rig.sim.binaryWindow = 2;
    rig.sim.binaryPayload = 1024;
    rig.sim.rxBufferSize = 2304;
    rig.sim.binaryWriteDelayMs = 20;
    if (fault == 0) from.broken = true;
    if (fault == 1) rig.sim.binaryIoErrorAfter = 4096;
    if (fault == 2) rig.sim.binaryCorruptEvery = 13;
    if (fault == 3) rig.sim.binaryWriteDelayMs = 3000;
    rig.move.start(&from, "FAIL.BIN", rig.wire.clock, fault == 0);
    for (unsigned i = 0; i < 300000 && rig.move.running(); ++i) {
      rig.sim.tick(rig.wire.clock);
      rig.move.service(rig.wire.clock);
      if (fault == 2 && rig.move.phase == BinaryRecoverSync) rig.sim.binaryCorruptEvery = 0;
      if ((fault == 3 || fault == 4) && rig.wire.peak == 2) rig.move.stop();
      rig.wire.advance();
    }
    check(rig.move.phase == BinaryFailed, "failed transfer settles within its bound");
    check(!rig.sim.binaryMode && !rig.sim.binaryOpen, "error cleanup restores ordinary commands");
    check(!rig.sim.card.count("FAIL.BIN"), "the failed partial file is removed");
    check(rig.wire.peak <= 2, "cleanup did not create another WRITE sequence");
  }

  for (const bool loseExitAck : {false, true}) {
    scenario = "FILE CLOSE ACK loss still confirms leaving binary mode";
    Rig rig(2000000);
    rig.sim.binaryWindow = 2;
    rig.sim.binaryPayload = 1024;
    rig.sim.rxBufferSize = 2304;
    rig.sim.binaryShutAckDrops = 1;
    rig.sim.binaryCloseAckDrops = loseExitAck;
    MemorySource from(noise(15000));
    rig.run(from, "CLOSE.BIN", 30000, false);
    check(rig.move.phase == BinaryDone && !rig.sim.binaryMode, "lost FILE CLOSE ACK cannot leave packet mode active");
    check(rig.sim.card["CLOSE.BIN"] == from.body && !rig.wire.shutWithPending, "close follows every confirmed byte");
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
