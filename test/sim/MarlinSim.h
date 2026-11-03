#pragma once

// A printer that behaves the way the real one does where it matters: a serial ring of a
// fixed size that silently loses what does not fit, checksums that are checked before
// anything else happens, a command queue of BUFSIZE slots, and answers in the exact shape
// Marlin 2.1.2 produces. Every number here was read out of the user's own sources.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

// Built with Marlin's own decoder beside it, the fake printer unpacks a compressed
// stream the way the real one does. Without it, it answers that it cannot.
#ifdef MARLIN_SIM_HEATSHRINK
  #include "heatshrink_decoder.h"
#endif

struct MarlinSim {
  // Knobs. The defaults are the printer as it stands today.
  size_t rxBufferSize = 64;         // libmaple USART_RX_BUF_SIZE, not Marlin's RX_BUFFER_SIZE
  int bufSize = 32;                 // Marlin BUFSIZE
  bool advancedOk = true;
  bool lfnWrite = false;
  bool sdWrite = true;
  bool xonXoff = false;
  bool binary = false;              // BINARY_FILE_TRANSFER built into the firmware
  uint16_t binaryPayload = 96;      // What Marlin hands the stream: MAX_CMD_SIZE
  uint8_t binaryWindow = 1;
  uint32_t binaryWritePackets = 0;
  uint32_t binaryWriteAckDropAt = 0, binaryWriteAckDropEvery = 0, binaryWriteAckCorruptEvery = 0;
  uint32_t binaryWriteAckDrops = 0;
  uint32_t binaryShutAckDrops = 0;
  uint32_t binaryWriteDelayMs = 0;
  size_t binaryIoErrorAfter = 0;
  uint32_t binaryCorruptEvery = 0;  // One packet byte in this many is flipped
  uint32_t binaryDropEvery = 0;     // One packet byte in this many never arrives
  uint32_t binaryCloseDrops = 0, binaryCloseAckDrops = 0;
  bool cardPresent = true;
  size_t cardFree = 8u * 1024 * 1024;
  int executePerTick = 8;           // Commands the main loop gets through in a millisecond
  uint32_t drainPerTick = 64;       // Bytes the main loop lifts out of the ring
  uint32_t stallEvery = 0;          // Every this many milliseconds the loop goes away
  uint32_t stallFor = 0;            // for this long, the way an SD block write does
  uint32_t corruptEvery = 0;        // One byte in this many is flipped coming in

  // What happened
  size_t dropped = 0;               // Bytes the ring had no room for. The loss we are hunting
  size_t highWater = 0;             // Fullest the ring ever got
  uint32_t resendsAsked = 0;
  uint32_t binaryResendsAsked = 0;
  bool binaryMode = false;          // M28 B1 puts the port here, a control packet takes it back
  bool binaryOpen = false;          // A file is open for binary writing
  bool binaryPacked = false;        // The open packet asked for a compressed stream
  std::string binaryFile;
  std::vector<std::string> seen;    // Commands that made it through, in order
  std::map<std::string, std::string> card;
  std::map<std::string, std::string> alias;   // Long name to the 8.3 the card really holds

  void reset() {
    ring.clear(); partial.clear(); cmdq.clear(); out.clear();
    lastN = 0; dropped = highWater = 0; resendsAsked = 0; arrived = 0;
    seen.clear(); card.clear(); alias.clear(); saving.clear(); stallUntil = 0;
    binaryMode = binaryOpen = false; binaryFile.clear(); binaryResendsAsked = 0;
    binState = BinWait; binSync = 0; binRetries = 0; binHeld = 0; binTook = 0;
    binData.clear(); binArrived = 0;
    pendingBinAck.clear(); binAckAt = nowTick = 0;
    binWriteBusy = false;
    binaryWritePackets = 0;
  }

  // A byte lands on the printer's pin
  void toPrinter(uint8_t b) {
    if (corruptEvery && ++arrived % corruptEvery == 0)
      b ^= 0x20;                    // A flipped bit inside a word, which a checksum must catch
    if (binaryMode) {
      ++binArrived;
      if (binaryDropEvery && binArrived % binaryDropEvery == 0)
        return;                     // The byte never lands, the packet is short
      if (binaryCorruptEvery && binArrived % binaryCorruptEvery == 0)
        b ^= 0x40;
    }
    if (ring.size() >= rxBufferSize) {
      dropped++;                    // Exactly what the hardware does: the byte is gone
      return;
    }
    ring.push_back(b);
    if (ring.size() > highWater)
      highWater = ring.size();
  }

