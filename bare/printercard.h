#pragma once

// What is on the printer's own card, and what of ours is not there yet.
//
// Two halves, both without Arduino types so the whole thing runs on a computer:
//   PrinterCard        the index, filled line by line from the reply to "M20 L"
//   printerCardTarget  the module's filename turned into the name it gets on the printer
//   PrinterCardPick    the send queue, worked out from those two and nothing else
//
// There is no stored queue and no stored "already sent" mark. Everything is derived from
// the listing the printer just gave us, so a reboot loses nothing and nothing can drift
// out of step with the card.
//
// Marlin on this board never lists .bin files, whatever flags M20 is given. The index is
// therefore gcode only, and a missing .bin here means nothing at all.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define PRINTER_CARD_SLOTS      32
#define PRINTER_CARD_NAME_BYTES 64   // BinaryTransfer's own buffer, the tightest of the limits
#define PRINTER_CARD_DOS_BYTES  14   // 8.3 and the end marker
#define PRINTER_CARD_LINE_BYTES 320  // PrinterLink hands over lines no longer than this
#define PRINTER_CARD_NAME_MAX   (PRINTER_CARD_NAME_BYTES - 1)

// Marlin's parsePath() refuses a long name holding any of these, so we never offer one.
#define PRINTER_CARD_FORBIDDEN "|<>^+=?/[];:,*\"\\"

struct PrinterCardEntry {
  char dos[PRINTER_CARD_DOS_BYTES];     // the 8.3 name, always present
  char name[PRINTER_CARD_NAME_BYTES];   // the long one, empty when it is too long to be ours
  uint32_t size;
};

inline bool printerCardSame(const char *a, const char *b) {
  if (!*a || !*b)
    return false;
  for (; *a && *b; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
    if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
    if (x != y)
      return false;
  }
  return *a == *b;
}

inline bool printerCardIsGcode(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot || dot == name)
    return false;
  return printerCardSame(dot, ".gco") || printerCardSame(dot, ".gcode") || printerCardSame(dot, ".g");
}

inline bool printerCardTarget(const char *moduleName, char *out, const size_t max);

class PrinterCard {
public:
  // Called the moment "M20 L" goes out, and again by "Begin file list" itself, so a
  // second listing cannot be mistaken for more of the first one.
  void begin() {
    held = filling = 0;
    rejected_ = skipped_ = oversize_ = seen_ = 0;
    listing_ = complete_ = spared_ = clash_ = false;
  }

  void ask(const char *wanted, const uint16_t from) {
    from_ = from;
    wanted_[0] = wantedTarget_[0] = 0;
    if (wanted && *wanted) {
      snprintf(wanted_, sizeof wanted_, "%s", wanted);
      printerCardTarget(wanted, wantedTarget_, sizeof wantedTarget_);
    }
    begin();
  }

  // Every line the printer sends while a listing is expected. Returns true when the line
  // belonged to the listing, so the caller can leave the rest of its handling alone.
  bool observe(const char *raw) {
    char line[PRINTER_CARD_LINE_BYTES];
    while (*raw == ' ' || *raw == '\t')
      raw++;
    size_t n = 0;
    while (raw[n] && n + 1 < sizeof line) {
      line[n] = raw[n];
      n++;
    }
    const bool cut = raw[n] != 0;             // a line this long is not one of ours
    line[n] = 0;
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' '))
      line[--n] = 0;

    if (!strcmp(line, "Begin file list")) {
      begin();
      listing_ = true;
      return true;
    }
    if (!listing_)
      return false;
    if (!strcmp(line, "End file list")) {
      listing_ = false;
      complete_ = true;
      held = filling;
      if (spared_)
        slot[held++] = slot[PRINTER_CARD_SLOTS];
      return true;
    }
    if (cut)
      rejected_++;
    else
      take(line);
    return true;
  }

  bool listing() const  { return listing_; }
  bool complete() const { return complete_; }   // false until "End file list" arrives

  uint8_t count() const { return held; }
  const PrinterCardEntry &at(const uint8_t i) const { return slot[i]; }

  const PrinterCardEntry *find(const char *name, bool *ambiguous = nullptr) const {
    if (ambiguous) *ambiguous = false;
    if (!name || !complete_)
      return nullptr;
    const PrinterCardEntry *found = nullptr;
    for (uint8_t i = 0; i < held; i++)
      if (printerCardSame(name, slot[i].dos) || printerCardSame(name, slot[i].name)) {
        if (found) {
          if (ambiguous) *ambiguous = true;
          return nullptr;
        }
        found = &slot[i];
      }
    if (found && spared_ && clash_ && found == &slot[held - 1]) {
      if (ambiguous) *ambiguous = true;
      return nullptr;
    }
    return found;
  }

  uint16_t rejected() const { return rejected_; }   // lines that did not parse
  uint16_t skipped() const  { return skipped_; }    // parsed, but not a gcode file in the root
  uint16_t total() const    { return seen_; }
  uint16_t from() const     { return from_; }
  uint16_t more() const {
    return seen_ > (uint32_t)from_ + PRINTER_CARD_SLOTS ? (uint16_t)(from_ + PRINTER_CARD_SLOTS) : 0;
  }
  uint16_t oversize() const { return oversize_; }   // long names too long to be anything of ours

  // The only question worth asking: is this exact file already over there? Same name and
  // same size, because a half sent file has the right name and the wrong length.
  bool holds(const char *target, const uint32_t size) const {
    for (uint8_t i = 0; i < held; i++)
      if (slot[i].size == size &&
          (printerCardSame(target, slot[i].name) || printerCardSame(target, slot[i].dos)))
        return true;
    return false;
  }

