#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <SD_MMC.h>

#include "../ESP8266WirelessPrintAsync/BinaryTransfer.h"
#include "cardbytes.h"
#include "../ESP8266WirelessPrintAsync/BootGuard.h"
#include "page.h"
#include "printercard.h"
#include "gcodemeta.h"
#include "webauth.h"
#include "telegram.h"
#include "relay.h"
bool relayCarriesTelegram();
#include "../ESP8266WirelessPrintAsync/YandexHome.h"

SemaphoreHandle_t tlsGate;
volatile bool tlsWanted;

volatile uint32_t loopBeat;
volatile bool uploadActive;
static String lastRestart;      // why the guardian last restarted the module, read back from NVS
static void guardian(void *);
#include "ota.h"

#define POWEROFF_NOZZLE   100    // the nozzle has to be below this before mains may go
#define POWEROFF_BED      50     // and the bed below this
#define POWEROFF_GRACE    30000  // everything has to stay that way for this long
#define POWEROFF_RETRY    60000  // between attempts that did not get through
#define POWEROFF_ATTEMPTS 10     // before giving up on this print
#define POWERDRAW_EVERY   60000  // how often the socket is asked what it is drawing
#define POWERDRAW_IDLE    300000 // and how often when nothing is printing

// The module is fed by the printer and dies with it, so nothing about arming may
// outlive a reboot: coming back up already armed would switch the mains off again.
static bool autoPowerOff;        // the wish, kept in NVS
static bool powerArmed;          // this print has ended and the socket may go
static bool powerSawIdle;        // the heaters were seen off, so turning them on is a veto
static uint32_t powerReadySince, powerRetryAt;
static uint8_t powerTries;

#ifndef SKETCH_VERSION
  #define SKETCH_VERSION "unknown"   // set from the git revision at build time, build_version.py
#endif

#define JOIN_WAIT_MS   90000
#define PORTAL_NAME    "KP3S-setup"
#define PRINTER_RX     13      // gpio14 is taken by the SD_MMC clock
#define PRINTER_TX     16      // not gpio12, a strapping pin that stops the board booting
#define PRINTER_BAUD   115200  // What Marlin boots at here; M575 moves it afterwards
#define PRINTER_FAST   2000000 // What a transfer runs at; measured 164 KB/s with no repeats
#define REPLY_WAIT_MS  4000

static HardwareSerial Printer(1);
static size_t printerRxRoom;

static size_t writePrinterAscii(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    Printer.write(data[i]);
    delayMicroseconds(100);
  }
  return len;
}

static void printerLine(const String &line) {
  writePrinterAscii((const uint8_t *)line.c_str(), line.length());
  writePrinterAscii((const uint8_t *)"\n", 1);
}

// The wire, as BinaryTransfer wants to see it.
struct SerialIo : LinkIo {
  int read() { return Printer.available() ? Printer.read() : -1; }
  size_t write(const uint8_t *data, size_t len) { return Printer.write(data, len); }
  size_t writeAscii(const uint8_t *data, size_t len) { return writePrinterAscii(data, len); }
  size_t writable() { return Printer.availableForWrite(); }
  uint32_t now() { return millis(); }
};

// A firmware image packs to nothing and the plain path is the one proven byte for
// byte on this printer, so only gcode is offered for compression.
static bool worthPacking(const char *name) {
  const char *dot = strrchr(name, '.');
  return !dot || strcasecmp(dot, ".bin") != 0;
}

// Upload, send and print are one wish, so they are one piece of state. Each step is
// entered only once the one before it is known to have worked.
enum Chore : uint8_t { ChoreNone, ChoreSending, ChoreDone, ChoreFailed };

static Chore chore;
static String lastUpload;
static bool choreShouldPrint;
static char choreTarget[PRINTER_CARD_NAME_BYTES];
static String choreTrouble;

static bool cardUp;

// The module is fed by the printer, so it comes up whenever the printer does, and the
// card is not always ready at that moment. One attempt was losing it for the whole
// session: twice today the page said the card was empty and a restart brought it back.
static bool mountCard() {
  for (int tries = 0; tries < 5; tries++) {
    if (SD_MMC.begin("/sdcard", true)) {         // 1 bit mode, which leaves gpio13 for the printer
      cardUp = true;
      return true;
    }
    SD_MMC.end();
    delay(200);
  }
  cardUp = false;
  return false;
}

static SerialIo serialIo;
static CardBytes sendBytes;
static BinaryTransfer sender;
static char sendName[BINARY_NAME_BYTES];
static bool sendAsked;
static String sendTrouble;

#define LOG_ROOM   1200
#define LINE_ROOM   300

// One reader, and only one. Five separate places used to pull bytes off this port, so a
// single page request during a transfer could eat the acknowledgement the transfer was
// waiting for and sink a forty second firmware push. Everything that is not the binary
// sender now goes through here, and everything else reads what this worked out.
static char seenLine[LINE_ROOM];
static size_t seenAt;
static char saidLog[LOG_ROOM];
static size_t saidHeld;
static uint32_t saidSeq;         // counts lines ever kept, so a reader can tell new from old
static bool printing;
static uint32_t printAt, printOf;

static void remember(const char *text, const size_t len) {
  if (len + 1 >= LOG_ROOM)
    return;
  if (saidHeld + len + 1 >= LOG_ROOM) {          // Make room by dropping whole lines
    size_t drop = saidHeld + len + 2 - LOG_ROOM;
    const char *nl = (const char *)memchr(saidLog + drop, '\n', saidHeld - drop);
    drop = nl ? (size_t)(nl - saidLog) + 1 : saidHeld;
    memmove(saidLog, saidLog + drop, saidHeld - drop);
    saidHeld -= drop;
  }
  memcpy(saidLog + saidHeld, text, len);
  saidHeld += len;
  saidLog[saidHeld++] = '\n';
  saidSeq++;
}

// The bot's task lives on the other core and only ever carries messages. These are
// the three things it asks of this side.
void logEvent(const String &text) { remember(text.c_str(), text.length()); }
void logFlush() {}
bool isPrinting;                 // what the bot calls it, kept in step with printing
bool somethingIsStreaming();     // the binary sender owns the wire, so the bot waits

static PrinterCard printerCard;
static PrinterCard printerListing;
static bool listingRequested;
static bool sawOk;               // a whole line of it, not the tail of "echo:SD card ok"

static float toolNow = 0, toolSet = 0, bedNow = 0, bedSet = 0;
static uint32_t heardTemp;       // when, so a stale reading can be told from a cold printer
static char machine[32];
static char selected[PRINTER_CARD_NAME_BYTES];
static uint32_t printSince;

// " T:24.37 /0.00 B:23.50 /0.00 @:0 B@:0", from M155's own report and from any ok that
// carries one. Marlin sends it from idle(), so it arrives whatever the queue is doing.
static bool sawTemps(const char *text) {
  const char *t = strstr(text, "T:");
  if (!t || (t != text && t[-1] != ' '))
    return false;
  const char *slash = strchr(t, '/');
  if (!slash)
    return false;
  toolNow = atof(t + 2);
  toolSet = atof(slash + 1);
  const char *b = strstr(slash, "B:");
  if (b) {
    const char *bslash = strchr(b, '/');
    if (bslash) {
      bedNow = atof(b + 2);
      bedSet = atof(bslash + 1);
    }
  }
  heardTemp = millis();
  return true;
}

static void sawLine(const char *text, const size_t len) {
  const bool hadTemps = sawTemps(text);
  if (!strcmp(text, "ok") || !strncmp(text, "ok ", 3)) {
    sawOk = true;
    return;
  }
  if (hadTemps)
    return;                      // the bare autoreport, kept out of the log
  if (listingRequested && printerListing.observe(text))
    return;                      // a listing line belongs to the index and nowhere else
  if (!strncmp(text, "SD printing byte ", 17)) {
    const char *slash = strchr(text + 17, '/');
    if (slash) {
      if (!printing)
        printSince = millis();
      printing = true;
      printAt = strtoul(text + 17, NULL, 10);
      printOf = strtoul(slash + 1, NULL, 10);
    }
    return;                      // The bulk of the traffic, and the state above says it better
  }
  if (!strcmp(text, "Not SD printing")) {
    if (printing) {
      powerArmed = autoPowerOff;
      powerSawIdle = false;
      powerReadySince = 0;
      powerTries = 0;
    }
    printing = false;
    return;
  }
  if (!strncmp(text, "Done printing file", 18)) {
    if (printing) {
      powerArmed = autoPowerOff;
      powerSawIdle = false;
      powerReadySince = 0;
      powerTries = 0;
    }
    printing = false;
    printAt = printOf;
  }
  if (!strcmp(text, "start")) {
    heardTemp = 0;               // its autoreport is off again, and so is what it knew
    machine[0] = 0;
    printing = false;
    selected[0] = 0;
    printAt = printOf = printSince = 0;
    printerCard.begin();
  }
  if (!strncmp(text, "File opened: ", 13)) {
    const char *end = strstr(text + 13, " Size:");
    const size_t room = end ? (size_t)(end - (text + 13)) : strlen(text + 13);
    selected[0] = 0;
    printAt = printOf = printSince = 0;
    if (room && room < sizeof selected) {
      char opened[PRINTER_CARD_NAME_BYTES];
      memcpy(opened, text + 13, room);
      opened[room] = 0;
      const PrinterCardEntry *entry = printerCard.find(opened);
      printOf = end ? strtoul(end + 6, nullptr, 10) : 0;
      if (!entry || entry->size == printOf)
        printerCardResolve(printerCard, opened, selected, sizeof selected);
      else
        snprintf(selected, sizeof selected, "%s", opened);
    }
  }
  remember(text, len);
}

static void feed(const int ch) {
  if (ch < 0 || ch == '\r')
    return;
  if (ch == '\n') {
    seenLine[seenAt] = 0;
    if (seenAt)
      sawLine(seenLine, seenAt);
    seenAt = 0;
    return;
  }
  if (seenAt < sizeof(seenLine) - 1)
    seenLine[seenAt++] = (char)ch;
}

static void drain() {
  while (Printer.available())
    feed(Printer.read());
}

