// Host test for the gcode reader. Build and run it before flashing:
//   c++ -std=c++11 -O1 -Wall -fsanitize=address,undefined -o /tmp/gcode test/test_gcode_reader.cpp && /tmp/gcode

#include "../ESP8266WirelessPrintAsync/GcodeReader.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

// Stands in for the card: hands the reader whatever a file would, in whatever sized
// bites, so a line that straddles a block boundary is exercised like the real thing.
class Paper {
  public:
    Paper(const std::string &body, size_t bite = GCODE_BLOCK_BYTES)
      : body(body), at(0), bite(bite) {}

    int read(uint8_t *into, size_t room) {
      size_t want = room < bite ? room : bite;
      if (at + want > body.size())
        want = body.size() - at;
      memcpy(into, body.data() + at, want);
      at += want;
      return (int)want;
    }

    bool available() const { return at < body.size(); }

  private:
    std::string body;
    size_t at, bite;
};

static std::vector<std::string> readAll(const std::string &body, size_t bite, size_t *bytes = NULL) {
  GcodeReader reader;
  reader.rewind();
  Paper paper(body, bite);

  std::vector<std::string> out;
  size_t total = 0;
  while (!(reader.blockSpent() && !paper.available())) {
    total += reader.nextLine(paper);
    out.push_back(reader.line());
  }
  if (bytes)
    *bytes = total;
  return out;
}

static void plainLinesComeBackWhole() {
  const std::string body = "G28\nG1 X10 Y10\nM104 S200\n";
  const std::vector<std::string> lines = readAll(body, GCODE_BLOCK_BYTES);

  assert(lines.size() == 3);
  assert(lines[0] == "G28");
  assert(lines[1] == "G1 X10 Y10");
  assert(lines[2] == "M104 S200");
}

// The bug this test exists for: a file saved on Windows carries a carriage return at
// the end of every line, and it used to be handed to the printer, landing between the
// command and its checksum. The printer answered "No Checksum with line number" and
// asked for the line again, for ever.
static void aWindowsFilePrintsToo() {
  const std::string body = "G28\r\nG1 X10 Y10\r\nM104 S200\r\n";
  const std::vector<std::string> lines = readAll(body, GCODE_BLOCK_BYTES);

  assert(lines.size() == 3);
  for (size_t i = 0; i < lines.size(); ++i)
    assert(lines[i].find('\r') == std::string::npos);
  assert(lines[0] == "G28");
  assert(lines[2] == "M104 S200");
}

static void theCountIsTheFilePosition() {
  const std::string body = "G28\nG1 X10\n";
  size_t counted = 0;
  const std::vector<std::string> lines = readAll(body, GCODE_BLOCK_BYTES, &counted);

  assert(lines.size() == 2);
  assert(counted == body.size());          // Newlines included, or the progress drifts

  size_t countedCRLF = 0;
  const std::string windows = "G28\r\nG1 X10\r\n";
  readAll(windows, GCODE_BLOCK_BYTES, &countedCRLF);
  assert(countedCRLF == windows.size());   // The trimmed return still counts as read
}

// Every block boundary has to fall somewhere, so walk it across a line.
static void aLineSplitAcrossBlocksIsStillOneLine() {
  std::string body;
  for (int i = 0; i < 400; ++i) {
    char line[64];
    snprintf(line, sizeof(line), "G1 X%d.123 Y%d.456 E%d.789\r\n", i, i * 2, i * 3);
    body += line;
  }
  const std::vector<std::string> whole = readAll(body, GCODE_BLOCK_BYTES);

  const size_t bites[] = { 1, 2, 3, 7, 13, 64, 511, 1024, 2047, 2048 };
  for (size_t b = 0; b < sizeof(bites) / sizeof(bites[0]); ++b) {
    const std::vector<std::string> piecemeal = readAll(body, bites[b]);
    assert(piecemeal == whole);
  }
  assert(whole.size() == 400);
  assert(whole[399] == "G1 X399.123 Y798.456 E1197.789");
}

static void aLineTooLongIsCutNotOverrun() {
  std::string body(GCODE_LINE_BYTES * 3, 'G');
  body += "\nM105\n";
  const std::vector<std::string> lines = readAll(body, GCODE_BLOCK_BYTES);

  assert(lines.size() == 2);
  assert(lines[0].size() == GCODE_LINE_BYTES - 1);   // Cut to what the buffer holds
  assert(lines[1] == "M105");                        // And the next line is not lost
}

static void aFileWithNoLastNewlineKeepsItsLastLine() {
  const std::vector<std::string> lines = readAll("G28\nM105", GCODE_BLOCK_BYTES);

  assert(lines.size() == 2);
  assert(lines[1] == "M105");
}

static void emptyLinesSurvive() {
  const std::vector<std::string> lines = readAll("G28\n\r\n\nM105\n", GCODE_BLOCK_BYTES);

  assert(lines.size() == 4);
  assert(lines[0] == "G28");
  assert(lines[1] == "");
  assert(lines[2] == "");
  assert(lines[3] == "M105");
}

static void rewindStartsOverCleanly() {
  GcodeReader reader;
  Paper first("G28\nG1 X1\n");
  reader.rewind();
  reader.nextLine(first);
  assert(std::string(reader.line()) == "G28");

  Paper second("M105\n");
  reader.rewind();                       // Must not hand back what was left in the block
  reader.nextLine(second);
  assert(std::string(reader.line()) == "M105");
}

int main() {
  plainLinesComeBackWhole();
  aWindowsFilePrintsToo();
  theCountIsTheFilePosition();
  aLineSplitAcrossBlocksIsStillOneLine();
  aLineTooLongIsCutNotOverrun();
  aFileWithNoLastNewlineKeepsItsLastLine();
  emptyLinesSurvive();
  rewindStartsOverCleanly();

  printf("GcodeReader: 8 tests passed\n");
  return 0;
}
