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
import zlib

from tools.xex.xex import IMPORT_THUNK, XexError, load_keys, parse_xex

from .discover360 import XenonDiscovery, find_helpers
from .lift360 import TitleLifter
from .seh import seh_seeds
from .xenon import XenonDecoder, XenonLifter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
RUNTIME_DIRS = [os.path.join(REPO, "consoles", "x360", "runtime"), os.path.join(REPO, "consoles", "gc", "runtime")]


class CodeView:
    """The XEX image as discovery sees it: code sections minus the import thunks (16 bytes each) and minus the
    register save / restore helpers once they are known (`hide`): they are inlined, not functions."""

    def __init__(self, img):
        self.img = img
        self.thunks = {r.addr for r in img.imports if r.kind == IMPORT_THUNK}
        self.thunk_words = set()
        for t in self.thunks:
            self.thunk_words.update(range(t, t + 16, 4))
        self.hidden = set()

    def hide(self, helpers):
        for h, _ in helpers.values():
            self.hidden.update(range(h.start, h.end + 4, 4))

    def in_code(self, a):
        if not self.img.in_code(a):
            return False
        return (a & ~3) not in self.thunk_words and (a & ~3) not in self.hidden

    def u32(self, a):
        return self.img.u32(a)


BUCKET_BYTES = 0x10000  # guest code per generated file (XBA-283: ~1.2 MB of C; docs/android/prepare.md)
SPLIT_BYTES = 2000000   # C past which a bucket's file is cut again (dense code)


def fn_name(entry):
    return "xe_fn_%08X" % entry


def image_crc(img):
    """CRC-32 of the mapped image: the pack and the runtime's loaded XEX must agree (stale packs are refused)."""
    return zlib.crc32(img.image) & 0xFFFFFFFF


