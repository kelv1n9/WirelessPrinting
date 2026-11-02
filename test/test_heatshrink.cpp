// Our compressor, judged by Marlin's own decompressor.
//
// A round trip against an encoder and decoder both written here would agree with
// itself and prove nothing, the same trap test/check_against_marlin.py exists to
// avoid for the packet layer. So this links the decoder sources straight out of
// the user's Marlin tree, unmodified, and replays them exactly the way
// Marlin/src/feature/binary_stream.h does: 96 byte packets into sink, poll into a
// 512 byte buffer, write the buffer out whenever it fills, flush the rest on close.
// The window and lookahead sizes also come from Marlin's heatshrink_config.h, so
// they cannot drift away from the printer unnoticed.
//
// test/run.sh stages the copy of the Marlin sources and passes -I to it.

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include "heatshrink_decoder.h"        // Marlin's, verbatim
#include "heatshrink_config.h"         // Marlin's, verbatim
#include "../ESP8266WirelessPrintAsync/heatshrink_encoder.h"

static_assert(HeatshrinkEncoder::WINDOW_BITS == HEATSHRINK_STATIC_WINDOW_BITS,
              "the encoder's window does not match the printer's");
static_assert(HeatshrinkEncoder::LOOKAHEAD_BITS == HEATSHRINK_STATIC_LOOKAHEAD_BITS,
              "the encoder's lookahead does not match the printer's");

static int failures = 0;
static const char *scenario = "";
static void check(const bool ok, const char *what) {
  if (!ok) { printf("  FAIL  %s: %s\n", scenario, what); ++failures; }
}

// ---------------------------------------------------------------- the printer side

// What SDFileTransferProtocol::file_write and file_close do with a packet, minus
// the card. decode_buffer and the 96 byte packet size are Marlin's.
struct PrinterSide {
  heatshrink_decoder hsd;
  uint8_t decode_buffer[512];
  size_t data_waiting = 0;
  std::string file;

  PrinterSide() { heatshrink_decoder_reset(&hsd); }

  void write(const uint8_t *buffer, const size_t length) {
    size_t total_processed = 0, processed_count = 0;
    HSD_poll_res presult;
    while (total_processed < length) {
      heatshrink_decoder_sink(&hsd, const_cast<uint8_t *>(&buffer[total_processed]),
                              length - total_processed, &processed_count);
      total_processed += processed_count;
      do {
        presult = heatshrink_decoder_poll(&hsd, &decode_buffer[data_waiting],
                                          sizeof(decode_buffer) - data_waiting, &processed_count);
        data_waiting += processed_count;
        if (data_waiting == sizeof(decode_buffer)) {
          file.append(reinterpret_cast<char *>(decode_buffer), data_waiting);
          data_waiting = 0;
        }
      } while (presult == HSDR_POLL_MORE);
    }
  }

  void close() {
    if (data_waiting) {
      file.append(reinterpret_cast<char *>(decode_buffer), data_waiting);
      data_waiting = 0;
    }
    heatshrink_decoder_finish(&hsd);
  }
};

// ---------------------------------------------------------------- the module side

// Compresses `in` the way bare.cpp will: a chunk off the card at a time, the
// compressed bytes cut into Marlin's 96 byte write packets.
static std::string compress(const std::string &in, const size_t chunk) {
  static HeatshrinkEncoder enc;        // 2 KB, too big for the stack here or there
  enc.reset();
  std::vector<uint8_t> out(HeatshrinkEncoder::max_output(chunk));
  std::string packed;
  for (size_t at = 0; at < in.size(); at += chunk) {
    const size_t take = in.size() - at < chunk ? in.size() - at : chunk;
    const size_t n = enc.encode(reinterpret_cast<const uint8_t *>(in.data()) + at, take,
                                out.data(), out.size());
    // 0 is normal: the encoder holds input back until it has a full slice.
    packed.append(reinterpret_cast<char *>(out.data()), n);
  }
  packed.append(reinterpret_cast<char *>(out.data()), enc.finish(out.data(), out.size()));
  return packed;
}

static std::string through_marlin(const std::string &packed) {
  PrinterSide printer;
  for (size_t at = 0; at < packed.size(); at += 96) {
    const size_t take = packed.size() - at < 96 ? packed.size() - at : 96;
    printer.write(reinterpret_cast<const uint8_t *>(packed.data()) + at, take);
  }
  printer.close();
  return printer.file;
}

