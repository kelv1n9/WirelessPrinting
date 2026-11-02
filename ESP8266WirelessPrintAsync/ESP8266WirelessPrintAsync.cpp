// Required: https://github.com/greiman/SdFat

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <esp_wifi.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_spi_flash.h>
#include "TlsArena.h"
#include "PanicWitness.h"
#include "BootGuard.h"
#include "PrinterLink.h"
#include "CardTransfer.h"
#include "BinaryTransfer.h"
#include <rom/spi_flash.h>
#include <Preferences.h>
#include <time.h>
#include <mbedtls/base64.h>
#include <ArduinoJson.h>          // https://github.com/bblanchon/ArduinoJson (for implementing a subset of the OctoPrint API)
#include <DNSServer.h>
#include "StorageFS.h"
#include "GcodeReader.h"
#include "FileFeeder.h"
#include <memory>
#include <ESPAsyncWebServer.h>    // https://github.com/me-no-dev/ESPAsyncWebServer
#include <ESPAsyncWiFiManager.h>  // https://github.com/alanswx/ESPAsyncWiFiManager/

#include "WebAuth.h"
#include "CommandQueue.h"
#include "YandexHome.h"
#include "TelegramBot.h"

// Use Serial1 rather than the normal Serial0, which prints stuff during boot that confuses the printer
HardwareSerial PrinterSerial(1);

WiFiServer telnetServer(23);
WiFiClient serverClient;

AsyncWebServer server(80);
DNSServer dns;

// Configurable parameters
#ifndef SKETCH_VERSION
  #define SKETCH_VERSION "unknown"   // Set from the git revision at build time, see build_version.py
#endif
#define OTA_UPDATES                     // Enable OTA firmware updates, comment if you don't want it (OTA may lead to security issues because someone may load any code on device)
//#define OTA_PASSWORD ""               // Uncomment to protect OTA updates and assign a password (inside "")
#define MAX_SUPPORTED_EXTRUDERS 6       // Number of supported extruder
#define REPEAT_M115_TIMES 1             // M115 retries with same baud (MAX 255)

#define PRINTER_RX_BUFFER_SIZE 0        // This is printer firmware 'RX_BUFFER_SIZE'. If such parameter is unknown please use 0
#define TEMPERATURE_REPORT_INTERVAL 2   // Ask the printer for its temperatures status every 2 seconds
#define MAX_TIMEOUT_RETRIES 12          // Unanswered windows before a print is given up on
#define MAX_RESEND_RETRIES 10           // Times the same line is re-sent before the print is given up on
#define MAX_COMMAND_ROOM 320            // Longest line this ever sends, checksum included
#define MAX_RESPONSE_LENGTH 4096        // An unrecognized response longer than this is discarded instead of growing the heap. M115 alone is over 1 KB
#define WIFI_PORTAL_TIMEOUT 180         // Seconds the configuration portal stays up before retrying the stored network
#define WIFI_RETRY_INTERVAL 30000       // Reconnection attempt interval while the network is down
#define WIFI_REBOOT_AFTER 600000        // Reboot after this long offline, so a changed network can be configured again
#define TRANSFER_YIELD 3                // Ticks the loop steps aside for while a file is arriving
#define WDT_TIMEOUT 30                  // Seconds without a loop iteration before the watchdog reboots the device
#define MAX_LISTED_FILES 64             // Upper bound on the directory listing, so a full card cannot exhaust the heap
#define THUMBNAIL_SCAN_BYTES 65536      // Slicers put the preview at the very top, so looking further only costs time
#define THUMBNAIL_MAX_BYTES 49152       // A hundred pixel preview is two kilobytes; anything near the heap size is a trap
// Where a cancelled print leaves the head. It has to be a homing move, not a move to a
// coordinate: a printer that has not been homed since it was switched on does not know
// where it is, so an absolute move drives the head into the ends and grinds there. Z is
// left alone on purpose, because homing it would push the nozzle into whatever is still
// standing on the bed.
#define CANCEL_PARK "G28 X Y"
#define POWEROFF_NOZZLE 100             // Nozzle has to be below this before the socket may be switched off
#define POWEROFF_BED 50                 // And the bed below this
#define POWEROFF_GRACE 30000            // Everything has to stay that way for this long
#define POWEROFF_RETRY 60000            // Retry interval when the request did not get through
#define ENERGY_POLL 60000               // How often the socket is asked what the printer is drawing
#define ENERGY_TARIFF 0.077             // AZN per kWh, the first band for households, overridable
#define FILAMENT_PRICE 40               // AZN per kg, only used when the file does not say
#define FILAMENT_SCAN_BYTES 65536       // The slicer appends its summary after the last move
#define LOG_PATH "/log/events.log"      // What happened, so a failure at night leaves something to read
#define LOG_PREVIOUS "/log/events.1.log"
#define LOG_MAX_BYTES 262144            // Rolled over at this size, so two files are the whole cost
#define LOG_TAIL_BYTES 4096             // How much of it a read hands back: the server copies this again
#define LOG_BUFFER_BYTES 1024           // Held in memory between writes to keep the card off the hot path
#define DOWNLOAD_CACHE_BYTES 4096       // Bigger reads faster, but every byte here is a byte TLS cannot have
#define BENCH_BURST_BYTES 65536      // Only the card bench uses this, and only while it runs
#define UPLOAD_BUFFER_BYTES 8192        // Gathered before touching the card. The card writes at 2.8 MB/s
                                       // and the link delivers 220 KB/s, so a larger one buys nothing and
                                       // every byte of it is a byte the encryption cannot have.
#define UPLOAD_ABANDONED_AFTER 10000  // A transfer that has gone quiet this long has lost its client
#define SERIAL_LINES_PER_LOOP 40        // Replies handled per pass, so a shouting printer cannot starve the loop
#define LOG_FLUSH_INTERVAL 5000
#define LOG_LINK_INTERVAL 60000         // A link summary while printing, and only when something changed
#define LOG_SAMPLES_PER_PRINT 5         // Damaged lines written out in full, enough for a sample
#define HEAP_BLOCK_WARNING 15000        // The encryption has its own reserved pools now, so only a real squeeze matters
#define DEAD_MAN_SECONDS 120            // Marlin shuts itself down if this module stops talking for this long
#define TELNET_STALL_MS 200             // A write that takes longer than this is going nowhere
#define REMOUNT_INTERVAL 30000          // How often to try the card again once it has gone
#define CONSOLE_LINES 60                // What the page can scroll back through
#define PRINTER_SERIAL_RX_BUFFER 2048   // M503 answers faster than the loop can read one byte at a time
#define MDNS_HOSTNAME "kp3s"            // http://kp3s.local, rather than the chip id nobody can type
#define NOZZLE_CEILING 275
#define BED_CEILING 120
#define LOCAL_TIMEZONE "<+04>-4"        // Baku, four hours ahead and no daylight saving since 2016
#define PRINTER_RX_PIN 13               // gpio14 is taken by the SD_MMC clock
#define PRINTER_TX_PIN 16               // not gpio12: it is a strapping pin and a pulled up line stops the board booting
#define LINE_PROBE_MS 3000              // Long enough to catch a printer that only speaks now and then
#define BOOT_PROBE_MS 6000              // The printer greets whoever is listening, but only once, at power on
#define UPDATE_ABANDONED_AFTER 30000    // An upload that stops halfway must not leave the printer muted
#define POWEROFF_ATTEMPTS 10            // Attempts before giving up for this print
const uint32_t serialBauds[] = { 115200, 250000, 57600 };    // Marlin valid bauds (removed very low bauds; roughly ordered by popularity to speed things up)

#define API_VERSION     "0.1"
#define VERSION         "1.3.10"

// The sketch on the ESP
bool ESPrestartRequired;  // Set this flag in the callbacks to restart ESP

// Information from M115
String fwMachineType = "Unknown";   // Written by the loop on M115, read by the pages and the bot
uint8_t fwExtruders = 1;
bool fwAutoreportTempCap, fwProgressCap, fwBuildPercentCap, fwAutoreportSDCap;
bool externalPrinting;           // The printer is running a file from its own card, not from us

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

// Telemetry gathered while streaming the file
uint16_t currentLayer, totalLayers;
float currentZ;
int32_t slicerMinutesLeft = -1, slicerPercent = -1;
uint32_t linesMangled;
int16_t fanSpeed;
uint32_t linesSent, linesResent;

// Serial communication
// The loop rewrites these on every line the printer sends, and the /info page reads them
// from the server's task. Same reason as the file paths above: room for the longest line
// each can hold, so an assignment never frees the buffer a reader is standing on.
String lastCommandSent, lastReceivedResponse;
bool printEndedEarly;
uint32_t wifiRetryTimer, wifiDownSince;

bool autoPowerOff, powerOffArmed, powerOffSawIdle, updateSucceeded;
uint8_t powerOffAttempts;
uint32_t powerOffReadySince, powerOffNoticeTimer, powerOffRetryAt, powerOffCheckTimer;
float energyWh, energyLastWatt = -1, energyTariff = ENERGY_TARIFF;
float filamentGrams = -1, filamentPrice = FILAMENT_PRICE, filamentDefaultPrice = FILAMENT_PRICE;
uint32_t energyLastAt, energyPollAt;
String logBuffer, consoleRing[CONSOLE_LINES];
// Widening the firmware slots means rewriting the partition table at 0x8000, and the
// table lives outside every partition: the normal flash calls refuse that address and
// this core is built to abort on it, so the ROM routines are used with the cache off.
// Taken from the build's own partitions.bin, which is why the two cannot disagree.
extern "C" {
  void spi_flash_disable_interrupts_caches_and_other_cpu(void);
  void spi_flash_enable_interrupts_caches_and_other_cpu(void);
}

#define PARTITION_TABLE_AT 0x8000
#define PARTITION_TABLE_USED 256        // Wide enough to cover the old table as well as the new

static const uint8_t wantedPartitionTable[PARTITION_TABLE_USED] = {
  0xAA, 0x50, 0x01, 0x02, 0x00, 0x90, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00,
  0x6E, 0x76, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAA, 0x50, 0x01, 0x00,
  0x00, 0xE0, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x6F, 0x74, 0x61, 0x64,
  0x61, 0x74, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0xAA, 0x50, 0x00, 0x10, 0x00, 0x00, 0x01, 0x00,
  0x00, 0x00, 0x1F, 0x00, 0x61, 0x70, 0x70, 0x30, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xAA, 0x50, 0x00, 0x11, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x1F, 0x00,
  0x61, 0x70, 0x70, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEB, 0xEB, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xF2, 0x13, 0xFF, 0x81, 0x20, 0x9A, 0xF1, 0x14, 0x42, 0x4B, 0x37, 0xE6,
  0x67, 0xEE, 0x94, 0x09, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF,
};

// The raw bench: a socket with nothing but lwIP between the wire and the buffer, so the
// async server's share of the cost can be told apart from the radio's. The loop writes
// the result and the server's task reads it, so it is a fixed array and not a String.
WiFiServer rawServer(9000);
char rawResult[96] = "not run";

// mbedtls wants two buffers of about sixteen kilobytes for every handshake. A room of
// its own was tried and made things worse: fifty-seven kilobytes set aside for good left
// the web server with a largest free block of nine, and the module went down under any
// load at all. The bot checks for a block big enough before it opens anything instead,
// which costs nothing and leaves the heap whole.
SemaphoreHandle_t logMutex;      // logEvent is reached from the loop and from the server task
SemaphoreHandle_t consoleMutex;  // The ring is filled by the loop and read by the console page
SemaphoreHandle_t tlsGate;       // One encrypted conversation at a time: two will not fit in the heap
SemaphoreHandle_t cardMutex;     // Protects SD card from concurrent operations
volatile bool tlsWanted;
uint32_t consoleSeq, loggedSamples;
uint32_t logFlushAt, logLinkAt, loggedSent, loggedResent, loggedMangled, loggedTimeouts, linkTimeouts;
volatile bool lineProbeRequested, updateRunning;
uint32_t updateTouchedAt;
uint32_t uploadStartedAt, uploadCardMicros, uploadBytes;
String uploadReport = "not run";
String downloadReport = "not run";
#define STREAM_STILL_WARM 1500
#define SEND_HEADROOM_BYTES 4096
#define STREAM_STALL_GIVE_UP 20000
#define STREAMS_AT_ONCE 2

volatile uint32_t lastStreamAt;

bool somethingIsStreaming() {
  const uint32_t at = lastStreamAt;
  return at && millis() - at < STREAM_STILL_WARM;
}

static inline size_t sendableNow() {
  const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  return block > SEND_HEADROOM_BYTES ? block - SEND_HEADROOM_BYTES : 0;
}

volatile uint8_t streamsLive;

// Holds the card for as long as it is sending. A streaming answer reads the card again
// on every packet, so claiming it once per read leaves gaps the journal and the feeder
// walk straight into, and two readers inside the driver is what hangs the module.
struct DownloadJob {
  DownloadJob() {
    streamsLive++;
    haveCard = cardMutex && xSemaphoreTakeRecursive(cardMutex, pdMS_TO_TICKS(300)) == pdTRUE;
  }

  ~DownloadJob() {
    if (haveCard)
      xSemaphoreGiveRecursive(cardMutex);
    if (streamsLive)
      streamsLive--;
  }

  bool haveCard = false;

  FileWrapper file;
  FileFeeder<FileWrapper> feeder;
  uint32_t startedAt = 0, fills = 0, cardMicros = 0, worstMicros = 0;
  uint32_t heldOff = 0, heldSince = 0, blockLow = 0;

  size_t served = 0;
};
uint32_t sinkStartedAt;
volatile bool uploadLanding, sinkLanding;

static inline bool receivingFile() {
  return uploadLanding || sinkLanding || telegramBot.receiving();
}

String sinkReport = "not run";
char lineProbeResult[192] = "{\"verdict\":\"not measured yet, ask once more\"}";
String bootProbeResult = "{\"verdict\":\"not run\"}";

uint8_t serialBaudIndex;
extern PrinterLink printerLink;
void linkBegin();
String linkReport();

// Stored files
Preferences preferences;

// Three tasks read these while the loop writes them. Assigning a String frees the old
// buffer and allocates a new one, so a reader standing on the old pointer reads memory
// that has just been handed to somebody else. Giving each one room for the longest path
// it can ever hold means the assignment copies into the buffer that is already there and
// frees nothing, which is what the readers need. pathBufferMoved stays at zero as long
// as that holds.
uint32_t pathBufferMoved;
static const char *selectedFileRoom, *printingFileRoom;
static const char *commandRoom, *responseRoom;
static const char *machineRoom, *probeRoom;

static int buffersStillHome();

String selectedFile;
size_t selectedFileSize, filePos;
uint32_t selectedFileDate;

String printingFile;
size_t printingFileSize;

String lastUploadFields, uploadFailure;

// Temperature for printer status reporting
#define TEMP_COMMAND      "M105"
#define AUTOTEMP_COMMAND  "M155 S"
#define SDREPORT_COMMAND  "M27 S"       // The printer tells us when it prints from its own card
#define SDREPORT_INTERVAL 5

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

void logEvent(const String &text);
static String spell(const int32_t minutes);

static bool telnetLoggedIn;

// Whoever reaches this port can push gcode straight at the printer, so until the
// password has been typed the socket is deaf and nothing is echoed down it. The word
// itself crosses the house network as typed, which a raw socket cannot help; the web
// uses digest and never sends it at all.
inline bool telnetReady() {
  return serverClient && serverClient.connected() && telnetLoggedIn;
}

// A client whose machine went to sleep leaves a socket that is neither closed nor
// listening, and a write to it costs ten seconds in the core's own retry loop. Two of
// those in one pass of the loop is most of the watchdog's patience, so the debug
// stream is dropped rather than allowed to hold up a print.
inline void telnetDropIfStalled(const uint32_t began) {
  if (millis() - began <= TELNET_STALL_MS)
    return;

  serverClient.stop();
  logEvent("dropped a telnet client that had stopped reading");
}

