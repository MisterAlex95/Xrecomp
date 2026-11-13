"""
tools/seed_from_log/__main__.py

Feed function addresses a *run* discovered back into the seed file, so the next
codegen pass knows about them.

Static analysis cannot see every function. Two kinds are routinely invisible:

  * **Indirect-call targets.** Where a vtable call goes is a runtime fact.
    (`tools.rtti` recovers these statically when a title has RTTI; most do not.)
  * **Thread start routines.** The address handed to PsCreateSystemThreadEx is
    only ever pushed as an argument -- nothing calls it -- so the detector never
    finds it, it gets no dispatch entry, and the game thread silently never
    starts. The process then exits cleanly after two kernel calls, which reads
    like a successful run rather than zero progress. This is the single most
    common "it built and did nothing" cause.

Usage:

    py -3 -m tools.seed_from_log run.log game/title.xbe \
        --functions build/disasm/functions.json \
        --seeds     config/seed_functions.json

Then re-run disasm with --seed-functions and lift again. The log can also be
the runtime's discovery file (`discovered.jsonl` in the prepare's work folder,
src/kernel/recomp_discovery.h), which holds the same addresses without the noise.

An address the title mentioned is NOT automatically a function. A garbage slot
points at data just as easily, and seeding data splits real functions and breaks
the build far more thoroughly than the missing target did. Every candidate must
clear three gates:

  1. it lies in an executable section,
  2. the disassembler's own probe reads it as a function body, and
  3. it is not inside an instruction of a body the analysis established,
     unless the bytes there are a prologue (then the body is the one wrong).

The first two are needed together. The probe alone accepts data that happens to
decode -- a kernel thunk table is a run of code addresses and decodes happily --
and on Wreckless seeding two of its entries once took a boot that reached 34
assets down to 1. The third is Fable's (profile-free WP1): 12 of 14 garbage call targets
a corrupted run recorded decoded as bodies two bytes into a `mov`, and one of
them cut a real function in half.

A discovery file also says *when* a run met an address, and a run that went
wrong calls through garbage. classify_discovery() holds back what only a wild
call site or a tainted stretch of a run reported (the call-site and taint rules
in its docstring); gate_discovery() applies all of it.
"""

import argparse
import bisect
import json
import os
import re
import sys
from pathlib import Path

from tools.disasm import config
from tools.disasm.engine import DisasmEngine
from tools.disasm.loader import DATA_SECTION_NAMES, load_image

# Each pattern must have exactly one group: the hex address.
LOG_PATTERNS = [
    # Unresolved indirect call, from the RECOMP_ICALL dispatch failure path.
    (re.compile(r"Failed to resolve VA (0x[0-9A-Fa-f]+)"),
     "Indirect-call target observed at runtime"),
    # PsCreateSystemThreadEx handed a routine with no dispatch entry. Nothing
    # in the image calls this address, so only a run can reveal it.
    (re.compile(r"start routine (0x[0-9A-Fa-f]+) not found in dispatch"),
     "Thread start routine observed at runtime"),
    # PsCreateSystemThreadEx's StartContext1. The Xbox thread convention hands
    # the wrapper its real entry through the context pointer, so on several
    # titles this is the game's main and nothing calls it directly. It is not
    # always a function -- the gates below decide, which is what they are for.
    (re.compile(r"PsCreateSystemThreadEx.*?ctx1=(0x[0-9A-Fa-f]+)"),
     "PsCreateSystemThreadEx StartContext1 observed at runtime"),
    # Generic kernel-bridge complaint about an address it could not dispatch.
    (re.compile(r"not found in dispatch.*?(0x[0-9A-Fa-f]{6,8})"),
     "Address the kernel bridge could not dispatch"),
]


# The runtime's discovery file (src/kernel/recomp_discovery.h, XBA-64): one
# JSON object per line. Untranslated instructions ("unimpl") are not seeds:
# they go to the lifter coverage report.
DISCOVERY_KINDS = {
    "icall": "Indirect-call target observed at runtime",
    "thread": "Thread start routine observed at runtime",
    "thread_ctx": "PsCreateSystemThreadEx StartContext1 observed at runtime",
}

# Entries that say the run went wrong from there on (the taint rule). A
# `wild` entry is an indirect call to an address that is not code (a v2
# runtime writes it; an older one wrote such a target as an `icall`).
TAINT_KINDS = ("wild", "smc", "exception")


def _hex(v):
    try:
        return int(v, 16) if isinstance(v, str) else None
    except ValueError:
        return None


