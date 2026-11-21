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
invalid words reached, direct calls.

Direct calls (WP-16): a `bl` to a function of the pack calls its C function straight from the caller (xenon_rt.h
RC_CALL, lift360.py TitleLifter.direct_call) instead of leaving to the runtime's dispatcher, and a `bctrl` looks its
target up in the runtime's table from C (RC_CALL_IND), where the resume table maps the return point to the caller. Such a pack sets XE_RC_FEAT_DIRECT_CALLS and names its CRC xe_rc_image_crc2 (a
runtime made before refuses it). --no-direct-calls or X360_DIRECT_CALLS=0: every call through the dispatcher, the C
of before byte for byte.
"""

import argparse
import collections
import json
import os
import shutil
import subprocess
import sys

from . import CONSOLE  # noqa: F401  (puts the repository root on sys.path)
import struct
import zlib

from tools.xex.xex import IMPORT_THUNK, XexError, load_keys, parse_xex

from .discover360 import XenonDiscovery, find_helpers, pointer_seeds
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
        # img.in_code as (first, last) word addresses, and the words left out in one set: discovery and the lift ask
        # for every word they read (millions of calls), the sections' loop was a sixth of the lift's time
        end = img.base + len(img.image) if img.image is not None else 0
        self._ranges = [(lo, min(hi, end) - 4) for lo, hi in img.code_ranges()] if img.image is not None else []
        self._out = set(self.thunk_words)
        self._image, self._base = img.image, img.base

    def hide(self, helpers):
        for h, _ in helpers.values():
            self.hidden.update(range(h.start, h.end + 4, 4))
        self._out = self.thunk_words | self.hidden

    def in_code(self, a):
        for lo, last in self._ranges:
            if lo <= a <= last:
                return (a & ~3) not in self._out
        return False

    def u32(self, a):
        return _BE32(self._image, a - self._base)[0]   # img.u32 without its two calls (as many as in_code)


_BE32 = struct.Struct(">I").unpack_from
BUCKET_BYTES = 0x10000  # guest code per generated file (XBA-283: ~1.2 MB of C; docs/android/prepare.md)
# C past which a bucket's file is cut again (dense code). 1.5 MB since the dead CR stores (xenon.py) made the C ~8 %
# bigger: one NDK clang -O1 job on a 1.8 MB file peaked at 502 MB, at the edge of Prepare's 512 MB per job. A function
# that would take a file past it starts the next file.
SPLIT_BYTES = 1500000


def fn_name(entry):
    return "xe_fn_%08X" % entry


def image_crc(img):
    """CRC-32 of the mapped image: the pack and the runtime's loaded XEX must agree (stale packs are refused)."""
    return zlib.crc32(img.image) & 0xFFFFFFFF


# XBA-284: past this many resume points the table is cut into parts, one file each (x360_rc_resume_NNNNN.c), by the
# guest address of the function a point belongs to (resume_part_bytes of code per part): one 463k-entry table took
# 389 MB in one clang job for a 12 MB title, over Prepare's 512 MB per job past ~20 MB of code; a part stays near
# 40k entries whatever the title's size, and adding a function changes its own part (and the small list of parts).
RESUME_SPLIT = 32768
RESUME_PART_BYTES = 0x100000


