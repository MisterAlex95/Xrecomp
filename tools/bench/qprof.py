"""
Executed instructions and a flat profile of any program under qemu-user (XBA-277): the method of
`python -m tools.bench icount` (one instruction per translation block, qemu's exec log), for the
console runtimes and their packs, whose benchmarks are whole programs run for N frames.

    python -m tools.bench.qprof [--arch x86_64|aarch64] [-L ROOT] [--top N] [--save F.json] -- CMD ARGS...
    python -m tools.bench.qprof --load F.json [--top N]

Prints `rc R total N`, then (without --save) the symbols that ran the most instructions: the main
executable's (build it non-PIE, -fno-pie -no-pie, or the addresses are guessed) and those of every
shared object glibc loaded (LD_DEBUG=files gives their bases: a pack's libtitle.so included).
--save keeps the raw counts to resolve later with --load where binutils' nm can read the binaries
(the arm64 image runs qemu but has no nm). Count the frames you care about as the difference of two
runs (e.g. --frames 30 less --frames 10): start-up (ROM hash, boot, discovery of the pack) is in
both.

The console benchmarks (our own programs, never a game):
  GBA  consoles/gba/tests/bench/build_rom.sh OUT.gba --functions 300 --seed 1, then
       tools/consoles/gba_recomp_build.py OUT.gba WORK --loop 4 --gba-run gba_run --frames 120,
       then `gba_run OUT.gba --frames N --quiet --pack WORK/libtitle.so`
  PS1  consoles/ps1/tests/bench/build_exe.sh OUT.exe --functions 300 --seed 1, ps1_recomp_build.py,
       ps1_run likewise
  N64  consoles/n64/tests/bench/build_rom.sh OUT.z64, n64_recomp_build.py, n64_run likewise
arm64: the runtimes and packs built with NDK clang --target=aarch64-linux-gnu against an aarch64
glibc sysroot (-fuse-ld=lld; the pack builders take --cc), run with --arch aarch64 -L <that root>
in xba-arm64-deps. x86-64: xba-deps' build, --arch x86_64.
"""

import bisect
import collections
import json
import os
import re
import subprocess
import sys
import tempfile

TRACE = re.compile(rb"Trace \d+: 0x[0-9a-f]+ \[[0-9a-f]+/([0-9a-f]+)/")
LINK_MAP = re.compile(r"file=(\S+) \[0\];  generating link map")
BASE = re.compile(r"dynamic: 0x[0-9a-f]+\s+base: (0x[0-9a-f]+)")


def run(cmd, arch="x86_64", root=None):
    """{pc: count}, total, exit code, [(base, path)] of the shared objects."""
    logdir = tempfile.mkdtemp(prefix="qprof.")
    q = ["qemu-" + arch] + (["-L", root] if root else []) + \
        ["-one-insn-per-tb", "-d", "nochain,exec", "-D", "/dev/stdout",
         "-E", "LD_DEBUG=files", "-E", "LD_DEBUG_OUTPUT=" + os.path.join(logdir, "ld")] + cmd
    p = subprocess.Popen(q, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    pcs = collections.Counter()
    total = 0
    for line in p.stdout:
        if line.startswith(b"Trace"):
            m = TRACE.match(line)
            if m:
                pcs[int(m.group(1), 16)] += 1
                total += 1
    rc = p.wait()
    mods, cur = [], None
    for fn in sorted(os.listdir(logdir)):
        with open(os.path.join(logdir, fn), errors="replace") as f:
            for line in f:
                m = LINK_MAP.search(line)
                if m:
                    cur = m.group(1)
                m = BASE.search(line)
                if m and cur:
                    mods.append((int(m.group(1), 16), cur))
        os.unlink(os.path.join(logdir, fn))
    os.rmdir(logdir)
    return pcs, total, rc, mods


def symbols(path, base):
    out = subprocess.run(["nm", "-S", "--defined-only", path], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        f = line.split()
        if len(f) == 4 and f[2].lower() in ("t", "w"):
            s, z = int(f[0], 16), int(f[1], 16)
            syms.append((base + s, base + s + z, os.path.basename(path) + ":" + f[3]))
    return syms


def profile(pcs, cmd, mods, arch):
    """{symbol: count} ('?' for addresses no symbol covers)."""
    exe = cmd[0]
    hdr = subprocess.run(["readelf", "-h", exe], capture_output=True, text=True).stdout
    ebase = (0x555555554000 if arch == "x86_64" else 0x5500000000) if "DYN" in hdr else 0
    syms = symbols(exe, ebase)
    for base, path in mods:
        if os.path.exists(path):
            syms += symbols(path, base)
    syms.sort()
    starts = [s[0] for s in syms]
    agg = collections.Counter()
    for pc, n in pcs.items():
        i = bisect.bisect_right(starts, pc) - 1
        agg[syms[i][2] if i >= 0 and pc < syms[i][1] else "?"] += n
    return agg


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    opts = {"--arch": "x86_64", "-L": None, "--top": "40", "--save": None, "--load": None}
    while args and args[0] != "--":
        if args[0] not in opts or len(args) < 2:
            raise SystemExit(__doc__)
        opts[args[0]] = args[1]
        args = args[2:]
    cmd = args[1:]
    arch = opts["--arch"]
    if opts["--load"]:
        with open(opts["--load"]) as f:
            d = json.load(f)
        pcs = collections.Counter({int(k): v for k, v in d["pcs"].items()})
        total, rc, mods, cmd, arch = d["total"], d["rc"], [tuple(m) for m in d["mods"]], d["cmd"], d["arch"]
    elif cmd:
        pcs, total, rc, mods = run(cmd, arch, opts["-L"])
    else:
        raise SystemExit(__doc__)
    print("rc %d total %d" % (rc, total))
    if opts["--save"]:
        with open(opts["--save"], "w") as f:
            json.dump({"pcs": pcs, "total": total, "rc": rc, "mods": mods, "cmd": cmd, "arch": arch}, f)
        return 0
    for name, n in profile(pcs, cmd, mods, arch).most_common(int(opts["--top"])):
        print("%12d %5.1f%%  %s" % (n, 100.0 * n / max(1, total), name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
