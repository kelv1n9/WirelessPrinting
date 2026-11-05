#pragma once

#include <Arduino.h>
#include <SD_MMC.h>
#include <time.h>
#include <WebServer.h>

// What happened and when, on the module's card, so a failure at night leaves something
// to read in the morning. Lines gather in memory and only the loop writes them out:
// several tasks say things, and none of them should wait on the card to do it.

#define JOURNAL_PATH  "/log/events.log"
#define JOURNAL_OLD   "/log/events.1.log"
#define JOURNAL_MAX   (256 * 1024)     // rolled over at this size, so two files are the whole cost
#define JOURNAL_HELD  1024
#define JOURNAL_LINE  240
#define JOURNAL_TAIL  4096             // how much of it a read hands back
#define JOURNAL_EVERY 5000

static char journalHeld[JOURNAL_HELD];
static size_t journalLen;
static uint32_t journalLost, journalAt;
static portMUX_TYPE journalLock = portMUX_INITIALIZER_UNLOCKED;

inline void journal(const String &text) {
  char line[JOURNAL_LINE + 24];
  const time_t now = time(nullptr);
  size_t n;
  if (now > 1700000000) {
    struct tm t;
    localtime_r(&now, &t);
    n = strftime(line, 24, "%d.%m %H:%M:%S ", &t);
  } else {
    n = snprintf(line, 24, "+%lus ", (unsigned long)(millis() / 1000));
  }
  for (size_t i = 0; i < text.length() && n < sizeof line - 1; i++) {
    const char ch = text[i];
    line[n++] = (ch >= 32 && ch <= 126) || (uint8_t)ch >= 0x80 ? ch : ' ';
  }
  line[n++] = '\n';
  portENTER_CRITICAL(&journalLock);
  if (journalLen + n <= sizeof journalHeld) {
    memcpy(journalHeld + journalLen, line, n);
    journalLen += n;
  } else {
    journalLost++;
  }
  portEXIT_CRITICAL(&journalLock);
}

// From the loop only. Now is for a restart, which would take the lines with it.
inline void journalFlush(const bool now = false) {
  if (!journalLen || (!now && (int32_t)(millis() - journalAt) < 0))
    return;
  journalAt = millis() + JOURNAL_EVERY;
  char out[JOURNAL_HELD];          // on the loop's stack, not kept for good
  portENTER_CRITICAL(&journalLock);
  const size_t n = journalLen;
  memcpy(out, journalHeld, n);
  journalLen = 0;
  const uint32_t lost = journalLost;
  journalLost = 0;
  portEXIT_CRITICAL(&journalLock);

  SD_MMC.mkdir("/log");
  File probe = SD_MMC.open(JOURNAL_PATH);
  const size_t size = probe ? probe.size() : 0;
  if (probe)
    probe.close();
  if (size > JOURNAL_MAX) {
    SD_MMC.remove(JOURNAL_OLD);
    SD_MMC.rename(JOURNAL_PATH, JOURNAL_OLD);
  }
  File file = SD_MMC.open(JOURNAL_PATH, FILE_APPEND);
  if (!file)
    return;
  if (lost)
    file.print(String("(") + lost + " lines lost, too many at once)\n");
  file.write((const uint8_t *)out, n);
  file.close();
}

// The end of the file, plus whatever has not been written out yet, a piece at a time
// from the stack: a buffer kept for this alone cost the heap the room TLS needs.
// Read in the loop, which is the only one that empties what is held.
inline void journalSend(WebServer &web, const bool previous) {
  char buf[512];
  File file = SD_MMC.open(previous ? JOURNAL_OLD : JOURNAL_PATH);
  const size_t size = file ? file.size() : 0;
  size_t from = size > JOURNAL_TAIL ? size - JOURNAL_TAIL : 0;
  size_t first = 0, cut = 0;
  if (file) {
    file.seek(from);
    first = file.read((uint8_t *)buf, sizeof buf);
    if (from && first > 0) {
      const char *nl = (const char *)memchr(buf, '\n', first);
      if (nl)
        cut = nl - buf + 1;          // starts on a whole line
    }
  }
  portENTER_CRITICAL(&journalLock);
  const size_t held = previous ? 0 : journalLen;
  portEXIT_CRITICAL(&journalLock);
  const size_t total = (size - from - cut) + held;
  if (!total) {
    if (file)
      file.close();
    web.send(404, "text/plain", "nothing written down yet\n");
    return;
  }
  web.setContentLength(total);
  web.send(200, "text/plain; charset=utf-8", "");
  if (first > cut)
    web.sendContent_P(buf + cut, first - cut);
  int got;
  while (file && (got = file.read((uint8_t *)buf, sizeof buf)) > 0)
    web.sendContent_P(buf, got);
  if (file)
    file.close();
  if (held)
    web.sendContent_P(journalHeld, held);
}
