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

// No comma and no colon in here. The challenge header separates its parameters with
// commas, and although a quoted value may hold one, Safari splits on them anyway and
// falls back to basic. The hint about the user name goes in the body instead.
#define WEBAUTH_REALM    "KP3S"
#define WEBAUTH_USER     "print"
#define WEBAUTH_KEY_LEN  32

// The one server, declared here because the guard below is what every route
// is registered through.
WebServer server(80);

static String webAuthPassword, webAuthApiKey;

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

// Whether this request may go through, without saying anything back. The upload path
// needs the answer long before there is a reply to put a refusal in.
static bool webAuthAllows() {
  if (!webAuthSet())
    return true;
  if (webAuthApiKey != "" && server.hasHeader("X-Api-Key")
      && server.header("X-Api-Key") == webAuthApiKey)
    return true;
  return server.authenticate(WEBAUTH_USER, webAuthPassword.c_str());
}

static bool webAuthLets() {
  if (webAuthAllows())
    return true;
  server.requestAuthentication(DIGEST_AUTH, WEBAUTH_REALM,
    F("<!doctype html><meta charset=utf-8><title>KP3S</title>"
      "<p style='font:15px system-ui;padding:1rem'>The user name is <b>print</b>. "
      "Reload the page to try again.</p>"));
  return false;
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
  keep.end();

  static const char *wanted[] = {"X-Api-Key"};
  server.collectHeaders(wanted, 1);
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
    static bool letting = false;
    if (server.upload().status == UPLOAD_FILE_START)
      letting = webAuthAllows();
    if (letting)
      upload();
  });
}
