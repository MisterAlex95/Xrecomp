"""A stand-in for the N64 lifter (`python -m n64_tools.recomp ROM OUTDIR [--discovered FILE]`,
consoles/n64/runtime/n64_recomp.h) for the Prepare's edge-case tests (tools/prepare/test_n64_prepare.py): fast, and
it fails on demand. Run with XBA_N64_RECOMP=fake_n64_recomp and XBA_N64_TOOLS_DIR=<this directory>.

It writes one C file: a table with the right ABI, source_sha256 (of ROM in big-endian order, any byte order given) and
one entry per discovered record (address + hash; each a function that does nothing: the tests never run it).
FAKE_N64_RECOMP_FAIL=1 makes it fail (exit 3)."""

import hashlib
import os
import sys


def normalize(data):
    head = data[:4]
    out = bytearray(data)
    if head == b"\x37\x80\x40\x12":
        n = len(data) & ~1
        out[0:n:2], out[1:n:2] = data[1:n:2], data[0:n:2]
    elif head == b"\x40\x12\x37\x80":
        n = len(data) & ~3
        out[0:n:4], out[1:n:4], out[2:n:4], out[3:n:4] = data[3:n:4], data[2:n:4], data[1:n:4], data[0:n:4]
    return bytes(out)


def main(argv):
    if os.environ.get("FAKE_N64_RECOMP_FAIL"):
        print("fake_n64_recomp: failing as asked", file=sys.stderr)
        return 3
    rom, out = argv[0], argv[1]
    disc = argv[argv.index("--discovered") + 1] if "--discovered" in argv else None
    with open(rom, "rb") as f:
        sha = hashlib.sha256(normalize(f.read())).hexdigest()
    recs = []
    if disc:
        with open(disc) as f:
            for line in f:
                s = line.split("#")[0].split()
                if s:
                    recs.append((int(s[0], 16), int(s[1], 16) if len(s) > 1 else 0))
    lines = ['#include "n64_recomp_rt.h"', ""]
    for i, _ in enumerate(recs):
        lines.append("static void f%d(n64_machine *m) { (void)m; }" % i)
    lines.append("static const n64_recomp_entry entries[] = {")
    for i, (a, h) in enumerate(recs):
        lines.append("    {0x%08Xu, 4u, 0x%016XULL, f%d}," % (a, h, i))
    if not recs:
        lines.append("    {0u, 0u, 0u, 0},")
    lines.append("};")
    lines.append("static const n64_recomp_table table = {N64_RECOMP_ABI, %du, entries, \"%s\", 0x80000400u};"
                 % (len(recs), sha))
    lines.append("__attribute__((visibility(\"default\"))) const n64_recomp_table *n64_recomp_title_table(void) "
                 "{ return &table; }")
    with open(os.path.join(out, "n64_rc_table.c"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("fake_n64_recomp: %d entries for %s" % (len(recs), sha))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
