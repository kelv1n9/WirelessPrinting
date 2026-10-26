#!/bin/zsh
# Меряет три вещи по отдельности, чтобы медленную передачу было на кого свалить:
# карту, радио и полный путь файла через HTTP.
#
#   scripts/bench.sh [--mb N] [--runs N] [--verbose]
#
# Хост берётся из $KP3S, по умолчанию http://kp3s.local

set -u
M=${KP3S:-http://kp3s.local}
MB=2; RUNS=2; V=0
while [ $# -gt 0 ]; do
  case "$1" in
    --mb)      MB=$2; shift 2 ;;
    --runs)    RUNS=$2; shift 2 ;;
    --verbose) V=1; shift ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *)         echo "неизвестный ключ: $1" >&2; exit 2 ;;
  esac
done

TMP=$(mktemp -d); trap 'rm -rf $TMP' EXIT
say() { [ $V = 1 ] && echo "$@"; }
fail() { echo "ОШИБКА: $*" >&2; exit 1; }

ST=$(curl -s --max-time 8 "$M/api/status") || fail "модуль не ответил"
[ -z "$ST" ] && fail "модуль не ответил"
say "$ST"
echo "$ST" | grep -q '"printing":true' && fail "идёт печать, стенд не запускаю"
echo "$ST" | python3 -c '
import json,sys
d=json.load(sys.stdin)
s=d["system"]
print("модуль  %s, uptime %s с, heap %d КБ, причина старта %s"
      % (s["version"], s["uptime"], s["heap"]//1024, s.get("bootReason","?")))
' || fail "не разобрал ответ /api/status"

CARD=$(curl -s --max-time 180 "$M/bench/card?mb=4") || fail "/bench/card не ответил"
say "$CARD"
echo "$CARD" | python3 -c '
import re,sys
t=sys.stdin.read()
w,r=re.findall(r"([\d.]+) KB/s",t)[:2]
print("карта   %s КБ/с запись, %s КБ/с чтение" % (w,r))
' || { echo "карта   не померилась: $CARD" >&2; exit 1; }

AIR=$(curl -s --max-time 30 "$M/bench/air")
say "$AIR"
echo "$AIR" | head -1 | sed 's/^ours: /радио   /'

S=$(python3 -c 'import time;print(time.time())')
curl -s --max-time 300 "$M/bench/net?mb=4" -o /dev/null || fail "/bench/net не ответил"
E=$(python3 -c 'import time;print(time.time())')
python3 -c "print('сеть    %.1f КБ/с отдача мимо карты' % (4*1024/($E-$S)))"

head -c $((MB*1024*1024)) /dev/zero | tr '\0' 'G' > $TMP/bench.gcode
SZ=$(stat -f%z $TMP/bench.gcode)
SRC=$(shasum -a 256 $TMP/bench.gcode | awk '{print $1}')
BAD=0; UPS=(); DNS=()
for RUN in $(seq 1 $RUNS); do
  S=$(python3 -c 'import time;print(time.time())')
  CODE=$(curl -s --max-time 900 -F "file=@$TMP/bench.gcode" "$M/api/files/local" -o /dev/null -w "%{http_code}")
  P=$(python3 -c 'import time;print(time.time())')
  curl -s --max-time 900 "$M/download?name=bench.gcode" -o $TMP/back.tmp
  E=$(python3 -c 'import time;print(time.time())')
  curl -s -X POST --max-time 20 "$M/delete?name=bench.gcode" -o /dev/null
  GOT=$(shasum -a 256 $TMP/back.tmp 2>/dev/null | awk '{print $1}')
  [ "$CODE" = 201 ] || { echo "ОШИБКА: заливка вернула HTTP $CODE" >&2; BAD=1; }
  [ "$GOT" = "$SRC" ] || { echo "ОШИБКА: файл вернулся битым, прогон $RUN" >&2; BAD=1; }
  UPS+=($(python3 -c "print('%.1f' % ($SZ/1024/($P-$S)))"))
  DNS+=($(python3 -c "print('%.1f' % ($SZ/1024/($E-$P)))"))
  say "прогон $RUN: ${UPS[-1]} КБ/с туда, ${DNS[-1]} КБ/с обратно, HTTP $CODE"
done
python3 -c "
ups=[${(j:,:)UPS}]; dns=[${(j:,:)DNS}]
print('файл    %d МБ x%d: %.1f КБ/с туда, %.1f КБ/с обратно'
      % ($MB, $RUNS, sum(ups)/len(ups), sum(dns)/len(dns)))"

AFTER=$(curl -s --max-time 8 "$M/api/status" | python3 -c 'import json,sys;print(json.load(sys.stdin)["system"]["uptime"])' 2>/dev/null)
[ -n "$AFTER" ] || fail "модуль не ответил после стенда"
echo "$ST" | python3 -c "
import json,sys
before=json.load(sys.stdin)['system']['uptime']
print('итог    ' + ('перезагрузился во время стенда' if int($AFTER) < int(before) else 'не перезагружался'))"
[ $BAD = 0 ] || exit 1
