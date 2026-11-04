// The relay's loopback writer against a sink that stops taking bytes for a while, the
// way the firmware route does while it erases flash.
#include <cstdio>
#include <cstring>
#include <vector>
#include "../bare/relaypour.h"

static uint32_t clockMs = 0;

struct StallingSink {
  std::vector<uint8_t> got;
  uint32_t stallFrom, stallFor, perWrite;
  bool open = true;
  bool connected() const { return open; }
  size_t write(const uint8_t *d, size_t n) {
    if (clockMs >= stallFrom && clockMs < stallFrom + stallFor) return 0;
    const size_t take = n < perWrite ? n : perWrite;
    got.insert(got.end(), d, d + take);
    return take;
  }
};

static int bad = 0;
static void check(bool good, const char *what) { if (!good) { std::printf("  FAIL  %s\n", what); bad = 1; } }

int main() {
  std::vector<uint8_t> data(50000);
  for (size_t i = 0; i < data.size(); i++) data[i] = (uint8_t)(i * 7 + 3);
  auto now = [] { return clockMs; };
  auto nap = [] { clockMs += 5; };

  {
    clockMs = 0;
    StallingSink s{{}, 0, 8000, 1460};
    const bool ok = relayPourAll(s, data.data(), data.size(), 30000, now, nap);
    check(ok, "an eight second stall at the start is waited out");
    check(s.got == data, "and every byte arrives in order");
  }
  {
    clockMs = 0;
    StallingSink s{{}, 20, 12000, 700};
    bool ok = true;
    for (size_t at = 0; at < data.size(); at += 2044) {
      const size_t n = data.size() - at < 2044 ? data.size() - at : 2044;
      ok = ok && relayPourAll(s, data.data() + at, n, 30000, now, nap);
    }
    check(ok, "a stall in the middle of piecewise writes is waited out");
    check(s.got == data, "with nothing lost or doubled");
  }
  {
    clockMs = 0;
    StallingSink s{{}, 0, 1000000, 1460};
    const bool ok = relayPourAll(s, data.data(), data.size(), 30000, now, nap);
    check(!ok, "a sink that never reads again is given up on");
    check(clockMs >= 30000 && clockMs < 31000, "after the patience and not long after");
  }
  {
    clockMs = 0;
    StallingSink s{{}, 0, 100, 1460};
    s.open = false;
    check(!relayPourAll(s, data.data(), data.size(), 30000, now, nap), "a closed sink fails at once");
    check(clockMs == 0, "without waiting");
  }
  std::printf(bad ? "\nsomething is wrong\n" : "\nrelay pour: all checks passed\n");
  return bad;
}
