#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#define DOWNLOAD_CACHE_BYTES 4096
#include "../ESP8266WirelessPrintAsync/FileFeeder.h"

// Stands in for the card: hands back at most biteSize bytes per call, so a reader that
// assumes one read fills its buffer is caught here instead of on the module.
class Paper {
  public:
    Paper(const std::string &body, size_t biteSize = 0)
      : text(body), bite(biteSize ? biteSize : body.size() + 1) {}

    int read(uint8_t *into, size_t room) {
      if (at >= text.size())
        return 0;
      size_t take = room < bite ? room : bite;
      if (take > text.size() - at)
        take = text.size() - at;
      memcpy(into, text.data() + at, take);
      at += take;
      reads++;
      return (int)take;
    }

    size_t at = 0, reads = 0;

  private:
    std::string text;
    size_t bite;
};

static std::string drain(const std::string &body, size_t room, size_t bite = 0, size_t declared = 0) {
  Paper paper(body, bite);
  FileFeeder<Paper> feeder;
  feeder.start(declared ? declared : body.size());

  std::string out;
  std::vector<uint8_t> buffer(room);
  for (int guard = 0; guard < 100000; ++guard) {
    const size_t served = feeder.serve(paper, buffer.data(), room);
    if (!served)
      break;
    assert(served <= room);
    out.append((const char *)buffer.data(), served);
  }
  return out;
}

static std::string ramp(size_t bytes) {
  std::string s;
  s.reserve(bytes);
  for (size_t i = 0; i < bytes; ++i)
    s.push_back((char)('a' + (i % 26)));
  return s;
}

static void aSmallFileComesBackWhole() {
  const std::string body = ramp(100);
  assert(drain(body, 64) == body);
}

static void aFileLargerThanTheCacheComesBackWhole() {
  const std::string body = ramp(DOWNLOAD_CACHE_BYTES * 5 + 123);
  assert(drain(body, 1436) == body);
}

static void theRoomOfferedNeverOverflows() {
  const std::string body = ramp(DOWNLOAD_CACHE_BYTES * 3);
  for (size_t room : {(size_t)1, (size_t)7, (size_t)1436, (size_t)4095, (size_t)4096,
                      (size_t)4097, (size_t)5744, (size_t)8192})
    assert(drain(body, room) == body);
}

static void aCardHandingBackDribblesStillFills() {
  const std::string body = ramp(DOWNLOAD_CACHE_BYTES * 2 + 55);
  for (size_t bite : {(size_t)1, (size_t)3, (size_t)512, (size_t)1000, (size_t)4096})
    assert(drain(body, 5744, bite) == body);
}

static void itStopsAtTheDeclaredLengthNotTheFileEnd() {
  const std::string body = ramp(10000);
  assert(drain(body, 1436, 0, 4000) == body.substr(0, 4000));
}

static void aShortFileEndsInsteadOfSpinning() {
  const std::string body = ramp(500);
  assert(drain(body, 1436, 0, 9999) == body);
}

static void anEmptyFileServesNothing() {
  Paper paper("");
  FileFeeder<Paper> feeder;
  feeder.start(0);
  uint8_t buffer[64];
  assert(feeder.serve(paper, buffer, sizeof(buffer)) == 0);
  assert(feeder.spent());
}

static void oneAckCarriesAsMuchAsItCan() {
  const std::string body = ramp(DOWNLOAD_CACHE_BYTES * 3);
  Paper paper(body);
  FileFeeder<Paper> feeder;
  feeder.start(body.size());
  std::vector<uint8_t> buffer(5744);
  assert(feeder.serve(paper, buffer.data(), 5744) == 5744);   // not clipped to the 4096 cache
}

static void twoFeedersDoNotShareAnything() {
  const std::string one = ramp(9000), two = ramp(7000);
  Paper paperOne(one), paperTwo(two);
  FileFeeder<Paper> feederOne, feederTwo;
  feederOne.start(one.size());
  feederTwo.start(two.size());

  std::string gotOne, gotTwo;
  std::vector<uint8_t> buffer(1436);
  while (!feederOne.spent() || !feederTwo.spent()) {
    size_t n = feederOne.serve(paperOne, buffer.data(), 1436);
    gotOne.append((const char *)buffer.data(), n);
    n = feederTwo.serve(paperTwo, buffer.data(), 1436);
    gotTwo.append((const char *)buffer.data(), n);
  }
  assert(gotOne == one);
  assert(gotTwo == two);
}

int main() {
  aSmallFileComesBackWhole();
  aFileLargerThanTheCacheComesBackWhole();
  theRoomOfferedNeverOverflows();
  aCardHandingBackDribblesStillFills();
  itStopsAtTheDeclaredLengthNotTheFileEnd();
  aShortFileEndsInsteadOfSpinning();
  anEmptyFileServesNothing();
  oneAckCarriesAsMuchAsItCan();
  twoFeedersDoNotShareAnything();
  printf("FileFeeder: 9 tests passed\n");
  return 0;
}