def _entries(text):
    """The file's entries in order: {"t", "va" (int, None on a run line),
    "ret" (int or None), "tainted", "v"}; torn and foreign lines skipped."""
    for line in text.splitlines():
        try:
            e = json.loads(line)
        except ValueError:
            continue
        if not isinstance(e, dict) or not isinstance(e.get("t"), str):
            continue
        va = _hex(e.get("va"))
        if e["t"] != "run" and va is None:
            continue
        yield {"t": e["t"], "va": va, "ret": _hex(e.get("ret")), "tainted": e.get("tainted") is True,
               "v": e.get("v") if isinstance(e.get("v"), int) else 1}


def scan_discovery(text):
    """{address: reason} from a discovery file; lines that are not whole JSON
    objects (a run killed mid-write) are skipped."""
    out = {}
    for e in _entries(text):
        if e["t"] in DISCOVERY_KINDS:
            out.setdefault(e["va"], DISCOVERY_KINDS[e["t"]])
    return out


def scan_log(text):
    """{address: reason} for every candidate the log mentions: a run's
    stdout+stderr, or its discovery file (one JSON object per line)."""
    if text.lstrip().startswith("{"):
        return scan_discovery(text)
    out = {}
    for pat, reason in LOG_PATTERNS:
        for m in pat.finditer(text):
            out.setdefault(int(m.group(1), 16), reason)
    return out


def classify_discovery(text, is_exec=None):
    """Which seed candidates of a discovery file a *healthy* stretch of a run
    reported (the Fable corruption, profile-free plan WP1).

    A run that went wrong calls through garbage: on Fable one call site
    (`ret 0x000CFA7A`) produced 34 targets, most 0xFFxxxxxx, and 14 of them
    happened to land in executable bytes that decode. Seeded, one of them cut
    a real function in half and the next build corrupted the guest stack. Two
    rules, applied per run segment (from one {"t":"run"} line to the next):

      * call site: an `icall` whose return address (`ret`, the call site)
        produced a target outside the executable sections in the same run
        (an `icall` to a non-executable address, or a `wild` entry) comes from
        a wild site and is never a seed;
      * taint: entries written after the first sign that the run went wrong
        (an indirect call to a non-executable address, an `smc` entry, an
        `exception` entry) are suspect. A runtime that knows (run line
        "v": 2, src/kernel/recomp_discovery.h) marks them "tainted": true;
        for every file the order of entries in the segment decides too.

    An address is clean when at least one of its entries is clean: what a
    tainted run reported becomes a seed only when a healthy run reports it
    too (a v2 runtime writes the entry again for it, untainted).

    A plain "the address occurs as a dword in the image" test does not
    discriminate (8 of Fable's 14 junk values occur by coincidence, one 841
    times), which is why the evidence is the run's own.

    `is_exec(va)`: in an executable section. None (no XBE at hand): every
    `icall` target is taken as code and only `wild`, `smc` and `exception`
    entries are signs -- what a v2 file says on its own.

    Returns {va: {"reason", "clean": bool, "clean_v2": clean in a segment a
    v2 runtime wrote, "why": what held it back or None}}."""
    exe = is_exec or (lambda va: True)
    segments, seg = [], []
    for e in _entries(text):
        if e["t"] == "run" and seg:
            segments.append(seg)
            seg = []
        seg.append(e)
    if seg:
        segments.append(seg)
    out = {}
    for seg in segments:
        wild_sites = {e["ret"] for e in seg
                      if e["ret"] is not None and (e["t"] == "wild" or (e["t"] == "icall" and not exe(e["va"])))}
        tainted = None          # the entry that tainted the segment
        v2 = seg[0]["t"] == "run" and seg[0]["v"] >= 2
        for e in seg:
            t = e["t"]
            if t == "run":
                continue
            if tainted is None and (t in TAINT_KINDS or (t == "icall" and not exe(e["va"]))):
                tainted = e
            if t not in DISCOVERY_KINDS:
                continue
            why = None
            if t == "icall" and e["ret"] in wild_sites:
                why = "from a wild call site (ret 0x%08X also called outside the code)" % e["ret"]
            elif e["tainted"]:
                why = "written after the run went wrong (marked tainted)"
            elif tainted is not None:
                why = "written after the run went wrong (%s 0x%08X)" % (tainted["t"], tainted["va"])
            r = out.setdefault(e["va"], {"reason": DISCOVERY_KINDS[t], "clean": False, "clean_v2": False,
                                         "why": why})
            if why is None:
                r["clean"], r["why"] = True, None
                r["clean_v2"] = r["clean_v2"] or v2
    return out