  void tick(const uint32_t now) {
    nowTick = now;
    if (stallEvery && now >= stallUntil && now % stallEvery == 0)
      stallUntil = now + stallFor;
    if (now < stallUntil)
      return;

    if (binaryMode) {
      if (binWriteBusy) {
        if (now < binAckAt) return;
        if (!pendingBinAck.empty()) say(pendingBinAck);
        pendingBinAck.clear();
        binWriteBusy = false;
      }
      binaryDrain(now);
      for (int i = 0; i < executePerTick && !cmdq.empty(); i++) {
        const std::string cmd = cmdq.front();
        cmdq.pop_front();
        execute(cmd);
      }
      return;
    }

    for (uint32_t i = 0; i < drainPerTick && !ring.empty(); i++) {
      if ((int)cmdq.size() >= bufSize)
        break;
      const char c = (char)ring.front();
      ring.pop_front();
      if (c == '\r')
        continue;
      if (c != '\n') {
        if (partial.size() < 512)
          partial.push_back(c);
        continue;
      }
      accept(partial);
      partial.clear();
    }

    for (int i = 0; i < executePerTick && !cmdq.empty(); i++) {
      const std::string cmd = cmdq.front();
      cmdq.pop_front();                 // Off the queue first, so B reports the real room
      execute(cmd);
    }
  }

  int fromPrinter() {
    if (out.empty())
      return -1;
    const int c = (uint8_t)out.front();
    out.erase(out.begin());
    return c;
  }

  // Roughly what a FAT card does: six safe characters, a tilde and a number
  static std::string dosAlias(const std::string &name) {
    if (name.size() <= 12 && name.find(' ') == std::string::npos) {
      std::string up = name;
      for (char &c : up) c = (char)toupper((unsigned char)c);
      return up;
    }
    std::string stem, ext;
    const size_t dot = name.rfind('.');
    for (size_t i = 0; i < (dot == std::string::npos ? name.size() : dot) && stem.size() < 6; i++) {
      const char c = (char)toupper((unsigned char)name[i]);
      if (isalnum((unsigned char)c)) stem += c;
    }
    if (dot != std::string::npos)
      for (size_t i = dot + 1; i < name.size() && ext.size() < 3; i++)
        ext += (char)toupper((unsigned char)name[i]);
    if (stem.empty()) stem = "FILE";
    return stem + "~1" + (ext.empty() ? "" : "." + ext);
  }

private:
  std::deque<uint8_t> ring;
  std::string partial, out, saving;
  std::deque<std::string> cmdq;
  long lastN = 0;
  uint32_t arrived = 0, stallUntil = 0;

  void say(const std::string &s) { out += s + "\n"; }

  void resend() {
    resendsAsked++;
    partial.clear();                // Marlin throws away the line it was collecting
    say("Resend: " + std::to_string(lastN + 1));
  }

  // src/gcode/queue.cpp:473 onwards. The checksum is checked before anything is written.
  void accept(const std::string &line) {
    if (line.empty())
      return;
    if (line[0] != 'N') {
      if (!saving.empty() && line.compare(0, 3, "M29") != 0) {
        say("Error:No Checksum with line number, Last Line: " + std::to_string(lastN));
        return;
      }
      cmdq.push_back(line);
      return;
    }

    const size_t star = line.find('*');
    if (star == std::string::npos) {
      say("Error:No Checksum with line number, Last Line: " + std::to_string(lastN));
      return;
    }
    uint8_t sum = 0;
    for (size_t i = 0; i < star; i++)
      sum ^= (uint8_t)line[i];
    if ((uint8_t)atoi(line.c_str() + star + 1) != sum) {
      resend();
      return;
    }

    const long n = atol(line.c_str() + 1);
    const std::string body = payload(line, star);
    const bool isM110 = body.compare(0, 4, "M110") == 0;
    if (!isM110 && n != lastN + 1) {
      resend();
      return;
    }
    lastN = isM110 ? 0 : n;
    cmdq.push_back(body);
  }

  // src/sd/cardreader.cpp:808 strips "N<n> " and everything from the star
  static std::string payload(const std::string &line, const size_t star) {
    const size_t space = line.find(' ');
    if (space == std::string::npos || space > star)
      return line.substr(0, star);
    return line.substr(space + 1, star - space - 1);
  }