// Compresses, decompresses with Marlin's decoder, demands the original back.
static double round_trip(const std::string &in, const char *what, const size_t chunk = 4096) {
  const std::string packed = compress(in, chunk);
  const std::string back = through_marlin(packed);
  if (back != in) {
    printf("  FAIL  %s: Marlin's decoder gave back %zu bytes, not %zu\n",
           what, back.size(), in.size());
    size_t at = 0;
    while (at < back.size() && at < in.size() && back[at] == in[at]) at++;
    printf("        first difference at byte %zu\n", at);
    ++failures;
    return 0;
  }
  return in.empty() ? 0 : (double)in.size() / (double)packed.size();
}

// ---------------------------------------------------------------- the material

static uint32_t seed = 12345;
static uint32_t rnd() { seed = seed * 1103515245u + 12345u; return seed >> 8; }

// Sliced gcode, the shape a slicer emits it in: a header of settings, then layer
// after layer of short moves. No real sliced file ships with this repo.
static std::string fake_sliced_gcode(const int layers) {
  std::string g =
    ";FLAVOR:Marlin\n;TIME:14233\n;Filament used: 11.2374m\n;Layer height: 0.2\n"
    ";MINX:72.993\n;MINY:84.316\n;MINZ:0.2\n;MAXX:177.007\n;MAXY:165.684\n;MAXZ:60\n"
    ";Generated with Cura_SteamEngine 5.2.1\nM140 S60\nM105\nM190 S60\nM104 S210\n"
    "M105\nM109 S210\nM82 ;absolute extrusion mode\nG28 ;Home\nG1 Z15.0 F6000\nG92 E0\n";
  double e = 0;
  for (int l = 0; l < layers; l++) {
    char line[160];
    snprintf(line, sizeof line, ";LAYER:%d\nG0 F7500 X%.3f Y%.3f Z%.2f\n;TYPE:WALL-OUTER\n",
             l, 90.0 + (l % 7) * 0.4, 95.0 + (l % 5) * 0.3, 0.2 * (l + 1));
    g += line;
    for (int i = 0; i < 90; i++) {
      const double x = 95.0 + 30.0 * ((i * 37 % 101) / 101.0);
      const double y = 100.0 + 28.0 * ((i * 53 % 97) / 97.0);
      e += 0.0431;
      snprintf(line, sizeof line, "G1 X%.3f Y%.3f E%.5f\n", x, y, e);
      g += line;
      if (i % 30 == 29) g += "G0 F9000 X112.345 Y118.900\n;TYPE:FILL\n";
    }
    g += "G1 F1500 E" ; g += std::to_string((int)e); g += "\nM106 S255\n";
  }
  g += "M107\nG91\nG1 E-2 F2700\nG1 E-2 Z0.2 F2400\nG90\nM104 S0\nM140 S0\nM84 X Y E\n";
  return g;
}

static std::string from_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return std::string();
  std::string out;
  char b[4096];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
  fclose(f);
  return out;
}

