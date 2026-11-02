#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>

static size_t feed(const unsigned char *picture, size_t total,
                   unsigned char *buffer, size_t maxLen, size_t index) {
  const size_t left = total - index;
  const size_t take = maxLen < left ? maxLen : left;
  memcpy(buffer, picture + index, take);
  return take;
}

static void whole(size_t total, size_t maxLen) {
  std::vector<unsigned char> src(total);
  for (size_t i = 0; i < total; i++)
    src[i] = (unsigned char)(i * 31 + 7);

  std::vector<unsigned char> out;
  size_t index = 0;
  int rounds = 0;
  while (index < total) {
    std::vector<unsigned char> chunk(maxLen);
    const size_t got = feed(src.data(), total, chunk.data(), maxLen, index);
    assert(got > 0);
    assert(got <= maxLen);
    assert(index + got <= total);
    out.insert(out.end(), chunk.begin(), chunk.begin() + got);
    index += got;
    assert(++rounds < 100000);
  }
  assert(out.size() == total);
  assert(memcmp(out.data(), src.data(), total) == 0);

  unsigned char spare[8];
  assert(feed(src.data(), total, spare, sizeof(spare), total) == 0);
}

int main() {
  whole(36864, 5744);
  whole(36864, 1436);
  whole(1, 5744);
  whole(5744, 5744);
  whole(5745, 5744);
  whole(2048, 1);
  printf("thumbnail feed: all checks passed\n");
  return 0;
}