def _load(xbe, analysis_json):
    """load_image, parsing the XBE on the spot when no analysis JSON exists
    yet: a prepare in a fresh work folder re-gates the seeds it was handed
    (a copied discovered-seeds.json) before its own analysis has run."""
    if analysis_json and Path(str(analysis_json)).is_file():
        return load_image(str(xbe), str(analysis_json))
    if Path(os.path.splitext(str(xbe))[0] + "_analysis.json").is_file():
        return load_image(str(xbe), None)
    import contextlib
    import io
    import tempfile
    from tools.xbe_parser.xbe_parser import XBEParser, export_json
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "analysis.json")
        with contextlib.redirect_stdout(io.StringIO()):
            export_json(XBEParser(str(xbe)).parse(), p)
        return load_image(str(xbe), p)


class SeedChecker:
    """The static gates for one XBE and its analysis: executable section,
    reads as a function body, not inside an instruction of a body the
    analysis established (unless a prologue is there)."""

    def __init__(self, xbe, functions_path=None, analysis_json=None, untrusted=()):
        """`untrusted`: function starts whose bodies do not count as
        established -- the seeds earlier runs gave, which made their own
        bodies in functions.json (a junk seed's body must not vouch for
        anything). A cut body of a real function still counts: an
        instruction that starts in it is checked even when it runs past its
        end."""
        self.image = _load(xbe, analysis_json)
        # No linear sweep: the probes decode what they need on their own
        # (probes_as_function_body only uses the sweep as a cache), so the
        # verdicts are the same and a re-gate on every prepare stays cheap.
        self.engine = DisasmEngine(self.image)
        self.executable = [(s.virtual_addr, s.virtual_addr + s.virtual_size)
                           for s in self.image.sections
                           if s.executable and s.name not in DATA_SECTION_NAMES]
        self.bounds = []
        if functions_path and Path(functions_path).is_file():
            as_int = lambda v: int(v, 0) if isinstance(v, str) else v
            skip = set(untrusted)
            self.bounds = sorted((as_int(f["start"]), as_int(f["end"]), f["name"])
                                 for f in json.loads(Path(functions_path).read_text())
                                 if as_int(f["start"]) not in skip)
        self.starts = [b[0] for b in self.bounds]

    def is_exec(self, va):
        return va is not None and any(lo <= va < hi for lo, hi in self.executable)

    def _covering(self, start, end, va, max_insns=20000):
        """The instruction of the body [start, end) whose bytes hold `va` past
        its first byte, following the body's control flow from its start --
        not the linear sweep, which a data blob puts out of phase (Halo's XPP
        init sits in unclaimed bytes right after one, and is a real entry).
        None when `va` is a boundary or the walk never covers it."""
        sec = self.image.get_section_at_va(start)
        if sec is None:
            return None
        data = self.image.get_section_data(sec)
        cs = self.engine._cs
        work, seen = [start], set()
        while work and len(seen) < max_insns:
            a = work.pop()
            while start <= a < end and a not in seen:
                off = a - sec.virtual_addr
                if off < 0 or off >= len(data):
                    break
                insn = next(cs.disasm(data[off:off + 16], a, count=1), None)
                if insn is None:
                    break
                seen.add(a)
                if a < va < a + insn.size:
                    return insn
                m = insn.mnemonic
                if m in config.JMP_MNEMONICS or m in config.COND_JMP_MNEMONICS:
                    if insn.op_str.startswith("0x"):
                        try:
                            work.append(int(insn.op_str, 16))
                        except ValueError:
                            pass
                if m in config.RET_MNEMONICS or m in config.JMP_MNEMONICS or m in ("int3", "hlt", "ud2"):
                    break
                a += insn.size
        return None

    def inside_instruction(self, va):
        """(body name, instruction address) when `va` is inside an instruction
        of an established body, else None."""
        for start, end, name in self.bounds[:bisect.bisect_left(self.starts, va)]:
            if end < va:
                continue
            insn = self._covering(start, end, va)
            if insn is not None:
                return name, insn.address
        return None

    def verdict(self, va):
        """(True, "new function" / "alias inside sub_...") or (False, why)."""
        if not self.is_exec(va):
            return False, "not in an executable section"
        if not self.engine.probes_as_function_body(va):
            return False, "does not read as a function body"
        hit = self.inside_instruction(va)
        if hit and not self.engine.probes_as_prologue(va):
            return False, "inside the instruction at 0x%08X of %s (no prologue there)" % (hit[1], hit[0])
        i = bisect.bisect_right(self.starts, va) - 1
        inside = i >= 0 and self.bounds[i][0] < va < self.bounds[i][1]
        return True, ("alias inside " + self.bounds[i][2]) if inside else "new function"


