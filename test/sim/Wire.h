#pragma once

// The wire itself: bytes take as long as the baud rate says they take, so the numbers
// the tests print are times, not guesses.

#include <deque>
#include "../../ESP8266WirelessPrintAsync/PrinterLink.h"
#include "MarlinSim.h"

struct Wire : LinkIo {
  MarlinSim &sim;
  uint32_t baud;
  uint32_t clock = 0;
  size_t txRoom = 2048;

  explicit Wire(MarlinSim &printer, const uint32_t rate = 115200) : sim(printer), baud(rate) {}

  int read() override {
    if (rx.empty()) return -1;
    const int c = rx.front();
    rx.pop_front();
    return c;
  }

  size_t write(const uint8_t *data, const size_t len) override {
    if (tx.size() + len > txRoom)
      return 0;
    for (size_t i = 0; i < len; i++)
      tx.push_back(data[i]);
    return len;
  }

  size_t writable() override { return txRoom - tx.size(); }
  uint32_t now() override { return clock; }

  void advance() {                  // One millisecond of the world
    clock++;
    const double perMs = baud / 10.0 / 1000.0;    // 8N1, so ten bits to a byte

    outward += perMs;
    while (outward >= 1.0 && !tx.empty()) {
      sim.toPrinter(tx.front());
      tx.pop_front();
      outward -= 1.0;
    }
    if (tx.empty() && outward > perMs)
      outward = perMs;

    sim.tick(clock);

    inward += perMs;
    while (inward >= 1.0) {
      const int c = sim.fromPrinter();
      if (c < 0) break;
      rx.push_back((uint8_t)c);
      inward -= 1.0;
    }
    if (inward > perMs)
      inward = perMs;
  }

private:
  std::deque<uint8_t> tx, rx;
  double outward = 0, inward = 0;
};
