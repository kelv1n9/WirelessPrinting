#pragma once

// The only thing in the firmware that is allowed to touch the wire to the printer.
// Written without Arduino types so the very same code runs against the simulated
// printer on a computer.
//
// The window holds raw lines, not numbered ones. Numbers are put on at the moment a line
// goes out, which is what makes it possible to renumber the whole window after a reset
// without losing a single line.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define LINK_RING_BYTES     1280    // Well over any in flight budget, and static memory is tight
#define LINK_WINDOW_SLOTS   48
#define LINK_LINE_BYTES     320
#define LINK_REPLY_BYTES    300
#define LINK_FRAME_ROOM     16      // "N<n> " in front and "*<sum>" behind
#define LINK_RESEND_GRACE   300     // Repeated asks inside this are the echo of one flush

#define XON_BYTE  0x11
#define XOFF_BYTE 0x13

struct LinkIo {
  virtual ~LinkIo() {}
  virtual int read() = 0;                                   // -1 when nothing has arrived
  virtual size_t write(const uint8_t *data, size_t len) = 0;
  virtual size_t writeAscii(const uint8_t *data, size_t len) { return write(data, len); }
  virtual size_t writable() = 0;
  virtual uint32_t now() = 0;
};

struct LinkCaps {
  bool seen = false;
  bool advancedOk = false;      // Deduced from the shape of the answers, not from M115
  bool lfnWrite = false;
  bool xonXoff = false;
  bool binary = false;
  bool sdWrite = false;
  bool autoreportTemp = false;
  bool autoreportSd = false;
  bool progress = false;
  bool buildPercent = false;
  bool card = false;
  int slots = 0;                // Highest B ever reported, which is the printer's BUFSIZE
};

class PrinterLink {
public:
  // How much may be in flight. Both are deliberately settable at run time, because the
  // safe value depends on the printer's serial buffer and that is measured, not assumed.
  uint16_t budgetBytes = 56;
  uint8_t  budgetLines = 1;     // Raised to what B reports as soon as a B is seen
  uint8_t  slotCeiling = LINK_WINDOW_SLOTS - 1;

  LinkCaps caps;
  uint32_t resends = 0, errors = 0, acked = 0, queued = 0, resyncs = 0, refused = 0, stale = 0;
  uint32_t timeouts = 0;
  uint32_t lastAckAt = 0;
  uint32_t ackTimeout = 2000;   // Silence this long means something was lost on the way
  uint16_t settleMs = 30;       // After a rewind, the time the wire needs to empty itself

  void (*onOther)(const char *line, void *user) = nullptr;   // Everything that is not protocol
  void (*onAck)(const char *line, void *user) = nullptr;     // The answers themselves, for the log
  uint16_t linesPerService = 40;   // A printer in trouble shouts, and the watchdog is watching
  void *user = nullptr;

  void begin(LinkIo *link) { io = link; reset(); lastAckAt = io ? io->now() : 0; }

  void reset() {
    head = tail = cursor = 0;
    ringHead = ringTail = 0;
    ringUsed = 0;
    replyLen = 0;
    paused = false;
    number = 0;
    resendAt = resendFor = 0;
    pending = 0;
    inflight = 0;
    ghostOks = 0;
  }

  bool idle() const { return head == tail; }
  uint8_t waiting() const { return (uint8_t)((head - tail + LINK_WINDOW_SLOTS) % LINK_WINDOW_SLOTS); }
  uint16_t inflightBytes() const { return inflight; }

  bool canAccept(const size_t rawLen) const {
    if (rawLen + LINK_FRAME_ROOM > LINK_LINE_BYTES) return false;
    if (waiting() + 1 >= LINK_WINDOW_SLOTS) return false;
    if (waiting() >= slotCeiling) return false;
    return ringUsed + rawLen <= LINK_RING_BYTES;
  }

  // Puts the line in the window as it stands. Numbering happens when it goes out.
  bool send(const char *line) {
    while (*line == ' ') line++;
    size_t raw = strlen(line);
    while (raw && (line[raw - 1] == '\r' || line[raw - 1] == '\n' || line[raw - 1] == ' '))
      raw--;
    if (!raw || !canAccept(raw)) {
      if (raw) refused++;
      return false;
    }

    Slot &slot = slots[head];
    slot.at = ringHead;
    slot.len = (uint16_t)raw;
    slot.number = 0;                      // Not numbered until it is actually sent
    for (size_t i = 0; i < raw; i++) {
      ring[ringHead] = line[i];
      ringHead = (uint16_t)((ringHead + 1) % LINK_RING_BYTES);
    }
    ringUsed += raw;
    head = (uint8_t)((head + 1) % LINK_WINDOW_SLOTS);
    queued++;
    return true;
  }

  void service() {
    receive();
    // A damaged newline leaves the printer waiting for the end of a line while we wait
    // for its answer. Nothing but a clock breaks that, so here is the clock.
    if (io && pending && (int32_t)(io->now() - settleUntil) >= 0
        && io->now() - lastAckAt > ackTimeout) {
      timeouts++;
      lastAckAt = io->now();
      rewindCursor(tail);
    }
    transmit();
  }

private:
  struct Slot { uint16_t at, len; uint32_t number; };

