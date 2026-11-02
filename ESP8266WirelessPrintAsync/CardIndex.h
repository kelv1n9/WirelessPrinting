#pragma once

// What is on the printer's own card, as M20 reports it. Fixed storage, nothing on the
// heap, because this is filled from the serial reader while the web may be reading it.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#define CARD_INDEX_SLOTS  24
#define CARD_DOS_BYTES    14      // 8.3 plus a slash and the end marker
#define CARD_NAME_BYTES   48

class CardIndex {
public:
  struct Entry {
    char dos[CARD_DOS_BYTES];
    char name[CARD_NAME_BYTES];   // The long one when the printer offers it, else the same
    uint32_t size;
  };

  void expect() {                 // Called the moment M20 goes out
    collecting = false;
    asked = true;
    filling = 0;
    missed = 0;
  }

  bool observe(const char *line) {
    if (!strncmp(line, "Begin file list", 15)) {
      collecting = true;
      filling = 0;
      missed = 0;
      return true;
    }
    if (!collecting)
      return false;
    if (!strncmp(line, "End file list", 13)) {
      collecting = false;
      asked = false;
      held = filling;
      lost = missed;
      ready = true;
      return true;
    }
    take(line);
    return true;
  }

  bool busy() const  { return collecting || asked; }
  bool fresh() const { return ready; }
  void stale()       { ready = false; }
  uint8_t count() const { return held; }
  uint8_t overflowed() const { return lost; }
  const Entry &at(const uint8_t i) const { return slot[i]; }

  // Returns the size the printer reports, or -1 when it does not have the file
  int32_t sizeOf(const char *dos) const {
    for (uint8_t i = 0; i < held; i++)
      if (!strcasecmp(slot[i].dos, dos))
        return (int32_t)slot[i].size;
    return -1;
  }

  // A file written under a long name is listed under its 8.3 alias as well, so a
  // lookup has to be willing to match either of the two.
  int32_t sizeOfEither(const char *name) const {
    for (uint8_t i = 0; i < held; i++)
      if (!strcasecmp(slot[i].dos, name) || !strcasecmp(slot[i].name, name))
        return (int32_t)slot[i].size;
    return -1;
  }

  bool holds(const char *name) const { return sizeOfEither(name) >= 0; }

private:
  Entry slot[CARD_INDEX_SLOTS];
  uint8_t held = 0, filling = 0, lost = 0, missed = 0;
  bool collecting = false, asked = false, ready = false;

  // "NAME.GCO 12345" and, when the printer was asked with L, " Long Name.gcode" after it
  void take(const char *line) {
    while (*line == ' ') line++;
    if (!*line || *line == '/')            // A folder line, which we do not descend into
      return;
    const char *space = strchr(line, ' ');
    if (!space)
      return;
    if (filling >= CARD_INDEX_SLOTS) {
      if (missed < 255) missed++;
      return;
    }

    Entry &e = slot[filling];
    size_t dosLen = (size_t)(space - line);
    if (dosLen >= CARD_DOS_BYTES) dosLen = CARD_DOS_BYTES - 1;
    memcpy(e.dos, line, dosLen);
    e.dos[dosLen] = 0;

    e.size = (uint32_t)strtoul(space + 1, nullptr, 10);

    const char *rest = strchr(space + 1, ' ');
    const char *pretty = rest ? rest + 1 : e.dos;
    while (*pretty == ' ') pretty++;
    size_t nameLen = strlen(pretty);
    if (nameLen >= CARD_NAME_BYTES) nameLen = CARD_NAME_BYTES - 1;
    memcpy(e.name, pretty, nameLen);
    e.name[nameLen] = 0;

    filling++;
  }
};

// Marlin builds its own 8.3 name unless it was compiled with long name writing, and its
// converter is not reachable from here, so the short name is picked on this side instead.
inline void cardShortName(const char *longName, char *out, size_t max) {
  const char *slash = strrchr(longName, '/');
  const char *stem = slash ? slash + 1 : longName;
  const char *dot = strrchr(stem, '.');
  size_t stemLen = dot && dot > stem ? (size_t)(dot - stem) : strlen(stem);

  size_t at = 0;
  for (size_t i = 0; i < stemLen && at < 8; ++i) {
    const unsigned char ch = (unsigned char)stem[i];
    if (isalnum(ch))
      out[at++] = (char)toupper(ch);
    else if (at && out[at - 1] != '_')
      out[at++] = '_';
  }
  while (at && out[at - 1] == '_')
    at--;
  out[at] = 0;
  if (!at)
    strcpy(out, "PRINT");
  if (strlen(out) + 5 >= max)
    out[max - 5] = 0;
  strcat(out, ".GCO");
}
