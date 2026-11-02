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

static SerialIo serialIo;
static CardBytes sendBytes;
static BinaryTransfer sender;
static char sendName[BINARY_NAME_BYTES];
static bool sendAsked;
static bool cardUp;
static String sendTrouble;

// Writes one line and gathers whatever comes back, up to the first ok or the wait.
static String say(const String line) {
  while (Printer.available())
    Printer.read();

  Printer.println(line);

  String out;
  const uint32_t until = millis() + REPLY_WAIT_MS;
  while ((int32_t)(millis() - until) < 0) {
    while (Printer.available() && out.length() < 3000)
      out += (char)Printer.read();
    if (out.endsWith("ok\n") || out.endsWith("ok\r\n"))
      break;
    delay(5);
  }
  return out;
}

// Whatever the printer has said on its own since this was last asked.
static String heard() {
  String out;
  while (Printer.available() && out.length() < 3000)
    out += (char)Printer.read();
  return out;
}

static const char *phaseName(const BinaryPhase p) {
  switch (p) {
    case BinaryIdle:      return "idle";
    case BinarySwitching: return "switching";
    case BinarySyncing:   return "syncing";
    case BinaryOpening:   return "opening";
    case BinaryWriting:   return "writing";
    case BinaryShutting:  return "closing";
    case BinaryAborting:  return "aborting";
    case BinaryDone:      return "done";
    case BinaryFailed:    return "failed";
  }
  return "?";
}

static WebServer server(80);
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

  server.on("/", HTTP_GET, []() { server.send(200, "text/html", page()); });
  server.on("/api/status", HTTP_GET, []() {
    server.send(200, "application/json",
      "{\"bare\":true,\"printing\":false,\"slot\":\"" + slot() +
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
    const String cmd = server.arg("cmd");
    if (cmd == "") {
      server.send(400, "text/plain", "cmd is empty\n");
      return;
    }

    while (Printer.available())
      Printer.read();

    Printer.print(cmd);
    Printer.print('\n');

    String out;
    const uint32_t until = millis() + REPLY_WAIT_MS;
    while ((int32_t)(millis() - until) < 0) {
      while (Printer.available() && out.length() < 3000)
        out += (char)Printer.read();
      if (out.endsWith("ok\n") || out.endsWith("ok\r\n"))
        break;
      delay(5);
    }

    server.send(200, "text/plain", out == "" ? "the printer said nothing\n" : out);
  });

  // Whatever has arrived on its own, for catching a printer as it starts up.
  server.on("/printer/heard", HTTP_GET, []() {
    String out;
    while (Printer.available() && out.length() < 3000)
      out += (char)Printer.read();
    server.send(200, "text/plain", out);
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
  }, []() {
    static File into;
    static uint8_t lined[2048] __attribute__((aligned(32)));   // SD_MMC moves this over DMA
    HTTPUpload &up = server.upload();

    if (up.status == UPLOAD_FILE_START) {
      sendTrouble = "";
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
  });

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

  server.on("/printer/send", HTTP_POST, []() {
    if (sender.running()) {
      server.send(409, "text/plain", "a transfer is already running\n");
      return;
    }
    const String name = server.arg("name");
    if (name == "") {
      server.send(400, "text/plain", "name is empty\n");
      return;
    }
    sendBytes.shut();
    if (!sendBytes.open(("/" + name).c_str())) {
      server.send(404, "text/plain", "no such file on the module's card\n");
      return;
    }
    strncpy(sendName, name.c_str(), sizeof(sendName) - 1);
    sendName[sizeof(sendName) - 1] = 0;
    sendAsked = true;
    server.send(200, "text/plain", "taken, watch /printer/send/status\n");
  });

  // The printer prints from its own card. Nothing here feeds it gcode, which is the
  // whole point: the module can reboot, lose the network or be reflashed mid print and
  // the printer will not notice. M21 is sent every time because a binary transfer leaves
  // the card unmounted behind it.
  server.on("/printer/print", HTTP_POST, []() {
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
    server.send(200, "text/plain", say("M524"));
  });

  server.on("/printer/progress", HTTP_GET, []() {
    server.send(200, "text/plain", heard());
  });

  // Both ends have to move together. M575 is not saved, so a power cycle undoes a mistake.
  server.on("/printer/baud", HTTP_POST, []() {
    const uint32_t rate = server.arg("rate").toInt();
    if (rate < 9600) {
      server.send(400, "text/plain", "rate looks wrong\n");
      return;
    }

    Printer.print("M575 B");
    Printer.println(rate);
    Printer.flush();
    delay(200);
    Printer.updateBaudRate(rate);
    delay(200);

    while (Printer.available())
      Printer.read();
    Printer.println("M115");

    String out;
    const uint32_t until = millis() + REPLY_WAIT_MS;
    while ((int32_t)(millis() - until) < 0) {
      while (Printer.available() && out.length() < 1500)
        out += (char)Printer.read();
      if (out.endsWith("ok\n") || out.endsWith("ok\r\n") || out.indexOf("\nok") >= 0)
        break;
      delay(5);
    }

    server.send(200, "text/plain", "now at " + String(rate) + ", printer says:\n" + out);
  });

  server.on("/printer/send/status", HTTP_GET, []() {
    server.send(200, "text/plain",
      String("phase    ") + phaseName(sender.phase) + "\n" +
      "name     " + sendName + "\n" +
      "written  " + String(sender.written) + " of " + String(sender.total) + "\n" +
      "packets  " + String(sender.packets) + ", resends " + String(sender.resends) + "\n" +
      "payload  " + String(sender.theirPayload) + ", their version " + sender.theirVersion + "\n" +
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
    sender.start(&sendBytes, sendName, millis());
  }
  if (sender.running()) {
    sender.service(millis());
    if (!sender.running())
      sendBytes.shut();
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
