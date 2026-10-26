#!/usr/bin/env python3
"""Считает статистику принтера по журналу модуля.

  python3 scripts/printer_stats.py [--archive КАТАЛОГ] [--json] [--verbose]

Журнал модуля ротируется на 256 КБ, поэтому собранные строки складываются в
архив и берутся уже оттуда. По умолчанию архив лежит в ~/.kp3s-stats.
"""
import argparse, json, os, re, sys
from datetime import datetime, timedelta, timezone

ARCHIVE = os.path.expanduser("~/.kp3s-stats")
LINES = "events.all.log"

START = re.compile(r"print start (?P<name>.+?) (?P<size>\d+) bytes, (?P<layers>\d+) layers")
END = re.compile(
    r"print end (?P<how>complete|cancelled|ended early) (?P<name>.+?) (?P<secs>\d+)s "
    r"layer (?P<layer>\d+)/(?P<layers>\d+) .*?"
    r"(?P<wh>[\d.]+)Wh (?P<g>[\d.]+)g (?P<azn>[\d.]+)AZN")
EXT_START = "the printer started a file from its own card"
EXT_END = "the print from the printer's own card ended"


def read(path):
    """Отдаёт (unix, текст) по строкам, пропуская те, что записаны до синхронизации часов."""
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            parts = line.rstrip("\r\n").split(" ", 2)
            if len(parts) < 3 or not parts[0].isdigit():
                continue
            yield int(parts[0]), parts[2]


def collect(path):
    prints, external, open_external = [], [], None
    for when, text in read(path):
        done = END.search(text)
        if done:
            prints.append({
                "at": when,
                "name": done["name"],
                "how": done["how"],
                "seconds": int(done["secs"]),
                "layer": int(done["layer"]),
                "layers": int(done["layers"]),
                "wh": float(done["wh"]),
                "grams": float(done["g"]),
                "azn": float(done["azn"]),
                "external": False,
            })
        elif text.startswith(EXT_START):
            open_external = when
        elif text.startswith(EXT_END) and open_external:
            external.append({
                "at": when,
                "name": "(printer's card)",
                "how": "complete",
                "seconds": when - open_external,
                "layer": 0, "layers": 0, "wh": 0.0, "grams": 0.0, "azn": 0.0,
                "external": True,
            })
            open_external = None
    return sorted(prints + external, key=lambda one: one["at"])


def summarise(jobs):
    done = [one for one in jobs if one["how"] == "complete"]
    seconds = sum(one["seconds"] for one in jobs)
    return {
        "prints": len(jobs),
        "completed": len(done),
        "cancelled": sum(1 for one in jobs if one["how"] != "complete"),
        "external": sum(1 for one in jobs if one["external"]),
        "hours": round(seconds / 3600, 1),
        "seconds": seconds,
        "grams": round(sum(one["grams"] for one in jobs), 1),
        "wh": round(sum(one["wh"] for one in jobs), 1),
        "azn": round(sum(one["azn"] for one in jobs), 2),
        "longest": max((one["seconds"] for one in jobs), default=0),
        "longest_name": max(jobs, key=lambda one: one["seconds"])["name"] if jobs else "",
    }


def main():
    ask = argparse.ArgumentParser(add_help=True)
    ask.add_argument("--archive", default=ARCHIVE)
    ask.add_argument("--json", action="store_true")
    ask.add_argument("--verbose", action="store_true")
    args = ask.parse_args()

    path = os.path.join(args.archive, LINES)
    if not os.path.exists(path):
        print("нет архива: %s\nсначала собери журнал: scripts/collect-stats.sh" % path, file=sys.stderr)
        return 1

    jobs = collect(path)
    now = datetime.now(timezone.utc)
    windows = {
        "all": [one for one in jobs],
        "month": [one for one in jobs if one["at"] >= (now - timedelta(days=30)).timestamp()],
        "week": [one for one in jobs if one["at"] >= (now - timedelta(days=7)).timestamp()],
    }
    out = {
        "generated": int(now.timestamp()),
        "periods": {key: summarise(value) for key, value in windows.items()},
        "jobs": [one for one in jobs[-60:]],
    }

    if args.json:
        print(json.dumps(out, ensure_ascii=False, indent=1))
        return 0

    for key, title in (("all", "за всё время"), ("month", "за месяц"), ("week", "за неделю")):
        s = out["periods"][key]
        print("%-14s %3d печат., %5.1f ч, %7.1f г, %7.1f Вт·ч, %6.2f AZN  (отменено %d)"
              % (title, s["prints"], s["hours"], s["grams"], s["wh"], s["azn"], s["cancelled"]))
    if args.verbose:
        print()
        for one in jobs[-20:]:
            print("%s  %-40s %6.1f ч  %s" % (
                datetime.fromtimestamp(one["at"]).strftime("%d %b %H:%M"),
                one["name"][:40], one["seconds"] / 3600, one["how"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
