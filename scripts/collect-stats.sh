#!/bin/zsh
# Забирает журнал модуля и дописывает его в архив, из которого считается статистика.
# Модуль питается от принтера, поэтому розетка поднимается на время чтения и
# возвращается в то положение, в котором была.
#
#   scripts/collect-stats.sh [--verbose]
#
# Если принтер уже работает или печатает, розетка не трогается.

set -u
# Системный резолвер ждёт обычный DNS пять секунд на каждое имя .local, поэтому
# имя разбирается один раз, а дальше идёт адрес.
M=${KP3S:-}
if [ -z "$M" ]; then
  IP=$(ping -c 1 -W 2000 kp3s.local 2>/dev/null | sed -n '1s/.*(\([0-9.]*\)).*/\1/p')
  M=${IP:+http://$IP}
  M=${M:-http://kp3s.local}
fi
ARCHIVE=${KP3S_ARCHIVE:-$HOME/.kp3s-stats}
V=0
[ "${1:-}" = "--verbose" ] && V=1
say() { [ $V = 1 ] && echo "$@"; }

mkdir -p $ARCHIVE
TMP=$(mktemp -d)
WAS=$(~/bin/printer-power status 2>/dev/null | awk '{print $1}')
cleanup() {
  rm -rf $TMP
  if [ "${WAS:-}" = "off" ] && [ "${TURNED:-}" = "1" ]; then
    if curl -s --max-time 5 "$M/api/status" 2>/dev/null | grep -q '"printing":true'; then
      echo "печать началась, розетку оставляю включённой" >&2
    else
      say "возвращаю розетку в выключенное положение"
      ~/bin/printer-power off >/dev/null 2>&1
    fi
  fi
}
trap cleanup EXIT

say "розетка была: $WAS"
if [ "$WAS" = "off" ]; then
  ~/bin/printer-power on >/dev/null 2>&1 || { echo "ОШИБКА: розетка не включилась" >&2; exit 1; }
  TURNED=1
  say "жду модуль"
  for i in $(seq 1 30); do
    sleep 3
    curl -s --max-time 5 "$M/api/status" >/dev/null 2>&1 && break
  done
fi

curl -s --max-time 5 "$M/api/status" >/dev/null 2>&1 || { echo "ОШИБКА: модуль не отвечает" >&2; exit 1; }

GOT=0
for NAME in events.1.log events.log; do     # старый файл первым, чтобы порядок строк не ломался
  CODE=$(curl -s --max-time 120 "$M/download?name=$NAME" -o $TMP/$NAME -w "%{http_code}")
  [ "$CODE" = 200 ] && { GOT=$((GOT+1)); say "$NAME: $(wc -c < $TMP/$NAME) байт"; } || say "$NAME: нет ($CODE)"
done
[ $GOT -gt 0 ] || { echo "ОШИБКА: ни одного файла журнала не забрано" >&2; exit 1; }

python3 -I - "$ARCHIVE/events.all.log" $TMP/events.1.log $TMP/events.log <<'PY'
import os, sys
target, sources = sys.argv[1], sys.argv[2:]
seen, kept = set(), []
if os.path.exists(target):
    with open(target, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\r\n")
            if line and line not in seen:
                seen.add(line); kept.append(line)
before = len(kept)
for source in sources:
    if not os.path.exists(source):
        continue
    with open(source, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\r\n")
            if line and line not in seen:
                seen.add(line); kept.append(line)
with open(target, "w", encoding="utf-8") as handle:
    handle.write("\n".join(kept) + "\n")
print("архив: %d строк, новых %d" % (len(kept), len(kept) - before))
PY
