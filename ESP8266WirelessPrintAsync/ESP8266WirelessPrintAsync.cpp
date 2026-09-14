// Required: https://github.com/greiman/SdFat

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <ArduinoJson.h>          // https://github.com/bblanchon/ArduinoJson (for implementing a subset of the OctoPrint API)
#include <DNSServer.h>
#include "StorageFS.h"
#include <ESPAsyncWebServer.h>    // https://github.com/me-no-dev/ESPAsyncWebServer
#include <ESPAsyncWiFiManager.h>  // https://github.com/alanswx/ESPAsyncWiFiManager/

#include "CommandQueue.h"
#include "YandexHome.h"

// Use Serial1 rather than the normal Serial0, which prints stuff during boot that confuses the printer
HardwareSerial PrinterSerial(1);

WiFiServer telnetServer(23);
WiFiClient serverClient;

AsyncWebServer server(80);
DNSServer dns;

// Configurable parameters
#define SKETCH_VERSION "2.x-localbuild" // Gets inserted at build time by the PlatformIO workflow
#define OTA_UPDATES                     // Enable OTA firmware updates, comment if you don't want it (OTA may lead to security issues because someone may load any code on device)
//#define OTA_PASSWORD ""               // Uncomment to protect OTA updates and assign a password (inside "")
#define MAX_SUPPORTED_EXTRUDERS 6       // Number of supported extruder
#define REPEAT_M115_TIMES 1             // M115 retries with same baud (MAX 255)

#define PRINTER_RX_BUFFER_SIZE 0        // This is printer firmware 'RX_BUFFER_SIZE'. If such parameter is unknown please use 0
#define TEMPERATURE_REPORT_INTERVAL 2   // Ask the printer for its temperatures status every 2 seconds
#define KEEPALIVE_INTERVAL 2500         // Marlin defaults to 2 seconds, get a little of margin
#define MAX_TIMEOUT_RETRIES 5           // Silent periods of KEEPALIVE_INTERVAL before a print is given up on
#define MAX_RESPONSE_LENGTH 4096        // An unrecognized response longer than this is discarded instead of growing the heap. M115 alone is over 1 KB
#define WIFI_PORTAL_TIMEOUT 180         // Seconds the configuration portal stays up before retrying the stored network
#define WIFI_RETRY_INTERVAL 30000       // Reconnection attempt interval while the network is down
#define WIFI_REBOOT_AFTER 600000        // Reboot after this long offline, so a changed network can be configured again
#define WDT_TIMEOUT 30                  // Seconds without a loop iteration before the watchdog reboots the device
#define MAX_LISTED_FILES 64             // Upper bound on the directory listing, so a full card cannot exhaust the heap
#define CANCEL_PARK "G1 X0 Y180 F6000"  // Where a cancelled print leaves the head, comment out to home only
#define POWEROFF_NOZZLE 100             // Nozzle has to be below this before the socket may be switched off
#define POWEROFF_BED 50                 // And the bed below this
#define POWEROFF_GRACE 30000            // Everything has to stay that way for this long
#define POWEROFF_RETRY 60000            // Retry interval when the request did not get through
#define POWEROFF_ATTEMPTS 10            // Attempts before giving up for this print
const uint32_t serialBauds[] = { 115200, 250000, 57600 };    // Marlin valid bauds (removed very low bauds; roughly ordered by popularity to speed things up)

#define API_VERSION     "0.1"
#define VERSION         "1.3.10"

// The sketch on the ESP
bool ESPrestartRequired;  // Set this flag in the callbacks to restart ESP

// Information from M115
String fwMachineType = "Unknown";
uint8_t fwExtruders = 1;
bool fwAutoreportTempCap, fwProgressCap, fwBuildPercentCap;

// Printer status
bool printerConnected,
     startPrint,
     isPrinting,
     printPause,
     restartPrint,
     cancelPrint,
     printerRestarted,
     autoreportTempEnabled;

uint32_t printStartTime;
float printCompletion;

// Serial communication
String lastCommandSent, lastReceivedResponse;
uint32_t lineNumber;
String lastSentLine;
bool swallowNextOk;
uint8_t timeoutRetries;
uint32_t wifiRetryTimer, wifiDownSince;

bool autoPowerOff, powerOffArmed;
uint8_t powerOffAttempts;
uint32_t powerOffReadySince, powerOffNoticeTimer, powerOffRetryAt, powerOffCheckTimer;

uint8_t serialBaudIndex;
uint16_t printerUsedBuffer;
uint32_t serialReceiveTimeoutTimer;

// Stored files
Preferences preferences;

String selectedFile;
size_t selectedFileSize, filePos;
uint32_t selectedFileDate;

String printingFile;
size_t printingFileSize;

String lastUploadFields, uploadFailure;

// Temperature for printer status reporting
#define TEMP_COMMAND      "M105"
#define AUTOTEMP_COMMAND  "M155 S"

struct Temperature {
  String actual, target;
};

uint32_t temperatureTimer;

Temperature toolTemperature[MAX_SUPPORTED_EXTRUDERS];
Temperature bedTemperature;


// https://forum.arduino.cc/index.php?topic=228884.msg2670971#msg2670971
inline String IpAddress2String(const IPAddress& ipAddress) {
  return String(ipAddress[0]) + "." +
         String(ipAddress[1]) + "." +
         String(ipAddress[2]) + "." +
         String(ipAddress[3]);
}

inline void telnetSend(const String line) {
  if (serverClient && serverClient.connected())     // send data to telnet client if connected
    serverClient.println(line);
}

bool isFloat(const String value) {
  for (int i = 0; i < value.length(); ++i) {
    char ch = value[i];
    if (ch != ' ' && ch != '.' && ch != '-' && !isDigit(ch))
      return false;
  }

  return true;
}

// Parse temperatures from printer responses like
// ok T:32.8 /0.0 B:31.8 /0.0 T0:32.8 /0.0 @:0 B@:0
bool parseTemp(const String response, const String whichTemp, Temperature *temperature) {
  int tpos = response.indexOf(whichTemp + ":");
  if (tpos != -1) { // This response contains a temperature
    int slashpos = response.indexOf(" /", tpos);
    int spacepos = response.indexOf(" ", slashpos + 1);
    // if match mask T:xxx.xx /xxx.xx
    if (slashpos != -1 && spacepos != -1) {
      String actual = response.substring(tpos + whichTemp.length() + 1, slashpos);
      String target = response.substring(slashpos + 2, spacepos);
      if (isFloat(actual) && isFloat(target)) {
        temperature->actual = actual;
        temperature->target = target;

        return true;
      }
    }
  }

  return false;
}

// Parse temperatures from prusa firmare (sent when heating)
// ok T:32.8 E:0 B:31.8
bool parsePrusaHeatingTemp(const String response, const String whichTemp, Temperature *temperature) {
  int tpos = response.indexOf(whichTemp + ":");
  if (tpos != -1) { // This response contains a temperature
    int spacepos = response.indexOf(" ", tpos);
    if (spacepos == -1)
      spacepos = response.length();
    String actual = response.substring(tpos + whichTemp.length() + 1, spacepos);
    if (isFloat(actual)) {
      temperature->actual = actual;

      return true;
    }
  }

  return false;
}

int8_t parsePrusaHeatingExtruder(const String response) {
  Temperature tmpTemperature;

  return parsePrusaHeatingTemp(response, "E", &tmpTemperature) ? tmpTemperature.actual.toInt() : -1;
}