// Writes one line and gathers whatever comes back, up to the first ok or the wait. What it
// reads still goes through the parser, so a reply is never lost to the state.
// Asks the printer what is on its card and fills the index from the answer. The card is
// released after every binary transfer, so M21 goes first every time.
static bool askPrinterCard();

static String say(const String line) {
  if (sender.running())
    return "a transfer owns the wire\n";

  drain();                       // Anything already waiting belongs to the log, not to this reply

  sawOk = false;
  printerLine(line);

  String out;
  const uint32_t until = millis() + REPLY_WAIT_MS;
  while ((int32_t)(millis() - until) < 0) {
    while (Printer.available() && out.length() < 3000) {
      const int ch = Printer.read();
      feed(ch);
      out += (char)ch;
    }
    if (sawOk)
      break;
    delay(5);
  }
  return out;
}

static const char *choreName() {
  switch (chore) {
    case ChoreSending: return "sending";
    case ChoreDone:    return "done";
    case ChoreFailed:  return "failed";
    default:           return "none";
  }
}

// What the scanner finds is written beside the file, under the name the printer will
// know it by, so the job can be looked up from what the printer says rather than from
// what we called it here.
#define META_DIR "/meta"

static String metaPath(const char *target, const char *suffix) {
  return String(META_DIR "/") + target + suffix;
}

static String metaReadPath(const char *name, const char *suffix) {
  char target[PRINTER_CARD_NAME_BYTES];
  return printerCardResolve(printerCard, name, target, sizeof target)
    ? metaPath(target, suffix) : String();
}

struct MetaOut : GcodeMetaSink {
  File into;
  File fans;
  File picture;
  String tmp, keepAs;

  void layer(const GcodeMetaLayer &one) override {
    if (into)
      into.write((const uint8_t *)&one, sizeof one);
  }
  void fan(const GcodeMetaFan &one) override {
    if (fans)
      fans.write((const uint8_t *)&one, sizeof one);
  }

  // A slicer writes its sizes one after another and only the last one is worth having,
  // so each goes to a file of its own and the loser is thrown away at once.
  void thumbOpen(uint16_t, uint16_t) override {
    if (picture)
      picture.close();
    SD_MMC.remove(tmp);
    picture = SD_MMC.open(tmp, FILE_WRITE);
  }
  void thumbByte(uint8_t value) override {
    if (picture)
      picture.write(value);
  }
  void thumbClose(bool keep) override {
    if (!picture)
      return;
    picture.close();
    if (!keep) {
      SD_MMC.remove(tmp);
      return;
    }
    SD_MMC.remove(keepAs);
    SD_MMC.rename(tmp, keepAs);
  }
};

static GcodeMetaScan metaScan;
static MetaOut metaOut;
static char metaTarget[PRINTER_CARD_NAME_BYTES];
static bool metaWanted;

static bool metaHead(const char *target, GcodeMetaHead &head) {
  File one = SD_MMC.open(metaReadPath(target, ".hdr"));
  if (!one)
    return false;
  const bool good = one.read((uint8_t *)&head, sizeof head) == (int)sizeof head &&
                    head.magic == GCODEMETA_MAGIC;
  one.close();
  char canonical[PRINTER_CARD_NAME_BYTES];
  if (!good || !printerCardResolve(printerCard, target, canonical, sizeof canonical))
    return false;
  const PrinterCardEntry *entry = printerCard.find(canonical);
  char current[PRINTER_CARD_NAME_BYTES];
  const bool active = printerCardResolve(printerCard, selected, current, sizeof current) &&
                      printerCardSame(current, canonical);
  return (!entry || entry->size == head.size) && (!active || !printOf || printOf == head.size);
}

// The table is on the card, not in memory: a tall print has thousands of rows and the
// answer needs a dozen of them. Seeking beats reading the lot.
static uint32_t metaLayerAt(const char *target, const uint32_t count, const uint32_t byte,
                            uint16_t *z100, uint16_t *minutes) {
  File table = SD_MMC.open(metaReadPath(target, ".idx"));
  if (!table || table.size() != (uint64_t)count * sizeof(GcodeMetaLayer))
    return 0;
  uint32_t low = 0, high = count;
  GcodeMetaLayer one;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    table.seek(mid * sizeof one);
    if (table.read((uint8_t *)&one, sizeof one) != (int)sizeof one)
      return 0;
    if (one.at <= byte)
      low = mid + 1;
    else
      high = mid;
  }
  if (low) {
    table.seek((low - 1) * sizeof one);
    if (table.read((uint8_t *)&one, sizeof one) == (int)sizeof one) {
      if (z100) *z100 = one.z100;
      if (minutes) *minutes = one.minutes;
    }
    else
      return 0;
  }
  table.close();
  return low;
}

static int metaFanAt(const char *target, const uint32_t count, const uint32_t byte) {
  File table = SD_MMC.open(metaReadPath(target, ".fan"));
  if (!table || table.size() != (uint64_t)count * sizeof(GcodeMetaFan))
    return -1;
  uint32_t low = 0, high = count;
  GcodeMetaFan one;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    table.seek(mid * sizeof one);
    if (table.read((uint8_t *)&one, sizeof one) != (int)sizeof one)
      return -1;
    if (one.at <= byte)
      low = mid + 1;
    else
      high = mid;
  }
  int speed = 0;
  if (low) {
    table.seek((low - 1) * sizeof one);
    if (table.read((uint8_t *)&one, sizeof one) == (int)sizeof one)
      speed = one.speed;
    else
      return -1;
  }
  table.close();
  return speed;
}

// One body for both upload routes: the module's own and the one a slicer speaks.
static File upInto;
static uint8_t upLined[2048] __attribute__((aligned(32)));   // SD_MMC moves this over DMA

// One upload onto the module's card, whoever carries it: the web server's form parser
// or the relay, which hands over the file itself without a form around it.
static bool uploadBegin(const String &filename) {
  metaWanted = false;
  sendTrouble = "";
  lastUpload = filename;
  if (!cardUp) {
    sendTrouble = "the module has no card";
    return false;
  }
  upInto = SD_MMC.open("/" + filename, FILE_WRITE);
  if (!upInto) {
    sendTrouble = "the module's card would not take the file";
    return false;
  }

  // Read once, while it goes past. Reading it again afterwards would be a second
  // pass over megabytes for something that is right here.
  metaWanted = printerCardIsGcode(filename.c_str()) &&
               printerCardTarget(filename.c_str(), metaTarget, sizeof metaTarget);
  if (metaWanted) {
    SD_MMC.mkdir(META_DIR);
    SD_MMC.remove(metaPath(metaTarget, ".hdr"));
    SD_MMC.remove(metaPath(metaTarget, ".png"));
    metaOut.into = SD_MMC.open(metaPath(metaTarget, ".idx"), FILE_WRITE);
    metaOut.fans = SD_MMC.open(metaPath(metaTarget, ".fan"), FILE_WRITE);
    metaOut.tmp = metaPath(metaTarget, ".new");
    metaOut.keepAs = metaPath(metaTarget, ".png");
    metaScan.begin(&metaOut);
  }
  return true;
}

static void uploadWrite(const uint8_t *data, const size_t len) {
  if (!upInto)
    return;
  for (size_t at = 0; at < len; at += sizeof(upLined)) {
    const size_t take = len - at < sizeof(upLined) ? len - at : sizeof(upLined);
    memcpy(upLined, data + at, take);
    if (upInto.write(upLined, take) != take) {
      sendTrouble = "the module's card stopped taking bytes";
      break;
    }
    if (metaWanted)
      metaScan.feed(upLined, take);
  }
}

static void uploadEnd(const uint32_t expected) {
  if (!upInto)
    return;
  upInto.flush();
  upInto.close();
  File stored = SD_MMC.open("/" + lastUpload);
  const uint32_t size = stored ? stored.size() : 0;
  if (!stored || size != expected)
    sendTrouble = "the stored upload size does not match";
  stored.close();
  if (metaWanted) {
    metaScan.finish(size);
    metaOut.picture.close();
    if (metaOut.into)
      metaOut.into.close();
    if (metaOut.fans)
      metaOut.fans.close();
    File head;
    if (sendTrouble == "" && size == expected)
      head = SD_MMC.open(metaPath(metaTarget, ".hdr"), FILE_WRITE);
    if (head) {
      head.write((const uint8_t *)&metaScan.head, sizeof metaScan.head);
      head.close();
    }
    metaWanted = false;
  }
}

static void uploadAbort(const char *why) {
  upInto.close();
  metaOut.into.close();
  metaOut.fans.close();
  metaOut.picture.close();
  metaWanted = false;
  sendTrouble = why;
}

// The relay is writing a file of its own; the web server's form must not cut in
static volatile bool relayPutting;

static void takeUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (relayPutting) {
      sendTrouble = "a file is coming in through the relay";
      return;
    }
    uploadBegin(up.filename);
  }
  else if (up.status == UPLOAD_FILE_WRITE && upInto && !relayPutting) {
    loopBeat = millis();
    uploadWrite(up.buf, up.currentSize);
  }
  else if (up.status == UPLOAD_FILE_END && upInto && !relayPutting)
    uploadEnd(up.totalSize);
  else if (up.status == UPLOAD_FILE_ABORTED && !relayPutting)
    uploadAbort("upload aborted");
}

// Takes a file on the module's card all the way to the printer, and onto it if asked.
// Says no rather than half doing it, with the reason left in choreTrouble.
bool somethingIsStreaming() { return otaActive.load() || sender.running(); }

String relayAuthHello() { return webAuthForWorker(); }

// The printer's buzzer, for the two things worth hearing from across the room: a file
// that reached the printer's card whole, and the module coming back on new firmware.
// Kept behind a setting because test runs go by the dozen in a lived in room.
static bool soundOn = true;

static void chirp(const int times) {
  if (!soundOn || printing || sender.running())
    return;
  for (int i = 0; i < times; i++) {
    Printer.println("M300 S2000 P90");
    if (i + 1 < times)
      Printer.println("M300 S0 P70");
  }
}

