#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_websocket_client.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <atomic>
#include <ArduinoJson.h>
#include "relay.h"
#include "relayroots.h"
#include "telegram.h"
#include "relaypour.h"
#include "inflate.h"

void logEvent(const String &text);
bool printerWireBusy();
extern volatile uint32_t crumbRelay, crumbWs;   // RTC, survives a panic: which step the relay task was at

#define RELAY_FRAME_LIMIT   (24 * 1024)   // a request, body and all, as the Worker sends it
#define RELAY_CHUNK         2044          // plus four bytes of number makes one 2048 byte buffer, one frame
#define RELAY_IDLE_MS       20000
// What a TLS handshake is started with: two record buffers of 16.7 KB each have to be
// found whole, and about 65 KB stays taken once it is up. This used to ask for 45 KB
// in one piece, a guess. After WiFi had gone and come back the largest piece was
// 44.1 KB with 105 KB free, so the relay sat "waiting for memory" until the guardian
// restarted the module (found 05.11.2026 by dropping WiFi on purpose).
#define RELAY_BLOCK_FLOOR   (20 * 1024)
#define RELAY_HEAP_FLOOR    (80 * 1024)
#define RELAY_RETRY_MS      15000
#define RELAY_BEAT_MS       10000
#define RELAY_SILENT_MS     35000
#define RELAY_BODY_PIECES   4             // chunks of an upload waiting to be written or poured
#define RELAY_WINDOW        16384         // what a file is unpacked in: the card's block, 14 bits
#define RELAY_PUT_CREDIT    24            // pieces the Worker may have on the way to a file for the card
#define RELAY_SLOT_WAIT_MS  15000         // the socket waits this long for a slot: the card has been seen to stop for 4.5 s

static esp_websocket_client_handle_t ws;
static volatile uintptr_t wsBorn;      // which client the events are believed from: the newest
static char redialMark[1];             // put in the inbox when a live link is lost
static portMUX_TYPE wsLock = portMUX_INITIALIZER_UNLOCKED;

// The socket is taken out of use by whoever asks first, and only they shut it down:
// the loop does when the network goes, the relay's own task when the link is lost.
static esp_websocket_client_handle_t takeSocket() {
  portENTER_CRITICAL(&wsLock);
  const esp_websocket_client_handle_t had = ws;
  ws = nullptr;
  portEXIT_CRITICAL(&wsLock);
  return had;
}
static QueueHandle_t inbox;
// Upload pieces live in a few fixed slots that go round, rather than a fresh allocation
// for each: a thousand 2 KB mallocs in a row left the largest free block at 7 KB.
#define RELAY_SLOT_BYTES    2048
struct Piece {
  uint8_t slot;
  uint16_t len;
};
static uint8_t slots[RELAY_BODY_PIECES][RELAY_SLOT_BYTES];
static volatile bool taking;            // a file is being written from the slots right now
static volatile uint32_t takingFor;     // and this is its request
static std::atomic<uint32_t> owed;      // pieces written and not yet acknowledged
static QueueHandle_t bodies, freeSlots;
static uint8_t lastOp;
static TaskHandle_t worker;
static char *assembling;
static size_t assemblingRoom;
static volatile bool connected;
static volatile uint32_t linkEpoch;     // counts connections, so work begun on one is not carried on over the next
static String url, key, auth, state = "off";
static uint32_t nextTry;

// For the relay's own call to the status just before an upload. Made at boot, known only
// here and to the password check, and refused from anywhere but the loopback because
// nothing outside the module ever learns it.
String relayInternalKey;

// What the upload path has been doing, so the next problem is read and not guessed
static volatile uint32_t piecesIn, piecesDropped, acksSent, worstAckMs, worstGapMs, pourFails;
static volatile uint32_t putWaitMs, putWorkMs, putAckMs, putWriteMs, sendFails, putTaken, wsStackLeft, worstWriteMs;
static volatile bool workerHasBot;      // the Worker said it holds a bot token

// One Yandex request at a time, answered straight from the socket's task so that an
// upload being poured in by the other task cannot hold the answer up.
static SemaphoreHandle_t yandexAnswered;
static volatile uint32_t yandexAsked;
static String yandexBody;
static int yandexCode;

static const char *FORWARD[] = {"Content-Type", "Cache-Control", "WWW-Authenticate",
                                "Location", "Content-Disposition", "Content-Encoding"};

static void loadSettings() {
  Preferences keep;
  keep.begin("relay", true);
  url = keep.getString("url", "");
  key = keep.getString("key", "");
  keep.end();
}

