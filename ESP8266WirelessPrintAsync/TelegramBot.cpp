#include "TelegramBot.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

#include "StorageFS.h"

extern void logEvent(const String &text);
extern void logFlush();
extern SemaphoreHandle_t tlsGate;   // Only one encrypted conversation fits in this heap
extern volatile bool tlsWanted;

// A crash takes the journal buffer with it, so a breadcrumb is only useful once it is
// already on the card.
static void trail(const String text) {
  logEvent(text);
  logFlush();
}

extern String sanitizeFilename(const String filename);
extern bool isPrinting;
extern volatile bool cardSendRunning;
extern bool firmwareBegin();
extern bool firmwareChunk(uint8_t *data, const size_t len);
extern String firmwareEnd(const bool whole);
extern void firmwareReboot();
extern bool somethingIsStreaming();

#define TELEGRAM_HOST    "https://api.telegram.org"
#define TELEGRAM_STACK   16384         // The handshake checks a chain of three and needs the room
// Every one of these opens a connection, and opening one costs sixty kilobytes of heap
// for a second or two. Holding the quiet request open longer is the cheapest way to make
// that window rare: the bot still answers, it just waits longer for something to answer.
#define TELEGRAM_WAIT    25            // Seconds Telegram holds a quiet request open
#define TELEGRAM_TIMEOUT 20000
#define TELEGRAM_JSON    4096
#define TELEGRAM_REPLY_GRACE 1500      // How long to let the loop answer before asking again
#define TELEGRAM_MAX_FILE (20UL * 1024 * 1024)   // The bot API will not hand over more
#define TELEGRAM_CHUNK   8192          // Read in one go; every call through the encryption costs
#define TELEGRAM_SEEN_MAX 5
// Encryption asks for two buffers of about sixteen kilobytes each. Free heap says
// nothing about whether such a block exists, and asking when it does not is what used
// to take the module down: the handshake fails, the failure leaves rubbish behind, the
// next one has less to work with, and a few rounds of that end in a panic.
#define TELEGRAM_BLOCK_FLOOR 20000
#define TELEGRAM_HEAP_FLOOR 80000
#define TELEGRAM_HEAP_FLOOR_PRINTING 95000
#define TELEGRAM_COOLING 300000        // Stand off this long after a run of failures

