"""ctest gc_relload (XBA-264): REL modules loaded at run time by our homebrew relload (homebrew/relload: a small
OSLink), recorded by the runtime, lifted by Prepare at their load address, run natively.

    python3 run_relload.py GC_RUN HOMEBREW_DIR WORKDIR

1. relload_moda.elf / relload_modb.elf (relocatable objects) become rel/a.rel and rel/b.rel against relload.elf's
   symbols (gc_tools.elf2rel), and go with relload.dol into a GCM (gc_tools.disc).
2. gc_run boots the disc interpreted, --discovered: the program links A at its buffer, runs it, unlinks it, links B
   at the same address, runs it every field; its report must hold the expected results, and the discovered file one
   REL record per module, both at that address.
3. gc_tools.recomp lifts the DOL and both modules (one address, two units), compiled with the host cc into a pack.
4. gc_run with the pack: the same report, no interpreter miss in the modules' code, and B's every-field calls run
   natively (the interpreted instruction count well under the interpreted run's).
Exits 77 when the corpus or a C compiler is missing.
"""

import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

from gc_tools import disc, elf2rel, rel  # noqa: E402
from gc_tools.dol import Image  # noqa: E402
from gc_tools.recomp import load_rels, read_discovered, read_rel_records, recompile  # noqa: E402

RUNTIME = os.path.join(HERE, "..", "runtime")
FRAMES = 240


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


# A cross build's gc_run runs under the emulator ctest uses (XBR_TEST_EMULATOR, comma separated); none when native.
EMULATOR = [w for w in os.environ.get("XBR_TEST_EMULATOR", "").split(",") if w]


