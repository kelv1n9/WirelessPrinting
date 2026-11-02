#pragma once

#define COMMAND_BUFFER_SIZE   16
#define COMMAND_MAX_BYTES     320   // Matches GCODE_LINE_BYTES, so no line is ever cut short

#include <Arduino.h>

// The queue used to hold Strings and hand out copies of them. Copies are what killed it:
// peekSend runs on every pass of the main loop, so a printing module asked the heap for a
// small block and gave it back thousands of times a second. The heap stayed free but
// stopped being whole, and the first thing to need an unbroken block died. Here the text
// lives in one fixed array and callers get a pointer into it.
class CommandQueue {
  private:
    static int head, tail;
    static char commandBuffer[COMMAND_BUFFER_SIZE][COMMAND_MAX_BYTES];
    static SemaphoreHandle_t mutex;

    static inline int nextBufferSlot(int index) {
      int next = index + 1;

      return next >= COMMAND_BUFFER_SIZE ? 0 : next;
    }

  public:
    static void begin();

    static inline bool isEmpty() {
      return head == tail;
    }

    static int getFreeSlots();
    static void clear();
    static bool push(const char *command);
    static bool push(const String &command) { return push(command.c_str()); }

    // The next command to be sent, or NULL when there is none. The pointer stays good
    // until popSend, so a caller that keeps the text must copy it before that.
    static const char *peekSend();

    static void popSend();
};

extern CommandQueue commandQueue;
