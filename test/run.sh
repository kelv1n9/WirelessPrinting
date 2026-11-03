#!/bin/sh
# Every host test in one go. Address and undefined behaviour sanitisers are always on.
set -e
CXX=/Library/Developer/CommandLineTools/usr/bin/clang++
FLAGS="-std=c++17 -O1 -g -fsanitize=address,undefined -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"
cd "$(dirname "$0")"
fail=0
MARLIN="$HOME/Documents/My Projects/Kingroon KP3S/KP3S/Marlin-2.1.2"
HS="$MARLIN/Marlin/src/libs/heatshrink"
STAGE=/tmp/hs_marlin
if [ -f "$HS/heatshrink_decoder.cpp" ]; then
  rm -rf "$STAGE"
  mkdir -p "$STAGE/Marlin/src/libs/heatshrink" "$STAGE/Marlin/src/inc"
  cp "$HS/heatshrink_decoder.cpp" "$HS/heatshrink_decoder.h" \
     "$HS/heatshrink_common.h" "$HS/heatshrink_config.h" "$STAGE/Marlin/src/libs/heatshrink/"
  echo '#pragma once
#define ENABLED(V) 1' > "$STAGE/Marlin/src/inc/MarlinConfigPre.h"
fi

RUN=$(mktemp -d "${TMPDIR:-/tmp}/kp3s-tests.XXXXXX")
trap 'rm -rf "$RUN"' EXIT
pids=""
batch=""
wait_batch() {
  for pid in $pids; do
    wait "$pid" || fail=1
  done
  for name in $batch; do
    printf '\n=== %s ===\n' "$name"
    cat "$RUN/$name.log"
  done
  pids=""
  batch=""
}
count=0
for t in test_ota test_cardbytes test_boot_guard test_web_auth test_command_queue test_printer_link test_card_transfer test_binary_transfer test_printercard test_gcodemeta; do
  (
    $CXX $FLAGS -I stubs -o "$RUN/$t" "$t.cpp" && "$RUN/$t"
  ) > "$RUN/$t.log" 2>&1 &
  pids="$pids $!"
  batch="$batch $t"
  count=$((count + 1))
  if [ "$count" -eq 4 ]; then
    wait_batch
    count=0
  fi
done
wait_batch
# Both our sender and our fake printer were written from one reading of Marlin's binary
# protocol, so they would agree with each other even if the reading were wrong. This asks
# Marlin's own reference sender instead. Skipped when the Marlin tree is not beside us.

# Same reason, for the compressor: our encoder is judged by Marlin's own decoder,
# copied out of their tree untouched. Only the config system is stubbed, so that
# the decoder compiles on the host without dragging in a board and a HAL.
if [ -f "$HS/heatshrink_decoder.cpp" ]; then
  # The whole transfer again, this time with a printer that really decompresses.
  printf '\n=== test_binary_transfer, compressed, against Marlin own decoder ===\n'
  $CXX $FLAGS -Wno-unknown-pragmas -I stubs -DMARLIN_SIM_HEATSHRINK \
    -I "$STAGE/Marlin/src/libs/heatshrink" \
    -o /tmp/test_binary_transfer_packed test_binary_transfer.cpp \
    "$STAGE/Marlin/src/libs/heatshrink/heatshrink_decoder.cpp"
  /tmp/test_binary_transfer_packed || fail=1

  printf '\n=== heatshrink, against Marlin own decoder ===\n'
  $CXX $FLAGS -Wno-unknown-pragmas -I stubs -I "$STAGE/Marlin/src/libs/heatshrink" \
    -o /tmp/test_heatshrink test_heatshrink.cpp "$STAGE/Marlin/src/libs/heatshrink/heatshrink_decoder.cpp"
  /tmp/test_heatshrink "$MARLIN" || fail=1
else
  printf '\n=== heatshrink, against Marlin own decoder: skipped, no Marlin tree ===\n'
fi

if [ -f "$MARLIN/buildroot/share/scripts/MarlinBinaryProtocol.py" ]; then
  printf '\n=== packets against Marlin itself ===\n'
  $CXX $FLAGS -I stubs -I . -o /tmp/emit_packets emit.cpp
  /tmp/emit_packets /tmp/wire_packets.bin >/dev/null 2>&1
  python3 check_against_marlin.py /tmp/wire_packets.bin "$MARLIN" || fail=1
else
  printf '\n=== packets against Marlin itself: skipped, no Marlin tree ===\n'
fi

CXX="$CXX" CXXFLAGS="$FLAGS" python3 -B test_printer_rx_dma.py "$MARLIN" || fail=1
CXX="$CXX" CXXFLAGS="$FLAGS" python3 -B test_printer_crc.py "$MARLIN" || fail=1
CXX="$CXX" python3 -B test_printer_sdio.py "$MARLIN" || fail=1
CXX="$CXX" CXXFLAGS="$FLAGS" python3 -B test_transfer_completion.py || fail=1

CXX="$CXX" CXXFLAGS="$FLAGS" python3 -B test_ui_metadata.py || fail=1
node test_page_ui.js || fail=1

exit $fail