bool relayConfigured() { return url != "" && key != ""; }
String relayUrl() { return url; }
String relayState() { return state; }

String relayStats() {
  return "pieces " + String(piecesIn) + ", dropped " + String(piecesDropped) +
         ", acks " + String(acksSent) + ", worst ack " + String(worstAckMs) +
         " ms, worst gap " + String(worstGapMs) + " ms, pour fails " + String(pourFails) +
         ", last put waited " + String(putWaitMs) + " ms, worked " + String(putWorkMs) +
         " ms (card " + String(putWriteMs) + " ms, worst write " + String(worstWriteMs) + " ms), acked " + String(putAckMs) + " ms, send fails " +
         String(sendFails) + ", socket stack left " + String(wsStackLeft) + ", taken " + String(putTaken) + ", heap " + String(heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

void relaySet(const String &u, const String &k) {
  Preferences keep;
  keep.begin("relay", false);
  keep.putString("url", u);
  if (k != "")
    keep.putString("key", k);
  keep.end();
  loadSettings();
  nextTry = 0;
}

void relayForget() {
  Preferences keep;
  keep.begin("relay", false);
  keep.clear();
  keep.end();
  loadSettings();
}

// The socket library writes each frame as a header and then a body. With Nagle on, the
// body waited for the header to be acknowledged: 90 ms on every answer, measured against
// a 75 ms round trip to Cloudflare. The library gives no way to its socket, so it is
// found by where it leads.
static void noDelay() {
  for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
    struct sockaddr_in peer;
    socklen_t n = sizeof peer;
    if (getpeername(fd, (struct sockaddr *)&peer, &n) || peer.sin_family != AF_INET ||
        peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK) || ntohs(peer.sin_port) == 80)
      continue;
    const int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
  }
}

static volatile uint32_t stopId;      // the download the Worker has called off
static volatile uint32_t heardAt;     // when anything last came from the Worker, a pong included

