#!/bin/sh
# Every host test in one go. Address and undefined behaviour sanitisers are always on.
set -e
CXX=/Library/Developer/CommandLineTools/usr/bin/clang++
FLAGS="-std=c++17 -O1 -g -fsanitize=address,undefined -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"
cd "$(dirname "$0")"
fail=0
for t in test_boot_guard test_web_auth test_command_queue test_printer_link test_card_transfer test_binary_transfer; do
  printf '\n=== %s ===\n' "$t"
  $CXX $FLAGS -I stubs -o "/tmp/$t" "$t.cpp"
  "/tmp/$t" || fail=1
done
# Both our sender and our fake printer were written from one reading of Marlin's binary
# protocol, so they would agree with each other even if the reading were wrong. This asks
# Marlin's own reference sender instead. Skipped when the Marlin tree is not beside us.
MARLIN="$HOME/Documents/My Projects/Kingroon KP3S/KP3S/Marlin-2.1.2"
if [ -f "$MARLIN/buildroot/share/scripts/MarlinBinaryProtocol.py" ]; then
  printf '\n=== packets against Marlin itself ===\n'
  $CXX $FLAGS -I stubs -I . -o /tmp/emit_packets emit.cpp
  /tmp/emit_packets /tmp/wire_packets.bin >/dev/null 2>&1
  python3 check_against_marlin.py /tmp/wire_packets.bin "$MARLIN" || fail=1
else
  printf '\n=== packets against Marlin itself: skipped, no Marlin tree ===\n'
fi

exit $fail
