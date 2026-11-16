"""The Xbox 360 static recompiler, spike (XBA-230): XEX2 -> discovered functions -> C.

    python3 -m x360_tools.recomp FILE.xex OUTDIR [--key-file KEY]... [--seeds-only-pdata] [--compile] [--sweep]

Parses the XEX (tools/xex; keys from the user, never from the repository), seeds discovery with the entry point
and every .pdata function start (MSVC's exact function table), follows calls to a fixpoint (tools/ppc/discover.py
with the Xenon decoder; import thunks are not code), and lifts each function with the Xenon lifter into
x360_rc_NNN.c (+ x360_rc_table.c: entry -> function). The C builds against consoles/x360/runtime/xenon_rt.h; --compile
runs the host cc on every file (-fsyntax-only), which is the check a title can get before there is a runtime.

--sweep decodes every word of the code sections (linear, no discovery) and prints a histogram of what the decoder
does not know and what the lifter does not lift: the coverage of the Xenon layer on a real title.

Stats (stats.json): functions, instructions lifted, instructions handed to RC_INTERP by mnemonic (`unlifted`),
invalid words reached.
"""

import argparse
import collections
import json
import os
import shutil
import subprocess
import sys

from . import CONSOLE  # noqa: F401  (puts the repository root on sys.path)
from tools.ppc.discover import Discovery
from tools.xex.xex import IMPORT_THUNK, XexError, load_keys, parse_xex

from .xenon import XenonDecoder, XenonLifter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
RUNTIME_DIRS = [os.path.join(REPO, "consoles", "x360", "runtime"), os.path.join(REPO, "consoles", "gc", "runtime")]


class CodeView:
    """The XEX image as discovery sees it: code sections minus the import thunks (16 bytes each)."""

    def __init__(self, img):
        self.img = img
        self.thunks = {r.addr for r in img.imports if r.kind == IMPORT_THUNK}

    def in_code(self, a):
        if not self.img.in_code(a):
            return False
        return not any(t <= a < t + 16 for t in self.thunks)

    def u32(self, a):
        return self.img.u32(a)


def fn_name(entry):
    return "xe_fn_%08X" % entry


