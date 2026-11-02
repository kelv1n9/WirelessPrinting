#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>

// There is no serial port to watch and no coredump partition, so a panic leaves nothing
// behind but the word "panic" in the boot reason. Every investigation then costs a
// reproduction run, and the fault is probabilistic.
//
// This keeps the last few requests in RTC memory, which the chip does not clear on a
// panic reboot, so the first thing /api/diag shows after a crash is what the server was
// doing when it went down. Walking a task stack was tried before and was itself what
// took the module down; nothing here reads anything but its own words.

#define WITNESS_SLOTS 16
#define WITNESS_URL   40
#define WITNESS_MAGIC 0x57314E35

struct WitnessEntry {
  char url[WITNESS_URL];
  uint32_t atMs;
  uint32_t block;      // Largest free piece when the request arrived
  uint32_t heap;
  uint8_t inFlight;    // How many requests were already being handled
  uint8_t done;        // Cleared on arrival, set when the handler returned
};

RTC_NOINIT_ATTR static uint32_t witnessMagic;
RTC_NOINIT_ATTR static uint32_t witnessSeq;
RTC_NOINIT_ATTR static WitnessEntry witnessRing[WITNESS_SLOTS];
RTC_NOINIT_ATTR static uint32_t witnessBootSeq;

// The ring keeps being written after a reboot, and the traffic that caused the crash is
// usually still running, so the evidence is gone within a second of coming back up. This
// is the ring as it was at the moment of the reboot, copied once and never touched again.
RTC_NOINIT_ATTR static WitnessEntry witnessBefore[WITNESS_SLOTS];
RTC_NOINIT_ATTR static uint32_t witnessBeforeSeq;

// Where inside a handler the processor was. Handlers run one at a time in the server's
// task, so one word is enough to say which step the last one reached.
RTC_NOINIT_ATTR static uint32_t witnessStepMark;
RTC_NOINIT_ATTR static uint32_t witnessStepBefore;

// How little room the web server's task had left on its stack. FreeRTOS answers a stack
// overflow with a panic reboot and nothing else, which looks exactly like the fault being
// chased, so the figure has to be kept somewhere a reboot does not wipe.
RTC_NOINIT_ATTR static uint32_t witnessStackLow;
RTC_NOINIT_ATTR static uint32_t witnessStackBefore;
RTC_NOINIT_ATTR static uint32_t witnessBlockLow;
RTC_NOINIT_ATTR static uint32_t witnessBlockBefore;

inline void witnessStep(const uint32_t code) {
  witnessStepMark = code;
}

static volatile uint8_t witnessInFlight;

// Called before the ring is written for the first time in this boot, so that a power on
// does not read somebody else's leftovers as a crash report.
inline void witnessBegin() {
  if (witnessMagic != WITNESS_MAGIC) {
    witnessMagic = WITNESS_MAGIC;
    witnessSeq = 0;
    witnessBootSeq = 0;
    memset(witnessRing, 0, sizeof(witnessRing));
    memset(witnessBefore, 0, sizeof(witnessBefore));
    witnessBeforeSeq = 0;
    witnessStepBefore = 0;
    witnessStackBefore = 0;
    witnessBlockBefore = 0;
  }
  else {
    memcpy(witnessBefore, witnessRing, sizeof(witnessBefore));
    witnessBeforeSeq = witnessSeq;
    witnessStepBefore = witnessStepMark;
    witnessStackBefore = witnessStackLow;
    witnessBlockBefore = witnessBlockLow;
  }
  witnessStepMark = 0;
  witnessStackLow = 0xFFFFFFFF;
  witnessBlockLow = 0xFFFFFFFF;
  witnessSeq = 0;
  memset(witnessRing, 0, sizeof(witnessRing));
  ++witnessBootSeq;
}

// Called from the main loop. Copies the two figures that only mean something after a
// crash into memory the crash does not clear.
inline void witnessWatch(const uint32_t stackLeft, const uint32_t block) {
  if (stackLeft < witnessStackLow)
    witnessStackLow = stackLeft;
  if (block < witnessBlockLow)
    witnessBlockLow = block;
}

inline uint32_t witnessArrived(const char *url) {
  const uint32_t at = witnessSeq++;
  WitnessEntry &slot = witnessRing[at % WITNESS_SLOTS];

  strncpy(slot.url, url ? url : "?", WITNESS_URL - 1);
  slot.url[WITNESS_URL - 1] = 0;
  slot.atMs = millis();
  slot.block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  slot.heap = ESP.getFreeHeap();
  slot.inFlight = witnessInFlight;
  slot.done = 0;
  if (witnessInFlight < 255)
    ++witnessInFlight;

  return at;
}

// The index the spy handed the request that is being served right now. A handler that
// wants its own disconnect callback has to carry this one along, because onDisconnect
// keeps a single function and the later call replaces the earlier.
inline uint32_t witnessHere() {
  return witnessSeq ? witnessSeq - 1 : 0;
}

inline void witnessLeft(const uint32_t at) {
  witnessRing[at % WITNESS_SLOTS].done = 1;
  if (witnessInFlight)
    --witnessInFlight;
}

// Oldest first. A line without "done" is a request that never came back, which after a
// panic is the one worth looking at.
static void witnessSpell(String &out, const WitnessEntry *ring, const uint32_t end, const char *when) {
  const uint32_t first = end > WITNESS_SLOTS ? end - WITNESS_SLOTS : 0;
  for (uint32_t at = first; at < end; ++at) {
    const WitnessEntry &slot = ring[at % WITNESS_SLOTS];
    if (!slot.url[0])
      continue;
    out += "  ";
    out += when;
    out += " ";
    out += String(slot.atMs);
    out += "ms ";
    out += slot.done ? "answered        " : "NEVER CAME BACK ";
    out += slot.url;
    out += " block ";
    out += String(slot.block);
    out += " heap ";
    out += String(slot.heap);
    out += " alongside ";
    out += String(slot.inFlight);
    out += "\n";
  }
}

// What the server was doing just before the last reboot, then what it has done since.
inline String witnessReport() {
  if (witnessMagic != WITNESS_MAGIC)
    return "  nothing recorded\n";

  String out;
  out.reserve(2 * WITNESS_SLOTS * 100 + 64);
  out += "  step before the reboot ";
  out += String(witnessStepBefore);
  out += ", step now ";
  out += String(witnessStepMark);
  out += "\n";
  out += "  server stack left before the reboot ";
  out += String(witnessStackBefore);
  out += ", now ";
  out += String(witnessStackLow);
  out += "\n";
  out += "  largest free piece before the reboot ";
  out += String(witnessBlockBefore);
  out += ", now ";
  out += String(witnessBlockLow);
  out += "\n";
  witnessSpell(out, witnessBefore, witnessBeforeSeq, "BEFORE THE REBOOT");
  witnessSpell(out, witnessRing, witnessSeq, "this boot        ");

  return out == "" ? "  nothing recorded\n" : out;
}
