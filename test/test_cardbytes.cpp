#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

struct ByteSource {
  virtual ~ByteSource() {}
  virtual int next(uint8_t *, size_t) = 0;
  virtual uint32_t size() const = 0;
};

struct Entry {
  std::vector<uint8_t> bytes;
  uint32_t reported = 0;
  size_t shortRead = 4096;
  size_t errorAt = std::numeric_limits<size_t>::max();
  size_t calls = 0, closes = 0, largestRead = 0;
};

struct File {
  Entry *entry = nullptr;
  size_t position = 0;
  bool opened = false;

  File() = default;
  explicit File(Entry *e) : entry(e), opened(e != nullptr) {}
  explicit operator bool() const { return opened; }
  uint32_t size() const { return entry->reported; }
  void close() {
    if (opened) ++entry->closes;
    opened = false;
  }
  int read(uint8_t *out, size_t max) {
    assert(opened && out && max && max <= 4096);
    assert(reinterpret_cast<uintptr_t>(out) % 32 == 0);
    ++entry->calls;
    entry->largestRead = std::max(entry->largestRead, max);
    if (position >= entry->errorAt) return -1;
    const size_t count = std::min({max, entry->shortRead,
                                  entry->bytes.size() - position, entry->errorAt - position});
    if (count) std::memcpy(out, entry->bytes.data() + position, count);
    position += count;
    return int(count);
  }
};

struct {
  std::map<std::string, Entry> files;
  File open(const char *path) {
    const auto found = files.find(path);
    return found == files.end() ? File() : File(&found->second);
  }
} SD_MMC;

#include "../bare/cardbytes.h"

static Entry &put(const char *name, size_t length, uint8_t seed = 0) {
  Entry &entry = SD_MMC.files[name];
  entry = Entry();
  entry.bytes.resize(length);
  entry.reported = uint32_t(length);
  for (size_t i = 0; i < length; ++i)
    entry.bytes[i] = uint8_t(i * 37 + (i >> 8) + seed);
  return entry;
}

static size_t drain(CardBytes &reader, const Entry &entry, size_t chunk, bool error = false) {
  std::vector<uint8_t> out(chunk + 2, 0xA5);
  size_t total = 0;
  int got;
  while ((got = reader.next(out.data() + 1, chunk)) > 0) {
    assert(size_t(got) <= chunk && got <= 4096);
    if (!error && entry.shortRead == 4096)
      assert(size_t(got) == std::min({chunk, size_t(4096) - total % 4096, entry.bytes.size() - total}));
    assert(total + size_t(got) <= entry.bytes.size());
    assert(std::memcmp(out.data() + 1, entry.bytes.data() + total, size_t(got)) == 0);
    assert(out.front() == 0xA5 && out.back() == 0xA5);
    total += size_t(got);
  }
  assert(got == (error ? -1 : 0));
  return total;
}

int main() {
  static CardBytes reader;
  unsigned cases = 0;
  for (size_t length : {size_t(0), size_t(1), size_t(4095), size_t(4096), size_t(4097), size_t(1048576)}) {
    for (size_t chunk : {size_t(1), size_t(96), size_t(256), size_t(1024), size_t(8192)}) {
      reader.shut();
      Entry &entry = put("/normal", length);
      assert(reader.open("/normal") == (length > 0));
      assert(reader.size() == length);
      if (length) {
        assert(drain(reader, entry, chunk) == length);
        assert(reader.consumed == length);
        const size_t reads = (length + 4095) / 4096;
        assert(entry.calls == reads);
        uint8_t byte;
        assert(reader.next(&byte, 1) == 0 && entry.calls == reads);
      }
      else {
        uint8_t byte;
        assert(!reader.file && entry.closes == 1);
        assert(reader.next(&byte, 1) == -1 && entry.calls == 0);
      }
      reader.shut();
      assert(reader.size() == length && !reader.file);
      ++cases;
    }
  }
  for (size_t shortRead : {size_t(1), size_t(7), size_t(511), size_t(4095)}) {
    Entry &entry = put("/short", 8193);
    entry.shortRead = shortRead;
    assert(reader.open("/short"));
    assert(drain(reader, entry, 1024) == entry.bytes.size());
    assert(entry.calls == (entry.bytes.size() + shortRead - 1) / shortRead);
    reader.shut();
    ++cases;
  }
  for (size_t available : {size_t(0), size_t(1), size_t(4096), size_t(4097)}) {
    Entry &entry = put("/truncated", available);
    entry.reported = 10000;
    assert(reader.open("/truncated"));
    assert(drain(reader, entry, 96, true) == available);
    assert(reader.size() == 10000 && !reader.file && entry.closes == 1);
    uint8_t byte;
    const size_t calls = entry.calls;
    assert(reader.next(&byte, 1) == -1 && entry.calls == calls);
    ++cases;
  }
  for (size_t errorAt : {size_t(0), size_t(1), size_t(4096), size_t(4100)}) {
    Entry &entry = put("/error", 10000);
    entry.errorAt = errorAt;
    assert(reader.open("/error"));
    assert(drain(reader, entry, 256, true) == errorAt);
    assert(reader.size() == 10000 && !reader.file && entry.closes == 1);
    assert(reader.at == 0 && reader.cached == 0 && reader.consumed == 0);
    ++cases;
  }
  Entry &first = put("/first", 9000, 1);
  Entry &second = put("/second", 4097, 173);
  assert(reader.open("/first"));
  std::vector<uint8_t> out(8192);
  assert(reader.next(out.data(), 96) == 96 && first.calls == 1);
  assert(reader.open("/second") && first.closes == 1 && reader.consumed == 0);
  assert(drain(reader, second, 8192) == second.bytes.size());
  assert(second.calls == 2);
  assert(reader.open("/first") && second.closes == 1);
  const size_t priorCalls = first.calls;
  assert(reader.next(out.data(), out.size()) == 4096 && first.calls == priorCalls + 1);
  reader.shut();
  assert(reader.size() == first.reported && reader.at == 0 && reader.cached == 0 && reader.consumed == 0);
  assert(reader.next(out.data(), 96) == -1 && first.calls == priorCalls + 1);
  reader.shut();
  assert(first.closes == 2 && reader.size() == 9000);
  assert(reader.open("/second"));
  assert(reader.next(out.data(), 1) == 1);
  assert(!reader.open("/missing") && !reader.file && reader.size() == 0);
  assert(reader.next(out.data(), 1) == -1);
  assert(reader.open("/second"));
  const size_t secondCalls = second.calls;
  assert(reader.next(nullptr, 1) == -1 && reader.next(out.data(), 0) == -1);
  assert(second.calls == secondCalls && reader.consumed == 0);
  assert(drain(reader, second, 96) == second.bytes.size());
  reader.shut();
  std::printf("PASS CardBytes: %u length/chunk/short-read/error cases plus reopen, close, EOF, invalid reads and aligned 4KB refills\n", cases);
}
