#!/bin/zsh
# Показывает консоль принтера, пока не нажмёшь Ctrl-C. Температуры и "ok"
# выброшены, иначе в потоке не видно ничего кроме них.
#
#   scripts/console.sh [--verbose] [--save ФАЙЛ]
#
# --verbose печатает всё, включая температуры.

set -u
M=${KP3S:-http://kp3s.local}
V=0; SAVE=
while [ $# -gt 0 ]; do
  case "$1" in
    --verbose) V=1; shift ;;
    --save)    SAVE=$2; shift 2 ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *)         echo "неизвестный ключ: $1" >&2; exit 2 ;;
  esac
done
[ -n "$SAVE" ] && : > "$SAVE"

SEQ=$(curl -s --max-time 6 "$M/api/console?since=0" | python3 -c 'import json,sys;print(json.load(sys.stdin)["seq"])' 2>/dev/null)
[ -n "$SEQ" ] || { echo "ОШИБКА: модуль не ответил" >&2; exit 1; }
echo "слушаю $M с номера $SEQ, Ctrl-C чтобы бросить" >&2

while true; do
  R=$(curl -s --max-time 6 "$M/api/console?since=$SEQ" 2>/dev/null)
  if [ -z "$R" ]; then
    echo "$(date +%H:%M:%S) --- модуль не ответил ---"
    sleep 2; continue
  fi
  NEW=$(echo "$R" | V=$V python3 -c '
import json, sys, os, time
d = json.load(sys.stdin)
# Модуль помечает принятое "<", посланное ">". Шум надо искать уже без метки.
noise = ("X:", "echo:busy", "ok T:", "ok N", "T:")
for l in d["lines"]:
    body = l.lstrip("<>").strip()
    if os.environ["V"] != "1" and (body == "ok" or body.startswith(noise)):
        continue
    print(time.strftime("%H:%M:%S"), l, flush=True)
print(d["seq"], file=sys.stderr)
' 2>$TMPDIR/cseq.$$)
  [ -n "$NEW" ] && { echo "$NEW"; [ -n "$SAVE" ] && echo "$NEW" >> "$SAVE"; }
  S=$(cat $TMPDIR/cseq.$$ 2>/dev/null); [ -n "$S" ] && SEQ=$S
  rm -f $TMPDIR/cseq.$$
  sleep 2
done
