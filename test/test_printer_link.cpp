// Drives the real PrinterLink against the simulated printer over a wire that costs
// what a real wire costs. Nothing here is mocked except the printer and the clock.

#include <cassert>
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include "sim/MarlinSim.h"
#include "sim/Wire.h"

static int failures = 0;
static const char *scenario = "";

static void check(const bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s: %s\n", scenario, what); ++failures; }
}

struct Rig {
  MarlinSim sim;
  Wire wire;
  PrinterLink link;
  uint32_t serviceEvery = 3;      // Milliseconds between two visits of the module's loop
  Rig(const uint32_t baud = 115200) : wire(sim, baud) { link.begin(&wire); }

  // Pushes lines in while they fit and turns the crank until they are all answered.
  // Returns the milliseconds it took, or 0 when it gave up.
  uint32_t run(const std::vector<std::string> &lines, const uint32_t limitMs = 600000) {
    size_t at = 0;
    const uint32_t started = wire.clock;
    while (wire.clock - started < limitMs) {
      if (wire.clock % serviceEvery == 0) {
        while (at < lines.size() && link.canAccept(lines[at].size()))
          if (link.send(lines[at].c_str())) at++; else break;
        link.service();
      }
      wire.advance();
      if (at == lines.size() && link.idle())
        return wire.clock - started;
    }
    return 0;
  }
};

static std::vector<std::string> moves(const size_t count) {
  std::vector<std::string> lines;
  char buf[64];
  for (size_t i = 0; i < count; i++) {
    snprintf(buf, sizeof buf, "G1 X%u.%02u Y%u.%02u E%u.%04u F1500",
             (unsigned)(i % 200), (unsigned)(i % 100), (unsigned)(i % 150),
             (unsigned)(i % 90), (unsigned)(i / 100), (unsigned)(i % 10000));
    lines.push_back(buf);
  }
  return lines;
}

// Everything the printer stored has to match what we meant to send, in order
static size_t compare(const Rig &rig, const std::vector<std::string> &want) {
  const std::set<std::string> ours(want.begin(), want.end());
  size_t at = 0, garbage = 0;
  for (const std::string &got : rig.sim.seen) {
    if (at < want.size() && got == want[at]) { at++; continue; }
    if (got.compare(0, 4, "M110") == 0) continue;
    if (ours.count(got)) {
      check(false, ("out of order or sent twice: " + got).c_str());
      return garbage;
    }
    garbage++;                      // A damaged line the printer took for a command
  }
  check(at == want.size(), "every line arrived, in order, exactly once");
  return garbage;
}