inline void telnetSend(const String &line) {
  if (!telnetReady())
    return;

  const uint32_t began = millis();
  serverClient.println(line);
  telnetDropIfStalled(began);
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
bool parseTemp(const String &response, const String &whichTemp, Temperature *temperature) {
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
bool parsePrusaHeatingTemp(const String &response, const String &whichTemp, Temperature *temperature) {
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

bool parseTemperatures(const String &response) {
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

bool startBeep = true;

inline void playSound() {
  if (startBeep)
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
  if (selectedFileRoom && selectedFile.c_str() != selectedFileRoom)
    ++pathBufferMoved;
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

// Orca appends its summary after the last move, including what the job weighs and what
// the spool cost, so the price of a print comes out of the file itself.
// ponytail: the first extruder only, this printer has one
void readFilamentUse(const String path, float &grams, float &price) {
  grams = -1;
  price = filamentDefaultPrice;

  FileWrapper file = storageFS.open(path);
  if (!file)
    return;

  const uint32_t size = file.size();
  if (size > FILAMENT_SCAN_BYTES)
    file.seek(size - FILAMENT_SCAN_BYTES);

  while (file.available()) {
    const String line = file.readStringUntil('\n');
    if (!line.startsWith("; "))
      continue;

    const int equals = line.indexOf('=');
    if (equals == -1)
      continue;

    if (line.startsWith("; total filament used [g]") || (grams < 0 && line.startsWith("; filament used [g]")))
      grams = line.substring(equals + 1).toFloat();
    else if (line.startsWith("; filament_cost"))
      price = line.substring(equals + 1).toFloat();
  }
  file.close();
}

uint16_t readTotalLayers(const String path) {
  FileWrapper file = storageFS.open(path);
  if (!file)
    return 0;

  char buffer[1025];
  const size_t got = file.read((uint8_t *)buffer, sizeof(buffer) - 1);
  buffer[got] = 0;
  file.close();

  const char *found = strstr(buffer, "total layer number:");

  return found == NULL ? 0 : atoi(found + 19);
}

// Nothing about a print survives it today: the telnet log exists only while somebody
// is watching. One line on the card per print is enough to answer, the morning after,
// what ran, how long, how it ended and how the link behaved.
// The loop fills this on every line from the printer while the console page reads it
// from the server's task. The lines differ in length, so each assignment frees the old
// buffer, and a reader standing on that buffer takes the heap down with it.
void consoleAdd(const String &line) {
  if (consoleMutex)
    xSemaphoreTake(consoleMutex, portMAX_DELAY);
  consoleRing[consoleSeq % CONSOLE_LINES] = line;
  ++consoleSeq;
  if (consoleMutex)
    xSemaphoreGive(consoleMutex);
}

// A damaged line is worth keeping as it arrived, so escape what cannot be written
// as text rather than dropping it.
String printableOnly(const String &text) {
  String out;
  out.reserve(text.length() + 8);
  for (unsigned int i = 0; i < text.length(); ++i) {
    const char ch = text[i];
    if (ch >= 32 && ch <= 126)
      out += ch;
    else {
      char hex[6];
      snprintf(hex, sizeof(hex), "\\x%02X", (uint8_t)ch);
      out += hex;
    }
  }

  return out;
}

void logFlush();

// The web server runs in its own task, so uploads and the main loop both land here.
// Appending to a String is read, reallocate and copy, which two tasks cannot share.
void logEvent(const String &text) {
  const time_t now = time(NULL);   // Straight after a power cut the clock is nonsense until NTP answers
  const String line = (now > 1600000000 ? String((uint32_t)now) : String("-")) +
                      " " + String(millis() / 1000) + " " + text + "\r\n";

  if (logMutex)
    xSemaphoreTake(logMutex, portMAX_DELAY);
  logBuffer += line;
  const bool full = logBuffer.length() > LOG_BUFFER_BYTES;
  if (logMutex)
    xSemaphoreGive(logMutex);

  if (full)
    logFlush();
}

void logFlush() {
  if (logBuffer == "" || !storageFS.isActive())
    return;

  if (cardMutex && xSemaphoreTakeRecursive(cardMutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return;   // Card busy; will flush on next cycle

  if (logMutex)                    // Taken out of the way first, so the card is written unlocked
    xSemaphoreTake(logMutex, portMAX_DELAY);
  const String pending = logBuffer;
  logBuffer = "";
  if (logMutex)
    xSemaphoreGive(logMutex);

  if (pending == "") {
    if (cardMutex)
      xSemaphoreGiveRecursive(cardMutex);
    return;
  }

  storageFS.mkdir("/log");

  FileWrapper probe = storageFS.open(LOG_PATH);
  const uint32_t size = probe ? probe.size() : 0;
  if (probe)
    probe.close();

  if (size > LOG_MAX_BYTES) {          // Roll over rather than grow without end
    storageFS.remove(LOG_PREVIOUS);
    storageFS.rename(LOG_PATH, LOG_PREVIOUS);
  }

  FileWrapper file = storageFS.open(LOG_PATH, "a");
  if (file) {
    file.write((const uint8_t *)pending.c_str(), pending.length());
    file.close();
  }

  if (cardMutex)
    xSemaphoreGiveRecursive(cardMutex);

  logFlushAt = millis() + LOG_FLUSH_INTERVAL;
}

// A line per resend would be noise and a line per print would miss the moment things
// started going wrong, so the counters are reported as a running total, and only when
// one of them has moved.
// Why the board started, straight out of the chip. Needs no card, which matters
// because a boot with no card is exactly the boot that writes nothing down.
const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout, supply sagged";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    case ESP_RST_EXT:      return "reset pin";
    default:               return "unknown";
  }
}

// Losing the card costs the files, the page and the log at once, so keep asking for it
// rather than waiting for somebody to notice and reboot.
void handleStorage() {
  static uint32_t retryAt;

  if (storageFS.isActive() || isPrinting)
    return;

  if ((int32_t)(millis() - retryAt) < 0)
    return;

  retryAt = millis() + REMOUNT_INTERVAL;
  if (storageFS.mount()) {
    storageFS.mkdir("/ui");
    logEvent("card mounted again");
  }
}

void handleLogging() {
  if (logBuffer != "" && (int32_t)(millis() - logFlushAt) >= 0)
    logFlush();

  // The encryption needs an unbroken block, not a total, so the block is what gets
  // written down. A crash with no warning in the journal was the whole problem before.
  static uint32_t heapWatchAt;
  if ((int32_t)(millis() - heapWatchAt) >= 0) {
    heapWatchAt = millis() + 60000;
    const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (block < HEAP_BLOCK_WARNING)
      logEvent("heap getting broken up: free " + String(ESP.getFreeHeap()) +
               ", largest block " + String(block));
  }

  if (!isPrinting || (int32_t)(millis() - logLinkAt) < 0)
    return;

  logLinkAt = millis() + LOG_LINK_INTERVAL;
  if (linesResent == loggedResent && linesMangled == loggedMangled && linkTimeouts == loggedTimeouts)
    return;

  loggedSent = linesSent;
  loggedResent = linesResent;
  loggedMangled = linesMangled;
  loggedTimeouts = linkTimeouts;
  logEvent("link sent " + String(linesSent) + " resent " + String(linesResent) +
           " mangled " + String(linesMangled) + " timeouts " + String(linkTimeouts) +
           " rssi " + String(WiFi.RSSI()) +
           " heap " + String(ESP.getFreeHeap()) +
           " block " + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

// The file used to be read with readStringUntil, which asks the card driver for one
// byte at a time and grows the line sixteen bytes per reallocation. Hundreds of lines
// a second of that is what leaves the heap with no unbroken block for the encryption.
static GcodeReader gcode;

static inline bool gcodeAtEnd(FileWrapper &file) {
  return gcode.blockSpent() && !file.available();
}

//*****************************************************
//* Sending a file to the printer's own card, and      *
//* letting the printer print it without us            *
//*****************************************************
// This is the same feeding loop as a print, with M28 opened around it. Two things about
// M28 decide the shape of the code. While it is open Marlin stops executing what it is
// sent and writes it to the file instead, so every periodic command of ours would end up
// inside the gcode; they are held back for the duration. And the printer stands perfectly
// still for the whole transfer, which the dead man timer reads as a dead module, so it is
// disarmed first and armed again at the end.
//
// The point of all this: once the file is on the printer's card, the printer prints it on
// its own. This module can reboot, lose wifi or be unplugged and the print carries on.

volatile bool cardSendRunning;          // Read by everything that must keep off the wire
String cardSendSource, cardSendTarget;
bool sendQueueEnabled = true;
String sendQueueHeldBack;               // A file that failed once is not tried again on its own
#define SEND_QUEUE_PAUSE 5000
String cardSendReport = "not run";
bool cardSendDeleteWhenDone;

CardIndex printerCard;                  // What the printer says is on its own card
CardTransfer cardSend;

// Hands the transfer one line at a time out of the module's own card. The lock is taken
// per line rather than for the whole transfer, so the web is never shut out for a minute.
// Shares the print feeder's reader on purpose. A transfer and a print can never run at
// the same time, and two of these buffers do not fit in the static memory that is left.
struct CardFileLines : LineSource {
  FileWrapper file;
  GcodeReader &reader = gcode;
  uint32_t taken = 0;

  int next(char *out, const size_t max) override {
    if (!file)
      return -1;
    if (cardMutex && xSemaphoreTakeRecursive(cardMutex, pdMS_TO_TICKS(500)) != pdTRUE)
      return 0;                         // Busy this pass, the line is still there
    const size_t used = reader.nextLine(file);
    if (cardMutex)
      xSemaphoreGiveRecursive(cardMutex);
    if (!used)
      return -1;                        // Nothing left in the file

    taken += used;
    size_t n = strlen(reader.line());
    if (n >= max) n = max - 1;
    memcpy(out, reader.line(), n);
    out[n] = 0;
    return (int)n;
  }

  uint32_t used() const override { return taken; }
};
static CardFileLines cardSendLines;

// Marlin creates files on the card through its own 8.3 converter unless it was built with
// long name writing, so the short name is chosen here, where the long one is still known.
void forEachStoredFile(std::function<void(const String, const uint32_t, const uint32_t)> visit);

String shortCardName(const String &longName) {
  char out[CARD_DOS_BYTES];
  cardShortName(longName.c_str(), out, sizeof out);
  return String(out);
}

String cardTargetFor(const String &path) {
  if (!printerLink.caps.lfnWrite)
    return shortCardName(path);
  String name = baseName(path);
  if (name.length() >= CARD_TARGET_BYTES)
    name = name.substring(0, CARD_TARGET_BYTES - 1);
  return name;
}

const char *cardSendWhyNot() {
  if (!printerConnected)        return "the printer is not answering";
  if (isPrinting)               return "this module is printing";
  if (externalPrinting)         return "the printer is printing from its own card";
  if (cardSend.running())       return "a file is already going over";
  if (updateRunning)            return "the firmware is being written";
  if (!storageFS.isActive())    return "this module has no card";
  return NULL;
}

bool cardSendBegin(const String &path, const bool printAfter, const bool deleteAfter) {
  if (cardSendWhyNot())
    return false;

  cardSendLines.file = storageFS.open(path);
  if (!cardSendLines.file)
    return false;
  cardSendLines.reader.rewind();
  cardSendLines.taken = 0;

  cardSendSource = path;
  cardSendTarget = cardTargetFor(path);
  cardSendDeleteWhenDone = deleteAfter;
  #ifdef DEAD_MAN_SECONDS
    cardSend.deadManSeconds = DEAD_MAN_SECONDS;
  #endif
  cardSend.link = &printerLink;
  cardSend.index = &printerCard;

  if (!cardSend.start(&cardSendLines, cardSendTarget.c_str(),
                      cardSendLines.file.size(), printAfter, millis())) {
    cardSendLines.file.close();
    return false;
  }

  cardSendRunning = true;
  logEvent("sending " + baseName(path) + " to the printer's card as " + cardSendTarget
           + ", " + String(cardSendLines.file.size()) + " bytes");
  lcd("Sending to printer");
  return true;
}

static void cardSendSettle() {
  const bool good = cardSend.phase == CardDone;
  const uint32_t spent = cardSend.finishedAt - cardSend.startedAt;

  cardSendReport = String(good ? "sent" : "failed") + ": " + cardSendTarget
                 + ", " + String(cardSend.written) + " bytes in " + String(spent / 1000) + " s, "
                 + String(spent ? cardSend.written / (float)spent : 0, 1) + " KB/s, "
                 + String(cardSend.lines) + " lines, " + String(cardSend.skipped) + " skipped"
                 + (good ? "" : String(", ") + cardSend.trouble);
  logEvent(cardSendReport);

  cardSendLines.file.close();
  cardSend.phase = CardIdle;
  cardSendRunning = false;

  if (!good) {
    lcd("Send failed");
    sendQueueHeldBack = baseName(cardSendSource);
    return;
  }

  sendQueueHeldBack = "";
  playSound();                          // The file is on the printer's card, whole
  lcd("On printer card");
  if (cardSendDeleteWhenDone) {
    if (cardMutex && xSemaphoreTakeRecursive(cardMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      storageFS.remove(cardSendSource);
      xSemaphoreGiveRecursive(cardMutex);
      logEvent("removed " + baseName(cardSendSource) + " from this module, it lives on the printer now");
    }
  }
}

// A module nobody can reach is as bad as one anyone can, and there is no wire to this
// one. So the password it invented for itself is put where the owner can read it: the
// printer's own screen and Telegram if the bot is set up.
//
// Never the journal. That is a file on the card, it is served over plain HTTP, and it
// outlives a fall back to an older image that has no password on it at all.
//
// The screen is repainted rather than written once, because everything else the module
// says goes to the same single line, and because a screen is easy to miss.
void handleWebAuthGreeting() {
  static uint32_t sayAgain;
  static bool toldOnce;

  if (!webAuthIsFresh() || !printerConnected)
    return;
  if (toldOnce && (int32_t)(millis() - sayAgain) < 0)
    return;
  sayAgain = millis() + WEBAUTH_GREET_AGAIN;

  lcd(webAuthGreeting());
  if (!toldOnce) {
    toldOnce = true;
    logEvent("a web password was invented, it is on the printer's screen");
    telegramBot.notify(webAuthGreeting() + "\nChange it on the settings page.");
  }
}

void handleCardSend() {
  if (cardSend.phase == CardIdle)
    return;
  cardSend.service(millis());
  if (cardSend.running() || !cardSend.quiet())
    return;
  cardSendSettle();
}

// ---------------------------------------------------------- flashing the printer
// The image goes onto the printer's own card with Marlin's binary protocol, then M997
// reboots it and the bootloader on the board picks the image up. No card to pull out.
#define PRINTER_FIRMWARE_PATH  "/printer.bin"
#define PRINTER_FIRMWARE_NAME  "Robin_nano35.bin"

LinkIo *printerWire();          // The one object that owns the serial port, defined below

// Kept off the heap until it is wanted: its buffers are nearly three hundred bytes and
// bss is the segment that runs out on this board. Flashing the printer happens once in
// a while, with no print running, which is exactly when the heap is whole.
BinaryTransfer *printerFlash;
volatile bool printerFlashRunning;
String printerFlashReport;

struct CardBytes : ByteSource {
  FileWrapper file;
  uint32_t taken = 0;

  int next(uint8_t *out, const size_t max) override {
    if (!file)
      return -1;
    int got = 0;
    if (cardMutex && xSemaphoreTakeRecursive(cardMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      got = file.read(out, max);
      xSemaphoreGiveRecursive(cardMutex);
    }
    else
      return -1;
    if (got < 0)
      return -1;
    taken += got;
    return got;
  }

  uint32_t size() const override { return bytes; }
  uint32_t bytes = 0;            // FileWrapper cannot be asked through a const handle
};
static CardBytes printerFlashBytes;

const char *printerFlashWhyNot() {
  if (!printerConnected)        return "the printer is not answering";
  if (isPrinting)               return "this module is printing";
  if (externalPrinting)         return "the printer is printing from its own card";
  if (cardSend.running())       return "a file is going to the printer's card";
  if (printerFlash)             return "the printer is already being flashed";
  if (updateRunning)            return "this module's own firmware is being written";
  if (!storageFS.isActive())    return "this module has no card";
  return NULL;
}

bool printerFlashBegin(const String &target) {
  if (printerFlashWhyNot())
    return false;

  printerFlashBytes.file = storageFS.open(PRINTER_FIRMWARE_PATH);
  if (!printerFlashBytes.file || !printerFlashBytes.file.size())
    return false;
  printerFlashBytes.taken = 0;
  printerFlashBytes.bytes = printerFlashBytes.file.size();

  printerFlash = new BinaryTransfer();
  if (!printerFlash) {
    printerFlashBytes.file.close();
    return false;
  }
  printerFlash->begin(printerWire());
  if (!printerFlash->start(&printerFlashBytes, target.c_str(), millis())) {
    delete printerFlash;
    printerFlash = NULL;
    printerFlashBytes.file.close();
    return false;
  }

  printerFlashRunning = true;
  logEvent("flashing the printer with " + String(printerFlashBytes.bytes)
           + " bytes as " + target);
  lcd("Flashing printer");
  return true;
}

void handlePrinterFlash() {
  if (!printerFlash)
    return;

  printerFlash->service(millis());
  if (printerFlash->running())
    return;

  const bool good = printerFlash->phase == BinaryDone;
  const uint32_t spent = printerFlash->finishedAt - printerFlash->startedAt;
  printerFlashReport = String(good ? "flashed" : "failed") + ": "
                     + String(printerFlash->written) + " bytes in " + String(spent / 1000) + " s, "
                     + String(printerFlash->packets) + " packets, "
                     + String(printerFlash->resends) + " repeats"
                     + (good ? "" : String(", ") + printerFlash->trouble);
  logEvent(printerFlashReport);

  printerFlashBytes.file.close();
  delete printerFlash;
  printerFlash = NULL;
  printerFlashRunning = false;

  if (!good) {
    lcd("Flash failed");
    return;
  }

  // The image is on the printer's card. M997 is a plain reboot on this board, and the
  // bootloader is what does the flashing, so the printer goes away for a while.
  playSound();
  lcd("Rebooting printer");
  commandQueue.push("M997");
  printerCard.stale();
  logEvent("asked the printer to restart so its bootloader takes the image");
}

void askCardList() {
  printerCard.stale();
  printerCard.expect();
  commandQueue.push("M21");
  commandQueue.push("M20 L");
}

// The queue is not kept anywhere: it is every gcode on this module's card that the printer's
// card does not already hold. Nothing to lose over a reboot and nothing to keep in step.
void handleSendQueue() {
  static uint32_t lookAt;

  if (!sendQueueEnabled || cardSend.phase != CardIdle || cardSendWhyNot())
    return;
  if ((int32_t)(millis() - lookAt) < 0)
    return;
  lookAt = millis() + SEND_QUEUE_PAUSE;

  if (!printerCard.fresh()) {
    if (!printerCard.busy())
      askCardList();
    return;
  }

  String pick;
  forEachStoredFile([&pick](const String name, const uint32_t size, const uint32_t date) {
    (void)size; (void)date;
    if (pick != "" || name == sendQueueHeldBack)
      return;
    if (!printerCard.holds(cardTargetFor(name).c_str()))
      pick = name;
  });

  if (pick != "")
    cardSendBegin("/" + pick, false, true);
}


void handlePrint() {
  static FileWrapper gcodeFile;
  static float prevM532Completion;

  if (isPrinting) {
    const bool abortPrint = (restartPrint || cancelPrint || printerRestarted);
    if (abortPrint || gcodeAtEnd(gcodeFile)) {
      printEndedEarly = !abortPrint && filePos + 64 < printingFileSize;
      gcodeFile.close();
      if (fwProgressCap)
        commandQueue.push("M530 S0");
      if (printEndedEarly) {
        cancelPrint = true;
        lcd("Ended early");
      }
      else if (!abortPrint)
        lcd("Complete");
      {
        const float grams = filamentGrams < 0 ? 0 : filamentGrams * (printingFileSize ? (float)filePos / printingFileSize : 1);
        logEvent(String("print end ") + (printEndedEarly ? "ended early" : abortPrint ? "cancelled" : "complete") +
                 " " + baseName(printingFile) +
                 " " + String((millis() - printStartTime) / 1000) + "s" +
                 " layer " + String(currentLayer) + "/" + String(totalLayers) +
                 " sent " + String(linesSent) + " resent " + String(linesResent) +
                 " mangled " + String(linesMangled) + " timeouts " + String(linkTimeouts) +
                 " " + String(energyWh, 1) + "Wh " + String(grams, 2) + "g " +
                 String(energyWh / 1000 * energyTariff + grams / 1000 * filamentPrice, 3) + "AZN");
        logFlush();
        // Before the socket is armed, because switching it off also switches off the
        // radio that would carry this.
        telegramBot.notify(String(printEndedEarly ? "Print ended early" : abortPrint ? "Print cancelled" : "Print finished")
                           + " \u2014 " + baseName(printingFile) + "\n\n"
                           + "Duration   " + spell((millis() - printStartTime) / 60000) + "\n"
                           + "Layers     " + String(currentLayer) + " / " + String(totalLayers) + "\n"
                           + "Filament   " + String(grams, 1) + " g\n"
                           + "Energy     " + String(energyWh, 0) + " Wh\n"
                           + "Cost       " + String(energyWh / 1000 * energyTariff + grams / 1000 * filamentPrice, 2) + " AZN");
      }
      #ifdef DEAD_MAN_SECONDS
        commandQueue.push("M85 S0");     // Cooling down is silent, and silence must not kill the printer now
      #endif
      printPause = false;
      isPrinting = false;
      powerOffArmed = autoPowerOff;
      powerOffSawIdle = false;
      powerOffReadySince = 0;
      powerOffAttempts = 0;
    }
    // The card is asked for, not simply used. The web server reads the same card from its
    // own task, and whoever gets there second used to wait inside the driver. Half a
    // second is far more than a block read needs and far less than the watchdog allows,
    // and a pass given up here is picked up on the next one.
    else if (!printPause && commandQueue.getFreeSlots() > 4
             && (!cardMutex || xSemaphoreTakeRecursive(cardMutex, pdMS_TO_TICKS(500)) == pdTRUE)) {    // Keep some space for "service" commands
      filePos += gcode.nextLine(gcodeFile);
      if (cardMutex)
        xSemaphoreGiveRecursive(cardMutex);
      static String line;                 // Kept between passes, so its buffer is not
      line = gcode.line();                // handed back and asked for again every line

      if (line.startsWith(";LAYER_CHANGE"))
        ++currentLayer;
      else if (line.startsWith(";Z:"))
        currentZ = line.substring(3).toFloat();

      int pos = line.indexOf(';');
      if (line.length() > 0 && pos != 0 && line[0] != '(' && line[0] != '\r') {
        if (pos != -1)
          line.remove(pos);             // In place, where substring would allocate

        if (line.startsWith("M73")) {
          const int r = line.indexOf('R');
          if (r != -1)
            slicerMinutesLeft = line.substring(r + 1).toInt();
          const int pp = line.indexOf('P');
          if (pp != -1)
            slicerPercent = line.substring(pp + 1).toInt();
        }
        else if (line.startsWith("M107"))
          fanSpeed = 0;
        else if (line.startsWith("M106")) {
          const int sPos = line.indexOf('S');
          fanSpeed = sPos == -1 ? 255 : line.substring(sPos + 1).toInt();
        }

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

  if (!isPrinting && (startPrint || restartPrint) && cardSendRunning) {
    startPrint = restartPrint = false;    // The reader and the wire belong to the transfer
    lcd("Busy sending");
    logEvent("a print was asked for while a file was going to the printer's card, refused");
  }
  else if (!isPrinting && (startPrint || restartPrint)) {
    startPrint = restartPrint = false;

    filePos = 0;
    prevM532Completion = 0.0;
    currentLayer = 0;
    currentZ = 0;
    slicerMinutesLeft = slicerPercent = -1;
    fanSpeed = -1;
    linesSent = linesResent = linesMangled = 0;

    gcodeFile = storageFS.open(selectedFile);
    gcode.rewind();
    if (!gcodeFile)
      lcd("Can't open file");
    else {
      printingFile = selectedFile;
      if (printingFileRoom && printingFile.c_str() != printingFileRoom)
        ++pathBufferMoved;
      printingFileSize = selectedFileSize;
      totalLayers = readTotalLayers(printingFile);
      lcd("Printing...");
      playSound();
      printStartTime = millis();
      isPrinting = true;
      #ifdef DEAD_MAN_SECONDS
        commandQueue.push("M85 S" + String(DEAD_MAN_SECONDS));   // Nothing else turns the heaters off if this module dies
      #endif
      linkTimeouts = loggedResent = loggedMangled = loggedTimeouts = loggedSamples = 0;
      logLinkAt = millis() + LOG_LINK_INTERVAL;
      logEvent("print start " + baseName(printingFile) + " " + String(printingFileSize) + " bytes, " +
               String(totalLayers) + " layers, rssi " + String(WiFi.RSSI()));
      energyWh = 0;
      energyLastWatt = -1;
      energyLastAt = yandexHome.getPowerAt();
      readFilamentUse(printingFile, filamentGrams, filamentPrice);
      energyPollAt = millis();
      if (fwProgressCap) {
        commandQueue.push("M530 S1 L0");
        commandQueue.push("M531 " + baseName(printingFile));
      }
    }
  }
}

// The server hands the body over in packets of about 1.4 KB. Writing each one straight
// to the card costs a full FatFs round trip, and on a 20 MB file that is fourteen thousand
// of them, so they are gathered here and written in one go.
//
// The card controller moves this over DMA, which wants a word aligned address. Without
// the hint the driver quietly falls back to copying through a small bounce buffer, and
// writing a megabyte takes thirteen seconds instead of half of one.
// Held for good rather than taken per request: see the /log handler for why.
static char logTail[LOG_TAIL_BYTES] __attribute__((aligned(32)));
static size_t logTailHeld;
static volatile bool logTailBusy;

static uint8_t uploadBuffer[UPLOAD_BUFFER_BYTES] __attribute__((aligned(32)));
static size_t uploadBuffered;
static uint32_t uploadTouchedAt;

static bool uploadFlush(FileWrapper &file) {
  if (uploadBuffered == 0)
    return true;

  const size_t pending = uploadBuffered;
  uploadBuffered = 0;
  return file.write(uploadBuffer, pending) == pending;
}

// One file handle serves every upload, so a second client arriving mid-transfer would
// write its body into the first one's file. The latecomer is turned away instead.
bool uploadIsBusy(AsyncWebServerRequest *request) {
  return request->_tempObject != NULL;
}

static void refuseUpload(AsyncWebServerRequest *request) {
  if (!request->_tempObject)
    request->_tempObject = malloc(1);     // Freed with the request, and only its presence matters
}

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  static FileWrapper file;
  static String path;
  static AsyncWebServerRequest *owner;

  if (!index) {
    // A dropped connection never delivers its final chunk, so the handle would stay
    // open and lock every later upload out. Silence for long enough means it is gone.
    if (file && owner && owner != request) {
      if (millis() - uploadTouchedAt < UPLOAD_ABANDONED_AFTER) {
        refuseUpload(request);
        return;
      }
      file.close();
      if (path != "")
        storageFS.remove(path);
      path = "";
      logEvent("gave up on an upload that went quiet, taking the card back");
    }
    owner = request;
    uploadBuffered = 0;
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
    else if (isPrinting || cardSendRunning)
      uploadFailure = "a print is running, the card is busy";
    else if (telegramBot.receiving())
      uploadFailure = "a file is arriving from Telegram, the card is busy";
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

  if (uploadIsBusy(request))        // Turned away at the door, its body goes nowhere
    return;

  if (!index) {
    uploadStartedAt = millis();
    uploadCardMicros = 0;
    uploadBytes = 0;
    uploadLanding = true;
  }
  uploadTouchedAt = lastStreamAt = millis();

  if (uploadFailure == "" && file) {
    size_t taken = 0;
    while (taken < len) {
      const size_t room = UPLOAD_BUFFER_BYTES - uploadBuffered;
      const size_t piece = room < len - taken ? room : len - taken;
      memcpy(uploadBuffer + uploadBuffered, data + taken, piece);
      uploadBuffered += piece;
      taken += piece;

      if (uploadBuffered == UPLOAD_BUFFER_BYTES) {
        const uint32_t into = micros();
        const bool wrote = uploadFlush(file);
        uploadCardMicros += micros() - into;
        uploadBytes += UPLOAD_BUFFER_BYTES;
        if (!wrote) {
          uploadFailure = "write error, card full";
          break;
        }
      }
    }
  }

  if (final) {
    if (file) {
      if (uploadFailure == "") {
        const size_t rest = uploadBuffered;
        if (uploadFlush(file))
          uploadBytes += rest;
        else
          uploadFailure = "write error, card full";
      }
      file.close();
    }
    uploadBuffered = 0;
    uploadLanding = false;
    {
      const uint32_t ms = millis() - uploadStartedAt;
      uploadReport = String(uploadBytes) + " bytes in " + String(ms) + " ms, "
                     + String(ms ? uploadBytes / (float)ms : 0, 1) + " KB/s, card held it "
                     + String(uploadCardMicros / 1000) + " ms";
    }
    if (uploadFailure != "") {
      if (path != "")
        storageFS.remove(path);
    }
    else if (path != "")
      selectFile(path);
    path = "";
    owner = NULL;
  }
}

bool isInterfaceFilename(const String name) {
  if (name.startsWith("."))
    return false;

  String lowercase = name;
  lowercase.toLowerCase();
  if (lowercase.endsWith(".gz"))       // index.html.gz is served compressed, and is a third the size
    lowercase = lowercase.substring(0, lowercase.length() - 3);

  return lowercase.endsWith(".html") || lowercase.endsWith(".css") || lowercase.endsWith(".js") ||
         lowercase.endsWith(".json") || lowercase.endsWith(".svg") || lowercase.endsWith(".png") ||
         lowercase.endsWith(".ico") || lowercase.endsWith(".woff2") || lowercase.endsWith(".txt");
}

void handleInterfaceUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
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
    else if (isPrinting)
      uploadFailure = "a print is running, the card is busy";
    else if (!isInterfaceFilename(name))
      uploadFailure = "not an interface file";
    else if (request->contentLength() >= storageFS.freeBytes())
      uploadFailure = "not enough free space";
    else {
      storageFS.mkdir("/ui");
      // The server prefers the plain file when both are there, so the one being
      // replaced goes, whichever of the pair it is.
      if (name.endsWith(".gz"))
        storageFS.remove("/ui/" + name.substring(0, name.length() - 3));
      else
        storageFS.remove("/ui/" + name + ".gz");

      file = storageFS.open("/ui/" + name, "w");
      if (!file)
        uploadFailure = "cannot create file";
      else
        path = "/ui/" + name;
    }
  }

  if (uploadFailure == "" && file && file.write(data, len) != len)
    uploadFailure = "write error, card full";

  if (final) {
    if (file)
      file.close();
    if (uploadFailure != "" && path != "")
      storageFS.remove(path);
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

String M115ExtractString(const String &response, const String &field) {
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

bool M115ExtractBool(const String &response, const String &field, const bool onErrorValue = false) {
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
    MDNS.setInstanceName(getDeviceName().c_str());  // Can't call MDNS.init because 'ArduinoOTA.begin' already did, so only the shown name is changed here
  #else
    if (!MDNS.begin(MDNS_HOSTNAME))
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
      PrinterSerial.begin(serialBauds[serialBaudIndex], SERIAL_8N1, PRINTER_RX_PIN, PRINTER_TX_PIN);
      printerLink.reset();
      printerCard.stale();
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
          fwAutoreportTempCap = printerLink.caps.autoreportTemp;
          fwAutoreportSDCap = printerLink.caps.autoreportSd;
          fwProgressCap = printerLink.caps.progress;
          fwBuildPercentCap = printerLink.caps.buildPercent;

          mDNSInit();

          String text = IpAddress2String(WiFi.localIP()) + " " + storageFS.getActiveFS();
          lcd(text);
          playSound();

          if (fwAutoreportTempCap)
            commandQueue.push(AUTOTEMP_COMMAND + String(TEMPERATURE_REPORT_INTERVAL));   // Start auto report temperatures
          if (fwAutoreportSDCap)
            commandQueue.push(SDREPORT_COMMAND + String(SDREPORT_INTERVAL));             // And when it prints without us
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

String jsonEscape(const String &text) {
  String escaped;
  escaped.reserve(text.length() + 8);
  for (unsigned int i = 0; i < text.length(); ++i) {
    const char ch = text[i];
    if (ch == '"' || ch == '\\')
      escaped += '\\';
    if (ch >= 32)
      escaped += ch;
  }

  return escaped;
}

String htmlEscape(const String &text) {
  String escaped;
  escaped.reserve(text.length() + 16);
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

String urlEncode(const String &text) {
  static const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(text.length() + 16);

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

// The bot numbers the files so a phone keyboard never has to spell one out.
String storedFile(const int wanted, String &listing) {
  String found;
  listing.reserve(1024);
  int seen = 0;

  FileWrapper dir = storageFS.open("/");
  if (dir) {
    FileWrapper file = dir.openNextFile();
    while (file) {
      const String name = baseName(file.name());
      if (!file.isDirectory() && isGcodeFilename(name)) {
        seen++;
        listing += String(seen) + ". " + name + "  (" + String(file.size() / 1024) + " KB)\n";
        if (seen == wanted)
          found = "/" + name;
      }
      file.close();
      file = dir.openNextFile();
    }
    dir.close();
  }

  return found;
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
  String json;
  json.reserve(2048);          // The core grows a String sixteen bytes at a time
  json = "{\r\n  \"files\": [";
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
  html.reserve(1536);

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

inline String getState();

// Taking firmware over the bot's connection, with the flash handling kept here where
// everything it has to put aside already lives.
bool firmwareBegin() {
  if (isPrinting || updateRunning)
    return false;

  if (Update.isRunning())
    Update.abort();
  if (!Update.begin(UPDATE_SIZE_UNKNOWN))
    return false;

  lcd("Updating...");
  logEvent("firmware over telegram started");
  logFlush();
  updateTouchedAt = millis();   // The loop gives up on an upload that stops moving, and
  updateRunning = true;         // an unmarked one looks stopped from the moment it starts
  PrinterSerial.end();

  return true;
}

bool firmwareChunk(uint8_t *data, const size_t len) {
  updateTouchedAt = millis();

  return Update.isRunning() && Update.write(data, len) == len;
}

// Returns what went wrong, or nothing at all when the image is in and sound. The reason
// is read before anything is aborted, because aborting overwrites it with "Aborted".
String firmwareEnd(const bool whole) {
  String trouble;

  if (!whole) {
    trouble = "the transfer stopped early";
    Update.abort();
  }
  else if (!Update.end(true)) {
    trouble = String(Update.errorString()) + " (wrote " + String(Update.progress()) + " bytes)";
    Update.abort();
  }

  updateRunning = false;
  if (trouble != "")
    PrinterSerial.begin(serialBauds[serialBaudIndex], SERIAL_8N1, PRINTER_RX_PIN, PRINTER_TX_PIN);

  if (trouble == "")
    bootGuardArm();

  logEvent(trouble == "" ? "firmware over telegram done" : "firmware over telegram failed: " + trouble);
  logFlush();

  return trouble;
}

void firmwareReboot() {
  ESPrestartRequired = true;
}

// "3 h 48 min", or "47 min" when there is no hour to speak of.
static String spell(const int32_t minutes) {
  if (minutes < 0)
    return "";
  if (minutes < 60)
    return String(minutes) + " min";

  return String(minutes / 60) + " h " + String(minutes % 60) + " min";
}

// The bot's task only carries messages. Orders are carried out here, in the loop, so
// nothing the printer owns is touched from two places at once.
void handleTelegram() {
  static String awaitingStop;     // Cancelling asks twice, and only the chat that asked

  String chat, line;
  while (telegramBot.nextCommand(chat, line)) {
    line.trim();
    const int space = line.indexOf(' ');
    String verb = space < 0 ? line : line.substring(0, space);
    const String rest = space < 0 ? "" : line.substring(space + 1);
    verb.toLowerCase();
    const int addressed = verb.indexOf('@');      // In a group a bot is called /status@thebot
    if (addressed > 0)
      verb = verb.substring(0, addressed);

    if (verb == "/start" || verb == "/help") {
      telegramBot.send(chat,
        String(getDeviceName()) + "\n\n"
        "/status - what the printer is doing\n"
        "/files - what is on the card\n"
        "/print <number> - start that file\n"
        "/stop - cancel the running print\n"
        "/nozzle <degrees> - set the hot end, 0 turns it off\n"
        "/bed <degrees> - set the bed, 0 turns it off\n"
        "/off - cut power at the socket\n\n"
        "Send a .gcode file and it goes on the card. Send a .bin and the module flashes "
        "itself with it and reboots, which it will not do while a print is running. "
        "Telegram only lets a bot fetch files up to 20 MB.");
    }
    else if (verb == "/status") {
      String text;

      if (isPrinting) {
        const int32_t elapsed = (millis() - printStartTime) / 60000;

        // The slicer's own figure is worth more than anything we can work out here.
        // Failing that, elapsed time divided by progress is only meaningful once the
        // print is properly under way: the first minutes are heating and a slow first
        // layer, and extrapolating from those said twenty-five hours on a four hour job.
        int32_t minutesLeft = -1;
        String left = "working it out";
        if (slicerMinutesLeft >= 0) {
          minutesLeft = slicerMinutesLeft;
          left = spell(minutesLeft);
        }
        else if (printCompletion >= 5 && elapsed >= 10) {
          minutesLeft = (int32_t)(elapsed / printCompletion * (100 - printCompletion));
          left = "about " + spell(minutesLeft);
        }

        text = (printPause ? "Paused" : "Printing") + String(" \u2014 ") + String(printCompletion, 1) + "%\n"
             + baseName(printingFile) + "\n\n"
             + "Layer      " + String(currentLayer) + " / " + String(totalLayers) + "\n"
             + "Elapsed    " + spell(elapsed) + "\n"
             + "Remaining  " + left + "\n";

        // Only worth saying once the clock has been set and there is a figure to add to it.
        const time_t now = time(NULL);
        if (minutesLeft >= 0 && now > 1700000000) {
          const time_t when = now + minutesLeft * 60;
          struct tm today, ending;
          char clock[24] = "";
          if (localtime_r(&now, &today) && localtime_r(&when, &ending))
            // A print that runs past midnight needs the day said as well, or the time
            // on its own reads as earlier today.
            strftime(clock, sizeof(clock), ending.tm_yday == today.tm_yday ? "%H:%M" : "%H:%M, %a %d %b", &ending);
          if (clock[0])
            text += "Finishes   " + String(clock) + "\n";
        }
      }
      else {
        text = getState() + "\n"
             + "Selected   " + (selectedFile == "" ? String("nothing") : baseName(selectedFile)) + "\n\n";
      }

      text += "Nozzle     " + toolTemperature[0].actual
            + (toolTemperature[0].target.toFloat() > 0 ? " / " + toolTemperature[0].target : String(" (off)")) + "\n"
            + "Bed        " + bedTemperature.actual
            + (bedTemperature.target.toFloat() > 0 ? " / " + bedTemperature.target : String(" (off)")) + "\n";

      const float watts = yandexHome.getPower();
      if (watts >= 0)
        text += "Power      " + String(watts, 0) + " W\n";

      telegramBot.send(chat, text);
    }
    else if (verb == "/nozzle" || verb == "/bed") {
      const bool nozzle = verb == "/nozzle";
      const int ceiling = nozzle ? NOZZLE_CEILING : BED_CEILING;
      const Temperature &reading = nozzle ? toolTemperature[0] : bedTemperature;
      const String what = nozzle ? "Nozzle" : "Bed";

      bool number = rest.length() > 0 && rest.length() <= 3;
      for (int i = 0; number && i < rest.length(); ++i)
        number = isDigit(rest[i]);

      if (!printerConnected)
        telegramBot.send(chat, "The printer is not answering.");
      else if (rest == "")
        telegramBot.send(chat, what + " is at " + reading.actual
                               + (reading.target.toFloat() > 0 ? ", heading for " + reading.target : ", switched off")
                               + ".\nSend " + verb + " <degrees> to change it, up to "
                               + String(ceiling) + ", or 0 to switch it off.");
      else if (!number || rest.toInt() > ceiling)
        telegramBot.send(chat, "Give a whole number from 0 to " + String(ceiling) + ".");
      else if (!commandQueue.push((nozzle ? "M104 S" : "M140 S") + String(rest.toInt())))
        telegramBot.send(chat, "The printer has a full queue right now. Try once more.");
      else {
        String said = rest.toInt() == 0 ? what + " switched off."
                                        : what + " set to " + String(rest.toInt()) + " \u00B0C.";
        if (isPrinting)
          said += "\nA print is running, so the file will set it back at the next change.";
        telegramBot.send(chat, said);
      }
    }
    else if (verb == "/files") {
      String listing;
      storedFile(0, listing);
      telegramBot.send(chat, listing == "" ? "The card is empty."
                                           : "On the card\n\n" + listing + "\nSend /print <number> to start one.");
    }
    else if (verb == "/print") {
      String listing;
      const String wanted = storedFile(rest.toInt(), listing);
      if (isPrinting)
        telegramBot.send(chat, "Already printing \u2014 " + baseName(printingFile));
      else if (!printerConnected)
        telegramBot.send(chat, "The printer is not answering.");
      else if (rest.toInt() <= 0 || wanted == "")
        telegramBot.send(chat, "No file with that number. Send /files to see them.");
      else {
        selectFile(wanted);
        startPrint = true;
        telegramBot.send(chat, "Starting \u2014 " + baseName(wanted) + "\nSend /status to follow it.");
      }
    }
    else if (verb == "/stop") {
      if (!isPrinting) {
        telegramBot.send(chat, "Nothing is printing.");
        awaitingStop = "";
      }
      else if (rest == "yes" || awaitingStop == chat) {
        cancelPrint = true;
        awaitingStop = "";
        telegramBot.send(chat, "Cancelling \u2014 " + baseName(printingFile));
      }
      else {
        awaitingStop = chat;
        telegramBot.send(chat, "Cancel " + baseName(printingFile) + " at " + String(printCompletion, 1)
                               + "%?\nSend /stop again to confirm.");
      }
    }
    else if (verb == "/off") {
      if (isPrinting)
        telegramBot.send(chat, "A print is running. Send /stop first.");
      else if (!yandexHome.hasToken() || yandexHome.getDeviceId() == "")
        telegramBot.send(chat, "No socket is set up.");
      else if (yandexHome.request(YandexHome::PowerOff))
        telegramBot.send(chat, "Switching the socket off. This module runs off the printer, so it goes "
                               "quiet too and cannot switch anything back on.");
      else
        telegramBot.send(chat, "The socket would not take the order.");
    }
    else
      telegramBot.send(chat, "I do not know that one. Send /help.");
  }
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

// The socket reports watts, never energy, so the area under those readings has to be
// accumulated here. A reading is a point measurement of a load that steps between a
// few watts and most of a kilowatt, so the trapezoid between two of them is an
// estimate and nothing more.
// Whether anything actually arrives on the receive wire. Reading the level says
// nothing on this board, which pulls the pin up harder than the chip pulls it down,
// so the wire is listened to instead: a line carrying data spends part of its time low.
String probeReceiveLine(const uint32_t milliseconds) {
  pinMode(PRINTER_RX_PIN, INPUT);
  delay(5);

  uint32_t samples = 0, low = 0, edges = 0;
  int last = digitalRead(PRINTER_RX_PIN);
  const uint32_t until = millis() + milliseconds;
  while ((int32_t)(millis() - until) < 0) {
    const int now = digitalRead(PRINTER_RX_PIN);
    if (now != last)
      ++edges;
    last = now;
    if (!now)
      ++low;
    ++samples;
  }

  String verdict;
  if (edges > 0)
    verdict = "data is arriving on this wire";
  else if (low == samples)
    verdict = "stuck low, shorted to ground or the far end holds it down";
  else if (low == 0)
    verdict = "idle high the whole time, nothing was sent down this wire";
  else
    verdict = "unreadable";

  return "{\"pin\":" + String(PRINTER_RX_PIN) +
         ",\"milliseconds\":" + String(milliseconds) +
         ",\"samples\":" + String(samples) +
         ",\"low\":" + String(low) +
         ",\"edges\":" + String(edges) +
         ",\"verdict\":\"" + verdict + "\"}";
}

void handleLineProbe() {
  if (!lineProbeRequested)
    return;
  lineProbeRequested = false;

  PrinterSerial.end();
  {
    const String measured = probeReceiveLine(LINE_PROBE_MS);
    strncpy(lineProbeResult, measured.c_str(), sizeof(lineProbeResult) - 1);
    lineProbeResult[sizeof(lineProbeResult) - 1] = 0;
  }
  PrinterSerial.begin(serialBauds[serialBaudIndex], SERIAL_8N1, PRINTER_RX_PIN, PRINTER_TX_PIN);
}

void handleEnergy() {
  const uint32_t at = yandexHome.getPowerAt();
  if (at != energyLastAt) {
    const float watts = yandexHome.getPower();
    if (watts >= 0) {
      if (energyLastWatt >= 0 && at > energyLastAt)
        energyWh += (energyLastWatt + watts) / 2 * (at - energyLastAt) / 3600000.0;
      energyLastWatt = watts;
    }
    energyLastAt = at;
  }

  if (!isPrinting || yandexHome.busy() || yandexHome.getDeviceId() == "")
    return;
  if ((int32_t)(millis() - energyPollAt) < 0)
    return;

  energyPollAt = millis() + ENERGY_POLL;
  yandexHome.request(YandexHome::PowerDraw);
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
    if (powerOffSawIdle) {          // The heaters were already off, so somebody turned them back on
      powerOffArmed = false;
      lcd(IpAddress2String(WiFi.localIP()));
    }
    powerOffReadySince = 0;         // Otherwise the end of the file is still draining out of the queue
    return;
  }
  powerOffSawIdle = true;

  if (!commandQueue.isEmpty() || nozzle >= POWEROFF_NOZZLE || bedTemperature.actual.toFloat() >= POWEROFF_BED) {
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
    logEvent("gave up switching the printer off after " + String(POWEROFF_ATTEMPTS) + " attempts");
    lcd("Power off failed");
    return;
  }

  ++powerOffAttempts;
  powerOffRetryAt = now + POWEROFF_RETRY;
  logEvent("switching the printer off, attempt " + String(powerOffAttempts) +
           ", nozzle " + toolTemperature[0].actual + ", bed " + bedTemperature.actual);
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

// The page is built in the server's task. Listing the card and asking it how full it is
// are card reads, and the print feeder is reading the same card from the main loop. Two
// readers meet inside the driver, the loop ends up waiting there instead of feeding the
// watchdog, and a running print dies. So the page asks for the card and does without it
// if the print has it.
// The page is built here, in a buffer that exists for the whole life of the module, and
// it is sent from that same buffer. Nothing it does reaches the heap. Every earlier
// version assembled it out of Strings, and a String that grows is a hole punched in the
// heap: three browsers asking for the page a few times a second slid the largest
// unbroken block down from twenty kilobytes to nine over four minutes, and the first
// thing that needed an unbroken block after that took the module down. One reader at a
// time, because there is one buffer; the second is told to come back.
#define PAGE_BUFFER_BYTES 8192
static char pageBuffer[PAGE_BUFFER_BYTES];
static size_t pageHeld;
static volatile bool pageBusy;

static void pageAdd(const char *text) {
  const size_t len = strlen(text);
  if (pageHeld + len < sizeof(pageBuffer)) {
    memcpy(pageBuffer + pageHeld, text, len);
    pageHeld += len;
  }
}

static inline void pageAdd(const String &text) {
  pageAdd(text.c_str());
}

// Fills pageBuffer and says how much of it was used.
size_t buildFallbackPage() {
  pageHeld = 0;

  // While a print is running the card is not shared at all. The feeder only holds it for
  // the length of one block read, so asking politely would succeed most of the time and
  // put a second reader in the driver anyway, which is the whole of that fault.
  const bool gotCard = !isPrinting && !somethingIsStreaming()
                       && cardMutex && xSemaphoreTakeRecursive(cardMutex, pdMS_TO_TICKS(250)) == pdTRUE;

  pageAdd("<h1>");
  pageAdd(getDeviceName());
  pageAdd("</h1><p>");
  pageAdd(getState());
  pageAdd(". Selected: <b>");
  pageAdd(htmlEscape(jobFilename()));
  pageAdd("</b></p>"
          "<p><button onclick=\"job('start')\">Print selected</button> "
          "<button onclick=\"job('cancel')\">Cancel print</button></p>"
          "<h2>Files on the printer\'s card</h2><table>");

  if (!printerConnected)
    pageAdd("<tr><td>the printer is not answering</td></tr>");
  else if (!printerCard.fresh())
    pageAdd("<tr><td>not read yet</td>"
            "<td><button onclick=\"post('/printer/refresh')\">Read</button></td></tr>");
  else if (!printerCard.count())
    pageAdd("<tr><td>nothing on it</td></tr>");
  else
    for (uint8_t i = 0; i < printerCard.count(); i++) {
      const CardIndex::Entry &entry = printerCard.at(i);
      const String picked = urlEncode(entry.dos);
      pageAdd("<tr><td>");
      pageAdd(htmlEscape(entry.name));
      pageAdd("</td><td align=\"right\">");
      pageAdd(String(entry.size / 1024));
      pageAdd(" KiB</td><td><button onclick=\"post('/printer/print?name=");
      pageAdd(picked);
      pageAdd("')\">Print</button> <button onclick=\"post('/printer/delete?name=");
      pageAdd(picked);
      pageAdd("')\">Delete</button></td></tr>");
    }

  pageAdd("</table><p>");
  pageAdd(cardSendRunning ? "sending " + cardSendTarget + ", " + String(cardSend.written / 1024) + " KiB so far"
                          : htmlEscape(cardSendReport));
  pageAdd("</p>"
          "<h2>Files on ");
  pageAdd(storageFS.getActiveFS());
  pageAdd("</h2><table>");

  if (gotCard) {
    pageAdd(fileListHtml());
    pageAdd("</table><p>");
    pageAdd(uint64ToString(storageFS.freeBytes() / 1048576));
    pageAdd(" MiB free of ");
    pageAdd(uint64ToString(storageFS.totalBytes() / 1048576));
    pageAdd(" MiB</p>");
    xSemaphoreGiveRecursive(cardMutex);
  }
  else {
    pageAdd(isPrinting ? "<tr><td>printing, the file list is back when it ends</td></tr>"
                       : "<tr><td>the card is busy, reload in a moment</td></tr>");
    pageAdd("</table>");
  }

  pageAdd("<h2>Upload</h2>"
          "<form enctype=\"multipart/form-data\" action=\"/api/files/local\" method=\"POST\">\n"
          "<input name=\"file\" type=\"file\" accept=\".gcode,.GCODE,.gco,.GCO\" required/><br/>\n"
          "<input type=\"checkbox\" name=\"print\" id=\"printImmediately\" value=\"true\">\n"
          "<label for=\"printImmediately\">Print immediately</label><br/>\n"
          "<input type=\"submit\" value=\"Upload\"/>\n"
          "</form>"
          "<h2>Who gets in</h2>");
  if (webAuthIsFresh())
    pageAdd("<p><b>This is still the password the module made up for itself. Change it.</b></p>");
  pageAdd("<form action=\"/auth/password\" method=\"POST\">"
          "<input name=\"pass\" type=\"password\" minlength=\"8\" required "
          "placeholder=\"new password, 8 or more\"/> "
          "<input type=\"submit\" value=\"Change\"/></form>"
          "<p>Key for the slicer, put it in Orca where it asks for an OctoPrint API key:<br/><code>");
  pageAdd(htmlEscape(webAuthKey()));
  pageAdd("</code></p>"
          "<p><button onclick=\"post('/auth/key')\">Make a new key</button> "
          "the old one stops working at once</p>"
          "<p><a href=\"/info\">Info</a> | <a href=\"/yandex\">Yandex socket</a></p>"
          "<hr><p>WirelessPrinting <a href=\"https://github.com/kelv1n9/WirelessPrinting/commit/");
  pageAdd(SKETCH_VERSION);
  pageAdd("\">");
  pageAdd(SKETCH_VERSION);
  pageAdd("</a></p>\n");
  #ifdef OTA_UPDATES
    pageAdd("<p>OTA Update Device: <a href=\"/update\">Click Here</a></p>");
  #endif
  pageAdd(R"HTML(<script>
function post(url) { fetch(url, {method: 'POST'}).then(function(r) { if (!r.ok) alert('Failed: ' + r.status); location.reload(); }); }
function job(command) { fetch('/api/job', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({command: command})}).then(function(r) { if (!r.ok) alert('Failed: ' + r.status); location.reload(); }); }
</script>)HTML");

  return pageHeld;
}

// Bulk receive with nothing but lwIP between the wire and the buffer, so the async
// server's share of the cost can be told apart from the radio's.
void handleRawBench() {
  if (isPrinting || !rawServer.hasClient())
    return;

  WiFiClient client = rawServer.available();
  client.setNoDelay(true);
  size_t got = 0;
  const uint32_t started = millis();
  uint32_t quiet = started, lastByteAt = started;
  while (client.connected() && millis() - quiet < 5000) {
    const int taken = client.read(uploadBuffer, UPLOAD_BUFFER_BYTES);
    if (taken > 0) {
      got += taken;
      quiet = lastByteAt = millis();
    }
    else
      vTaskDelay(1);
    esp_task_wdt_reset();
  }
  const uint32_t ms = lastByteAt - started;
  client.stop();
  snprintf(rawResult, sizeof(rawResult), "%u bytes in %u ms, %.1f KB/s",
           (unsigned)got, (unsigned)ms, got / (float)ms);
  logEvent(String("raw bench: ") + rawResult);
}

// The flash cannot be read while it is being written, so everything between these two
// calls has to be in RAM already: the attribute puts this function and its literals there.
static IRAM_ATTR int rewriteTableSector(const uint32_t *image, const int length) {
  spi_flash_disable_interrupts_caches_and_other_cpu();
  int trouble = esp_rom_spiflash_erase_sector(PARTITION_TABLE_AT / 4096);
  if (trouble == 0)
    trouble = esp_rom_spiflash_write(PARTITION_TABLE_AT, image, length);
  spi_flash_enable_interrupts_caches_and_other_cpu();

  return trouble;
}

// Returns what went wrong, or nothing at all when the table on the flash is the one
// this firmware was built against.
String writePartitionTable() {
  if (isPrinting || updateRunning)
    return "a print or an upload is running";

  // app0 starts at the same place in both tables, so an image running from it stays
  // where it is. app1 moves, and a module running from app1 would lose itself.
  const esp_partition_t *slot = esp_ota_get_running_partition();
  if (!slot || slot->address != 0x10000)
    return "this is running from " + String(slot ? slot->label : "?") +
           ", and only app0 has the same address in both tables";

  static uint8_t before[PARTITION_TABLE_USED] __attribute__((aligned(4)));
  static uint8_t after[PARTITION_TABLE_USED] __attribute__((aligned(4)));
  static uint8_t wanted[PARTITION_TABLE_USED] __attribute__((aligned(4)));
  memcpy(wanted, wantedPartitionTable, PARTITION_TABLE_USED);

  if (wanted[0] != 0xAA || wanted[1] != 0x50)
    return "what this firmware carries is not a partition table";

  if (spi_flash_read(PARTITION_TABLE_AT, before, PARTITION_TABLE_USED) != ESP_OK)
    return "could not read the table that is there now";
  if (before[0] != 0xAA || before[1] != 0x50)
    return "what is on the flash now is not a partition table either, not touching it";
  if (memcmp(before, wanted, PARTITION_TABLE_USED) == 0)
    return "";                 // Already what we want

  logEvent("rewriting the partition table, the firmware slots grow to 1920 KB");
  logFlush();                  // The card cannot be written while the flash is busy

  int trouble = rewriteTableSector((const uint32_t *)wanted, PARTITION_TABLE_USED);
  const bool readBack = spi_flash_read(PARTITION_TABLE_AT, after, PARTITION_TABLE_USED) == ESP_OK;

  if (trouble || !readBack || memcmp(after, wanted, PARTITION_TABLE_USED) != 0) {
    rewriteTableSector((const uint32_t *)before, PARTITION_TABLE_USED);   // Put back what was there
    logEvent("the new table did not take, the old one has been put back");
    logFlush();
    return "the new table did not read back as written, the old one is back in place";
  }

  logEvent("partition table written, rebooting into the wider slot");
  logFlush();

  return "";
}

// Every one of these must still sit where it was put at boot. One that has moved was
// reallocated under a reader in another task, which is the crash this is here to catch.
static int buffersStillHome() {
  return (selectedFile.c_str() == selectedFileRoom)
       + (printingFile.c_str() == printingFileRoom)
       + (lastCommandSent.c_str() == commandRoom)
       + (lastReceivedResponse.c_str() == responseRoom)
       + (fwMachineType.c_str() == machineRoom)
       + (bootProbeResult.c_str() == probeRoom);
}

void setup() {
  // Before any of them is ever assigned, while the heap is still one whole piece
  selectedFile.reserve(StorageFS::getMaxPathLength());
  printingFile.reserve(StorageFS::getMaxPathLength());
  lastCommandSent.reserve(MAX_COMMAND_ROOM);
  lastReceivedResponse.reserve(MAX_RESPONSE_LENGTH);
  fwMachineType.reserve(64);
  bootProbeResult.reserve(192);
  machineRoom = fwMachineType.c_str();
  probeRoom = bootProbeResult.c_str();
  commandRoom = lastCommandSent.c_str();
  responseRoom = lastReceivedResponse.c_str();
  selectedFileRoom = selectedFile.c_str();
  printingFileRoom = printingFile.c_str();

  bootGuardBegin();                     // Before anything that can crash gets a chance to
  witnessBegin();                       // Before the ring is read or written
  tlsArenaBegin();                      // Before mbedtls is asked for anything
  logMutex = xSemaphoreCreateMutex();   // Before anything can log
  consoleMutex = xSemaphoreCreateMutex();
  tlsGate = xSemaphoreCreateMutex();    // Before anything can reach out over TLS
  cardMutex = xSemaphoreCreateRecursiveMutex(); // Before anything touches the card
  bootProbeResult = probeReceiveLine(BOOT_PROBE_MS);
  PrinterSerial.setRxBufferSize(PRINTER_SERIAL_RX_BUFFER);

  commandQueue.begin();
  linkBegin();                          // Before a single byte can go either way
  storageFS.begin();
  yandexHome.begin();
  telegramBot.begin();
  storageFS.mkdir("/ui");

  const esp_reset_reason_t why = esp_reset_reason();

  const esp_partition_t *slot = esp_ota_get_running_partition();
  logEvent(String("boot ") + SKETCH_VERSION + ", after " + resetReason() +
           ", free heap " + String(ESP.getFreeHeap()) +
           ", block " + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) +
           ", slot " + String(slot ? slot->label : "?"));
  (void)why;

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
  // The module hangs off the printer's 5V rail, so nothing here is worth trading for power.
  WiFi.mode(WIFI_STA);                        // The manager leaves its access point up otherwise
  WiFi.setSleep(false);                       // Modem sleep costs beacons and latency
  esp_wifi_set_ps(WIFI_PS_NONE);              // The same thing again, straight at the driver
  esp_wifi_set_max_tx_power(80);              // 20 dBm, as high as the radio goes
  setCpuFrequencyMhz(240);
  wifiDownSince = millis();
  configTzTime(LOCAL_TIMEZONE, "pool.ntp.org", "time.google.com");

  telnetServer.begin();
  telnetServer.setNoDelay(true);
  rawServer.begin();            // Bulk receive with nothing but lwIP in the way, for the bench
  rawServer.setNoDelay(true);

  initSelectedFile();

  // Closes every route before the server answers one. It has to stay below the WiFi
  // call above: esp_random only draws on real entropy once the radio is running, and
  // the password this invents on a fresh module is drawn from it. Moved any earlier,
  // every module would invent the same one.
  webAuthBegin();

  preferences.begin("wirelessprint", true);
  autoPowerOff = preferences.getBool("autooff", false);
  energyTariff = preferences.getFloat("tariff", ENERGY_TARIFF);
  filamentDefaultPrice = preferences.getFloat("kilo", FILAMENT_PRICE);
  filamentPrice = filamentDefaultPrice;
  preferences.end();

  // Registered before every other handler, and handles nothing: _attachHandler walks the
  // list in order and asks each one, so this is the one place every request goes through.
  class WitnessSpy : public AsyncWebHandler {
    public:
      bool canHandle(AsyncWebServerRequest *request) override {
        const uint32_t at = witnessArrived(request->url().c_str());
        request->onDisconnect([at]() { witnessLeft(at); });
        return false;
      }
  };
  server.addHandler(new WitnessSpy());

  server.onNotFound([](AsyncWebServerRequest * request) {
    if (request->url() == "/" || request->url() == "/index.html") {
      if (pageBusy) {
        request->send(503, "text/plain", "the page is being built, reload in a moment");
        return;
      }

      pageBusy = true;
      const size_t held = buildFallbackPage();
      AsyncWebServerResponse *response = request->beginResponse("text/html; charset=utf-8", held,
                                         [held](uint8_t *buffer, size_t room, size_t index) -> size_t {
        const size_t left = held - index;
        const size_t take = room < left ? room : left;
        memcpy(buffer, pageBuffer + index, take);
        return take;
      });
      const uint32_t mine = witnessHere();      // onDisconnect keeps one function, so carry the spy's
      request->onDisconnect([mine]() { pageBusy = false; witnessLeft(mine); });
      request->send(response);
      return;
    }
    telnetSend("404 | Page '" + request->url() + "' not found");
    request->send(404, "text/html; charset=utf-8", "<h1>Page not found!</h1>");
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
                     "<p>Automatic switch off is <b>" + String(autoPowerOff ? "ON" : "OFF") + "</b> "
                     "<button onclick=\"post('/yandex/auto?on=" + String(autoPowerOff ? "0" : "1") + "')\">"
                     + String(autoPowerOff ? "Disable" : "Enable") + "</button></p>"
                     "<p>"
                     "<button onclick=\"post('/yandex/abort')\">Cancel the pending switch off</button></p>"
                     "<h2>Sockets</h2>"
                     "<p><button onclick=\"post('/yandex/devices')\">Load from Yandex</button> "
                     "<button onclick=\"post('/yandex/test')\">Test: switch on</button> "
                     "<button onclick=\"if (confirm('This cuts power to the printer and to this module. Continue?')) post('/yandex/off')\">Switch off now</button></p>"
                     "<form method=\"POST\" action=\"/yandex/tariff\">"
                     "<p>Electricity costs <input type=\"number\" step=\"0.001\" min=\"0\" name=\"tariff\" "
                     "value=\"" + String(energyTariff, 3) + "\" size=\"8\"> AZN per kWh, "
                     "filament costs <input type=\"number\" step=\"0.01\" min=\"0\" name=\"kilo\" "
                     "value=\"" + String(filamentDefaultPrice, 2) + "\" size=\"8\"> AZN per kg "
                     "<button type=\"submit\">Save</button></p>"
                     "<p>A file that carries its own filament price uses that instead.</p></form>"
                     "<p><button onclick=\"post('/restart')\">Restart the module</button></p>"
                     "<table>" + list + "</table>"
                     "<p><a href=\"/\">Back</a></p>";
    message += R"HTML(<script>
function post(url) { fetch(url, {method: 'POST'}).then(function(r) { if (!r.ok) alert('Failed: ' + r.status); setTimeout(function(){ location.reload(); }, 2500); }); }
</script>)HTML";
    request->send(200, "text/html; charset=utf-8", message);
  });

  server.on("/telegram", HTTP_GET, [](AsyncWebServerRequest * request) {
    String rows;
    for (int which = 0; which < 2; which++) {
      const String list = which == 0 ? telegramBot.getChats() : telegramBot.getSeen();
      int at = 0;
      while (at < (int)list.length()) {
        const int comma = list.indexOf(',', at);
        const String one = comma < 0 ? list.substring(at) : list.substring(at, comma);
        if (one != "")
          rows += "<tr><td><code>" + htmlEscape(one) + "</code></td>"
                  "<td>" + String(which == 0 ? "allowed" : "knocked, not allowed") + "</td>"
                  "<td>" + (which == 0
                    ? "<button onclick=\"post('/telegram/deny?chat=" + urlEncode(one) + "')\">Remove</button>"
                    : "<button onclick=\"post('/telegram/allow?chat=" + urlEncode(one) + "')\">Allow</button>") +
                  "</td></tr>";
        if (comma < 0)
          break;
        at = comma + 1;
      }
    }
    if (rows == "")
      rows = "<tr><td colspan=\"3\"><i>Nobody yet. Send the bot a message and reload this page.</i></td></tr>";

    String message = "<h1>Telegram</h1>"
                     "<p>Status: <b>" + htmlEscape(telegramBot.getStatus()) + "</b></p>"
                     "<p>Token: <b>" + String(telegramBot.hasToken() ? "stored" : "not set") + "</b></p>"
                     "<form method=\"POST\" action=\"/telegram/token\">"
                     "<input name=\"token\" type=\"password\" size=\"60\" placeholder=\"Bot token from BotFather\" required/> "
                     "<input type=\"submit\" value=\"Store token\"/>"
                     "</form>"
                     "<p>The token is written to NVS and never shown again.</p>"
                     "<h2>Who may command the printer</h2>"
                     "<p>Only the chats listed as allowed are answered. Anyone else is ignored in silence, "
                     "and their id turns up here so you can let them in.</p>"
                     "<table>" + rows + "</table>"
                     "<p><a href=\"/\">Back</a></p>";
    message += R"HTML(<script>
function post(url) { fetch(url, {method: 'POST'}).then(function(r) { if (!r.ok) alert('Failed: ' + r.status); setTimeout(function(){ location.reload(); }, 500); }); }
</script>)HTML";
    request->send(200, "text/html; charset=utf-8", message);
  });

  server.on("/telegram/token", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("token", true)) {
      request->send(400, "text/plain", "token is required");
      return;
    }
    telegramBot.setToken(request->getParam("token", true)->value());
    request->redirect("/telegram");
  });

  server.on("/telegram/allow", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("chat")) {
      request->send(400, "text/plain", "chat is required");
      return;
    }
    telegramBot.allow(request->getParam("chat")->value());
    request->send(204, "text/plain", "");
  });

  server.on("/telegram/deny", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("chat")) {
      request->send(400, "text/plain", "chat is required");
      return;
    }
    telegramBot.deny(request->getParam("chat")->value());
    request->send(204, "text/plain", "");
  });

  // The image for the printer, not for this module. Kept apart from the module's own
  // /update on purpose: sending one to the other bricks the wrong board.
  server.on("/printer/firmware", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (uploadFailure != "") {
      request->send(409, "text/plain", uploadFailure + "\n");
      uploadFailure = "";
      return;
    }
    request->send(200, "text/plain", "the image is on this module, POST /printer/flash to send it\n");
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data,
        size_t len, bool final) {
    static FileWrapper image;

    if (!index) {
      uploadFailure = "";
      const char *why = printerFlashWhyNot();
      if (why)
        uploadFailure = why;
      else if (!filename.endsWith(".bin"))
        uploadFailure = "the printer takes a .bin image";
      else if (request->contentLength() >= storageFS.freeBytes())
        uploadFailure = "not enough free space";
      else {
        storageFS.remove(PRINTER_FIRMWARE_PATH);
        image = storageFS.open(PRINTER_FIRMWARE_PATH, "w");
        if (!image)
          uploadFailure = "cannot create file";
      }
    }

    if (uploadFailure == "" && image && image.write(data, len) != len)
      uploadFailure = "write error, card full";

    if (final) {
      if (image)
        image.close();
      if (uploadFailure != "")
        storageFS.remove(PRINTER_FIRMWARE_PATH);
      else
        logEvent("took an image for the printer, " + String(index + len) + " bytes");
    }
  });

  server.on("/printer/flash", HTTP_POST, [](AsyncWebServerRequest * request) {
    const char *why = printerFlashWhyNot();
    if (why) {
      request->send(409, String("text/plain"), String(why) + "\n");
      return;
    }
    const String target = request->hasParam("name", true)
                        ? sanitizeFilename(request->getParam("name", true)->value())
                        : String(PRINTER_FIRMWARE_NAME);
    if (!printerFlashBegin(target)) {
      request->send(409, "text/plain", "no image on this module, POST it to /printer/firmware first\n");
      return;
    }
    request->send(202, "text/plain", "sending the image, watch the journal\n");
  });

  server.on("/printer/flash/status", HTTP_GET, [](AsyncWebServerRequest * request) {
    String out = "{\"running\":";
    out += printerFlash ? "true" : "false";
    if (printerFlash) {
      out += ",\"phase\":" + String((int)printerFlash->phase);
      out += ",\"written\":" + String(printerFlash->written);
      out += ",\"total\":" + String(printerFlash->total);
      out += ",\"packets\":" + String(printerFlash->packets);
      out += ",\"repeats\":" + String(printerFlash->resends);
    }
    out += ",\"last\":\"" + printerFlashReport + "\"}";
    request->send(200, "application/json", out);
  });

  server.on("/auth/password", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("pass", true)) {
      request->send(400, "text/plain", "pass is required");
      return;
    }
    const String chosen = request->getParam("pass", true)->value();
    if (chosen.length() < 8) {
      request->send(400, "text/plain", "eight characters at the very least\n");
      return;
    }
    webAuthSetPassword(chosen);
    lcd("Web password set");
    request->send(200, "text/plain", "changed, log in again\n");
  });

  server.on("/auth/key", HTTP_POST, [](AsyncWebServerRequest * request) {
    webAuthNewKey();
    request->send(200, "text/plain", "a new key is on the front page\n");
  });

  server.on("/yandex/token", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("token", true)) {
      request->send(400, "text/plain", "token is required");
      return;
    }
    yandexHome.setToken(request->getParam("token", true)->value());
    request->redirect("/yandex");
  });

  server.on("/yandex/tariff", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("tariff", true)) {
      request->send(400, "text/plain", "tariff is required");
      return;
    }
    const float value = request->getParam("tariff", true)->value().toFloat();
    const float kilo = request->hasParam("kilo", true) ? request->getParam("kilo", true)->value().toFloat()
                                                       : filamentDefaultPrice;
    preferences.begin("wirelessprint", false);
    if (value >= 0) {
      energyTariff = value;
      preferences.putFloat("tariff", value);
    }
    if (kilo >= 0) {
      filamentDefaultPrice = kilo;
      preferences.putFloat("kilo", kilo);
    }
    preferences.end();
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

  server.on("/restart", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (isPrinting) {
      request->send(409, "text/plain", "a print is running");
      return;
    }
    request->send(200, "text/plain", "restarting");
    ESPrestartRequired = true;
  });

  server.on("/yandex/off", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (isPrinting) {
      request->send(409, "text/plain", "a print is running, cancel it first");
      return;
    }
    request->send(yandexHome.request(YandexHome::PowerOff) ? 204 : 409, "text/plain", "");
  });

  server.on("/api/beep", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (request->hasParam("on"))
      startBeep = request->getParam("on")->value() != "0";
    request->send(200, "text/plain", startBeep ? "on\n" : "off\n");
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

  // Puts a file on the printer's own card, and optionally tells the printer to print it.
  // From then on the print belongs to the printer and survives anything that happens here.
  server.on("/printer/send", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("name")) {
      request->send(400, "text/plain", "name is required");
      return;
    }
    const char *why = cardSendWhyNot();
    if (why) {
      request->send(409, "text/plain", String(why) + "\n");
      return;
    }

    const String path = "/" + sanitizeFilename(request->getParam("name")->value());
    if (!storageFS.exists(path)) {
      request->send(404, "text/plain", "no such file on this module\n");
      return;
    }
    // Keeping the original is the exception, because the point is to free this card
    const bool keep = paramIsTrue(request, "keep");
    if (!cardSendBegin(path, paramIsTrue(request, "print"), !keep)) {
      request->send(409, "text/plain", "could not start\n");
      return;
    }
    request->send(200, "text/plain", "sending as " + cardSendTarget + "\n");
  });

  // What the printer itself holds. Served from the copy in memory, so opening the page
  // costs nothing on the wire even in the middle of a print.
  server.on("/printer/files", HTTP_GET, [](AsyncWebServerRequest * request) {
    String out = "{\"fresh\":";
    out += printerCard.fresh() ? "true" : "false";
    out += ",\"busy\":";
    out += printerCard.busy() ? "true" : "false";
    out += ",\"missed\":" + String(printerCard.overflowed());
    out += ",\"files\":[";
    for (uint8_t i = 0; i < printerCard.count(); i++) {
      const CardIndex::Entry &e = printerCard.at(i);
      if (i) out += ",";
      out += "{\"dos\":\"" + jsonEscape(e.dos) + "\",\"name\":\"" + jsonEscape(e.name)
           + "\",\"size\":" + String(e.size) + "}";
    }
    out += "]}";
    request->send(200, "application/json", out);
  });

  server.on("/printer/refresh", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!printerConnected) {
      request->send(409, "text/plain", "the printer is not answering\n");
      return;
    }
    if (cardSendRunning) {
      request->send(409, "text/plain", "a file is going over right now\n");
      return;
    }
    askCardList();
    request->send(200, "text/plain", "asked\n");
  });

  server.on("/printer/print", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("name")) {
      request->send(400, "text/plain", "name is required");
      return;
    }
    if (isPrinting || externalPrinting || cardSendRunning) {
      request->send(409, "text/plain", "busy\n");
      return;
    }
    const String name = sanitizeFilename(request->getParam("name")->value());
    if (printerCard.fresh() && !printerCard.holds(name.c_str())) {
      request->send(404, "text/plain", "the printer's card does not hold that\n");
      return;
    }
    commandQueue.push("M23 " + name);
    commandQueue.push("M24");
    commandQueue.push(SDREPORT_COMMAND + String(SDREPORT_INTERVAL));
    logEvent("asked the printer to print " + name + " from its own card");
    request->send(200, "text/plain", "started " + name + "\n");
  });

  server.on("/printer/delete", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("name")) {
      request->send(400, "text/plain", "name is required");
      return;
    }
    if (isPrinting || externalPrinting || cardSendRunning) {
      request->send(409, "text/plain", "nothing is deleted while a print or a transfer runs\n");
      return;
    }
    const String name = sanitizeFilename(request->getParam("name")->value());
    commandQueue.push("M30 " + name);
    printerCard.stale();
    printerCard.expect();
    commandQueue.push("M20 L");
    logEvent("asked the printer to delete " + name + " from its own card");
    request->send(200, "text/plain", "deleted " + name + "\n");
  });

  server.on("/printer/cancel", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (cardSendRunning) {
      cardSend.stop();                 // The loop owns the file and the reader
      request->send(200, "text/plain", "transfer stopping\n");
      return;
    }
    if (!externalPrinting) {
      request->send(409, "text/plain", "the printer is not printing from its own card\n");
      return;
    }
    commandQueue.push("M524");
    logEvent("asked the printer to stop the print from its own card");
    request->send(200, "text/plain", "cancelled\n");
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

  server.on("/api/rename", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (isPrinting) {
      request->send(409, "text/plain", "a print is running, the card is busy");
      return;
    }
    if (!request->hasParam("from", true) || !request->hasParam("to", true)) {
      request->send(400, "text/plain", "from and to are required");
      return;
    }

    const String from = "/" + sanitizeFilename(request->getParam("from", true)->value());
    String wanted = sanitizeFilename(request->getParam("to", true)->value());
    wanted.trim();
    if (wanted == "" || wanted == ".") {
      request->send(400, "text/plain", "the new name is empty");
      return;
    }

    const int dot = from.lastIndexOf('.');                  // Keep the extension, a gcode file has to stay one
    if (dot != -1 && !wanted.endsWith(from.substring(dot)))
      wanted += from.substring(dot);
    const String to = "/" + wanted;

    if (!storageFS.exists(from)) {
      request->send(404, "text/plain", "no such file");
      return;
    }
    if (to == from) {
      request->send(204, "text/plain", "");
      return;
    }
    if (storageFS.exists(to)) {
      request->send(409, "text/plain", "that name is taken");
      return;
    }
    if (!storageFS.rename(from, to)) {
      request->send(500, "text/plain", "the card refused to rename it");
      return;
    }

    if (from == selectedFile)
      selectFile(to);
    request->send(204, "text/plain", "");
  });

  // Info page
  server.on("/partitions", HTTP_POST, [](AsyncWebServerRequest * request) {
    const String trouble = writePartitionTable();
    if (trouble != "") {
      request->send(409, "text/plain", trouble + "\n");
      return;
    }
    request->send(200, "text/plain", "the table on the flash is the one this firmware was built against\n");
    ESPrestartRequired = true;
  });

  server.on("/api/link", HTTP_ANY, [](AsyncWebServerRequest * request) {
    if (request->method() == HTTP_POST) {
      if (request->hasParam("bytes", true))
        printerLink.budgetBytes = constrain(request->getParam("bytes", true)->value().toInt(), 24, 1400);
      if (request->hasParam("lines", true))
        printerLink.slotCeiling = constrain(request->getParam("lines", true)->value().toInt(), 1, LINK_WINDOW_SLOTS - 1);
      if (request->hasParam("reset", true)) {
        printerLink.resends = printerLink.errors = printerLink.acked = printerLink.queued = 0;
        printerLink.resyncs = printerLink.refused = printerLink.stale = printerLink.timeouts = 0;
      }
    }
    request->send(200, "text/plain", linkReport() + "\n");
  });

  server.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", "ok");
  });

  server.on("/api/diag", HTTP_GET, [](AsyncWebServerRequest * request) {
    const esp_partition_t *slot = esp_ota_get_running_partition();
    multi_heap_info_t shape;
    heap_caps_get_info(&shape, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    request->send(200, "text/plain",
                  "version       " + String(SKETCH_VERSION) + "\n"
                  "slot          " + String(slot ? slot->label : "?") +
                  " at 0x" + String(slot ? slot->address : 0, HEX) +
                  " size " + String(slot ? slot->size : 0) + "\n"
                  "boot reason   " + String(resetReason()) + "\n"
                  "uptime        " + String(millis() / 1000) + " s\n"
                  "heap free     " + String(ESP.getFreeHeap()) + "\n"
                  "heap block    " + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) + "\n"
                  "heap low      " + String(ESP.getMinFreeHeap()) + "\n"
                  "heap shape    " + String(shape.free_blocks) + " free pieces, "
                  + String(shape.allocated_blocks) + " handed out, lowest free ever "
                  + String(shape.minimum_free_bytes) + "\n"
                  "bot stack low " + String(telegramBot.stackLow()) + " of " + String(16384) + "\n"
                  "socket stack " + String(yandexHome.stackLow()) + " of " + String(10240) + "\n"
                  "upload last   " + uploadReport + "\n"
                  "paths moved   " + String(pathBufferMoved) + "\n"
                  "buffers home  " + String(buffersStillHome()) + " of 6\n"
                  "raw bench     " + String(rawResult) + "\n"
                  "streaming     " + String(somethingIsStreaming() ? "warm" : "cold") + ", tls gate "
                  + String(tlsGate ? uxSemaphoreGetCount(tlsGate) : 99) + ", largest block "
                  + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) + "\n"
                  "download last " + downloadReport + "\n"
                  "tls arena    " + tlsArenaReport() + "\n"
                  "card send     " + cardSendReport + "\n"
                  "boot guard    " + bootGuardReport() + "\n"
                  "link          " + linkReport() + "\n"
                  "server task   stack left " + String(asyncTcpStackLow) + " of 16384, queue peak "
                  + String(asyncTcpQueueHigh) + " of 32, events dropped " + String(asyncTcpDropped) + "\n");
  });

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
      const bool failed = !updateSucceeded;
      logEvent(failed ? "firmware upload failed: " + String(Update.errorString()) : "firmware upload done");
      if (updateRunning) {        // The loop stands aside while this is set, and it is the loop that reboots
        updateRunning = false;
        if (failed)
          PrinterSerial.begin(serialBauds[serialBaudIndex], SERIAL_8N1, PRINTER_RX_PIN, PRINTER_TX_PIN);
      }
      AsyncWebServerResponse *response = request->beginResponse(failed ? 500 : 200, "text/plain",
                                                                failed ? String(Update.errorString()) : String("OK, rebooting"));
      response->addHeader("Connection", "close");
      request->send(response);
      if (!failed)
        bootGuardArm();            // The new image has to earn its keep before it is kept
      ESPrestartRequired = !failed;
    },
    [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!index) {
        updateSucceeded = false;
        if (Update.isRunning())
          Update.abort();
        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
          return;
        lcd("Updating...");
        logEvent("firmware upload started");
        logFlush();
        updateRunning = true;      // Writing the flash stalls everything that reads from it
        PrinterSerial.end();
      }

      updateTouchedAt = millis();

      if (!Update.isRunning())
        return;

      if (Update.write(data, len) != len)
        Update.abort();
      else if (final)
        updateSucceeded = Update.end(true);
    });
  #endif

  // Download page
  server.on("/thumbnail", HTTP_GET, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("name")) {
      request->send(400, "text/plain", "name is required");
      return;
    }

    if (isPrinting) {
      request->send(503, "text/plain", "a print is running, the card is busy");
      return;
    }

    // Decoding one of these needs room three times over, and a browser asks for every
    // file at once. Saying no costs a grey square; running out costs a reboot.
    if (ESP.getFreeHeap() < 80000) {
      request->send(503, "text/plain", "busy");   // The page waits and asks again
      return;
    }

    // The card is read here for as long as the header takes, so it has to be claimed
    // for that whole time, not quietly shared with the journal and the feeder.
    if (!cardMutex || xSemaphoreTakeRecursive(cardMutex, pdMS_TO_TICKS(300)) != pdTRUE) {
      request->send(503, "text/plain", "the card is busy");
      return;
    }

    FileWrapper file = storageFS.open("/" + sanitizeFilename(request->getParam("name")->value()));
    if (!file) {
      xSemaphoreGiveRecursive(cardMutex);
      request->send(404, "text/plain", "no such file");
      return;
    }

    // Read in blocks and cut the lines here. Asking the card for one line at a time
    // means asking it for one byte at a time, and a header of sixty kilobytes then
    // takes five seconds, which is long enough to hold up every other request the
    // page makes at the same time.
    static char block[1024] __attribute__((aligned(32)));
    String encoded, line;
    bool inside = false, finished = false;
    unsigned int scanned = 0;

    while (!finished && scanned < THUMBNAIL_SCAN_BYTES && encoded.length() < THUMBNAIL_MAX_BYTES) {
      const int taken = file.read((uint8_t *)block, sizeof(block));
      if (taken <= 0)
        break;
      scanned += taken;

      for (int at = 0; at < taken; at++) {
        if (block[at] != '\n') {
          if (block[at] != '\r')
            line += block[at];
          continue;
        }

        line.trim();
        if (line.startsWith(";")) {
          if (!inside)
            inside = line.indexOf("thumbnail begin") != -1;
          else if (line.indexOf("thumbnail end") != -1) {
            finished = true;
            break;
          }
          else {
            line = line.substring(1);
            line.trim();
            encoded += line;
          }
        }
        line = "";
      }
    }
    file.close();
    xSemaphoreGiveRecursive(cardMutex);          // Done with the card, the rest is memory

    size_t length = 0;
    if (encoded == "" || mbedtls_base64_decode(NULL, 0, &length, (const unsigned char *)encoded.c_str(), encoded.length()) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || length == 0) {
      request->send(404, "text/plain", "no preview in this file");
      return;
    }

    auto picture = std::shared_ptr<uint8_t>((uint8_t *)malloc(length), free);
    if (!picture) {
      request->send(507, "text/plain", "out of memory");
      return;
    }
    if (mbedtls_base64_decode(picture.get(), length, &length, (const unsigned char *)encoded.c_str(), encoded.length()) != 0) {
      request->send(404, "text/plain", "broken preview");
      return;
    }
    encoded = String();

    const size_t total = length;
    AsyncWebServerResponse *response = request->beginResponse("image/png", total,
                                       [picture, total](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      const size_t left = total - index;
      const size_t take = maxLen < left ? maxLen : left;
      memcpy(buffer, picture.get() + index, take);
      return take;
    });
    response->addHeader("Cache-Control", "max-age=86400");
    request->send(response);
  });

  // Two ceiling tests, so a slow transfer can be blamed on the right half of the machine.
  // /bench/net sends made up bytes and never touches the card; /bench/card works the card
  // and never touches the network until it reports.
  server.on("/bench/ota", HTTP_GET, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("url")) {
      request->send(400, "text/plain", "url is required");
      return;
    }
    if (isPrinting) {
      request->send(409, "text/plain", "a print is running");
      return;
    }
    telegramBot.probe(request->getParam("url")->value());
    request->send(202, "text/plain", "walking it, watch the journal\n");
  });

  server.on("/bench/https", HTTP_GET, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("url")) {
      request->send(200, "text/plain", telegramBot.benchResult() + "\n");
      return;
    }
    if (isPrinting) {
      request->send(409, "text/plain", "a print is running");
      return;
    }
    const size_t chunk = request->hasParam("chunk") ? request->getParam("chunk")->value().toInt() : 0;
    telegramBot.fetchBench(request->getParam("url")->value(), chunk, request->hasParam("old"));
    request->send(202, "text/plain", "running it, ask again without url for the answer\n");
  });

  server.on("/bench/telegram", HTTP_GET, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("cmd")) {
      const String answer = telegramBot.benchAnswer();
      request->send(200, "text/plain; charset=utf-8", answer == "" ? "nothing yet\n" : answer + "\n");
      return;
    }
    telegramBot.benchSend(request->getParam("cmd")->value());
    request->send(202, "text/plain", "taken, ask again without cmd for the answer\n");
  });

  server.on("/bench/power", HTTP_GET, [](AsyncWebServerRequest * request) {
    if (request->hasParam("set"))
      esp_wifi_set_max_tx_power(request->getParam("set")->value().toInt());
    int8_t now = 0;
    esp_wifi_get_max_tx_power(&now);
    request->send(200, "text/plain", String(now) + " quarter dBm, " + String(now / 4.0, 2) + " dBm\n");
  });

  server.on("/bench/air", HTTP_GET, [](AsyncWebServerRequest * request) {
    wifi_ap_record_t ours;
    String out = "ours: rssi " + String(WiFi.RSSI()) + " channel " + String(WiFi.channel());
    if (esp_wifi_sta_get_ap_info(&ours) == ESP_OK)
      out += " phy " + String(ours.phy_11n ? "11n" : (ours.phy_11g ? "11g" : "11b"))
           + (ours.second ? " 40MHz" : " 20MHz");
    out += "\n\nneighbours:\n";

    const int found = WiFi.scanNetworks();
    for (int i = 0; i < found; i++)
      out += String(WiFi.channel(i)) + "\t" + String(WiFi.RSSI(i)) + "\t" + WiFi.SSID(i) + "\n";
    WiFi.scanDelete();
    request->send(200, "text/plain; charset=utf-8", out);
  });

  server.on("/bench/sink", HTTP_POST, [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", sinkReport + "\n");
  }, NULL, [](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    static uint32_t calls, maxGap, lastAt, gapsOver20;
    const uint32_t now = millis();
    if (!index) {
      sinkStartedAt = now;
      calls = maxGap = gapsOver20 = 0;
      sinkLanding = true;
    }
    else {
      const uint32_t gap = now - lastAt;
      if (gap > maxGap)
        maxGap = gap;
      if (gap >= 20)
        gapsOver20++;
    }
    lastAt = lastStreamAt = now;
    calls++;
    if (index + len >= total) {
      sinkLanding = false;
      const uint32_t ms = now - sinkStartedAt;
      sinkReport = String(total) + " bytes in " + String(ms) + " ms, "
                   + String(ms ? total / (float)ms : 0, 1) + " KB/s, " + String(calls) + " calls of "
                   + String(calls ? total / calls : 0) + " bytes, longest gap " + String(maxGap)
                   + " ms, gaps over 20 ms: " + String(gapsOver20);
    }
  });

  server.on("/bench/net", HTTP_GET, [](AsyncWebServerRequest * request) {
    size_t total = 8UL * 1024 * 1024;
    if (request->hasParam("mb"))
      total = request->getParam("mb")->value().toInt() * 1024UL * 1024UL;

    request->send(request->beginResponse("application/octet-stream", total, [total](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      lastStreamAt = millis();

      if (index && sendableNow() < maxLen)
        return RESPONSE_TRY_AGAIN;

      const size_t bytes = min(total - index, maxLen);
      memset(buffer, 'G', bytes);
      return bytes;
    }));
  });

  server.on("/bench/card", HTTP_GET, [](AsyncWebServerRequest * request) {
    if (isPrinting || !storageFS.isActive()) {
      request->send(409, "text/plain", "the card is busy");
      return;
    }
    size_t total = 8UL * 1024 * 1024;
    if (request->hasParam("mb"))
      total = request->getParam("mb")->value().toInt() * 1024UL * 1024UL;

    // Borrowed for the run and handed straight back: holding the biggest block on the
    // heap for the rest of the uptime is what the encryption then cannot have.
    size_t room = BENCH_BURST_BYTES;
    uint8_t *burst = NULL;
    while (!burst && room >= 4096) {
      burst = (uint8_t *)malloc(room);
      if (!burst)
        room /= 2;
    }
    if (!burst) {
      request->send(500, "text/plain", "no room for the buffer");
      return;
    }
    size_t chunk = UPLOAD_BUFFER_BYTES;
    if (request->hasParam("chunk"))
      chunk = constrain(request->getParam("chunk")->value().toInt(), 512, (int)room);

    memset(burst, 'G', room);
    FileWrapper file = storageFS.open("/bench.tmp", "w");
    if (!file) {
      free(burst);
      request->send(500, "text/plain", "cannot create the file");
      return;
    }
    uint32_t started = millis();
    size_t written = 0;
    while (written < total && file.write(burst, chunk) == chunk) {
      written += chunk;
      if ((written & 0xFFFF) == 0)        // Card work without a breath trips the watchdog
        vTaskDelay(1);
    }
    file.close();
    const uint32_t writeMs = millis() - started;

    started = millis();
    file = storageFS.open("/bench.tmp");
    size_t got = 0;
    int bytes;
    while ((bytes = file.read(burst, chunk)) > 0) {
      got += bytes;
      if ((got & 0xFFFF) == 0)
        vTaskDelay(1);
    }
    file.close();
    const uint32_t readMs = millis() - started;
    storageFS.remove("/bench.tmp");
    free(burst);

    request->send(200, "text/plain",
                  "buffer " + String(room) + ", chunk " + String(chunk) + ", wrote " + String(written) + " bytes in " + String(writeMs) + " ms, "
                  + String(written / (float)writeMs, 1) + " KB/s\n"
                  "read " + String(got) + " bytes in " + String(readMs) + " ms, "
                  + String(got / (float)readMs, 1) + " KB/s\n");
  });

  server.on("/download", HTTP_GET, [](AsyncWebServerRequest * request) {
    String path = selectedFile;
    if (request->hasParam("name")) {
      const String name = sanitizeFilename(request->getParam("name")->value());
      path = (name == "events.log" || name == "events.1.log") ? "/log/" + name : "/" + name;
      if (path.startsWith("/log/"))
        logFlush();
    }

    if (isPrinting) {
      request->send(503, "text/plain", "a print is running, the card is busy\n");
      return;
    }

    if (telegramBot.receiving()) {
      request->send(503, "text/plain", "a file is arriving from Telegram, the card is busy\n");
      return;
    }

    if (ESP.getFreeHeap() < 30000 || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 8000) {
      request->send(503, "text/plain", "not enough memory right now, try again\n");
      return;
    }

    if (streamsLive >= STREAMS_AT_ONCE && somethingIsStreaming()) {
      AsyncWebServerResponse *busy = request->beginResponse(503, "text/plain", "another file is going out, try again\n");
      busy->addHeader("Retry-After", "5");
      request->send(busy);
      return;
    }

    auto job = std::make_shared<DownloadJob>();
    if (!job->haveCard) {
      request->send(503, "text/plain", "the card is busy, try again\n");
      return;
    }
    job->file = storageFS.open(path);
    if (!job->file) {
      request->send(404, "text/plain", "no such file");
      return;
    }
    const size_t fileSize = job->file.size();
    job->feeder.start(fileSize);
    job->startedAt = millis();
    request->client()->setAckTimeout(20000);

    AsyncWebServerResponse *response = request->beginResponse("application/x-gcode", fileSize, [job, fileSize](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      lastStreamAt = millis();

      if (index && sendableNow() < maxLen) {
        job->heldOff++;
        if (!job->heldSince)
          job->heldSince = millis();
        if (millis() - job->heldSince < STREAM_STALL_GIVE_UP)
          return RESPONSE_TRY_AGAIN;
      }
      job->heldSince = 0;

      const uint32_t into = micros();
      const size_t served = job->feeder.serve(job->file, buffer, maxLen);
      const uint32_t spent = micros() - into;

      job->fills++;
      job->cardMicros += spent;
      if (spent > job->worstMicros)
        job->worstMicros = spent;
      job->served += served;
      {
        const uint32_t block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (!job->blockLow || block < job->blockLow)
          job->blockLow = block;
      }

      if (job->served >= fileSize || (maxLen && !served)) {
        const uint32_t ms = millis() - job->startedAt;
        downloadReport = String(job->served) + " of " + String(fileSize) + " bytes in " + String(ms)
                         + " ms, " + String(ms ? job->served / (float)ms : 0, 1) + " KB/s, "
                         + String(job->fills) + " fills of " + String(job->fills ? job->served / job->fills : 0)
                         + " bytes, card held it " + String(job->cardMicros / 1000) + " ms, longest read "
                         + String(job->worstMicros / 1000) + " ms, stood aside " + String(job->heldOff)
                         + " times, block low " + String(job->blockLow);
      }

      return served;
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
    if (isPrinting) {
      request->send(503, "application/json", "{\"files\":[],\"error\":\"card is busy printing\"}");
      return;
    }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 14000) {
      request->send(503, "application/json", "{\"files\":[],\"error\":\"low memory\"}");
      return;
    }
    if (!cardMutex || xSemaphoreTakeRecursive(cardMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
      request->send(503, "application/json", "{\"files\":[],\"error\":\"card busy\"}");
      return;
    }
    String json = fileListJson();
    xSemaphoreGiveRecursive(cardMutex);
    request->send(200, "application/json", json);
  });

  // For Slic3r OctoPrint compatibility
  server.on("/api/files/local", HTTP_POST, [](AsyncWebServerRequest * request) {
    // https://docs.octoprint.org/en/master/api/files.html?highlight=api%2Ffiles%2Flocal#upload-file-or-create-folder
    lastUploadFields = "";
    for (int i = 0; i < request->params(); ++i) {
      AsyncWebParameter *param = request->getParam(i);
      lastUploadFields += " " + param->name() + "=" + param->value();
    }

    if (uploadIsBusy(request)) {
      request->send(409, "application/json", "{\"error\": \"another upload is already running\"}");
      return;
    }

    if (uploadFailure != "") {
      lcd("Upload failed");
      const int code = uploadFailure.endsWith("the card is busy") ? 409 : 500;
      request->send(code, "application/json", "{\"error\": \"" + jsonEscape(uploadFailure) + "\"}");
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
    if (request->contentLength() == 0)
      request->send(400, "text/plain", "missing body");
    },
    [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
      request->send(400, "text/plain", "file not supported");
    },
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (total == 0 || total > 2048) {
        request->send(400, "text/plain", "invalid body size");
        return;
      }
      if (!index) {
        request->_tempObject = malloc(total + 1);
        if (request->_tempObject)
          ((char *)request->_tempObject)[0] = '\0';
      }
      char *content = (char *)request->_tempObject;
      if (!content) {
        request->send(503, "text/plain", "low memory");
        return;
      }
      if (index + len <= total) {
        memcpy(content + index, data, len);
        content[index + len] = '\0';
      }
      if (index + len >= total) {
        DynamicJsonDocument doc(1024);
        auto error = deserializeJson(doc, content);
        if (error)
          request->send(400, "text/plain", error.c_str());
        else {
          int responseCode = apiJobHandler(doc.as<JsonObject>());
          request->send(responseCode, "text/plain", "");
        }
      }
  });
  
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest * request) {
    const uint32_t elapsed = isPrinting ? (millis() - printStartTime) / 1000 : 0;
    const bool fromSlicer = isPrinting && slicerPercent >= 0;
    const float completion = fromSlicer ? (float)slicerPercent : printCompletion;
    int32_t remaining = -1;
    String remainingFrom = "unknown";
    if (isPrinting) {
      if (slicerMinutesLeft >= 0) {
        remaining = slicerMinutesLeft * 60;
        remainingFrom = "slicer";
      }
      else if (printCompletion > 0.5) {
        remaining = (int32_t)(elapsed / printCompletion * (100 - printCompletion));
        remainingFrom = "extrapolated";
      }
    }

    const float usedGrams = filamentGrams < 0 ? 0 : filamentGrams * (isPrinting ? completion / 100 : 1);

    const time_t now = time(NULL);

    String message;
    message.reserve(1536);
    message = "{\"state\":\"";
    message += getState();
    message += "\",\"connected\":";
    message += stringify(printerConnected);
    message += ",\"printing\":";
    message += stringify(isPrinting);
    message += ",\"paused\":";
    message += stringify(printPause);
    message += ",\"temperature\":{\"tool\":{\"actual\":";
    message += toolTemperature[0].actual;
    message += ",\"target\":";
    message += toolTemperature[0].target;
    message += "},\"bed\":{\"actual\":";
    message += bedTemperature.actual;
    message += ",\"target\":";
    message += bedTemperature.target;
    message += "}},\"job\":{\"file\":\"";
    message += jsonEscape(jobFilename());
    message += "\",\"size\":";
    message += String(jobFileSize());
    message += ",\"position\":";
    message += String(filePos);
    message += ",\"completion\":";
    message += String(completion, 2);
    message += ",\"completionFrom\":\"";
    message += (fromSlicer ? "slicer" : "position");
    message += "\",\"elapsed\":";
    message += String(elapsed);
    message += ",\"remaining\":";
    message += String(remaining);
    message += ",\"remainingFrom\":\"";
    message += remainingFrom;
    message += "\",\"layer\":";
    message += String(currentLayer);
    message += ",\"layers\":";
    message += String(totalLayers);
    message += ",\"z\":";
    message += String(currentZ, 2);
    message += ",\"fan\":";
    message += String(fanSpeed);
    message += "},\"link\":{\"baud\":";
    message += String(serialBauds[serialBaudIndex]);
    message += ",\"sent\":";
    message += String(linesSent);
    message += ",\"resent\":";
    message += String(linesResent);
    message += ",\"mangled\":";
    message += String(linesMangled);
    message += "},\"energy\":{\"wh\":";
    message += String(energyWh, 1);
    message += ",\"watt\":";
    message += String(yandexHome.getPower(), 1);
    message += ",\"power\":";
    message += String(energyWh / 1000 * energyTariff, 3);
    message += ",\"grams\":";
    message += String(usedGrams, 2);
    message += ",\"filament\":";
    message += String(usedGrams / 1000 * filamentPrice, 3);
    message += ",\"cost\":";
    message += String(energyWh / 1000 * energyTariff + usedGrams / 1000 * filamentPrice, 3);
    message += ",\"tariff\":";
    message += String(energyTariff, 3);
    message += ",\"kilo\":";
    message += String(filamentPrice, 2);
    message += "},\"storage\":{\"card\":";
    message += stringify(storageFS.isActive());
    message += ",\"selected\":\"";
    message += jsonEscape(baseName(selectedFile));
    message += "\",\"free\":";
    message += uint64ToString(storageFS.freeBytes());
    message += ",\"total\":";
    message += uint64ToString(storageFS.totalBytes());
    message += "},\"socket\":{\"token\":";
    message += stringify(yandexHome.hasToken());
    message += ",\"device\":\"";
    message += jsonEscape(yandexHome.getDeviceName());
    message += "\",\"auto\":";
    message += stringify(autoPowerOff);
    message += ",\"state\":\"";
    message += jsonEscape(powerOffState());
    message += "\",\"status\":\"";
    message += jsonEscape(yandexHome.getStatus());
    message += "\"},\"system\":{\"heap\":";
    message += String(ESP.getFreeHeap());
    message += ",\"block\":";
    message += String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    message += ",\"uptime\":";
    message += String(millis() / 1000);
    message += ",\"epoch\":";
    message += String((uint32_t)now);
    message += ",\"rssi\":";
    message += String(WiFi.RSSI());
    message += ",\"machine\":\"";
    message += jsonEscape(fwMachineType);
    message += "\",\"bootReason\":\"";
    message += resetReason();
    message += "\",\"version\":\"" SKETCH_VERSION "\"}}";
    request->send(200, "application/json", message);
  });

  // The page asks for what it has not seen rather than the whole ring every second
  server.on("/api/console", HTTP_GET, [](AsyncWebServerRequest * request) {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 12000) {
      request->send(503, "application/json", "{\"seq\":0,\"lines\":[]}");
      return;
    }

    uint32_t since = 0;
    if (request->hasParam("since"))
      since = strtoul(request->getParam("since")->value().c_str(), NULL, 10);

    const uint32_t oldest = consoleSeq > CONSOLE_LINES ? consoleSeq - CONSOLE_LINES : 0;
    if (since < oldest)
      since = oldest;

    String json;
    json.reserve(2560);
    if (consoleMutex && xSemaphoreTake(consoleMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      json = "{\"seq\":";
      json += String(consoleSeq);
      json += ",\"lines\":[";
      for (uint32_t i = since; i < consoleSeq; ++i) {
        if (i != since)
          json += ",";
        json += "\"";
        json += jsonEscape(consoleRing[i % CONSOLE_LINES]);
        json += "\"";
      }
      json += "]}";
      xSemaphoreGive(consoleMutex);
    } else {
      json = "{\"seq\":" + String(consoleSeq) + ",\"lines\":[]}";
    }

    request->send(200, "application/json", json);
  });

  server.on("/api/command", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (!request->hasParam("cmd", true)) {
      request->send(400, "text/plain", "cmd is required");
      return;
    }

    String command = request->getParam("cmd", true)->value();
    command.trim();
    if (command == "") {
      request->send(400, "text/plain", "cmd is empty");
      return;
    }

    consoleAdd(">" + command);          // Shown from here, since a print hides the gcode going out
    commandQueue.push(command);
    request->send(204, "text/plain", "");
  });

  server.on("/log/clear", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (isPrinting) {
      request->send(409, "text/plain", "a print is running, the card is busy");
      return;
    }
    if (logMutex)
      xSemaphoreTake(logMutex, portMAX_DELAY);
    logBuffer = "";
    if (logMutex)
      xSemaphoreGive(logMutex);
    storageFS.remove(LOG_PATH);
    storageFS.remove(LOG_PREVIOUS);
    logFlushAt = millis() + LOG_FLUSH_INTERVAL;
    request->send(204, "text/plain", "");
  });

  // The tail goes into a buffer that is here for good and is handed out from there. The
  // String it used to be built in cost four kilobytes on the heap, which the response
  // then copied into its own content and again into the packet, so one reading of the
  // journal asked the heap for three pieces of four kilobytes within a millisecond of
  // each other. That is what was taking the module down, not the size of the heap.
  server.on("/log", HTTP_GET, [](AsyncWebServerRequest * request) {
    witnessStep(101);
    if (logTailBusy) {                     // One reader at a time, the buffer is shared
      witnessStep(102);
      request->send(503, "text/plain", "the journal is being read, try again");
      return;
    }

    witnessStep(102);
    if (isPrinting) {                      // The card belongs to the print, answer from memory
      if (logMutex)
        xSemaphoreTake(logMutex, portMAX_DELAY);
      logTailHeld = logBuffer.length() < sizeof(logTail) ? logBuffer.length() : sizeof(logTail);
      memcpy(logTail, logBuffer.c_str(), logTailHeld);
      if (logMutex)
        xSemaphoreGive(logMutex);
      if (logTailHeld == 0) {              // Just flushed, so the words are on the card and
        static const char waiting[] =      // the card is the print's until it finishes
          "the journal is on the card and the card belongs to the print until it ends\n";
        logTailHeld = sizeof(waiting) - 1;
        memcpy(logTail, waiting, logTailHeld);
      }
    }
    else {
      witnessStep(103);
      if (!cardMutex || xSemaphoreTakeRecursive(cardMutex, pdMS_TO_TICKS(150)) != pdTRUE) {
        witnessStep(104);
        request->send(503, "text/plain", "card busy, try again");
        return;
      }

      witnessStep(105);
      const char *path = request->hasParam("previous") ? LOG_PREVIOUS : LOG_PATH;
      FileWrapper file = storageFS.open(path);
      if (!file) {
        xSemaphoreGiveRecursive(cardMutex);
        witnessStep(106);
        request->send(404, "text/plain", "nothing recorded yet");
        return;
      }

      witnessStep(107);
      const size_t size = file.size();
      if (size > sizeof(logTail))          // The whole of it would not fit, and nobody reads it
        file.seek(size - sizeof(logTail));

      witnessStep(108);
      logTailHeld = 0;
      int taken;
      while (logTailHeld < sizeof(logTail) &&
             (taken = file.read((uint8_t *)logTail + logTailHeld, sizeof(logTail) - logTailHeld)) > 0)
        logTailHeld += taken;
      file.close();
      xSemaphoreGiveRecursive(cardMutex);
    }

    witnessStep(109);
    if (logTailHeld == 0) {
      witnessStep(110);
      request->send(404, "text/plain", "nothing recorded yet");
      return;
    }

    logTailBusy = true;
    const size_t held = logTailHeld;
    witnessStep(111);
    AsyncWebServerResponse *response = request->beginResponse("text/plain; charset=utf-8", held,
                                       [held](uint8_t *buffer, size_t room, size_t index) -> size_t {
      witnessStep(113);
      const size_t left = held - index;
      const size_t take = room < left ? room : left;
      memcpy(buffer, logTail + index, take);
      witnessStep(114);
      return take;
    });
    const uint32_t mine = witnessHere();        // The spy's hook is replaced here, so carry it
    request->onDisconnect([mine]() { logTailBusy = false; witnessLeft(mine); });
    witnessStep(112);
    request->send(response);
    witnessStep(115);
  });

  server.on("/api/witness", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", witnessReport());
  });

  server.on("/api/line", HTTP_GET, [](AsyncWebServerRequest * request) {
    lineProbeRequested = true;
    request->send(200, "application/json", "{\"atBoot\":" + bootProbeResult + ",\"now\":" + String(lineProbeResult) + "}");
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
    if (request->contentLength() == 0)
      request->send(400, "text/plain", "missing body");
    },
    [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
      request->send(400, "text/plain", "file not supported");
    },
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (total == 0 || total > 2048) {
        request->send(400, "text/plain", "invalid body size");
        return;
      }
      if (!index) {
        request->_tempObject = malloc(total + 1);
        if (request->_tempObject)
          ((char *)request->_tempObject)[0] = '\0';
      }
      char *content = (char *)request->_tempObject;
      if (!content) {
        request->send(503, "text/plain", "low memory");
        return;
      }
      if (index + len <= total) {
        memcpy(content + index, data, len);
        content[index + len] = '\0';
      }
      if (index + len >= total) {
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

  server.on("/ui/upload", HTTP_POST, [](AsyncWebServerRequest * request) {
    if (uploadFailure != "") {
      request->send(500, "application/json", "{\"error\": \"" + jsonEscape(uploadFailure) + "\"}");
      return;
    }
    request->send(201, "application/json", "{\"done\": true}");
  }, handleInterfaceUpload);

  server.begin();

  #ifdef OTA_UPDATES
    // OTA setup
    ArduinoOTA.setHostname(MDNS_HOSTNAME);     // This is also the name the network answers to
    // The same secret as the web, so there is one to lose rather than two. espota needs
    // it as --auth, and the /update route behind the password is the gentler way in.
    ArduinoOTA.setPassword(webAuthPassword.c_str());
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      esp_task_wdt_reset();
    });
    ArduinoOTA.onEnd([]() {
      bootGuardArm();
    });
    ArduinoOTA.begin();
  #endif

  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);
}