  LinkIo *io = nullptr;
  char ring[LINK_RING_BYTES];
  Slot slots[LINK_WINDOW_SLOTS];
  uint8_t head = 0, tail = 0, cursor = 0;
  uint16_t ringHead = 0, ringTail = 0;
  size_t ringUsed = 0;
  char reply[LINK_REPLY_BYTES];
  size_t replyLen = 0;
  bool paused = false;
  uint32_t number = 0;                 // Last number handed out
  uint32_t resendAt = 0, resendFor = 0;
  uint8_t pending = 0;                 // Slots written to the wire and not yet answered
  uint16_t inflight = 0;               // Their bytes, newline included
  uint8_t ghostOks = 0;                // Answers owed to lines sent outside the window
  uint32_t settleUntil = 0;

  static bool isM110(const char *text, const size_t len) {
    return len >= 4 && !memcmp(text, "M110", 4) && (len == 4 || text[4] == ' ');
  }

  size_t frame(const Slot &slot, const uint32_t n, uint8_t *out) const {
    char head8[16];
    const int lead = snprintf(head8, sizeof head8, "N%u ", (unsigned)n);
    size_t at = 0;
    uint8_t sum = 0;
    for (int i = 0; i < lead; i++) { out[at] = (uint8_t)head8[i]; sum ^= out[at++]; }
    for (uint16_t i = 0; i < slot.len; i++) {
      out[at] = (uint8_t)ring[(slot.at + i) % LINK_RING_BYTES];
      sum ^= out[at++];
    }
    at += snprintf((char *)out + at, 8, "*%u", (unsigned)sum);
    out[at++] = '\n';
    return at;
  }

  void transmit() {
    if (!io || paused)
      return;
    // Bytes already on the wire keep arriving after a rewind. Pouring a fresh window on
    // top of them overruns the printer's buffer, which causes the next error, and so on.
    if ((int32_t)(io->now() - settleUntil) < 0)
      return;
    while (cursor != head) {
      Slot &slot = slots[cursor];
      char first[8] = {0};
      for (uint16_t i = 0; i < slot.len && i < 5; i++)
        first[i] = ring[(slot.at + i) % LINK_RING_BYTES];
      const bool reset110 = isM110(first, slot.len < 5 ? slot.len : 5);

      // A line keeps the number it was first given, or gets the next one
      const uint32_t n = slot.number ? slot.number : (reset110 ? 0 : number + 1);

      uint8_t out[LINK_LINE_BYTES + LINK_FRAME_ROOM];
      const size_t len = frame(slot, n, out);

      if (pending + 1 > budgetLines) return;
      if (inflight + len > budgetBytes) return;
      if (io->writable() < len) return;
      if (io->write(out, len) != len) return;

      slot.number = n;
      if (reset110) number = 0; else number = n;
      if (!pending) lastAckAt = io->now();
      inflight = (uint16_t)(inflight + len);
      pending++;
      cursor = (uint8_t)((cursor + 1) % LINK_WINDOW_SLOTS);
    }
  }

  void receive() {
    if (!io)
      return;
    uint16_t done = 0;
    for (int c = io->read(); c >= 0; c = io->read()) {
      if (c == XOFF_BYTE) { paused = true;  continue; }
      if (c == XON_BYTE)  { paused = false; continue; }
      if (c == '\r') continue;
      if (c != '\n') {
        if (replyLen + 1 < LINK_REPLY_BYTES)
          reply[replyLen++] = (char)c;
        continue;
      }
      reply[replyLen] = 0;
      if (replyLen) digest(reply);
      replyLen = 0;
      if (++done >= linesPerService)
        return;                       // The rest waits in the buffer for the next pass
    }
  }

  void digest(const char *line) {
    if (line[0] == 'o' && line[1] == 'k') {
      takeOk(line + 2);
      if (onAck) onAck(line, user);
      return;
    }
    if (!strncmp(line, "Resend", 6) || !strncmp(line, "rs", 2)) { takeResend(line); return; }
    if (!strncmp(line, "Cap:", 4))                            { takeCap(line + 4); return; }
    if (!strncmp(line, "Error:", 6)) {
      errors++;
      if (onOther) onOther(line, user);
      return;
    }
    if (onOther) onOther(line, user);
  }

  void dropOldest() {
    const Slot &slot = slots[tail];
    uint8_t probe[LINK_LINE_BYTES + LINK_FRAME_ROOM];
    const size_t len = frame(slot, slot.number, probe);
    ringTail = (uint16_t)((ringTail + slot.len) % LINK_RING_BYTES);
    ringUsed -= slot.len;
    inflight = (uint16_t)(inflight >= len ? inflight - len : 0);
    if (pending) pending--;
    tail = (uint8_t)((tail + 1) % LINK_WINDOW_SLOTS);
    acked++;
    lastAckAt = io ? io->now() : 0;
  }

