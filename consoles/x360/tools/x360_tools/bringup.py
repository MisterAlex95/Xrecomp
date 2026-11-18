"""One command from a title's XEX to a recompiled run (XBA-270), for a dump of your own:

    python3 -m x360_tools.bringup DEFAULT.XEX WORKDIR [--key-file KEY] [--disc IMAGE|DIR] [--x360-run PATH]
                                  [--rounds N] [--insns N] [--cc CC] [-j N]

1. parse the XEX (with your key: none is in the repository) and write it plain (decrypted, expanded) to
   WORKDIR/plain.xex, the file x360_run loads;
2. the imports the runtime's HLE covers, and the ones it does not (by name);
3. the decoder / lifter coverage of the code (unknown words, not-lifted mnemonics, helpers, jump tables);
4. recompile, build the pack (WORKDIR/libtitle.so, -DXE_RUNTIME), run it under x360_run, and when execution reaches
   code the pack lacks, recompile with those addresses and run again (up to --rounds);
5. a summary with no title data in it (counts, export names, the runtime's last message), also written to
   WORKDIR/summary.txt: what to paste in the ticket.

What the title printed through DbgPrint stays in WORKDIR/dbg.txt (not in the summary: it is the title's text).
"""

import argparse
import collections
import glob
import json
import os
import shutil
import subprocess
import sys
import time

from . import CONSOLE  # noqa: F401
from tools.xex.xex import COMPRESSION_NAMES, XexError, load_keys, parse_xex, plain_xex

from .hle_coverage import coverage
from .recomp import REPO, RUNTIME_DIRS, recompile, sweep, sweep_shapes


def find_x360_run(explicit):
    if explicit:
        return explicit
    for pattern in ("build/**/x360_run", "build*/**/x360_run"):
        for p in sorted(glob.glob(os.path.join(REPO, pattern), recursive=True)):
            if os.access(p, os.X_OK) and os.path.isfile(p):
                return p
    return shutil.which("x360_run")