int main(int argc, char **argv) {
  printf("window %d bits, lookahead %d bits, taken from Marlin's heatshrink_config.h\n",
         HEATSHRINK_STATIC_WINDOW_BITS, HEATSHRINK_STATIC_LOOKAHEAD_BITS);
  printf("encoder state: %zu bytes\n\n", sizeof(HeatshrinkEncoder));

  scenario = "the empty cases";
  {
    check(round_trip(std::string(), "empty input") == 0, "nothing in, nothing out");
    check(compress(std::string(), 4096).empty(), "an empty file compresses to no bytes");
    round_trip(std::string("A"), "one byte");
    round_trip(std::string("AB"), "two bytes");
    round_trip(std::string("AA"), "two bytes that repeat");
    round_trip(std::string(1, '\0'), "a single zero byte");
    round_trip(std::string(40, '\0'), "a run of zeros");
  }

  scenario = "the window edges";
  {
    for (size_t n : {size_t(255), size_t(256), size_t(257), size_t(511), size_t(512), size_t(513),
                     size_t(4095), size_t(4096), size_t(4097)}) {
      std::string s;
      for (size_t i = 0; i < n; i++) s += (char)('a' + (i % 23));
      char what[64]; snprintf(what, sizeof what, "%zu bytes of a repeating pattern", n);
      round_trip(s, what);
      std::string r;
      for (size_t i = 0; i < n; i++) r += (char)(rnd() & 0xFF);
      snprintf(what, sizeof what, "%zu bytes of noise", n);
      round_trip(r, what);
    }
    // A match reaching the full 256 back, and one overlapping itself.
    std::string far;
    for (int i = 0; i < 256; i++) far += (char)('A' + (i % 26));
    far += far.substr(0, 16);
    round_trip(far, "a back reference the whole window away");
    round_trip(std::string(5000, 'Z'), "one byte repeated past the window");
  }

  scenario = "input that does not compress";
  {
    std::string noise;
    for (int i = 0; i < 64 * 1024; i++) noise += (char)(rnd() & 0xFF);
    const double ratio = round_trip(noise, "64 KB of random binary");
    printf("  64 KB of random binary: ratio %.3f (expansion is correct here)\n", ratio);
    std::string every;
    for (int i = 0; i < 256; i++) every += (char)i;
    round_trip(every, "every byte value once");
  }

  scenario = "chunking must not change the answer";
  {
    const std::string g = fake_sliced_gcode(40);
    const std::string reference = compress(g, 1u << 20);
    for (size_t chunk : {size_t(1), size_t(7), size_t(96), size_t(255), size_t(256),
                         size_t(257), size_t(512), size_t(4096), size_t(65536)}) {
      const std::string other = compress(g, chunk);
      char what[80]; snprintf(what, sizeof what, "fed %zu bytes at a time", chunk);
      // Byte identical output whatever the chunking is, so the card's read size can
      // never change what the printer receives.
      if (other != reference) { printf("  FAIL  %s: %s\n", scenario, what); ++failures; }
      round_trip(g, what, chunk);
    }
  }

  scenario = "real sliced gcode";
  {
    // cold_cube_head.gcode is the first 2000 lines of a print pulled off the
    // module's card, OrcaSlicer 2.4.1 for this printer, byte for byte: the
    // settings header, the base64 PNG thumbnail, and the first layers. A head
    // extract, so its ratio is that head's, not a whole print's. The full
    // 604 KB file round-tripped too, it is just too big to keep in the repo.
    const std::string real = from_file("cold_cube_head.gcode");
    check(!real.empty(), "the real gcode extract is there");
    if (!real.empty()) {
      double ratio = 0;
      for (size_t chunk : {size_t(1), size_t(96), size_t(512), size_t(4096), size_t(65536)}) {
        char what[80]; snprintf(what, sizeof what, "real gcode fed %zu bytes at a time", chunk);
        ratio = round_trip(real, what, chunk);
      }
      printf("  %-40s %7zu bytes, ratio %.2f\n", "cold_cube_head.gcode (OrcaSlicer)",
             real.size(), ratio);
    }

    const char *marlin = argc > 1 ? argv[1] : "";
    const char *paths[] = {
      "/buildroot/test-gcode/M808-loops.gcode",
      "/buildroot/share/sublime/RepRapTools/syntax_test_G-code.gcode",
    };
    for (const char *rel : paths) {
      const std::string body = from_file((std::string(marlin) + rel).c_str());
      if (body.empty()) continue;
      const double ratio = round_trip(body, rel);
      printf("  %-40s %7zu bytes, ratio %.2f\n", rel + 1, body.size(), ratio);
    }
  }

  scenario = "a whole print, far larger than the window";
  {
    const std::string g = fake_sliced_gcode(400);
    const clock_t t0 = clock();
    const std::string packed = compress(g, 4096);
    const double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    const std::string back = through_marlin(packed);
    check(back == g, "Marlin's decoder gives back every byte");
    printf("  sliced gcode %.2f MB -> %.2f MB, ratio %.2f, %.1f MB/s on this host\n",
           g.size() / 1048576.0, packed.size() / 1048576.0,
           (double)g.size() / (double)packed.size(), g.size() / 1048576.0 / secs);
  }

  printf(failures ? "\n%d checks failed\n" : "\nall checks passed\n", failures);
  return failures != 0;
}
