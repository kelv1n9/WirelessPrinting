#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../bare/hsunpack.h"
int main(int argc, char **argv) {
  const size_t chunk = argc > 1 ? atoi(argv[1]) : 2040;
  std::vector<uint8_t> in; int c;
  while ((c = getchar()) != EOF) in.push_back((uint8_t)c);
  static HsUnpack u; u.begin();
  for (size_t at = 0; at < in.size(); at += chunk)
    u.feed(in.data() + at, in.size() - at < chunk ? in.size() - at : chunk, [](uint8_t b) { putchar(b); });
  return 0;
}
