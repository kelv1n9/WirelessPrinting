#pragma once

// Puts a file on the printer's own SD card and checks afterwards that it got there whole.
// Knows nothing about files or the web, only about lines and the link, so it runs against
// the simulated printer on a computer exactly as it runs on the module.
//
// Two things about M28 shape all of this. While the write is open Marlin stores whatever
// arrives instead of executing it, so nothing else may be sent for the whole transfer.
// And the printer stands still the whole time, so the dead man timer has to come off.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "PrinterLink.h"
#include "CardIndex.h"

#define CARD_TARGET_BYTES   CARD_NAME_BYTES
#define CARD_OPEN_WAIT      15000
#define CARD_CLOSE_WAIT     30000
#define CARD_LIST_WAIT      20000
#define CARD_CONTROL_SLOTS  6

enum CardPhase : uint8_t {
  CardIdle, CardOpening, CardStreaming, CardClosing, CardChecking, CardDone, CardFailed
};

// Where the lines come from. One implementation reads the module's card, the other a
// string in a test.
struct LineSource {
  virtual ~LineSource() {}
  virtual int next(char *out, size_t max) = 0;   // Length of the line, or -1 at the end
  virtual uint32_t used() const = 0;             // Bytes of the source taken so far
};

class CardTransfer {
public:
  PrinterLink *link = nullptr;
  CardIndex *index = nullptr;
  int deadManSeconds = 0;        // 0 leaves the timer alone

  CardPhase phase = CardIdle;
  uint32_t written = 0;          // What the printer should end up holding
  uint32_t lines = 0, skipped = 0, total = 0, startedAt = 0, finishedAt = 0;
  const char *trouble = "";
  char target[CARD_TARGET_BYTES] = {0};
  bool printAfter = false;

  bool running() const { return phase > CardIdle && phase < CardDone; }

  // Done is not the same as finished: putting the dead man timer back and starting the
  // print are still queued behind whatever was in the window.
  bool quiet() const { return controlTail == controlHead && (!link || link->idle()); }

  bool start(LineSource *from, const char *name, const uint32_t totalBytes,
             const bool thenPrint, const uint32_t now) {
    if (!link || !from || running())
      return false;
    size_t n = strlen(name);
    if (!n || n >= CARD_TARGET_BYTES)
      return false;

    source = from;
    memcpy(target, name, n + 1);
    total = totalBytes;
    printAfter = thenPrint;
    written = lines = skipped = 0;
    trouble = "";
    stopWanted = false;
    opened = false;
    startedAt = changedAt = now;
    finishedAt = 0;

    controlHead = controlTail = 0;
    if (deadManSeconds)
      pushControl("M85 S0");
    pushControl("M21");
    char open[CARD_TARGET_BYTES + 8];
    snprintf(open, sizeof open, "M28 %s", target);
    pushControl(open);
    phase = CardOpening;
    return true;
  }

  // Asked for from anywhere, acted on by the service loop, which owns the source
  void stop() { stopWanted = true; }

  void observe(const char *line) {
    if (phase == CardOpening) {
      if (!strncmp(line, "Writing to file", 15)) {
        opened = true;
        return;
      }
      if (!strncmp(line, "open failed", 11) || !strncmp(line, "Error:", 6)) {
        trouble = "the printer would not open the file";
        phase = CardFailed;
        return;
      }
    }
    if (index)
      index->observe(line);
  }

  void service(const uint32_t now) {
    // A control command that the window had no room for would simply vanish, and a lost
    // M29 leaves the file on the card open while everything after it is written into it.
    if (!flushControl())
      return;
    switch (phase) {
      case CardOpening:   serveOpening(now);   break;
      case CardStreaming: serveStreaming(now); break;
      case CardClosing:   serveClosing(now);   break;
      case CardChecking:  serveChecking(now);  break;
      default: break;
    }
  }