def seed_entry(va, reason):
    return {"start": "0x%08X" % va, "observed": True, "note": reason + "; decodes as a function body."}


def gate_discovery(text, checker, only=None):
    """A discovery file through every gate: (accepted {va: (reason, note)},
    refused {va: why}, held {va: why}). Refused: a static gate said no (the
    same XBE and analysis say the same next time). Held: only a tainted
    stretch of a run or a wild call site reported it (a healthy run may
    still). `only`: the addresses to decide (None: all of them)."""
    accepted, refused, held = {}, {}, {}
    for va, c in sorted(classify_discovery(text, checker.is_exec).items()):
        if only is not None and va not in only:
            continue
        if not checker.is_exec(va):
            refused[va] = "not in an executable section"
        elif not c["clean"]:
            held[va] = c["why"]
        else:
            ok, note = checker.verdict(va)
            if ok:
                accepted[va] = (c["reason"], note)
            else:
                refused[va] = note
    return accepted, refused, held


def gate(xbe, functions_path, candidates, existing=(), analysis_json=None, log=print):
    """The seed entries to add for {address: reason}: those that clear the
    three static gates in the module docstring, not already in `existing`
    (seed entries). Each is {"start", "observed", "note"}, ready for
    --seed-functions. For a run's log, which has no order to go by; a
    discovery file goes through gate_discovery, which adds the call-site and
    taint rules."""
    if not candidates:
        return []
    checker = SeedChecker(xbe, functions_path, analysis_json)
    have = {e["start"].lower() for e in existing if isinstance(e, dict)}
    out = []
    for va, reason in sorted(candidates.items()):
        if ("0x%08x" % va) in have:
            log("  = %08X  already seeded" % va)
            continue
        ok, note = checker.verdict(va)
        if not ok:
            log("  - %08X  %s" % (va, note))
            continue
        log("  + %08X  %s  (%s)" % (va, note, reason))
        out.append(seed_entry(va, reason))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", type=Path,
                    help="stdout+stderr of a run, or its discovery file (discovered.jsonl)")
    ap.add_argument("xbe", type=Path)
    ap.add_argument("--functions", type=Path, required=True,
                    help="disasm functions.json: the bodies a candidate must not cut, and "
                         "whether it is a new function or an alias inside a known one")
    ap.add_argument("--seeds", type=Path, required=True,
                    help="seed file to update (created if absent)")
    ap.add_argument("--analysis-json", type=Path,
                    help="xbe_parser output; only needed when it is not named "
                         "<xbe stem>_analysis.json next to the XBE")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    text = args.log.read_text(errors="replace")
    candidates = scan_log(text)
    if not candidates:
        print("No seedable addresses in the log.")
        return 0

    existing = json.loads(args.seeds.read_text()) if args.seeds.exists() else []
    if text.lstrip().startswith("{"):
        have = {e["start"].lower() for e in existing if isinstance(e, dict)}
        checker = SeedChecker(args.xbe, args.functions, args.analysis_json,
                              untrusted={int(k, 16) for k in have})
        accepted, refused, held = gate_discovery(text, checker)
        new = []
        for va in sorted(set(accepted) | set(refused) | set(held)):
            if va in accepted:
                if ("0x%08x" % va) in have:
                    print("  = %08X  already seeded" % va)
                    continue
                print("  + %08X  %s  (%s)" % (va, accepted[va][1], accepted[va][0]))
                new.append(seed_entry(va, accepted[va][0]))
            elif va in refused:
                print("  - %08X  %s" % (va, refused[va]))
            else:
                print("  ~ %08X  held: %s" % (va, held[va]))
    else:
        new = gate(args.xbe, args.functions, candidates, existing, args.analysis_json)
    existing += new
    added = len(new)

    if args.dry_run:
        print("dry run: %d would be added" % added)
        return 0

    args.seeds.parent.mkdir(parents=True, exist_ok=True)
    args.seeds.write_text(json.dumps(existing, indent=1))
    print("seeds: %d (+%d) -> %s" % (len(existing), added, args.seeds))
    if added:
        print("now re-run tools.disasm --seed-functions and lift again")
    return 0


if __name__ == "__main__":
    sys.exit(main())
