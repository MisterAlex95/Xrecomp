"""A shift by cl that turns out to be zero leaves every flag alone.

x86 changes no flag when a shift's masked count is 0. The lifter guarded _cf
for that case but wrote the result snapshot (_fa/_fas) unconditionally and
made the shift the flag owner, so `cmp eax, eax; shl ecx, cl; jz` with
cl = 0 branched on ecx == 0 instead of the cmp's ZF.

A register count now records `_shz` (count was zero) before the write, the
snapshot sits under `if (!_shz)`, and the condition reads
`_shz ? <the setter before> : <the shift>` -- the previous setter's snapshot
is still intact exactly when _shz is set.

Real bytes, translated as a function, built at -O2, run.
"""
import unittest

from .lift_fixture import build_and_run, gnu_cc, translate

# taken -> eax = 1, else eax = 0
TAIL = "06 b800000000 c3 b801000000 c3"

CASES = {
    # cmp eax, eax ; shl ecx, cl ; jz -- the cmp sets ZF; a zero count keeps
    # it whatever ecx holds, a non-zero one answers from the shifted value.
    "shl_jz": ("39c0 d3e1 74" + TAIL, [
        ("eax = 7; ecx = 0x00000500u;", 1),   # cl = 0, ecx != 0: still ZF = 1
        ("eax = 7; ecx = 0x00000501u;", 0),   # cl = 1: 0x501 << 1 != 0
        ("eax = 7; ecx = 0x80000001u;", 0),   # cl = 1: 0x80000001 << 1 = 2
        ("eax = 7; ecx = 0x00000020u;", 1),   # cl = 32 masks to 0
    ]),
    # cmp ebx, edx ; shr eax, cl ; jc
    "shr_jc": ("39d3 d3e8 72" + TAIL, [
        ("ebx = 1; edx = 2; ecx = 0; eax = 0;", 1),        # count 0: the cmp's CF
        ("ebx = 2; edx = 1; ecx = 0; eax = 1;", 0),
        ("ebx = 1; edx = 2; ecx = 1; eax = 2;", 0),        # shr 2, 1: CF = 0
        ("ebx = 2; edx = 1; ecx = 1; eax = 3;", 1),        # shr 3, 1: CF = 1
        ("ebx = 1; edx = 2; ecx = 32; eax = 2;", 1),       # 32 masks to 0
    ]),
    # cmp ebx, edx ; sar eax, cl ; jl
    "sar_jl": ("39d3 d3f8 7c" + TAIL, [
        ("ebx = 0xFFFFFFFFu; edx = 1; ecx = 0; eax = 4;", 1),  # -1 < 1
        ("ebx = 1; edx = 0xFFFFFFFFu; ecx = 0; eax = 0xFFFFFFFEu;", 0),
        ("ebx = 1; edx = 0xFFFFFFFFu; ecx = 1; eax = 0xFFFFFFFEu;", 1),  # -1: SF
    ]),
}


class ShiftZeroCountTest(unittest.TestCase):
    def setUp(self):
        if not gnu_cc():
            self.skipTest("no GNU C compiler available")

    def test_compiled_at_O2(self):
        functions, body = [], ["    int bad = 0;"]
        for name, (hexcode, rows) in CASES.items():
            functions.append(translate(hexcode, name=name))
            for setup, want in rows:
                body.append("    guest_reset(); %s %s();" % (setup, name))
                body.append('    if (eax != %du) { printf("%s: %s -> %%u\\n", eax);'
                            ' bad++; }' % (want, name, setup))
        body.append("    return bad != 0;")
        rc, out = build_and_run(functions, "\n".join(body))
        self.assertEqual(rc, 0, out + "\n".join(functions))

    def test_the_snapshot_is_guarded_and_an_immediate_is_not(self):
        code = translate(CASES["shl_jz"][0])
        self.assertIn("_shz = !", code)
        self.assertIn("if (!_shz) { _fa = ", code)
        # shl ecx, 4: known non-zero, no guard, no _shz declared.
        code = translate("39c0 c1e104 74" + TAIL)
        self.assertNotIn("_shz", code)


if __name__ == "__main__":
    unittest.main()