def build_pack(src, out, cc, jobs):
    """Compile every C file of src into a shared library (parallel objects, then one link)."""
    inc = []
    for d in RUNTIME_DIRS:
        inc += ["-I", d]
    flags = ["-O1", "-fPIC", "-ffp-contract=off", "-DXE_RUNTIME=1", "-w"]
    files = sorted(glob.glob(os.path.join(src, "*.c")))
    objs, procs = [], []
    for f in files:
        o = f[:-2] + ".o"
        objs.append(o)
        procs.append(subprocess.Popen([cc, "-c"] + flags + inc + [f, "-o", o]))
        if len(procs) >= jobs:
            if procs.pop(0).wait():
                raise RuntimeError("compiling the pack failed")
    for p in procs:
        if p.wait():
            raise RuntimeError("compiling the pack failed")
    subprocess.run([cc, "-shared", "-o", out] + objs + ["-lm"], check=True)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python3 -m x360_tools.bringup")
    ap.add_argument("xex")
    ap.add_argument("work")
    ap.add_argument("--key-file", action="append", default=[])
    ap.add_argument("--disc", help="the title's disc: an image or a directory of its files")
    ap.add_argument("--x360-run", help="the x360_run binary (default: looked for under build/)")
    ap.add_argument("--rounds", type=int, default=4)
    ap.add_argument("--insns", type=int, default=2000000000)
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("-j", type=int, default=3)
    a = ap.parse_args(argv)
    os.makedirs(a.work, exist_ok=True)
    lines = []

    def say(s):
        print(s)
        lines.append(s)

    t0 = time.time()
    try:
        with open(a.xex, "rb") as f:
            raw = f.read()
        img = parse_xex(raw, load_keys(a.key_file))
    except (OSError, XexError) as ex:
        print("error: %s" % ex, file=sys.stderr)
        return 1
    say("xex: image %d bytes, %s, compression %s, %d .pdata functions, %d import libraries" % (
        img.image_size, "encrypted" if img.encryption else "not encrypted",
        COMPRESSION_NAMES.get(img.compression, img.compression), len(img.pdata), len(img.libraries)))
    if img.image is None:
        say("stop: the image is not readable: %s" % img.image_note)
        return 1
    plain = os.path.join(a.work, "plain.xex")
    with open(plain, "wb") as f:
        f.write(plain_xex(raw, img))

    rows = coverage(img)
    missing = [r for r in rows if not r[4]]
    by_lib = collections.Counter(r[0] for r in rows)
    say("imports: %d (%s), covered by the HLE: %d, missing: %d" % (
        len(rows), ", ".join("%s %d" % kv for kv in by_lib.items()), len(rows) - len(missing), len(missing)))
    for lib, o, n, kind, _ in missing:
        say("  missing %s %d %s (%s)" % (lib, o, n, kind))

    total, unknown, unlifted = sweep(img)
    kinds, jt_ok, jt_bad = sweep_shapes(img)
    say("sweep: %d instruction words, %d unknown (%.2f %%), %d decoded but not lifted" % (
        total, sum(unknown.values()), 100.0 * sum(unknown.values()) / max(total, 1), sum(unlifted.values())))
    if unlifted:
        say("  not lifted: %s" % ", ".join("%s %d" % kv for kv in unlifted.most_common(15)))
    say("  helpers: %s; bctr: %d tables resolved, %d not" % (
        ", ".join("%s %d" % kv for kv in sorted(kinds.items())) or "none", jt_ok, jt_bad))

    run = find_x360_run(a.x360_run)
    if not run:
        say("stop: no x360_run (build consoles/x360, or pass --x360-run)")
        return 1
    discovered = set()
    disc_file = os.path.join(a.work, "discovered.txt")
    outcome = "not run"
    for rnd in range(1, a.rounds + 1):
        src = os.path.join(a.work, "src")
        shutil.rmtree(src, ignore_errors=True)
        t = time.time()
        s = recompile(img, src, extra_seeds=discovered, comments=False)
        rt = time.time() - t
        t = time.time()
        pack = os.path.join(a.work, "libtitle.so")
        build_pack(src, pack, a.cc, a.j)
        bt = time.time() - t
        say("round %d: %d functions, %d instructions, %d to the interpreter, %d jump tables, %d bctr unresolved; "
            "recompiled in %.1f s, pack built in %.1f s" % (rnd, s["functions"], s["instructions"],
                                                           s["interp_fallback_insns"], s["jump_tables"],
                                                           s["unresolved_bctr"], rt, bt))
        missed = os.path.join(a.work, "missed.txt")
        cmd = [run, plain, "--pack", pack, "--insns", str(a.insns), "--stats", "--missed", missed,
               "--dbg", os.path.join(a.work, "dbg.txt"), "--log", os.path.join(a.work, "log.txt")]
        if a.disc:
            cmd += ["--disc", a.disc]
        try:
            rc = subprocess.run(cmd, timeout=3600).returncode
        except subprocess.TimeoutExpired:
            rc = -1
        log = open(os.path.join(a.work, "log.txt")).read().splitlines() if os.path.exists(
            os.path.join(a.work, "log.txt")) else []
        last = [x for x in log if x.startswith("x360_run:")]
        stats = [x for x in log if x.startswith("stats:")]
        unimpl = sorted({x.split(" ")[1] for x in log if "has no HLE" in x and x.startswith("x360: ")})
        outcome = last[-1] if last else "x360_run exit %d" % rc
        say("  run: %s" % outcome)
        if stats:
            say("  %s" % stats[-1])
        if unimpl:
            say("  called without an HLE: %s" % ", ".join(unimpl))
        new = set()
        if os.path.exists(missed):
            for line in open(missed):
                line = line.split("#")[0].strip()
                if line:
                    new.add(int(line, 16))
        new -= discovered
        if not new:
            break
        discovered |= new
        with open(disc_file, "w") as f:
            f.write("".join("%08X\n" % v for v in sorted(discovered)))
        say("  %d address(es) reached without lifted code: recompiling with them" % len(new))
    dbg = os.path.join(a.work, "dbg.txt")
    say("DbgPrint: %d lines (in %s, not copied here)" % (
        sum(1 for _ in open(dbg, errors="replace")) if os.path.exists(dbg) else 0, dbg))
    say("total %.1f s" % (time.time() - t0))
    with open(os.path.join(a.work, "summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(a.work, "summary.json"), "w") as f:
        json.dump({"missing_imports": [list(r[:4]) for r in missing], "outcome": outcome,
                   "discovered": len(discovered)}, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
