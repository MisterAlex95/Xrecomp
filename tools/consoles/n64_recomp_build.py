#!/usr/bin/env python3
"""Builds an N64 title pack on a desktop and runs the discovery loop (XBA-169 / XBA-170 / XBA-171).

    tools/consoles/n64_recomp_build.py ROM WORKDIR [--discovered FILE] [--cc CC] [--opt -O1] [--jobs N] [--count]
                                       [--loop N --n64-run PATH [--frames F] [--run-args "ARGS"]] [--elf ELF]

ROM: an N64 ROM in any byte order (nothing else: no symbol or ELF file goes into the pack).
1. `python3 -m n64_tools.recomp ROM WORKDIR/src [--discovered WORKDIR/discovered.txt]` (consoles/n64/tools)
2. every WORKDIR/src/*.c compiled with the host compiler (the lifted-code flags of Prepare: -O1 by default, no SLP
   vectorizer; -fPIC -I consoles/n64/runtime), linked into WORKDIR/libtitle.so (on macOS with -undefined
   dynamic_lookup: the core's symbols come from the executable). Objects are kept in WORKDIR/obj, named by a hash of
   the file and the command, as Prepare's object cache: a round compiles only the files the lifter changed.
3. with --loop N: up to N rounds of  n64_run ROM --pack libtitle.so --missed WORKDIR/discovered.txt  then a rebuild
   with the code the interpreter had to run (bytes included: overlays, generated or TLB-mapped code), until a round
   records nothing new (converged).

--discovered FILE seeds WORKDIR/discovered.txt (appended). --count builds with -DN64_RC_COUNT (native instruction
counter: n64_run --stats prints the native share). --elf ELF (our homebrew keeps it) only MEASURES the discovery
after the loop (n64_tools.coverage: the share of the ELF's function symbols found), it is never an input. Writes
WORKDIR/report.json (rounds, functions, instructions, records, native %, compile time per 1000 instructions,
convergence, coverage). Exit 0 when the pack is built (and, with --loop, the loop converged), 1 otherwise.
"""

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOLS = os.path.join(REPO, "consoles", "n64", "tools")
RUNTIME = os.path.join(REPO, "consoles", "n64", "runtime")
sys.path.insert(0, REPO)
from tools.prepare import gba  # noqa: E402  (Prepare's flags for lifted code: gba.lifted_cflags, shared)


def log(msg):
    print("n64_recomp_build: " + msg, flush=True)


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
    cmd = [sys.executable, "-m", "n64_tools.recomp", inp, src, "--no-comments"]
    if discovered and os.path.exists(discovered):
        cmd += ["--discovered", discovered]
    env = dict(os.environ)
    env["PYTHONPATH"] = TOOLS + os.pathsep + env.get("PYTHONPATH", "")
    t0 = time.time()
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, check=False)
    if r.returncode != 0:
        raise RuntimeError("n64_tools.recomp failed:\n" + r.stderr)
    summary = json.loads(r.stdout.strip().splitlines()[-1])
    summary["lift_seconds"] = round(time.time() - t0, 2)
    return summary


def build(src, lib, cc, opt, jobs, cflags, objdir):
    """Compiles the files of src whose object is not in objdir yet (objects named by a hash of the file and the
    command: Prepare's object cache, so a discovery round recompiles only the files it changed), links lib. Returns
    (seconds, compiler seconds, files compiled, files)."""
    cfiles = sorted(f for f in os.listdir(src) if f.endswith(".c"))
    os.makedirs(objdir, exist_ok=True)
    cpu = {}

    def compile_one(f):
        path = os.path.join(src, f)
        cmd = cc + opt + ["-fPIC", "-w", "-I", RUNTIME] + cflags + ["-c", path, "-o"]
        with open(path, "rb") as fh:
            key = hashlib.sha256(fh.read() + "\0".join(cmd).encode()).hexdigest()[:20]
        obj = os.path.join(objdir, "%s-%s.o" % (f[:-2], key))
        if os.path.exists(obj):
            return obj
        t0 = time.time()
        r = subprocess.run(cmd + [obj + ".tmp"], capture_output=True, text=True, check=False)
        cpu[f] = time.time() - t0
        if r.returncode != 0:
            raise RuntimeError("%s failed:\n%s" % (" ".join(cmd), r.stderr[-4000:]))
        os.replace(obj + ".tmp", obj)
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
    for old in set(os.listdir(objdir)) - {os.path.basename(o) for o in objs}:
        os.remove(os.path.join(objdir, old))
    return time.time() - t0, sum(cpu.values()), len(cpu), len(cfiles)