  void ok() {
    if (!advancedOk) { say("ok"); return; }
    char buf[64];
    snprintf(buf, sizeof buf, "ok N%ld P%d B%d", lastN, 15, bufSize - (int)cmdq.size());
    say(buf);
  }

  void execute(const std::string &cmd) {
    if (!saving.empty()) {
      if (cmd.compare(0, 3, "M29") == 0) {
        saving.clear();
        seen.push_back(cmd);
        ok();
        return;
      }
      card[saving] += cmd + "\r\n";
      cardFree -= cmd.size() + 2;
      ok();
      return;
    }

    seen.push_back(cmd);

    if (cmd.compare(0, 4, "M115") == 0) {
      say("FIRMWARE_NAME:Marlin 2.1.2 SOURCE_CODE_URL:... PROTOCOL_VERSION:1.0 MACHINE_TYPE:KP3S EXTRUDER_COUNT:1");
      say(std::string("Cap:SERIAL_XON_XOFF:") + (xonXoff ? "1" : "0"));
      say(std::string("Cap:BINARY_FILE_TRANSFER:") + (binary ? "1" : "0"));
      say("Cap:AUTOREPORT_TEMP:1");
      say("Cap:AUTOREPORT_SD_STATUS:1");
      say(std::string("Cap:SDCARD:") + (cardPresent ? "1" : "0"));
      say(std::string("Cap:SD_WRITE:") + (sdWrite ? "1" : "0"));
      say(std::string("Cap:LFN_WRITE:") + (lfnWrite ? "1" : "0"));
      say("Cap:EMERGENCY_PARSER:1");
      say("Cap:LONG_FILENAME:1");
    }
    else if (cmd.compare(0, 3, "M21") == 0) {
      if (!cardPresent) { say("Error:No SD card"); ok(); return; }
      say("echo:SD card ok");
    }
    else if (cmd.compare(0, 3, "M20") == 0) {
      const bool longToo = cmd.find(" L") != std::string::npos;
      say("Begin file list");
      for (const auto &entry : card) {
        const std::string dos = alias.count(entry.first) ? alias.at(entry.first) : entry.first;
        std::string row = dos + " " + std::to_string(entry.second.size());
        if (longToo && dos != entry.first) row += " " + entry.first;
        say(row);
      }
      say("End file list");
    }
    else if (cmd.compare(0, 3, "M28") == 0) {
      // M28B1 does not open anything and never reaches the card check: it hands the
      // port to the binary stream. Without the feature built in, Marlin takes the B1
      // for a filename and opens a file called that.
      if (binary && cmd.find("B1") != std::string::npos) {
        binaryMode = true;
        binState = BinWait; binSync = 0; binRetries = 0; binData.clear();
        say("Switching to Binary Protocol");
        ok();
        return;
      }
      if (!cardPresent || !sdWrite) { say("Error:No SD card"); ok(); return; }
      const size_t space = cmd.find(' ');
      std::string name = space == std::string::npos ? "OUT.GCO" : cmd.substr(space + 1);
      while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) name.pop_back();
      if (!lfnWrite && name.size() > 12) { say("Error:Long filename not supported"); ok(); return; }
      saving = name;
      card[name].clear();
      alias[name] = dosAlias(name);
      say("Writing to file: " + name);
    }
    else if (cmd.compare(0, 3, "M30") == 0) {
      const size_t space = cmd.find(' ');
      if (space != std::string::npos) card.erase(cmd.substr(space + 1));
    }
    else if (cmd.compare(0, 4, "M105") == 0) {
      say("ok T:210.00 /210.00 B:60.00 /60.00 @:70 B@:40");   // M105 answers with the ok itself
      return;
    }

