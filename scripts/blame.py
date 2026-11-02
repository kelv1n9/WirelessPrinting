#!/usr/bin/env python3
"""Бьёт по одному маршруту за раз и говорит, какой из них роняет модуль.

    scripts/blame.py --seconds 60 --threads 3

Каждый маршрут получает свой отрезок времени. Перезагрузка внутри отрезка
записывается на этот маршрут. Между отрезками модуль ждут, пока он не поднимется,
чтобы падение не приписалось следующему.
"""

import argparse, json, sys, threading, time, urllib.error, urllib.request

ROUTES = [
    "/api/status", "/api/job", "/api/printer", "/api/files", "/api/console?since=0",
    "/api/diag", "/info", "/log", "/thumbnail?name=cold_cube.gcode",
    "/download?name=short.gcode",
]


def status(host, timeout=8):
    try:
        with urllib.request.urlopen(host + "/api/status", timeout=timeout) as answer:
            return json.loads(answer.read())
    except Exception:                                 # noqa: BLE001
        return None


def waitUp(host, patience=90):
    until = time.monotonic() + patience
    while time.monotonic() < until:
        now = status(host)
        if now is not None:
            return now
        time.sleep(2)
    return None


def hammer(host, route, stop, tally):
    while not stop.is_set():
        started = time.monotonic()
        try:
            with urllib.request.urlopen(host + route, timeout=30) as answer:
                answer.read()
                code = answer.status
        except urllib.error.HTTPError as trouble:
            code = trouble.code
        except Exception as trouble:                  # noqa: BLE001
            code = type(trouble).__name__
        tally.append((time.monotonic() - started, code))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="http://192.168.1.83")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--threads", type=int, default=3)
    parser.add_argument("--routes", nargs="*", default=ROUTES)
    args = parser.parse_args()

    verdicts = []
    for route in args.routes:
        before = waitUp(args.host)
        if before is None:
            print(f"{route}: модуль не поднялся, прекращаю", file=sys.stderr)
            break
        baseUptime = before["system"]["uptime"]

        stop, tally = threading.Event(), []
        threads = [threading.Thread(target=hammer, args=(args.host, route, stop, tally), daemon=True)
                   for _ in range(args.threads)]
        for one in threads:
            one.start()

        reboot, blockLow = None, 10 ** 9
        until = time.monotonic() + args.seconds
        lastUptime = baseUptime
        while time.monotonic() < until and reboot is None:
            time.sleep(1)
            now = status(args.host)
            if now is None:
                continue
            uptime = now["system"]["uptime"]
            blockLow = min(blockLow, now["system"]["block"])
            if uptime < lastUptime:
                reboot = round(args.seconds - (until - time.monotonic()))
            lastUptime = uptime

        stop.set()
        for one in threads:
            one.join(timeout=35)

        bad = [code for _, code in tally if code != 200]
        slow = max([spent for spent, _ in tally], default=0)
        mark = f"УПАЛ на {reboot} с" if reboot is not None else "выжил"
        print(f"{route:34s} {mark:16s} запросов {len(tally):5d}  не 200 {len(bad):4d}  "
              f"медленнейший {slow:6.2f} с  block минимум {blockLow}")
        if bad:
            kinds = {}
            for code in bad:
                kinds[code] = kinds.get(code, 0) + 1
            print(f"{'':34s} {kinds}")
        verdicts.append((route, reboot))

    print("\nроняют модуль:", [route for route, reboot in verdicts if reboot is not None] or "ни один")
    return 0


if __name__ == "__main__":
    sys.exit(main())
