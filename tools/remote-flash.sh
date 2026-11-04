#!/bin/bash
# Flashes the module through the Cloudflare relay, and keeps trying until the module
# is running the new build. Ctrl+C stops it; nothing is left half done on the module,
# a broken flash leaves the old firmware running.
#
#   tools/remote-flash.sh           build, then try until it is on
#   tools/remote-flash.sh --check   only say what the module is running now

cd "$(dirname "$0")/.." || exit 1
RELAY=https://kp3s.gadirov190102.workers.dev
LOCAL=192.168.1.83
PIO=~/.platformio/penv/bin/pio

# The password is read from ~/.netrc and never printed: the entry for the module's
# address is copied under the relay's host name into a private file for this run.
umask 077
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
NETRC="$WORK/netrc"
sed -n "s/^machine ${LOCAL//./\\.} /machine ${RELAY#https://} /p" ~/.netrc > "$NETRC"
if [ ! -s "$NETRC" ]; then
  echo "No entry for $LOCAL in ~/.netrc, so the module's password is not known here."
  exit 1
fi

r() { curl -s --digest --netrc-file "$NETRC" "$@"; }
stamp() { date +%H:%M:%S; }

status() {
  r --max-time 15 "$RELAY/api/status" | python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
except Exception:
    print("offline"); sys.exit()
print(d["system"]["version"], d["printing"], d["uptime"], d["block"], d["slot"])'
}

if [ "$1" = "--check" ]; then
  read -r version printing up block slot <<< "$(status)"
  echo "$(stamp)  module runs $version, printing $printing, up ${up}s, slot $slot, largest free block $block"
  exit 0
fi

echo "$(stamp)  building"
"$PIO" run -e bare > "$WORK/build.log" 2>&1 || { tail -20 "$WORK/build.log"; exit 1; }
cp .pio/build/bare/firmware.bin "$WORK/firmware.bin"
WANT=$(git rev-parse --short=7 HEAD)
git status --porcelain --untracked-files=no | grep -q . && WANT="$WANT-dirty"
echo "$(stamp)  image $(stat -f %z "$WORK/firmware.bin") bytes, version $WANT"

attempt=0
while :; do
  attempt=$((attempt + 1))
  echo
  echo "$(stamp)  attempt $attempt"

  # Wait for the module, and never flash in the middle of a print
  until read -r version printing up block slot <<< "$(status)" && [ "$version" != offline ]; do
    echo "$(stamp)    module not reachable, waiting"
    sleep 20
  done
  if [ "$version" = "$WANT" ]; then
    echo "$(stamp)  done: the module already runs $WANT"
    exit 0
  fi
  if [ "$printing" = True ]; then
    echo "$(stamp)    a print is running, waiting for it to finish"
    sleep 120
    attempt=$((attempt - 1))
    continue
  fi

  # A flash goes best on fresh memory, so the module is restarted first
  echo "$(stamp)    running $version, largest free block $block; restarting it for fresh memory"
  r -o /dev/null --max-time 15 -X POST "$RELAY/restart"
  sleep 15
  until read -r version printing up block slot <<< "$(status)" && [ "$version" != offline ]; do
    sleep 5
  done
  sleep 20
  read -r version printing up block slot <<< "$(status)"
  echo "$(stamp)    back, largest free block $block; sending the image"

  answer=$(r -H "Expect:" --max-time 1500 -o "$WORK/answer" -w "%{http_code} %{time_total}" \
             -F "firmware=@$WORK/firmware.bin" "$RELAY/update")
  code=${answer%% *}
  took=${answer##* }
  said=$(tr '\n' ' ' < "$WORK/answer" | sed 's/<[^>]*>//g' | cut -c1-160)
  echo "$(stamp)    answer $code after ${took%.*}s: $said"

  if [ "$code" = 200 ]; then
    echo "$(stamp)    flashed, waiting for the module to come back on the new build"
    sleep 30
    for i in $(seq 1 30); do
      read -r version printing up block slot <<< "$(status)"
      if [ "$version" = "$WANT" ]; then
        sleep 70          # BootGuard keeps an image only after a minute online
        guard=$(r --max-time 15 "$RELAY/api/status" | python3 -c 'import sys,json;print(json.load(sys.stdin)["guard"])' 2>/dev/null)
        echo "$(stamp)  done: the module runs $WANT in $slot ($guard)"
        exit 0
      fi
      sleep 10
    done
    echo "$(stamp)    it said flashed but did not come back on $WANT; trying again"
  fi

  echo "$(stamp)    did not take, next try in 30 s"
  sleep 30
done
