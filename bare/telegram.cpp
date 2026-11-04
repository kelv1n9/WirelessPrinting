#include "telegram.h"

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

extern bool isPrinting;
extern bool somethingIsStreaming();
extern bool relayCarriesTelegram();
extern bool relayConfigured();
extern void relaySendTelegram(const String &chat, const String &text);

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
int64_t TelegramBot::offset = -1;

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

static bool telegramPrimed = false;      // The first answer is only read for its place in the queue
static uint32_t coolingUntil = 0;        // Nothing is attempted before this
static uint8_t refusals = 0;             // Connections lost or refused in a row

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

  // With the relay set up the bot never opens a connection of its own, and the stack a
  // handshake needs is twelve kilobytes the relay's own handshake goes without.
  xTaskCreatePinnedToCore(run, "telegram", relayConfigured() ? 4096 : TELEGRAM_STACK, NULL, 1, &task, 0);
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


// With the relay up, Telegram talks to the Worker and the Worker talks to us down the
// socket that is already open. Two encrypted connections do not fit in this heap, and
// the one to the Worker is the one that must stay.
void TelegramBot::fromRelay(const String chat, const String text) {
  if (!allowed(chat)) {
    remember(chat);
    return;
  }
  deliver(chat, text);
}

void TelegramBot::send(const String chat, const String text) {
  Hold hold(mutex);

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

    if (!message["document"].isNull()) {
      send(chat, "This build does not take files over the bot yet. Send it through the "
                 "web page or the slicer.");
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

void TelegramBot::run(void *argument) {
  int failures = 0;

  for (;;) {
    if (somethingIsStreaming() || tlsWanted) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    // Once the relay is set up the bot never opens a connection of its own: while the
    // relay was reconnecting the bot's long poll took the memory its handshake needed,
    // and the relay never came back.
    if (relayConfigured()) {
      if (!relayCarriesTelegram()) {
        {
          Hold hold(mutex);
          status = "waiting for the relay, and for the Worker to hold the bot token";
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      for (;;) {
        Message one;
        {
          Hold hold(mutex);
          if (outboxHead == outboxTail)
            break;
          one = outbox[outboxTail];
          outboxTail = (outboxTail + 1) % QUEUE;
        }
        relaySendTelegram(one.chat, one.text);
      }
      {
        Hold hold(mutex);
        status = "through the relay";
      }
      vTaskDelay(pdMS_TO_TICKS(200));
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

    vTaskDelay(pdMS_TO_TICKS(failures ? 1000 << (failures - 1) : 50));
  }
}