// A file sent to the bot, waiting to become a chore, and the chat to tell when it ends
static String fileChat, fileName, choreChat, choreFile;

// Said once when a print starts and once when it stops, to everyone the bot knows.
static void tellTheBot(const String &text) {
  if (telegramBot.hasToken() || relayCarriesTelegram())
    telegramBot.notify(text);
}

static String spanWords(const int32_t seconds) {
  if (seconds < 0)
    return "unknown";
  const int32_t minutes = seconds / 60;
  if (minutes < 60)
    return String(minutes) + " min";
  return String(minutes / 60) + " h " + String(minutes % 60) + " min";
}

static bool choreRetried;        // the transfer to the printer has been tried a second time

static bool startChore(const String &name, const bool andPrint, const uint8_t window = 2) {
  choreTrouble = "";
  if (sender.running()) {
    choreTrouble = "a transfer is already running";
    return false;
  }
  if (printing) {
    choreTrouble = "the printer is printing, the wire is not free";
    return false;
  }
  if (name == "") {
    choreTrouble = "name is empty";
    return false;
  }
  sendBytes.shut();
  if (!sendBytes.open(("/" + name).c_str())) {
    choreTrouble = "no such file on the module's card";
    return false;
  }
  if (!printerCardTarget(name.c_str(), choreTarget, sizeof choreTarget)) {
    sendBytes.shut();
    choreTrouble = "that name cannot be put on the printer";
    return false;
  }
  strncpy(sendName, name.c_str(), sizeof(sendName) - 1);
  sendName[sizeof(sendName) - 1] = 0;
  choreShouldPrint = andPrint;
  sender.windowLimit = window;
  chore = ChoreSending;
  sendAsked = true;
  return true;
}

// The module's card is a place to pass through, not a place to keep things, so an
// upload goes on to the printer's card either way. andPrint only says whether to start
// it when it lands. Answers as the slicer expects.
static String fileLanded(const bool andPrint) {
  String said = andPrint ? "printing" : "sending";
  if (!startChore(lastUpload, andPrint))
    said = choreTrouble.length() ? choreTrouble : "the printer is busy";
  else if (fileChat != "") {
    choreChat = fileChat;            // it came from the bot, so the bot says how it ended
    choreFile = fileName;
    fileChat = "";
  }
  return "{\"done\":true,\"name\":\"" + lastUpload + "\",\"said\":\"" + said + "\"}";
}

// A file the relay writes straight onto the card, with no form and no loopback around
// it. The writing happens on the relay's task; what comes after, starting the transfer
// to the printer, belongs to the loop, so the relay hands over and waits for the answer.
static volatile bool putLanded;
static bool putPrint;
static String putPath, putAnswer, putType;
static int putCode;
static SemaphoreHandle_t putDone;

static void putFinished() {
  relayPutting = false;
  uploadActive = false;
}

int relayPutBegin(const String &said, const String &key, const String &once, const String &name, String &why) {
  if (!webAuthAllowsWith("POST", said, key, once, "")) {
    why = "the password was not right";
    return 401;
  }
  if (relayPutting || uploadActive || otaActive.load()) {
    why = "another upload is under way";
    return 409;
  }
  relayPutting = true;
  uploadActive = true;
  if (!uploadBegin(name)) {
    why = sendTrouble;
    sendTrouble = "";
    putFinished();
    return 503;
  }
  return 0;
}

void relayPutWrite(const uint8_t *data, const size_t len) {
  uploadWrite(data, len);
}

void relayPutAbort() {
  uploadAbort("the upload broke off");
  sendTrouble = "";
  putFinished();
}

int relayPutEnd(const uint32_t size, const bool andPrint, const String &path, String &body, String &type) {
  uploadEnd(size);
  putPrint = andPrint;
  putPath = path;
  xSemaphoreTake(putDone, 0);
  putLanded = true;
  const bool answered = xSemaphoreTake(putDone, pdMS_TO_TICKS(10000)) == pdTRUE;
  putFinished();
  if (!answered) {
    type = "text/plain";
    body = "the file is on the card, the loop did not answer\n";
    return 504;
  }
  body = putAnswer;
  type = putType;
  return putCode;
}

static void finishPut() {
  if (sendTrouble != "") {
    putCode = putPath.startsWith("/api/files/local") ? 500 : 400;
    putType = "text/plain";
    putAnswer = sendTrouble + "\n";
    sendTrouble = "";
  }
  else if (putPath.startsWith("/api/files/local")) {
    putCode = 201;
    putType = "application/json";
    putAnswer = fileLanded(putPrint);
  }
  else {
    putCode = 200;
    putType = "text/plain";
    putAnswer = "stored\n";
  }
}


// A transfer leaves the printer a moment short of reading gcode again, and anything
// sent into that gap is swallowed as packet bytes rather than answered. So the wire is
// not trusted until a plain command has come back with an ok of its own.
// Only the firmware can tell a finished print from a gap between heater pulses: the
// socket reads nine watts with the printer idle, and a threshold on that once cut a
// print in half. So this waits for both heaters to be off and cold and to stay that
// way, and anything that turns a heater back on takes the arming away.
static void powerOffTick(const uint32_t now) {
  if (!powerArmed || printing || sender.running())
    return;

  if (toolSet > 0 || bedSet > 0) {
    if (powerSawIdle)
      powerArmed = false;        // the heaters were off and somebody turned them back on
    powerReadySince = 0;         // or the tail of the file is still draining out
    return;
  }
  powerSawIdle = true;

  if (toolNow >= POWEROFF_NOZZLE || bedNow >= POWEROFF_BED) {
    powerReadySince = 0;
    return;
  }

  if (!powerReadySince) {
    powerReadySince = now;
    powerRetryAt = now;
    powerTries = 0;
  }
  if (now - powerReadySince < POWEROFF_GRACE)
    return;
  if (yandexHome.busy() || (int32_t)(now - powerRetryAt) < 0)
    return;

  if (powerTries >= POWEROFF_ATTEMPTS) {
    powerArmed = false;
    remember("gave up switching the printer off", 33);
    return;
  }
  powerTries++;
  powerRetryAt = now + POWEROFF_RETRY;
  yandexHome.request(YandexHome::PowerOff);
}

static String powerOffState() {
  if (!autoPowerOff)
    return "disabled";
  if (!powerArmed)
    return "waiting for a print to finish";
  if (!powerReadySince)
    return "armed, printer still warm";
  const uint32_t waited = millis() - powerReadySince;
  return waited < POWEROFF_GRACE
    ? "switching off in " + String((POWEROFF_GRACE - waited) / 1000) + "s"
    : "switching off now, attempt " + String(powerTries);
}

// A printer that answers nothing is usually not a printer that is busy. It is a
// printer still in packet mode from a transfer that broke off, reading every gcode
// line as packet bytes. It will stay there for good, so after the plain asking fails
// it is handed the one packet that gets it out and asked again.
static uint32_t linkRate = PRINTER_BAUD;

// An empty packet, owned by no transfer, for letting a printer out of packet mode when
// the transfer that put it there died with the module. SYNC is the one packet Marlin
// takes whatever its number, so it is the way back in; ABORT drops the half file and
// CLOSE returns it to gcode.
static void loosePacket(const uint8_t number, const uint8_t protocol, const uint8_t type) {
  uint8_t one[8] = {0xAD, 0xB5, number, (uint8_t)((protocol << 4) | type), 0, 0, 0, 0};
  uint16_t low = 0, high = 0;
  for (int i = 2; i < 6; i++) {
    low = (low + one[i]) % 255;
    high = (high + low) % 255;
  }
  one[6] = low;
  one[7] = high;
  Printer.write(one, sizeof one);
}

static bool askedBack() {
  for (int tries = 0; tries < 3; tries++) {
    say("M105");
    if (sawOk)
      return true;
    delay(100);
  }
  return false;
}

// A printer that answers nothing is seldom busy. Either a transfer that died with the
// module left it reading packets, or it is still at the fast rate from before the
// module rebooted. Both are tried before giving up, and the rate that answers is kept.
static bool settleWire() {
  uint32_t rates[2];
  rates[0] = linkRate;
  rates[1] = linkRate == PRINTER_BAUD ? PRINTER_FAST : PRINTER_BAUD;
  for (const uint32_t rate : rates) {
    if (rate != linkRate) {
      Printer.updateBaudRate(rate);
      delay(50);
      drain();
    }
    if (askedBack()) {
      linkRate = rate;
      return true;
    }
    // A reboot can land in the middle of a 1 KB packet, and Marlin then waits for its
    // tail, taking anything sent as part of it. It gives up after 500 ms with nothing
    // arriving (PACKET_MAX_WAIT in binary_stream.h), and every byte restarts that wait,
    // so the line is left quiet for longer than that before the way out goes down it.
    const uint32_t quiet = millis();
    while (millis() - quiet < 700) {
      drain();
      delay(10);
    }
    loosePacket(0, 0, 1);
    loosePacket(1, 1, 4);
    loosePacket(2, 0, 2);
    delay(150);
    drain();
    if (askedBack()) {
      linkRate = rate;
      return true;
    }
  }
  Printer.updateBaudRate(linkRate);
  delay(50);
  drain();
  return false;
}

// Both ends move together. M575 is not saved, so a power cycle undoes a mistake.
static bool moveLink(const uint32_t rate) {
  if (rate == linkRate)
    return true;
  printerLine("M575 B" + String(rate));
  Printer.flush();
  delay(200);
  Printer.updateBaudRate(rate);
  linkRate = rate;
  delay(200);
  drain();
  return askedBack();
}

static bool askPrinterCard() {
  if (sender.running() || sendAsked || printing)
    return false;
  if (!settleWire())
    return false;
  if (printing)
    return false;
  say("M21");
  if (printing)
    return false;
  printerListing.begin();
  listingRequested = true;
  say("M20 L");
  listingRequested = false;
  if (!printerListing.complete() || printerListing.rejected() || printerListing.dropped())
    return false;
  printerCard = printerListing;
  return true;
}

