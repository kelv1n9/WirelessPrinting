#pragma once

// Everything the web server answers goes through here first. The hook it installs is
// the patched library's single choke point, so a route cannot be left open by being
// forgotten: there is nothing to remember per route.
//
// Digest, not basic, because the module speaks plain HTTP inside the house and basic
// would put the password on the wire at every request. Digest never sends it at all.
// From outside the house nothing reaches this directly: the plan is one outgoing TLS
// connection to a relay, so there is no port to knock on.
//
// Orca cannot do digest and only knows the OctoPrint way, a key in a header, so that is
// accepted as well. The key does cross the house network as it is, which is why it is a
// separate secret from the password and can be rolled on its own.

#include <Arduino.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>

#define WEBAUTH_REALM     "WirelessPrinting"
#define WEBAUTH_USER      "print"
#define WEBAUTH_PASS_LEN  10
#define WEBAUTH_KEY_LEN   32

extern Preferences preferences;
void logEvent(const String &text);

static String webAuthPassword, webAuthApiKey;
static bool webAuthUntouched;        // Still the password the module made up for itself

// No letters or digits that are read wrong off a small screen: 0 O o 1 l I
static String webAuthInvent(const unsigned int length) {
  static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";
  String out;
  out.reserve(length);
  for (unsigned int i = 0; i < length; i++)
    out += alphabet[esp_random() % (sizeof(alphabet) - 1)];
  return out;
}

static bool webAuthGuard(AsyncWebServerRequest *request) {
  if (webAuthApiKey != "" && request->hasHeader("X-Api-Key")
      && request->getHeader("X-Api-Key")->value() == webAuthApiKey)
    return true;

  if (request->authenticate(WEBAUTH_USER, webAuthPassword.c_str(), WEBAUTH_REALM))
    return true;

  request->requestAuthentication(WEBAUTH_REALM);
  return false;
}

inline bool webAuthIsFresh() { return webAuthUntouched; }
inline const String &webAuthKey() { return webAuthApiKey; }

// Shown on the printer's screen and sent to Telegram, never written to a web page and
// never put in a reply, because anyone who can read those already got past this.
inline String webAuthGreeting() {
  return "Web password " + webAuthPassword + ", user " + WEBAUTH_USER;
}

inline void webAuthSetPassword(const String &chosen) {
  webAuthPassword = chosen;
  webAuthUntouched = false;
  preferences.begin("wirelessprint", false);
  preferences.putString("webpass", webAuthPassword);
  preferences.putBool("webfresh", false);
  preferences.end();
  logEvent("the web password was changed");
}

inline String webAuthNewKey() {
  webAuthApiKey = webAuthInvent(WEBAUTH_KEY_LEN);
  preferences.begin("wirelessprint", false);
  preferences.putString("webkey", webAuthApiKey);
  preferences.end();
  logEvent("a new key was made for the slicer");
  return webAuthApiKey;
}

inline void webAuthBegin() {
  preferences.begin("wirelessprint", false);
  webAuthPassword = preferences.getString("webpass", "");
  webAuthApiKey = preferences.getString("webkey", "");
  webAuthUntouched = preferences.getBool("webfresh", true);

  if (webAuthPassword == "") {
    webAuthPassword = webAuthInvent(WEBAUTH_PASS_LEN);
    webAuthUntouched = true;
    preferences.putString("webpass", webAuthPassword);
    preferences.putBool("webfresh", true);
  }
  if (webAuthApiKey == "") {
    webAuthApiKey = webAuthInvent(WEBAUTH_KEY_LEN);
    preferences.putString("webkey", webAuthApiKey);
  }
  preferences.end();

  asyncAuthGuard = webAuthGuard;      // From here on every route is behind it
}
