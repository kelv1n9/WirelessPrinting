#!/usr/bin/env python3
"""Готовит prints.json для страницы статистики из архива журналов.

  python3 scripts/make_dashboard_json.py [ФАЙЛ] [--archive КАТАЛОГ]

Без имени файла пишет в prints.json рядом с архивом.
"""
import argparse, datetime, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from printer_stats import collect, ARCHIVE, LINES


def main():
    ask = argparse.ArgumentParser(add_help=True)
    ask.add_argument("target", nargs="?")
    ask.add_argument("--archive", default=ARCHIVE)
    args = ask.parse_args()

    source = os.path.join(args.archive, LINES)
    if not os.path.exists(source):
        print("нет архива: %s" % source, file=sys.stderr)
        return 1
    target = args.target or os.path.join(args.archive, "prints.json")

    rows = []
    for job in collect(source):
        when = datetime.datetime.fromtimestamp(job["at"])
        rows.append({
            "finished": when.strftime("%Y-%m-%d %H:%M"),
            "day": when.strftime("%Y-%m-%d"),
            "at": job["at"],
            "name": job["name"],
            "outcome": "завершена" if job["how"] == "complete"
                       else ("отменена" if job["how"] == "cancelled" else "прервана"),
            "hours": round(job["seconds"] / 3600, 2),
            "layers": job["layers"],
            "grams": round(job["grams"], 1),
            "wh": round(job["wh"], 1),
            "azn": round(job["azn"], 3),
            "source": "с карты принтера" if job["external"] else "через модуль",
        })

    with open(target, "w", encoding="utf-8") as handle:
        json.dump(rows, handle, ensure_ascii=False, indent=0)
    print("%s: %d печатей, %.1f ч" % (target, len(rows), sum(one["hours"] for one in rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
