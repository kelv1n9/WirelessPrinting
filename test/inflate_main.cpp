#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../bare/inflate.h"
int main(int argc, char **argv) {
  const size_t chunk = argc > 1 ? atoi(argv[1]) : 2040;
  std::vector<uint8_t> in; int c;
  while ((c = getchar()) != EOF) in.push_back((uint8_t)c);
  const size_t window = argc > 2 ? atoi(argv[2]) : 16384;
  std::vector<uint8_t> room(window);
  Inflate z;
  if (!z.begin(room.data(), window)) return 3;
  bool good = true;
  for (size_t at = 0; at < in.size() && good; at += chunk) {
    const size_t n = in.size() - at < chunk ? in.size() - at : chunk;
    good = z.feed(in.data() + at, n, at + n >= in.size(), [](const uint8_t *p, size_t k) { fwrite(p, 1, k, stdout); });
  }
  if (in.empty()) good = z.feed(nullptr, 0, true, [](const uint8_t *p, size_t k) { fwrite(p, 1, k, stdout); });
  z.end();
  return good && z.finished() ? 0 : 2;
}
