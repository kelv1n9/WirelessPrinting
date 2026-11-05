// Replays the boot guard against a simulated module: NVS that survives a reboot, two
// firmware slots, a loader that can refuse an image, and a clock the test owns.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <setjmp.h>
#include <map>
#include <string>

uint32_t testClock = 0;
jmp_buf testRestartJump;
int testRestarts = 0;
std::map<std::string, uint32_t> testNvs;
bool testNvsWorks = true;

#include "stubs/Arduino.h"
#include "stubs/esp_partition.h"

esp_partition_t testApp0 = { 0x10000, "app0" };
esp_partition_t testApp1 = { 0x200000, "app1" };
esp_partition_t *testRunning = &testApp0;
esp_partition_t *testBootSet = &testApp0;
bool testApp0Written = true, testApp1Written = true;

#include "../ESP8266WirelessPrintAsync/BootGuard.h"

static int failures = 0;
static const char *scenario = "";

static void check(const bool ok, const char *what) {
  if (!ok) {
    printf("  FAIL  %s: %s\n", scenario, what);
    ++failures;
  }
}

// One power up. Returns true when the guard sent the module back to the other slot.
// up_ms is how long the module then stays alive, online says whether wifi came up.
static bool boot(const uint32_t up_ms, const bool online) {
  guardOpen = guardArmed = guardConfirmed = guardQuickCleared = guardQuickCounted = false;
  guardStore = 0;
  guardBootAt = guardOnlineSince = 0;
  guardOnline = false;
  guardTrialsLeft = guardQuickCount = 0;
  guardVerdict = "not started";

  testRunning = testBootSet;           // The loader hands control to whatever is marked
  const int before = testRestarts;
  testClock = 1000;

  if (setjmp(testRestartJump) == 0) {
    bootGuardBegin();
    for (uint32_t t = 0; t <= up_ms; t += 1000) {
      testClock = 1000 + t;
      bootGuardWatch(online);
    }
  }
  return testRestarts > before;
}

static void freshFlash() {
  testNvs.clear();
  testRestarts = 0;
  testRunning = testBootSet = &testApp0;
  testApp0Written = testApp1Written = true;
  testNvsWorks = true;
}

// What an upload does once the image is written and the loader has been pointed at it
static void uploadInto(esp_partition_t *slot) {
  testBootSet = slot;
  bootGuardArm();
}

int main() {
  const uint32_t LONG = BOOTGUARD_HEALTH_MS + 5000;

  scenario = "cold module just runs";
  freshFlash();
  check(!boot(LONG, true), "must not fall back");
  check(testNvs["trials"] == 0, "no trial is pending");
  check(strcmp(guardVerdict, "settled") == 0, guardVerdict);

  scenario = "good upload confirms itself";
  freshFlash();
  uploadInto(&testApp1);
  check(testNvs["trials"] == BOOTGUARD_TRIALS, "armed");
  check(!boot(LONG, true), "must not fall back");
  check(testRunning == &testApp1, "stays on the new image");
  check(testNvs["trials"] == 0, "trial cleared");
  check(strcmp(guardVerdict, "confirmed") == 0, guardVerdict);

  scenario = "image that keeps rebooting goes back";
  freshFlash();
  uploadInto(&testApp1);
  check(!boot(5000, true), "first boot is a free try");
  check(testRunning == &testApp1, "still trying the new one");
  check(boot(5000, true), "second boot must fall back");
  check(testBootSet == &testApp0, "pointed back at the old slot");
  check(!boot(LONG, true), "and then stays put");
  check(testRunning == &testApp0, "running the old image");

  scenario = "image that never reaches the network goes back";
  freshFlash();
  uploadInto(&testApp1);
  check(boot(BOOTGUARD_OFFLINE_MS + 5000, false), "must fall back on its own");
  check(testBootSet == &testApp0, "pointed back at the old slot");

  scenario = "network that comes up too late does not count";
  freshFlash();
  uploadInto(&testApp1);
  check(!boot(BOOTGUARD_HEALTH_MS - 5000, true), "first boot is a free try");
  check(testNvs["trials"] == 1, "still on trial");
  check(boot(BOOTGUARD_HEALTH_MS - 5000, true), "second short boot falls back");

  scenario = "empty spare slot is never booted into";
  freshFlash();
  testApp1Written = false;
  testNvs["trials"] = 1;
  testNvs["pending"] = testApp0.address;
  check(!boot(5000, true), "must keep running instead");
  check(testBootSet == &testApp0, "boot slot untouched");
  check(strcmp(guardVerdict, "nowhere to fall back to") == 0, guardVerdict);

  scenario = "with no spare a healthy image is still kept";
  freshFlash();
  testApp1Written = false;
  testNvs["trials"] = 1;
  testNvs["pending"] = testApp0.address;
  check(!boot(LONG, true), "must keep running");
  check(strcmp(guardVerdict, "confirmed") == 0, guardVerdict);

  scenario = "loader already refused the image";
  freshFlash();
  testNvs["trials"] = BOOTGUARD_TRIALS;
  testNvs["pending"] = testApp1.address;   // we were testing app1, yet we woke in app0
  check(!boot(LONG, true), "nothing left to do");
  check(testNvs["trials"] == 0, "trial forgotten");
  check(strcmp(guardVerdict, "the loader turned the image down on its own") == 0, guardVerdict);

  scenario = "three flicks of the socket send it back";
  freshFlash();
  testBootSet = &testApp1; testRunning = &testApp1;   // settled on app1, no trial pending
  check(!boot(5000, true), "one");
  check(!boot(5000, true), "two");
  check(boot(5000, true), "three must fall back");
  check(testBootSet == &testApp0, "pointed at the other slot");

  scenario = "the supply bouncing as the printer comes on does not count";
  freshFlash();
  testBootSet = &testApp1; testRunning = &testApp1;
  for (int i = 0; i < 6; i++)
    check(!boot(500, true), "a reset within the first moment is not a flick");
  check(testBootSet == &testApp1, "a confirmed image stays");
  check(!boot(5000, true), "a real flick still counts, one");
  check(!boot(5000, true), "two");
  check(boot(5000, true), "and three still sends it back");

  scenario = "flicks do not add up once the module has been up a while";
  freshFlash();
  testBootSet = &testApp1; testRunning = &testApp1;
  check(!boot(5000, true), "one");
  check(!boot(BOOTGUARD_QUICK_MS + 5000, true), "a long run clears the count");
  check(testNvs["quick"] == 0, "count cleared");
  check(!boot(5000, true), "one again");
  check(!boot(5000, true), "two, still not three");
  check(testBootSet == &testApp1, "stayed where it was");

  scenario = "a settled image survives any number of restarts";
  freshFlash();
  for (int i = 0; i < 20; i++)
    check(!boot(BOOTGUARD_QUICK_MS + 5000, true), "never falls back");
  check(testBootSet == &testApp0, "same slot throughout");

  scenario = "no settings storage means no net, but also no harm";
  freshFlash();
  testNvsWorks = false;
  check(!boot(LONG, true), "must not fall back");
  check(strcmp(guardVerdict, "settings unavailable, running without a net") == 0, guardVerdict);

  printf(failures ? "\n%d checks failed\n" : "\nall checks passed\n", failures);
  return failures != 0;
}