// Called once per gcode line for the whole of a print. Built as text rather than as a
// String, because four reallocations a line, hundreds of lines a second, is what leaves
// the heap unable to find the unbroken block the encryption needs.
// Everything that goes out to the printer and everything that comes back goes through
// PrinterLink, and nothing else is allowed near the port. It carries the line numbers,
// the checksums, the window, the repeats and the silence timer, and it is the same code
// that is driven against a simulated printer in test/test_printer_link.cpp.
struct SerialIo : LinkIo {
  int read() override { return PrinterSerial.read(); }
  size_t write(const uint8_t *data, const size_t len) override { return PrinterSerial.write(data, len); }
  size_t writable() override { return PrinterSerial.availableForWrite(); }
  uint32_t now() override { return millis(); }
};
static SerialIo serialIo;

LinkIo *printerWire() { return &serialIo; }
PrinterLink printerLink;

// Takes raw text on purpose. An answer arrives thousands of times a print, and a String
// built for each one is what left the heap without an unbroken block for the encryption.
static void echoToWatchers(const char *line, const char *detail, const bool boring) {
  if (!boring)
    consoleAdd("<" + String(line));
  if (telnetReady()) {
    const uint32_t began = millis();
    serverClient.print('<');
    serverClient.print(line);
    serverClient.print('#');
    serverClient.print(detail);
    serverClient.println('#');
    telnetDropIfStalled(began);
  }
}

