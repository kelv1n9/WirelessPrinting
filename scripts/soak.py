#!/usr/bin/env python3
"""Держит модуль под веб-нагрузкой и следит, не перезагрузился ли он.

    scripts/soak.py --minutes 10 --threads 3 --print cold_cube.gcode

Запросы идут по кругу в несколько потоков, раз в 0.3..1 с на поток. Раз в секунду
снимается /api/status: uptime ловит перезагрузку, block и heap показывают кучу,
прогресс печати показывает, жива ли подача. В конце печатается сводка.

Печать запускается только если её попросили ключом --print, и только если принтер
свободен. Без ключа стенд гоняет одну веб-нагрузку.
"""

import os, argparse, json, random, statistics, sys, threading, time, urllib.error, urllib.request

# The module wants a key now. Put it in KP3S_KEY, it is on the module's front page.
# Installed as the default opener so every urlopen below carries it unchanged.
_key = os.environ.get("KP3S_KEY", "")
if _key:
    _opener = urllib.request.build_opener()
    _opener.addheaders = [("X-Api-Key", _key)]
    urllib.request.install_opener(_opener)

ROUTES = [
    "/api/status", "/api/job", "/api/printer", "/api/files", "/api/console?since=0",
    "/api/connection", "/api/version", "/api/diag", "/info", "/log", "/api/settings",
]

stop = threading.Event()
lock = threading.Lock()
latencies = []
failures = {}


def fetch(host, route, timeout=30):
    started = time.monotonic()
    try:
        with urllib.request.urlopen(host + route, timeout=timeout) as answer:
            answer.read()
            return time.monotonic() - started, answer.status
    except urllib.error.HTTPError as trouble:
        return time.monotonic() - started, trouble.code
    except Exception as trouble:                      # noqa: BLE001 - any failure is a data point
        return time.monotonic() - started, type(trouble).__name__


def worker(host, routes):
    while not stop.is_set():
        route = random.choice(routes)
        spent, code = fetch(host, route)
        with lock:
            latencies.append(spent)
            if code != 200:
                failures[f"{route} {code}"] = failures.get(f"{route} {code}", 0) + 1
        stop.wait(random.uniform(0.3, 1.0))


def post(host, route, timeout=15):
    request = urllib.request.Request(host + route, data=b"", method="POST")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as answer:
            return answer.status, answer.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as trouble:
        return trouble.code, trouble.read().decode("utf-8", "replace")
    except Exception as trouble:                      # noqa: BLE001
        return None, str(trouble)


def status(host, timeout=10):
    try:
        with urllib.request.urlopen(host + "/api/status", timeout=timeout) as answer:
            return json.loads(answer.read())
    except Exception:                                 # noqa: BLE001
        return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="http://192.168.1.83")
    parser.add_argument("--minutes", type=float, default=10)
    parser.add_argument("--threads", type=int, default=3)
    parser.add_argument("--routes", type=int, default=len(ROUTES))
    parser.add_argument("--print", dest="gcode", default=None,
                        help="файл на карте, который запустить на печать на время прогона")
    parser.add_argument("--label", default="run")
    parser.add_argument("--stop-on-reboot", dest="stopOnReboot", action="store_true",
                        help="прекратить на первой перезагрузке, чтобы успеть прочитать свидетеля")
    args = parser.parse_args()

    before = status(args.host)
    if before is None:
        print("модуль не отвечает", file=sys.stderr)
        return 2
    if args.gcode and before["printing"]:
        print("уже печатает, не трогаю", file=sys.stderr)
        return 2

    post(args.host, "/api/beep?on=0")                 # Прогон не должен пищать

    if args.gcode:
        code, body = post(args.host, "/select?name=" + args.gcode)
        if code not in (200, 204):
            print(f"не выбрался файл: {code} {body}", file=sys.stderr)
            return 2
        request = urllib.request.Request(
            args.host + "/api/job", data=json.dumps({"command": "start"}).encode(),
            headers={"Content-Type": "application/json"}, method="POST")
        try:
            urllib.request.urlopen(request, timeout=15).read()
        except Exception as trouble:                  # noqa: BLE001
            print(f"печать не стартовала: {trouble}", file=sys.stderr)
            return 2
        time.sleep(3)

    routes = ROUTES[:max(1, min(args.routes, len(ROUTES)))]
    threads = [threading.Thread(target=worker, args=(args.host, routes), daemon=True)
               for _ in range(args.threads)]
    for one in threads:
        one.start()

    started = time.monotonic()
    deadline = started + args.minutes * 60
    lastUptime, reboots, samples = None, [], []
    blockLow, heapLow, lastPos = 10 ** 9, 10 ** 9, 0
    missed = 0

    print(f"[{args.label}] {args.threads} потока, {len(routes)} маршрутов, {args.minutes} мин")
    while time.monotonic() < deadline:
        time.sleep(1)
        now = status(args.host)
        if now is None:
            missed += 1
            continue
        system = now["system"]
        uptime = system["uptime"]
        if lastUptime is not None and uptime < lastUptime:
            reboots.append((round(time.monotonic() - started), system.get("bootReason", "?")))
            print(f"  !! ПЕРЕЗАГРУЗКА на {reboots[-1][0]} с, причина: {reboots[-1][1]}")
            if args.stopOnReboot:
                break
        lastUptime = uptime
        blockLow = min(blockLow, system["block"])
        heapLow = min(heapLow, system["heap"])
        lastPos = now["job"]["position"]
        samples.append((system["block"], system["heap"]))
        if len(samples) % 30 == 0:
            spent = round(time.monotonic() - started)
            print(f"  {spent:4d} с  block {system['block']:6d}  heap {system['heap']:6d}  "
                  f"поз {lastPos:8d}  завершено {now['job']['completion']:5.1f}%  "
                  f"энергия {now['energy']['wh']:.1f} Wh")

    stop.set()
    for one in threads:
        one.join(timeout=5)

    after = status(args.host)
    if args.gcode and after and after["printing"]:
        request = urllib.request.Request(
            args.host + "/api/job", data=json.dumps({"command": "cancel"}).encode(),
            headers={"Content-Type": "application/json"}, method="POST")
        try:
            urllib.request.urlopen(request, timeout=15).read()
        except Exception:                             # noqa: BLE001
            pass

    ordered = sorted(latencies)
    print(f"\n[{args.label}] итог")
    print(f"  запросов        {len(latencies)}")
    if ordered:
        print(f"  задержка        медиана {statistics.median(ordered):.3f} с, "
              f"p95 {ordered[int(len(ordered) * 0.95)]:.3f} с, максимум {ordered[-1]:.3f} с")
        print(f"  дольше 2 с      {sum(1 for one in ordered if one > 2)}")
        print(f"  дольше 4 с      {sum(1 for one in ordered if one > 4)}")
    print(f"  не 200          {sum(failures.values())} {failures if failures else ''}")
    print(f"  /api/status нет {missed}")
    print(f"  block минимум   {blockLow}")
    print(f"  heap минимум    {heapLow}")
    print(f"  позиция в файле {lastPos}")
    if after:
        print(f"  энергия         {after['energy']['wh']:.2f} Wh, ватт {after['energy']['watt']}")
    print(f"  ПЕРЕЗАГРУЗОК    {len(reboots)} {reboots if reboots else ''}")

    return 1 if reboots else 0


if __name__ == "__main__":
    sys.exit(main())