def run(gc_run, args):
    r = subprocess.run(EMULATOR + [gc_run] + args, capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        fail("gc_run %s: exit %d\n%s\n%s" % (" ".join(args), r.returncode, r.stdout[-2000:], r.stderr[-2000:]))
    return r.stdout


def peek_words(out):
    words = []
    for line in out.splitlines():
        if line.startswith("peek "):
            words += [int(x, 16) for x in line[5:].split()]
        elif words and line and all(c in "0123456789ABCDEF " for c in line):
            words += [int(x, 16) for x in line.split()]
    return words


def stats(out):
    for line in out.splitlines():
        if line.startswith("stats:"):
            t = line.split()
            return int(t[2]), int(t[4])
    fail("no stats line")


def report(words):
    if len(words) < 10 or struct.pack(">II", words[0], words[1]) != b"XBARELD1":
        fail("no report: %s" % words[:4])
    s = lambda v: v - (1 << 32) if v & 0x80000000 else v  # noqa: E731
    keys = ("done", "base", "a", "b", "counter", "b_calls", "b_sum", "err")
    return {k: s(w) if k in ("a", "b", "b_sum", "err") else w for k, w in zip(keys, words[2:10])}


def check_report(r, what):
    want_sum = sum(3 * (i & 7) ** 2 + 1 + 20 for i in range(r["b_calls"]))
    if (r["done"], r["err"], r["a"], r["b"], r["counter"]) != (1, 0, 126, 168, 2) or r["b_sum"] != want_sum \
            or r["b_calls"] < FRAMES // 2:
        fail("%s: report %s (want done 1, err 0, A 126, B 168, counter 2, b_sum %d)" % (what, r, want_sum))


def main():
    gc_run, hb, work = sys.argv[1:4]
    dol_path = os.path.join(hb, "relload.dol")
    if not all(os.path.isfile(os.path.join(hb, f)) for f in ("relload.dol", "relload.elf", "relload_moda.elf",
                                                              "relload_modb.elf")):
        print("skipped: no relload in %s (tools/consoles/build_homebrew.sh gc)" % hb)
        return 77
    cc = shutil.which(os.environ.get("CC", "cc"))
    if not cc:
        print("skipped: no C compiler")
        return 77
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    with open(os.path.join(hb, "relload.elf"), "rb") as f:
        prog = elf2rel.Elf(f.read())
    syms = prog.symbol_addrs()
    rep_at = syms["report"]
    mods = {}
    for mid, name in ((1, "a"), (2, "b")):
        with open(os.path.join(hb, "relload_mod%s.elf" % name), "rb") as f:
            mods["rel/%s.rel" % name] = elf2rel.elf2rel(f.read(), syms, mid)
    with open(dol_path, "rb") as f:
        dol = f.read()
    g = disc.build_gcm(dol, sorted(mods.items()), game_id=b"GXRL", maker=b"01")
    iso = os.path.join(work, "relload.iso")
    with open(iso, "wb") as f:
        f.write(g.data)
    peek = ["--peek", "%08X:10" % rep_at]

    # 2. interpreted, recorded
    dpath = os.path.join(work, "discovered.txt")
    out = run(gc_run, [iso, "--frames", str(FRAMES), "--stats", "--discovered", dpath] + peek)
    r = report(peek_words(out))
    check_report(r, "interpreted")
    _native0, interp0 = stats(out)
    recs = read_rel_records(dpath)
    print("interpreted: report %s; records %s" % (r, recs))
    if sorted(x[0] for x in recs) != [1, 2] or any(x[1] != r["base"] for x in recs):
        fail("REL records %s, want modules 1 and 2 at %08X" % (recs, r["base"]))
    for (mid, base, bss, h, _co) in recs:
        lo = rel.Loaded(rel.Rel(mods["rel/%s.rel" % "ab"[mid - 1]]), base, bss).link()
        if lo.exec_hash() != h:
            fail("module %d: Prepare's relocation hashes %08X, the run saw %08X" % (mid, lo.exec_hash(), h))

    # 3. lifted
    rels, skipped = load_rels(recs, mods)
    if skipped or len(rels) != 2:
        fail("REL instances %d, skipped %s" % (len(rels), skipped))
    src = os.path.join(work, "src")
    s = recompile(Image(dol), src, read_discovered(dpath), False, rels)
    print("lifted: %d DOL functions, modules %s" % (s["functions"], [(x["rel"]["id"], x["functions"])
                                                                     for x in s["rels"]]))
    objs = []
    for fn in sorted(os.listdir(src)):
        if fn.endswith(".c"):
            o = os.path.join(src, fn[:-2] + ".o")
            objs.append(o)
            subprocess.run([cc, "-c", "-fPIC", "-O1", "-w", "-ffp-contract=off", "-frounding-math", "-DGC_RC_COUNT",
                            "-I", RUNTIME, "-o", o, os.path.join(src, fn)], check=True)
    pack = os.path.join(work, "libtitle.so")
    subprocess.run([cc, "-shared", "-o", pack] + objs + ["-lm"], check=True)

    # 4. with the pack
    missed = os.path.join(work, "missed.txt")
    out = run(gc_run, [iso, "--frames", str(FRAMES), "--stats", "--pack", pack, "--missed", missed] + peek)
    r2 = report(peek_words(out))
    check_report(r2, "recompiled")
    native, interp = stats(out)
    ranges = []
    for lo in rels:
        for k in lo.rel.exec_sections():
            ranges.append((lo.addrs[k], lo.addrs[k] + lo.rel.sections[k][1]))
    with open(missed) as f:
        miss = [int(x, 16) for x in f.read().splitlines() if x and x[0] != "#"]
    inmod = [a for a in miss if any(lo <= a < hi for lo, hi in ranges)]
    print("recompiled: report %s; native %d, interpreted %d (interpreted run: %d); %d misses, %d in the modules"
          % (r2, native, interp, interp0, len(miss), len(inmod)))
    if inmod:
        fail("interpreter misses in the modules' code: %s" % ["%08X" % a for a in inmod])
    if native <= 0 or interp * 2 > interp0:
        fail("native %d, interpreted %d of %d: the pack did not take over" % (native, interp, interp0))
    print("gc_relload: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