// api.telegram.org is signed under this one, which stands until 2038. Pinning it means
// the bot token only ever leaves the module down a connection we have checked.
static const char TELEGRAM_ROOT_CA[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDxTCCAq2gAwIBAgIBADANBgkqhkiG9w0BAQsFADCBgzELMAkGA1UEBhMCVVMx\n"
"EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoT\n"
"EUdvRGFkZHkuY29tLCBJbmMuMTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRp\n"
"ZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5MDkwMTAwMDAwMFoXDTM3MTIzMTIz\n"
"NTk1OVowgYMxCzAJBgNVBAYTAlVTMRAwDgYDVQQIEwdBcml6b25hMRMwEQYDVQQH\n"
"EwpTY290dHNkYWxlMRowGAYDVQQKExFHb0RhZGR5LmNvbSwgSW5jLjExMC8GA1UE\n"
"AxMoR28gRGFkZHkgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIw\n"
"DQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAL9xYgjx+lk09xvJGKP3gElY6SKD\n"
"E6bFIEMBO4Tx5oVJnyfq9oQbTqC023CYxzIBsQU+B07u9PpPL1kwIuerGVZr4oAH\n"
"/PMWdYA5UXvl+TW2dE6pjYIT5LY/qQOD+qK+ihVqf94Lw7YZFAXK6sOoBJQ7Rnwy\n"
"DfMAZiLIjWltNowRGLfTshxgtDj6AozO091GB94KPutdfMh8+7ArU6SSYmlRJQVh\n"
"GkSBjCypQ5Yj36w6gZoOKcUcqeldHraenjAKOc7xiID7S13MMuyFYkMlNAJWJwGR\n"
"tDtwKj9useiciAF9n9T521NtYJ2/LOdYq7hfRvzOxBsDPAnrSTFcaUaz4EcCAwEA\n"
"AaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYE\n"
"FDqahQcQZyi27/a9BUFuIMGU2g/eMA0GCSqGSIb3DQEBCwUAA4IBAQCZ21151fmX\n"
"WWcDYfF+OwYxdS2hII5PZYe096acvNjpL9DbWu7PdIxztDhC2gV7+AJ1uP2lsdeu\n"
"9tfeE8tTEH6KRtGX+rcuKxGrkLAngPnon1rpN5+r5N9ss4UXnT3ZJE95kTXWXwTr\n"
"gIOrmgIttRD02JDHBHNA7XIloKmf7J6raBKZV8aPEjoJpL1E/QYVN8Gb5DKj7Tjo\n"
"2GTzLH4U/ALqn83/B2gX2yKQOC16jdFU8WnjXzPKej17CuPKf1855eJ1usV2GDPO\n"
"LPAvTK33sefOT6jEm0pUBsV/fdUID+Ic/n4XuKxe9tQWskMJDE32p2u0mYRlynqI\n"
"4uJEvlz36hz1\n"
"-----END CERTIFICATE-----\n";

TelegramBot telegramBot;

SemaphoreHandle_t TelegramBot::mutex = NULL;
TaskHandle_t TelegramBot::task = NULL;
String TelegramBot::token, TelegramBot::chats, TelegramBot::seen, TelegramBot::status;
TelegramBot::Message TelegramBot::inbox[TelegramBot::QUEUE], TelegramBot::outbox[TelegramBot::QUEUE];
int TelegramBot::inboxHead = 0, TelegramBot::inboxTail = 0;
int TelegramBot::outboxHead = 0, TelegramBot::outboxTail = 0;
String TelegramBot::waitingChat, TelegramBot::waitingId, TelegramBot::waitingName;
size_t TelegramBot::waitingSize = 0;
int64_t TelegramBot::offset = -1;
volatile bool TelegramBot::landing = false;
String TelegramBot::probeUrl, TelegramBot::benchUrl, TelegramBot::benchOutcome = "not run";
size_t TelegramBot::benchChunk = 0;
bool TelegramBot::benchLegacy = false;
static UBaseType_t stackLowest = 0xFFFFFFFF;

// One buffer for every transfer the bot makes: a file to the card, firmware to the
// flash, the bench. They all run on this one task and never overlap, and a second
// buffer would be eight kilobytes the encryption cannot have.
// Aligned, because the card controller moves this over DMA and an odd address makes
// the driver copy every block through a bounce buffer instead.
static uint8_t telegramBuffer[TELEGRAM_CHUNK] __attribute__((aligned(32)));
static bool telegramPrimed = false;      // The first answer is only read for its place in the queue
static uint32_t coolingUntil = 0;        // Nothing is attempted before this
static uint8_t refusals = 0;             // Connections lost or refused in a row

class Hold {
  public:
    inline Hold(SemaphoreHandle_t mutex) : mutex(mutex) {
      if (mutex)
        xSemaphoreTake(mutex, portMAX_DELAY);
    }

    inline ~Hold() {
      if (mutex)
        xSemaphoreGive(mutex);
    }

  private:
    SemaphoreHandle_t mutex;
};

static String fromInt64(const int64_t value) {
  char text[24];
  snprintf(text, sizeof(text), "%lld", (long long)value);

  return String(text);
}

// A comma separated list is enough for a handful of chat ids and shows itself plainly
// on the settings page.
static bool listHas(const String list, const String item) {
  return list != "" && ("," + list + ",").indexOf("," + item + ",") >= 0;
}

static String listAdd(const String list, const String item) {
  return listHas(list, item) ? list : (list == "" ? item : list + "," + item);
}

static String listRemove(const String list, const String item) {
  String out;
  int at = 0;
  while (at < (int)list.length()) {
    const int comma = list.indexOf(',', at);
    const String one = comma < 0 ? list.substring(at) : list.substring(at, comma);
    if (one != "" && one != item)
      out = out == "" ? one : out + "," + one;
    if (comma < 0)
      break;
    at = comma + 1;
  }

  return out;
}

void TelegramBot::begin() {
  mutex = xSemaphoreCreateMutex();

  Preferences preferences;
  preferences.begin("telegram", true);
  token = preferences.getString("token", "");
  chats = preferences.getString("chats", "");
  preferences.end();

  status = token == "" ? "no token" : "starting";

  xTaskCreatePinnedToCore(run, "telegram", TELEGRAM_STACK, NULL, 1, &task, 0);
}

bool TelegramBot::hasToken() {
  Hold hold(mutex);

  return token != "";
}

void TelegramBot::setToken(const String value) {
  Hold hold(mutex);

  token = value;
  status = value == "" ? "no token" : "token stored";
  offset = -1;
  telegramPrimed = false;

  Preferences preferences;
  preferences.begin("telegram", false);
  preferences.putString("token", value);
  preferences.end();
}

void TelegramBot::forgetToken() {
  setToken("");
}

String TelegramBot::getChats() {
  Hold hold(mutex);

  return chats;
}

String TelegramBot::getSeen() {
  Hold hold(mutex);

  return seen;
}

bool TelegramBot::allowed(const String chat) {
  Hold hold(mutex);

  return listHas(chats, chat);
}

void TelegramBot::allow(const String chat) {
  Hold hold(mutex);

  chats = listAdd(chats, chat);
  seen = listRemove(seen, chat);

  Preferences preferences;
  preferences.begin("telegram", false);
  preferences.putString("chats", chats);
  preferences.end();
}

void TelegramBot::deny(const String chat) {
  Hold hold(mutex);

  chats = listRemove(chats, chat);
  seen = listRemove(seen, chat);

  Preferences preferences;
  preferences.begin("telegram", false);
  preferences.putString("chats", chats);
  preferences.end();
}

String TelegramBot::getStatus() {
  Hold hold(mutex);

  return status;
}

bool TelegramBot::receiving() {
  return landing;
}

// Remembers a stranger so the settings page can offer them, and no more than a few of
// them, so knocking cannot fill anything up.
void TelegramBot::remember(const String chat) {
  Hold hold(mutex);

  if (listHas(seen, chat) || listHas(chats, chat))
    return;

  int count = 1;
  for (unsigned int at = 0; at < seen.length(); at++)
    if (seen[at] == ',')
      count++;
  while (count >= TELEGRAM_SEEN_MAX && seen.indexOf(',') >= 0) {
    seen = seen.substring(seen.indexOf(',') + 1);
    count--;
  }
  seen = listAdd(seen, chat);
}

bool TelegramBot::nextCommand(String &chat, String &text) {
  Hold hold(mutex);

  if (inboxHead == inboxTail)
    return false;

  chat = inbox[inboxTail].chat;
  text = inbox[inboxTail].text;
  inboxTail = (inboxTail + 1) % QUEUE;

  return true;
}

void TelegramBot::deliver(const String chat, const String text) {
  Hold hold(mutex);

  const int next = (inboxHead + 1) % QUEUE;
  if (next == inboxTail)
    return;                      // The loop is not keeping up, and dropping beats blocking

  inbox[inboxHead] = { chat, text };
  inboxHead = next;
}

static const char BENCH_CHAT[] = "bench";
static String benchReply;

void TelegramBot::benchSend(const String text) {
  {
    Hold hold(mutex);
    benchReply = "";
  }
  deliver(BENCH_CHAT, text);
}

String TelegramBot::benchAnswer() {
  Hold hold(mutex);

  return benchReply;
}

void TelegramBot::send(const String chat, const String text) {
  Hold hold(mutex);

  if (chat == BENCH_CHAT) {
    benchReply = text;
    return;
  }

  const int next = (outboxHead + 1) % QUEUE;
  if (next == outboxTail)
    return;

  outbox[outboxHead] = { chat, text };
  outboxHead = next;
}

void TelegramBot::notify(const String text) {
  String list;
  {
    Hold hold(mutex);
    list = chats;
  }

  int at = 0;
  while (at < (int)list.length()) {
    const int comma = list.indexOf(',', at);
    const String one = comma < 0 ? list.substring(at) : list.substring(at, comma);
    if (one != "")
      send(one, text);
    if (comma < 0)
      break;
    at = comma + 1;
  }
}

String TelegramBot::encode(const String text) {
  String out;
  for (unsigned int at = 0; at < text.length(); at++) {
    const char one = text[at];
    if (isalnum((unsigned char)one) || one == '-' || one == '_' || one == '.' || one == '~')
      out += one;
    else {
      char escaped[4];
      snprintf(escaped, sizeof(escaped), "%%%02X", (unsigned char)one);
      out += escaped;
    }
  }

  return out;
}

bool TelegramBot::call(const String path, String &body, const uint32_t timeout) {
  String secret;
  {
    Hold hold(mutex);
    if (token == "")
      return false;
    secret = token;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Hold hold(mutex);
    status = "no network";
    return false;
  }

  // Standing aside is the whole fix for the spiral that took the module down: a cramped
  // heap makes the handshake fail, a failed handshake leaves the heap more cramped.
  if (somethingIsStreaming()) {
    Hold hold(mutex);
    status = "waiting for a long answer to go out";
    return false;
  }

  const size_t room = ESP.getFreeHeap();
  const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (block < TELEGRAM_BLOCK_FLOOR ||
      room < (isPrinting ? TELEGRAM_HEAP_FLOOR_PRINTING : TELEGRAM_HEAP_FLOOR)) {
    Hold hold(mutex);
    status = "waiting for memory, heap " + String(room) + " block " + String(block);
    return false;
  }

  // Trying again and again through a bad patch is how the rubbish piles up. After a run
  // of failures the bot keeps quiet for a while and lets whatever took the memory finish.
  if (coolingUntil && (int32_t)(millis() - coolingUntil) < 0) {
    Hold hold(mutex);
    status = "standing off after a run of failures";
    return false;
  }
  coolingUntil = 0;

  // The socket's own task talks over the same kind of connection, and two of them at
  // once will not fit. Whoever gets here second waits.
  if (!tlsGate || xSemaphoreTake(tlsGate, pdMS_TO_TICKS(50)) != pdTRUE) {
    Hold hold(mutex);
    status = "the other connection is busy";
    return false;
  }

  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA);
  client.setTimeout(timeout / 1000);

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(timeout);
  if (!http.begin(client, String(TELEGRAM_HOST) + "/bot" + secret + path)) {
    client.stop();   // Frees the encryption buffers before the gate lets the next one in
    xSemaphoreGive(tlsGate);
    Hold hold(mutex);
    status = "cannot open connection";
    return false;
  }

  const int code = http.GET();
  if (code > 0)
    body = http.getString();
  http.end();
  client.stop();   // Frees the encryption buffers before the gate lets the next one in
  xSemaphoreGive(tlsGate);

  if (code != 200) {
    trail("telegram: " + String(code < 0 ? HTTPClient::errorToString(code) : "HTTP " + String(code))
          + ", heap " + String(ESP.getFreeHeap())
          + " block " + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    if (code < 0 && ++refusals >= 3) {
      refusals = 0;
      coolingUntil = millis() + TELEGRAM_COOLING;
      trail("telegram: three refusals in a row, standing off for "
            + String(TELEGRAM_COOLING / 60000) + " minutes");
    }
  }
  else
    refusals = 0;

  Hold hold(mutex);
  if (code == 401)
    status = "token rejected";
  else if (code < 0)
    status = "connection failed, " + String(HTTPClient::errorToString(code));
  else if (code != 200)
    status = "HTTP " + String(code);
  else
    status = "ok";

  return code == 200;
}

void TelegramBot::flush() {
  for (;;) {
    Message one;
    {
      Hold hold(mutex);
      if (outboxHead == outboxTail)
        return;
      one = outbox[outboxTail];
      outboxTail = (outboxTail + 1) % QUEUE;
    }

    String body;
    call("/sendMessage?chat_id=" + one.chat + "&text=" + encode(one.text), body, TELEGRAM_TIMEOUT);
  }
}

bool TelegramBot::poll() {
  String body;
  if (!call("/getUpdates?offset=" + fromInt64(offset) + "&limit=2&timeout=" + String(TELEGRAM_WAIT)
            + "&allowed_updates=%5B%22message%22%5D", body, TELEGRAM_TIMEOUT + TELEGRAM_WAIT * 1000))
    return false;

  DynamicJsonDocument document(TELEGRAM_JSON);
  if (deserializeJson(document, body)) {
    Hold hold(mutex);
    status = "unreadable answer";
    trail("telegram: could not read the answer");
    return false;
  }

  bool answered = false;
  for (JsonObject update : document["result"].as<JsonArray>()) {
    offset = (int64_t)update["update_id"].as<long long>() + 1;

    // Whatever was waiting from before the module started is read for its number and
    // thrown away, or a reboot would carry out yesterday's orders again.
    if (!telegramPrimed)
      continue;

    JsonObject message = update["message"];
    if (message.isNull())
      continue;

    const String chat = fromInt64((int64_t)message["chat"]["id"].as<long long>());
    if (!allowed(chat)) {
      remember(chat);                 // Strangers get no answer at all, only a line in the log
      continue;
    }

    JsonObject file = message["document"];
    if (!file.isNull()) {
      // Fetching it here would open a second encrypted connection while the first one
      // and the answer it carried are both still held, which is more memory than this
      // module has. It is noted and collected on the next round instead.
      const char *id = file["file_id"];
      if (id && waitingId == "") {
        waitingChat = chat;
        waitingId = id;
        waitingName = (const char *)(file["file_name"] | "print.gcode");
        waitingSize = file["file_size"] | 0;
      }
      else if (id)
        send(chat, "Still busy with the last file, send this one again in a moment.");
      continue;
    }

    const char *text = message["text"];
    if (text) {
      deliver(chat, String(text));
      answered = true;
    }
  }

  // The loop answers in its own time, and asking Telegram again straight away would
  // leave the reply sitting here for another poll.
  telegramPrimed = true;

  if (answered) {
    const uint32_t until = millis() + TELEGRAM_REPLY_GRACE;
    while (millis() < until) {
      {
        Hold hold(mutex);
        if (outboxHead != outboxTail)
          break;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

  return true;
}

void TelegramBot::fetchDocument(const String chat, const String fileId, const String fileName, const size_t size) {
  if (isPrinting || cardSendRunning) {
    send(chat, "Busy, the card is in use. Try again in a moment.");
    return;
  }
  if (size > TELEGRAM_MAX_FILE) {
    send(chat, "That file is " + String(size / 1024 / 1024) + " MB. Telegram only lets a bot fetch 20 MB.");
    return;
  }

  const String name = sanitizeFilename(fileName);
  const bool firmware = name.endsWith(".bin");

  if (firmware) {
    // Flashing ends in a reboot, and a reboot in the middle of a print ends the print.
    if (isPrinting) {
      send(chat, "A print is running. Firmware has to wait for it to finish.");
      return;
    }
  }
  else if (!name.endsWith(".gcode") && !name.endsWith(".gco") && !name.endsWith(".g")) {
    send(chat, "Send a .gcode file for the card, or a .bin to flash the module. This one is " + name);
    return;
  }
  else if (!storageFS.isActive()) {
    send(chat, "No card in the module.");
    return;
  }
  else if (size >= storageFS.freeBytes()) {
    send(chat, "Not enough room on the card.");
    return;
  }

  // Telegram does not always answer the first time, and giving up on one timeout means
  // giving up on the whole transfer.
  String body;
  bool asked = false;
  for (int attempt = 0; attempt < 3 && !asked; attempt++) {
    if (attempt)
      vTaskDelay(pdMS_TO_TICKS(1500));
    asked = call("/getFile?file_id=" + fileId, body, TELEGRAM_TIMEOUT);
  }
  if (!asked) {
    send(chat, "Could not ask where that file is after three tries: " + getStatus());
    return;
  }

  DynamicJsonDocument document(2048);
  if (deserializeJson(document, body)) {
    send(chat, "Telegram's answer made no sense: " + body.substring(0, 120));
    return;
  }
  // Asking a string field whether it is true answers no, because it does not spell
  // "true". The pointer is the thing to look at.
  const char *path = document["result"]["file_path"];
  if (!path || !*path) {
    send(chat, "Telegram gave no path for that file.");
    return;
  }
  const String remote = path;

  String secret;
  {
    Hold hold(mutex);
    secret = token;
  }

  // Opening the flash erases two megabytes of it, which takes seconds and keeps the
  // processor to itself. Done after the connection was open, Telegram hung up on the
  // silence before the first byte ever arrived, so it happens first now.
  if (firmware && !firmwareBegin()) {
    send(chat, "The module would not open its flash for writing.");
    return;
  }

  if (!tlsGate || xSemaphoreTake(tlsGate, pdMS_TO_TICKS(50)) != pdTRUE) {
    if (firmware)
      firmwareEnd(false);
    send(chat, "Busy talking to the socket, try again in a moment.");
    return;
  }

  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA);
  client.setTimeout(TELEGRAM_TIMEOUT / 1000);

  HTTPClient http;
  http.setReuse(false);   // So end() really closes it before the gate goes to the next one
  http.setTimeout(TELEGRAM_TIMEOUT);
  if (!http.begin(client, String(TELEGRAM_HOST) + "/file/bot" + secret + "/" + remote)) {
    client.stop();   // Frees the encryption buffers before the gate lets the next one in
    xSemaphoreGive(tlsGate);
    if (firmware)
      firmwareEnd(false);
    send(chat, "Could not reach Telegram for the file itself.");
    return;
  }

  if (http.GET() != 200) {
    http.end();
    client.stop();   // Frees the encryption buffers before the gate lets the next one in
    xSemaphoreGive(tlsGate);
    if (firmware)
      firmwareEnd(false);
    send(chat, "Telegram refused to hand the file over.");
    return;
  }

  landing = true;
  FileWrapper card;
  if (!firmware) {
    card = storageFS.open("/" + name, "w");
    if (!card) {
      landing = false;
      http.end();
      client.stop();   // Frees the encryption buffers before the gate lets the next one in
      xSemaphoreGive(tlsGate);
      send(chat, "Could not create " + name + " on the card.");
      return;
    }
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t written = 0;
  bool good = true;
  const uint32_t started = millis();

  // Not readBytes: neither client overrides it, so Stream hands it over one byte at a
  // time, and for an encrypted one that is a trip through mbedtls per byte. When no
  // byte is there it spins on the clock without yielding. read(buffer, length) takes a
  // whole record in one call.
  uint32_t quiet = millis(), breaths = 0;
  while (size == 0 ? http.connected() : written < size) {
    size_t want = TELEGRAM_CHUNK;
    if (size && size - written < want)
      want = size - written;
    const int taken = stream->read(telegramBuffer, want);
    if (taken <= 0) {
      if (!http.connected() || millis() - quiet > 30000 || millis() - started > 15UL * 60 * 1000)
        break;
      vTaskDelay(1);
      continue;
    }
    quiet = millis();
    if ((++breaths & 0x0F) == 0)      // A stream that never runs dry would never yield,
      vTaskDelay(1);                  // and the loop feeding the printer shares this priority

    // An image for this chip starts with 0xE9, and a gcode file sent by mistake would
    // otherwise be written over the firmware before anyone noticed.
    if (firmware && written == 0 && (taken == 0 || telegramBuffer[0] != 0xE9)) {
      good = false;
      break;
    }

    if (!(firmware ? firmwareChunk(telegramBuffer, taken) : card.write(telegramBuffer, taken) == (size_t)taken)) {
      good = false;
      break;
    }
    written += taken;
  }

  const bool whole = good && (!size || written == size);
  String trouble;
  if (firmware)
    trouble = firmwareEnd(whole);
  else
    card.close();
  http.end();
  client.stop();   // Frees the encryption buffers before the gate lets the next one in
  xSemaphoreGive(tlsGate);
  landing = false;

  const uint32_t seconds = (millis() - started) / 1000;

  if (firmware) {
    if (trouble != "") {
      send(chat, "Firmware not flashed \u2014 " + trouble + "\n\n"
                 "Received   " + String(written) + " of " + String(size) + " bytes\n"
                 "Elapsed    " + String(seconds) + " s\n\n"
                 "The module is still running what it ran before.");
      return;
    }

    send(chat, "Firmware flashed \u2014 " + name + "\n\n"
               "Size       " + String(written / 1024) + " KB\n"
               "Transfer   " + String(seconds) + " s\n\n"
               "Rebooting. Send /status in half a minute.");
    flush();                 // The answer has to be out before the module goes down
    firmwareReboot();
    return;
  }

  if (!whole) {
    storageFS.remove("/" + name);
    send(chat, "The file did not arrive whole, nothing was kept.");
    return;
  }

  send(chat, "Saved \u2014 " + name + "\n\n"
             "Size       " + String(written / 1024) + " KB\n"
             "Transfer   " + String(seconds) + " s\n\n"
             "Send /files to see it, /print <number> to start it.");
}

// These are written from the web server's task and read from this one. A String handed
// between two tasks without a lock frees its buffer under the other's feet, and the heap
// does not survive it.
void TelegramBot::probe(const String url) {
  Hold hold(mutex);
  probeUrl = url;
}

void TelegramBot::fetchBench(const String url, const size_t chunk, const bool legacy) {
  Hold hold(mutex);
  benchChunk = chunk;
  benchLegacy = legacy;
  benchOutcome = "running";
  benchUrl = url;
}

uint32_t TelegramBot::stackLow() {
  return stackLowest == 0xFFFFFFFF ? 0 : stackLowest;
}

String TelegramBot::benchResult() {
  Hold hold(mutex);
  return benchOutcome;
}

// The same path a file from the bot takes, down to the card, against a server of our
// own: the only way to time it without sending a real file every time.
void TelegramBot::takeBench(const String url, const size_t chunk, const bool legacy) {
  if (!tlsGate || xSemaphoreTake(tlsGate, pdMS_TO_TICKS(50)) != pdTRUE) {
    { Hold hold(mutex); benchOutcome = "the other connection would not let go"; }
    return;
  }

  const bool encrypted = url.startsWith("https://");
  WiFiClientSecure secure;
  WiFiClient plain;
  secure.setInsecure();                 // Only ever our own server, for timing this path
  secure.setTimeout(TELEGRAM_TIMEOUT / 1000);
  plain.setTimeout(TELEGRAM_TIMEOUT / 1000);
  WiFiClient &client = encrypted ? (WiFiClient &)secure : plain;

  HTTPClient http;
  http.setReuse(false);   // So end() really closes it before the gate goes to the next one
  http.setTimeout(TELEGRAM_TIMEOUT);
  if (!http.begin(client, url)) {
    client.stop();   // Frees the encryption buffers before the gate lets the next one in
    xSemaphoreGive(tlsGate);
    { Hold hold(mutex); benchOutcome = "could not open the connection"; }
    return;
  }

  const int code = http.GET();
  if (code != 200) {
    http.end();
    client.stop();   // Frees the encryption buffers before the gate lets the next one in
    xSemaphoreGive(tlsGate);
    { Hold hold(mutex); benchOutcome = "answer " + String(code); }
    return;
  }

  landing = true;
  FileWrapper card = storageFS.open("/bench.tmp", "w");
  if (!card) {
    landing = false;
    http.end();
    client.stop();   // Frees the encryption buffers before the gate lets the next one in
    xSemaphoreGive(tlsGate);
    { Hold hold(mutex); benchOutcome = "could not create the file"; }
    return;
  }

  const size_t want = chunk && chunk <= TELEGRAM_CHUNK ? chunk : TELEGRAM_CHUNK;
  WiFiClient *stream = http.getStreamPtr();
  const size_t expected = http.getSize() > 0 ? (size_t)http.getSize() : 0;
  size_t written = 0;
  uint32_t reads = 0, waits = 0;
  const uint32_t started = millis();

  while (expected == 0 ? http.connected() : written < expected) {
    size_t piece = want;
    if (expected && expected - written < piece)
      piece = expected - written;
    int taken;
    if (legacy) {                      // The loop as it was: ask, sleep, then a byte at a time
      const size_t waiting = stream->available();
      if (!waiting) {
        ++waits;
        if (millis() - started > 300000)
          break;
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      taken = stream->readBytes(telegramBuffer, waiting > want ? want : waiting);
    }
    else
      taken = stream->read(telegramBuffer, piece);
    ++reads;
    if (taken <= 0) {
      ++waits;
      if (!http.connected() || millis() - started > 300000)
        break;
      vTaskDelay(1);
      continue;
    }
    if (card.write(telegramBuffer, taken) != (size_t)taken)
      break;
    written += taken;
  }

  const uint32_t ms = millis() - started;
  card.close();
  http.end();
  client.stop();   // Frees the encryption buffers before the gate lets the next one in
  xSemaphoreGive(tlsGate);
  landing = false;
  storageFS.remove("/bench.tmp");

  const String outcome = String(written) + " of " + String(expected) + " bytes in " + String(ms) + " ms, "
                         + String(written / (float)ms, 1) + " KB/s, chunk " + String(want)
                         + ", " + String(reads) + " reads, " + String(waits) + " waits";
  {
    Hold hold(mutex);
    benchOutcome = outcome;
  }
  trail("https bench: " + outcome);
}

// The firmware path on its own, so it can be walked against a server of our own instead
// of through Telegram, and every step of it says where it got to.
void TelegramBot::takeFirmware(const String url, const String chat) {
  trail("ota: starting, heap " + String(ESP.getFreeHeap()));

  if (!firmwareBegin()) {
    trail("ota: the flash would not open");
    return;
  }
  trail("ota: flash open, heap " + String(ESP.getFreeHeap()));

  if (!tlsGate || xSemaphoreTake(tlsGate, pdMS_TO_TICKS(50)) != pdTRUE) {
    firmwareEnd(false);
    trail("ota: the other connection would not let go");
    return;
  }

  WiFiClientSecure client;
  if (url.startsWith("https://api.telegram.org"))
    client.setCACert(TELEGRAM_ROOT_CA);
  else
    client.setInsecure();               // Only ever our own server, for walking this path
  client.setTimeout(TELEGRAM_TIMEOUT / 1000);

  HTTPClient http;
  http.setReuse(false);   // So end() really closes it before the gate goes to the next one
  http.setTimeout(TELEGRAM_TIMEOUT);
  if (!http.begin(client, url)) {
    client.stop();   // Frees the encryption buffers before the gate lets the next one in
    xSemaphoreGive(tlsGate);
    firmwareEnd(false);
    trail("ota: could not open the connection");
    return;
  }

  const int code = http.GET();
  const int length = http.getSize();
  trail("ota: answer " + String(code) + ", length " + String(length)
        + ", connected " + String(http.connected() ? "yes" : "no")
        + ", heap " + String(ESP.getFreeHeap()));

  if (code != 200) {
    http.end();
    client.stop();   // Frees the encryption buffers before the gate lets the next one in
    xSemaphoreGive(tlsGate);
    firmwareEnd(false);
    trail("ota: refused");
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  const size_t expected = length > 0 ? (size_t)length : 0;
  size_t written = 0;
  bool good = true;
  uint32_t quiet = millis();
  const uint32_t started = millis();

  while (expected == 0 || written < expected) {
    const size_t waiting = stream->available();
    if (!waiting) {
      if (!http.connected() && !stream->available()) {
        trail("ota: the other end hung up after " + String(written) + " bytes");
        break;
      }
      if (millis() - quiet > 30000) {
        trail("ota: nothing arrived for thirty seconds, " + String(written) + " bytes in");
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    quiet = millis();

    const int taken = stream->read(telegramBuffer, TELEGRAM_CHUNK);
    if (taken <= 0)
      continue;
    if (written == 0 && telegramBuffer[0] != 0xE9) {
      trail("ota: that is not an image for this chip");
      good = false;
      break;
    }
    if (!firmwareChunk(telegramBuffer, taken)) {
      trail("ota: the flash refused a block at " + String(written));
      good = false;
      break;
    }
    written += taken;
    if ((written & 0x3FFFF) < TELEGRAM_CHUNK)
      trail("ota: " + String(written / 1024) + " KB in, heap " + String(ESP.getFreeHeap()));
  }

  const bool whole = good && expected && written == expected;
  const String trouble = firmwareEnd(whole);
  http.end();
  client.stop();   // Frees the encryption buffers before the gate lets the next one in
  xSemaphoreGive(tlsGate);

  const uint32_t seconds = (millis() - started) / 1000;
  trail("ota: " + String(trouble == "" ? "flashed" : "failed, " + trouble)
        + ", " + String(written) + " of " + String(expected) + " bytes in " + String(seconds) + " s");

  if (chat != "") {
    if (trouble != "")
      send(chat, "Firmware not flashed \u2014 " + trouble + "\n\n"
                 "Received   " + String(written) + " of " + String(expected) + " bytes\n"
                 "Elapsed    " + String(seconds) + " s\n\n"
                 "The module is still running what it ran before.");
    else {
      send(chat, "Firmware flashed\n\n"
                 "Size       " + String(written / 1024) + " KB\n"
                 "Transfer   " + String(seconds) + " s\n\n"
                 "Rebooting. Send /status in half a minute.");
      flush();
    }
  }

  if (trouble == "")
    firmwareReboot();
}

void TelegramBot::run(void *argument) {
  int failures = 0;

  for (;;) {
    if (somethingIsStreaming() || tlsWanted) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (!hasToken() || WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    flush();      // Anything the loop left for us goes out before we settle down to wait

    // A refused or broken request comes back at once, and asking again straight away
    // would open a fresh encrypted connection many times a second until the heap gave
    // out. Each failure in a row waits longer, up to a minute.
    if (poll())
      failures = 0;
    else if (failures < 6)
      failures++;

    {
      const UBaseType_t left = uxTaskGetStackHighWaterMark(NULL);
      if (left < stackLowest)
        stackLowest = left;
    }

    vTaskDelay(pdMS_TO_TICKS(failures ? 1000 << (failures - 1) : 50));

    String waiting;
    {
      Hold hold(mutex);
      waiting = probeUrl;
      probeUrl = "";
    }
    if (waiting != "")
      takeFirmware(waiting, "");

    size_t chunk = 0;
    bool legacy = false;
    {
      Hold hold(mutex);
      waiting = benchUrl;
      benchUrl = "";
      chunk = benchChunk;
      legacy = benchLegacy;
    }
    if (waiting != "")
      takeBench(waiting, chunk, legacy);

    if (waitingId != "") {
      const String chat = waitingChat, id = waitingId, name = waitingName;
      const size_t size = waitingSize;
      waitingId = "";
      fetchDocument(chat, id, name, size);
    }
  }
}
