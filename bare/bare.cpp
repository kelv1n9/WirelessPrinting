#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

#define JOIN_WAIT_MS   90000
#define PORTAL_NAME    "KP3S-setup"

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
      ",\"heap\":" + String(ESP.getFreeHeap()) + "}");
  });

  server.on("/update", HTTP_POST, []() {
    const bool good = !Update.hasError();
    server.sendHeader("Connection", "close");
    server.send(good ? 200 : 500, "text/plain", good ? "OK, rebooting\n" : "failed\n");
    if (good) {
      delay(300);
      ESP.restart();
    }
  }, onUpload);

  server.begin();
}

void loop() {
  server.handleClient();

  if (portal)
    return;

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