static void onEvent(void *born, esp_event_base_t, int32_t id, void *data) {
  if ((uintptr_t)born != wsBorn)
    return;                                  // the last words of a client already replaced
  esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;
  if ((id == WEBSOCKET_EVENT_DATA && e->op_code != 0x8) || id == WEBSOCKET_EVENT_CONNECTED)
    heardAt = millis();
  if (id == WEBSOCKET_EVENT_CONNECTED) {
    noDelay();
    linkEpoch++;
    connected = true;
    state = "connected";
    logEvent("remote access connected");
    return;
  }
  // A close said in words counts as a close. The library reads the frame and goes on
  // as if nothing had been said, and the link then looked alive for the seventy
  // seconds its ping takes to give up (found 06.11.2026 by closing it from the Worker).
  const bool closed = id == WEBSOCKET_EVENT_DATA && e->op_code == 0x8;
  if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR || closed) {
    const bool wasUp = connected;
    connected = false;
    if (wasUp) {
      logEvent(closed ? String("remote access closed by the other end")
                      : "remote access lost, last heard " + String((millis() - heardAt) / 1000) + " s ago");
      // The library waits ten seconds before it dials again and there is no setting
      // for it. A link that was up and is gone is dialled again at once, by a fresh
      // client, while this one is left to sit its ten seconds out and be put away.
      char *mark = redialMark;
      xQueueSend(inbox, &mark, 0);
    }
    connected = false;
    workerHasBot = false;
    state = "reconnecting";
    free(assembling);
    assembling = nullptr;
    return;
  }
  if (id != WEBSOCKET_EVENT_DATA || !e->data_len)
    return;
  if (e->op_code == 0x1 || e->op_code == 0x2)
    lastOp = e->op_code;
  else if (e->op_code != 0x0)
    return;                                  // pings and pongs
  if (lastOp == 0x2) {
    // A piece of an upload. The Worker sends each one small enough to arrive whole.
    // A piece can still arrive split across two reads of the socket, so it is put back
    // together before it is handed on. Dropping a part here lost bytes in the middle
    // of an upload and the file landed on the card shifted.
    static Piece cur;
    static bool holding;
    if (e->payload_len > RELAY_SLOT_BYTES)
      return;
    if (e->payload_offset == 0) {
      // While a file is being written, a full set of slots is waited for: the socket
      // is then not read, its window closes, and the pieces still on the way stay in
      // Cloudflare's buffers instead of this module's memory. That is what lets the
      // Worker keep four times as many in flight as there are slots. Nothing else is
      // ever waited for here: this is the socket's own task.
      if (!holding &&
          xQueueReceive(freeSlots, &cur.slot, taking ? pdMS_TO_TICKS(RELAY_SLOT_WAIT_MS) : 0) != pdTRUE) {
        piecesDropped++;
        return;
      }
      holding = true;
      cur.len = (uint16_t)e->payload_len;
    }
    if (!holding || e->payload_offset + e->data_len > cur.len)
      return;
    crumbWs = 100 + cur.slot;
    memcpy(slots[cur.slot] + e->payload_offset, e->data_ptr, e->data_len);
    crumbWs = 0;
    if (e->payload_offset + e->data_len == cur.len) {
      if (xQueueSend(bodies, &cur, 0) == pdTRUE) {
        holding = false;
        piecesIn++;
      }
      // Acknowledged from here and not from the task that writes the file. The library
      // holds its lock around this handler, so the writer, asking for that lock to send
      // while this waited for a slot only the writer could free, stopped both for three
      // seconds a time. The lock is recursive, so sending from inside it waits for nobody.
      if (taking && owed.load() >= 4) {
        char ack[48];
        const int n = snprintf(ack, sizeof ack, "{\"k\":%u,\"n\":%u}", (unsigned)takingFor, (unsigned)owed.exchange(0));
        const uint32_t t0 = millis();
        if (esp_websocket_client_send_text(ws, ack, n, pdMS_TO_TICKS(5000)) < 0)
          sendFails++;
        const uint32_t took = millis() - t0;
        putAckMs += took;
        if (took > worstAckMs)
          worstAckMs = took;
        acksSent++;
        const uint32_t left = uxTaskGetStackHighWaterMark(nullptr);
        if (!wsStackLeft || left < wsStackLeft)
          wsStackLeft = left;
      }
    }
    return;
  }
  if (e->payload_len == 1 && e->data_ptr[0] == 'q')
    return;                                  // the Worker's answer to a beat
  if (e->payload_len > RELAY_FRAME_LIMIT)
    return;
  if (e->payload_offset == 0) {
    free(assembling);
    assembling = (char *)malloc(e->payload_len + 1);
    assemblingRoom = e->payload_len;
  }
  if (!assembling)
    return;
  if (e->payload_len != (int)assemblingRoom || e->payload_offset < 0 || e->data_len < 0 ||
      (size_t)e->payload_offset + (size_t)e->data_len > assemblingRoom) {
    free(assembling);
    assembling = nullptr;
    return;
  }
  memcpy(assembling + e->payload_offset, e->data_ptr, e->data_len);
  if (e->payload_offset + e->data_len >= e->payload_len) {
    assembling[e->payload_len] = 0;
    if (!strncmp(assembling, "{\"t\":\"yxr\"", 10)) {
      // Parsed where it lies: the strings stay in the frame, so the document needs
      // room for four members and not for a second copy of a few kilobytes of answer
      DynamicJsonDocument doc(384);
      if (!deserializeJson(doc, assembling) && (uint32_t)(doc["q"] | 0) == yandexAsked) {
        yandexCode = doc["s"] | 0;
        yandexBody = (const char *)(doc["b"] | "");
        xSemaphoreGive(yandexAnswered);
      }
      free(assembling);
      assembling = nullptr;
      return;
    }
    if (!strncmp(assembling, "{\"t\":\"stop\",\"i\":", 16)) {
      // Whoever was downloading has gone. Seen here and not in the queue, where it
      // would wait behind the very download it is meant to end.
      stopId = strtoul(assembling + 16, nullptr, 10);
      free(assembling);
      assembling = nullptr;
      return;
    }
    // A request that brings a body. Its pieces come after it on this same socket, so
    // whatever is in the slots now belongs to a request that is over: one that was
    // refused, or one that broke off with pieces still on the way. Left there, they
    // took the slots of this file's first pieces, and its first attempt was lost.
    const char *l = strstr(assembling, ",\"l\":");
    if (!taking && (strstr(assembling, "\"t\":\"put\"") || (l && l[5] != '0')))
      for (Piece late; xQueueReceive(bodies, &late, 0) == pdTRUE;)
        xQueueSend(freeSlots, &late.slot, 0);
    if (xQueueSend(inbox, &assembling, 0) != pdTRUE)
      free(assembling);
    assembling = nullptr;
  }
}

// The next piece of a body, or false when none came for 20 s or the connection the
// request arrived on has gone. Without the second, a put cut off by a lost link went
// on waiting, and ate the first pieces of the Worker's next attempt on the new link.
static bool nextPiece(Piece &one, const uint32_t epoch) {
  for (uint32_t waited = 0; waited < RELAY_IDLE_MS; waited += 500) {
    if (!connected || linkEpoch != epoch)
      return false;
    if (xQueueReceive(bodies, &one, pdMS_TO_TICKS(500)) == pdTRUE)
      return true;
  }
  return false;
}

