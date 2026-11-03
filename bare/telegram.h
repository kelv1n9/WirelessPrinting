#pragma once

#include <Arduino.h>

// Carried over from the firmware that fed the printer line by line. Taking files and
// firmware over the bot went with it: both wrote to the card along a path that no
// longer exists. What is here is the part that was always separate, the talking.

// Telegram will not call us: the module sits behind the router with no address of its
// own. So it asks instead, holding a request open until something arrives. That wait
// belongs in a task of its own, because the loop it would otherwise block is the one
// feeding the printer.
//
// The task only carries messages. Commands are handed to the loop and acted on there,
// so nothing the printer owns is touched from two places at once.
class TelegramBot {
  public:
    static void begin();

    static bool hasToken();
    static void setToken(const String value);   // Kept in NVS, never handed back out
    static void forgetToken();

    static String getChats();     // Allowed, comma separated
    static String getSeen();      // Knocked and were turned away, offered for approval
    static void allow(const String chat);
    static void deny(const String chat);
    static bool allowed(const String chat);

    static String getStatus();

    // The loop's side of the wall
    static bool nextCommand(String &chat, String &text);
    static void send(const String chat, const String text);
    static void notify(const String text);      // To everyone allowed
    static void fromRelay(const String chat, const String text);   // A message the Worker took in

  private:
    struct Message {
      String chat, text;
    };

    static const int QUEUE = 8;

    static SemaphoreHandle_t mutex;
    static TaskHandle_t task;
    static String token, chats, seen, status;
    static Message inbox[QUEUE], outbox[QUEUE];
    static int inboxHead, inboxTail, outboxHead, outboxTail;
    static int64_t offset;

    static void run(void *argument);
    static bool poll();
    static void flush();
    static void deliver(const String chat, const String text);
    static void remember(const String chat);
    static bool call(const String path, String &body, const uint32_t timeout);
    static String encode(const String text);
};

extern TelegramBot telegramBot;
