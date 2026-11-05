#pragma once

#include <Arduino.h>

// The way in from outside the house. The module dials out to a Cloudflare Worker and
// keeps one WebSocket open; requests come down it and are put to the module's own web
// server over the loopback, so every route and the password behave exactly as they do
// at home. It is never started while the module has no password.
void relayBegin();
void relayTick(bool allowed);         // from the loop; allowed is "a password is set"
bool relayConfigured();
void relaySet(const String &url, const String &key);   // key kept in NVS, never handed back
void relayForget();
String relayUrl();
String relayState();
String relayStats();
void relayAuthChanged();          // the password or the slicer key changed; tell the Worker
void relaySendTelegramFetch(const String &chat, const String &fileId, const String &route,
                           const String &field, const String &name, const String &token);