private:
  PrinterCardEntry slot[PRINTER_CARD_SLOTS + 1];
  uint8_t held = 0, filling = 0;
  uint16_t rejected_ = 0, skipped_ = 0, oversize_ = 0, seen_ = 0, from_ = 0;
  bool listing_ = false, complete_ = false, spared_ = false, clash_ = false;
  char wanted_[PRINTER_CARD_NAME_BYTES] = "", wantedTarget_[PRINTER_CARD_NAME_BYTES] = "";

  bool isWanted(const char *name) const {
    return name && (printerCardSame(name, wanted_) || printerCardSame(name, wantedTarget_));
  }

  // "<8.3 name> <size>" and, because we always ask with L, " <long name>" after it.
  // Anything that is not exactly that shape is thrown away whole. A listing full of
  // rubbish from some other computer leaves an empty index, not a corrupt one.
  void take(const char *line) {
    const char *gap = strchr(line, ' ');
    if (!gap || gap == line) {
      rejected_++;
      return;
    }
    const size_t dosLen = (size_t)(gap - line);

    const char *p = gap + 1;
    if (*p < '0' || *p > '9') {
      rejected_++;
      return;
    }
    uint32_t size = 0;
    while (*p >= '0' && *p <= '9') {
      const uint32_t digit = (uint32_t)(*p - '0');
      if (size > (0xFFFFFFFFu - digit) / 10) {
        rejected_++;
        return;
      }
      size = size * 10 + digit;
      p++;
    }
    if (*p && *p != ' ') {                     // something glued to the size
      rejected_++;
      return;
    }

    const char *longName = nullptr;
    if (*p == ' ') {
      while (*p == ' ')
        p++;
      if (!*p) {
        rejected_++;
        return;
      }
      longName = p;
    }
    for (const char *c = line; *c; c++)
      if ((unsigned char)*c < 0x20) {
        rejected_++;
        return;
      }

    // Anything below the root can never be a file of ours, and neither can anything that
    // is not gcode. The rubbish other systems leave behind is both.
    if (memchr(line, '/', dosLen) || (longName && strchr(longName, '/'))) {
      skipped_++;
      return;
    }
    if (dosLen >= PRINTER_CARD_DOS_BYTES) {
      rejected_++;
      return;
    }

    char dos[PRINTER_CARD_DOS_BYTES];
    memcpy(dos, line, dosLen);
    dos[dosLen] = 0;

    // Without the L the printer sends no long name, so the short one has to answer for it
    if (!printerCardIsGcode(longName ? longName : dos)) {
      skipped_++;
      return;
    }

    const uint16_t index = seen_++;
    const bool seated = index >= from_ && filling < PRINTER_CARD_SLOTS;
    if (!seated) {
      if (!isWanted(dos) && !isWanted(longName))
        return;
      if (spared_) {
        clash_ = true;
        return;
      }
      spared_ = true;
    }

    PrinterCardEntry &e = slot[seated ? filling : PRINTER_CARD_SLOTS];
    memcpy(e.dos, dos, dosLen + 1);
    e.size = size;
    e.name[0] = 0;
    if (longName) {
      // A name we could not have written is left out rather than cut short. A cut name
      // would match a file of ours that is not actually there.
      const size_t len = strlen(longName);
      if (len < PRINTER_CARD_NAME_BYTES)
        memcpy(e.name, longName, len + 1);
      else
        oversize_++;
    }
    if (seated)
      filling++;
  }
};

inline bool printerCardOpened(const char *reply, const char *name, const uint32_t size) {
  if (!reply || !name || !*name || strlen(name) >= PRINTER_CARD_NAME_BYTES)
    return false;
  char expected[PRINTER_CARD_NAME_BYTES + 40];
  snprintf(expected, sizeof expected, "File opened: %s Size: %lu", name, (unsigned long)size);
  const size_t length = strlen(expected);
  const char *line = reply;
  while (*line) {
    if (!strncmp(line, expected, length) &&
        (line[length] == '\r' || line[length] == '\n' || line[length] == 0))
      return true;
    const char *end = strchr(line, '\n');
    if (!end)
      break;
    line = end + 1;
  }
  return false;
}

inline uint16_t printerCardTag(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; ++s) {
    h ^= (unsigned char)*s;
    h *= 16777619u;
  }
  return (uint16_t)(h ^ (h >> 16));
}