// Called for every answer that is not part of the protocol itself
static void onPrinterLine(const char *raw, void *) {
  String line(raw);
  const char *detail = "unrecognised";
  bool unsolicited = false;

  if (line.startsWith("start")) {
    if (isPrinting)
      logEvent("the printer restarted itself in the middle of the job");
    printerRestarted = true;
    detail = "printer restarted";
  }
  else if (!printerConnected)
    detail = "discovering";
  else if (parseTemperatures(line)) {
    unsolicited = true;
    detail = "temperatures";
  }
  else if (parsePosition(line))
    detail = "position";
  else if (line.startsWith("echo:busy"))
    detail = "busy";
  else if (line.startsWith("SD printing byte") || line.startsWith("Not SD printing")) {
    // Only the printer's own card prints produce these
    unsolicited = true;
    detail = "sd status";
    const bool running = line.startsWith("SD printing byte");
    if (running) {
      const int slash = line.indexOf('/');
      const uint32_t at = line.substring(16, slash).toInt();
      const uint32_t of = line.substring(slash + 1).toInt();
      printingFileSize = of;
      printCompletion = of ? min((float)at / of * 100, 100.0f) : 0;
    }
    if (running != externalPrinting) {
      externalPrinting = running;
      if (running) {
        printStartTime = millis();
        logEvent("the printer started a file from its own card");
      }
      else {
        printCompletion = 0;
        logEvent("the print from the printer's own card ended");
        powerOffArmed = autoPowerOff;
        powerOffSawIdle = false;
        powerOffReadySince = 0;
        powerOffAttempts = 0;
      }
    }
  }
  else if (line.startsWith("File opened:")) {
    const int size = line.indexOf(" Size:");
    printingFile = line.substring(13, size > 0 ? size : line.length());
    printingFile.trim();
    if (size > 0)
      printingFileSize = line.substring(size + 6).toInt();
    detail = "file opened";
  }
  else if (line.startsWith("//action:")) {
    unsolicited = true;
    const String action = line.substring(9);
    if (action.startsWith("cancel")) {
      detail = "screen asked to cancel";
      if (isPrinting) {
        cancelPrint = true;
        logEvent("cancelled from the printer's screen");
      }
    }
    else if (action.startsWith("pause")) {
      detail = "screen asked to pause";
      if (isPrinting) {
        printPause = true;
        logEvent("paused from the printer's screen");
      }
    }
    else if (action.startsWith("resume")) {
      detail = "screen asked to resume";
      printPause = false;
    }
    else
      detail = "host action";
  }
  else if (line.indexOf("Unknown command") != -1) {
    if (isPrinting)
      ++linesMangled;
    detail = "mangled line reached the printer";
  }
  else if (line.startsWith("echo: cold extrusion prevented"))
    detail = "cold extrusion";
  else if (line.startsWith("Error:")) {
    if (line.indexOf("halted") != -1 || line.indexOf("kill") != -1 ||
        line.indexOf("Thermal") != -1 || line.indexOf("MINTEMP") != -1 ||
        line.indexOf("MAXTEMP") != -1 || line.indexOf("Heating failed") != -1) {
      logEvent("the printer stopped the job itself: " + printableOnly(line));
      cancelPrint = true;
      detail = "ERROR";
    }
    else
      detail = "error, not fatal";       // The repeats behind it are the link's business
  }

  if (printerConnected && loggedSamples < LOG_SAMPLES_PER_PRINT &&
      (strcmp(detail, "mangled line reached the printer") == 0 || strcmp(detail, "ERROR") == 0)) {
    ++loggedSamples;
    logEvent("printer complained: " + printableOnly(line) + " (after " + printableOnly(lastCommandSent) + ")");
  }

  cardSend.observe(raw);          // The transfer and the card listing both live off these
  echoToWatchers(raw, detail, unsolicited);
  lastReceivedResponse = line;
}

