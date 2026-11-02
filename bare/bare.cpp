#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <SD_MMC.h>

#include "../ESP8266WirelessPrintAsync/BinaryTransfer.h"
#include "../ESP8266WirelessPrintAsync/BootGuard.h"
#include "page.h"
#include "printercard.h"
#include "gcodemeta.h"
#include "../ESP8266WirelessPrintAsync/YandexHome.h"

// The encrypted connection is the only one, but YandexHome asks for the gate the same
// way it did when the bot shared it, so the gate exists and is never contended.
SemaphoreHandle_t tlsGate;
volatile bool tlsWanted;

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

#define JOIN_WAIT_MS   90000
#define PORTAL_NAME    "KP3S-setup"
#define PRINTER_RX     13      // gpio14 is taken by the SD_MMC clock
#define PRINTER_TX     16      // not gpio12, a strapping pin that stops the board booting
#define PRINTER_BAUD   115200  // What Marlin boots at here; M575 moves it afterwards
#define REPLY_WAIT_MS  4000

static HardwareSerial Printer(1);

// The wire, as BinaryTransfer wants to see it.
struct SerialIo : LinkIo {
  int read() { return Printer.available() ? Printer.read() : -1; }
  size_t write(const uint8_t *data, size_t len) { return Printer.write(data, len); }
  size_t writable() { return Printer.availableForWrite(); }
  uint32_t now() { return millis(); }
};

// The file, likewise. It lives on the module's card, so its size is not the heap's business.
struct CardBytes : ByteSource {
  File file;
  uint32_t held = 0;

  bool open(const char *path) {
    file = SD_MMC.open(path);
    held = file ? file.size() : 0;
    return (bool)file && held > 0;
  }
  void shut() { if (file) file.close(); }

  int next(uint8_t *out, size_t max) {
    if (!file)
      return -1;
    const int took = file.read(out, max);
    return took < 0 ? -1 : took;
  }
  uint32_t size() const { return held; }
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

static SerialIo serialIo;
static CardBytes sendBytes;
static BinaryTransfer sender;
static char sendName[BINARY_NAME_BYTES];
static bool sendAsked;
static bool cardUp;
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

static PrinterCard printerCard;
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
  if (printerCard.observe(text))
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
  }
  if (!strncmp(text, "File opened: ", 13)) {
    const char *end = strstr(text + 13, " Size:");
    const size_t room = end ? (size_t)(end - (text + 13)) : strlen(text + 13);
    snprintf(selected, sizeof selected, "%.*s", (int)room, text + 13);
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
  Printer.println(line);

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

struct MetaOut : GcodeMetaSink {
  File into;
  File picture;
  String tmp, keepAs;

  void layer(const GcodeMetaLayer &one) override {
    if (into)
      into.write((const uint8_t *)&one, sizeof one);
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
  File one = SD_MMC.open(metaPath(target, ".hdr"));
  if (!one)
    return false;
  const bool good = one.read((uint8_t *)&head, sizeof head) == (int)sizeof head &&
                    head.magic == GCODEMETA_MAGIC;
  one.close();
  return good;
}

// The table is on the card, not in memory: a tall print has thousands of rows and the
// answer needs a dozen of them. Seeking beats reading the lot.
static uint32_t metaLayerAt(const char *target, const uint32_t count, const uint32_t byte,
                            uint16_t *z100, uint16_t *minutes) {
  File table = SD_MMC.open(metaPath(target, ".idx"));
  if (!table)
    return 0;
  uint32_t low = 0, high = count;
  GcodeMetaLayer one;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    table.seek(mid * sizeof one);
    if (table.read((uint8_t *)&one, sizeof one) != (int)sizeof one)
      break;
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
  }
  table.close();
  return low;
}

static WebServer server(80);

// One body for both upload routes: the module's own and the one a slicer speaks.
static void takeUpload() {
  static File into;
  static uint8_t lined[2048] __attribute__((aligned(32)));   // SD_MMC moves this over DMA
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    sendTrouble = "";
    lastUpload = up.filename;
    if (!cardUp) {
      sendTrouble = "the module has no card";
      return;
    }
    into = SD_MMC.open("/" + up.filename, FILE_WRITE);
    if (!into) {
      sendTrouble = "the module's card would not take the file";
      return;
    }

    // Read once, while it goes past. Reading it again afterwards would be a second
    // pass over megabytes for something that is right here.
    metaWanted = printerCardIsGcode(up.filename.c_str()) &&
                 printerCardTarget(up.filename.c_str(), metaTarget, sizeof metaTarget);
    if (metaWanted) {
      SD_MMC.mkdir(META_DIR);
      metaOut.into = SD_MMC.open(metaPath(metaTarget, ".idx"), FILE_WRITE);
      metaOut.tmp = metaPath(metaTarget, ".new");
      metaOut.keepAs = metaPath(metaTarget, ".png");
      metaScan.begin(&metaOut);
    }
  }
  else if (up.status == UPLOAD_FILE_WRITE && into) {
    for (size_t at = 0; at < up.currentSize; at += sizeof(lined)) {
      const size_t take = up.currentSize - at < sizeof(lined) ? up.currentSize - at : sizeof(lined);
      memcpy(lined, up.buf + at, take);
      if (into.write(lined, take) != take) {
        sendTrouble = "the module's card stopped taking bytes";
        break;
      }
      if (metaWanted)
        metaScan.feed(lined, take);
    }
  }
  else if (up.status == UPLOAD_FILE_END && into) {
    const uint32_t size = into.size();
    into.close();
    if (metaWanted) {
      metaScan.finish(size);
      if (metaOut.into)
        metaOut.into.close();
      File head = SD_MMC.open(metaPath(metaTarget, ".hdr"), FILE_WRITE);
      if (head) {
        head.write((const uint8_t *)&metaScan.head, sizeof metaScan.head);
        head.close();
      }
      metaWanted = false;
    }
  }
}