def write_resume_parts(outdir, resume, base, part_bytes, table):
    """Writes the resume parts and appends xe_rc_resume_parts (the runtime reads them instead of xe_rc_resume:
    x360_bind) to table. Each address is in exactly one part; parts are sorted inside, and by address window."""
    parts = collections.OrderedDict()
    for a in sorted(resume):
        parts.setdefault((resume[a] - base) // part_bytes, []).append(a)
    files = []
    for k in sorted(parts):
        addrs = parts[k]
        out = ['/* Resume points of the functions at image offset %d MB (XBA-284). */\n#include "xenon_rt.h"\n\n'
               % (k * part_bytes >> 20)]
        for e in sorted({resume[a] for a in addrs}):
            out.append("void %s(xe_machine *m_);\n" % fn_name(e))
        out.append("\nconst xe_rc_entry xe_rc_resume_%05d[] = {\n" % k)
        for a in addrs:
            out.append("    {0x%08Xu, %s},\n" % (a, fn_name(resume[a])))
        out.append("};\n")
        path = os.path.join(outdir, "x360_rc_resume_%05d.c" % k)
        with open(path, "w") as fh:
            fh.write("".join(out))
        files.append(path)
    table.append("\n")
    for k in sorted(parts):
        table.append("extern const xe_rc_entry xe_rc_resume_%05d[];\n" % k)
    table.append("const xe_rc_part xe_rc_resume_parts[] = {\n")
    for k in sorted(parts):
        table.append("    {xe_rc_resume_%05d, %du},\n" % (k, len(parts[k])))
    table.append("};\nconst unsigned xe_rc_resume_part_count = %du;\n" % len(parts))
    return files


def direct_calls_default():
    """WP-16: direct calls unless X360_DIRECT_CALLS=0 (Prepare runs this module with its own arguments: the switch is
    the environment's, for comparisons)."""
    return os.environ.get("X360_DIRECT_CALLS", "1") != "0"


def recompile(img, outdir, extra_seeds=(), use_pdata=True, per_file=400, comments=True, omit=(),
              split_bytes=SPLIT_BYTES, bucket_bytes=BUCKET_BYTES, resume_split=RESUME_SPLIT,
              resume_part_bytes=RESUME_PART_BYTES, direct_calls=None):
    if img.image is None:
        raise XexError("no image to recompile: %s" % img.image_note)
    view = CodeView(img)
    dec = XenonDecoder(text=comments)
    helpers = find_helpers(view, dec, img.code_ranges())
    view.hide(helpers)
    seeds = {img.entry} | set(extra_seeds)
    pointed = set()
    if use_pdata:
        seeds |= {start for start, _ in img.pdata}
        seeds |= seh_seeds(img)   # XBA-282: __except blocks, filters, __finally (scope tables only name them)
        # leaf functions outside .pdata that only a pointer reaches (vtables, callbacks)
        pointed = pointer_seeds(img, view, lambda a: dec.decode(view.u32(a), a)) - seeds
        seeds |= pointed
    seeds = {a for a in seeds if a not in view.hidden}
    disc = XenonDiscovery(view, dec, sorted(seeds))
    funcs = disc.run()
    if omit:  # XBA-283 tests: functions left out of the pack on purpose (the runtime interprets them)
        funcs = [f for f in funcs if f.entry not in omit]
    if direct_calls is None:
        direct_calls = direct_calls_default()
    direct = None
    leaders = {}
    if direct_calls:
        # WP-16: a `bl` calls its target's function from C when the target is a function of the pack and the resume
        # table will map the return point to the caller (lift360.py TitleLifter.direct_call): the table first, from
        # the leaders alone, built as below (an address several functions hold goes to the first, entries to their own)
        owner = {}
        for f in funcs:
            lead = TitleLifter(view, disc.insn, f, fn_name(f.entry), comments=False, helpers=helpers,
                               tables=disc.jt.tables).leaders
            for a in lead:
                owner.setdefault(a, f.entry)
            leaders[f.entry] = lead      # the lift below takes them as they are (find_leaders twice was a third)
            f.release()
        for f in funcs:
            owner[f.entry] = f.entry
        direct = ({f.entry for f in funcs}, owner)
    direct_rets = []
    indirect = 0
    resume = {}
    os.makedirs(outdir, exist_ok=True)
    for fn in os.listdir(outdir):  # an earlier lift's files (XBA-283 renamed them: no duplicate functions at link)
        if fn.startswith("x360_rc_") and fn.endswith(".c"):
            os.remove(os.path.join(outdir, fn))
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
    def flush(b, sub, out, callees):
        if callees:  # WP-16: the functions this file calls directly (xenon_rt.h XE_RC_LOCAL)
            out[0] = head + "".join("XE_RC_LOCAL void %s(xe_machine *m_);\n" % fn_name(e)
                                    for e in sorted(callees)) + "\n"
        path = os.path.join(outdir, "x360_rc_%05d_%d.c" % (b, sub))
        with open(path, "w") as fh:
            fh.write("".join(out))
        files.append(path)

    for b, group in buckets.items():
        out, size, sub, callees = [head], 0, 0, set()
        for n, f in enumerate(group):
            lifter = TitleLifter(view, disc.insn, f, fn_name(f.entry), comments=comments, helpers=helpers,
                                 tables=disc.jt.tables, direct=direct, leaders=leaders.pop(f.entry, None))
            text = lifter.function()
            if size and size + len(text) >= split_bytes:  # a big function starts a file of its own (clang's memory)
                flush(b, sub, out, callees)
                out, size, sub, callees = [head], 0, sub + 1, set()
            out.append(text)
            out.append("\n")
            size += len(text) + 1
            fallbacks += lifter.fallbacks
            inlined += lifter.inlined
            callees |= lifter.callees
            direct_rets += lifter.direct_rets
            indirect += lifter.indirect
            for a in lifter.leaders:
                resume.setdefault(a, f.entry)
            for a in f.packed:
                i = disc.insn(a)
                if lifter.needs_interp(i):
                    unlifted[i.name or ".invalid"] += 1
                    if i.name is None:
                        invalid += 1
            f.release()
            if n + 1 == len(group) or size >= split_bytes or len(out) // 2 >= per_file:
                flush(b, sub, out, callees)
                out, size, sub, callees = [head], 0, sub + 1, set()
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
    for ret, caller in direct_rets:  # WP-16: the table the leaders gave is the one the runtime gets (cannot differ)
        if resume.get(ret) != caller:
            raise RuntimeError("direct call returning to %08X: the resume table maps it to %s, not to %08X"
                               % (ret, resume.get(ret), caller))
    if len(resume) <= resume_split:
        table.append("\nconst xe_rc_entry xe_rc_resume[] = {\n")
        for a in sorted(resume):
            table.append("    {0x%08Xu, %s},\n" % (a, fn_name(resume[a])))
        table.append("};\n")
    else:
        files += write_resume_parts(outdir, resume, img.base, resume_part_bytes, table)
    table.append("const unsigned xe_rc_resume_count = %du;\n" % len(resume))
    # XBA-283: the save / restore helpers' entries (inlined, not functions): interpreted code calls them, the
    # runtime must not report them as missed functions
    table.append("\nconst uint32_t xe_rc_helpers[] = {\n%s};\nconst unsigned xe_rc_helper_count = %du;\n" % (
        "".join("    0x%08Xu,\n" % a for a in sorted(helpers)) or "    0u,\n", len(helpers)))
    # XBA-283: bit 0, lwarx / stwcx. through RC_LWARX / RC_STWCX (atomic: x360_run --host-threads needs it)
    # bit 1 (XE_RC_FEAT_VIEW4K), when the pack is built so (xenon_rt.h, XE_VIEW4K_DYNAMIC): its accesses follow the
    # 0xE0000000 view where the host's pages moved it
    # bit 2 (XE_RC_FEAT_DIRECT_CALLS, WP-16): the functions call each other from C (RC_CALL); such a pack names its CRC
    # xe_rc_image_crc2, which a runtime made before does not look for: it refuses the pack instead of running it
    table.append("const unsigned xe_rc_features = 1u | XE_RC_PACK_FEATURES%s;\n"
                 % (" | XE_RC_FEAT_DIRECT_CALLS" if direct else ""))
    table.append("const uint32_t xe_rc_image_crc%s = 0x%08Xu;\nconst uint32_t xe_rc_image_base = 0x%08Xu;\n"
                 % ("2" if direct else "", image_crc(img), img.base))
    path = os.path.join(outdir, "x360_rc_table.c")
    with open(path, "w") as fh:
        fh.write("".join(table))
    files.append(path)
    st = disc.stats()
    stats = {"functions": st["functions"], "instructions": st["instructions"],
             "lifted_instructions": st["lifted_instructions"], "interp_fallback_insns": fallbacks,
             "invalid_words": invalid, "unlifted": dict(unlifted.most_common()), "seeds": len(seeds),
             "pointer_seeds": len(pointed), "pdata_functions": len(img.pdata), "files": [os.path.basename(p) for p in files],
             "helpers": sorted({"%s@%08X" % (h.kind, h.start) for h, _ in helpers.values()}),
             "helper_calls_inlined": inlined, "jump_tables": st["jump_tables"],
             "jump_table_targets": st["jump_table_targets"], "unresolved_bctr": st["unresolved_bctr"],
             "resume_points": len(resume), "direct_calls": len(direct_rets) - indirect,
             "direct_indirect_calls": indirect}
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
    ap.add_argument("--resume-split", type=int, default=RESUME_SPLIT,
                    help="resume points past which the table is cut into parts (default %(default)s)")
    ap.add_argument("--resume-part-bytes", type=lambda v: int(v, 0), default=RESUME_PART_BYTES,
                    help="guest code per resume table part (default %(default)s)")
    ap.add_argument("--no-direct-calls", action="store_true",
                    help="every call leaves to the runtime's dispatcher (the shape before WP-16; also "
                         "X360_DIRECT_CALLS=0)")
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
                      omit=omit, split_bytes=a.split_bytes, bucket_bytes=a.bucket_bytes,
                      resume_split=a.resume_split, resume_part_bytes=a.resume_part_bytes,
                      direct_calls=False if a.no_direct_calls else None)
        print("recomp: %(functions)d functions, %(instructions)d instructions, %(interp_fallback_insns)d handed to "
              "RC_INTERP, %(invalid_words)d invalid words" % s)
        print("  not lifted: %s" % ", ".join("%s %d" % kv for kv in list(s["unlifted"].items())[:30]))
        print("  save / restore helpers: %d found, %d calls inlined; jump tables: %d (%d targets), %d bctr "
              "unresolved; %d resume points; %d direct calls (+%d bctrl)"
              % (len(s["helpers"]), s["helper_calls_inlined"], s["jump_tables"], s["jump_table_targets"],
                 s["unresolved_bctr"], s["resume_points"], s["direct_calls"], s["direct_indirect_calls"]))
        if a.compile:
            compile_check([os.path.join(a.outdir, p) for p in s["files"]])
            print("  every file compiles (cc -fsyntax-only)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
