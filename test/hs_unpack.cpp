// Unpacks a heatshrink 8,4 stream from stdin to stdout with Marlin's own decoder, so
// the Worker's JavaScript encoder is judged against the code the printer runs.
#include <cstdio>
#include <vector>
#include "Marlin/src/libs/heatshrink/heatshrink_decoder.h"

int main() {
  static heatshrink_decoder hsd;
  heatshrink_decoder_reset(&hsd);
  std::vector<uint8_t> in;
  int c;
  while ((c = getchar()) != EOF) in.push_back((uint8_t)c);
  uint8_t out[512];
  size_t at = 0;
  while (at < in.size()) {
    size_t sunk = 0;
    heatshrink_decoder_sink(&hsd, in.data() + at, in.size() - at, &sunk);
    at += sunk;
    size_t got;
    do {
      got = 0;
      heatshrink_decoder_poll(&hsd, out, sizeof out, &got);
      fwrite(out, 1, got, stdout);
    } while (got);
  }
  while (heatshrink_decoder_finish(&hsd) == HSDR_FINISH_MORE) {
    size_t got = 0;
    heatshrink_decoder_poll(&hsd, out, sizeof out, &got);
    fwrite(out, 1, got, stdout);
  }
  return 0;
}
