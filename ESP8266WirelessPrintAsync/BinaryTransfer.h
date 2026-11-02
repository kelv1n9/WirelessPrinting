#pragma once

// Marlin's own binary protocol, the one its buildroot/share/scripts/MarlinBinaryProtocol.py
// speaks, so a firmware image can be put on the printer's card byte for byte. Plain M28
// cannot carry one: Marlin strips the line number and everything from the asterisk and
// appends CRLF, which mangles every packet of a binary file.
//
// Stop and wait, one packet in flight, exactly like the reference sender. The printer
// answers every packet with ok<sync>, asks again with rs<sync>, and gives up with
// fe<sync>. Nothing here is clever: the firmware of a printer is not the place.
//
// Entered with the ASCII line M28B1 and left with a control packet, after which the
// printer is listening for ordinary gcode again.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "PrinterLink.h"          // LinkIo, the one thing that touches the wire
#include "heatshrink_encoder.h"

#define BINARY_TOKEN        0xB5AD
#define BINARY_HEADER_BYTES 8
#define BINARY_FOOTER_BYTES 2
#define BINARY_MAX_PAYLOAD  96    // Marlin hands the stream its line buffer, MAX_CMD_SIZE
#define BINARY_PACKET_ROOM  (BINARY_HEADER_BYTES + BINARY_MAX_PAYLOAD + BINARY_FOOTER_BYTES)
#define BINARY_REPLY_BYTES  96
#define BINARY_NAME_BYTES   64

#define BINARY_ACK_WAIT     2000  // Marlin's own packet wait is 500 ms, leave it room
#define BINARY_OPEN_WAIT    15000
#define BINARY_CLOSE_WAIT   30000
#define BINARY_ASK_WAIT     1500  // For the version line, after the query is acknowledged
#define BINARY_MAX_RESENDS  40    // Over a whole image, not per packet

enum BinaryProtocol : uint8_t { BinaryControl = 0, BinaryFile = 1 };
enum BinaryControlType : uint8_t { BinarySync = 1, BinaryClose = 2 };
enum BinaryFileType : uint8_t { BinaryQuery = 0, BinaryOpen = 1, BinaryShut = 2,
                                BinaryWrite = 3, BinaryAbort = 4 };

enum BinaryPhase : uint8_t {
  BinaryIdle, BinarySwitching, BinarySyncing, BinaryAsking, BinaryOpening,
  BinaryWriting, BinaryShutting, BinaryAborting, BinaryDone, BinaryFailed
};

// The window and lookahead are compile time constants on both sides and the stream
// carries no header, so a disagreement writes a silently wrong file rather than
// failing. These are what this encoder emits; anything else and compression is off.
#define BINARY_WINDOW_BITS    8
#define BINARY_LOOKAHEAD_BITS 4

#define BINARY_PACK_SLICE  256    // fed to the encoder at a time
#define BINARY_PACK_ROOM   1024   // compressed bytes held back, waiting for a packet

// Where the bytes come from. Separate so a test can hand over memory and the firmware
// can hand over a file on the card without either knowing about the other.
struct ByteSource {
  virtual ~ByteSource() {}
  virtual int next(uint8_t *out, size_t max) = 0;   // bytes taken, 0 at the end, -1 on trouble
  virtual uint32_t size() const = 0;
};

class BinaryTransfer {
public:
  BinaryPhase phase = BinaryIdle;
  uint32_t written, total, packets, resends, startedAt, finishedAt;
  const char *trouble;

  // What the printer said about itself when it synchronised
  uint16_t theirPayload = BINARY_MAX_PAYLOAD;
  char theirVersion[16];

  // What it said about compression when asked, and what came of it
  char theirCompression[24];
  bool packing;             // true once the open packet has claimed a compressed stream
  uint32_t sent;            // bytes actually put on the wire, against written

  void begin(LinkIo *link) {
    io = link;
    phase = BinaryIdle;
  }

  bool running() const { return phase != BinaryIdle && phase != BinaryDone && phase != BinaryFailed; }

  // Compression is asked for, not assumed: the printer is questioned first and the
  // stream stays plain unless its answer matches this encoder exactly.
  bool start(ByteSource *from, const char *name, const uint32_t now, const bool pack = true) {
    if (running() || !io || !from || !name || !*name)
      return false;

    source = from;
    stopping = false;
    wanted = pack;
    packing = false;
    sent = 0;
    theirCompression[0] = 0;
    packer.reset();
    packHead = packTail = 0;
    drained = false;
    total = from->size();
    written = packets = resends = 0;
    trouble = NULL;
    sync = 0;
    held = 0;
    tail = 0;
    atEnd = false;
    theirPayload = BINARY_MAX_PAYLOAD;
    theirVersion[0] = 0;
    snprintf(target, sizeof target, "%s", name);

    startedAt = now;
    nowish = now;
    handshake();
    return true;
  }

