"""Which imports of a title the runtime's HLE covers (XBA-270).

    python3 -m x360_tools.hle_coverage FILE.xex [--key-file KEY]
    python3 -m x360_tools.hle_coverage --summary       what the runtime implements of every export
    python3 -m x360_tools.hle_coverage --implemented   one line per implemented export (x360_hle_test --list-hle's text)
    python3 -m x360_tools.hle_coverage --missing [LIB] the exports without an HLE

Lists every import of the XEX by library, ordinal and name (x360_exports.inc), function or variable, and whether
consoles/x360/runtime implements it (read from its sources; the x360_hle_table test compares that reading with the
runtime's real dispatch table). What a title needs first is what this list says is missing.
"""

import argparse
import glob
import os
import re
import sys

from . import CONSOLE  # noqa: F401
from .exports import all_exports
from .exports import name as export_name
from tools.xex.xex import IMPORT_THUNK, XexError, load_keys, parse_xex

RUNTIME = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "runtime"))


def _strip_comments(src):
    return re.sub(r"/\*.*?\*/", " ", src, flags=re.S)


def _table(src, start):
    """The initialiser that follows `start` (up to its closing brace at depth 0)."""
    at = src.index("{", src.index(start) + len(start))
    depth = 0
    for k in range(at, len(src)):
        depth += (src[k] == "{") - (src[k] == "}")
        if not depth:
            return re.sub(r"^\s*#.*$", "", src[at:k], flags=re.M)  # a table's own #define E(name) is no row
    raise ValueError("unterminated table after " + start)


def implemented():
    """{(lib, name)}: lib "xboxkrnl" or "xam". Exactly what x360_hle_lookup answers and x360_kernel_init's variables:
    the HLE table of x360_kernel.c, the macro tables of x360_internal.h, MORE (x360_more.c), XMA (x360_xma.c), the
    x360_tab_<group> tables of every runtime file, and the variable exports."""
    out = set()
    sources = {}
    for path in sorted(glob.glob(os.path.join(RUNTIME, "*.c"))):
        with open(path) as f:
            sources[os.path.basename(path)] = _strip_comments(f.read())
    with open(os.path.join(RUNTIME, "x360_internal.h")) as f:
        hdr = _strip_comments(f.read())

    table = _table(sources["x360_kernel.c"], "static const x360_hle_row HLE[]")
    for m in re.finditer(r"\bK\((\w+)\)|\bKA\((\w+),", table):
        out.add(("xboxkrnl", m.group(1) or m.group(2)))
    for m in re.finditer(r"\bXM\((\w+),", table):
        out.add(("xam", m.group(1)))
    # the macro tables: X360_<GROUP>_CALLS(X) / X360_<GROUP>_ALIASES(Y), each a run of continued lines
    for m in re.finditer(r"#define X360_(\w+)_(CALLS|ALIASES)\((X|Y)\)((?:[^\n]*\\\n)*[^\n]*)", hdr):
        lib = "xam" if m.group(1) in ("XAM", "INPUT") else "xboxkrnl"
        for n in re.finditer(r"\b[XY]\((\w+)", m.group(4)):
            out.add((lib, n.group(1)))
    more = _table(sources["x360_more.c"], "static const x360_hle_row MORE[]")
    for m in re.finditer(r"\bKM\((\w+),", more):
        out.add(("xboxkrnl", m.group(1)))
    for m in re.finditer(r"\bXM2\((\w+),", more):
        out.add(("xam", m.group(1)))
    for m in re.finditer(r"\bE\((\w+)\)", _table(sources["x360_xma.c"], "static const x360_hle_row XMA[]")):
        out.add(("xboxkrnl", m.group(1)))
    for src in sources.values():
        for start in re.finditer(r"const x360_hle_row x360_tab_\w+\[\]", src):
            rows = _table(src[start.start():], start.group(0))
            for m in re.finditer(r"\bX360_H([KX])\((\w+),", rows):
                out.add(("xboxkrnl" if m.group(1) == "K" else "xam", m.group(2)))
        for m in re.finditer(r"\b(?:x360_)?var\(x, X360_LIB_\w+, X360_(xboxkrnl|xam)_(\w+)", src):
            out.add((m.group(1), m.group(2)))
    return out


def summary():
    """{lib: {"function": [implemented, exports], "variable": [implemented, exports]}} over x360_exports.inc."""
    have = implemented()
    out = {}
    for lib, n, k in all_exports():
        row = out.setdefault(lib, {"function": [0, 0], "variable": [0, 0]})["function" if k == "FN" else "variable"]
        row[1] += 1
        row[0] += (lib, n) in have
    return out


def coverage(img):
    """[(library, ordinal, name, 'function' | 'variable', covered)] for the XEX's imports (one row per import)."""
    have = implemented()
    rows = []
    for lib in img.libraries:
        short = lib.name.split(".")[0].lower()
        thunks = {r.ordinal for r in lib.records if r.kind == IMPORT_THUNK}
        seen = set()
        for r in lib.records:
            if r.ordinal in seen:
                continue
            seen.add(r.ordinal)
            n = export_name(short, r.ordinal) or "?"
            kind = "function" if r.ordinal in thunks else "variable"
            rows.append((lib.name, r.ordinal, n, kind, (short, n) in have))
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python3 -m x360_tools.hle_coverage")
    ap.add_argument("xex", nargs="?")
    ap.add_argument("--key-file", action="append", default=[])
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--implemented", action="store_true")
    ap.add_argument("--missing", nargs="?", const="all")
    a = ap.parse_args(argv)
    if a.summary:
        for lib, kinds in sorted(summary().items()):
            for k, (n, total) in sorted(kinds.items()):
                print("%-9s %-9s %4d / %4d" % (lib, k, n, total))
        return 0
    if a.implemented:
        have = implemented()
        for lib, n, k in sorted(all_exports()):
            if (lib, n) in have:
                print(lib, n, "function" if k == "FN" else "variable")
        return 0
    if a.missing:
        have = implemented()
        for lib, n, k in sorted(all_exports()):
            if (lib, n) not in have and a.missing in ("all", lib):
                print(lib, n, "function" if k == "FN" else "variable")
        return 0
    if not a.xex:
        ap.error("a XEX, --summary, --implemented or --missing")
    try:
        with open(a.xex, "rb") as f:
            img = parse_xex(f.read(), load_keys(a.key_file))
    except (OSError, XexError) as ex:
        print("error: %s" % ex, file=sys.stderr)
        return 1
    if img.image is None:
        print("error: %s (the import records live in the image)" % img.image_note, file=sys.stderr)
        return 1
    rows = coverage(img)
    for lib, o, n, kind, ok in rows:
        print("%-14s %4d  %-40s %-8s %s" % (lib, o, n, kind, "HLE" if ok else "missing"))
    have = sum(1 for r in rows if r[4])
    print("imports: %d, covered by the HLE: %d, missing: %d" % (len(rows), have, len(rows) - have))
    return 0


if __name__ == "__main__":
    sys.exit(main())