    ok();
  }

  // ---------------------------------------------------------------- binary protocol
  // Mirrors BinaryStream::receive in Marlin 2.1.2, including which bytes each checksum
  // covers and which packet is answered even when the counter disagrees.
  enum BinState : uint8_t { BinWait, BinHeader, BinData, BinFooter };
  BinState binState = BinWait;
  uint8_t binSync = 0, binRetries = 0, binHeader[8] = {0};
  size_t binHeld = 0, binTook = 0;
  uint16_t binChecksum = 0, binHeaderChecksum = 0;
  std::string binData;
  uint32_t binArrived = 0, binLastByteAt = 0;
  uint32_t nowTick = 0, binAckAt = 0;
  std::string pendingBinAck;
  bool binWriteBusy = false;

  static uint16_t binFletcher(uint16_t cs, const uint8_t value) {
    const uint16_t low = (uint16_t)(((cs & 0xFF) + value) % 255);
    return (uint16_t)(((((cs >> 8) + low) % 255) << 8) | low);
  }

  uint16_t binHeaderWord(const int at) const {
    return (uint16_t)(binHeader[at] | (binHeader[at + 1] << 8));
  }

  void binReset() {
    binState = BinWait;
    binHeld = binTook = 0;
    binChecksum = binHeaderChecksum = 0;
    binData.clear();
    binHeader[0] = binHeader[1] = 0;
  }

  void binAskAgain() {
    binRetries++;
    binaryResendsAsked++;
    char b[32];
    snprintf(b, sizeof b, "rs%u", (unsigned)binSync);
    say(b);
    binReset();
  }

  void binGiveUp() {
    char b[32];
    snprintf(b, sizeof b, "fe%u", (unsigned)binHeader[2]);
    say(b);
    binSync = 0;
    binRetries = 0;
    binReset();
  }

  void binDispatch(const uint8_t protocol, const uint8_t type) {
    if (protocol == 0) {                       // control
      if (type == 2)                           // close, back to reading gcode
        binaryMode = false;
      return;
    }
    if (protocol != 1) {
      say("Unsupported Binary Protocol");
      return;
    }
    switch (type) {
      case 0:                                  // query
#ifdef MARLIN_SIM_HEATSHRINK
        say("PFT:version:0.1.0:compression:heatshrink,"
            + std::to_string(HEATSHRINK_STATIC_WINDOW_BITS) + ","
            + std::to_string(HEATSHRINK_STATIC_LOOKAHEAD_BITS)
            + (binaryWindow == 2 ? ":window:2" : ""));
#else
        say(std::string("PFT:version:0.1.0:compression:none") + (binaryWindow == 2 ? ":window:2" : ""));
#endif
        break;
      case 1: {                                // open
        if (binaryOpen) { say("PFT:busy"); break; }
        // dummy, compression, then the name with its terminator
        if (binData.size() < 3 || binData.back() != '\0') { say("PFT:fail"); break; }
        if (!cardPresent || !sdWrite) { say("PFT:fail"); break; }
        binaryFileName(binData);
        binaryPacked = (binData[1] & 1) != 0;
#ifdef MARLIN_SIM_HEATSHRINK
        heatshrink_decoder_reset(&hsd);
#else
        if (binaryPacked) { say("PFT:fail"); break; }
#endif
        binaryOpen = true;
        card[binaryFile].clear();
        alias[binaryFile] = dosAlias(binaryFile);
        say("PFT:success");
        break;
      }
      case 2:                                  // close
        if (!binaryOpen) { say("PFT:invalid"); break; }
        binaryOpen = false;
        say("PFT:success");
        break;
      case 3:                                  // write
        if (!binaryOpen) { say("PFT:invalid"); break; }
        if (binaryIoErrorAfter && card[binaryFile].size() + binData.size() > binaryIoErrorAfter) {
          say("PFT:ioerror");
          break;
        }
        if (binaryPacked) { unpackInto(card[binaryFile], binData); break; }
        card[binaryFile] += binData;
        cardFree -= binData.size();
        break;
      case 4:                                  // abort
        if (binaryOpen) card.erase(binaryFile);
        binaryOpen = false;
        say("PFT:success");
        break;
      default:
        say("PTF:invalid");
        break;
    }
  }

#ifdef MARLIN_SIM_HEATSHRINK
  heatshrink_decoder hsd;

  // Driven exactly as binary_stream.h drives it: sink what fits, then poll dry.
  void unpackInto(std::string &into, const std::string &from) {
    size_t done = 0;
    while (done < from.size()) {
      size_t took = 0;
      heatshrink_decoder_sink(&hsd, (uint8_t *)from.data() + done, from.size() - done, &took);
      done += took;
      HSD_poll_res res;
      do {
        uint8_t out[512];
        size_t made = 0;
        res = heatshrink_decoder_poll(&hsd, out, sizeof out, &made);
        into.append((const char *)out, made);
        cardFree -= made;
      } while (res == HSDR_POLL_MORE);
    }
  }
#else
  void unpackInto(std::string &, const std::string &) {}