  // Asks for the stub to be thrown away and the printer put back where it was. Not
  // immediate: a packet is always outstanding, and sending the abort under the same
  // number would have the printer take it for a repeat of that packet and drop it.
  // So the abort waits for the answer to what is already on the wire.
  void stop() {
    if (!running() || stopping)
      return;
    stopping = true;
    trouble = "the transfer was stopped";
  }

  void service(const uint32_t now) {
    if (!running())
      return;

    nowish = now;
    receive();

    if (!running() || (int32_t)(now - waitUntil) < 0)
      return;

    // The packet was taken but no version line came back. Repeating it would be
    // read as a duplicate and dropped, so go on with the stream uncompressed.
    if (phase == BinaryAsking && asked) {
      packing = false;
      openFile();
      return;
    }

    // Marlin asks again on its own after 500 ms of silence, so a wait this long means
    // the answer itself went missing rather than the packet.
    if (++resends > BINARY_MAX_RESENDS) {
      fail("the printer stopped answering");
      return;
    }

    if (phase == BinarySwitching || phase == BinarySyncing)
      handshake();          // It may never have gone into binary mode at all
    else
      repeat();
  }

private:
  LinkIo *io = NULL;
  ByteSource *source = NULL;
  char target[BINARY_NAME_BYTES];

  uint8_t sync;
  bool wanted = true;
  bool asked = false;
  HeatshrinkEncoder packer;
  uint8_t packed[BINARY_PACK_ROOM];
  uint16_t packHead, packTail;         // compressed bytes live in packed[packTail..packHead)
  bool drained;                        // the source is spent and the encoder flushed
  uint8_t packet[BINARY_PACKET_ROOM];
  uint16_t held;                       // length of the packet last sent, for a repeat
  bool atEnd;
  bool stopping = false;

  char reply[BINARY_REPLY_BYTES];
  uint16_t tail;
  uint32_t waitUntil = 0;

  static uint16_t fletcher(uint16_t cs, const uint8_t value) {
    const uint16_t low = (((cs & 0xFF) + value) % 255);
    return (uint16_t)(((((cs >> 8) + low) % 255) << 8) | low);
  }

  static uint16_t fletcherOver(const uint8_t *from, const size_t count) {
    uint16_t cs = 0;
    for (size_t i = 0; i < count; i++)
      cs = fletcher(cs, from[i]);
    return cs;
  }

  void fail(const char *why) {
    trouble = why;
    phase = BinaryFailed;
  }

  void sendAscii(const char *line) {
    if (!io)
      return;
    io->write((const uint8_t *)line, strlen(line));
    io->write((const uint8_t *)"\n", 1);
  }

  // Lays a packet into the buffer and puts it on the wire. The token is not part of
  // either checksum, and the header checksum is part of the payload one.
  void put(const uint8_t protocol, const uint8_t type, const uint8_t *data, const uint16_t count) {
    packet[0] = BINARY_TOKEN & 0xFF;
    packet[1] = (BINARY_TOKEN >> 8) & 0xFF;
    packet[2] = sync;
    packet[3] = (uint8_t)(((protocol & 0xF) << 4) | (type & 0xF));
    packet[4] = count & 0xFF;
    packet[5] = (count >> 8) & 0xFF;

    const uint16_t headerSum = fletcherOver(packet + 2, 4);
    packet[6] = headerSum & 0xFF;
    packet[7] = (headerSum >> 8) & 0xFF;

    uint16_t at = BINARY_HEADER_BYTES;
    if (count) {
      memcpy(packet + at, data, count);
      at += count;
      const uint16_t full = fletcherOver(packet + 2, (size_t)(at - 2));
      packet[at++] = full & 0xFF;
      packet[at++] = (full >> 8) & 0xFF;
    }

    held = at;
    io->write(packet, held);
    packets++;
  }

  void repeat() {
    if (held)
      io->write(packet, held);
    waitUntil = nowish + BINARY_ACK_WAIT;
  }