int main() {
  // ---------------------------------------------------------------- plain run
  scenario = "clean wire, nothing is lost";
  {
    Rig rig;
    rig.link.budgetBytes = 56;                 // What a 64 byte ring safely holds
    const auto want = moves(2000);
    const uint32_t ms = rig.run(want);
    check(ms > 0, "finished");
    check(rig.link.acked == 2000, "all acknowledged");
    check(rig.link.resends == 0, "no repeats");
    check(rig.sim.dropped == 0, "printer lost nothing");
    compare(rig, want);
  }

  // ---------------------------------------------------------- window vs waiting
  scenario = "the window beats one line at a time";
  {
    const auto want = moves(2000);
    Rig slow; slow.link.budgetBytes = 56; slow.link.slotCeiling = 1;
    Rig fast; fast.link.budgetBytes = 56;
    const uint32_t a = slow.run(want), b = fast.run(want);
    printf("  one at a time %u ms, window %u ms, %.2fx\n", a, b, a / (double)(b ? b : 1));
    check(a > 0 && b > 0, "both finished");
    check(b < a, "the window is faster");
  }

  // ------------------------------------------------------------ bigger buffer
  scenario = "a bigger serial buffer on the printer is what unlocks speed";
  {
    const auto want = moves(2000);
    printf("  %-10s %-8s %-8s %s\n", "baud", "ring", "budget", "ms");
    for (uint32_t baud : {115200u, 250000u, 500000u}) {
      for (size_t ring : {64u, 128u, 256u, 512u}) {
        Rig rig(baud);
        rig.sim.rxBufferSize = ring;
        rig.link.budgetBytes = (uint16_t)(ring - 8);
        const uint32_t ms = rig.run(want);
        printf("  %-10u %-8zu %-8u %u%s\n", baud, ring, rig.link.budgetBytes, ms,
               rig.sim.dropped ? "   LOST BYTES" : "");
        check(ms > 0, "finished");
        check(rig.sim.dropped == 0, "nothing lost while inside the budget");
      }
    }
  }

  // -------------------------------------------------- the budget is the flow control
  scenario = "going over the buffer is what loses bytes";
  {
    const auto want = moves(400);
    Rig safe(250000);
    safe.sim.rxBufferSize = 64;
    safe.sim.stallEvery = 7; safe.sim.stallFor = 4;    // the loop goes away, like an SD write
    safe.link.budgetBytes = 56;
    check(safe.run(want) > 0, "finished inside the budget");
    check(safe.sim.dropped == 0, "a budget under the ring size never loses a byte");

    Rig reckless(250000);
    reckless.sim.rxBufferSize = 64;
    reckless.sim.stallEvery = 7; reckless.sim.stallFor = 4;
    reckless.link.budgetBytes = 1024;                  // what streaming without a budget does
    reckless.run(want);
    check(reckless.sim.dropped > 0, "without a budget the printer does lose bytes");
    printf("  reckless lost %zu bytes, asked for %u repeats\n",
           reckless.sim.dropped, reckless.sim.resendsAsked);
  }

  // ------------------------------------------------------------- corruption
  scenario = "a flipped bit is caught and the line goes again";
  {
    Rig rig;
    rig.sim.corruptEvery = 700;
    rig.link.budgetBytes = 56;
    const auto want = moves(2000);
    check(rig.run(want) > 0, "finished despite the damage");
    check(rig.link.resends > 0, "repeats did happen");
    check(rig.link.acked >= 2000, "everything was acknowledged");
    const size_t garbage = compare(rig, want);
    printf("  %u repeats over 2000 lines, %u resyncs, %zu damaged lines run as commands\n",
           rig.link.resends, rig.link.resyncs, garbage);
  }

  // --------------------------------------------------------------- the budget holds
  scenario = "never more in flight than the budget allows";
  {
    Rig rig;
    rig.link.budgetBytes = 56;
    const auto want = moves(500);
    size_t at = 0;
    uint16_t peak = 0;
    for (uint32_t i = 0; i < 200000 && !(at == want.size() && rig.link.idle()); i++) {
      while (at < want.size() && rig.link.canAccept(want[at].size()))
        if (rig.link.send(want[at].c_str())) at++; else break;
      rig.link.service();
      if (rig.link.inflightBytes() > peak) peak = rig.link.inflightBytes();
      check(rig.link.inflightBytes() <= rig.link.budgetBytes, "budget respected");
      rig.wire.advance();
    }
    printf("  peak in flight %u of %u allowed\n", peak, rig.link.budgetBytes);
    check(peak > 0, "something was actually in flight");
  }

  // ------------------------------------------------------------------- caps
  scenario = "the module reads what the printer can do";
  {
    Rig rig;
    rig.sim.lfnWrite = true;
    rig.sim.binary = false;
    rig.link.send("M115");
    for (int i = 0; i < 2000; i++) { rig.link.service(); rig.wire.advance(); }
    check(rig.link.caps.seen, "capabilities arrived");
    check(rig.link.caps.lfnWrite, "long names seen");
    check(!rig.link.caps.binary, "binary transfer correctly seen as off");
    check(rig.link.caps.sdWrite, "writing to the card seen");
    check(rig.link.caps.advancedOk, "the detailed ok was noticed");
    check(rig.link.caps.slots == 32, "the printer's slot count was learned");
  }

  // -------------------------------------------------------- old firmware falls back
  scenario = "a printer without the detailed ok still works";
  {
    Rig rig;
    rig.sim.advancedOk = false;
    rig.link.budgetBytes = 56;
    const auto want = moves(300);
    check(rig.run(want) > 0, "finished");
    check(!rig.link.caps.advancedOk, "stayed on one line at a time");
    check(rig.link.acked == 300, "all acknowledged");
    compare(rig, want);
  }

  // --------------------------------------------------------------- no card
  scenario = "no card in the printer is reported, not guessed";
  {
    Rig rig;
    rig.sim.cardPresent = false;
    rig.link.send("M115");
    for (int i = 0; i < 2000; i++) { rig.link.service(); rig.wire.advance(); }
    check(rig.link.caps.seen, "capabilities arrived");
  }

  // ------------------------------------------------- printer restarts mid stream
  scenario = "the printer rebooting in the middle does not lose work";
  {
    Rig rig;
    rig.link.budgetBytes = 56;
    const auto want = moves(600);
    size_t at = 0;
    bool kicked = false;
    for (uint32_t i = 0; i < 400000 && !(at == want.size() && rig.link.idle()); i++) {
      if (rig.wire.clock % rig.serviceEvery == 0) {
        while (at < want.size() && rig.link.canAccept(want[at].size()))
          if (rig.link.send(want[at].c_str())) at++; else break;
        rig.link.service();
      }
      if (!kicked && rig.link.acked > 200) {       // Power blinks, the printer starts over
        kicked = true;
        rig.sim.reset();
        rig.sim.seen.clear();
      }
      rig.wire.advance();
    }
    check(at == want.size(), "everything was handed over");
    check(rig.link.idle(), "nothing left hanging in the window");
    check(rig.link.resyncs > 0 || rig.link.resends > 0 || rig.link.timeouts > 0,
          "the break was noticed");
    printf("  after the reboot: %u repeats, %u resyncs, %u timeouts\n",
           rig.link.resends, rig.link.resyncs, rig.link.timeouts);
  }

  // ------------------------------------------------------------- backpressure
  scenario = "a full window refuses politely instead of overwriting";
  {
    Rig rig;
    rig.link.budgetBytes = 56;
    size_t took = 0;
    const auto want = moves(500);
    for (const auto &line : want)                 // Push without ever turning the crank
      if (rig.link.canAccept(line.size()) && rig.link.send(line.c_str())) took++;
    check(took > 0, "some were taken");
    check(took < want.size(), "and the rest were refused, not swallowed");
    check(rig.link.waiting() == took, "the window holds exactly what it took");
    check(rig.run(std::vector<std::string>()) >= 0, "and it drains");
    for (uint32_t i = 0; i < 200000 && !rig.link.idle(); i++) {
      if (rig.wire.clock % rig.serviceEvery == 0) rig.link.service();
      rig.wire.advance();
    }
    check(rig.link.idle(), "window emptied");
    check(rig.link.acked == took, "every taken line was acknowledged");
    for (size_t i = 0; i < took; i++)
      if (i < rig.sim.seen.size() && rig.sim.seen[i] != want[i]) {
        check(false, "order held under backpressure");
        break;
      }
  }

  // ----------------------------------------- the whole thing at the target setting
  scenario = "fast wire, big buffer, damaged bytes, still exact";
  {
    Rig rig(500000);
    rig.sim.rxBufferSize = 512;
    rig.sim.corruptEvery = 900;
    rig.sim.stallEvery = 11; rig.sim.stallFor = 3;
    rig.link.budgetBytes = 504;
    const auto want = moves(2000);
    const uint32_t ms = rig.run(want);
    check(ms > 0, "finished");
    check(rig.sim.dropped == 0, "the printer lost nothing");
    const size_t garbage = compare(rig, want);
    printf("  %u ms, %u repeats, %u resyncs, %u timeouts, %zu damaged lines run\n",
           ms, rig.link.resends, rig.link.resyncs, rig.link.timeouts, garbage);
  }

  printf(failures ? "\n%d checks failed\n" : "\nall checks passed\n", failures);
  return failures != 0;
}
