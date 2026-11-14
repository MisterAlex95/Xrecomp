#!/usr/bin/env python3
"""Builds a PS1 title pack on a desktop and runs the discovery loop (XBA-160 / XBA-161 / XBA-162).

    tools/consoles/ps1_recomp_build.py INPUT WORKDIR [--discovered FILE] [--cc CC] [--opt -O1] [--jobs N] [--count]
                                       [--loop N --ps1-run PATH [--frames F] [--run-args "ARGS"]]

INPUT: a PS-X EXE or a disc image (.cue / .bin / .iso).
1. `python3 -m ps1_tools.recomp INPUT WORKDIR/src [--discovered WORKDIR/discovered.txt]` (consoles/ps1/tools)
2. every WORKDIR/src/*.c compiled with the host compiler (-O1 by default, -fPIC -I consoles/ps1/runtime), linked into
   WORKDIR/libtitle.so (on macOS with -undefined dynamic_lookup: the core's symbols come from the executable)
3. with --loop N: up to N rounds of  ps1_run INPUT --pack libtitle.so --missed WORKDIR/discovered.txt  then a rebuild
   with the code the interpreter had to run (bytes included: overlays), until a round records nothing new.

--discovered FILE seeds WORKDIR/discovered.txt (appended). --count builds with -DPS1_RC_COUNT (native instruction
counter: ps1_run --stats prints the native share). Writes WORKDIR/report.json (rounds, functions, instructions,
records, native %, compile time per 1000 instructions). Exit 0 when the pack is built (and, with --loop, the loop
converged), 1 otherwise.
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
TOOLS = os.path.join(REPO, "consoles", "ps1", "tools")
RUNTIME = os.path.join(REPO, "consoles", "ps1", "runtime")


def log(msg):
    print("ps1_recomp_build: " + msg, flush=True)


def records(path):
    """The (address, hash) keys of a discovered file."""
    out = set()
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                s = line.split("#", 1)[0].split()
                if s:
                    out.add((s[0].upper(), s[1].upper() if len(s) > 1 else ""))
    return out


def lift(inp, src, discovered):
    cmd = [sys.executable, "-m", "ps1_tools.recomp", inp, src, "--no-comments"]
    if discovered and os.path.exists(discovered):
        cmd += ["--discovered", discovered]
    env = dict(os.environ)
    env["PYTHONPATH"] = TOOLS + os.pathsep + env.get("PYTHONPATH", "")
    t0 = time.time()
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, check=False)
    if r.returncode != 0:
        raise RuntimeError("ps1_tools.recomp failed:\n" + r.stderr)
    summary = json.loads(r.stdout.strip().splitlines()[-1])
    summary["lift_seconds"] = round(time.time() - t0, 2)
    return summary


def build(src, lib, cc, opt, jobs, cflags):
    cfiles = sorted(f for f in os.listdir(src) if f.endswith(".c"))
    objdir = os.path.join(src, "obj")
    shutil.rmtree(objdir, ignore_errors=True)
    os.makedirs(objdir)
    cpu = {}

    def compile_one(f):
        obj = os.path.join(objdir, f[:-2] + ".o")
        cmd = cc + [opt, "-fPIC", "-w", "-I", RUNTIME] + cflags + ["-c", os.path.join(src, f), "-o", obj]
        t0 = time.time()
        r = subprocess.run(cmd, capture_output=True, text=True, check=False)
        cpu[f] = time.time() - t0
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
    return time.time() - t0, sum(cpu.values())


def run_once(ps1_run, inp, lib, missed, frames, run_args):
    cmd = [ps1_run, inp, "--quiet", "--pack", lib, "--missed", missed, "--stats", "--frames", str(frames)]
    cmd += run_args
    r = subprocess.run(cmd, capture_output=True, text=True, check=False, timeout=900)
    m = re.search(r"([0-9.]+)% native", r.stdout)
    return r.returncode, (float(m.group(1)) if m else None), r.stdout


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input")
    ap.add_argument("workdir")
    ap.add_argument("--discovered", help="seed records (ps1_recomp.h discovered format)")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--opt", default="-O1", help="optimisation flag for the lifted C (default -O1)")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("JOBS", "4")))
    ap.add_argument("--count", action="store_true", help="build with -DPS1_RC_COUNT")
    ap.add_argument("--loop", type=int, default=0, help="discovery rounds at most (needs --ps1-run)")
    ap.add_argument("--ps1-run", help="the ps1_run executable")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--run-args", default="", help="more ps1_run arguments, e.g. \"--result --bios X\"")
    args = ap.parse_args(argv)

    inp = os.path.abspath(args.input)
    work = os.path.abspath(args.workdir)
    src = os.path.join(work, "src")
    lib = os.path.join(work, "libtitle.so")
    disc = os.path.join(work, "discovered.txt")
    os.makedirs(work, exist_ok=True)
    if args.discovered:
        with open(args.discovered, encoding="utf-8") as f, open(disc, "a", encoding="utf-8") as out:
            out.write(f.read())
    cc = shlex.split(args.cc)
    cflags = ["-DPS1_RC_COUNT"] if args.count else []
    report = {"input": inp, "opt": args.opt, "rounds": []}
    try:
        rounds = max(1, args.loop)
        converged = args.loop == 0
        if args.loop and not args.ps1_run:
            ap.error("--loop needs --ps1-run")
        for i in range(1, rounds + 1):
            before = records(disc)
            summary = lift(inp, src, disc)
            secs, cpu = build(src, lib, cc, args.opt, args.jobs, cflags)
            insns = max(1, summary["instructions"])
            rnd = {"round": i, "functions": summary["functions"], "instructions": summary["instructions"],
                   "ram_functions": summary["ram_functions"], "records_in": len(before),
                   "lift_seconds": summary["lift_seconds"], "build_seconds": round(secs, 2),
                   "compile_cpu_seconds_per_1k_insns": round(cpu * 1000.0 / insns, 3)}
            log("round %d: %d functions (%d from recorded RAM code), %d instructions, %d records in, lifted in %.1f s, "
                "built in %.1f s (%.2f s of compiler per 1000 instructions, %s)"
                % (i, summary["functions"], summary["ram_functions"], summary["instructions"], len(before),
                   summary["lift_seconds"], secs, rnd["compile_cpu_seconds_per_1k_insns"], args.opt))
            if args.loop:
                rc, native, _ = run_once(args.ps1_run, inp, lib, disc, args.frames, shlex.split(args.run_args))
                after = records(disc)
                rnd.update({"run_exit": rc, "native_percent": native, "records_new": len(after - before)})
                log("round %d: run exit %d, %s native, %d new records"
                    % (i, rc, "n/a" if native is None else "%.2f%%" % native, len(after - before)))
                report["rounds"].append(rnd)
                if after == before:
                    converged = True
                    break
            else:
                report["rounds"].append(rnd)
        if args.loop and not converged:
            # the last round's records are not lifted yet: one more build so the pack holds them
            lift(inp, src, disc)
            build(src, lib, cc, args.opt, args.jobs, cflags)
            log("not converged after %d rounds (pack rebuilt with every record)" % rounds)
        report["converged"] = converged
        report["rounds_run"] = len(report["rounds"])
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as e:
        log(str(e))
        return 1
    with open(os.path.join(work, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1, sort_keys=True)
    log("%s -> %s%s" % (os.path.basename(inp), lib,
                         "" if not args.loop else (", converged after %d round(s)" % report["rounds_run"]
                                                   if converged else ", not converged")))
    return 0 if converged else 1


if __name__ == "__main__":
    sys.exit(main())