  // M28B1 is an ordinary gcode line and has to be acknowledged before the printer is
  // reading packets at all. Sending the sync packet straight after it puts binary bytes
  // into Marlin's line buffer, where they are read as a command and thrown away, and
  // costs a whole timeout before anything works.
  void handshake() {
    sendAscii("M28B1");
    phase = BinarySwitching;
    waitUntil = nowish + BINARY_ACK_WAIT;
  }

  void askQuery() {
    phase = BinaryAsking;
    asked = false;
    put(BinaryFile, BinaryQuery, NULL, 0);
    waitUntil = nowish + BINARY_ACK_WAIT;
  }

  void askToSync() {
    phase = BinarySyncing;
    put(BinaryControl, BinarySync, NULL, 0);
    waitUntil = nowish + BINARY_ACK_WAIT;
  }

  void receive() {
    int got;
    while (io && (got = io->read()) >= 0) {
      const char ch = (char)got;
      if (ch == '\n' || ch == '\r') {
        if (tail) {
          reply[tail] = 0;
          observe(reply);
          tail = 0;
        }
        continue;
      }
      if (tail < BINARY_REPLY_BYTES - 1)
        reply[tail++] = ch;
    }
  }

  void observe(const char *line) {
    // Still in ASCII: anything that reads like an answer to M28B1 means the port is ours
    if (phase == BinarySwitching) {
      if (!strncmp(line, "Switching to Binary", 19)) {
        askToSync();
        return;
      }
      // Marlin built without the feature takes the B1 for a filename and opens a file
      // called that. Close it again, or it sits on the card collecting whatever we send.
      if (!strncmp(line, "Writing to file", 15)) {
        sendAscii("M29");
        sendAscii("M30 B1");
        fail("this printer has no binary transfer built in");
        return;
      }
      if (!strncmp(line, "ok", 2)) {
        askToSync();
        return;
      }
      if (!strncmp(line, "Error", 5) || !strncmp(line, "echo:Unknown", 12))
        fail("the printer refused to switch to binary");
      return;
    }

    if (!strncmp(line, "ss", 2)) {
      // ss<sync>,<payload>,<version>
      unsigned theirs = 0, room = 0;
      char version[16] = {0};
      if (sscanf(line + 2, "%u,%u,%15s", &theirs, &room, version) >= 2) {
        sync = (uint8_t)theirs;
        if (room && room < BINARY_MAX_PAYLOAD)
          theirPayload = (uint16_t)room;
        snprintf(theirVersion, sizeof theirVersion, "%s", version);
      }
      afterAck(true);
      return;
    }
    if (!strncmp(line, "ok", 2)) {
      unsigned theirs = 0;
      if (sscanf(line + 2, "%u", &theirs) == 1 && (uint8_t)theirs != sync)
        return;                            // An answer to something already behind us
      sync = (uint8_t)(sync + 1);
      afterAck(false);
      return;
    }
    if (!strncmp(line, "rs", 2)) {
      resends++;
      if (resends > BINARY_MAX_RESENDS) {
        fail("the printer kept asking for the same packet");
        return;
      }
      if (held)
        io->write(packet, held);
      return;
    }
    if (!strncmp(line, "fe", 2)) {
      fail("the printer gave up on the stream");
      return;
    }
    if (!strncmp(line, "PFT:version", 11)) {
      if (phase != BinaryAsking)
        return;
      const char *said = strstr(line, ":compression:");
      if (said) {
        said += 13;
        snprintf(theirCompression, sizeof theirCompression, "%s", said);
        unsigned window = 0, lookahead = 0;
        packing = wanted
               && sscanf(said, "heatshrink,%u,%u", &window, &lookahead) == 2
               && window == BINARY_WINDOW_BITS
               && lookahead == BINARY_LOOKAHEAD_BITS;
      }
      openFile();
      return;
    }
    if (!strncmp(line, "PFT:busy", 8)) {
      fail("the printer already has a transfer open");
      return;
    }
    if (!strncmp(line, "PFT:fail", 8)) {
      fail("the printer would not open the file");
      return;
    }
    if (!strncmp(line, "PFT:ioerror", 11)) {
      fail("the printer could not write its card");
      return;
    }
    if (!strncmp(line, "PFT:invalid", 11)) {
      fail("the printer refused the step");
      return;
    }
    if (!strncmp(line, "PFT:success", 11))
      afterSuccess();
  }

