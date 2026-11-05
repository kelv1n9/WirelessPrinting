#pragma once

// Who is allowed to talk to this module.
//
// Until a password is set nothing is refused, which is exactly where this module has
// always stood: it sits on the house network with no authorization at all. The page
// says so in a band across the top, and the relay to the outside will not start while
// that is true, because the house is not the threat the relay opens up.
//
// Once a password is set every route is behind it. Digest, not basic, because the
// module speaks plain HTTP inside the house and basic would put the password on the
// wire at every request. Digest never sends it.
//
// Orca cannot do digest and only knows the OctoPrint way, a key in a header, so that
// is accepted as well. The key does cross the house network as it is, which is why it
// is a separate secret from the password and can be rolled on its own.

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <MD5Builder.h>
#include <base64.h>
#include <mbedtls/sha256.h>

// No comma and no colon in here. The challenge header separates its parameters with
// commas, and although a quoted value may hold one, Safari splits on them anyway and
// falls back to basic. The hint about the user name goes in the body instead.
#define WEBAUTH_REALM    "KP3S"
#define WEBAUTH_USER     "print"
#define WEBAUTH_KEY_LEN  32

// The one server, declared here because the guard below is what every route
// is registered through.
WebServer server(80);
extern volatile uint32_t loopBeat;
extern volatile bool uploadActive;
extern String relayInternalKey;     // made at boot by the relay, never leaves the module

static String webAuthPassword, webAuthApiKey, webAuthNonce;

// A pass for a file sent to the Telegram bot. The Worker fetches it from Telegram and
// carries it down, and needs to get past the password to do so without ever being
// given the password or the slicer's key. One at a time, ten minutes.
static String webAuthOnceToken;
static uint32_t webAuthOnceUntil;

inline bool webAuthSet() { return webAuthPassword != ""; }
inline const String &webAuthKey() { return webAuthApiKey; }

// No letters or digits that are read wrong off a small screen: 0 O o 1 l I
static String webAuthInvent(const unsigned int length) {
  static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";
  String out;
  out.reserve(length);
  for (unsigned int i = 0; i < length; i++)
    out += alphabet[esp_random() % (sizeof(alphabet) - 1)];
  return out;
}

static String webAuthMd5(const String &of) {
  MD5Builder md5;
  md5.begin();
  md5.add(of);
  md5.calculate();
  return md5.toString();
}

// name="value", or name=value up to a comma, whichever the client wrote
static String webAuthField(const String &from, const char *name) {
  const String key = String(name) + "=";
  int at = from.indexOf(key);
  while (at > 0 && from[at - 1] != ' ' && from[at - 1] != ',')
    at = from.indexOf(key, at + 1);        // cnonce must not be found inside nonce
  if (at < 0)
    return "";
  at += key.length();
  if (from[at] == '"') {
    const int end = from.indexOf('"', at + 1);
    return end < 0 ? String("") : from.substring(at + 1, end);
  }
  int end = from.indexOf(',', at);
  if (end < 0)
    end = from.length();
  String out = from.substring(at, end);
  out.trim();
  return out;
}

static const char *webAuthVerb() {
  switch (server.method()) {
    case HTTP_POST:   return "POST";
    case HTTP_PUT:    return "PUT";
    case HTTP_DELETE: return "DELETE";
    case HTTP_PATCH:  return "PATCH";
    case HTTP_HEAD:   return "HEAD";
    default:          return "GET";
  }
}

// Digest worked out here rather than by the library, because the library keeps one
// nonce for the whole server and makes a new one with every refusal. A browser asks
// for a page and its icon and its data at the same time: each refusal overwrites the
// nonce the others were answered with, every answer then fails to add up, and a right
// password reads as a wrong one. This nonce is made once and left alone.
static bool webAuthDigestFits(const String &said, const char *verb) {
  if (webAuthField(said, "username") != WEBAUTH_USER)
    return false;
  if (webAuthField(said, "nonce") != webAuthNonce)
    return false;
  const String uri = webAuthField(said, "uri");
  const String answer = webAuthField(said, "response");
  if (uri == "" || answer == "")
    return false;

  const String ha1 = webAuthMd5(String(WEBAUTH_USER) + ":" + WEBAUTH_REALM + ":" + webAuthPassword);
  const String ha2 = webAuthMd5(String(verb) + ":" + uri);

  const String qop = webAuthField(said, "qop");
  String ours;
  if (qop == "auth")
    ours = webAuthMd5(ha1 + ":" + webAuthNonce + ":" + webAuthField(said, "nc") + ":" +
                      webAuthField(said, "cnonce") + ":auth:" + ha2);
  else
    ours = webAuthMd5(ha1 + ":" + webAuthNonce + ":" + ha2);
  return ours.equalsIgnoreCase(answer);
}

static bool webAuthBasicFits(const String &said) {
  String wanted = base64::encode(String(WEBAUTH_USER) + ":" + webAuthPassword);
  wanted.trim();
  return said == wanted;
}

// Whether this request may go through, without saying anything back. The upload path
// needs the answer long before there is a reply to put a refusal in.
// The check itself, given what a request carried. The web server's requests and the
// files the relay writes straight to the card both come through here.
static bool webAuthAllowsWith(const char *verb, const String &said, const String &key,
                              const String &once, const String &internal) {
  if (!webAuthSet())
    return true;
  if (relayInternalKey.length() == 32 && internal == relayInternalKey)
    return true;
  if (webAuthOnceToken != "" && (int32_t)(webAuthOnceUntil - millis()) > 0 && once == webAuthOnceToken)
    return true;
  if (webAuthApiKey != "" && key == webAuthApiKey)
    return true;
  if (said.startsWith("Digest "))
    return webAuthDigestFits(said.substring(7), verb);
  if (said.startsWith("Basic "))
    return webAuthBasicFits(said.substring(6));
  return false;
}