bool parseTemperatures(const String response) {
  bool tempResponse;

  if (fwExtruders == 1)
    tempResponse = parseTemp(response, "T", &toolTemperature[0]);
  else {
    tempResponse = false;
    for (int t = 0; t < fwExtruders; t++)
      tempResponse |= parseTemp(response, "T" + String(t), &toolTemperature[t]);
  }
  tempResponse |= parseTemp(response, "B", &bedTemperature);
  if (!tempResponse) {
    // Parse Prusa heating temperatures
    int e = parsePrusaHeatingExtruder(response);
    tempResponse = e >= 0 && e < MAX_SUPPORTED_EXTRUDERS && parsePrusaHeatingTemp(response, "T", &toolTemperature[e]);
    tempResponse |= parsePrusaHeatingTemp(response, "B", &bedTemperature);
    }

  return tempResponse;
}

// Parse position responses from printer like
// X:-33.00 Y:-10.00 Z:5.00 E:37.95 Count X:-3300 Y:-1000 Z:2000
inline bool parsePosition(const String response) {
  return response.indexOf("X:") != -1 && response.indexOf("Y:") != -1 &&
         response.indexOf("Z:") != -1 && response.indexOf("E:") != -1;
}

inline void lcd(const String text) {
  commandQueue.push("M117 " + text);
}

inline void playSound() {
  commandQueue.push("M300 S500 P50");
}

inline String stringify(bool value) {
  return value ? "true" : "false";
}

inline String baseName(const String path) {
  return path.startsWith("/") ? path.substring(1) : path;
}

inline String jobFilename() {
  const String path = isPrinting ? printingFile : selectedFile;

  return path == "" ? "Unknown" : baseName(path);
}

inline size_t jobFileSize() {
  return isPrinting ? printingFileSize : selectedFileSize;
}

inline bool isGcodeFilename(const String name) {
  if (name.startsWith("."))
    return false;

  String lowercase = name;
  lowercase.toLowerCase();

  return lowercase.endsWith(".gcode") || lowercase.endsWith(".gco") || lowercase.endsWith(".g");
}

String sanitizeFilename(const String filename) {
  String name = filename;
  int pos = name.lastIndexOf('/');
  if (pos != -1)
    name = name.substring(pos + 1);
  pos = name.lastIndexOf('\\');
  if (pos != -1)
    name = name.substring(pos + 1);

  String clean;
  for (unsigned int i = 0; i < name.length(); ++i) {
    const char ch = name[i];
    const bool safe = ch >= 32 && ch < 127 && ch != '"' && ch != '*' && ch != ':' &&
                      ch != '<' && ch != '>' && ch != '?' && ch != '|';
    clean += safe ? ch : '_';
  }
  while (clean.startsWith("."))
    clean = clean.substring(1);

  const unsigned int limit = storageFS.getMaxPathLength() - 1;
  if (clean.length() > limit)
    clean = clean.substring(clean.length() - limit);

  return clean;
}

void selectFile(const String path) {
  selectedFile = path;
  selectedFileSize = 0;
  selectedFileDate = 0;

  if (path != "") {
    FileWrapper file = storageFS.open(path);
    if (file) {
      selectedFileSize = file.size();
      selectedFileDate = file.lastWrite();
      file.close();
    }
  }

  preferences.begin("wirelessprint", false);
  preferences.putString("selected", path);
  preferences.end();
}

void handlePrint() {
  static FileWrapper gcodeFile;
  static float prevM532Completion;

  if (isPrinting) {
    const bool abortPrint = (restartPrint || cancelPrint || printerRestarted);
    if (abortPrint || !gcodeFile.available()) {
      gcodeFile.close();
      if (fwProgressCap)
        commandQueue.push("M530 S0");
      if (!abortPrint)
        lcd("Complete");
      printPause = false;
      isPrinting = false;
      powerOffArmed = autoPowerOff;
      powerOffReadySince = 0;
      powerOffAttempts = 0;
    }
    else if (!printPause && commandQueue.getFreeSlots() > 4) {    // Keep some space for "service" commands
      String line = gcodeFile.readStringUntil('\n'); // The G-Code line being worked on
      filePos += line.length() + 1;
      int pos = line.indexOf(';');
      if (line.length() > 0 && pos != 0 && line[0] != '(' && line[0] != '\r') {
        if (pos != -1)
          line = line.substring(0, pos);
        commandQueue.push(line);
      }

      // Send to printer completion (if supported)
      printCompletion = printingFileSize > 0 ? min((float)filePos / printingFileSize * 100, 100.0f) : 0;
      if (fwProgressCap && printCompletion - prevM532Completion >= 0.1) {
        commandQueue.push("M532 X" + String((int)(printCompletion * 10) / 10.0));
        prevM532Completion = printCompletion;
      }
    }
  }

  if (!isPrinting && (startPrint || restartPrint)) {
    startPrint = restartPrint = false;

    filePos = 0;
    prevM532Completion = 0.0;

    gcodeFile = storageFS.open(selectedFile);
    if (!gcodeFile)
      lcd("Can't open file");
    else {
      printingFile = selectedFile;
      printingFileSize = selectedFileSize;
      lcd("Printing...");
      playSound();
      printStartTime = millis();
      isPrinting = true;
      if (fwProgressCap) {
        commandQueue.push("M530 S1 L0");
        commandQueue.push("M531 " + baseName(printingFile));
      }
    }
  }
}

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  static FileWrapper file;
  static String path;

  if (!index) {
    if (file) {
      file.close();
      if (path != "")
        storageFS.remove(path);
    }
    path = "";
    uploadFailure = "";

    const String name = sanitizeFilename(filename);
    if (!storageFS.isActive())
      uploadFailure = "no storage";
    else if (!isGcodeFilename(name))
      uploadFailure = "not a gcode file";
    else if (isPrinting && "/" + name == printingFile)
      uploadFailure = "that file is being printed";
    else if (request->contentLength() >= storageFS.freeBytes())
      uploadFailure = "not enough free space";
    else {
      file = storageFS.open("/" + name, "w");
      if (!file)
        uploadFailure = "cannot create file";
      else {
        path = "/" + name;
        lcd("Receiving...");
      }
    }
  }

  if (uploadFailure == "" && file && file.write(data, len) != len)
    uploadFailure = "write error, card full";

  if (final) {
    if (file)
      file.close();
    if (uploadFailure != "") {
      if (path != "")
        storageFS.remove(path);
    }
    else if (path != "")
      selectFile(path);
    path = "";
  }
}

bool paramIsTrue(AsyncWebServerRequest *request, const char *name) {
  AsyncWebParameter *param = NULL;
  if (request->hasParam(name, true))
    param = request->getParam(name, true);
  else if (request->hasParam(name))
    param = request->getParam(name);
  if (param == NULL)
    return false;

  const String value = param->value();

  return value != "false" && value != "0";
}

int apiJobHandler(JsonObject root) {
  const char* command = root["command"];
  if (command != NULL) {
    if (strcmp(command, "cancel") == 0) {
      if (!isPrinting)
        return 409;
      cancelPrint = true;
    }
    else if (strcmp(command, "start") == 0) {
      if (isPrinting || !printerConnected || selectedFile == "")
        return 409;
      startPrint = true;
    }
    else if (strcmp(command, "restart") == 0) {
      if (!printPause)
        return 409;
      restartPrint = true;
    }
    else if (strcmp(command, "pause") == 0) {
      if (!isPrinting)
        return 409;
      const char* action = root["action"];
      if (action == NULL)
        printPause = !printPause;
      else {
        if (strcmp(action, "pause") == 0)
          printPause = true;
        else if (strcmp(action, "resume") == 0)
          printPause = false;
        else if (strcmp(action, "toggle") == 0)
          printPause = !printPause;
      }
    }
  }

  return 204;
}

