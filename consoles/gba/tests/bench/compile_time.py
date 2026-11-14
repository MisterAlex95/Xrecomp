#!/usr/bin/env python3
"""Compile-time benchmark of the GBA recompiler's C (XBA-151).

    compile_time.py ROM [--cc "CC ARGS"] [--opt N] [--jobs N] [--work DIR] [--cflags "..."]

Lifts ROM (gba_tools.recomp, --no-comments) into WORK/gen, then compiles every file with Prepare's GBA flags
(tools/prepare/gba.py compile_flags: -O<opt> -fno-tree-slp-vectorize -g0 -fPIC -std=gnu11 -w -DNDEBUG) in JOBS
parallel processes, and prints one JSON line: instructions, files, CPU seconds (the sum of every compile's user +
system time, from wait4), wall seconds, CPU ms per instruction, the slowest file. With --cc the NDK's clang for
aarch64 (e.g. --cc "$NDK/bin/clang --target=aarch64-linux-android30 --sysroot=$NDK/sysroot") measures what a phone's
clang does, natively; docs/android/prepare.md « GBA packs » turns that into a phone estimate. ROM: build/bench's
synthetic one (build_rom.sh) or any of our homebrew; never a game in the repository.
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, REPO)
from tools.prepare import gba  # noqa: E402  (the flags Prepare uses)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rom")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--opt", default="1")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--work", default=os.path.join(REPO, "build", "bench", "compile_time"))
    ap.add_argument("--cflags", default="", help="extra flags")
    a = ap.parse_args()
    gen = os.path.join(a.work, "gen")
    env = dict(os.environ)
    env["PYTHONPATH"] = os.path.join(REPO, "consoles", "gba", "tools") + os.pathsep + env.get("PYTHONPATH", "")
    t0 = time.time()
    r = subprocess.run([sys.executable, "-m", "gba_tools.recomp", a.rom, gen, "--no-comments"], env=env,
                       capture_output=True, text=True, check=True)
    lift_s = time.time() - t0
    summary = json.loads(r.stdout.strip().splitlines()[-1])
    cc = shlex.split(a.cc)
    flags = gba.compile_flags(a.opt, REPO, gen) + shlex.split(a.cflags)
    files = sorted((f for f in os.listdir(gen) if f.endswith(".c")),
                   key=lambda f: -os.path.getsize(os.path.join(gen, f)))

    def one(f):
        p = subprocess.Popen(cc + flags + ["-c", os.path.join(gen, f), "-o", os.devnull])
        _, status, ru = os.wait4(p.pid, 0)
        p.returncode = os.waitstatus_to_exitcode(status)
        if p.returncode != 0:
            raise RuntimeError("%s failed" % f)
        return f, ru.ru_utime + ru.ru_stime

    t0 = time.time()
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        times = list(ex.map(one, files))
    wall = time.time() - t0
    cpu = sum(t for _, t in times)
    slow = max(times, key=lambda x: x[1])
    n = summary["instructions"]
    print(json.dumps({"instructions": n, "functions": summary["functions"], "files": len(files),
                      "lift_s": round(lift_s, 1), "cpu_s": round(cpu, 1), "wall_s": round(wall, 1),
                      "jobs": a.jobs, "ms_per_insn": round(1000 * cpu / max(1, n), 3),
                      "slowest": [slow[0], round(slow[1], 2)], "opt": a.opt, "cc": a.cc}))


if __name__ == "__main__":
    main()
