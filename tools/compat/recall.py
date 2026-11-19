"""
Prepare-time recall (XBA-129, XBA-75): of the code a title reached at run
time through an indirect call or a thread start, how much did the static
analysis find, and what kind of reference hid the rest.

    python -m tools.compat recall WORK XBE [--icall-dump icall_targets.dump] [--json]

Inputs, all from the title's work folder:

  * discovered.jsonl, its whole history (every run appends what is new): each
    icall / thread / thread_ctx entry is a target the analysis missed;
  * discovered-seeds.json / discovered-rejected.json: what the prepares made
    of them (seeded, or refused by seed_from_log's gates: not code);
  * the XBE, to say where a missed address is referenced from.

The share found statically needs the targets the run reached that the
analysis did find, which the discovery file does not record (only misses).
Every runtime records them since XBA-304 (the generated dispatch's window
fills as targets are reached) and writes icall_targets.dump beside
discovered.jsonl, merged over the runs; --icall-dump names another dump (a
RECOMP_ICALL_FEEDBACK build's, tools/recomp/icall_feedback.py). Without one,
or with an empty one (a pack prepared before XBA-304 records nothing), the
misses and their categories are still reported; the recall is then unknown.

Categories of a miss, from the image (input for the disassembler):

  thread start    a thread's start routine or StartContext1 (thread, thread_ctx)
  vtable          its address is a word in a run of words that all point into
                  code (a vtable, a table of function pointers)
  table word      its address is a word in the image, alone among non-code
                  words (a dispatch or class-driver table entry, a callback)
  computed jump   its address appears nowhere in the image: computed at run
                  time (base + offset, a relocated or decoded pointer)

A miss whose address lies inside a function the analysis knows is also
counted as « inside a known function » (the sweep decoded through it).
"""

import argparse
import bisect
import json
import os
import struct
import sys

from tools.compat.heal import SEED_KINDS, entries_from

CATEGORIES = ("thread start", "vtable", "table word", "computed jump")