String M115ExtractString(const String response, const String field) {
  int spos = response.indexOf(field + ":");
  if (spos != -1) {
    spos += field.length() + 1;
    int epos = response.indexOf(':', spos);
    if (epos == -1)
      epos = response.indexOf('\n', spos);
    if (epos == -1)
      return response.substring(spos);
    else {
      while (epos >= spos && response[epos] != ' ' && response[epos] != '\n')
        --epos;
      return response.substring(spos, epos);
    }
  }

  return "";
}

bool M115ExtractBool(const String response, const String field, const bool onErrorValue = false) {
  const int pos = response.indexOf(field + ":");
  if (pos == -1)
    return onErrorValue;

  const unsigned int valuePos = (unsigned int)pos + field.length() + 1;

  return valuePos < response.length() ? response[valuePos] == '1' : onErrorValue;
}

inline String getDeviceId() {
  uint64_t chipid = ESP.getEfuseMac();

  return String((uint16_t)(chipid >> 32), HEX) + String((uint32_t)chipid, HEX);
}

inline String getDeviceName() {
  return fwMachineType + " (" + getDeviceId() + ")";
}

void mDNSInit() {
  #ifdef OTA_UPDATES
    MDNS.setInstanceName(getDeviceId().c_str());    // Can't call MDNS.init because it has been already done by 'ArduinoOTA.begin', here I just change instance name
  #else
    if (!MDNS.begin(getDeviceId().c_str()))
      return;
  #endif

  // For Cura WirelessPrint - deprecated in favor of the OctoPrint API
  MDNS.addService("wirelessprint", "tcp", 80);
  MDNS.addServiceTxt("wirelessprint", "tcp", "version", SKETCH_VERSION);

  // OctoPrint API
  // Unfortunately, Slic3r doesn't seem to recognize it
  MDNS.addService("octoprint", "tcp", 80);
  MDNS.addServiceTxt("octoprint", "tcp", "path", "/");
  MDNS.addServiceTxt("octoprint", "tcp", "api", API_VERSION);
  MDNS.addServiceTxt("octoprint", "tcp", "version", SKETCH_VERSION);

  MDNS.addService("http", "tcp", 80);
  MDNS.addServiceTxt("http", "tcp", "path", "/");
  MDNS.addServiceTxt("http", "tcp", "api", API_VERSION);
  MDNS.addServiceTxt("http", "tcp", "version", SKETCH_VERSION);
}

bool detectPrinter() {
  static int printerDetectionState;
  static byte nM115;

  switch (printerDetectionState) {
    case 0:
      // Start printer detection
      serialBaudIndex = 0;
      printerDetectionState = 10;
      break;

    case 10:
      // Initialize baud and send a request to printer
      PrinterSerial.begin(serialBauds[serialBaudIndex], SERIAL_8N1, 13, 12); // gpio13 = rx, gpio12 = tx (gpio14 taken by SD_MMC clock)
      telnetSend("Connecting at " + String(serialBauds[serialBaudIndex]));
      commandQueue.push("M110 N0"); // M110 - Reset line numbering before using checksums
      commandQueue.push("M115"); // M115 - Firmware Info
      printerDetectionState = 20;
      break;

    case 20:
      // Check if there is a printer response
      if (commandQueue.isEmpty()) {
        String value = M115ExtractString(lastReceivedResponse, "MACHINE_TYPE");
        if (value == "") {
          if (nM115++ >= REPEAT_M115_TIMES) {
            nM115 = 0;
            ++serialBaudIndex;
            if (serialBaudIndex < sizeof(serialBauds) / sizeof(serialBauds[0]))
              printerDetectionState = 10;
            else
              printerDetectionState = 0;   
          } 
          else
            printerDetectionState = 10;      
        }
        else {
          telnetSend("Connected");

          printerRestarted = false;
          fwMachineType = value;
          value = M115ExtractString(lastReceivedResponse, "EXTRUDER_COUNT");
          fwExtruders = value == "" ? 1 : min(value.toInt(), (long)MAX_SUPPORTED_EXTRUDERS);
          fwAutoreportTempCap = M115ExtractBool(lastReceivedResponse, "Cap:AUTOREPORT_TEMP");
          fwProgressCap = M115ExtractBool(lastReceivedResponse, "Cap:PROGRESS");
          fwBuildPercentCap = M115ExtractBool(lastReceivedResponse, "Cap:BUILD_PERCENT");

          mDNSInit();

          String text = IpAddress2String(WiFi.localIP()) + " " + storageFS.getActiveFS();
          lcd(text);
          playSound();

          if (fwAutoreportTempCap)
            commandQueue.push(AUTOTEMP_COMMAND + String(TEMPERATURE_REPORT_INTERVAL));   // Start auto report temperatures
          else
            temperatureTimer = millis();
          return true;
        }
      }
      break;
  }

  return false;
}

String uint64ToString(const uint64_t value) {
  char buffer[21];
  snprintf(buffer, sizeof(buffer), "%llu", value);

  return String(buffer);
}

String jsonEscape(const String text) {
  String escaped;
  for (unsigned int i = 0; i < text.length(); ++i) {
    const char ch = text[i];
    if (ch == '"' || ch == '\\')
      escaped += '\\';
    if (ch >= 32)
      escaped += ch;
  }

  return escaped;
}

String htmlEscape(const String text) {
  String escaped;
  for (unsigned int i = 0; i < text.length(); ++i) {
    const char ch = text[i];
    if (ch == '&')
      escaped += "&amp;";
    else if (ch == '<')
      escaped += "&lt;";
    else if (ch == '>')
      escaped += "&gt;";
    else if (ch == '"')
      escaped += "&quot;";
    else if (ch == '\'')
      escaped += "&#39;";
    else
      escaped += ch;
  }

  return escaped;
}