def coverage(inp, elf, disc):
    cmd = [sys.executable, "-m", "n64_tools.coverage", inp, elf, "--json", disc + ".coverage.json"]
    if os.path.exists(disc):
        cmd += ["--discovered", disc]
    env = dict(os.environ)
    env["PYTHONPATH"] = TOOLS + os.pathsep + env.get("PYTHONPATH", "")
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, check=False)
    if r.returncode != 0:
        raise RuntimeError("n64_tools.coverage failed:\n" + r.stderr)
    with open(disc + ".coverage.json", encoding="utf-8") as f:
        rep = json.load(f)
    os.remove(disc + ".coverage.json")
    return rep


def run_once(n64_run, inp, lib, missed, frames, run_args):
    cmd = [n64_run, inp, "--quiet", "--pack", lib, "--missed", missed, "--stats", "--frames", str(frames)]
    cmd += run_args
    r = subprocess.run(cmd, capture_output=True, text=True, check=False, timeout=900)
    m = re.search(r"([0-9.]+)% native", r.stdout)
    return r.returncode, (float(m.group(1)) if m else None), r.stdout


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input")
    ap.add_argument("workdir")
    ap.add_argument("--discovered", help="seed records (n64_recomp.h discovered format)")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--opt", default="-O1", help="optimisation flag for the lifted C (default -O1)")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("JOBS", "4")))
    ap.add_argument("--count", action="store_true", help="build with -DN64_RC_COUNT")
    ap.add_argument("--loop", type=int, default=0, help="discovery rounds at most (needs --n64-run)")
    ap.add_argument("--n64-run", help="the n64_run executable")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--run-args", default="", help="more n64_run arguments, e.g. \"--result\"")
    ap.add_argument("--elf", help="measure the discovery against this ELF's function symbols (never an input)")
    args = ap.parse_args(argv)

    inp = os.path.abspath(args.input)
    work = os.path.abspath(args.workdir)
    src = os.path.join(work, "src")
    lib = os.path.join(work, "libtitle.so")
    disc = os.path.join(work, "discovered.txt")
    os.makedirs(work, exist_ok=True)
    if args.discovered and os.path.exists(args.discovered):
        with open(args.discovered, encoding="utf-8") as f, open(disc, "a", encoding="utf-8") as out:
            out.write(f.read())
    cc = shlex.split(args.cc)
    cflags = ["-DN64_RC_COUNT"] if args.count else []
    opt = gba.lifted_cflags(args.opt[2:] if args.opt.startswith("-O") else args.opt)
    objdir = os.path.join(work, "obj")
    report = {"input": inp, "opt": args.opt, "rounds": []}
    try:
        rounds = max(1, args.loop)
        converged = args.loop == 0
        if args.loop and not args.n64_run:
            ap.error("--loop needs --n64-run")
        for i in range(1, rounds + 1):
            before = records(disc)
            summary = lift(inp, src, disc)
            secs, cpu, compiled, nfiles = build(src, lib, cc, opt, args.jobs, cflags, objdir)
            insns = max(1, summary["instructions"])
            rnd = {"round": i, "functions": summary["functions"], "instructions": summary["instructions"],
                   "ram_functions": summary["ram_functions"], "records_in": len(before),
                   "lift_seconds": summary["lift_seconds"], "build_seconds": round(secs, 2),
                   "files": nfiles, "files_compiled": compiled}
            if compiled == nfiles:
                rnd["compile_cpu_seconds_per_1k_insns"] = round(cpu * 1000.0 / insns, 3)
            log("round %d: %d functions (%d from recorded RAM code), %d instructions, %d records in, lifted in %.1f s, "
                "built in %.1f s (%d of %d files compiled%s, %s)"
                % (i, summary["functions"], summary["ram_functions"], summary["instructions"], len(before),
                   summary["lift_seconds"], secs, compiled, nfiles,
                   ", %.2f s of compiler per 1000 instructions" % rnd["compile_cpu_seconds_per_1k_insns"]
                   if compiled == nfiles else "", " ".join(opt)))
            if args.loop:
                rc, native, _ = run_once(args.n64_run, inp, lib, disc, args.frames, shlex.split(args.run_args))
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
            build(src, lib, cc, opt, args.jobs, cflags, objdir)
            log("not converged after %d rounds (pack rebuilt with every record)" % rounds)
        report["converged"] = converged
        report["rounds_run"] = len(report["rounds"])
        if args.elf:
            cov = coverage(inp, args.elf, disc)
            report["coverage"] = {k: cov[k] for k in ("symbols", "found", "covered", "found_percent",
                                                     "covered_percent", "functions")}
            report["coverage"]["missed"] = [m["name"] for m in cov["missed"]]
            log("coverage: %d / %d function symbols found (%.2f%%), %.2f%% covered by lifted code"
                % (cov["found"], cov["symbols"], cov["found_percent"], cov["covered_percent"]))
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
