#!/bin/zsh
# Гоняет приём прошивки по тому же пути, которым идёт файл из Telegram, но
# с локального сервера. Нужен, чтобы чинить этот путь, не отправляя мегабайт
# руками каждый раз.
#
#   scripts/ota-test.sh [--bin ФАЙЛ] [--verbose]
#
# ВНИМАНИЕ: при успехе модуль прошьётся этим образом и перезагрузится.
# По умолчанию берётся .pio/build/esp32dev/firmware.bin, то есть то же, что
# залил бы pio run -t upload.

set -u
M=${KP3S:-http://kp3s.local}
BIN=.pio/build/esp32dev/firmware.bin
V=0
while [ $# -gt 0 ]; do
  case "$1" in
    --bin)     BIN=$2; shift 2 ;;
    --verbose) V=1; shift ;;
    -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
    *)         echo "неизвестный ключ: $1" >&2; exit 2 ;;
  esac
done
say() { [ $V = 1 ] && echo "$@"; }

[ -f "$BIN" ] || { echo "ОШИБКА: нет образа $BIN, собери сначала: pio run" >&2; exit 1; }
SZ=$(stat -f%z "$BIN")

ST=$(curl -s --max-time 8 "$M/api/status")
[ -n "$ST" ] || { echo "ОШИБКА: модуль не ответил" >&2; exit 1; }
echo "$ST" | grep -q '"printing":true' && { echo "ОШИБКА: идёт печать, не трогаю" >&2; exit 1; }
WAS=$(echo "$ST" | python3 -c 'import json,sys;d=json.load(sys.stdin)["system"];print(d["version"],d["uptime"])')

TMP=$(mktemp -d)
cleanup() { [ -n "${PID:-}" ] && kill $PID 2>/dev/null; rm -rf $TMP; }
trap cleanup EXIT

cp "$BIN" $TMP/fw.bin
openssl req -x509 -newkey rsa:2048 -nodes -keyout $TMP/key.pem -out $TMP/cert.pem \
  -days 2 -subj "/CN=ota-test" 2>/dev/null || { echo "ОШИБКА: openssl не выдал сертификат" >&2; exit 1; }

cat > $TMP/serve.py <<'PY'
import http.server, ssl, os, sys
ROOT = os.path.dirname(os.path.abspath(__file__))
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        body = open(os.path.join(ROOT, "fw.bin"), "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(os.path.join(ROOT, "cert.pem"), os.path.join(ROOT, "key.pem"))
s = http.server.ThreadingHTTPServer(("0.0.0.0", 8443), H)
s.socket = ctx.wrap_socket(s.socket, server_side=True)
s.serve_forever()
PY
python3 -I $TMP/serve.py & PID=$!
sleep 1
kill -0 $PID 2>/dev/null || { echo "ОШИБКА: сервер не поднялся, занят порт 8443?" >&2; exit 1; }

IP=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null)
[ -n "$IP" ] || { echo "ОШИБКА: не нашёл адрес мака в сети" >&2; exit 1; }
URL="https://$IP:8443/fw.bin"
say "раздаю $URL, образ $SZ байт"
echo "было    $WAS"

MARK=$(curl -s --max-time 8 "$M/log" | wc -c | tr -d ' ')
ANS=$(curl -s --max-time 15 "$M/bench/ota?url=$URL" -w " [HTTP %{http_code}]")
say "модуль ответил: $ANS"
echo "$ANS" | grep -q "202" || { echo "ОШИБКА: модуль отказался: $ANS" >&2; exit 1; }

echo -n "жду"
for i in $(seq 1 90); do
  sleep 2; echo -n "."
  LOG=$(curl -s --max-time 6 "$M/log" 2>/dev/null)
  echo "$LOG" | grep -q "ota: flashed\|ota: failed\|ota: that is not\|ota: refused" && break
done
echo
LOG=$(curl -s --max-time 8 "$M/log")
say "$LOG"
echo "$LOG" | grep "ota:" | tail -4
echo "$LOG" | grep -q "ota: flashed" \
  && { echo "итог    прошился, модуль перезагружается"; exit 0; } \
  || { echo "итог    НЕ прошился, строки ota: выше" >&2; exit 1; }