static String openPrinterFile(const char *name) {
  if (printing || sender.running() || sendAsked)
    return "the printer is busy\n";
  char canonical[PRINTER_CARD_NAME_BYTES];
  if (!printerCardResolve(printerCard, name, canonical, sizeof canonical))
    return "the filename is empty or ambiguous\n";
  const PrinterCardEntry *entry = printerCard.find(canonical);
  if (!entry)
    return "the file is not in the printer listing\n";
  if (printerCard.find(entry->dos) != entry)
    return "the printer listing has an ambiguous DOS alias\n";
  return say("M23 " + String(entry->dos));
}

// Starts a file that is already on the printer's card. Empty when it started, the
// reason when it did not. listed says the card was asked for a moment ago.
static String printOnPrinter(const String &name, const bool listed = false) {
  if (sender.running())
    return "a transfer owns the wire";
  if (printing || sendAsked)
    return "the printer is busy";
  if (name == "")
    return "name is empty";
  if (!listed && !askPrinterCard())
    return "the printer did not finish a listing";
  const String opened = openPrinterFile(name.c_str());
  if (opened.indexOf("File opened") < 0)
    return "the printer would not open it";
  say("M27 S3");                 // It reports on its own from idle(), so a full queue cannot delay it
  say("M24");
  return "";
}

static const char *phaseName(const BinaryPhase p) {
  switch (p) {
    case BinaryIdle:      return "idle";
    case BinarySwitching: return "switching";
    case BinarySyncing:   return "syncing";
    case BinaryAsking:    return "asking";
    case BinaryOpening:   return "opening";
    case BinaryWriting:   return "writing";
    case BinaryShutting:  return "closing";
    case BinaryLeaving:   return "leaving binary";
    case BinaryAborting:  return "aborting";
    case BinaryRecoverSync: return "recover sync";
    case BinaryRecoverAbort: return "recover abort";
    case BinaryRecoverClose: return "recover close";
    case BinaryDone:      return "done";
    case BinaryFailed:    return "failed";
  }
  return "?";
}

static Preferences store;
static bool portal;

static String slot() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  return running ? String(running->label) : String("?");
}

static void startPortal() {
  portal = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(PORTAL_NAME);

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html",
      "<!doctype html><meta charset=utf-8><title>KP3S</title>"
      "<form method=POST action=/join><p><input name=ssid placeholder=network required>"
      "<p><input name=pass type=password placeholder=password>"
      "<p><button>Join</button></form>");
  });

  server.on("/join", HTTP_POST, []() {
    const String ssid = server.arg("ssid");
    const String pass = server.arg("pass");
    server.send(200, "text/plain", "trying, the module reboots now\n");
    WiFi.persistent(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    delay(1500);
    ESP.restart();
  });

  server.begin();
}