String urlEncode(const String text) {
  static const char hex[] = "0123456789ABCDEF";
  String encoded;

  for (unsigned int i = 0; i < text.length(); ++i) {
    const char ch = text[i];
    const bool unreserved = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                            (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-' || ch == '~';
    if (unreserved)
      encoded += ch;
    else {
      encoded += '%';
      encoded += hex[((uint8_t)ch >> 4) & 0x0F];
      encoded += hex[(uint8_t)ch & 0x0F];
    }
  }

  return encoded;
}

String firstStoredFile() {
  String found;

  FileWrapper dir = storageFS.open("/");
  if (dir) {
    FileWrapper file = dir.openNextFile();
    while (file) {
      const String name = baseName(file.name());
      if (!file.isDirectory() && isGcodeFilename(name)) {
        found = "/" + name;
        file.close();
        break;
      }
      file.close();
      file = dir.openNextFile();
    }
    dir.close();
  }

  return found;
}

void forEachStoredFile(std::function<void(const String, const uint32_t, const uint32_t)> visit) {
  FileWrapper dir = storageFS.open("/");
  if (!dir)
    return;

  unsigned int listed = 0;
  FileWrapper file = dir.openNextFile();
  while (file && listed < MAX_LISTED_FILES) {
    const String name = baseName(file.name());
    if (!file.isDirectory() && isGcodeFilename(name)) {
      visit(name, file.size(), file.lastWrite());
      ++listed;
    }
    file.close();
    file = dir.openNextFile();
  }
  if (file)
    file.close();
  dir.close();
}

String fileListJson() {
  String json = "{\r\n  \"files\": [";
  bool first = true;

  forEachStoredFile([&json, &first](const String name, const uint32_t size, const uint32_t date) {
    const String escaped = jsonEscape(name);
    if (!first)
      json += ",";
    first = false;
    json += "\r\n    {"
            "\"name\": \"" + escaped + "\", "
            "\"path\": \"" + escaped + "\", "
            "\"display\": \"" + escaped + "\", "
            "\"origin\": \"local\", "
            "\"size\": " + String(size) + ", "
            "\"date\": " + String(date) + ", "
            "\"type\": \"machinecode\", "
            "\"typePath\": [\"machinecode\", \"gcode\"], "
            "\"selected\": " + stringify("/" + name == selectedFile) + ", "
            "\"refs\": {\"download\": \"/download?name=" + urlEncode(name) + "\"}"
            "}";
  });

  json += "\r\n  ],\r\n"
          "  \"free\": " + uint64ToString(storageFS.freeBytes()) + ",\r\n"
          "  \"total\": " + uint64ToString(storageFS.totalBytes()) + "\r\n"
          "}";

  return json;
}

String fileListHtml() {
  String html;

  forEachStoredFile([&html](const String name, const uint32_t size, const uint32_t date) {
    const String shown = htmlEscape(name);
    const String encoded = urlEncode(name);
    const bool current = "/" + name == selectedFile;
    html += "<tr><td>" + String(current ? "&#9654; " : "") + shown + "</td>"
            "<td align=\"right\">" + String(size / 1024) + " KiB</td>"
            "<td><button onclick=\"post('/select?name=" + encoded + "')\">Select</button> "
            "<button onclick=\"post('/delete?name=" + encoded + "')\">Delete</button> "
            "<a href=\"/download?name=" + encoded + "\">Download</a></td></tr>";
  });

  return html == "" ? "<tr><td colspan=\"3\"><i>No files</i></td></tr>" : html;
}

void initSelectedFile() {
  preferences.begin("wirelessprint", true);
  const String stored = preferences.getString("selected", "");
  preferences.end();

  selectFile(storageFS.exists(stored) ? stored : firstStoredFile());
}

inline String getState() {
  if (!printerConnected)
    return "Discovering printer";
  else if (cancelPrint)
    return "Cancelling";
  else if (printPause)
    return "Paused";
  else if (isPrinting)
    return "Printing";
  else
    return "Operational";
}

void handleAutoPowerOff() {
  const uint32_t now = millis();
  if ((int32_t)(now - powerOffCheckTimer) < 0)
    return;
  powerOffCheckTimer = now + 1000;

  if (!powerOffArmed)
    return;

  if (isPrinting || !printerConnected) {
    powerOffReadySince = 0;
    return;
  }

  float target = bedTemperature.target.toFloat();
  float nozzle = 0;
  for (int t = 0; t < fwExtruders; ++t) {
    target = max(target, toolTemperature[t].target.toFloat());
    nozzle = max(nozzle, toolTemperature[t].actual.toFloat());
  }

  if (target > 0) {
    powerOffArmed = false;
    lcd("Power off cancelled");
    return;
  }

  if (nozzle >= POWEROFF_NOZZLE || bedTemperature.actual.toFloat() >= POWEROFF_BED) {
    powerOffReadySince = 0;
    return;
  }

  if (powerOffReadySince == 0) {
    powerOffReadySince = now;
    powerOffNoticeTimer = now;
    powerOffRetryAt = now;
    powerOffAttempts = 0;
  }

  const uint32_t waited = now - powerOffReadySince;
  if (waited < POWEROFF_GRACE) {
    if ((int32_t)(now - powerOffNoticeTimer) >= 0) {
      powerOffNoticeTimer = now + 15000;
      lcd("Power off in " + String((POWEROFF_GRACE - waited) / 1000) + "s");
    }
    return;
  }

  if (yandexHome.busy() || (int32_t)(now - powerOffRetryAt) < 0)
    return;

  if (powerOffAttempts >= POWEROFF_ATTEMPTS) {
    powerOffArmed = false;
    lcd("Power off failed");
    return;
  }

  ++powerOffAttempts;
  powerOffRetryAt = now + POWEROFF_RETRY;
  lcd("Powering off");
  yandexHome.request(YandexHome::PowerOff);
}

inline String powerOffState() {
  if (!autoPowerOff)
    return "disabled";
  if (!powerOffArmed)
    return "waiting for a print to finish";
  if (powerOffReadySince == 0)
    return "armed, printer still warm";
  const uint32_t waited = millis() - powerOffReadySince;

  return waited < POWEROFF_GRACE ? "switching off in " + String((POWEROFF_GRACE - waited) / 1000) + "s"
                                 : "switching off now, attempt " + String(powerOffAttempts);
}

void setup() {
  commandQueue.begin();
  storageFS.begin();
  yandexHome.begin();

  for (int t = 0; t < MAX_SUPPORTED_EXTRUDERS; t++)
    toolTemperature[t] = { "0.0", "0.0" };
  bedTemperature = { "0.0", "0.0" };

  // Wait for connection
  WiFi.mode(WIFI_STA);
  AsyncWiFiManager wifiManager(&server, &dns);
  // wifiManager.resetSettings();   // Uncomment this to reset the settings on the device, then you will need to reflash with USB and this commented out!
  wifiManager.setDebugOutput(false);  // So that it does not send stuff to the printer that the printer does not understand
  if (WiFi.SSID() != "")
    wifiManager.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT);
  wifiManager.autoConnect("AutoConnectAP");
  WiFi.setAutoReconnect(true);
  wifiDownSince = millis();

  telnetServer.begin();
  telnetServer.setNoDelay(true);

  initSelectedFile();

  preferences.begin("wirelessprint", true);
  autoPowerOff = preferences.getBool("autooff", false);
  preferences.end();

  server.onNotFound([](AsyncWebServerRequest * request) {
    telnetSend("404 | Page '" + request->url() + "' not found");
    request->send(404, "text/html; charset=utf-8", "<h1>Page not found!</h1>");
  });

  // Main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    String message = "<h1>" + getDeviceName() + "</h1>"
                     "<p>" + getState() + ". Selected: <b>" + htmlEscape(jobFilename()) + "</b></p>"
                     "<p><button onclick=\"job('start')\">Print selected</button> "
                     "<button onclick=\"job('cancel')\">Cancel print</button></p>"
                     "<h2>Files on " + storageFS.getActiveFS() + "</h2>"
                     "<table>" + fileListHtml() + "</table>"
                     "<p>" + uint64ToString(storageFS.freeBytes() / 1048576) + " MiB free of " +
                             uint64ToString(storageFS.totalBytes() / 1048576) + " MiB</p>"
                     "<h2>Upload</h2>"
                     "<form enctype=\"multipart/form-data\" action=\"/api/files/local\" method=\"POST\">\n"
                     "<input name=\"file\" type=\"file\" accept=\".gcode,.GCODE,.gco,.GCO\" required/><br/>\n"
                     "<input type=\"checkbox\" name=\"print\" id=\"printImmediately\" value=\"true\">\n"
                     "<label for=\"printImmediately\">Print immediately</label><br/>\n"
                     "<input type=\"submit\" value=\"Upload\"/>\n"
                     "</form>"
                     "<pre>curl -F \"file=@/path/to/some.gcode\" -F \"print=true\" " + IpAddress2String(WiFi.localIP()) + "/api/files/local</pre>\n"
                     "<p><a href=\"/info\">Info</a> | <a href=\"/yandex\">Yandex socket</a></p>"
                     "<hr>"
                     "<p>WirelessPrinting <a href=\"https://github.com/kelv1n9/WirelessPrinting/commit/" + SKETCH_VERSION + "\">" + SKETCH_VERSION + "</a></p>\n"
                    #ifdef OTA_UPDATES
                      "<p>OTA Update Device: <a href=\"/update\">Click Here</a></p>"
                    #endif
                     ;
    message += R"HTML(<script>
function post(url) { fetch(url, {method: 'POST'}).then(function(r) { if (!r.ok) alert('Failed: ' + r.status); location.reload(); }); }
function job(command) { fetch('/api/job', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({command: command})}).then(function(r) { if (!r.ok) alert('Failed: ' + r.status); location.reload(); }); }
</script>)HTML";
    request->send(200, "text/html; charset=utf-8", message);
  });

  server.on("/yandex", HTTP_GET, [](AsyncWebServerRequest * request) {
    String list;
    const String cached = yandexHome.getDevices();
    const String selected = yandexHome.getDeviceId();
    int start = 0;
    while (start < (int)cached.length()) {
      int end = cached.indexOf('\n', start);
      if (end == -1)
        end = cached.length();
      const String row = cached.substring(start, end);
      const int tab = row.indexOf('\t');
      if (tab != -1) {
        const String id = row.substring(0, tab);
        const String name = row.substring(tab + 1);
        list += "<tr><td>" + String(id == selected ? "&#9654; " : "") + htmlEscape(name) + "</td>"
                "<td><code>" + htmlEscape(id) + "</code></td>"
                "<td><button onclick=\"post('/yandex/select?id=" + urlEncode(id) + "&amp;name=" + urlEncode(name) + "')\">Use this one</button></td></tr>";
      }
      start = end + 1;
    }
    if (list == "")
      list = "<tr><td colspan=\"3\"><i>No sockets loaded yet</i></td></tr>";

    String message = "<h1>Yandex smart home</h1>"
                     "<p>Status: <b>" + htmlEscape(yandexHome.getStatus()) + "</b>"
                     + String(yandexHome.busy() ? " (working...)" : "") + "</p>"
                     "<p>Token: <b>" + String(yandexHome.hasToken() ? "stored" : "not set") + "</b>. "
                     "Socket: <b>" + String(yandexHome.getDeviceId() == "" ? "not selected" : htmlEscape(yandexHome.getDeviceName())) + "</b></p>"
                     "<form method=\"POST\" action=\"/yandex/token\">"
                     "<input name=\"token\" type=\"password\" size=\"60\" placeholder=\"OAuth token\" required/> "
                     "<input type=\"submit\" value=\"Store token\"/>"
                     "</form>"
                     "<p>The token is written to NVS and never shown again.</p>"
                     "<h2>Switch off after a print</h2>"
                     "<p>State: <b>" + powerOffState() + "</b></p>"
                     "<p>Waits until the print is over, both targets are zero, the nozzle is below "
                     + String(POWEROFF_NOZZLE) + " and the bed below " + String(POWEROFF_BED)
                     + ", then holds that for " + String(POWEROFF_GRACE / 1000) + " seconds.</p>"
                     "<p><button onclick=\"post('/yandex/auto?on=" + String(autoPowerOff ? "0" : "1") + "')\">"
                     + String(autoPowerOff ? "Turn automatic switch off OFF" : "Turn automatic switch off ON") + "</button> "
                     "<button onclick=\"post('/yandex/abort')\">Cancel the pending switch off</button></p>"
                     "<h2>Sockets</h2>"
                     "<p><button onclick=\"post('/yandex/devices')\">Load from Yandex</button> "
                     "<button onclick=\"post('/yandex/test')\">Test: switch on</button></p>"
                     "<table>" + list + "</table>"
                     "<p><a href=\"/\">Back</a></p>";
    message += R"HTML(<script>
function post(url) { fetch(url, {method: 'POST'}).then(function(r) { if (!r.ok) alert('Failed: ' + r.status); setTimeout(function(){ location.reload(); }, 2500); }); }
</script>)HTML";
    request->send(200, "text/html; charset=utf-8", message);
  });

  server.on("/yandex/token", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("token", true)) {
      request->send(400, "text/plain", "token is required");
      return;
    }
    yandexHome.setToken(request->getParam("token", true)->value());
    request->redirect("/yandex");
  });

  server.on("/yandex/auto", HTTP_POST, [](AsyncWebServerRequest * request) {
    autoPowerOff = request->hasParam("on") && request->getParam("on")->value() == "1";
    if (!autoPowerOff)
      powerOffArmed = false;

    preferences.begin("wirelessprint", false);
    preferences.putBool("autooff", autoPowerOff);
    preferences.end();

    request->send(204, "text/plain", "");
  });

  server.on("/yandex/abort", HTTP_POST, [](AsyncWebServerRequest * request) {
    powerOffArmed = false;
    powerOffReadySince = 0;
    request->send(204, "text/plain", "");
  });

  server.on("/yandex/devices", HTTP_POST, [](AsyncWebServerRequest * request) {
    request->send(yandexHome.request(YandexHome::DeviceList) ? 204 : 409, "text/plain", "");
  });

  server.on("/yandex/select", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("id")) {
      request->send(400, "text/plain", "id is required");
      return;
    }
    yandexHome.setDevice(request->getParam("id")->value(),
                         request->hasParam("name") ? request->getParam("name")->value() : "");
    request->send(204, "text/plain", "");
  });

  server.on("/yandex/test", HTTP_POST, [](AsyncWebServerRequest * request) {
    request->send(yandexHome.request(YandexHome::PowerOn) ? 204 : 409, "text/plain", "");
  });

  server.on("/select", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("name")) {
      request->send(400, "text/plain", "name is required");
      return;
    }
    const String name = sanitizeFilename(request->getParam("name")->value());
    const String path = "/" + name;
    if (!isGcodeFilename(name) || !storageFS.exists(path)) {
      request->send(404, "text/plain", "no such file");
      return;
    }
    selectFile(path);
    request->send(204, "text/plain", "");
  });

  server.on("/delete", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("name")) {
      request->send(400, "text/plain", "name is required");
      return;
    }
    const String path = "/" + sanitizeFilename(request->getParam("name")->value());
    if (!storageFS.exists(path)) {
      request->send(404, "text/plain", "no such file");
      return;
    }
    if (isPrinting && path == printingFile) {
      request->send(409, "text/plain", "that file is being printed");
      return;
    }
    storageFS.remove(path);
    if (path == selectedFile)
      selectFile(firstStoredFile());
    request->send(204, "text/plain", "");
  });

  // Info page
  server.on("/info", HTTP_GET, [](AsyncWebServerRequest * request) {
    String message = "<pre>"
                     "Free heap: " + String(ESP.getFreeHeap()) + "\n\n"
                     "File system: " + storageFS.getActiveFS() + "\n";
    if (storageFS.isActive()) {
      message += "Card free: " + uint64ToString(storageFS.freeBytes()) + " of " + uint64ToString(storageFS.totalBytes()) + "\n"
                 "Selected file: " + htmlEscape(baseName(selectedFile)) + "\n"
                 "Selected file size: " + String(selectedFileSize) + "\n";
      if (isPrinting)
        message += "Printing file: " + htmlEscape(baseName(printingFile)) + "\n";
    }
    message += "Last upload fields:" + htmlEscape(lastUploadFields) + "\n";
    if (uploadFailure != "")
      message += "Last upload failure: " + htmlEscape(uploadFailure) + "\n";
    message += "\n"
               "Last command sent: " + lastCommandSent + "\n"
               "Last received response: " + lastReceivedResponse + "\n";
    if (printerConnected) {
      message += "\n"
                 "EXTRUDER_COUNT: " + String(fwExtruders) + "\n"
                 "AUTOREPORT_TEMP: " + stringify(fwAutoreportTempCap);
      if (fwAutoreportTempCap)
        message += " Enabled: " + stringify(autoreportTempEnabled);
      message += "\n"
                 "PROGRESS: " + stringify(fwProgressCap) + "\n"
                 "BUILD_PERCENT: " + stringify(fwBuildPercentCap) + "\n";
    }
    message += "</pre>";
    request->send(200, "text/html; charset=utf-8", message);
  });

  #ifdef OTA_UPDATES
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest * request) {
      request->send(200, "text/html; charset=utf-8", "<h1>" + getDeviceName() + "</h1>"
                                      "<form method=\"POST\" action=\"/update\" enctype=\"multipart/form-data\">\n"
                                      "Firmware image: <input name=\"firmware\" type=\"file\" accept=\".bin\" required/><br/>\n"
                                      "<input type=\"submit\" value=\"Update\"/>\n"
                                      "</form>"
                                      "<p>The device reboots on success. Do not power it off during the upload.</p>");
    });

    server.on("/update", HTTP_POST, [](AsyncWebServerRequest * request) {
      const bool failed = Update.hasError() || Update.progress() == 0;
      AsyncWebServerResponse *response = request->beginResponse(failed ? 500 : 200, "text/plain",
                                                                failed ? String(Update.errorString()) : String("OK, rebooting"));
      response->addHeader("Connection", "close");
      request->send(response);
      ESPrestartRequired = !failed;
    },
    [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!index) {
        if (Update.isRunning())
          Update.abort();
        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
          return;
        lcd("Updating...");
      }

      if (!Update.isRunning())
        return;

      if (Update.write(data, len) != len)
        Update.abort();
      else if (final)
        Update.end(true);
    });
  #endif

  // Download page
  server.on("/download", HTTP_GET, [](AsyncWebServerRequest * request) {
    String path = selectedFile;
    if (request->hasParam("name"))
      path = "/" + sanitizeFilename(request->getParam("name")->value());

    FileWrapper probe = storageFS.open(path);
    if (!probe) {
      request->send(404, "text/plain", "no such file");
      return;
    }
    const size_t fileSize = probe.size();
    probe.close();

    AsyncWebServerResponse *response = request->beginResponse("application/x-gcode", fileSize, [path, fileSize](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      static size_t downloadBytesLeft;
      static FileWrapper downloadFile;

      if (!index) {
        downloadFile = storageFS.open(path);
        downloadBytesLeft = fileSize;
      }
      size_t bytes = min(downloadBytesLeft, maxLen);
      bytes = min(bytes, (size_t)2048);
      bytes = downloadFile.read(buffer, bytes);
      downloadBytesLeft -= bytes;
      if (bytes <= 0)
        downloadFile.close();

      return bytes;
    });
    response->addHeader("Content-Disposition", "attachment; filename=\"" + baseName(path) + "\"");
    request->send(response);
  });

  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest * request) {
    // https://docs.octoprint.org/en/master/api/general.html#post--api-login
    // https://github.com/fieldOfView/Cura-OctoPrintPlugin/issues/155#issuecomment-596109663
    request->send(200, "application/json", "{}");  });

  server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest * request) {
    // http://docs.octoprint.org/en/master/api/version.html
    request->send(200, "application/json", "{\r\n"
                                           "  \"api\": \"" API_VERSION "\",\r\n"
                                           "  \"server\": \"" VERSION "\"\r\n"
                                           "}");  });

  server.on("/api/connection", HTTP_GET, [](AsyncWebServerRequest * request) {
    // http://docs.octoprint.org/en/master/api/connection.html#get-connection-settings
    request->send(200, "application/json", "{\r\n"
                                           "  \"current\": {\r\n"
                                           "    \"state\": \"" + getState() + "\",\r\n"
                                           "    \"port\": \"Serial\",\r\n"
                                           "    \"baudrate\": " + serialBauds[serialBaudIndex] + ",\r\n"
                                           "    \"printerProfile\": \"Default\"\r\n"
                                           "  },\r\n"
                                           "  \"options\": {\r\n"
                                           "    \"ports\": \"Serial\",\r\n"
                                           "    \"baudrate\": " + serialBauds[serialBaudIndex] + ",\r\n"
                                           "    \"printerProfiles\": \"Default\",\r\n"
                                           "    \"portPreference\": \"Serial\",\r\n"
                                           "    \"baudratePreference\": " + serialBauds[serialBaudIndex] + ",\r\n"
                                           "    \"printerProfilePreference\": \"Default\",\r\n"
                                           "    \"autoconnect\": true\r\n"
                                           "  }\r\n"
                                           "}");
  });

  // Todo: http://docs.octoprint.org/en/master/api/connection.html#post--api-connection

  // File Operations
  // http://docs.octoprint.org/en/master/api/files.html#retrieve-all-files
  server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(200, "application/json", fileListJson());
  });

  // For Slic3r OctoPrint compatibility
  server.on("/api/files/local", HTTP_POST, [](AsyncWebServerRequest * request) {
    // https://docs.octoprint.org/en/master/api/files.html?highlight=api%2Ffiles%2Flocal#upload-file-or-create-folder
    lastUploadFields = "";
    for (int i = 0; i < request->params(); ++i) {
      AsyncWebParameter *param = request->getParam(i);
      lastUploadFields += " " + param->name() + "=" + param->value();
    }

    if (uploadFailure != "") {
      lcd("Upload failed");
      request->send(500, "application/json", "{\"error\": \"" + jsonEscape(uploadFailure) + "\"}");
      return;
    }

    lcd("Received");
    playSound();

    if (paramIsTrue(request, "print")) {
      if (!printerConnected || isPrinting) {
        request->send(409, "application/json", "{\"error\": \"printer is busy\"}");
        return;
      }
      startPrint = true;
    }

    // OctoPrint sends 201 here; https://github.com/fieldOfView/Cura-OctoPrintPlugin/issues/155#issuecomment-596110996
    request->send(201, "application/json", "{\r\n"
                                           "  \"files\": {\r\n"
                                           "    \"local\": {\r\n"
                                           "      \"name\": \"" + jsonEscape(baseName(selectedFile)) + "\",\r\n"
                                           "      \"origin\": \"local\"\r\n"
                                           "    }\r\n"
                                           "  },\r\n"
                                           "  \"done\": true\r\n"
                                           "}");
  }, handleUpload);

  server.on("/api/job", HTTP_GET, [](AsyncWebServerRequest * request) {
    // http://docs.octoprint.org/en/master/api/job.html#retrieve-information-about-the-current-job
    int32_t printTime = 0, printTimeLeft = 0;
    if (isPrinting) {
      printTime = (millis() - printStartTime) / 1000;
      printTimeLeft = (printCompletion > 0) ? printTime / printCompletion * (100 - printCompletion) : INT32_MAX;
    }
    request->send(200, "application/json", "{\r\n"
                                           "  \"job\": {\r\n"
                                           "    \"file\": {\r\n"
                                           "      \"name\": \"" + jsonEscape(jobFilename()) + "\",\r\n"
                                           "      \"origin\": \"local\",\r\n"
                                           "      \"size\": " + String(jobFileSize()) + ",\r\n"
                                           "      \"date\": " + String(selectedFileDate) + "\r\n"
                                           "    },\r\n"
                                           //"    \"estimatedPrintTime\": \"" + estimatedPrintTime + "\",\r\n"
                                           "    \"filament\": {\r\n"
                                           //"      \"length\": \"" + filementLength + "\",\r\n"
                                           //"      \"volume\": \"" + filementVolume + "\"\r\n"
                                           "    }\r\n"
                                           "  },\r\n"
                                           "  \"progress\": {\r\n"
                                           "    \"completion\": " + String(printCompletion) + ",\r\n"
                                           "    \"filepos\": " + String(filePos) + ",\r\n"
                                           "    \"printTime\": " + String(printTime) + ",\r\n"
                                           "    \"printTimeLeft\": " + String(printTimeLeft) + "\r\n"
                                           "  },\r\n"
                                           "  \"state\": \"" + getState() + "\"\r\n"
                                           "}");
  });

  server.on("/api/job", HTTP_POST, [](AsyncWebServerRequest *request) {
    // Job commands http://docs.octoprint.org/en/master/api/job.html#issue-a-job-command
    request->send(200, "text/plain", "");
    },
    [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
      request->send(400, "text/plain", "file not supported");
    },
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      static String content;

      if (!index)
        content = "";
      for (int i = 0; i < len; ++i)
        content += (char)data[i];
      if (content.length() >= total) {
        DynamicJsonDocument doc(1024);
        auto error = deserializeJson(doc, content);
        if (error)
          request->send(400, "text/plain", error.c_str());
        else {
          int responseCode = apiJobHandler(doc.as<JsonObject>());
          request->send(responseCode, "text/plain", "");
          content = "";
        }
      }
  });
  
  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest * request) {
    // https://github.com/probonopd/WirelessPrinting/issues/30
    // https://github.com/probonopd/WirelessPrinting/issues/18#issuecomment-321927016
    request->send(200, "application/json", "{}");
  });

  server.on("/api/printer", HTTP_GET, [](AsyncWebServerRequest * request) {
    // https://docs.octoprint.org/en/master/api/printer.html#retrieve-the-current-printer-state
    String readyState = stringify(printerConnected);
    String message = "{\r\n"
                     "  \"state\": {\r\n"
                     "    \"text\": \"" + getState() + "\",\r\n"
                     "    \"flags\": {\r\n"
                     "      \"operational\": " + readyState + ",\r\n"
                     "      \"paused\": " + stringify(printPause) + ",\r\n"
                     "      \"printing\": " + stringify(isPrinting) + ",\r\n"
                     "      \"pausing\": false,\r\n"
                     "      \"cancelling\": " + stringify(cancelPrint) + ",\r\n"
                     "      \"sdReady\": false,\r\n"
                     "      \"error\": false,\r\n"
                     "      \"ready\": " + readyState + ",\r\n"
                     "      \"closedOrError\": " + stringify(!printerConnected) + "\r\n"
                     "    }\r\n"
                     "  },\r\n"
                     "  \"temperature\": {\r\n";
    for (int t = 0; t < fwExtruders; ++t) {
      message += "    \"tool" + String(t) + "\": {\r\n"
                 "      \"actual\": " + toolTemperature[t].actual + ",\r\n"
                 "      \"target\": " + toolTemperature[t].target + ",\r\n"
                 "      \"offset\": 0\r\n"
                 "    },\r\n";
    }
    message += "    \"bed\": {\r\n"
               "      \"actual\": " + bedTemperature.actual + ",\r\n"
               "      \"target\": " + bedTemperature.target + ",\r\n"
               "      \"offset\": 0\r\n"
               "    }\r\n"
               "  },\r\n"
               "  \"sd\": {\r\n"
               "    \"ready\": false\r\n"
               "  }\r\n"
               "}";
    request->send(200, "application/json", message);
  });

  server.on("/api/printer/command", HTTP_POST, [](AsyncWebServerRequest *request) {
    // http://docs.octoprint.org/en/master/api/printer.html#send-an-arbitrary-command-to-the-printer
    request->send(200, "text/plain", "");
    },
    [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
      request->send(400, "text/plain", "file not supported");
    },
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      static String content;

      if (!index)
        content = "";
      for (size_t i = 0; i < len; ++i)
        content += (char)data[i];
      if (content.length() >= total) {
        DynamicJsonDocument doc(1024);
        auto error = deserializeJson(doc, content);
        if (error)
          request->send(400, "text/plain", error.c_str());
        else {
          JsonObject root = doc.as<JsonObject>();
          const char* command = root["command"];
          if (command != NULL)
            commandQueue.push(command);
          else {
            JsonArray commands = root["commands"].as<JsonArray>();
            for (JsonVariant command : commands)
              commandQueue.push(String(command.as<String>()));
            }
          request->send(204, "text/plain", "");
        }
        content = "";
      }
  });

  // For legacy PrusaControlWireless - deprecated in favor of the OctoPrint API
  server.on("/print", HTTP_POST, [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", "Received");
  }, handleUpload);

  // For legacy Cura WirelessPrint - deprecated in favor of the OctoPrint API
  server.on("/api/print", HTTP_POST, [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", "Received");
  }, handleUpload);

  server.begin();

  #ifdef OTA_UPDATES
    // OTA setup
    ArduinoOTA.setHostname(getDeviceId().c_str());
    #ifdef OTA_PASSWORD
      ArduinoOTA.setPassword(OTA_PASSWORD);
    #endif
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      esp_task_wdt_reset();
    });
    ArduinoOTA.begin();
  #endif

  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);
}