// The name a file on the module's card should get on the printer. Long names work here,
// the printer answers Cap:LFN_WRITE:1, so the name is kept as it is whenever the printer
// can take it as it is. When anything had to be done to it, the fingerprint of the whole
// original name is written into it, so that two files which would otherwise end up with
// one name keep two. Nothing is remembered between calls: the same input always gives the
// same output, which is what lets the queue be worked out afresh after every reboot.
inline bool printerCardTarget(const char *moduleName, char *out, const size_t max) {
  if (!moduleName || !out || max < 16)
    return false;

  const uint16_t tag = printerCardTag(moduleName);
  bool changed = false;

  if (*moduleName == '/')               // the module's own card root, not a rename
    moduleName++;
  const char *slash = strrchr(moduleName, '/');
  const char *base = moduleName;
  if (slash) {
    base = slash + 1;
    changed = true;
  }

  char clean[PRINTER_CARD_NAME_BYTES * 4];
  size_t n = 0;
  for (const char *p = base; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    char ch;
    if (c < 0x20 || c > 0x7E || strchr(PRINTER_CARD_FORBIDDEN, (char)c)) {
      ch = '_';                          // includes anything not plain ascii
      changed = true;
    }
    else
      ch = (char)c;
    if (n + 1 < sizeof clean)
      clean[n++] = ch;
    else
      changed = true;
  }
  clean[n] = 0;

  size_t start = 0;
  while (clean[start] == ' ') {          // fat dislikes both of these at the ends
    start++;
    changed = true;
  }
  while (n > start && (clean[n - 1] == ' ' || clean[n - 1] == '.')) {
    clean[--n] = 0;
    changed = true;
  }

  char *name = clean + start;
  size_t len = n - start;
  if (!len) {
    strcpy(clean, "PRINT.GCO");
    name = clean;
    len = 9;
    changed = true;
  }

  const char *dot = strrchr(name, '.');
  size_t extLen = (dot && dot != name) ? len - (size_t)(dot - name) : 0;
  if (extLen > 8)
    extLen = 0;                          // a dot that late is part of the name, not a suffix
  size_t stemLen = len - extLen;

  size_t limit = max - 1;
  if (limit > PRINTER_CARD_NAME_MAX)
    limit = PRINTER_CARD_NAME_MAX;
  if (len > limit)
    changed = true;

  const size_t room = limit - (changed ? 5 : 0);     // "~XXXX" when there is a fingerprint
  if (extLen > room) {
    extLen = 0;
    stemLen = len;
  }
  if (stemLen > room - extLen)
    stemLen = room - extLen;

  size_t at = 0;
  memcpy(out, name, stemLen);
  at += stemLen;
  if (changed) {
    snprintf(out + at, 6, "~%04X", tag);
    at += 5;
  }
  memcpy(out + at, name + len - extLen, extLen);
  at += extLen;
  out[at] = 0;
  return true;
}

inline bool printerCardResolve(const PrinterCard &card, const char *name,
                               char *out, const size_t max) {
  if (!name || !*name || !out || !max)
    return false;
  bool ambiguous = false;
  const PrinterCardEntry *entry = card.find(name, &ambiguous);
  if (ambiguous)
    return false;
  char target[PRINTER_CARD_NAME_BYTES];
  if (!entry) {
    if (!printerCardTarget(name, target, sizeof target))
      return false;
    entry = card.find(target, &ambiguous);
    if (ambiguous)
      return false;
    name = target;
  }
  if (entry)
    name = entry->name[0] ? entry->name : entry->dos;
  if (strlen(name) >= max)
    return false;
  memmove(out, name, strlen(name) + 1);
  return true;
}

// The send queue. Not a queue at all: every file on the module's card is offered to it,
// and it keeps the one that should go next. Offer them again after the next listing and
// the answer is as good as it was, with nothing held on to in between.
struct PrinterCardPick {
  char source[PRINTER_CARD_NAME_BYTES];   // what it is called on the module's card
  char target[PRINTER_CARD_NAME_BYTES];   // what it should be called on the printer's
  uint32_t size = 0;
  bool found = false;
  uint16_t waiting = 0;                   // how many are not over there yet, this one included

  void begin() {
    found = false;
    waiting = 0;
    size = 0;
    source[0] = target[0] = 0;
  }

  void consider(const PrinterCard &card, const char *moduleName, const uint32_t moduleSize) {
    if (!card.complete() || !moduleName || !*moduleName)
      return;                              // not knowing what is there is not the same as it being empty

    char want[PRINTER_CARD_NAME_BYTES];
    if (!printerCardTarget(moduleName, want, sizeof want))
      return;
    if (!printerCardIsGcode(want))         // .bin is never listed, so it is never judged here
      return;
    if (card.holds(want, moduleSize))
      return;

    waiting++;
    // First by name, so that which file goes next does not depend on the order the
    // module's card happens to hand its entries over.
    if (found && !lessi(moduleName, source))
      return;
    snprintf(source, sizeof source, "%s", moduleName);
    snprintf(target, sizeof target, "%s", want);
    size = moduleSize;
    found = true;
  }

private:
  static bool lessi(const char *a, const char *b) {
    for (;; a++, b++) {
      char x = *a, y = *b;
      if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
      if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
      if (x != y) return (unsigned char)x < (unsigned char)y;
      if (!x) return false;
    }
  }
};