  void takeOk(const char *rest) {
    int reportedB = -1;
    long reportedN = -1;
    for (const char *p = rest; *p; p++) {
      if (*p != ' ') continue;
      const char key = p[1];
      if (key != 'B' && key != 'P' && key != 'N') continue;
      const char *d = p + 2;
      if (*d < '0' || *d > '9') continue;
      long value = 0;
      while (*d >= '0' && *d <= '9') value = value * 10 + (*d++ - '0');
      if (key == 'B') reportedB = (int)value;
      else if (key == 'N') reportedN = value;
    }

    if (reportedB >= 0) {
      caps.advancedOk = true;
      if (reportedB > caps.slots) caps.slots = reportedB;
      const int room = reportedB < 1 ? 1 : reportedB;
      budgetLines = (uint8_t)(room > slotCeiling ? slotCeiling : room);
    }

    if (ghostOks) { ghostOks--; return; }     // Belongs to a line sent outside the window
    if (tail == cursor) return;               // Nothing is waiting for an answer

    // With a numbered answer we know exactly which line was taken, which matters after a
    // rewind, when answers to lines sent before it are still arriving.
    if (reportedN >= 0) {
      const uint32_t said = (uint32_t)reportedN;
      if (said < slots[tail].number) { stale++; return; }
      while (tail != cursor && slots[tail].number <= said)
        dropOldest();
      return;
    }
    dropOldest();
  }

  void takeResend(const char *line) {
    uint32_t want = 0;
    bool got = false;
    for (const char *p = line; *p; p++) {
      if (*p >= '0' && *p <= '9') {
        while (*p >= '0' && *p <= '9') want = want * 10 + (*p++ - '0');
        got = true;
        break;
      }
    }
    if (!got) return;

    // Everything the printer says while the wire is draining is the echo of the flush it
    // already did. Acting on it would start the same rewind over and over.
    if (io && (int32_t)(io->now() - settleUntil) < 0) return;
    if (cursor == tail) return;              // Nothing of ours is out there to repeat

    // One bad line makes the printer throw away everything already on its way to it, so
    // the lines behind it come back as more asks for the same number. That is one event.
    const uint32_t at = io ? io->now() : 0;
    if (!(want == resendFor && at - resendAt < LINK_RESEND_GRACE))
      resends++;
    resendFor = want;
    resendAt = at;
    rewindTo(want);
  }

  void rewindTo(const uint32_t want) {
    for (uint8_t i = tail; i != cursor; i = (uint8_t)((i + 1) % LINK_WINDOW_SLOTS)) {
      if (slots[i].number != want) continue;
      rewindCursor(i);
      return;
    }
    // Asked for something we no longer hold. Start the numbering over without losing a
    // single line: everything still in the window simply gets new numbers.
    resync();
  }

  void rewindCursor(const uint8_t to) {
    uint16_t bytes = 0;
    uint8_t lines = 0;
    for (uint8_t j = to; j != cursor; j = (uint8_t)((j + 1) % LINK_WINDOW_SLOTS)) {
      uint8_t probe[LINK_LINE_BYTES + LINK_FRAME_ROOM];
      bytes = (uint16_t)(bytes + frame(slots[j], slots[j].number, probe));
      lines++;
    }
    inflight = (uint16_t)(inflight >= bytes ? inflight - bytes : 0);
    pending = (uint8_t)(pending >= lines ? pending - lines : 0);
    cursor = to;
    paused = false;
    if (io) settleUntil = io->now() + settleMs;
  }

  void resync() {
    resyncs++;
    static const char zero[] = "N0 M110 N0*125\n";
    if (io && io->writable() >= sizeof zero - 1) {
      io->write((const uint8_t *)zero, sizeof zero - 1);
      ghostOks++;
    }
    number = 0;
    for (uint8_t i = tail; i != head; i = (uint8_t)((i + 1) % LINK_WINDOW_SLOTS))
      slots[i].number = 0;              // They will be numbered again on the way out
    cursor = tail;
    pending = 0;
    inflight = 0;
    paused = false;
    resendFor = 0;
    if (io) settleUntil = io->now() + settleMs;
  }

  void takeCap(const char *rest) {
    caps.seen = true;
    const bool on = strstr(rest, ":1") != nullptr;
    if (!strncmp(rest, "LFN_WRITE", 9))                  caps.lfnWrite = on;
    else if (!strncmp(rest, "SERIAL_XON_XOFF", 15))      caps.xonXoff = on;
    else if (!strncmp(rest, "BINARY_FILE_TRANSFER", 20)) caps.binary = on;
    else if (!strncmp(rest, "SD_WRITE", 8))              caps.sdWrite = on;
    else if (!strncmp(rest, "SDCARD", 6))                caps.card = on;
    else if (!strncmp(rest, "AUTOREPORT_TEMP", 15))      caps.autoreportTemp = on;
    else if (!strncmp(rest, "AUTOREPORT_SD_STATUS", 20)) caps.autoreportSd = on;
    else if (!strncmp(rest, "BUILD_PERCENT", 13))        caps.buildPercent = on;
    else if (!strncmp(rest, "PROGRESS", 8))              caps.progress = on;
  }
};