inline void restartSerialTimeout() {
  serialReceiveTimeoutTimer = millis() + KEEPALIVE_INTERVAL;
}

void transmitCommand(const String command, const uint32_t number) {
  String line = "N" + String(number) + " " + command;
  uint8_t checksum = 0;
  for (unsigned int i = 0; i < line.length(); ++i)
    checksum ^= (uint8_t)line[i];
  line += "*" + String(checksum);

  PrinterSerial.println(line);              // Send to 3D Printer
  lastSentLine = line;
  lineNumber = command.startsWith("M110") ? 0 : number;

  telnetSend(">" + line);
}

int32_t parseResendNumber(const String response, const int from) {
  unsigned int i = from;
  while (i < response.length() && !isDigit(response[i]))
    ++i;
  if (i >= response.length())
    return -1;

  int32_t value = 0;
  while (i < response.length() && isDigit(response[i]))
    value = value * 10 + (response[i++] - '0');

  return value;
}

void SendCommands() {
  String command = commandQueue.peekSend();  //gets the next command to be sent
  if (command != "") {
    bool noResponsePending = commandQueue.isAckEmpty();
    if (noResponsePending || printerUsedBuffer < PRINTER_RX_BUFFER_SIZE * 3 / 4) {  // Let's use no more than 75% of printer RX buffer
      if (noResponsePending)
        restartSerialTimeout();   // Receive timeout has to be reset only when sending a command and no pending response is expected

      transmitCommand(command, command.startsWith("M110") ? 0 : lineNumber + 1);
      printerUsedBuffer += lastSentLine.length();
      lastCommandSent = command;
      commandQueue.popSend();
    }
  }
}

