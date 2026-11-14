#!/usr/bin/env python3
"""Builds a GBA title pack on a desktop and runs the discovery loop (XBA-150 / XBA-151 / XBA-152).

    tools/consoles/gba_recomp_build.py ROM WORKDIR [--discovered FILE] [--cc CC] [--jobs N] [--count]
                                       [--loop N --gba-run PATH [--frames F] [--run-args "ARGS"]]

1. `python3 -m gba_tools.recomp ROM WORKDIR/src [--discovered WORKDIR/discovered.txt]` (consoles/gba/tools)
2. every WORKDIR/src/*.c compiled with the host compiler (-O2 -fPIC -I consoles/gba/runtime), linked into
   WORKDIR/libtitle.so (on macOS with -undefined dynamic_lookup: the core's symbols come from the executable)
3. with --loop N: up to N rounds of  gba_run ROM --pack libtitle.so --missed WORKDIR/discovered.txt  then a rebuild
   with the addresses the interpreter had to run, until a round finds nothing new (converged).

--discovered FILE seeds WORKDIR/discovered.txt (merged). --count builds with -DGBA_RC_COUNT (native instruction
counter: gba_run --stats prints the native share). Writes WORKDIR/report.json (rounds, functions, instructions,
discovered addresses, native %). Exit 0 when the pack is built (and, with --loop, the loop converged), 1 otherwise.
"""

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOLS = os.path.join(REPO, "consoles", "gba", "tools")
RUNTIME = os.path.join(REPO, "consoles", "gba", "runtime")


def log(msg):
    print("gba_recomp_build: " + msg, flush=True)


def read_addrs(path):
    out = set()
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                s = line.split("#", 1)[0].strip()
                if s:
                    try:
                        out.add(int(s, 16))
                    except ValueError:
                        pass
    return out


def write_addrs(path, addrs):
    with open(path, "w", encoding="utf-8") as f:
        for a in sorted(addrs):
            f.write("0x%08X\n" % a)


def lift(rom, src, discovered):
    cmd = [sys.executable, "-m", "gba_tools.recomp", rom, src, "--no-comments"]
    if discovered and os.path.exists(discovered):
        cmd += ["--discovered", discovered]
    env = dict(os.environ)
    env["PYTHONPATH"] = TOOLS + os.pathsep + env.get("PYTHONPATH", "")
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, check=False)
    if r.returncode != 0:
        raise RuntimeError("gba_tools.recomp failed:\n" + r.stderr)
    return json.loads(r.stdout.strip().splitlines()[-1])


def build(src, lib, cc, jobs, cflags):
    cfiles = sorted(f for f in os.listdir(src) if f.endswith(".c"))
    objdir = os.path.join(src, "obj")
    shutil.rmtree(objdir, ignore_errors=True)
    os.makedirs(objdir)

    def compile_one(f):
        obj = os.path.join(objdir, f[:-2] + ".o")
        cmd = cc + ["-O2", "-fPIC", "-w", "-I", RUNTIME] + cflags + ["-c", os.path.join(src, f), "-o", obj]
        r = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if r.returncode != 0:
            raise RuntimeError("%s failed:\n%s" % (" ".join(cmd), r.stderr[-4000:]))
        return obj

    t0 = time.time()
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        objs = list(ex.map(compile_one, cfiles))
    link = cc + ["-shared", "-o", lib] + objs
    if sys.platform == "darwin":
        link += ["-undefined", "dynamic_lookup"]
    r = subprocess.run(link, capture_output=True, text=True, check=False)
    if r.returncode != 0:
        raise RuntimeError("link failed:\n" + r.stderr)
    return time.time() - t0


def run_once(gba_run, rom, lib, missed, frames, run_args):
    cmd = [gba_run, rom, "--quiet", "--pack", lib, "--missed", missed, "--stats", "--frames", str(frames)]
    cmd += run_args
    r = subprocess.run(cmd, capture_output=True, text=True, check=False, timeout=900)
    m = re.search(r"([0-9.]+)% native", r.stdout)
    return r.returncode, (float(m.group(1)) if m else None), r.stdout


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rom")
    ap.add_argument("workdir")
    ap.add_argument("--discovered", help="seed addresses (one hex per line, bit 0 = Thumb)")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("JOBS", "4")))
    ap.add_argument("--count", action="store_true", help="build with -DGBA_RC_COUNT")
    ap.add_argument("--loop", type=int, default=0, help="discovery rounds at most (needs --gba-run)")
    ap.add_argument("--gba-run", help="the gba_run executable")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--run-args", default="", help="more gba_run arguments, e.g. \"--result\"")
    args = ap.parse_args(argv)

    rom = os.path.abspath(args.rom)
    work = os.path.abspath(args.workdir)
    src = os.path.join(work, "src")
    lib = os.path.join(work, "libtitle.so")
    disc = os.path.join(work, "discovered.txt")
    os.makedirs(work, exist_ok=True)
    if args.discovered:
        write_addrs(disc, read_addrs(disc) | read_addrs(args.discovered))
    cc = shlex.split(args.cc)
    cflags = ["-DGBA_RC_COUNT"] if args.count else []
    report = {"rom": rom, "rounds": []}
    try:
        rounds = max(1, args.loop)
        converged = args.loop == 0
        for i in range(1, rounds + 1):
            before = read_addrs(disc)
            summary = lift(rom, src, disc)
            secs = build(src, lib, cc, args.jobs, cflags)
            rnd = {"round": i, "functions": summary["functions"], "instructions": summary["instructions"],
                   "table_entries": summary["table_entries"], "discovered_in": len(before),
                   "build_seconds": round(secs, 2)}
            log("round %d: %d functions, %d instructions lifted, %d discovered addresses in, built in %.1f s"
                % (i, summary["functions"], summary["instructions"], len(before), secs))
            if args.loop:
                if not args.gba_run:
                    ap.error("--loop needs --gba-run")
                rc, native, _ = run_once(args.gba_run, rom, lib, disc, args.frames, shlex.split(args.run_args))
                after = read_addrs(disc)
                rnd.update({"run_exit": rc, "native_percent": native, "missed_new": len(after - before)})
                log("round %d: run exit %d, %s native, %d new missed addresses"
                    % (i, rc, "n/a" if native is None else "%.2f%%" % native, len(after - before)))
                report["rounds"].append(rnd)
                if after == before:
                    converged = True
                    break
            else:
                report["rounds"].append(rnd)
        if args.loop and not converged:
            # the last round's new addresses are not lifted yet: one more build so the pack holds them
            summary = lift(rom, src, disc)
            build(src, lib, cc, args.jobs, cflags)
            log("not converged after %d rounds (pack rebuilt with every address found)" % rounds)
        report["converged"] = converged
        report["rounds_run"] = len(report["rounds"])
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as e:
        log(str(e))
        return 1
    with open(os.path.join(work, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1, sort_keys=True)
    log("%s -> %s%s" % (os.path.basename(rom), lib,
                         "" if not args.loop else (", converged after %d round(s)" % report["rounds_run"]
                                                   if converged else ", not converged")))
    return 0 if converged else 1


if __name__ == "__main__":
    sys.exit(main())