// The answers themselves. M105 carries its reading on the same line as the answer.
static void onPrinterAck(const char *raw, void *) {
  if (strstr(raw, "T:"))                  // M105 answers with the reading on the same line
    parseTemperatures(String(raw));
  if (fwAutoreportTempCap && lastCommandSent.startsWith(AUTOTEMP_COMMAND))
    autoreportTempEnabled = (lastCommandSent[6] != '0');
  const bool boring = isPrinting || cardSendRunning;
  if (!boring || telnetReady())
    echoToWatchers(raw, "ok", boring);
}

String linkReport() {
  return "budget " + String(printerLink.budgetBytes) + " bytes and "
       + String(printerLink.budgetLines) + " of " + String(printerLink.slotCeiling) + " lines"
       + ", slots seen " + String(printerLink.caps.slots)
       + ", window " + String(printerLink.waiting()) + " holding " + String(printerLink.inflightBytes())
       + ", sent " + String(printerLink.queued) + ", answered " + String(printerLink.acked)
       + ", repeats " + String(printerLink.resends) + ", resyncs " + String(printerLink.resyncs)
       + ", silences " + String(printerLink.timeouts) + ", stale " + String(printerLink.stale)
       + ", refused " + String(printerLink.refused) + ", errors " + String(printerLink.errors)
       + (printerLink.caps.advancedOk ? ", detailed ok" : ", plain ok")
       + (printerLink.caps.lfnWrite ? ", long names" : ", 8.3 names");
}

