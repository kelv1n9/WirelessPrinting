import os
from pathlib import Path
import random
import shlex
import subprocess
import sys
import tempfile
import zlib

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('/Users/elvin/Documents/My Projects/Kingroon KP3S/KP3S/Marlin-2.1.2')
source = r'''
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "crc32.h"
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const size_t chunk = strtoul(argv[1], nullptr, 10);
  if (!chunk) return 2;
  std::vector<uint8_t> buffer(chunk);
  uint32_t crc = 0xFFFFFFFFU;
  size_t got;
  while ((got = fread(buffer.data(), 1, chunk, stdin)))
    crc = crc32_update(crc, buffer.data(), got);
  printf("%u", uint32_t(~crc));
  return ferror(stdin) ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory(prefix='printer-crc-') as folder:
    cpp = Path(folder) / 'crc.cpp'
    exe = Path(folder) / 'crc'
    cpp.write_text(source)
    subprocess.run([os.environ.get('CXX', 'c++')] + shlex.split(os.environ.get('CXXFLAGS', '')) + ['-std=c++14', '-fsanitize=address,undefined', '-I', str(root / 'Marlin/src/libs'), str(cpp), '-o', str(exe)], check=True)
    rng = random.Random(7)
    bodies = [b'', b'123456789', bytes(range(256)), bytes(rng.randrange(256) for _ in range(8193)), bytes(rng.randrange(256) for _ in range(100003))]
    for body in bodies:
        for chunk in [1, 128, 512, 4096]:
            result = subprocess.check_output([str(exe), str(chunk)], input=body)
            assert int(result) == zlib.crc32(body), (len(body), chunk)
print('Printer CRC32 matches zlib across empty, binary and fragmented input')
