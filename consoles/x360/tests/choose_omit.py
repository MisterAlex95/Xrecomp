"""choose_omit.py SRC_DIR OUT [--all] [--scalar] -- the functions an XBA-283 test leaves out of a title's pack.

SRC_DIR: x360_tools.recomp's C of the whole title. Writes to OUT the entries (hex, one per line) of every second
function by address (lifted and interpreted code then call each other both ways), or of every function with --all.
--scalar keeps the functions that use vector registers lifted."""

import glob
import os
import re
import sys


def functions(src):
    """{entry: C body} of every lifted function."""
    out = {}
    for path in sorted(glob.glob(os.path.join(src, "x360_rc_[0-9]*.c"))):
        with open(path) as fh:
            text = fh.read()
        parts = re.split(r"^RC_FN xe_fn_([0-9A-F]{8})\(", text, flags=re.M)
        for k in range(1, len(parts), 2):
            out[int(parts[k], 16)] = parts[k + 1]
    return out


def main(argv):
    src, out = argv[0], argv[1]
    fns = functions(src)
    entries = sorted(fns)
    if "--scalar" in argv:
        entries = [e for e in entries if "V(" not in fns[e]]
    if "--all" not in argv:
        entries = entries[1::2]
    with open(out, "w") as f:
        for e in entries:
            f.write("%08X\n" % e)
    print("%d of %d functions left out" % (len(entries), len(fns)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
