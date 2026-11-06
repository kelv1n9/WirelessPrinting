Import("env")
import subprocess

def git(*args):
    try:
        return subprocess.check_output(("git",) + args, stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return ""

sha = git("rev-parse", "--short=7", "HEAD") or "unknown"
if git("status", "--porcelain", "--untracked-files=no"):
    sha += "-dirty"

env.Append(CPPDEFINES=[("SKETCH_VERSION", env.StringifyMacro(sha))])


import gzip, os, re

def pack_page():
    here = env.subst("$PROJECT_DIR")
    source = os.path.join(here, "bare", "page.h")
    target = os.path.join(here, "bare", "page_gz.h")
    text = open(source, encoding="utf-8").read()
    html = re.search(r'R"PAGE\((.*)\)PAGE";', text, re.S).group(1).encode("utf-8")
    packed = gzip.compress(html, 9, mtime=0)
    rows = [", ".join(str(b) for b in packed[i:i + 24]) for i in range(0, len(packed), 24)]
    out = ("#pragma once\n\n#include <Arduino.h>\n\n"
           "static const uint8_t PAGE_GZ[] PROGMEM = {\n  " + ",\n  ".join(rows) + "\n};\n")
    if not os.path.exists(target) or open(target, encoding="utf-8").read() != out:
        open(target, "w", encoding="utf-8").write(out)

pack_page()
