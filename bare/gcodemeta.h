#pragma once

// What a sliced file says about itself, read once while it is being written to the
// module's card and never read again.
//
// The module used to count layers as it fed the printer line by line. It does not feed
// the printer any more: the printer reads its own card, and the only thing that comes
// back over the wire is "SD printing byte N/M". So the answer has to be prepared in
// advance. Every layer change is written down against the byte it starts at, and the
// layer, the height and the time left are then looked up by that number.
//
// Nothing is held in memory while scanning: entries go straight out to the sink as they
// are found, and the picture is not decoded at all, only located, so that it can be
// decoded later from the file itself.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#define GCODEMETA_MAGIC    0x4D33504Bu   // "KP3M"
#define GCODEMETA_LINE     256
#define GCODEMETA_ENTRY    8

struct GcodeMetaHead {
  uint32_t magic;
  uint32_t size;        // of the gcode, so a stale index can be told from a fresh one
  uint32_t seconds;     // what the slicer estimated for the whole print
  uint32_t thumbAt;     // first byte of the base64, not of the comment that opens it
  uint32_t thumbLen;    // bytes of base64 including the line breaks and the semicolons
  uint32_t layers;      // what the header declared, which is not always what was found
  uint32_t found;       // entries written to the sink
  uint16_t thumbW, thumbH;
  float grams;
};

struct GcodeMetaLayer {
  uint32_t at;          // byte the layer starts at
  uint16_t z100;        // height in hundredths of a millimetre
  uint16_t minutes;     // what the last M73 said was left
};

struct GcodeMetaSink {
  virtual ~GcodeMetaSink() {}
  virtual void layer(const GcodeMetaLayer &one) = 0;

  // A picture, already decoded, a byte at a time. Slicers write several sizes one
  // after another, so a block that turns out to be smaller than one already taken is
  // closed with keep false and should be thrown away rather than kept.
  virtual void thumbOpen(uint16_t w, uint16_t h) { (void)w; (void)h; }
  virtual void thumbByte(uint8_t value) { (void)value; }
  virtual void thumbClose(bool keep) { (void)keep; }
};

class GcodeMetaScan {
public:
  GcodeMetaHead head;

  void begin(GcodeMetaSink *into) {
    memset(&head, 0, sizeof head);
    head.magic = GCODEMETA_MAGIC;
    sink = into;
    at = held = 0;
    minutes = 0;
    waiting = false;
    pendingAt = 0;
    inThumb = false;
    bestPixels = 0;
    bits = 0;
    nbits = 0;
  }

  void feed(const uint8_t *data, const size_t len) {
    for (size_t i = 0; i < len; i++) {
      const char ch = (char)data[i];
      if (ch == '\n') {
        line[held] = 0;
        sawLine();
        at += held + 1;            // the newline belongs to the line that ended
        held = 0;
        cut = 0;
        continue;
      }
      if (ch == '\r') {
        cut++;                     // counted, so the offsets stay true to the file
        continue;
      }
      if (held + 1 < GCODEMETA_LINE)
        line[held++] = ch;
      else
        cut++;
    }
  }

  // The file ended. A last line without a newline still counts, and a layer that never
  // got its height is written down with none rather than dropped.
  void finish(const uint32_t size) {
    if (held) {
      line[held] = 0;
      sawLine();
      at += held;
      held = 0;
    }
    if (waiting)
      emit(pendingAt, 0);
    head.size = size;
  }

private:
  GcodeMetaSink *sink = nullptr;
  char line[GCODEMETA_LINE];
  size_t held = 0, cut = 0;
  uint32_t at = 0, pendingAt = 0;
  uint16_t minutes = 0;
  bool waiting = false, inThumb = false;
  uint32_t bestPixels = 0, thumbStart = 0;
  uint16_t thumbW = 0, thumbH = 0;
  uint32_t bits = 0;
  uint8_t nbits = 0;

  // Base64, decoded as it arrives, so nothing has to be held while a picture goes past.
  static int sextet(const char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;                       // padding and anything else ends the run
  }

  void sixBits(const char ch) {
    const int value = sextet(ch);
    if (value < 0)
      return;
    bits = (bits << 6) | (uint32_t)value;
    nbits += 6;
    if (nbits >= 8) {
      nbits -= 8;
      if (sink)
        sink->thumbByte((uint8_t)((bits >> nbits) & 0xFF));
    }
  }

  // The line as read, with any carriage returns already dropped. Its first byte sits at
  // `at`, and `cut` is what was thrown away, which the next offset has to account for.
  size_t lineBytes() const { return held + cut; }