def recompile(img, outdir, extra_seeds=(), use_pdata=True, per_file=400, comments=True):
    if img.image is None:
        raise XexError("no image to recompile: %s" % img.image_note)
    view = CodeView(img)
    seeds = {img.entry} | set(extra_seeds)
    if use_pdata:
        seeds |= {start for start, _ in img.pdata}
    dec = XenonDecoder()
    disc = Discovery(view, dec, sorted(seeds))
    funcs = disc.run()
    os.makedirs(outdir, exist_ok=True)
    unlifted = collections.Counter()
    fallbacks = 0
    invalid = 0
    files = []
    for k in range(0, len(funcs), per_file):
        chunk = funcs[k:k + per_file]
        out = ['/* Lifted from an XEX2 by x360_tools.recomp (XBA-230). */\n#include "xenon_rt.h"\n\n']
        for f in chunk:
            lifter = XenonLifter(view, disc.insn, f, fn_name(f.entry), comments=comments)
            out.append(lifter.function())
            out.append("\n")
            fallbacks += lifter.fallbacks
            for a in f.addrs:
                i = disc.insn(a)
                if lifter.needs_interp(i):
                    unlifted[i.name or ".invalid"] += 1
                    if i.name is None:
                        invalid += 1
        path = os.path.join(outdir, "x360_rc_%03d.c" % (k // per_file))
        with open(path, "w") as fh:
            fh.write("".join(out))
        files.append(path)
    table = ['/* Entry -> lifted function (XBA-230). */\n#include "xenon_rt.h"\n\n']
    for f in funcs:
        table.append("void %s(xe_machine *m_);\n" % fn_name(f.entry))
    table.append("\nconst xe_rc_entry xe_rc_table[] = {\n")
    for f in funcs:
        table.append("    {0x%08Xu, %s},\n" % (f.entry, fn_name(f.entry)))
    table.append("};\nconst unsigned xe_rc_count = %du;\n" % len(funcs))
    path = os.path.join(outdir, "x360_rc_table.c")
    with open(path, "w") as fh:
        fh.write("".join(table))
    files.append(path)
    st = disc.stats()
    stats = {"functions": st["functions"], "instructions": st["instructions"],
             "lifted_instructions": st["lifted_instructions"], "interp_fallback_insns": fallbacks,
             "invalid_words": invalid, "unlifted": dict(unlifted.most_common()), "seeds": len(seeds),
             "pdata_functions": len(img.pdata), "files": [os.path.basename(p) for p in files]}
    with open(os.path.join(outdir, "stats.json"), "w") as fh:
        json.dump(stats, fh, indent=1)
    return stats


def sweep(img):
    """Linear decode of the code sections: (instructions, Counter of unknown words by primary opcode, Counter of
    decoded-but-unlifted mnemonics)."""
    dec = XenonDecoder()
    view = CodeView(img)
    unknown, unlifted = collections.Counter(), collections.Counter()
    total = 0

    class _F:
        entry = 0
        addrs = set()

    lifter = XenonLifter(view, lambda a: dec.decode(view.u32(a), a), _F, "sweep")
    for lo, hi in img.code_ranges():
        for a in range(lo, hi, 4):
            if not view.in_code(a):
                continue
            w = view.u32(a)
            if w == 0:
                continue  # padding
            total += 1
            i = dec.decode(w, a)
            if i.name is None:
                unknown["op%d" % (w >> 26)] += 1
            elif lifter.needs_interp(i):
                unlifted[i.name] += 1
    return total, unknown, unlifted


def compile_check(files, cc="cc"):
    if not shutil.which(cc):
        raise RuntimeError("no C compiler (%s)" % cc)
    inc = []
    for d in RUNTIME_DIRS:
        inc += ["-I", d]
    for p in files:
        subprocess.run([cc, "-fsyntax-only", "-std=gnu11", "-Wall", "-Wno-unused-label"] + inc + [p], check=True)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python3 -m x360_tools.recomp")
    ap.add_argument("xex")
    ap.add_argument("outdir", nargs="?")
    ap.add_argument("--key-file", action="append", default=[])
    ap.add_argument("--no-pdata", action="store_true", help="seed from the entry point only")
    ap.add_argument("--compile", action="store_true", help="check every C file with the host cc")
    ap.add_argument("--sweep", action="store_true", help="decoder / lifter coverage over the code sections")
    ap.add_argument("--no-comments", action="store_true")
    a = ap.parse_args(argv)
    try:
        with open(a.xex, "rb") as f:
            img = parse_xex(f.read(), load_keys(a.key_file))
    except (OSError, XexError) as ex:
        print("error: %s" % ex, file=sys.stderr)
        return 1
    if img.image is None:
        print("error: %s" % img.image_note, file=sys.stderr)
        return 1
    if a.sweep:
        total, unknown, unlifted = sweep(img)
        nu, nl = sum(unknown.values()), sum(unlifted.values())
        print("sweep: %d instruction words, %d unknown (%.2f %%), %d decoded but not lifted (%.2f %%)" % (
            total, nu, 100.0 * nu / max(total, 1), nl, 100.0 * nl / max(total, 1)))
        print("  unknown by primary opcode: %s" % ", ".join("%s %d" % kv for kv in unknown.most_common(20)))
        print("  not lifted: %s" % ", ".join("%s %d" % kv for kv in unlifted.most_common(30)))
    if a.outdir:
        s = recompile(img, a.outdir, use_pdata=not a.no_pdata, comments=not a.no_comments)
        print("recomp: %(functions)d functions, %(instructions)d instructions, %(interp_fallback_insns)d handed to "
              "RC_INTERP, %(invalid_words)d invalid words" % s)
        print("  not lifted: %s" % ", ".join("%s %d" % kv for kv in list(s["unlifted"].items())[:30]))
        if a.compile:
            compile_check([os.path.join(a.outdir, p) for p in s["files"]])
            print("  every file compiles (cc -fsyntax-only)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
