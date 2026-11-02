// What the scanner makes of real sliced files, judged against what the files say about
// themselves in their own header and against a count taken with grep.

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include "../bare/gcodemeta.h"

static int bad = 0;

static void check(const bool good, const char *what) {
  if (!good) {
    printf("  FAIL  %s\n", what);
    bad = 1;
  }
}

struct Collect : GcodeMetaSink {
  std::vector<GcodeMetaLayer> rows;
  std::string picture, taking;
  int opened = 0, kept = 0;

  void layer(const GcodeMetaLayer &one) override { rows.push_back(one); }
  void thumbOpen(uint16_t, uint16_t) override { taking.clear(); opened++; }
  void thumbByte(uint8_t value) override { taking += (char)value; }
  void thumbClose(bool keep) override { if (keep) { picture = taking; kept++; } }
};

static std::string slurp(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f)
    return "";
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::string out((size_t)n, 0);
  if (fread(&out[0], 1, (size_t)n, f) != (size_t)n)
    out.clear();
  fclose(f);
  return out;
}

// The same thing grep would say, so the scanner is judged against the file and not
// against another reading of the file by the same hand.
static size_t countLines(const std::string &text, const char *prefix) {
  size_t n = 0, at = 0;
  const size_t len = strlen(prefix);
  while (at <= text.size()) {
    const size_t nl = text.find('\n', at);
    const size_t end = nl == std::string::npos ? text.size() : nl;
    if (end - at >= len && !text.compare(at, len, prefix))
      n++;
    if (nl == std::string::npos)
      break;
    at = nl + 1;
  }
  return n;
}

static void scanOne(const std::string &path, const size_t chunk) {
  const std::string body = slurp(path);
  if (body.empty()) {
    printf("  skipped, no %s\n", path.c_str());
    return;
  }

  Collect into;
  GcodeMetaScan scan;
  scan.begin(&into);
  for (size_t at = 0; at < body.size(); at += chunk)
    scan.feed((const uint8_t *)body.data() + at,
              body.size() - at < chunk ? body.size() - at : chunk);
  scan.finish((uint32_t)body.size());

  const size_t changes = countLines(body, ";LAYER_CHANGE");
  const char *name = strrchr(path.c_str(), '/');
  name = name ? name + 1 : path.c_str();

  if (chunk == 4096)
    printf("  %-30s %u layers declared, %zu found, %u s, %.2f g, thumb %ux%u %u bytes\n",
           name, scan.head.layers, into.rows.size(), scan.head.seconds,
           scan.head.grams, scan.head.thumbW, scan.head.thumbH, scan.head.thumbLen);

  // A slicer writes its estimate and its weight in the block it appends at the end,
  // so a file cut short has a header and a picture but none of those.
  const bool whole = body.find("estimated printing time") != std::string::npos;

  check(into.rows.size() == changes, "every layer change is written down");
  check(scan.head.thumbLen > 0 && scan.head.thumbW > 0, "a picture was located");
  check(into.opened > 0 && into.kept > 0, "and handed over");
  check(into.picture.size() > 100, "the decoded picture is not empty");
  check(into.picture.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0, "and it really is a png");
  check(into.picture.size() >= scan.head.thumbLen / 2 &&
        into.picture.size() <= scan.head.thumbLen,
        "its size is what that much base64 decodes to");
  if (whole) {
    check(scan.head.layers == changes, "and the header agreed with them");
    check(scan.head.seconds > 0, "the estimate was read");
    check(scan.head.grams > 0, "the filament was read");
  }
  else
    check(into.rows.size() <= scan.head.layers, "a file cut short has no more layers than it declared");

  // The located range has to be the base64 and nothing else
  const std::string thumb = body.substr(scan.head.thumbAt, scan.head.thumbLen);
  check(thumb.compare(0, 2, "; ") == 0, "the picture starts at a comment of base64");
  check(thumb.find("thumbnail begin") == std::string::npos, "and not at the line that opens it");
  check(thumb.find("thumbnail end") == std::string::npos, "and stops before the one that closes it");

  // Heights climb, offsets climb, and nothing sits past the end of the file
  uint32_t lastAt = 0;
  uint16_t lastZ = 0;
  bool climbs = true, inside = true;
  for (const GcodeMetaLayer &one : into.rows) {
    if (one.at < lastAt || one.z100 < lastZ)
      climbs = false;
    if (one.at >= body.size())
      inside = false;
    lastAt = one.at;
    lastZ = one.z100;
  }
  check(climbs, "offsets and heights only ever climb");
  check(inside, "and none of them points past the file");
  check(into.rows.empty() || into.rows.front().minutes >= into.rows.back().minutes,
        "the time left never grows");

  // Every offset has to land exactly on the line it claims
  bool lands = true;
  for (const GcodeMetaLayer &one : into.rows)
    if (body.compare(one.at, 13, ";LAYER_CHANGE"))
      lands = false;
  check(lands, "every offset lands on the layer change itself");

  // And the lookup has to give the layer a byte really belongs to
  if (into.rows.size() > 3) {
    uint16_t z = 0, left = 0;
    check(gcodeMetaLayerAt(into.rows.data(), into.rows.size(), into.rows[2].at, &z, &left) == 3,
          "the first byte of a layer belongs to that layer");
    check(z == into.rows[2].z100, "with its own height");
    check(gcodeMetaLayerAt(into.rows.data(), into.rows.size(), into.rows[2].at - 1, &z, &left) == 2,
          "and the byte before it to the one before");
    check(gcodeMetaLayerAt(into.rows.data(), into.rows.size(), 0, &z, &left) == 0,
          "nothing before the first layer belongs to a layer at all");
    check(gcodeMetaLayerAt(into.rows.data(), into.rows.size(),
                           (uint32_t)body.size(), &z, &left) == into.rows.size(),
          "and the end of the file belongs to the last one");
  }
}

int main(int argc, char **argv) {
  const std::string here = argc > 1 ? argv[1] : "cold_cube_head.gcode";

  // Chunk size must not change the answer: the upload hands over whatever arrives.
  const size_t chunks[] = {1, 7, 512, 4096, 65536};
  for (const size_t chunk : chunks)
    scanOne(here, chunk);

  for (int i = 2; i < argc; i++)
    for (const size_t chunk : chunks)
      scanOne(argv[i], chunk);

  printf(bad ? "\nsomething is wrong\n" : "\nall checks passed\n");
  return bad;
}