static bool webAuthAllows() {
  return webAuthAllowsWith(webAuthVerb(),
                           server.hasHeader("Authorization") ? server.header("Authorization") : String(),
                           server.hasHeader("X-Api-Key") ? server.header("X-Api-Key") : String(),
                           server.hasHeader("X-Relay-Once") ? server.header("X-Relay-Once") : String(),
                           server.hasHeader("X-Relay-Internal") ? server.header("X-Relay-Internal") : String());
}

static bool webAuthLets() {
  if (webAuthAllows())
    return true;
  server.sendHeader("WWW-Authenticate",
    "Digest realm=\"" WEBAUTH_REALM "\", qop=\"auth\", nonce=\"" + webAuthNonce +
    "\", opaque=\"0\"");
  server.send(401, "text/html",
    F("<!doctype html><meta charset=utf-8><title>KP3S</title>"
      "<p style='font:15px system-ui;padding:1rem'>The user name is <b>print</b>. "
      "Reload the page to try again.</p>"));
  return false;
}

// What the relay's Worker needs to check a request the same way this module does, so it
// can answer from its cache while the module is busy: the digest hash of user, realm
// and password (the password itself cannot be got back from it), the nonce in use, and
// a SHA-256 of the slicer's key. Never shown anywhere, sent only down the relay.
inline String webAuthForWorker() {
  if (!webAuthSet())
    return "{\"t\":\"auth\",\"open\":true}";
  uint8_t digest[32];
  mbedtls_sha256((const unsigned char *)webAuthApiKey.c_str(), webAuthApiKey.length(), digest, 0);
  String keyHash;
  for (uint8_t b : digest) {
    if (b < 16) keyHash += '0';
    keyHash += String(b, HEX);
  }
  return String("{\"t\":\"auth\",\"user\":\"" WEBAUTH_USER "\",\"realm\":\"" WEBAUTH_REALM "\",\"nonce\":\"") +
         webAuthNonce + "\",\"ha1\":\"" +
         webAuthMd5(String(WEBAUTH_USER) + ":" + WEBAUTH_REALM + ":" + webAuthPassword) +
         "\",\"key\":\"" + keyHash + "\"}";
}

inline String webAuthMakeOnce() {
  webAuthOnceToken = webAuthInvent(32);
  webAuthOnceUntil = millis() + 600000;
  return webAuthOnceToken;
}

inline void webAuthSetPassword(const String &chosen) {
  webAuthPassword = chosen;
  Preferences keep;
  keep.begin("webauth", false);
  keep.putString("pass", webAuthPassword);
  keep.end();
}

inline void webAuthForget() {
  webAuthPassword = "";
  Preferences keep;
  keep.begin("webauth", false);
  keep.remove("pass");
  keep.end();
}

inline String webAuthNewKey() {
  webAuthApiKey = webAuthInvent(WEBAUTH_KEY_LEN);
  Preferences keep;
  keep.begin("webauth", false);
  keep.putString("key", webAuthApiKey);
  keep.end();
  return webAuthApiKey;
}

inline void webAuthBegin() {
  Preferences keep;
  keep.begin("webauth", false);
  webAuthPassword = keep.getString("pass", "");
  webAuthApiKey = keep.getString("key", "");
  if (webAuthApiKey == "") {
    webAuthApiKey = webAuthInvent(WEBAUTH_KEY_LEN);
    keep.putString("key", webAuthApiKey);
  }
  // Kept across reboots: the relay sends an upload again if it broke off, with the
  // answer it already worked out, and a nonce made fresh at every boot turned the second
  // try into a 401 whenever the first had ended in a restart.
  webAuthNonce = keep.getString("nonce", "");
  if (webAuthNonce.length() != 32) {
    webAuthNonce = webAuthInvent(32);
    keep.putString("nonce", webAuthNonce);
  }
  keep.end();

  static const char *wanted[] = {"X-Api-Key", "X-Relay-Once", "X-Relay-Internal"};
  server.collectHeaders(wanted, 3);
}

// Every route goes on through here, so there is nothing to remember per route. The
// upload callback runs while the body is still arriving, long before the handler, so
// it is asked separately: by the time the handler could refuse, the file would already
// be on the card and the firmware already written.
inline void route(const char *uri, const HTTPMethod method,
                  WebServer::THandlerFunction fn) {
  server.on(uri, method, [fn]() { if (webAuthLets()) fn(); });
}

inline void route(const char *uri, const HTTPMethod method,
                  WebServer::THandlerFunction fn, WebServer::THandlerFunction upload) {
  server.on(uri, method, [fn]() { if (webAuthLets()) fn(); }, [upload]() {
    loopBeat = millis();          // a long upload is one call of the loop, and not a hang
    const int phase = server.upload().status;
    uploadActive = phase == UPLOAD_FILE_START || phase == UPLOAD_FILE_WRITE;
    static bool letting = false;
    if (server.upload().status == UPLOAD_FILE_START)
      letting = webAuthAllows();
    if (letting)
      upload();
  });
}