void ReceiveResponses() {
  static int lineStartPos;
  static String serialResponse;

  while (PrinterSerial.available()) {
    char ch = (char)PrinterSerial.read();
    if (ch != '\n') {
      serialResponse += ch;
      if (serialResponse.length() > MAX_RESPONSE_LENGTH) {
        serialResponse = "";
        lineStartPos = 0;
        telnetSend("#OVERFLOW#");
      }
    }
    else {
      bool incompleteResponse = false;
      String responseDetail = "";

      if (serialResponse.startsWith("Resend:", lineStartPos) || serialResponse.startsWith("rs ", lineStartPos)) {
        const int32_t requested = parseResendNumber(serialResponse, lineStartPos);
        if (requested >= 0 && (uint32_t)requested < lineNumber)
          printerRestarted = true;
        if (!printerRestarted && !commandQueue.isAckEmpty() && lastCommandSent != "")   // Only one command is ever unacknowledged, so it is the one being asked for
          transmitCommand(lastCommandSent, requested < 0 ? lineNumber : (uint32_t)requested);
        swallowNextOk = true;
        responseDetail = "resend";
      }
      else if (serialResponse.startsWith("start", lineStartPos)) {
        printerRestarted = true;
        responseDetail = "printer restarted";
      }
      else if (swallowNextOk && serialResponse.startsWith("ok", lineStartPos)) {
        swallowNextOk = false;                 // This ok belongs to the corrupted line, the re-sent one is still pending
        responseDetail = "resend ok";
      }
      else if (serialResponse.startsWith("ok", lineStartPos)) {
        if (lastCommandSent.startsWith(TEMP_COMMAND))
          parseTemperatures(serialResponse);
        else if (fwAutoreportTempCap && lastCommandSent.startsWith(AUTOTEMP_COMMAND))
          autoreportTempEnabled = (lastCommandSent[6] != '0');

        unsigned int cmdLen = commandQueue.popAcknowledge().length();     // Go on with next command
        printerUsedBuffer = max(printerUsedBuffer - cmdLen, 0u);
        timeoutRetries = 0;
        responseDetail = "ok";
      }
      else if (printerConnected) {
        if (parseTemperatures(serialResponse))
          responseDetail = "autotemp";
        else if (parsePosition(serialResponse))
          responseDetail = "position";
        else if (serialResponse.startsWith("echo:busy"))
          responseDetail = "busy";
        else if (serialResponse.startsWith("echo: cold extrusion prevented")) {
          // To do: Pause sending gcode, or do something similar
          responseDetail = "cold extrusion";
        }
        else if (serialResponse.startsWith("Error:")) {
          if (serialResponse.indexOf("Last Line") == -1) {   // Every Marlin transmission error ends with 'Last Line: N' and is followed by a Resend
            cancelPrint = true;
            responseDetail = "ERROR";
          }
          else
            responseDetail = "resend error";
        }
        else {
          incompleteResponse = true;
          responseDetail = "wait more";
        }
      } else {
          incompleteResponse = true;
          responseDetail = "discovering";
      }

      int responseLength = serialResponse.length();
      telnetSend("<" + serialResponse.substring(lineStartPos, responseLength) + "#" + responseDetail + "#");
      if (incompleteResponse)
        lineStartPos = responseLength;
      else {
        lastReceivedResponse = serialResponse;
        lineStartPos = 0;
        serialResponse = "";
      }
      restartSerialTimeout();
    }
  }

  if (!commandQueue.isAckEmpty() && (signed)(serialReceiveTimeoutTimer - millis()) <= 0) {  // Command has been lost by printer, buffer has been freed
    if (!printerConnected)
      commandQueue.clear();
    else {
      telnetSend("#TIMEOUT#");
      if (lastCommandSent != "" && ++timeoutRetries <= MAX_TIMEOUT_RETRIES)
        transmitCommand(lastCommandSent, lineNumber);
      else {
        timeoutRetries = 0;
        commandQueue.clear();
        printerUsedBuffer = 0;
        if (isPrinting) {
          cancelPrint = true;
          lcd("Printer not responding");
        }
      }
    }
    lineStartPos = 0;
    serialResponse = "";
    restartSerialTimeout();
  }
}

