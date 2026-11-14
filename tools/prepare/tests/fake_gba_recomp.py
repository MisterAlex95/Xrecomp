"""A stand-in for the GBA lifter (`python -m gba_tools.recomp ROM OUTDIR [--discovered FILE]`,
consoles/gba/runtime/gba_recomp.h) for the Prepare's and the loader's tests, until the real one lands and wherever
it is not wanted (tools/prepare/test_gba_prepare.py, consoles/gba/tests). Run with XBA_GBA_RECOMP=fake_gba_recomp and
XBA_GBA_TOOLS_DIR=<this directory>.

It writes one C file: a table with the right ABI and rom_sha256 and one entry per discovered address (each a function
that does nothing: the tests never run it), sorted by address. FAKE_GBA_RECOMP_FAIL=1 makes it fail (exit 3)."""

import hashlib
import os
import sys


def main(argv):
    if os.environ.get("FAKE_GBA_RECOMP_FAIL"):
        print("fake_gba_recomp: failing as asked", file=sys.stderr)
        return 3
    rom, out = argv[0], argv[1]
    disc = argv[argv.index("--discovered") + 1] if "--discovered" in argv else None
    with open(rom, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    addrs = []
    if disc:
        with open(disc) as f:
            addrs = sorted({int(l.split("#")[0].strip(), 16) for l in f if l.split("#")[0].strip()})
    lines = ['#include "gba_recomp.h"', ""]
    for i, _ in enumerate(addrs):
        lines.append("static void f%d(gba_machine *m) { (void)m; }" % i)
    lines.append("static const gba_recomp_entry entries[] = {")
    for i, a in enumerate(addrs):
        lines.append("    {0x%08Xu, %du, f%d}," % (a & ~1, a & 1, i))
    if not addrs:
        lines.append("    {0u, 0u, 0},")
    lines.append("};")
    lines.append("static const gba_recomp_table table = {GBA_RECOMP_ABI, %du, entries, \"%s\"};" % (len(addrs), sha))
    lines.append('__attribute__((visibility("default"))) const gba_recomp_table *gba_recomp_title_table(void)')
    lines.append("{")
    lines.append("    return &table;")
    lines.append("}")
    with open(os.path.join(out, "gba_title.c"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("fake_gba_recomp: %d entries for %s" % (len(addrs), sha))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