static void sendText(const String &text) {
  if (ws && connected &&
      esp_websocket_client_send_text(ws, text.c_str(), text.length(), pdMS_TO_TICKS(5000)) < 0)
    sendFails++;
}

static void answer(uint32_t id, int status, const char *message) {
  sendText("{\"i\":" + String(id) + ",\"s\":" + String(status) +
           ",\"h\":{\"Content-Type\":\"text/plain\"}}");
  const size_t n = strlen(message);
  uint8_t *frame = (uint8_t *)malloc(4 + n);
  if (frame) {
    memcpy(frame, &id, 4);
    memcpy(frame + 4, message, n);
    esp_websocket_client_send_bin(ws, (const char *)frame, 4 + n, pdMS_TO_TICKS(5000));
    free(frame);
  }
  sendText("{\"i\":" + String(id) + ",\"e\":1}");
}

String relayAuthHello();

void relayAuthChanged() { sendText(relayAuthHello()); }

bool relayCarriesTelegram() { return connected && workerHasBot; }

bool relayYandexReady() { return connected; }

bool relayYandexCall(const String &path, const String &payload, const String &bearer,
                     String &body, int &code) {
  static uint32_t asked;
  xSemaphoreTake(yandexAnswered, 0);              // nothing left over from a late answer
  yandexAsked = ++asked;
  DynamicJsonDocument doc(512 + payload.length() + bearer.length());
  doc["t"] = "yx";
  doc["q"] = yandexAsked;
  doc["p"] = path;
  doc["b"] = payload;
  doc["a"] = bearer;
  String out;
  serializeJson(doc, out);
  doc.clear();
  sendText(out);
  out = String();
  if (xSemaphoreTake(yandexAnswered, pdMS_TO_TICKS(20000)) != pdTRUE)
    return false;
  code = yandexCode;
  body = yandexBody;
  yandexBody = String();
  return true;
}

void relaySendTelegram(const String &chat, const String &text) {
  DynamicJsonDocument doc(text.length() + 128);
  doc["t"] = "tg";
  doc["c"] = chat;
  doc["x"] = text;
  String out;
  serializeJson(doc, out);
  sendText(out);
}

