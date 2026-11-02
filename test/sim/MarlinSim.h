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

struct MarlinSim {
  // Knobs. The defaults are the printer as it stands today.
  size_t rxBufferSize = 64;         // libmaple USART_RX_BUF_SIZE, not Marlin's RX_BUFFER_SIZE
  int bufSize = 32;                 // Marlin BUFSIZE
  bool advancedOk = true;
  bool lfnWrite = false;
  bool sdWrite = true;
  bool xonXoff = false;
  bool binary = false;
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
  std::vector<std::string> seen;    // Commands that made it through, in order
  std::map<std::string, std::string> card;
  std::map<std::string, std::string> alias;   // Long name to the 8.3 the card really holds

  void reset() {
    ring.clear(); partial.clear(); cmdq.clear(); out.clear();
    lastN = 0; dropped = highWater = 0; resendsAsked = 0; arrived = 0;
    seen.clear(); card.clear(); alias.clear(); saving.clear(); stallUntil = 0;
  }

  // A byte lands on the printer's pin
  void toPrinter(uint8_t b) {
    if (corruptEvery && ++arrived % corruptEvery == 0)
      b ^= 0x20;                    // A flipped bit inside a word, which a checksum must catch
    if (ring.size() >= rxBufferSize) {
      dropped++;                    // Exactly what the hardware does: the byte is gone
      return;
    }
    ring.push_back(b);
    if (ring.size() > highWater)
      highWater = ring.size();
  }

  void tick(const uint32_t now) {
    if (stallEvery && now >= stallUntil && now % stallEvery == 0)
      stallUntil = now + stallFor;
    if (now < stallUntil)
      return;

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
};