void loop() {
  #ifdef OTA_UPDATES
    //****************
    //* OTA handling *
    //****************
    if (ESPrestartRequired) {  // check the flag here to determine if a restart is required
      ESPrestartRequired = false;
      delay(500);
      ESP.restart();
    }

    ArduinoOTA.handle();
  #endif

  if (WiFi.status() != WL_CONNECTED) {
    if ((signed)(wifiRetryTimer - millis()) <= 0) {
      wifiRetryTimer = millis() + WIFI_RETRY_INTERVAL;
      WiFi.disconnect();
      WiFi.begin();
    }
    if (!isPrinting && (signed)(millis() - wifiDownSince) >= WIFI_REBOOT_AFTER)
      ESP.restart();
  }
  else
    wifiDownSince = millis();

  //********************
  //* Printer handling *
  //********************
  if (!printerConnected)
    printerConnected = detectPrinter();
  else {
    #ifndef OTA_UPDATES
      MDNS.update();    // When OTA is active it's called by 'handle' method
    #endif

    handlePrint();
    handleAutoPowerOff();

    if (printerRestarted && !isPrinting) {
      printerRestarted = false;
      cancelPrint = false;
      commandQueue.clear();
      printerUsedBuffer = 0;
      lineNumber = 0;
      timeoutRetries = 0;
      swallowNextOk = false;
      lastCommandSent = "";
      commandQueue.push("M110 N0");
      lcd("Printer restarted");
    }
    else if (cancelPrint && !isPrinting) { // Only when cancelPrint has been processed by 'handlePrint'
      cancelPrint = false;
      swallowNextOk = !commandQueue.isAckEmpty();
      commandQueue.clear();
      printerUsedBuffer = 0;
      lcd("Print cancelled");
      commandQueue.push("G91");
      commandQueue.push("G1 E-3 F300");
      commandQueue.push("G1 Z10 F600");
      commandQueue.push("G90");
      commandQueue.push("M104 S0");
      commandQueue.push("M140 S0");
      commandQueue.push("M107");
      #ifdef CANCEL_PARK
        commandQueue.push(CANCEL_PARK);
      #endif
      commandQueue.push("M84");
      playSound();
    }

    if (!autoreportTempEnabled) {
      unsigned long curMillis = millis();
      if ((signed)(temperatureTimer - curMillis) <= 0) {
        commandQueue.push(TEMP_COMMAND);
        temperatureTimer = curMillis + TEMPERATURE_REPORT_INTERVAL * 1000;
      }
    }
  }

  SendCommands();
  ReceiveResponses();

  //*******************
  //* Telnet handling *
  //*******************
  // look for Client connect trial
  if (telnetServer.hasClient()) {   // A new client always takes over, a half-open socket would otherwise block telnet forever
    if (serverClient)
      serverClient.stop();

    serverClient = telnetServer.available();
    serverClient.flush();  // clear input buffer, else you get strange characters
  }

  static String telnetCommand;
  while (serverClient && serverClient.available()) {  // get data from Client
    {
    char ch = serverClient.read();
    if (ch == '\r' || ch == '\n') {
      if (telnetCommand.length() > 0) {
        commandQueue.push(telnetCommand);
        telnetCommand = "";
      }
    }
    else
      telnetCommand += ch;
    }
  }

  esp_task_wdt_reset();
}