// The module answers nothing else while a file goes down, so the status the page keeps
// asking for is handed to the Worker first, to be shown from there meanwhile.
static void pushStatus() {
  WiFiClient local;
  if (!local.connect(IPAddress(127, 0, 0, 1), 80, 2000))
    return;
  local.print("GET /api/status HTTP/1.0\r\nHost: kp3s\r\nX-Relay-Internal: " + relayInternalKey + "\r\n\r\n");
  String reply;
  const uint32_t started = millis();
  while (millis() - started < 3000 && (local.connected() || local.available())) {
    while (local.available() && reply.length() < 4096)
      reply += (char)local.read();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  local.stop();
  const int at = reply.indexOf("\r\n\r\n");
  if (!reply.startsWith("HTTP/1.") || reply.indexOf(" 200 ") < 0 || at < 0)
    return;
  DynamicJsonDocument doc(reply.length() + 128);
  doc["t"] = "st";
  doc["b"] = reply.substring(at + 4);
  String out;
  serializeJson(doc, out);
  sendText(out);
}

static bool pour(WiFiClient &local, const uint8_t *data, size_t n) {
  return relayPourAll(local, data, n, 30000, [] { return (uint32_t)millis(); },
                      [] { vTaskDelay(pdMS_TO_TICKS(5)); });
}

void relaySendTelegramFetch(const String &chat, const String &fileId, const String &route,
                           const String &field, const String &name, const String &token) {
  DynamicJsonDocument doc(512 + name.length() + fileId.length());
  doc["t"] = "tgfetch";
  doc["c"] = chat;
  doc["id"] = fileId;
  doc["route"] = route;
  doc["field"] = field;
  doc["name"] = name;
  doc["token"] = token;
  String out;
  serializeJson(doc, out);
  sendText(out);
}

uint8_t *relayPutBlock();
int relayPutBegin(const String &said, const String &key, const String &once, const String &name, String &why);
void relayPutWrite(const uint8_t *data, size_t len);
void relayPutAbort();
int relayPutEnd(uint32_t size, bool andPrint, const String &path, String &body, String &type);

static uint8_t frame[4 + RELAY_CHUNK];   // an answer on its way up, one piece at a time

static void answerWith(uint32_t id, int status, const String &type, const String &body) {
  sendText("{\"i\":" + String(id) + ",\"s\":" + String(status) + ",\"h\":{\"Content-Type\":\"" + type + "\"}}");
  memcpy(frame, &id, 4);
  for (size_t at = 0; at < body.length(); at += RELAY_CHUNK) {
    const size_t n = body.length() - at < RELAY_CHUNK ? body.length() - at : RELAY_CHUNK;
    memcpy(frame + 4, body.c_str() + at, n);
    esp_websocket_client_send_bin(ws, (const char *)frame, 4 + n, pdMS_TO_TICKS(5000));
  }
  sendText("{\"i\":" + String(id) + ",\"e\":1}");
}

// A file the Worker took out of the browser's or the slicer's form and packed with
// deflate. It goes straight onto the card: no loopback, no form parser, and a quarter
// of the bytes across the internet. The loop keeps running meanwhile.
static void put(uint32_t id, JsonDocument &doc) {
  const String path = doc["p"] | "/api/files/local";
  const String name = doc["name"] | "upload.gcode";
  const uint32_t packed = doc["l"] | 0;
  const uint32_t size = doc["size"] | 0;
  const bool andPrint = doc["print"] | false;
  const bool deflated = doc["d"] | false;
  JsonObject h = doc["h"];
  const String said = h["authorization"] | "";
  const String key = h["x-api-key"] | "";
  const String once = h["x-relay-once"] | "";
  doc.clear();
  const uint32_t epoch = linkEpoch;

  // A file still going from the card to the printer is waited for. Empty
  // acknowledgements keep the Worker from taking the silence for a lost module.
  // The card is never begun on while that goes on, however long it takes.
  String why;
  int refused = -1;
  for (const uint32_t asked = millis(); connected && linkEpoch == epoch && millis() - asked < 180000;) {
    if (!printerWireBusy() && (refused = relayPutBegin(said, key, once, name, why)) != -1)
      break;
    sendText("{\"k\":" + String(id) + ",\"n\":0}");
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  crumbRelay = 1;
  pushStatus();
  if (refused == -1) {
    refused = 503;
    why = "files are still going to the printer, send it again in a minute";
  }
  if (refused) {
    answer(id, refused, (why + "\n").c_str());
    return;
  }

  // The window is the card's own block, so only the decoder's state is asked of the
  // heap. Without it the answer has the Worker send the form the long way round.
  static Inflate inflate;
  struct Unpacker {
    bool on;
    ~Unpacker() { if (on) inflate.end(); }
  } unpacker{deflated && inflate.begin(relayPutBlock(), RELAY_WINDOW)};
  if (!unpacker.on) {
    relayPutAbort();
    answer(id, 507, "no room to unpack this file\n");
    return;
  }

  crumbRelay = 2;
  // Said before the socket starts waiting for slots: from then on this task sends
  // nothing until the file is over. The first pieces are in the slots already.
  sendText("{\"k\":" + String(id) + ",\"n\":0,\"c\":" + String(RELAY_PUT_CREDIT) + "}");
  owed = 0;
  takingFor = id;

  crumbRelay = 4;
  uint32_t wrote = 0;
  auto writeOut = [&](const uint8_t *span, size_t n) {
    const uint32_t w0 = millis();
    crumbRelay = 6;
    relayPutWrite(span, n);
    const uint32_t one = millis() - w0;
    putWriteMs += one;
    if (one > worstWriteMs)
      worstWriteMs = one;
    crumbRelay = 5;
    wrote += n;
  };
  bool good = true;

  uint32_t taken = 0, waitedFrom = millis();
  putWaitMs = putWorkMs = putAckMs = putWriteMs = 0;
  taking = true;
  while (taken < packed && good) {
    Piece one;
    if (!nextPiece(one, epoch))
      break;
    const uint32_t gap = millis() - waitedFrom;
    if (gap > worstGapMs)
      worstGapMs = gap;
    putWaitMs += gap;
    const uint32_t workFrom = millis();
    uint32_t whose = 0;
    const uint8_t *data = slots[one.slot];
    if (one.len >= 4)
      memcpy(&whose, data, 4);
    if (one.len >= 4 && whose == id) {
      const size_t n = one.len - 4;
      good = inflate.feed(data + 4, n, taken + n >= packed, writeOut);
      taken += n;
      putTaken = taken;
    }
    xQueueSend(freeSlots, &one.slot, 0);
    if (whose == id)
      owed++;
    putWorkMs += millis() - workFrom;
    waitedFrom = millis();
  }
  // The Worker sends the next body only once this one is answered, so whatever is
  // still in the slots belongs to this file and is of no use to anyone.
  taking = false;
  for (Piece late; xQueueReceive(bodies, &late, 0) == pdTRUE;)
    xQueueSend(freeSlots, &late.slot, 0);

  crumbRelay = 7;
  if (taken < packed || !good || !inflate.finished() || wrote != size) {
    relayPutAbort();
    pourFails++;
    logEvent("a file through the relay broke off at " + String(taken) + " of " + String(packed) + " bytes: " +
             (!connected || linkEpoch != epoch ? "the link went" : !good ? "it did not unpack" : taken < packed ? "no piece for 20 s" :
              "it came out the wrong size") + ", pieces dropped so far " + String(piecesDropped));
    answer(id, 502, "the upload broke off on the way in\n");
    return;
  }
  crumbRelay = 8;
  String body, type;
  const int status = relayPutEnd(wrote, andPrint, path, body, type);
  answerWith(id, status, type, body);
  crumbRelay = 9;
}

int relayGetBegin(const String &said, const String &key, const String &name, uint32_t &size, String &why);
size_t relayGetRead(uint8_t *into, size_t len);
void relayGetEnd();

// A file off the card for a download, a piece at a time. The socket's own send
// buffer is the pace: each piece waits there until the last has left.
static void get(uint32_t id, JsonDocument &doc) {
  const String name = doc["name"] | "";
  JsonObject h = doc["h"];
  const String said = h["authorization"] | "";
  const String key = h["x-api-key"] | "";
  doc.clear();
  pushStatus();
  String why;
  uint32_t size = 0;
  const int refused = relayGetBegin(said, key, name, size, why);
  if (refused) {
    answer(id, refused, (why + "\n").c_str());
    return;
  }
  sendText("{\"i\":" + String(id) + ",\"s\":200,\"h\":{\"Content-Type\":\"application/octet-stream\","
           "\"Content-Length\":\"" + String(size) + "\"}}");
  memcpy(frame, &id, 4);
  uint32_t sent = 0;
  while (sent < size && connected && stopId != id) {
    const size_t n = relayGetRead(frame + 4, RELAY_CHUNK);
    if (!n || esp_websocket_client_send_bin(ws, (const char *)frame, 4 + n, pdMS_TO_TICKS(10000)) < 0)
      break;
    sent += n;
  }
  relayGetEnd();
  // Cut short is said so: the Worker then breaks the download off rather than end it
  // as if it were whole.
  sendText("{\"i\":" + String(id) + ",\"e\":1" + (sent == size ? "}" : ",\"x\":1}"));
}

static void carry(char *raw) {
  // Parsed where it lies, so the document holds a few dozen members and no text
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, raw))
    return;
  const char *kind = doc["t"] | "";
  if (!strcmp(kind, "hello")) {
    workerHasBot = doc["tg"] | false;
    sendText(relayAuthHello());
    return;
  }
  if (!strcmp(kind, "tgdoc")) {
    TelegramBot::fromRelay(String((const char *)(doc["c"] | "")),
                           String("/__file ") + (const char *)(doc["id"] | "") + " " +
                           String((uint32_t)(doc["size"] | 0)) + " " + (const char *)(doc["name"] | "file"));
    return;
  }
  if (!strcmp(kind, "get")) {
    const uint32_t gid = doc["i"] | 0;
    if (gid)
      get(gid, doc);
    return;
  }
  if (!strcmp(kind, "put")) {
    const uint32_t pid = doc["i"] | 0;
    if (pid)
      put(pid, doc);
    return;
  }
  if (!strcmp(kind, "tg")) {
    TelegramBot::fromRelay(String((const char *)(doc["c"] | "")), String((const char *)(doc["x"] | "")));
    return;
  }
  const uint32_t id = doc["i"] | 0;
  const char *method = doc["m"] | "GET";
  const char *path = doc["p"] | "/";
  const uint32_t bodyLen = doc["l"] | 0;
  if (!id)
    return;

  if (bodyLen)
    pushStatus();

  WiFiClient local;
  if (!local.connect(IPAddress(127, 0, 0, 1), 80, 3000)) {
    answer(id, 502, "the module's own web server did not take the connection\n");
    return;
  }
  // Each piece is 2040 bytes, a full segment and a short one. With Nagle on, the short
  // one waits for the full one to be acknowledged, and that acknowledgement is delayed:
  // tens of milliseconds a piece, most of the time an upload took.
  local.setNoDelay(true);
  String head = String(method) + " " + path + " HTTP/1.0\r\nHost: kp3s\r\nConnection: close\r\n";
  JsonObject h = doc["h"];
  for (JsonPair kv : h)
    head += String(kv.key().c_str()) + ": " + kv.value().as<const char *>() + "\r\n";
  head += "Content-Length: " + String(bodyLen) + "\r\n\r\n";
  doc.clear();
  if (!pour(local, (const uint8_t *)head.c_str(), head.length())) {
    local.stop();
    answer(id, 502, "the module's own web server stopped reading the request\n");
    return;
  }

  const uint32_t epoch = linkEpoch;
  uint32_t poured = 0, unacked = 0, waitedFrom = millis();
  while (poured < bodyLen) {
    Piece one;
    if (!nextPiece(one, epoch))
      break;
    const uint32_t gap = millis() - waitedFrom;
    if (gap > worstGapMs)
      worstGapMs = gap;
    uint32_t whose = 0;
    const uint8_t *data = slots[one.slot];
    if (one.len >= 4)
      memcpy(&whose, data, 4);
    bool good = true;
    if (one.len >= 4 && whose == id) {
      const size_t n = one.len - 4;
      good = pour(local, data + 4, n);
      poured += n;
    }
    xQueueSend(freeSlots, &one.slot, 0);     // a piece of a dropped request is just thrown away
    // Acknowledged two at a time: each send waits for the socket's own lock, and one per
    // piece made the pauses long enough for the web server to give up on the body.
    if (++unacked >= 2 || poured >= bodyLen) {
      const uint32_t t0 = millis();
      sendText("{\"k\":" + String(whose) + ",\"n\":" + String(unacked) + "}");
      const uint32_t took = millis() - t0;
      if (took > worstAckMs)
        worstAckMs = took;
      acksSent++;
      unacked = 0;
    }
    if (!good) {
      pourFails++;
      break;
    }
    waitedFrom = millis();
  }
  if (poured < bodyLen) {
    local.stop();
    answer(id, 502, "the upload broke off on the way in\n");
    return;
  }

  uint32_t last = millis();
  String line;
  int status = 0;
  String headers = "{";
  bool inHeaders = true;
  while (inHeaders && millis() - last < RELAY_IDLE_MS) {
    if (!local.available()) {
      if (!local.connected())
        break;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    const char ch = local.read();
    last = millis();
    if (ch == '\r')
      continue;
    if (ch != '\n') {
      if (line.length() < 512)
        line += ch;
      continue;
    }
    if (!status) {
      status = line.substring(line.indexOf(' ') + 1).toInt();
    }
    else if (line == "")
      inHeaders = false;
    else {
      const int colon = line.indexOf(':');
      if (colon > 0) {
        const String name = line.substring(0, colon);
        String value = line.substring(colon + 1);
        value.trim();
        value.replace("\\", "\\\\");
        value.replace("\"", "\\\"");
        for (const char *want : FORWARD)
          if (name.equalsIgnoreCase(want)) {
            headers += String(headers.length() > 1 ? "," : "") + "\"" + want + "\":\"" + value + "\"";
            break;
          }
      }
    }
    line = "";
  }
  headers += "}";
  if (!status) {
    local.stop();
    answer(id, 504, "the module's own web server did not answer\n");
    return;
  }
  sendText("{\"i\":" + String(id) + ",\"s\":" + String(status) + ",\"h\":" + headers + "}");

  memcpy(frame, &id, 4);
  last = millis();
  while (millis() - last < RELAY_IDLE_MS && connected) {
    const int ready = local.available();
    if (ready <= 0) {
      if (!local.connected())
        break;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    const int took = local.read(frame + 4, ready < RELAY_CHUNK ? ready : RELAY_CHUNK);
    if (took <= 0)
      continue;
    last = millis();
    if (esp_websocket_client_send_bin(ws, (const char *)frame, 4 + took, pdMS_TO_TICKS(10000)) < 0)
      break;
  }
  local.stop();
  sendText("{\"i\":" + String(id) + ",\"e\":1}");
}

static std::atomic<uint8_t> puttingAway;  // old clients not yet shut, each still holding its memory

static void putAway(void *old) {
  const bool ownTask = xTaskGetCurrentTaskHandle() != worker;
  esp_websocket_client_stop((esp_websocket_client_handle_t)old);
  esp_websocket_client_destroy((esp_websocket_client_handle_t)old);
  puttingAway--;
  if (ownTask)
    vTaskDelete(nullptr);
}

// A link that is down is dialled again by a fresh client, at once
static void redial() {
  const esp_websocket_client_handle_t old = connected ? nullptr : takeSocket();
  if (!old)
    return;
  wsBorn++;                            // nothing more is believed from it
  nextTry = millis() + 500;            // the loop starts the new one
  puttingAway++;
  // Put away on a task of its own: stopping it takes the ten seconds it sleeps,
  // and the requests on the new link must not wait behind that
  if (xTaskCreatePinnedToCore(putAway, "wsold", 4096, old, 1, nullptr, 0) != pdPASS)
    putAway(old);
}

// A beat every ten seconds, which the Worker answers without waking. It is how the
// Worker knows this module is there: a module whose power is cut says no goodbye, and
// Cloudflare went on holding its socket open for minutes, with every visitor waiting
// half a minute for an answer that could not come. It is also how this end knows the
// Worker is there, sooner than the seventy seconds the library's own ping takes.
static void beat() {
  static uint32_t beatAt;
  if (!connected || millis() - beatAt < RELAY_BEAT_MS)
    return;
  beatAt = millis();
  const int32_t silent = (int32_t)(millis() - heardAt);
  if (silent < RELAY_SILENT_MS) {
    sendText("p");
    return;
  }
  connected = false;
  workerHasBot = false;
  state = "reconnecting";
  logEvent("remote access silent for " + String(silent / 1000) + " s, dialling again");
  redial();
}

static void work(void *) {
  for (;;) {
    char *raw = nullptr;
    const bool got = xQueueReceive(inbox, &raw, pdMS_TO_TICKS(RELAY_BEAT_MS)) == pdTRUE;
    beat();
    if (!got || !raw)
      continue;
    if (raw == redialMark) {
      redial();
      continue;
    }
    carry(raw);
    free(raw);
  }
}

static void stopSocket() {
  const esp_websocket_client_handle_t had = takeSocket();
  if (!had)
    return;
  wsBorn++;                                  // nothing more is believed from it
  connected = false;
  esp_websocket_client_stop(had);
  esp_websocket_client_destroy(had);
}

void relayBegin() {
  loadSettings();
  inbox = xQueueCreate(6, sizeof(char *));
  {
    static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";
    relayInternalKey = "";
    for (int i = 0; i < 32; i++)
      relayInternalKey += alphabet[esp_random() % (sizeof(alphabet) - 1)];
  }
  yandexAnswered = xSemaphoreCreateBinary();
  bodies = xQueueCreate(RELAY_BODY_PIECES, sizeof(Piece));
  freeSlots = xQueueCreate(RELAY_BODY_PIECES, sizeof(uint8_t));
  for (uint8_t i = 0; i < RELAY_BODY_PIECES; i++)
    xQueueSend(freeSlots, &i, 0);
  xTaskCreatePinnedToCore(work, "relayio", 6144, nullptr, 1, &worker, 0);
}

void relayTick(const bool allowed) {
  if (!allowed || !relayConfigured() || WiFi.status() != WL_CONNECTED) {
    if (ws)
      stopSocket();
    state = !relayConfigured() ? "not set up" : !allowed ? "refused: no password is set" : "no network";
    return;
  }
  if (ws || (int32_t)(millis() - nextTry) < 0)
    return;
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < RELAY_BLOCK_FLOOR ||
      heap_caps_get_free_size(MALLOC_CAP_8BIT) < RELAY_HEAP_FLOOR) {
    state = "waiting for memory";
    // An old client being put away still holds its encrypted session, tens of
    // kilobytes that come back within a second or so: asked again soon, not in 15 s
    nextTry = millis() + (puttingAway ? 500 : RELAY_RETRY_MS);
    return;
  }

  auth = "Authorization: Bearer " + key + "\r\n";
  esp_websocket_client_config_t cfg = {};
  cfg.uri = url.c_str();
  if (url.startsWith("wss://"))
    cfg.cert_pem = RELAY_ROOTS;       // plain ws:// is for a Worker running on this network only
  cfg.headers = (char *)auth.c_str();
  cfg.buffer_size = 2048;
  cfg.task_stack = 6144;
  cfg.task_prio = 1;
  cfg.pingpong_timeout_sec = 60;
  ws = esp_websocket_client_init(&cfg);
  if (!ws) {
    state = "could not start";
    nextTry = millis() + RELAY_RETRY_MS;
    return;
  }
  esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, onEvent, (void *)++wsBorn);
  esp_websocket_client_start(ws);
  state = "connecting";
}