def recompile(img, outdir, extra_seeds=(), use_pdata=True, per_file=400, comments=True, omit=(),
              split_bytes=SPLIT_BYTES, bucket_bytes=BUCKET_BYTES):
    if img.image is None:
        raise XexError("no image to recompile: %s" % img.image_note)
    view = CodeView(img)
    dec = XenonDecoder()
    helpers = find_helpers(view, dec, img.code_ranges())
    view.hide(helpers)
    seeds = {img.entry} | set(extra_seeds)
    if use_pdata:
        seeds |= {start for start, _ in img.pdata}
        seeds |= seh_seeds(img)   # XBA-282: __except blocks, filters, __finally (scope tables only name them)
    seeds = {a for a in seeds if a not in view.hidden}
    disc = XenonDiscovery(view, dec, sorted(seeds))
    funcs = disc.run()
    if omit:  # XBA-283 tests: functions left out of the pack on purpose (the runtime interprets them)
        funcs = [f for f in funcs if f.entry not in omit]
    resume = {}
    os.makedirs(outdir, exist_ok=True)
    unlifted = collections.Counter()
    fallbacks = 0
    inlined = 0
    invalid = 0
    files = []
    head = '/* Lifted from an XEX2 by x360_tools.recomp (XBA-230). */\n#include "xenon_rt.h"\n\n'
    # XBA-283: one file per bucket_bytes of guest code (by function entry), cut again past split_bytes of C. One clang
    # job on a 6 MB file of the old 400-function split peaked near 1 GB for aarch64, over Prepare's 512 MB per job;
    # buckets by address keep every other file byte-identical when a Prepare adds functions, so its object cache
    # recompiles only the files that changed (and the table).
    buckets = collections.OrderedDict()
    for f in funcs:
        buckets.setdefault((f.entry - img.base) // bucket_bytes, []).append(f)
    for b, group in buckets.items():
        out, size, sub = [head], 0, 0
        for n, f in enumerate(group):
            lifter = TitleLifter(view, disc.insn, f, fn_name(f.entry), comments=comments, helpers=helpers,
                                 tables=disc.jt.tables)
            text = lifter.function()
            out.append(text)
            out.append("\n")
            size += len(text) + 1
            fallbacks += lifter.fallbacks
            inlined += lifter.inlined
            for a in lifter.leaders:
                resume.setdefault(a, f.entry)
            for a in f.addrs:
                i = disc.insn(a)
                if lifter.needs_interp(i):
                    unlifted[i.name or ".invalid"] += 1
                    if i.name is None:
                        invalid += 1
            if n + 1 == len(group) or size >= split_bytes or len(out) // 2 >= per_file:
                path = os.path.join(outdir, "x360_rc_%05d_%d.c" % (b, sub))
                with open(path, "w") as fh:
                    fh.write("".join(out))
                files.append(path)
                out, size, sub = [head], 0, sub + 1
    table = ['/* Entry -> lifted function (XBA-230). */\n#include "xenon_rt.h"\n\n']
    for f in funcs:
        table.append("void %s(xe_machine *m_);\n" % fn_name(f.entry))
    table.append("\nconst xe_rc_entry xe_rc_table[] = {\n")
    for f in funcs:
        table.append("    {0x%08Xu, %s},\n" % (f.entry, fn_name(f.entry)))
    table.append("};\nconst unsigned xe_rc_count = %du;\n" % len(funcs))
    # every resume point (block leader: entries, returns from calls, jump table targets) -> its function, sorted:
    # the runtime's dispatcher looks a PC up here (XBA-270); an entry always maps to its own function
    for f in funcs:
        resume[f.entry] = f.entry
    table.append("\nconst xe_rc_entry xe_rc_resume[] = {\n")
    for a in sorted(resume):
        table.append("    {0x%08Xu, %s},\n" % (a, fn_name(resume[a])))
    table.append("};\nconst unsigned xe_rc_resume_count = %du;\n" % len(resume))
    # XBA-283: the save / restore helpers' entries (inlined, not functions): interpreted code calls them, the
    # runtime must not report them as missed functions
    table.append("\nconst uint32_t xe_rc_helpers[] = {\n%s};\nconst unsigned xe_rc_helper_count = %du;\n" % (
        "".join("    0x%08Xu,\n" % a for a in sorted(helpers)) or "    0u,\n", len(helpers)))
    table.append("const uint32_t xe_rc_image_crc = 0x%08Xu;\nconst uint32_t xe_rc_image_base = 0x%08Xu;\n"
                 % (image_crc(img), img.base))
    path = os.path.join(outdir, "x360_rc_table.c")
    with open(path, "w") as fh:
        fh.write("".join(table))
    files.append(path)
    st = disc.stats()
    stats = {"functions": st["functions"], "instructions": st["instructions"],
             "lifted_instructions": st["lifted_instructions"], "interp_fallback_insns": fallbacks,
             "invalid_words": invalid, "unlifted": dict(unlifted.most_common()), "seeds": len(seeds),
             "pdata_functions": len(img.pdata), "files": [os.path.basename(p) for p in files],
             "helpers": sorted({"%s@%08X" % (h.kind, h.start) for h, _ in helpers.values()}),
             "helper_calls_inlined": inlined, "jump_tables": st["jump_tables"],
             "jump_table_targets": st["jump_table_targets"], "unresolved_bctr": st["unresolved_bctr"],
             "resume_points": len(resume)}
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


def sweep_shapes(img):
    """What --sweep says about MSVC's shapes: helpers found by kind, bctr resolved / not (linear, every bctr)."""
    from .discover360 import JumpTables
    from tools.ppc.isa import BCCTR
    dec = XenonDecoder()
    view = CodeView(img)
    helpers = find_helpers(view, dec, img.code_ranges())
    kinds = collections.Counter(h.kind for h in {id(h): h for h, _ in helpers.values()}.values())
    jt = JumpTables(view, lambda a: dec.decode(view.u32(a), a))
    ok = bad = 0
    for lo, hi in img.code_ranges():
        for a in range(lo, hi, 4):
            if view.in_code(a):
                i = dec.decode(view.u32(a), a)
                if i.kind == BCCTR and (i.rd & 0x14) == 0x14 and not i.lk:
                    if jt.resolve(a):
                        ok += 1
                    else:
                        bad += 1
    return kinds, ok, bad


def compile_check(files, cc="cc"):
    if not shutil.which(cc):
        raise RuntimeError("no C compiler (%s)" % cc)
    inc = []
    for d in RUNTIME_DIRS:
        inc += ["-I", d]
    for p in files:
        subprocess.run([cc, "-fsyntax-only", "-std=gnu11", "-Wall", "-Wno-unused-label"] + inc + [p], check=True)


def read_addrs(path):
    """Hex addresses, one per line, # comments (x360_run --missed, discovered.txt); a missing file is empty."""
    out = set()
    if os.path.exists(path):
        with open(path) as fh:
            for line in fh:
                line = line.split("#")[0].strip()
                if line:
                    out.add(int(line, 16))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python3 -m x360_tools.recomp")
    ap.add_argument("xex")
    ap.add_argument("outdir", nargs="?")
    ap.add_argument("--key-file", action="append", default=[])
    ap.add_argument("--no-pdata", action="store_true", help="seed from the entry point only")
    ap.add_argument("--compile", action="store_true", help="check every C file with the host cc")
    ap.add_argument("--sweep", action="store_true", help="decoder / lifter coverage over the code sections")
    ap.add_argument("--no-comments", action="store_true")
    ap.add_argument("--discovered", action="append", default=[],
                    help="more seeds: a file of hex addresses (x360_run --missed), one per line")
    ap.add_argument("--bucket-bytes", type=lambda v: int(v, 0), default=BUCKET_BYTES,
                    help="guest code per generated file (default %(default)s)")
    ap.add_argument("--split-bytes", type=int, default=SPLIT_BYTES, help="C past which a file is cut again")
    ap.add_argument("--omit", action="append", default=[],
                    help="tests: a file of hex function entries to leave out of the pack (or one hex address)")
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
        kinds, ok, bad = sweep_shapes(img)
        print("  helpers: %s; bctr: %d jump tables resolved, %d not (indirect calls / tables of other shapes)" % (
            ", ".join("%s %d" % kv for kv in sorted(kinds.items())) or "none", ok, bad))
    if a.outdir:
        seeds = set()
        for path in a.discovered:
            seeds |= read_addrs(path)
        omit = set()
        for o in a.omit:
            omit |= read_addrs(o) if os.path.exists(o) else {int(o, 16)}
        s = recompile(img, a.outdir, extra_seeds=seeds, use_pdata=not a.no_pdata, comments=not a.no_comments,
                      omit=omit, split_bytes=a.split_bytes,
                      bucket_bytes=a.bucket_bytes)
        print("recomp: %(functions)d functions, %(instructions)d instructions, %(interp_fallback_insns)d handed to "
              "RC_INTERP, %(invalid_words)d invalid words" % s)
        print("  not lifted: %s" % ", ".join("%s %d" % kv for kv in list(s["unlifted"].items())[:30]))
        print("  save / restore helpers: %d found, %d calls inlined; jump tables: %d (%d targets), %d bctr "
              "unresolved; %d resume points" % (len(s["helpers"]), s["helper_calls_inlined"], s["jump_tables"],
                                                s["jump_table_targets"], s["unresolved_bctr"], s["resume_points"]))
        if a.compile:
            compile_check([os.path.join(a.outdir, p) for p in s["files"]])
            print("  every file compiles (cc -fsyntax-only)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