void linkBegin() {
  printerLink.begin(&serialIo);
  printerLink.onOther = onPrinterLine;
  printerLink.onAck = onPrinterAck;
}

void SendCommands() {
  if (cardSendRunning || printerFlashRunning)
    return;                       // The transfer owns the wire until it closes the file
  const char *command;
  while ((command = commandQueue.peekSend()) != NULL) {
    if (!printerLink.canAccept(strlen(command)))
      break;
    if (!printerLink.send(command))
      break;
    lastCommandSent = command;
    ++linesSent;
    commandQueue.popSend();
  }
  printerLink.service();
}

void ReceiveResponses() {
  const uint32_t wasResent = printerLink.resends, wasTimedOut = printerLink.timeouts;
  printerLink.service();
  linesResent += printerLink.resends - wasResent;
  if (printerLink.timeouts != wasTimedOut) {
    ++linkTimeouts;
    telnetSend("#TIMEOUT#");
  }

  // The link gives up on nothing by itself, so the decision to abandon a job is made here
  if (printerConnected && isPrinting && printerLink.timeouts > MAX_TIMEOUT_RETRIES) {
    logEvent("printer stopped answering, " + String(printerLink.timeouts) + " silent stretches");
    cancelPrint = true;
    lcd("Printer not responding");
    printerLink.timeouts = 0;
  }
}