def _read_json(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def misses(history):
    """{va (int): kind} of the first entry naming each address."""
    out = {}
    for e in history:
        if e.get("t") in SEED_KINDS:
            try:
                va = int(e["va"], 16)
            except (KeyError, ValueError, TypeError):
                continue
            out.setdefault(va, e["t"])
    return out


class Image:
    """The XBE's sections as (va, bytes, executable); words read little-endian."""

    def __init__(self, sections):
        self.sections = sections
        self.code = [(va, va + len(b)) for va, b, x in sections if x]

    @classmethod
    def load(cls, xbe):
        from tools.disasm.loader import DATA_SECTION_NAMES, load_image
        xjson = os.path.splitext(xbe)[0] + "_analysis.json"
        img = load_image(xbe, xjson if os.path.isfile(xjson) else None)
        return cls([(s.virtual_addr, img.get_section_data(s), s.executable and s.name not in DATA_SECTION_NAMES)
                    for s in img.sections])

    def is_code(self, va):
        return any(lo <= va < hi for lo, hi in self.code)

    def word_sites(self, va):
        """[(section bytes, offset)] where the dword `va` is stored, 4-byte aligned or not."""
        needle = struct.pack("<I", va)
        out = []
        for _, data, _ in self.sections:
            i = data.find(needle)
            while i >= 0:
                out.append((data, i))
                i = data.find(needle, i + 1)
        return out

    def category(self, va, kind):
        if kind in ("thread", "thread_ctx"):
            return "thread start"
        sites = self.word_sites(va)
        if not sites:
            return "computed jump"
        for data, i in sites:
            neighbours = 0
            for j in (i - 4, i + 4):
                if 0 <= j and j + 4 <= len(data) and self.is_code(struct.unpack_from("<I", data, j)[0]):
                    neighbours += 1
            if neighbours:
                return "vtable"
        return "table word"


def parse_icall_dump(path):
    """{va: flags} from an icall-feedback dump (1 resolved, 2 unresolved, 3 both)."""
    from tools.recomp.icall_feedback import parse_dump
    return parse_dump(path)


DUMP_NAME = "icall_targets.dump"     # the runtime's, beside discovered.jsonl (recomp_icall_hits_dump)


def report(work, xbe=None, icall_dump=None, image=None):
    """The recall of one title as a dict (see the module docstring)."""
    history = entries_from(os.path.join(work, "discovered.jsonl"))
    missed = misses(history)
    seeded = {int(e["start"], 16) for e in _read_json(os.path.join(work, "discovered-seeds.json"), [])
              if isinstance(e, dict) and e.get("start")}
    rejected = {int(v, 16) for v in _read_json(os.path.join(work, "discovered-rejected.json"), [])
                if isinstance(v, str)}
    if image is None and xbe and os.path.isfile(xbe):
        try:
            image = Image.load(xbe)
        except Exception:           # an unreadable XBE: the categories are unknown, the counts are not
            image = None
    funcs = _read_json(os.path.join(work, "analysis", "disasm", "functions.json"), [])
    as_int = lambda v: int(v, 0) if isinstance(v, str) else int(v)
    bounds = sorted((as_int(f["start"]), as_int(f["end"])) for f in funcs if isinstance(f, dict)
                    and "start" in f and "end" in f)
    starts = [b[0] for b in bounds]
    by_cat = {c: 0 for c in CATEGORIES}
    inside = 0
    rows = []
    for va, kind in sorted(missed.items()):
        cat = image.category(va, kind) if image else None
        if cat:
            by_cat[cat] += 1
        i = bisect.bisect_right(starts, va) - 1
        is_inside = i >= 0 and bounds[i][0] < va < bounds[i][1] and va not in seeded
        inside += is_inside
        rows.append({"va": "0x%08X" % va, "kind": kind, "category": cat,
                     "outcome": "seeded" if va in seeded else "rejected" if va in rejected else "pending",
                     "inside": bool(is_inside)})
    out = {"misses": len(missed), "seeded": sum(r["outcome"] == "seeded" for r in rows),
           "rejected": sum(r["outcome"] == "rejected" for r in rows),
           "runs": sum(1 for e in history if e.get("t") == "run"),
           "categories": by_cat if image else None, "inside_known": inside, "targets": None,
           "found_statically": None, "recall": None, "rows": rows}
    if not icall_dump and os.path.isfile(os.path.join(work, DUMP_NAME)):
        icall_dump = os.path.join(work, DUMP_NAME)
    reached = parse_icall_dump(icall_dump) if icall_dump and os.path.isfile(icall_dump) else {}
    out["icall_dump"] = icall_dump if reached else None
    if reached:
        targets = set(reached) | set(missed)
        # Reached and resolved, and not one the runs taught the analysis.
        found = {va for va, flags in reached.items() if flags & 1 and va not in missed}
        out.update(targets=len(targets), found_statically=len(found),
                   recall=round(len(found) / len(targets), 3) if targets else None)
    return out


def summary(r):
    """One line for the compat report: « 93 % (4 misses: 2 vtable, 1 table word, 1 thread start) »."""
    if not r or not r.get("misses"):
        return "100 %" if r and r.get("recall") == 1 else "no miss" if r else ""
    cats = r.get("categories") or {}
    parts = ["%d %s" % (n, c) for c, n in cats.items() if n]
    head = "%d %%" % round(r["recall"] * 100) if r.get("recall") is not None else "recall unknown"
    return "%s (%d miss%s%s)" % (head, r["misses"], "" if r["misses"] == 1 else "es",
                                 ": " + ", ".join(parts) if parts else "")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.compat recall", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("work", help="the title's prepare work folder (discovered.jsonl, discovered-seeds.json)")
    ap.add_argument("xbe", nargs="?", help="its default.xbe, for the categories")
    ap.add_argument("--icall-dump", help="an icall-feedback dump, for the share found (default: the work "
                    "folder's %s, which the runtime writes)" % DUMP_NAME)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    r = report(a.work, a.xbe, a.icall_dump)
    if a.json:
        print(json.dumps(r, indent=1))
        return 0
    print(summary(r) or "no miss")
    for row in r["rows"]:
        print("  %s  %-10s %-14s %s%s" % (row["va"], row["kind"], row["category"] or "?", row["outcome"],
                                         ", inside a known function" if row["inside"] else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
