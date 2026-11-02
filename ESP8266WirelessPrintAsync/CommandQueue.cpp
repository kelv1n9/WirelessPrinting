#include "CommandQueue.h"
#include <string.h>

CommandQueue commandQueue;    //FIFO Queue

int CommandQueue::head = 0;
int CommandQueue::tail = 0;
char CommandQueue::commandBuffer[COMMAND_BUFFER_SIZE][COMMAND_MAX_BYTES];
SemaphoreHandle_t CommandQueue::mutex = NULL;

class QueueLock {
  public:
    inline QueueLock(SemaphoreHandle_t mutex) : mutex(mutex) {
      if (mutex)
        xSemaphoreTake(mutex, portMAX_DELAY);
    }

    inline ~QueueLock() {
      if (mutex)
        xSemaphoreGive(mutex);
    }

  private:
    SemaphoreHandle_t mutex;
};

void CommandQueue::begin() {
  mutex = xSemaphoreCreateMutex();
}

int CommandQueue::getFreeSlots() {
  QueueLock lock(mutex);

  int freeSlots = COMMAND_BUFFER_SIZE - 1;

  int next = tail;
  while (next != head) {
    --freeSlots;
    next = nextBufferSlot(next);
  }

  return freeSlots;
}

void CommandQueue::clear() {
  QueueLock lock(mutex);

  head = tail;
}

// Tries to add a command to the queue, returns true if possible
bool CommandQueue::push(const char *command) {
  QueueLock lock(mutex);

  if (!command || !command[0])
    return false;

  int next = nextBufferSlot(head);
  if (next == tail)
    return false;

  strncpy(commandBuffer[head], command, COMMAND_MAX_BYTES - 1);
  commandBuffer[head][COMMAND_MAX_BYTES - 1] = 0;
  head = next;

  return true;
}

const char *CommandQueue::peekSend() {
  QueueLock lock(mutex);

  return (tail == head) ? NULL : commandBuffer[tail];
}

// Frees the slot. The window of unacknowledged lines is PrinterLink's, and it holds its
// own copy, so nothing here has to outlive the send.
void CommandQueue::popSend() {
  QueueLock lock(mutex);

  if (tail != head)
    tail = nextBufferSlot(tail);
}
