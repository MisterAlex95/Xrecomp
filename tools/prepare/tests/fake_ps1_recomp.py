"""A stand-in for the PS1 lifter (`python -m ps1_tools.recomp INPUT OUTDIR [--discovered FILE]`,
consoles/ps1/runtime/ps1_recomp.h) for the Prepare's edge-case tests (tools/prepare/test_ps1_prepare.py): fast, and
it fails on demand. Run with XBA_PS1_RECOMP=fake_ps1_recomp and XBA_PS1_TOOLS_DIR=<this directory>.

It writes one C file: a table with the right ABI, source_sha256 (of INPUT, the boot EXE the Prepare copied) and one
entry per discovered record (address + hash; each a function that does nothing: the tests never run it).
FAKE_PS1_RECOMP_FAIL=1 makes it fail (exit 3)."""

import hashlib
import os
import sys


def main(argv):
    if os.environ.get("FAKE_PS1_RECOMP_FAIL"):
        print("fake_ps1_recomp: failing as asked", file=sys.stderr)
        return 3
    exe, out = argv[0], argv[1]
    disc = argv[argv.index("--discovered") + 1] if "--discovered" in argv else None
    with open(exe, "rb") as f:
        data = f.read()
    sha = hashlib.sha256(data).hexdigest()
    pc0 = int.from_bytes(data[0x10:0x14], "little")
    recs = []
    if disc:
        with open(disc) as f:
            for line in f:
                s = line.split("#")[0].split()
                if s:
                    recs.append((int(s[0], 16), int(s[1], 16) if len(s) > 1 else 0))
    lines = ['#include "ps1_recomp_rt.h"', ""]
    for i, _ in enumerate(recs):
        lines.append("static void f%d(ps1_machine *m) { (void)m; }" % i)
    lines.append("static const ps1_recomp_entry entries[] = {")
    for i, (a, h) in enumerate(recs):
        lines.append("    {0x%08Xu, 4u, 0x%016XULL, f%d}," % (a, h, i))
    if not recs:
        lines.append("    {0u, 0u, 0u, 0},")
    lines.append("};")
    lines.append("static const ps1_recomp_table table = {PS1_RECOMP_ABI, %du, entries, \"%s\", 0x%08Xu};"
                 % (len(recs), sha, pc0))
    lines.append("RC_EXPORT const ps1_recomp_table *ps1_recomp_title_table(void) { return &table; }")
    with open(os.path.join(out, "ps1_rc_table.c"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("fake_ps1_recomp: %d entries for %s" % (len(recs), sha))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