void setup() {
  Serial.begin(115200);
  bootGuardBegin();                 // First, so everything after it is allowed to be broken

  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin();                       // Whatever was stored by whoever set this module up

  const uint32_t until = millis() + JOIN_WAIT_MS;
  while (WiFi.status() != WL_CONNECTED && (int32_t)(millis() - until) < 0)
    delay(250);

  if (WiFi.status() != WL_CONNECTED) {
    startPortal();
    return;
  }

  webAuthBegin();

  route("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE_HTML); });
  // Everything the page draws, in the shape it has always expected. What this build
  // cannot know is said as nothing rather than guessed: the page hides those rows.
  route("/api/status", HTTP_GET, []() {
    const bool heard = heardTemp && millis() - heardTemp < 20000;
    const uint32_t ran = printing && printSince ? (millis() - printSince) / 1000 : 0;
    uint64_t freeCard = 0;
    if (cardUp)
      freeCard = SD_MMC.totalBytes() - SD_MMC.usedBytes();

    String j = "{\"bare\":true";
    j += ",\"printing\":" + String(printing ? "true" : "false");
    j += ",\"locked\":" + String(webAuthSet() ? "true" : "false");
    j += ",\"connected\":" + String(heard ? "true" : "false");
    j += ",\"state\":\"" + String(printing ? "printing" : (heard ? "ready" : "quiet")) + "\"";
    j += ",\"at\":" + String(printAt) + ",\"of\":" + String(printOf);
    j += ",\"slot\":\"" + slot() + "\"";
    j += ",\"uptime\":" + String(millis() / 1000);
    j += ",\"heap\":" + String(ESP.getFreeHeap());
    j += ",\"block\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    j += ",\"card\":" + String(cardUp ? "true" : "false");
    j += ",\"guard\":\"" + bootGuardReport() + "\"";

    j += ",\"system\":{\"machine\":\"" + String(machine[0] ? machine : "KP3S") + "\"";
    j += ",\"resetReason\":" + String((int)esp_reset_reason());
    j += ",\"guardianRestart\":\"" + lastRestart + "\"";
    j += ",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -100);
    j += ",\"version\":\"" SKETCH_VERSION "\"}";

    // What the slicer wrote down, looked up by the byte the printer says it is on
    GcodeMetaHead meta;
    uint32_t layer = 0, layers = 0;
    uint16_t z100 = 0, left = 0;
    double grams = 0;
    int32_t remaining = -1, fan = -1;
    if (selected[0] && cardUp && metaHead(selected, meta)) {
      layers = meta.found ? meta.found : meta.layers;
      grams = meta.grams;
      if (printing && printOf) {
        layer = metaLayerAt(selected, meta.found, printAt, &z100, &left);
        fan = meta.fans ? metaFanAt(selected, meta.fans, printAt) : -1;
        remaining = left ? (int32_t)left * 60 : -1;
        if (remaining < 0 && meta.seconds)
          remaining = (int32_t)(meta.seconds * (1.0 - (double)printAt / printOf));
      }
      else if (!printing && meta.seconds)
        remaining = (int32_t)meta.seconds;
    }

    j += ",\"job\":{\"file\":\"" + String(selected[0] ? selected : "—") + "\"";
    j += ",\"completion\":" + String(printOf ? (double)printAt * 100.0 / printOf : 0.0, 2);
    j += ",\"layer\":" + String(layer) + ",\"layers\":" + String(layers);
    j += ",\"z\":" + String(z100 / 100.0, 2);
    j += ",\"elapsed\":" + String(ran) + ",\"remaining\":" + String(remaining) +
         ",\"fan\":" + String(fan) + "}";

    const float watt = yandexHome.getPowerAt() &&
                       millis() - yandexHome.getPowerAt() < 2 * POWERDRAW_IDLE
                     ? yandexHome.getPower() : -1;
    j += ",\"energy\":{\"watt\":" + String(watt, 1) +
         ",\"wh\":0,\"grams\":" + String(grams, 2) + ",\"cost\":0,\"filament\":0,\"power\":0}";
    j += ",\"socket\":{\"auto\":" + String(autoPowerOff ? "true" : "false") +
         ",\"token\":" + String(yandexHome.hasToken() ? "true" : "false") +
         ",\"device\":" + String(yandexHome.getDeviceId() != "" ? "true" : "false") +
         ",\"name\":\"" + yandexHome.getDeviceName() +
         "\",\"said\":\"" + yandexHome.getStatus() +
         "\",\"state\":\"" + powerOffState() + "\"}";

    j += ",\"temperature\":{\"tool\":{\"actual\":" + String(toolNow, 1) + ",\"target\":" + String(toolSet, 1) + "}";
    j += ",\"bed\":{\"actual\":" + String(bedNow, 1) + ",\"target\":" + String(bedSet, 1) + "}}";

    j += ",\"storage\":{\"free\":" + String((uint32_t)(freeCard / 1024)) +
         ",\"selected\":\"" + String(selected) + "\"}";
    j += ",\"link\":{\"mangled\":0,\"rxBuffer\":" + String(printerRxRoom) + "}";
    j += "}";
    server.send(200, "application/json", j);
  });

  Printer.begin(PRINTER_BAUD, SERIAL_8N1, PRINTER_RX, PRINTER_TX);
  printerRxRoom = Printer.setRxBufferSize(2048);
  mountCard();

  // One line in, whatever the printer says back out. Enough to ask a printer what it is.
  route("/gcode", HTTP_GET, []() {
    if (sender.running()) {
      server.send(409, "text/plain", "a transfer owns the wire\n");
      return;
    }
    const String cmd = server.arg("cmd");
    if (cmd == "") {
      server.send(400, "text/plain", "cmd is empty\n");
      return;
    }

    const String out = say(cmd);
    server.send(200, "text/plain", out == "" ? "the printer said nothing\n" : out);
  });

  sender.begin(&serialIo);

  route("/upload", HTTP_POST, []() {
    if (sendTrouble != "") {
      const String why = sendTrouble;
      sendTrouble = "";
      server.send(400, "text/plain", why + "\n");
      return;
    }
    server.send(200, "text/plain", "stored\n");
  }, takeUpload);

  // What a slicer speaks. Only the two calls Orca actually makes are here.
  route("/api/version", HTTP_GET, []() {
    server.send(200, "application/json", "{\"api\":\"0.1\",\"server\":\"1.3.10\",\"text\":\"OctoPrint (KP3S module)\"}");
  });

  route("/api/files/local", HTTP_POST, []() {
    if (sendTrouble != "") {
      const String why = sendTrouble;
      sendTrouble = "";
      server.send(500, "application/json", "{\"error\":\"" + why + "\"}");
      return;
    }
    const String wanted = server.arg("print");
    server.send(201, "application/json", fileLanded(wanted == "true" || wanted == "1"));
  }, takeUpload);


  route("/files", HTTP_GET, []() {
    if (!cardUp) {
      server.send(503, "text/plain", "the module has no card\n");
      return;
    }
    String out;
    File dir = SD_MMC.open("/");
    for (File one = dir.openNextFile(); one; one = dir.openNextFile()) {
      if (!one.isDirectory())
        out += String(one.name()) + "  " + String(one.size()) + "\n";
      one.close();
    }
    dir.close();
    server.send(200, "text/plain", out == "" ? "nothing on the card\n" : out);
  });

  route("/download", HTTP_GET, []() {
    if (!cardUp) {
      server.send(503, "text/plain", "the module has no card\n");
      return;
    }
    if (sender.running()) {
      server.send(503, "text/plain", "a transfer owns the card\n");
      return;
    }
    File one = SD_MMC.open("/" + server.arg("name"));
    if (!one || one.isDirectory()) {
      if (one) one.close();
      server.send(404, "text/plain", "no such file\n");
      return;
    }
    server.streamFile(one, "application/octet-stream");
    one.close();
  });

  // The rest of what the page asks for, in the shapes it has always used.
  route("/api/files", HTTP_GET, []() {
    uint64_t freeCard = 0;
    String j = "{\"files\":[";
    if (cardUp) {
      freeCard = SD_MMC.totalBytes() - SD_MMC.usedBytes();
      File dir = SD_MMC.open("/");
      bool first = true;
      for (File one = dir.openNextFile(); one; one = dir.openNextFile()) {
        if (!one.isDirectory()) {
          const char *name = one.name();
          if (*name == '/')
            name++;
          j += String(first ? "" : ",") + "{\"name\":\"" + String(name) +
               "\",\"size\":" + String((uint32_t)one.size()) + "}";
          first = false;
        }
        one.close();
      }
      dir.close();
    }
    j += "],\"free\":" + String((uint32_t)(freeCard / 1024)) + "}";
    server.send(200, "application/json", j);
  });

  route("/delete", HTTP_POST, []() {
    if (!cardUp) {
      server.send(503, "text/plain", "the module has no card\n");
      return;
    }
    if (!SD_MMC.remove("/" + server.arg("name"))) {
      server.send(404, "text/plain", "no such file\n");
      return;
    }
    server.send(200, "text/plain", "gone\n");
  });

  // start takes the selected file the whole way, since it may not be over there yet
  route("/api/job", HTTP_POST, []() {
    const String body = server.arg("plain");
    if (body.indexOf("cancel") >= 0) {
      if (sender.running()) {
        server.send(409, "text/plain", "a transfer owns the wire\n");
        return;
      }
      server.send(200, "text/plain", say("M524"));
      return;
    }
    if (body.indexOf("start") < 0) {
      server.send(400, "text/plain", "start or cancel\n");
      return;
    }
    // The selected file is the one the printer last opened, so it is on its card already
    const String trouble = printOnPrinter(String(selected));
    if (trouble != "") {
      server.send(409, "text/plain", trouble + "\n");
      return;
    }
    server.send(200, "text/plain", "printing " + String(selected) + "\n");
  });

  route("/api/command", HTTP_POST, []() {
    if (sender.running()) {
      server.send(409, "text/plain", "a transfer owns the wire\n");
      return;
    }
    const String cmd = server.arg("cmd");
    if (cmd == "") {
      server.send(400, "text/plain", "cmd is empty\n");
      return;
    }
    server.send(200, "text/plain", say(cmd));
  });

  // The console, drained the same way the plain progress route drains it: whoever
  // asks gets the lines once, and the count says whether there were any.
  route("/api/console", HTTP_GET, []() {
    String j = "{\"seq\":" + String(saidSeq) + ",\"lines\":[";
    saidLog[saidHeld] = 0;
    bool first = true;
    for (char *at = saidLog; *at; ) {
      char *nl = strchr(at, '\n');
      if (nl)
        *nl = 0;
      String one(at);
      one.replace("\\", "\\\\");
      one.replace("\"", "\\\"");
      j += String(first ? "" : ",") + "\"" + one + "\"";
      first = false;
      if (!nl)
        break;
      at = nl + 1;
    }
    saidHeld = 0;
    j += "]}";
    server.send(200, "application/json", j);
  });

  // The picture was decoded out of the file as it was uploaded, so this is a plain
  // read of a small file. It never changes once written, so it is worth caching hard.
  route("/thumbnail", HTTP_GET, []() {
    if (!cardUp) {
      server.send(503, "text/plain", "the module has no card\n");
      return;
    }
    const String asked = server.arg("name");
    GcodeMetaHead head;
    if (!metaHead(asked.c_str(), head) || !head.thumbLen) {
      server.sendHeader("Cache-Control", "no-cache");
      server.send(404, "text/plain", "no picture for that one\n");
      return;
    }
    const uint32_t claimed = strtoul(server.arg("s").c_str(), nullptr, 10);
    server.sendHeader("Cache-Control", claimed && claimed == head.size
                      ? "public, max-age=604800, immutable" : "no-cache");
    const String path = metaReadPath(asked.c_str(), ".png");

    File one = SD_MMC.open(path);
    if (!one || one.isDirectory() || !one.size()) {
      if (one) one.close();
      server.send(404, "text/plain", "no picture for that one\n");
      return;
    }
    server.streamFile(one, "image/png");
    one.close();
  });

  // The socket. The token goes in here and never comes back out: this server has no
  // password, and a token that can be read is a token that is gone.
  route("/telegram", HTTP_GET, []() {
    String out =
      "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "<title>bot</title><style>body{font:15px/1.5 system-ui,sans-serif;max-width:40rem;margin:0 auto;padding:1rem}"
      "input,button{font:inherit;padding:.4rem .6rem}form{margin:1rem 0;display:inline-block}code{word-break:break-all}</style>"
      "<h1>Telegram bot</h1><p><a href=\"/\">back to the printer</a></p>";
    out += "<p>Token: <b>" + String(telegramBot.hasToken() ? "set" : "not set") + "</b></p>";
    out += "<p>Last said: " + telegramBot.getStatus() + "</p>";
    out += "<form method=post action=/telegram/token><input name=token type=password size=40 "
           "placeholder=\"bot token from BotFather\" required><button>Save</button></form>";
    if (telegramBot.hasToken())
      out += " <form method=post action=/telegram/forget><button>Forget the token</button></form>";

    const String allowed = telegramBot.getChats();
    out += "<h2>Who may give orders</h2>";
    if (allowed == "")
      out += "<p>Nobody yet. Write to the bot and its chat will appear below to be let in.</p>";
    else {
      out += "<ul>";
      int at = 0;
      while (at < (int)allowed.length()) {
        int comma = allowed.indexOf(',', at);
        if (comma < 0)
          comma = allowed.length();
        const String one = allowed.substring(at, comma);
        if (one != "")
          out += "<li><code>" + one + "</code> <form method=post action=\"/telegram/deny?chat=" +
                 one + "\"><button>Remove</button></form></li>";
        at = comma + 1;
      }
      out += "</ul>";
    }

    const String knocked = telegramBot.getSeen();
    if (knocked != "") {
      out += "<h2>Knocked and were turned away</h2><ul>";
      int at = 0;
      while (at < (int)knocked.length()) {
        int comma = knocked.indexOf(',', at);
        if (comma < 0)
          comma = knocked.length();
        const String one = knocked.substring(at, comma);
        if (one != "")
          out += "<li><code>" + one + "</code> <form method=post action=\"/telegram/allow?chat=" +
                 one + "\"><button>Let in</button></form></li>";
        at = comma + 1;
      }
      out += "</ul>";
    }
    server.send(200, "text/html", out);
  });

  route("/telegram/token", HTTP_POST, []() {
    const String given = server.arg("token");
    if (given.indexOf(':') < 10) {
      server.send(400, "text/plain", "that does not look like a bot token\n");
      return;
    }
    telegramBot.setToken(given);      // straight into NVS, never logged and never shown
    server.sendHeader("Location", "/telegram");
    server.send(303, "text/plain", "saved\n");
  });

  route("/telegram/forget", HTTP_POST, []() {
    telegramBot.forgetToken();
    server.sendHeader("Location", "/telegram");
    server.send(303, "text/plain", "gone\n");
  });

  route("/telegram/allow", HTTP_POST, []() {
    telegramBot.allow(server.arg("chat"));
    server.sendHeader("Location", "/telegram");
    server.send(303, "text/plain", "let in\n");
  });

  route("/telegram/deny", HTTP_POST, []() {
    telegramBot.deny(server.arg("chat"));
    server.sendHeader("Location", "/telegram");
    server.send(303, "text/plain", "removed\n");
  });

  // Somewhere to go other than a restart when the card did not come up
  route("/card/mount", HTTP_POST, []() {
    if (sender.running()) {
      server.send(409, "text/plain", "a transfer owns the wire\n");
      return;
    }
    server.send(mountCard() ? 200 : 503, "text/plain",
                cardUp ? "mounted\n" : "the card would not come up\n");
  });

  route("/sound", HTTP_POST, []() {
    soundOn = server.arg("on") != "0";
    Preferences keep;
    keep.begin("sound", false);
    keep.putBool("on", soundOn);
    keep.end();
    server.send(200, "text/plain", soundOn ? "sound on\n" : "sound off\n");
  });

  route("/relay", HTTP_GET, []() {
    String out =
      "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "<title>remote</title><style>body{font:15px/1.5 system-ui,sans-serif;max-width:40rem;margin:0 auto;padding:1rem}"
      "input,button{font:inherit;padding:.4rem .6rem}form{margin:1rem 0}code{word-break:break-all}</style>"
      "<h1>Remote access</h1><p><a href=\"/\">back to the printer</a></p>";
    out += "<p>State: <b>" + relayState() + "</b></p>";
    out += "<p>Uploads: " + relayStats() + "</p>";
    if (!webAuthSet())
      out += "<p>Set a <a href=/auth>password</a> first. The relay will not start without one.</p>";
    out += "<form method=post action=/relay/set>"
           "<p><input name=url size=48 placeholder=\"wss://kp3s.NAME.workers.dev/.device\" value=\"" +
           relayUrl() + "\"></p><p><input name=key type=password size=48 placeholder=\"" +
           String(relayConfigured() ? "device key is set, leave empty to keep it" : "device key") +
           "\"></p><button>Save</button></form>";
    if (relayConfigured())
      out += "<form method=post action=/relay/forget><button>Forget the relay</button></form>";
    server.send(200, "text/html", out);
  });

  route("/relay/set", HTTP_POST, []() {
    const String u = server.arg("url"), k = server.arg("key");
    if (!(u.startsWith("wss://") || u.startsWith("ws://")) || (k != "" && k.length() < 24) ||
        (k == "" && !relayConfigured())) {
      server.send(400, "text/plain", "the address must start with wss:// and the key must be at least 24 characters\n");
      return;
    }
    relaySet(u, k);                   // the key goes straight into NVS and is never shown
    server.sendHeader("Location", "/relay");
    server.send(303, "text/plain", "saved\n");
  });

  route("/relay/forget", HTTP_POST, []() {
    relayForget();
    server.sendHeader("Location", "/relay");
    server.send(303, "text/plain", "gone\n");
  });

  route("/auth", HTTP_GET, []() {
    String out =
      "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "<title>password</title><style>body{font:15px/1.5 system-ui,sans-serif;max-width:40rem;margin:0 auto;padding:1rem}"
      "input,button{font:inherit;padding:.4rem .6rem}form{margin:1rem 0}code{word-break:break-all}</style>"
      "<h1>Who may talk to this module</h1><p><a href=\"/\">back to the printer</a></p>";
    if (!webAuthSet())
      out += "<p><b>No password is set.</b> Anyone on this network can print, upload and "
             "reflash. Nothing will reach the module from outside the house until this "
             "is set.</p>";
    else
      out += "<p>A password is set. The user name is <code>" WEBAUTH_USER "</code>.</p>";
    out += "<form method=post action=/auth/password>"
           "<input name=pass type=password size=24 placeholder=\"new password\" "
           "minlength=8 required><button>Save</button></form>";
    if (webAuthSet()) {
      out += "<form method=post action=/auth/forget><button>Remove the password</button></form>";
      out += "<h2>Key for the slicer</h2><p>Orca cannot do passwords. Paste this as the "
             "API key:</p><p><code>" + webAuthKey() + "</code></p>"
             "<form method=post action=/auth/key><button>Make a new one</button></form>";
    }
    else
      out += "<p>The key for the slicer is shown once a password is set.</p>";
    server.send(200, "text/html", out);
  });

  route("/auth/password", HTTP_POST, []() {
    const String given = server.arg("pass");
    if (given.length() < 8) {
      server.send(400, "text/plain", "that is too short\n");
      return;
    }
    webAuthSetPassword(given);        // straight into NVS, never logged and never shown
    relayAuthChanged();
    server.sendHeader("Location", "/auth");
    server.send(303, "text/plain", "saved\n");
  });

  route("/auth/forget", HTTP_POST, []() {
    webAuthForget();
    relayAuthChanged();
    server.sendHeader("Location", "/auth");
    server.send(303, "text/plain", "gone\n");
  });

  route("/auth/key", HTTP_POST, []() {
    webAuthNewKey();
    relayAuthChanged();
    server.sendHeader("Location", "/auth");
    server.send(303, "text/plain", "made\n");
  });

  route("/yandex", HTTP_GET, []() {
    String out =
      "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "<title>socket</title><style>body{font:15px/1.5 system-ui,sans-serif;max-width:40rem;margin:0 auto;padding:1rem}"
      "input,button,select{font:inherit;padding:.4rem .6rem}form{margin:1rem 0}code{word-break:break-all}</style>"
      "<h1>Yandex socket</h1><p><a href=\"/\">back to the printer</a></p>";
    out += "<p>Token: <b>" + String(yandexHome.hasToken() ? "set" : "not set") + "</b>";
    out += ", device: <b>" + String(yandexHome.getDeviceId() == "" ? "not chosen"
                                    : yandexHome.getDeviceName()) + "</b></p>";
    out += "<p>Last said: " + yandexHome.getStatus() + "</p>";
    out += "<form method=post action=/yandex/token><input name=token type=password size=40 "
           "placeholder=\"OAuth token\" required><button>Save</button></form>";
    out += "<form method=post action=/yandex/forget><button>Forget the token</button></form>";
    out += "<form method=post action=/yandex/devices><button>Find the sockets</button></form>";

    const String found = yandexHome.getDevices();
    if (found != "") {
      out += "<form method=post action=/yandex/device><select name=id>";
      int at = 0;
      while (at < (int)found.length()) {
        int nl = found.indexOf('\n', at);
        if (nl < 0)
          nl = found.length();
        const String row = found.substring(at, nl);
        const int tab = row.indexOf('\t');
        if (tab > 0)
          out += "<option value=\"" + row.substring(0, tab) + "\">" + row.substring(tab + 1) + "</option>";
        at = nl + 1;
      }
      out += "</select><button>Use this one</button></form>";
    }
    out += "<p>Switch off when a print finishes: <b>" + String(autoPowerOff ? "on" : "off") + "</b>, " +
           powerOffState() + "</p>";
    out += "<form method=post action=\"/yandex/auto?on=" + String(autoPowerOff ? "0" : "1") +
           "\"><button>" + String(autoPowerOff ? "Turn it off" : "Turn it on") + "</button></form>";
    server.send(200, "text/html", out);
  });

  route("/yandex/token", HTTP_POST, []() {
    const String given = server.arg("token");
    if (given.length() < 20) {
      server.send(400, "text/plain", "that is too short to be a token\n");
      return;
    }
    yandexHome.setToken(given);         // straight into NVS, never logged and never shown
    server.sendHeader("Location", "/yandex");
    server.send(303, "text/plain", "saved\n");
  });

  route("/yandex/forget", HTTP_POST, []() {
    yandexHome.forgetToken();
    server.sendHeader("Location", "/yandex");
    server.send(303, "text/plain", "forgotten\n");
  });

  route("/yandex/devices", HTTP_POST, []() {
    yandexHome.request(YandexHome::DeviceList);
    delay(1200);                        // long enough that the page usually shows the answer
    server.sendHeader("Location", "/yandex");
    server.send(303, "text/plain", "asking\n");
  });

  route("/yandex/device", HTTP_POST, []() {
    const String id = server.arg("id");
    String name = id;
    const String found = yandexHome.getDevices();
    const int at = found.indexOf(id + "\t");
    if (at >= 0) {
      const int nl = found.indexOf('\n', at);
      name = found.substring(at + id.length() + 1, nl < 0 ? found.length() : nl);
    }
    yandexHome.setDevice(id, name);
    server.sendHeader("Location", "/yandex");
    server.send(303, "text/plain", "chosen\n");
  });

  route("/yandex/auto", HTTP_POST, []() {
    autoPowerOff = server.arg("on") == "1";
    if (!autoPowerOff)
      powerArmed = false;
    Preferences settings;
    settings.begin("yandex", false);
    settings.putBool("autooff", autoPowerOff);
    settings.end();
    server.send(200, "text/plain", autoPowerOff ? "will switch off after a print\n" : "will stay on\n");
  });

  route("/yandex/off", HTTP_POST, []() {
    if (!yandexHome.request(YandexHome::PowerOff)) {
      server.send(409, "text/plain", "the socket is busy\n");
      return;
    }
    server.send(200, "text/plain", "switching off\n");
  });

  route("/restart", HTTP_POST, []() {
    if (printing) {
      server.send(409, "text/plain", "the printer is printing\n");
      return;
    }
    server.send(200, "text/plain", "restarting\n");
    delay(200);
    ESP.restart();
  });

  route("/printer/files", HTTP_GET, []() {
    const bool fresh = askPrinterCard();
    if (!fresh && !printerCard.complete()) {
      server.send(503, "text/plain", "no cached printer list; refresh when the printer is idle\n");
      return;
    }
    String out = fresh ? "" : "cached: last complete printer listing; refresh when idle\n";
    for (uint8_t i = 0; i < printerCard.count(); i++) {
      const PrinterCardEntry &e = printerCard.at(i);
      out += String(e.dos) + "  " + String(e.size) + "  " + (e.name[0] ? e.name : e.dos) + "\n";
    }
    if (!printerCard.count())
      out += "nothing on the printer's card\n";

    server.send(200, "text/plain", out);
  });

  route("/printer/send", HTTP_POST, []() {
    const String window = server.arg("window");
    if (server.hasArg("window") && window != "1" && window != "2") {
      server.send(400, "text/plain", "window must be 1 or 2\n");
      return;
    }
    if (!startChore(server.arg("name"), server.arg("print") == "1", window == "1" ? 1 : 2)) {
      server.send(choreTrouble == "no such file on the module's card" ? 404 : 409,
                  "text/plain", choreTrouble + "\n");
      return;
    }
    server.send(200, "text/plain", "taken as " + String(choreTarget) + ", watch /printer/send/status\n");
  });

  // The printer prints from its own card. Nothing here feeds it gcode, which is the
  // whole point: the module can reboot, lose the network or be reflashed mid print and
  // the printer will not notice. M21 is sent every time because a binary transfer leaves
  // the card unmounted behind it.
  // Deleting on the printer, not here. The sidecars stay: they cost a couple of
  // kilobytes and they are still right if the same file goes over again.
  route("/printer/delete", HTTP_POST, []() {
    if (sender.running()) {
      server.send(409, "text/plain", "a transfer owns the wire\n");
      return;
    }
    if (printing) {
      server.send(409, "text/plain", "the printer is printing\n");
      return;
    }
    const String name = server.arg("name");
    if (name == "") {
      server.send(400, "text/plain", "name is empty\n");
      return;
    }
    say("M21");
    const String said = say("M30 " + name);
    if (said.indexOf("File deleted") < 0) {
      server.send(404, "text/plain", "the printer would not delete it:\n" + said);
      return;
    }
    server.send(200, "text/plain", "deleted " + name + "\n");
  });

  route("/printer/print", HTTP_POST, []() {
    const String name = server.arg("name");
    const String trouble = printOnPrinter(name);
    if (trouble != "") {
      server.send(trouble.startsWith("the printer would not") ? 404 : 409, "text/plain", trouble + "\n");
      return;
    }
    server.send(200, "text/plain", "printing " + name + "\n");
  });

  // Both ends have to move together. M575 is not saved, so a power cycle undoes a mistake.
  route("/printer/baud", HTTP_POST, []() {
    if (sender.running()) {
      server.send(409, "text/plain", "a transfer owns the wire\n");
      return;
    }
    const uint32_t rate = server.arg("rate").toInt();
    if (rate < 9600) {
      server.send(400, "text/plain", "rate looks wrong\n");
      return;
    }

    moveLink(rate);
    server.send(200, "text/plain", "now at " + String(rate) + ", printer says:\n" + say("M115"));
  });

  route("/printer/send/status", HTTP_GET, []() {
    server.send(200, "text/plain",
      String("phase    ") + phaseName(sender.phase) + "\n" +
      "name     " + sendName + "\n" +
      "written  " + String(sender.written) + " of " + String(sender.total) + "\n" +
      "packets  " + String(sender.packets) + ", resends " + String(sender.resends) + "\n" +
      "retries  asked " + String(sender.requested) + ", silent " + String(sender.silent) +
        ", ack total " + String(sender.ackMs) + "ms, worst " + String(sender.worstAckMs) + "ms\n" +
      "timing   prepare " + String(sender.prepareMs) + "ms, write " + String(sender.writeMs) + "ms\n" +
      "read     SD " + String(sendBytes.readMs) + "ms\n" +
      "note     " + String(sender.note[0] ? sender.note : "none") + "\n" +
      "payload  " + String(sender.theirPayload) + ", their version " + sender.theirVersion + "\n" +
      "window   " + String(sender.windowUsed) + " of " + String(sender.windowLimit) + "\n" +
      "as       " + String(choreTarget) + ", chore " + choreName() +
        (chore == ChoreFailed && choreTrouble.length() ? ", " + choreTrouble : "") + "\n" +
      "packing  " + String(sender.packing ? "on" : "off") +
        ", printer says " + (sender.theirCompression[0] ? sender.theirCompression : "nothing yet") +
        ", on the wire " + String(sender.sent) + "\n" +
      "took     " + String(sender.finishedAt > sender.startedAt
                           ? sender.finishedAt - sender.startedAt : 0) + "ms\n" +
      "trouble  " + String(sender.trouble ? sender.trouble : "none") + "\n");
  });

  route("/update", HTTP_POST, []() {
    const bool good = otaComplete;
    otaComplete = false;
    server.sendHeader("Connection", "close");
    server.send(good ? 200 : 500, "text/plain",
                good ? "OK, rebooting\n" : "failed: " + (otaTrouble.length() ? otaTrouble : String("nothing was written")) + "\n");
    if (good) {
      bootGuardArm();
      delay(300);
      ESP.restart();
    }
  }, onUpload);

  // The socket lives in a task of its own: a handshake takes a second or two, and in
  // the loop that would be a second or two with nobody reading the printer.
  tlsGate = xSemaphoreCreateMutex();
  {
    Preferences settings;
    settings.begin("yandex", true);
    autoPowerOff = settings.getBool("autooff", false);
    settings.end();
  }
  putDone = xSemaphoreCreateBinary();
  relayBegin();                      // first: the bot and Yandex size their stacks by whether the relay is set up
  yandexHome.begin();
  telegramBot.begin();
  {
    Preferences keep;
    keep.begin("guard", false);
    lastRestart = keep.getString("why", "");
    keep.remove("why");
    keep.end();
  }
  loopBeat = millis();
  xTaskCreatePinnedToCore(guardian, "guardian", 3072, nullptr, 2, nullptr, 0);
  {
    Preferences keep;
    keep.begin("sound", true);
    soundOn = keep.getBool("on", true);
    keep.end();
  }

  server.begin();
}

// The bot's task only carries messages. Orders are carried out here, in the loop, so
// nothing the printer owns is touched from two places at once.
static void handleTelegram() {
  static String awaitingStop;        // Cancelling asks twice, and only the chat that asked

  String chat, line;
  while (telegramBot.nextCommand(chat, line)) {
    line.trim();
    const int space = line.indexOf(' ');
    String verb = space < 0 ? line : line.substring(0, space);
    const String rest = space < 0 ? "" : line.substring(space + 1);
    verb.toLowerCase();
    const int addressed = verb.indexOf('@');        // In a group a bot is called /status@thebot
    if (addressed > 0)
      verb = verb.substring(0, addressed);

    if (verb == "/__file") {
      // "<file id> <size> <name>", put together by the relay from what Telegram said
      const int a = rest.indexOf(' '), b = rest.indexOf(' ', a + 1);
      const String fileId = a > 0 ? rest.substring(0, a) : "";
      const uint32_t size = b > a ? (uint32_t)rest.substring(a + 1, b).toInt() : 0;
      const String name = b > 0 ? rest.substring(b + 1) : "file";
      String lower = name;
      lower.toLowerCase();
      const bool bin = lower.endsWith(".bin");
      const bool printerFirmware = bin && lower.startsWith("robin_nano");
      const bool gcode = lower.endsWith(".gcode") || lower.endsWith(".gco") || lower.endsWith(".g");
      const bool forPrinter = gcode || printerFirmware;
      if (!bin && !gcode)
        telegramBot.send(chat, "Send a .gcode to print, a .bin to flash the module, or Robin_nano*.bin "
                               "to flash the printer. This one is " + name + ".");
      else if (size > 20UL * 1024 * 1024)
        telegramBot.send(chat, "That file is " + String(size / 1024 / 1024) +
                               " MB. Telegram only lets a bot fetch 20 MB.");
      else if (!relayCarriesTelegram() || fileId == "")
        telegramBot.send(chat, "Files come in through the Cloudflare relay, and it is not connected.");
      else if (sender.running() || sendAsked || chore == ChoreSending)
        telegramBot.send(chat, "A file is on its way to the printer already. Send this one in a moment.");
      else if (forPrinter && printing)
        telegramBot.send(chat, "A print is running and the wire to the printer is not free. "
                               "Send it when the print is done.");
      else {
        const String token = webAuthMakeOnce();
        if (forPrinter) {
          fileChat = chat;
          fileName = name;
        }
        relaySendTelegramFetch(chat, fileId, forPrinter ? "/api/files/local" : "/update",
                               forPrinter ? "file" : "firmware", name, token);
        telegramBot.send(chat, "Taking " + name + ", " + String(size / 1024) + " KB. About " +
                               String(size / 15000 + 10) + " s.");
      }
    }

    else if (verb == "/start" || verb == "/help") {
      telegramBot.send(chat,
        String(machine[0] ? machine : "KP3S") + "\n\n"
        "/status - what the printer is doing\n"
        "/files - what is on the printer's card\n"
        "/print <number> - start that file\n"
        "/stop - cancel the running print\n"
        "/nozzle <degrees> - set the hotend, 0 to turn it off\n"
        "/bed <degrees> - set the bed\n"
        "/on and /off - the socket\n\n"
        "Send a .gcode and it goes onto the printer's card. Send a .bin and the module "
        "flashes itself and reboots; a print carries on, the printer reads its own card. "
        "Send Robin_nano*.bin and it goes onto the printer's card to flash the printer at "
        "its next power on. Telegram only lets a bot fetch files up to 20 MB.");
    }

    else if (verb == "/status") {
      String out = String(machine[0] ? machine : "KP3S") + "\n";
      out += printing ? "Printing\n" : "Idle\n";
      if (selected[0]) {
        out += "File       " + String(selected) + "\n";
        if (printing && printOf) {
          GcodeMetaHead meta;
          uint16_t z100 = 0, left = 0;
          out += "Done       " + String(printAt * 100.0 / printOf, 1) + " %\n";
          if (cardUp && metaHead(selected, meta) && meta.found) {
            const uint32_t at = metaLayerAt(selected, meta.found, printAt, &z100, &left);
            out += "Layer      " + (at ? String(at) : String("unknown")) + " of " + String(meta.found);
            if (z100)
              out += ", " + String(z100 / 100.0, 2) + " mm";
            out += "\n";
            out += "Left       " + spanWords(left ? (int32_t)left * 60 : -1) + "\n";
          }
          out += "Elapsed    " + spanWords(printSince ? (int32_t)((millis() - printSince) / 1000) : -1) + "\n";
        }
      }
      out += "Nozzle     " + String(toolNow, 1) + " of " + String(toolSet, 0) + "\n";
      out += "Bed        " + String(bedNow, 1) + " of " + String(bedSet, 0) + "\n";
      if (yandexHome.hasToken() && yandexHome.getDeviceId() != "")
        out += "Socket     " + powerOffState() + "\n";
      out += "Module     up " + String(millis() / 1000) + " s, heap " + String(ESP.getFreeHeap());
      telegramBot.send(chat, out);
    }

    else if (verb == "/files") {
      const bool fresh = askPrinterCard();
      if (!fresh && !printerCard.complete())
        telegramBot.send(chat, "No cached printer list. Ask again when the printer is idle.");
      else if (!printerCard.count())
        telegramBot.send(chat, fresh ? "Nothing on the printer's card." : "Cached listing: nothing on the printer's card. Refresh when idle.");
      else {
        String out = fresh ? "On the printer's card:\n" : "Cached printer listing (refresh when idle):\n";
        for (uint8_t i = 0; i < printerCard.count(); i++) {
          const PrinterCardEntry &e = printerCard.at(i);
          out += String(i + 1) + ". " + String(e.name[0] ? e.name : e.dos) +
                 "  " + String(e.size / 1024) + " KB\n";
        }
        out += "\n/print <number> to start one";
        telegramBot.send(chat, out);
      }
    }

    else if (verb == "/print") {
      const int which = rest.toInt();
      if (printing)
        telegramBot.send(chat, "Already printing.");
      else if (sender.running())
        telegramBot.send(chat, "A transfer owns the wire, ask again in a moment.");
      else if (which <= 0)
        telegramBot.send(chat, "Say /files first, then /print with the number beside the one you want.");
      else if (!askPrinterCard() || which > (int)printerCard.count())
        telegramBot.send(chat, "There is no file with that number. Say /files again.");
      else {
        const PrinterCardEntry &e = printerCard.at(which - 1);
        const String name = e.name[0] ? e.name : e.dos;
        const String trouble = printOnPrinter(name, true);
        telegramBot.send(chat, trouble == "" ? "Printing " + name + "." : "Not printing " + name + ": " + trouble + ".");
      }
    }

    else if (verb == "/stop") {
      if (!printing) {
        telegramBot.send(chat, "Nothing is printing.");
        awaitingStop = "";
      }
      else if (rest == "yes" || awaitingStop == chat) {
        awaitingStop = "";
        say("M524");
        telegramBot.send(chat, "Cancelled.");
      }
      else {
        awaitingStop = chat;
        telegramBot.send(chat, "This will cancel the running print. Say /stop again to go ahead.");
      }
    }

    else if (verb == "/nozzle" || verb == "/bed") {
      const bool nozzle = verb == "/nozzle";
      if (rest == "")
        telegramBot.send(chat, nozzle
          ? "Nozzle " + String(toolNow, 1) + " of " + String(toolSet, 0) + ". Say /nozzle <degrees>."
          : "Bed " + String(bedNow, 1) + " of " + String(bedSet, 0) + ". Say /bed <degrees>.");
      else {
        const int wanted = rest.toInt();
        if (wanted < 0 || wanted > (nozzle ? 260 : 110))
          telegramBot.send(chat, "That is outside what this printer will take.");
        else {
          say((nozzle ? "M104 S" : "M140 S") + String(wanted));
          telegramBot.send(chat, String(nozzle ? "Nozzle" : "Bed") + " set to " + String(wanted) + ".");
        }
      }
    }

    else if (verb == "/on" || verb == "/off") {
      if (!yandexHome.hasToken() || yandexHome.getDeviceId() == "")
        telegramBot.send(chat, "No socket is set up.");
      else if (verb == "/off" && printing)
        telegramBot.send(chat, "The printer is printing. Cancel it first if you really mean this.");
      else {
        yandexHome.request(verb == "/on" ? YandexHome::PowerOn : YandexHome::PowerOff);
        telegramBot.send(chat, String("Asked the socket to go ") + (verb == "/on" ? "on." : "off."));
      }
    }

    else
      telegramBot.send(chat, "I do not know that one. Say /help.");
  }
}

// Hangs here leave no reset behind them: the loop has no watchdog in this core, and a
// relay that cannot get the memory for its handshake back never reconnects. This
// watches from outside and restarts the module, which a print does not notice: the
// printer reads its own card.
#define GUARD_LOOP_MS     60000
#define GUARD_RELAY_MS    90000
#define GUARD_BLOCK_FLOOR 6144
#define GUARD_BLOCK_MS    300000

static void guardianRestart(const char *why) {
  Preferences keep;
  keep.begin("guard", false);
  keep.putString("why", why);
  keep.end();
  esp_restart();
}

static void guardian(void *) {
  uint32_t relayDownSince = 0, lowSince = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    const uint32_t now = millis();
    if (otaActive.load())
      continue;
    if (now - loopBeat > GUARD_LOOP_MS)
      guardianRestart("the loop stood still for a minute");

    const bool relayWanted = relayConfigured() && webAuthSet() && WiFi.status() == WL_CONNECTED;
    // Not while a file is going to the printer: a restart there would break the
    // transfer, and the printer would be left waiting in packet mode.
    if (!relayWanted || relayState() == "connected" || somethingIsStreaming() || uploadActive)
      relayDownSince = 0;
    else if (!relayDownSince)
      relayDownSince = now;
    else if (now - relayDownSince > GUARD_RELAY_MS)
      guardianRestart("the relay was down for a minute and a half");

    // An upload breaks the heap into small pieces for as long as it runs and gives
    // them back at the end. Counting that as a hang restarted the module two minutes
    // into every long upload through the relay.
    if (uploadActive || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= GUARD_BLOCK_FLOOR)
      lowSince = 0;
    else if (!lowSince)
      lowSince = now;
    else if (now - lowSince > GUARD_BLOCK_MS)
      guardianRestart("the largest free block stayed under 6 KB for five minutes");
  }
}