#endif

  void binaryFileName(const std::string &body) {
    binaryFile.clear();
    for (size_t i = 2; i < body.size() && body[i]; i++)
      binaryFile += body[i];
  }

  void binaryDrain(const uint32_t now) {
    if (binState != BinWait && binLastByteAt && now - binLastByteAt > 500) {
      binLastByteAt = now;
      binAskAgain();                           // Marlin's PACKET_MAX_WAIT, then a resend
      return;
    }

    while (!ring.empty()) {
      const uint8_t d = ring.front();
      ring.pop_front();
      binLastByteAt = now;

      switch (binState) {
        case BinWait:
          binHeader[1] = d;
          if (binHeaderWord(0) == 0xB5AD) {
            binHeld = 2;
            binChecksum = 0;
            binState = BinHeader;
          }
          else
            binHeader[0] = d;
          break;

        case BinHeader: {
          binHeader[binHeld++] = d;
          binChecksum = binFletcher(binChecksum, d);
          if (binHeld == sizeof(binHeader) - 2)
            binHeaderChecksum = binChecksum;    // the checksum field itself is not in it
          if (binHeld < sizeof(binHeader))
            break;

          if (binHeaderWord(6) != binHeaderChecksum) { binAskAgain(); break; }

          const uint8_t protocol = (uint8_t)((binHeader[3] >> 4) & 0xF);
          const uint8_t type = (uint8_t)(binHeader[3] & 0xF);
          const uint8_t theirSync = binHeader[2];
          const uint16_t size = binHeaderWord(4);

          // The sync packet is answered whatever the counters say, that is its job
          if (protocol == 0 && type == 1) {
            char b[64];
            snprintf(b, sizeof b, "ss%u,%u,0.1.0", (unsigned)binSync, (unsigned)binaryPayload);
            say(b);
            binReset();
            break;
          }
          if (theirSync == (uint8_t)(binSync - 1)) {
            char b[32];                         // our answer was lost, say it again
            snprintf(b, sizeof b, "ok%u", (unsigned)theirSync);
            say(b);
            binReset();
            break;
          }
          if (theirSync != binSync) {
            if (binRetries) { binReset(); break; }   // already asked, let the pipe drain
            say("Datastream packet out of order");
            binAskAgain();
            break;
          }
          if (size > binaryPayload) { binGiveUp(); break; }

          binTook = 0;
          binHeld = 0;
          binData.clear();
          if (size)
            binState = BinData;
          else
            binFinish(protocol, type);
          break;
        }

        case BinData:
          binData.push_back((char)d);
          binChecksum = binFletcher(binChecksum, d);
          if (++binHeld == binHeaderWord(4)) {
            binState = BinFooter;
            binHeld = 0;
          }
          break;

        case BinFooter:
          binFooter[binHeld++] = d;
          if (binHeld < 2)
            break;
          if ((uint16_t)(binFooter[0] | (binFooter[1] << 8)) != binChecksum) {
            binAskAgain();
            break;
          }
          binFinish((uint8_t)((binHeader[3] >> 4) & 0xF), (uint8_t)(binHeader[3] & 0xF));
          break;
      }
      if (binWriteBusy) return;
    }
  }

  uint8_t binFooter[2] = {0};

  void binFinish(const uint8_t protocol, const uint8_t type) {
    if (protocol == 0 && type == 2 && binaryCloseDrops) {
      binaryCloseDrops--;
      binReset();
      return;
    }
    const uint8_t answered = binHeader[2];
    binSync = (uint8_t)(binSync + 1);
    binRetries = 0;
    char b[32];
    snprintf(b, sizeof b, "ok%u", (unsigned)answered);
    if (protocol == 1 && type == 3) {
      binDispatch(protocol, type);
      ++binaryWritePackets;
      const bool drop = binaryWriteAckDrops || binaryWritePackets == binaryWriteAckDropAt
                     || (binaryWriteAckDropEvery && binaryWritePackets % binaryWriteAckDropEvery == 0);
      if (binaryWriteAckDrops) --binaryWriteAckDrops;
      if (binaryWriteAckCorruptEvery && binaryWritePackets % binaryWriteAckCorruptEvery == 0)
        b[0] = 'x';
      if (binaryWriteDelayMs) {
        pendingBinAck = drop ? "" : b;
        binAckAt = nowTick + binaryWriteDelayMs;
        binWriteBusy = true;
      }
      else if (!drop)
        say(b);
    }
    else {
      if (protocol == 0 && type == 2 && binaryCloseAckDrops)
        binaryCloseAckDrops--;
      else if (protocol == 1 && type == 2 && binaryShutAckDrops)
        binaryShutAckDrops--;
      else
        say(b);
      binDispatch(protocol, type);
    }
    binReset();
  }
};