  // The acknowledgement of a packet moves the work along
  void afterAck(const bool wasSync) {
    if (stopping && phase != BinaryAborting) {
      // The wire is quiet now, so the abort gets a number of its own
      phase = BinaryAborting;
      put(BinaryFile, BinaryAbort, NULL, 0);
      waitUntil = nowish + BINARY_CLOSE_WAIT;
      return;
    }

    switch (phase) {
      case BinarySyncing:
        if (!wasSync)
          return;                          // Only ss ends the handshake
        askQuery();
        break;

      case BinaryAsking:
        // Marlin acknowledges the packet before it answers it, so the ok says
        // nothing yet. Give the answer a moment of its own.
        asked = true;
        waitUntil = nowish + BINARY_ASK_WAIT;
        break;

      case BinaryOpening:
        break;                             // Waiting on PFT:success rather than the ok

      case BinaryWriting:
        if (atEnd) {
          phase = BinaryShutting;
          put(BinaryFile, BinaryShut, NULL, 0);
          waitUntil = nowish + BINARY_CLOSE_WAIT;
          return;
        }
        writeMore();
        break;

      case BinaryShutting:
        break;                             // Waiting on PFT:success

      default:
        break;
    }
  }

  void afterSuccess() {
    if (phase == BinaryAborting) {
      put(BinaryControl, BinaryClose, NULL, 0);
      phase = BinaryFailed;
      finishedAt = nowish;
      return;
    }
    if (phase == BinaryOpening) {
      phase = BinaryWriting;
      writeMore();
      return;
    }
    if (phase == BinaryShutting) {
      // Nothing answers a close: the printer simply goes back to reading gcode
      put(BinaryControl, BinaryClose, NULL, 0);
      phase = BinaryDone;
      finishedAt = nowish;
    }
  }

  void openFile() {
    uint8_t body[BINARY_MAX_PAYLOAD];
    size_t at = 0;
    body[at++] = 0;                        // dummy transfer off, we mean it
    body[at++] = packing ? 1 : 0;          // what the printer agreed to, nothing assumed
    const size_t room = sizeof(body) - at - 1;
    size_t len = strlen(target);
    if (len > room)
      len = room;
    memcpy(body + at, target, len);
    at += len;
    body[at++] = 0;                        // Marlin checks for the terminator itself

    phase = BinaryOpening;
    put(BinaryFile, BinaryOpen, body, (uint16_t)at);
    waitUntil = nowish + BINARY_OPEN_WAIT;
  }

  void closeUp() {
    atEnd = true;
    phase = BinaryShutting;
    put(BinaryFile, BinaryShut, NULL, 0);
    waitUntil = nowish + BINARY_CLOSE_WAIT;
  }

  // Keeps at least one packet of compressed bytes ready, or marks the stream spent.
  bool fillPacked() {
    if (drained || (uint16_t)(packHead - packTail) >= BINARY_MAX_PAYLOAD)
      return true;

    if (packTail) {
      memmove(packed, packed + packTail, (size_t)(packHead - packTail));
      packHead -= packTail;
      packTail = 0;
    }

    while (BINARY_PACK_ROOM - packHead >= HeatshrinkEncoder::max_output(BINARY_PACK_SLICE)) {
      uint8_t slice[BINARY_PACK_SLICE];
      const int got = source->next(slice, sizeof slice);
      if (got < 0)
        return false;
      if (got == 0) {
        drained = true;
        packHead += packer.finish(packed + packHead, BINARY_PACK_ROOM - packHead);
        break;
      }
      written += got;
      packHead += packer.encode(slice, (size_t)got, packed + packHead, BINARY_PACK_ROOM - packHead);
    }
    return true;
  }

  void writeMore() {
    const size_t room = theirPayload < BINARY_MAX_PAYLOAD ? theirPayload : BINARY_MAX_PAYLOAD;

    if (packing) {
      if (!fillPacked()) {
        fail("the file could not be read");
        return;
      }
      const uint16_t have = packHead - packTail;
      if (!have) {
        closeUp();
        return;
      }
      const uint16_t take = have < room ? have : (uint16_t)room;
      sent += take;
      put(BinaryFile, BinaryWrite, packed + packTail, take);
      packTail += take;
      waitUntil = nowish + BINARY_ACK_WAIT;
      return;
    }

    uint8_t body[BINARY_MAX_PAYLOAD];
    const int got = source->next(body, room);

    if (got < 0) {
      fail("the file could not be read");
      return;
    }
    if (got == 0) {
      closeUp();
      return;
    }

    written += got;
    sent += got;
    put(BinaryFile, BinaryWrite, body, (uint16_t)got);
    waitUntil = nowish + BINARY_ACK_WAIT;
  }
  uint32_t nowish = 0;        // service keeps this current so the states can stamp waits
};