void loop() {
  witnessWatch(asyncTcpStackLow, heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  bootGuardWatch(WiFi.status() == WL_CONNECTED);

  if (updateRunning) {             // Let the upload have the chip to itself
    if ((int32_t)(millis() - updateTouchedAt) > UPDATE_ABANDONED_AFTER) {
      updateRunning = false;
      Update.abort();
      PrinterSerial.begin(serialBauds[serialBaudIndex], SERIAL_8N1, PRINTER_RX_PIN, PRINTER_TX_PIN);
    }
    esp_task_wdt_reset();
    return;
  }

  #ifdef OTA_UPDATES
    //****************
    //* OTA handling *
    //****************
    if (ESPrestartRequired) {  // check the flag here to determine if a restart is required
      ESPrestartRequired = false;
      logEvent("restarting");
      logFlush();
      delay(500);
      ESP.restart();
    }

    ArduinoOTA.handle();
  #endif

  static bool wifiWasUp = true;
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasUp) {
      wifiWasUp = false;
      logEvent("wifi lost");
    }
    if ((signed)(wifiRetryTimer - millis()) <= 0) {
      wifiRetryTimer = millis() + WIFI_RETRY_INTERVAL;
      WiFi.begin();       // No disconnect first, tearing down a flapping link only makes it worse
    }
    if (!isPrinting && (signed)(millis() - wifiDownSince) >= WIFI_REBOOT_AFTER)
      ESP.restart();
  }
  else {
    if (!wifiWasUp) {
      wifiWasUp = true;
      logEvent("wifi back, rssi " + String(WiFi.RSSI()) + ", ip " + IpAddress2String(WiFi.localIP()));
    }
    wifiDownSince = millis();
  }

  //********************
  //* Printer handling *
  //********************
  handleLineProbe();
  handleRawBench();
  handleStorage();
  handleLogging();
  handleTelegram();      // Answers even with the printer silent, so /status always says something

  if (!printerConnected)
    printerConnected = detectPrinter();
  else {
    #ifndef OTA_UPDATES
      MDNS.update();    // When OTA is active it's called by 'handle' method
    #endif

    handlePrint();
    handleCardSend();
    handlePrinterFlash();
    handleSendQueue();
    handleWebAuthGreeting();
    handleEnergy();
    handleAutoPowerOff();

    if (printerRestarted && !isPrinting) {
      printerRestarted = false;
      cancelPrint = false;
      commandQueue.clear();
      printerLink.reset();
      printerCard.stale();
      lastCommandSent = "";
      commandQueue.push("M110 N0");
      #ifdef DEAD_MAN_SECONDS
        commandQueue.push("M85 S0");
      #endif
      commandQueue.push("M104 S0");
      commandQueue.push("M140 S0");
      commandQueue.push("M107");
      lcd("Printer restarted");
    }
    else if (cancelPrint && !isPrinting) { // Only when cancelPrint has been processed by 'handlePrint'
      cancelPrint = false;
      commandQueue.clear();               // Takes the disarm queued when the print ended with it
      lcd("Print cancelled");
      #ifdef DEAD_MAN_SECONDS
        commandQueue.push("M85 S0");
      #endif
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

    // Not a word while M28 is open: anything sent then is written into the file
    if (!autoreportTempEnabled && !cardSendRunning) {
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
    telnetLoggedIn = false;
    serverClient.println("password:");
  }

  static String telnetCommand;
  while (serverClient && serverClient.available()) {  // get data from Client
    const int ch = serverClient.read();
    if (ch < 0)                 // available() can promise a byte that read() then refuses to hand over
      break;

    if (ch == '\r' || ch == '\n') {
      if (telnetCommand.length() > 0) {
        if (telnetLoggedIn)
          commandQueue.push(telnetCommand);
        else if (telnetCommand == webAuthPassword) {
          telnetLoggedIn = true;
          serverClient.println("go ahead");
        }
        else {
          serverClient.println("no");
          serverClient.stop();
          logEvent("a telnet client gave the wrong password and was dropped");
        }
        telnetCommand = "";
      }
    }
    else
      telnetCommand += (char)ch;
  }

  esp_task_wdt_reset();

  if (!isPrinting && receivingFile())
    vTaskDelay(TRANSFER_YIELD);
}
