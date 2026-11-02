// The queue once had two tails and only one of them ever moved, so it jammed after
// fifteen commands and never took another. That is what these check.

#include <cstdio>
#include <cstring>
#include "stubs/Arduino.h"
#include "../ESP8266WirelessPrintAsync/CommandQueue.h"
#include "../ESP8266WirelessPrintAsync/CommandQueue.cpp"

uint32_t testClock = 0;
static int failures = 0;
static const char *scenario = "";

static void check(const bool good, const char *what) {
  if (!good) { printf("  FAIL  %s: %s\n", scenario, what); failures++; }
}

int main() {
  commandQueue.begin();

  scenario = "it keeps taking commands long after it is first filled";
  {
    commandQueue.clear();
    for (int round = 0; round < 500; round++) {
      char line[32];
      snprintf(line, sizeof line, "G1 X%d", round);
      check(commandQueue.push(line), "pushed");
      const char *out = commandQueue.peekSend();
      check(out && !strcmp(out, line), "came back whole");
      commandQueue.popSend();
      check(commandQueue.isEmpty(), "and the queue is empty again");
    }
  }

  scenario = "it fills to capacity, drains, and fills again";
  {
    commandQueue.clear();
    int taken = 0;
    while (commandQueue.push("M105")) taken++;
    check(taken == COMMAND_BUFFER_SIZE - 1, "one slot is kept back to tell full from empty");
    check(!commandQueue.isEmpty(), "a full queue is not empty");
    check(commandQueue.getFreeSlots() == 0, "and has no room");

    int given = 0;
    while (commandQueue.peekSend()) { commandQueue.popSend(); given++; }
    check(given == taken, "everything pushed came back out");
    check(commandQueue.isEmpty(), "and it is empty");
    check(commandQueue.push("M114"), "and it takes commands again");
  }

  scenario = "order is kept and nothing is handed out twice";
  {
    commandQueue.clear();
    for (int i = 0; i < 10; i++) { char l[16]; snprintf(l, sizeof l, "N%d", i); commandQueue.push(l); }
    for (int i = 0; i < 10; i++) {
      char want[16]; snprintf(want, sizeof want, "N%d", i);
      const char *out = commandQueue.peekSend();
      check(out && !strcmp(out, want), want);
      commandQueue.popSend();
    }
    check(commandQueue.peekSend() == NULL, "nothing is left");
  }

  scenario = "an over long command is cut, not allowed to run off the slot";
  {
    commandQueue.clear();
    char huge[COMMAND_MAX_BYTES * 2];
    memset(huge, 'G', sizeof huge - 1);
    huge[sizeof huge - 1] = 0;
    check(commandQueue.push(huge), "taken");
    const char *out = commandQueue.peekSend();
    check(out && strlen(out) == COMMAND_MAX_BYTES - 1, "cut to the slot");
    check(!commandQueue.push(""), "an empty command is refused");
    check(!commandQueue.push(NULL), "and so is none at all");
  }

  printf(failures ? "\n%d checks failed\n" : "\nall checks passed\n", failures);
  return failures != 0;
}
