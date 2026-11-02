// The server had no authorisation at all and its pages are where the Yandex and the bot
// tokens get typed. These check the one thing the whole guard rests on: who is let past.

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include "stubs/Arduino.h"
#include "stubs/Preferences.h"
#include "stubs/ESPAsyncWebServer.h"

uint32_t testClock = 0;
ArAuthGuard asyncAuthGuard = nullptr;
Preferences preferences;
static std::string journal;
void logEvent(const String &text) { journal += text.s + "\n"; }

#include "../ESP8266WirelessPrintAsync/WebAuth.h"

static int failures = 0;
static const char *scenario = "";
static void check(const bool good, const char *what) {
  if (!good) { printf("  FAIL  %s: %s\n", scenario, what); failures++; }
}

// A caller who knows nothing, unless told otherwise
static AsyncWebServerRequest stranger() { return AsyncWebServerRequest(); }

int main() {
  scenario = "a password appears on its own and is remembered";
  {
    prefsStore().clear();
    webAuthBegin();
    const std::string first = webAuthPassword.s;
    check(first.size() == WEBAUTH_PASS_LEN, "it is as long as it was asked to be");
    check(webAuthIsFresh(), "and it knows the owner has not chosen it");
    check(webAuthKey().length() == WEBAUTH_KEY_LEN, "a key for the slicer came with it");
    check(asyncAuthGuard != nullptr, "and the server is no longer open");

    webAuthPassword = String("");           // as if the module had restarted
    webAuthBegin();
    check(webAuthPassword.s == first, "the same password came back after a restart");
  }

  scenario = "nothing readable is ambiguous on a small screen";
  {
    prefsStore().clear();
    std::set<std::string> seen;
    for (int i = 0; i < 200; i++) {
      const String made = webAuthInvent(WEBAUTH_PASS_LEN);
      check(made.length() == WEBAUTH_PASS_LEN, "the length holds");
      check(made.s.find_first_of("0O1lI") == std::string::npos, "no character that is read wrong");
      seen.insert(made.s);
    }
    check(seen.size() > 190, "and two of them are not the same");
  }

  scenario = "who is let past";
  {
    prefsStore().clear();
    webAuthBegin();
    const std::string password = webAuthPassword.s;
    const std::string key = webAuthKey().s;

    AsyncWebServerRequest nobody = stranger();
    check(!webAuthGuard(&nobody), "a caller with nothing is refused");
    check(nobody.challenged, "and is asked to log in");

    AsyncWebServerRequest wrong = stranger();
    wrong.offeredPassword = "not it";
    check(!webAuthGuard(&wrong), "the wrong password is refused");
    check(wrong.challenged, "and asked again");

    AsyncWebServerRequest right = stranger();
    right.offeredPassword = password;
    check(webAuthGuard(&right), "the right password gets in");
    check(!right.challenged, "without being asked");

    AsyncWebServerRequest slicer = stranger();
    slicer.hasApiKey = true;
    slicer.apiKey = key;
    check(webAuthGuard(&slicer), "the slicer's key gets in");

    AsyncWebServerRequest forged = stranger();
    forged.hasApiKey = true;
    forged.apiKey = key + "x";
    check(!webAuthGuard(&forged), "a key that is nearly right does not");
  }

  scenario = "an empty secret is never a skeleton key";
  {
    prefsStore().clear();
    webAuthBegin();
    webAuthApiKey = String("");             // as if it had never been stored

    AsyncWebServerRequest empty = stranger();
    empty.hasApiKey = true;
    empty.apiKey = "";
    check(!webAuthGuard(&empty), "an empty key against an empty key is refused");

    webAuthPassword = String("");
    AsyncWebServerRequest blank = stranger();
    blank.offeredPassword = "";
    check(!webAuthGuard(&blank), "and so is an empty password against an empty one");
  }

  scenario = "choosing a password replaces the invented one for good";
  {
    prefsStore().clear();
    webAuthBegin();
    webAuthSetPassword(String("a chosen one"));
    check(!webAuthIsFresh(), "it is no longer the module's own");

    webAuthPassword = String("");
    webAuthBegin();                          // restart
    check(webAuthPassword == "a chosen one", "and it survived the restart");
    check(!webAuthIsFresh(), "still not asking to be changed");

    AsyncWebServerRequest old = stranger();
    old.offeredPassword = "a chosen one";
    check(webAuthGuard(&old), "the chosen password works");
  }

  scenario = "a new key locks out the old one at once";
  {
    prefsStore().clear();
    webAuthBegin();
    const std::string was = webAuthKey().s;
    const String now = webAuthNewKey();
    check(now.s != was, "the key really changed");

    AsyncWebServerRequest stale = stranger();
    stale.hasApiKey = true;
    stale.apiKey = was;
    check(!webAuthGuard(&stale), "the old key is refused");

    AsyncWebServerRequest fresh = stranger();
    fresh.hasApiKey = true;
    fresh.apiKey = now.s;
    check(webAuthGuard(&fresh), "the new one is taken");
  }

  scenario = "the password is never written where an outsider could read it";
  {
    check(journal.find(webAuthPassword.s) == std::string::npos,
          "nothing put it in the journal");
  }

  printf(failures ? "\n%d checks failed\n" : "\nall checks passed\n", failures);
  return failures != 0;
}
