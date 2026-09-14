#include "CommandQueue.h"

CommandQueue commandQueue;    //FIFO Queue

int CommandQueue::head = 0;
int CommandQueue::sendTail = 0;
int CommandQueue::tail = 0;
String CommandQueue::commandBuffer[COMMAND_BUFFER_SIZE];
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

  head = sendTail = tail;
}

// Tries to Add a command to the queue, returns true if possible
bool CommandQueue::push(const String command) {
  QueueLock lock(mutex);

  int next = nextBufferSlot(head);
  if (next == tail || command == "")
    return false;

  commandBuffer[head] = command;
  head = next;

  return true;
}

String CommandQueue::peekSend() {
  QueueLock lock(mutex);

  return (sendTail == head) ? String() : commandBuffer[sendTail];
}

// Returns the next command to be sent, and advances to the next
String CommandQueue::popSend() {
  QueueLock lock(mutex);

  if (sendTail == head)
    return String();

  const String command = commandBuffer[sendTail];
  sendTail = nextBufferSlot(sendTail);
  
  return command;
}

// Returns the last command sent if it was received by the printer, otherwise returns empty
String CommandQueue::popAcknowledge() {
  QueueLock lock(mutex);

  if (tail == sendTail)
    return String();

  const String command = commandBuffer[tail];
  tail = nextBufferSlot(tail);

  return command;
}