  // Marlin writes the line it stored followed by a carriage return and a newline
  static size_t strip(char *line) {
    char *comment = strchr(line, ';');
    if (comment) *comment = 0;
    size_t n = strlen(line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r'))
      line[--n] = 0;
    size_t lead = 0;
    while (line[lead] == ' ' || line[lead] == '\t') lead++;
    if (lead) { memmove(line, line + lead, n - lead + 1); n -= lead; }
    return n;
  }

  uint8_t controlHead = 0, controlTail = 0;

private:
  LineSource *source = nullptr;
  uint32_t changedAt = 0;
  bool stopWanted = false, opened = false;
  char work[LINK_LINE_BYTES];
  char control[CARD_CONTROL_SLOTS][CARD_TARGET_BYTES + 8];

  bool pushControl(const char *cmd) {
    const uint8_t next = (uint8_t)((controlHead + 1) % CARD_CONTROL_SLOTS);
    if (next == controlTail)
      return false;
    size_t n = strlen(cmd);
    if (n >= sizeof control[0]) n = sizeof control[0] - 1;
    memcpy(control[controlHead], cmd, n);
    control[controlHead][n] = 0;
    controlHead = next;
    return true;
  }

  // True once the queue is empty. Anything it could not hand over is tried again later.
  bool flushControl() {
    while (controlTail != controlHead) {
      if (!link->send(control[controlTail]))
        return false;
      controlTail = (uint8_t)((controlTail + 1) % CARD_CONTROL_SLOTS);
    }
    return true;
  }

  void moveTo(const CardPhase next, const uint32_t now) { phase = next; changedAt = now; }

  void serveOpening(const uint32_t now) {
    if (stopWanted) { finish("stopped before it began", now); return; }
    if (opened) { moveTo(CardStreaming, now); return; }
    if (now - changedAt > CARD_OPEN_WAIT) {
      trouble = "the printer never said it opened the file";
      finish(trouble, now);
    }
  }

  void serveStreaming(const uint32_t now) {
    if (stopWanted) { closeFile("stopped", now); return; }

    for (int guard = 0; guard < 64; guard++) {
      if (link->waiting() + 4 >= link->slotCeiling
          || !link->canAccept(LINK_LINE_BYTES - LINK_FRAME_ROOM - 1))
        return;                       // No room for a longest line, so wait

      const int got = source->next(work, sizeof work);
      if (got < 0) { closeFile("", now); return; }

      const size_t kept = strip(work);
      if (!kept) { skipped++; continue; }
      if (!link->send(work)) { skipped--; return; }   // Put it back for the next pass
      written += kept + 2;            // Marlin ends every stored line with CR and LF
      lines++;
      changedAt = now;
    }
  }

  void closeFile(const char *why, const uint32_t now) {
    if (why && *why) trouble = why;
    pushControl("M29");
    moveTo(CardClosing, now);
  }

  void serveClosing(const uint32_t now) {
    if (!link->idle()) {
      if (now - changedAt > CARD_CLOSE_WAIT) {
        trouble = "the printer never finished taking the file";
        finish(trouble, now);
      }
      return;
    }
    if (!index) { finish(trouble, now); return; }
    index->stale();
    index->expect();
    pushControl("M20 L");   // Without L the long name never comes back
    moveTo(CardChecking, now);
  }

  void serveChecking(const uint32_t now) {
    if (!index->fresh()) {
      if (now - changedAt > CARD_LIST_WAIT) {
        trouble = "the printer never listed its card";
        finish(trouble, now);
      }
      return;
    }
    const int32_t onCard = index->sizeOfEither(target);
    if (onCard < 0)
      trouble = "the file is not on the printer's card";
    else if ((uint32_t)onCard != written)
      trouble = "the file on the card is the wrong size";
    finish(trouble, now);
  }

  void finish(const char *why, const uint32_t now) {
    const bool good = !why || !*why;
    if (deadManSeconds) {
      char back[16];
      snprintf(back, sizeof back, "M85 S%d", deadManSeconds);
      pushControl(back);
    }
    if (good && printAfter) {
      char pick[CARD_TARGET_BYTES + 8];
      snprintf(pick, sizeof pick, "M23 %s", target);
      pushControl(pick);
      pushControl("M24");
    }
    finishedAt = now;
    source = nullptr;
    phase = good ? CardDone : CardFailed;
  }
};
