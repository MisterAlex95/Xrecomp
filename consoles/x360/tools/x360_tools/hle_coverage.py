"""Which imports of a title the runtime's HLE covers (XBA-270).

    python3 -m x360_tools.hle_coverage FILE.xex [--key-file KEY]

Lists every import of the XEX by library, ordinal and name (x360_exports.inc), function or variable, and whether
consoles/x360/runtime implements it (read from its sources: the HLE table of x360_kernel.c, the file calls of
x360_internal.h, the variables x360_kernel_init fills). What a title needs first is what this list says is missing.
"""

import argparse
import os
import re
import sys

from . import CONSOLE  # noqa: F401
from .exports import name as export_name
from tools.xex.xex import IMPORT_THUNK, XexError, load_keys, parse_xex

RUNTIME = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "runtime"))


def implemented():
    """{(lib, name)}: lib "xboxkrnl" or "xam"."""
    out = set()
    with open(os.path.join(RUNTIME, "x360_kernel.c")) as f:
        src = f.read()
    table = src[src.index("static const hle_entry HLE[]"):]
    table = table[:table.index("};")]
    for m in re.finditer(r"\bK\((\w+)\)|\bKA\((\w+),", table):
        out.add(("xboxkrnl", m.group(1) or m.group(2)))
    for m in re.finditer(r"\bXM\((\w+),", table):
        out.add(("xam", m.group(1)))
    for m in re.finditer(r"var\(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_(\w+)", src):
        out.add(("xboxkrnl", m.group(1)))
    for m in re.finditer(r"var\(x, X360_LIB_XAM, X360_xam_(\w+)", src):
        out.add(("xam", m.group(1)))
    with open(os.path.join(RUNTIME, "x360_internal.h")) as f:
        hdr = f.read()
    # the macro tables: X360_<GROUP>_CALLS(X) / X360_<GROUP>_ALIASES(Y), each a run of continued lines
    for m in re.finditer(r"#define X360_(\w+)_(CALLS|ALIASES)\((X|Y)\)((?:[^\n]*\\\n)*[^\n]*)", hdr):
        lib = "xam" if m.group(1) == "XAM" else "xboxkrnl"
        for n in re.finditer(r"\b[XY]\((\w+)", m.group(4)):
            out.add((lib, n.group(1)))
    with open(os.path.join(RUNTIME, "x360_more.c")) as f:
        more = f.read()
    more = more[more.index("static const more_entry MORE[]"):]
    for m in re.finditer(r"\bKM\((\w+),", more):
        out.add(("xboxkrnl", m.group(1)))
    for m in re.finditer(r"\bXM2\((\w+),", more):
        out.add(("xam", m.group(1)))
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
    ap.add_argument("xex")
    ap.add_argument("--key-file", action="append", default=[])
    a = ap.parse_args(argv)
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
