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
}

static PrinterCard printerCard;
static bool sawOk;               // a whole line of it, not the tail of "echo:SD card ok"

static void sawLine(const char *text, const size_t len) {
  if (!strcmp(text, "ok") || !strncmp(text, "ok ", 3)) {
    sawOk = true;
    return;
  }
  if (printerCard.observe(text))
    return;                      // a listing line belongs to the index and nowhere else
  if (!strncmp(text, "SD printing byte ", 17)) {
    const char *slash = strchr(text + 17, '/');
    if (slash) {
      printing = true;
      printAt = strtoul(text + 17, NULL, 10);
      printOf = strtoul(slash + 1, NULL, 10);
    }
    return;                      // The bulk of the traffic, and the state above says it better
  }
  if (!strcmp(text, "Not SD printing")) {
    printing = false;
    return;
  }
  if (!strncmp(text, "Done printing file", 18)) {
    printing = false;
    printAt = printOf;
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
    if (!into)
      sendTrouble = "the module's card would not take the file";
  }
  else if (up.status == UPLOAD_FILE_WRITE && into) {
    for (size_t at = 0; at < up.currentSize; at += sizeof(lined)) {
      const size_t take = up.currentSize - at < sizeof(lined) ? up.currentSize - at : sizeof(lined);
      memcpy(lined, up.buf + at, take);
      if (into.write(lined, take) != take) {
        sendTrouble = "the module's card stopped taking bytes";
        break;
      }
    }
  }
  else if (up.status == UPLOAD_FILE_END && into)
    into.close();
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
  server.on("/api/status", HTTP_GET, []() {
    server.send(200, "application/json",
      "{\"bare\":true,\"printing\":" + String(printing ? "true" : "false") +
      ",\"at\":" + String(printAt) + ",\"of\":" + String(printOf) +
      ",\"slot\":\"" + slot() +
      "\",\"uptime\":" + String(millis() / 1000) +
      ",\"heap\":" + String(ESP.getFreeHeap()) +
      ",\"block\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) +
      ",\"card\":" + String(cardUp ? "true" : "false") +
      ",\"guard\":\"" + bootGuardReport() + "\"}");
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

  server.on("/printer/files", HTTP_GET, []() {
    if (sender.running()) {
      server.send(409, "text/plain", "a transfer owns the wire\n");
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
        (choreTrouble.length() ? ", " + choreTrouble : "") + "\n" +
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
