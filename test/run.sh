#!/bin/sh
# Every host test in one go. Address and undefined behaviour sanitisers are always on.
set -e
CXX=/Library/Developer/CommandLineTools/usr/bin/clang++
FLAGS="-std=c++17 -O1 -g -fsanitize=address,undefined -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"
cd "$(dirname "$0")"
fail=0
for t in test_boot_guard test_web_auth test_command_queue test_printer_link test_card_transfer; do
  printf '\n=== %s ===\n' "$t"
  $CXX $FLAGS -I stubs -o "/tmp/$t" "$t.cpp"
  "/tmp/$t" || fail=1
done
exit $fail