void loop() {
  loopBeat = millis();
  if (putLanded) {
    putLanded = false;
    finishPut();
    xSemaphoreGive(putDone);
  }
  server.handleClient();
  isPrinting = printing;             // what the bot's task reads, kept in step from one place
  bootGuardWatch(WiFi.status() == WL_CONNECTED);

  if (portal)
    return;

  if (sendAsked) {
    sendAsked = false;
    if (!printing && !moveLink(PRINTER_FAST))
      moveLink(PRINTER_BAUD);          // the fast rate did not answer, so the plain one carries it
    sender.start(&sendBytes, choreTarget, millis(), worthPacking(choreTarget));
  }
  if (!sender.running())
    drain();                     // The one reader, whenever the sender does not own the wire

  if (sender.running()) {
    sender.service(millis());
    if (!sender.running())
      sendBytes.shut();
  }

  // The transfer is over. Nothing is believed until the printer's own card says so.
  if (chore == ChoreSending && !sender.running() && !sendAsked) {
    const uint32_t was = sendBytes.size();
    if (sender.phase != BinaryDone && !choreRetried) {
      // Sent once more before it is called a failure: a printer that has just been
      // talked to at another rate, or just come out of packet mode, can refuse a step
      // and take the same file whole a moment later.
      choreRetried = true;
      const String name = sendName;
      const bool andPrint = choreShouldPrint;
      if (!startChore(name, andPrint)) {
        choreTrouble = sender.trouble ? sender.trouble : "the transfer did not finish";
        chore = ChoreFailed;
      }
    }
    else if (sender.phase != BinaryDone) {
      choreTrouble = sender.trouble ? sender.trouble : "the transfer did not finish";
      chore = ChoreFailed;
    }
    else if (!worthPacking(choreTarget)) {
      say("M21");
      const String opened = say("M23 " + String(choreTarget));
      if (!printerCardOpened(opened.c_str(), choreTarget, was)) {
        choreTrouble = "the firmware file is not on the printer's card at the right size";
        chore = ChoreFailed;
      }
      else {
        chore = ChoreDone;
        chirp(2);
      }
    }
    else if (!askPrinterCard()) {
      choreTrouble = "the printer did not answer with a listing";
      chore = ChoreFailed;
    }
    else if (!printerCard.holds(choreTarget, was)) {
      choreTrouble = "the file is not on the printer's card at the right size";
      chore = ChoreFailed;
    }
    else if (!choreShouldPrint) {
      chore = ChoreDone;
      chirp(2);
    }
    else {
      const String opened = openPrinterFile(choreTarget);
      if (opened.indexOf("File opened") < 0) {
        choreTrouble = "the printer would not open it";
        chore = ChoreFailed;
      }
      else {
        chirp(2);
        say("M27 S3");
        say("M24");
        chore = ChoreDone;
      }
    }
  }

  static uint32_t busySince;
  if (printing || sender.running() || sendAsked || chore == ChoreSending)
    busySince = millis();
  else if (linkRate != PRINTER_BAUD && millis() - busySince > 10000)
    moveLink(PRINTER_BAUD);

  static uint32_t slowTick;
  if ((int32_t)(millis() - slowTick) >= 0) {
    slowTick = millis() + 2000;
    relayTick(webAuthSet());
    static bool toldFlash;
    if (!toldFlash && heardTemp && bootGuardReport().startsWith("confirmed")) {
      toldFlash = true;
      chirp(3);
    }
  }

  // The printer can be switched off and on under us, so who it is and how often it
  // should report its temperature are asked again whenever it has gone quiet.
  static uint32_t askAgain;
  if (!sender.running() && (int32_t)(millis() - askAgain) >= 0 &&
      (!heardTemp || millis() - heardTemp > 30000)) {
    askAgain = millis() + 15000;
    if (printing) {
      // A printer that rebooted mid print has its autoreport off and nobody to turn it
      // back on. Asked for without waiting, because a wait here would stall the web.
      printerLine("M155 S2");
    }
    else if (settleWire()) {
      const String said = say("M115");
      const int at = said.indexOf("MACHINE_TYPE:");
      if (at >= 0) {
        const int end = said.indexOf(' ', at + 13);
        snprintf(machine, sizeof machine, "%s",
                 said.substring(at + 13, end < 0 ? said.length() : end).c_str());
        say("M155 S2");          // it reports from idle(), so the queue cannot delay it
      }
    }
  }

  handleTelegram();

  static Chore lastChore;
  if (chore != lastChore) {
    lastChore = chore;
    if (chore == ChoreDone || chore == ChoreFailed)
      choreRetried = false;
    if (choreChat != "" && (chore == ChoreDone || chore == ChoreFailed)) {
      String lower = choreFile;
      lower.toLowerCase();
      if (chore == ChoreFailed)
        telegramBot.send(choreChat, "Could not put " + choreFile + " on the printer's card \u2014 " + choreTrouble);
      else if (lower.startsWith("robin_nano"))
        telegramBot.send(choreChat, "Printer firmware is on its card \u2014 " + choreFile +
                                    "\n\nIt is flashed the next time the printer is switched on: /off, then /on.");
      else
        telegramBot.send(choreChat, "On the printer's card \u2014 " + choreFile +
                                    "\n\n/files to see it, /print <number> to start it.");
      choreChat = "";
    }
  }

  static bool toldPrinting;
  if (printing != toldPrinting) {
    toldPrinting = printing;
    if (printing)
      tellTheBot("Printing " + String(selected[0] ? selected : "a file") + ".");
    else
      tellTheBot(printAt && printAt >= printOf
                 ? "Finished " + String(selected[0] ? selected : "the file") + "."
                 : "The print stopped.");
  }

  powerOffTick(millis());

  // What the socket is drawing, asked for rarely: every handshake is sixty kilobytes
  // off the heap for a second, and the heap is what everything else here needs.
  static uint32_t drawAgain;
  if (yandexHome.hasToken() && yandexHome.getDeviceId() != "" &&
      !sender.running() && !yandexHome.busy() && (int32_t)(millis() - drawAgain) >= 0) {
    drawAgain = millis() + (printing ? POWERDRAW_EVERY : POWERDRAW_IDLE);
    yandexHome.request(YandexHome::PowerDraw);
  }

  static uint32_t downSince;
  if (WiFi.status() == WL_CONNECTED)
    downSince = 0;
  else {
    if (!downSince)
      downSince = millis();
    else if (millis() - downSince > JOIN_WAIT_MS)
      ESP.restart();
  }
}