  void emit(const uint32_t where, const uint16_t z100) {
    GcodeMetaLayer one;
    one.at = where;
    one.z100 = z100;
    one.minutes = minutes;
    if (sink)
      sink->layer(one);
    head.found++;
    waiting = false;
  }

  static const char *after(const char *text, const char *what) {
    const size_t n = strlen(what);
    return strncmp(text, what, n) ? nullptr : text + n;
  }

  // "12m 28s", "1h 2m 3s", "2d 1h" as the slicers write them
  static uint32_t spanSeconds(const char *text) {
    uint32_t total = 0;
    while (*text) {
      while (*text == ' ' || *text == '=')
        text++;
      if (*text < '0' || *text > '9')
        break;
      char *end = nullptr;
      const unsigned long value = strtoul(text, &end, 10);
      if (!end || end == text)
        break;
      switch (*end) {
        case 'd': total += value * 86400; break;
        case 'h': total += value * 3600;  break;
        case 'm': total += value * 60;    break;
        case 's': total += value;         break;
        default:  return total;
      }
      text = end + 1;
    }
    return total;
  }

  void sawLine() {
    if (inThumb) {
      if (strstr(line, "thumbnail end")) {
        inThumb = false;
        const uint32_t pixels = (uint32_t)thumbW * thumbH;
        const bool keep = pixels >= bestPixels;   // the later of two equals, which slicers write bigger
        if (keep) {
          bestPixels = pixels;
          head.thumbAt = thumbStart;
          head.thumbLen = at - thumbStart;
          head.thumbW = thumbW;
          head.thumbH = thumbH;
        }
        if (sink)
          sink->thumbClose(keep);
        return;
      }
      // Everything between the two comments is base64 behind a semicolon
      const char *p = line;
      while (*p == ';' || *p == ' ')
        p++;
      for (; *p; p++)
        sixBits(*p);
      return;
    }

    if (const char *rest = after(line, "; thumbnail begin ")) {
      unsigned w = 0, h = 0;
      if (sscanf(rest, "%ux%u", &w, &h) == 2) {
        thumbW = (uint16_t)w;
        thumbH = (uint16_t)h;
        thumbStart = at + (uint32_t)lineBytes() + 1;   // the base64 starts on the next line
        inThumb = true;
        bits = 0;
        nbits = 0;
        if (sink)
          sink->thumbOpen(thumbW, thumbH);
      }
      return;
    }

    if (const char *rest = after(line, "; total layer number:")) {
      head.layers = (uint32_t)strtoul(rest, nullptr, 10);
      return;
    }
    if (const char *rest = strstr(line, "estimated printing time")) {
      const char *equals = strchr(rest, '=');
      if (equals)
        head.seconds = spanSeconds(equals + 1);
      return;
    }
    if (const char *rest = strstr(line, "filament used [g]")) {
      const char *equals = strchr(rest, '=');
      if (equals)
        head.grams = (float)atof(equals + 1);
      return;
    }

    // M73 carries the time the slicer thinks is left, which beats anything worked out
    // from bytes: the last layers of a print are not the same length as the first.
    if (!strncmp(line, "M73 ", 4)) {
      const char *r = strstr(line, "R");
      if (r && r[1] >= '0' && r[1] <= '9')
        minutes = (uint16_t)strtoul(r + 1, nullptr, 10);
      return;
    }

    if (!strncmp(line, ";LAYER_CHANGE", 13)) {
      if (waiting)
        emit(pendingAt, 0);        // the one before never said how high it was
      pendingAt = at;
      waiting = true;
      return;
    }
    if (!strncmp(line, ";Z:", 3) && waiting) {
      const double z = atof(line + 3);
      emit(pendingAt, (uint16_t)(z * 100 + 0.5));
    }
  }
};

// Which layer a byte belongs to, by walking a table that is already sorted. The table
// is handed over whole because it is read from a file into a small window at a time.
inline uint32_t gcodeMetaLayerAt(const GcodeMetaLayer *table, const uint32_t count,
                                 const uint32_t byte, uint16_t *z100, uint16_t *minutes) {
  uint32_t low = 0, high = count;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    if (table[mid].at <= byte)
      low = mid + 1;
    else
      high = mid;
  }
  if (!low) {
    if (z100) *z100 = 0;
    if (minutes) *minutes = count ? table[0].minutes : 0;
    return 0;
  }
  if (z100) *z100 = table[low - 1].z100;
  if (minutes) *minutes = table[low - 1].minutes;
  return low;
}