// Takes a file on the module's card all the way to the printer, and onto it if asked.
// Says no rather than half doing it, with the reason left in choreTrouble.
static bool startChore(const String &name, const bool andPrint) {
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
  chore = ChoreSending;
  sendAsked = true;
  return true;
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

static bool settleWire() {
  for (int tries = 0; tries < 6; tries++) {
    say("M105");
    if (sawOk)
      return true;
    delay(100);
  }
  return false;
}

static bool askPrinterCard() {
  if (sender.running())
    return false;
  if (!settleWire())
    return false;
  say("M21");
  printerCard.begin();
  say("M20 L");
  return printerCard.complete();
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
    case BinaryAborting:  return "aborting";
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

static String page() {
  return "<!doctype html><meta charset=utf-8><title>KP3S</title>"
         "<style>body{font:16px system-ui;margin:2rem;max-width:34rem}"
         "input,button{font:inherit;padding:.4rem}</style>"
         "<h1>KP3S module</h1>"
         "<p>Bare firmware. Slot " + slot() +
         ", ip " + WiFi.localIP().toString() +
         ", rssi " + String(WiFi.RSSI()) +
         ", up " + String(millis() / 1000) + "s"
         ", heap " + String(ESP.getFreeHeap()) + "</p>"
         "<h2>Flash</h2>"
         "<form method=POST action=/update enctype=multipart/form-data>"
         "<input type=file name=firmware required><button>Send</button></form>";
}

static void onUpload() {
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
      Update.printError(Serial);
  }
  else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize)
      Update.printError(Serial);
  }
  else if (up.status == UPLOAD_FILE_END) {
    if (!Update.end(true))
      Update.printError(Serial);
  }
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

  MDNS.begin("kp3s");
  MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE_HTML); });
  // Everything the page draws, in the shape it has always expected. What this build
  // cannot know is said as nothing rather than guessed: the page hides those rows.
  server.on("/api/status", HTTP_GET, []() {
    const bool heard = heardTemp && millis() - heardTemp < 20000;
    const uint32_t ran = printing && printSince ? (millis() - printSince) / 1000 : 0;
    uint64_t freeCard = 0;
    if (cardUp)
      freeCard = SD_MMC.totalBytes() - SD_MMC.usedBytes();

    String j = "{\"bare\":true";
    j += ",\"printing\":" + String(printing ? "true" : "false");
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
    j += ",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -100);
    j += ",\"version\":\"" + slot() + " up " + String(millis() / 1000) + "s\"}";

    // What the slicer wrote down, looked up by the byte the printer says it is on
    GcodeMetaHead meta;
    uint32_t layer = 0, layers = 0;
    uint16_t z100 = 0, left = 0;
    double grams = 0;
    int32_t remaining = -1;
    if (selected[0] && cardUp && metaHead(selected, meta)) {
      layers = meta.found ? meta.found : meta.layers;
      grams = meta.grams;
      if (printing && printOf) {
        layer = metaLayerAt(selected, meta.found, printAt, &z100, &left);
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
    j += ",\"elapsed\":" + String(ran) + ",\"remaining\":" + String(remaining) + ",\"fan\":-1}";

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
    j += ",\"link\":{\"mangled\":0}";
    j += "}";
    server.send(200, "application/json", j);
  });

  Printer.begin(PRINTER_BAUD, SERIAL_8N1, PRINTER_RX, PRINTER_TX);
  cardUp = SD_MMC.begin("/sdcard", true);          // 1 bit mode, which leaves gpio13 for the printer

  // One line in, whatever the printer says back out. Enough to ask a printer what it is.
  server.on("/gcode", HTTP_GET, []() {
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

  server.on("/upload", HTTP_POST, []() {
    if (sendTrouble != "") {
      const String why = sendTrouble;
      sendTrouble = "";
      server.send(400, "text/plain", why + "\n");
      return;
    }
    server.send(200, "text/plain", "stored\n");
  }, takeUpload);

  // What a slicer speaks. Only the two calls Orca actually makes are here.
  server.on("/api/version", HTTP_GET, []() {
    server.send(200, "application/json", "{\"api\":\"0.1\",\"server\":\"1.3.10\",\"text\":\"KP3S module\"}");
  });

  server.on("/api/files/local", HTTP_POST, []() {
    if (sendTrouble != "") {
      const String why = sendTrouble;
      sendTrouble = "";
      server.send(500, "application/json", "{\"error\":\"" + why + "\"}");
      return;
    }
    const String wanted = server.arg("print");
    const bool andPrint = wanted == "true" || wanted == "1";
    String said = "stored";
    if (andPrint && !startChore(lastUpload, true))
      said = choreTrouble.length() ? choreTrouble : "the printer is busy";
    server.send(201, "application/json",
      "{\"done\":true,\"name\":\"" + lastUpload + "\",\"said\":\"" + said + "\"}");
  }, takeUpload);


  server.on("/files", HTTP_GET, []() {
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

  server.on("/download", HTTP_GET, []() {
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
  server.on("/api/files", HTTP_GET, []() {
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

  // Choosing a file is remembered here and acted on when the print is asked for: the
  // printer cannot hold a selection for a file that is not on its card yet.
  server.on("/select", HTTP_POST, []() {
    snprintf(selected, sizeof selected, "%s", server.arg("name").c_str());
    server.send(200, "text/plain", "selected\n");
  });

  server.on("/delete", HTTP_POST, []() {
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

  server.on("/api/rename", HTTP_POST, []() {
    if (!cardUp) {
      server.send(503, "text/plain", "the module has no card\n");
      return;
    }
    if (!SD_MMC.rename("/" + server.arg("from"), "/" + server.arg("to"))) {
      server.send(400, "text/plain", "the card would not rename it\n");
      return;
    }
    server.send(200, "text/plain", "renamed\n");
  });

  // start takes the selected file the whole way, since it may not be over there yet
  server.on("/api/job", HTTP_POST, []() {
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
    if (!startChore(String(selected), true)) {
      server.send(409, "text/plain", choreTrouble + "\n");
      return;
    }
    server.send(200, "text/plain", "taken as " + String(choreTarget) + "\n");
  });

  server.on("/api/command", HTTP_POST, []() {
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
  server.on("/api/console", HTTP_GET, []() {
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
  server.on("/thumbnail", HTTP_GET, []() {
    if (!cardUp) {
      server.send(503, "text/plain", "the module has no card\n");
      return;
    }
    const String asked = server.arg("name");
    char target[PRINTER_CARD_NAME_BYTES];
    String path = metaPath(asked.c_str(), ".png");
    if (!SD_MMC.exists(path) && printerCardTarget(asked.c_str(), target, sizeof target))
      path = metaPath(target, ".png");

    File one = SD_MMC.open(path);
    if (!one || one.isDirectory() || !one.size()) {
      if (one) one.close();
      server.send(404, "text/plain", "no picture for that one\n");
      return;
    }
    server.sendHeader("Cache-Control", "public, max-age=604800, immutable");
    server.streamFile(one, "image/png");
    one.close();
  });

  // The socket. The token goes in here and never comes back out: this server has no
  // password, and a token that can be read is a token that is gone.
  server.on("/yandex", HTTP_GET, []() {
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

  server.on("/yandex/token", HTTP_POST, []() {
    const String given = server.arg("token");
    if (given.length() < 20) {
      server.send(400, "text/plain", "that is too short to be a token\n");
      return;
    }
    yandexHome.setToken(given);         // straight into NVS, never logged and never shown
    server.sendHeader("Location", "/yandex");
    server.send(303, "text/plain", "saved\n");
  });

  server.on("/yandex/forget", HTTP_POST, []() {
    yandexHome.forgetToken();
    server.sendHeader("Location", "/yandex");
    server.send(303, "text/plain", "forgotten\n");
  });

  server.on("/yandex/devices", HTTP_POST, []() {
    yandexHome.request(YandexHome::DeviceList);
    delay(1200);                        // long enough that the page usually shows the answer
    server.sendHeader("Location", "/yandex");
    server.send(303, "text/plain", "asking\n");
  });

  server.on("/yandex/device", HTTP_POST, []() {
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

  server.on("/yandex/auto", HTTP_POST, []() {
    autoPowerOff = server.arg("on") == "1";
    if (!autoPowerOff)
      powerArmed = false;
    Preferences settings;
    settings.begin("yandex", false);
    settings.putBool("autooff", autoPowerOff);
    settings.end();
    server.send(200, "text/plain", autoPowerOff ? "will switch off after a print\n" : "will stay on\n");
  });

  server.on("/yandex/power", HTTP_POST, []() {
    if (!yandexHome.request(YandexHome::PowerDraw)) {
      server.send(409, "text/plain", "the socket is busy\n");
      return;
    }
    server.send(200, "text/plain", "asking\n");
  });

  server.on("/yandex/off", HTTP_POST, []() {
    if (!yandexHome.request(YandexHome::PowerOff)) {
      server.send(409, "text/plain", "the socket is busy\n");
      return;
    }
    server.send(200, "text/plain", "switching off\n");
  });

  server.on("/yandex/on", HTTP_POST, []() {
    if (!yandexHome.request(YandexHome::PowerOn)) {
      server.send(409, "text/plain", "the socket is busy\n");
      return;
    }
    server.send(200, "text/plain", "switching on\n");
  });

  server.on("/restart", HTTP_POST, []() {
    if (printing) {
      server.send(409, "text/plain", "the printer is printing\n");
      return;
    }
    server.send(200, "text/plain", "restarting\n");
    delay(200);
    ESP.restart();
  });

  server.on("/printer/files", HTTP_GET, []() {
    if (sender.running()) {
      server.send(409, "text/plain", "a transfer owns the wire\n");
      return;
    }
    // M20 waits behind every move already queued, so during a print the answer
    // would arrive long after the wait and read as an empty card.
    if (printing) {
      server.send(409, "text/plain", "the printer is printing, a listing would not come back\n");
      return;
    }
    if (!askPrinterCard()) {
      server.send(502, "text/plain", "the printer did not finish a listing\n");
      return;
    }
    String out;
    for (uint8_t i = 0; i < printerCard.count(); i++) {
      const PrinterCardEntry &e = printerCard.at(i);
      out += String(e.dos) + "  " + String(e.size) + "  " + (e.name[0] ? e.name : e.dos) + "\n";
    }
    if (!printerCard.count())
      out = "nothing on the printer's card\n";

    // What is on our card and not on theirs, worked out afresh rather than remembered
    if (cardUp) {
      PrinterCardPick pick;
      pick.begin();
      File dir = SD_MMC.open("/");
      for (File one = dir.openNextFile(); one; one = dir.openNextFile()) {
        if (!one.isDirectory()) {
          const char *bare = one.name();
          if (*bare == '/')             // the core hands roots back with a slash on the front
            bare++;
          pick.consider(printerCard, bare, one.size());
        }
        one.close();
      }
      dir.close();
      out += "\nwaiting  " + String(pick.waiting) + "\n";
      if (pick.found)
        out += "next     " + String(pick.source) + " as " + String(pick.target) + "\n";
    }
    server.send(200, "text/plain", out);
  });

  server.on("/printer/send", HTTP_POST, []() {
    if (!startChore(server.arg("name"), server.arg("print") == "1")) {
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
  server.on("/printer/print", HTTP_POST, []() {
    if (sender.running()) {
      server.send(409, "text/plain", "a transfer owns the wire\n");
      return;
    }
    const String name = server.arg("name");
    if (name == "") {
      server.send(400, "text/plain", "name is empty\n");
      return;
    }
    say("M21");
    const String opened = say("M23 " + name);
    if (opened.indexOf("File opened") < 0) {
      server.send(404, "text/plain", "the printer would not open it:\n" + opened);
      return;
    }
    say("M27 S3");            // It reports on its own from idle(), so a full queue cannot delay it
    say("M24");
    server.send(200, "text/plain", "printing " + name + "\n");
  });

  server.on("/printer/cancel", HTTP_POST, []() {
    if (sender.running()) {
      server.send(409, "text/plain", "a transfer owns the wire\n");
      return;
    }
    server.send(200, "text/plain", say("M524"));
  });

  server.on("/printer/progress", HTTP_GET, []() {
    String out;
    if (printOf)
      out = "SD printing byte " + String(printAt) + "/" + String(printOf) + "\n";
    if (!printing)
      out += "Not SD printing\n";
    saidLog[saidHeld] = 0;
    out += saidLog;
    saidHeld = 0;                // Lines are handed over once; the figures above are not
    server.send(200, "text/plain", out);
  });

  // Both ends have to move together. M575 is not saved, so a power cycle undoes a mistake.
  server.on("/printer/baud", HTTP_POST, []() {
    if (sender.running()) {
      server.send(409, "text/plain", "a transfer owns the wire\n");
      return;
    }
    const uint32_t rate = server.arg("rate").toInt();
    if (rate < 9600) {
      server.send(400, "text/plain", "rate looks wrong\n");
      return;
    }

    Printer.print("M575 B");     // No reply to wait for: the printer moves as the line ends
    Printer.println(rate);
    Printer.flush();
    delay(200);
    Printer.updateBaudRate(rate);
    delay(200);
    drain();                     // Whatever the change garbled is not an answer to anything

    server.send(200, "text/plain", "now at " + String(rate) + ", printer says:\n" + say("M115"));
  });

  server.on("/printer/send/status", HTTP_GET, []() {
    server.send(200, "text/plain",
      String("phase    ") + phaseName(sender.phase) + "\n" +
      "name     " + sendName + "\n" +
      "written  " + String(sender.written) + " of " + String(sender.total) + "\n" +
      "packets  " + String(sender.packets) + ", resends " + String(sender.resends) + "\n" +
      "payload  " + String(sender.theirPayload) + ", their version " + sender.theirVersion + "\n" +
      "as       " + String(choreTarget) + ", chore " + choreName() +
        (chore == ChoreFailed && choreTrouble.length() ? ", " + choreTrouble : "") + "\n" +
      "packing  " + String(sender.packing ? "on" : "off") +
        ", printer says " + (sender.theirCompression[0] ? sender.theirCompression : "nothing yet") +
        ", on the wire " + String(sender.sent) + "\n" +
      "took     " + String(sender.finishedAt > sender.startedAt
                           ? sender.finishedAt - sender.startedAt : 0) + "ms\n" +
      "trouble  " + String(sender.trouble ? sender.trouble : "none") + "\n");
  });

  server.on("/update", HTTP_POST, []() {
    const bool good = !Update.hasError();
    server.sendHeader("Connection", "close");
    server.send(good ? 200 : 500, "text/plain", good ? "OK, rebooting\n" : "failed\n");
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
  yandexHome.begin();

  server.begin();
}

void loop() {
  server.handleClient();
  bootGuardWatch(WiFi.status() == WL_CONNECTED);

  if (portal)
    return;

  if (sendAsked) {
    sendAsked = false;
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
    if (sender.phase != BinaryDone) {
      choreTrouble = sender.trouble ? sender.trouble : "the transfer did not finish";
      chore = ChoreFailed;
    }
    else if (!askPrinterCard()) {
      choreTrouble = "the printer did not answer with a listing";
      chore = ChoreFailed;
    }
    else if (!printerCard.holds(choreTarget, was)) {
      choreTrouble = "the file is not on the printer's card at the right size";
      chore = ChoreFailed;
    }
    else if (!choreShouldPrint)
      chore = ChoreDone;
    else {
      const String opened = say("M23 " + String(choreTarget));
      if (opened.indexOf("File opened") < 0) {
        choreTrouble = "the printer would not open it";
        chore = ChoreFailed;
      }
      else {
        say("M27 S3");
        say("M24");
        chore = ChoreDone;
      }
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
      Printer.println("M155 S2");
    }
    else {
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
