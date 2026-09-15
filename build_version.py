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
