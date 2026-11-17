"""rol/ror, shld/shrd, the 2/3-operand imul and xadd produce CF into _cf.

rol/ror change only CF and OF on x86, but the tracker made them the owner of
every flag and had no condition for them: `test eax, eax; rol ecx, 1; jz`
lifted the jz to the never-assigned `_flags` fallback (never taken), and so
did a `jc` after the rotate. None of the four wrote _cf either, so an adc or
rcr after them consumed the carry of some earlier instruction:
`stc; shrd eax, edx, 1; rcr ecx, 1` rotated in the stc's 1.

Now each writes _cf where x86 writes CF, rol/ror keep the setter before
them for ZF/SF/PF (the "__cf_over" state), and a rotate by cl that turns out
to be zero leaves the incoming CF in _cf.

Real bytes, translated as a function, built at -O2, run.
"""
import unittest

from .lift_fixture import build_and_run, gnu_cc, translate

TAIL = "7206 b800000000 c3 b801000000 c3"   # jc L ; eax = 0 ; ret ; L: eax = 1 ; ret
JZ_TAIL = "7406 b800000000 c3 b801000000 c3"

CASES = {
    # test eax, eax ; rol ecx, 1 ; jz
    "test_rol_jz": ("85c0 d1c1 " + JZ_TAIL, [
        ("eax = 0; ecx = 0x80000000u;", "eax == 1"),
        ("eax = 5; ecx = 0;", "eax == 0"),
    ]),
    # rol eax, 1 ; jc
    "rol_jc": ("d1c0 " + TAIL, [
        ("eax = 0x80000000u;", "eax == 1"),
        ("eax = 0x40000000u;", "eax == 0"),
    ]),
    # ror al, 1 ; jc -- the carry is the byte's top bit, not bit 31
    "ror8_jc": ("d0c8 " + TAIL, [
        ("eax = 0x00000001u;", "eax == 1"),
        ("eax = 0x80000002u;", "eax == 0"),
    ]),
    # stc ; rol eax, 1 ; adc edx, 0 ; ret
    "rol_adc": ("f9 d1c0 83d200 c3", [
        ("eax = 1; edx = 5;", "edx == 5"),
        ("eax = 0x80000000u; edx = 5;", "edx == 6"),
    ]),
    # cmp ebx, edx ; rol eax, cl ; jc -- cl = 0 keeps the cmp's CF
    "rol_cl_zero_keeps_cf": ("39d3 d3c0 " + TAIL, [
        ("ebx = 1; edx = 2; ecx = 0; eax = 0;", "eax == 1"),
        ("ebx = 2; edx = 1; ecx = 0; eax = 0x80000000u;", "eax == 0"),
        ("ebx = 2; edx = 1; ecx = 1; eax = 0x80000000u;", "eax == 1"),
        ("ebx = 2; edx = 1; ecx = 32; eax = 0x80000000u;", "eax == 0"),
    ]),
    # stc ; shrd eax, edx, 1 ; rcr ecx, 1 ; ret
    "shrd_rcr": ("f9 0facd001 d1d9 c3", [
        ("eax = 2; edx = 0; ecx = 0;", "ecx == 0"),
        ("eax = 3; edx = 0; ecx = 0;", "ecx == 0x80000000u"),
    ]),
    # stc ; shld eax, edx, 4 ; jc
    "shld_jc": ("f9 0fa4d004 " + TAIL, [
        ("eax = 0x10000000u; edx = 0;", "eax == 1"),
        ("eax = 0x20000000u; edx = 0;", "eax == 0"),
    ]),
    # stc ; imul eax, ecx ; adc edx, 0 ; ret
    "imul_adc": ("f9 0fafc1 83d200 c3", [
        ("eax = 2; ecx = 3; edx = 5;", "edx == 5"),
        ("eax = 0x10000u; ecx = 0x10000u; edx = 5;", "edx == 6"),
    ]),
    # lock xadd [esi], eax ; jc
    "xadd_jc": ("f00fc106 " + TAIL, [
        ("esi = 0x1000; eax = 1; *(uint32_t *)(g_ram + 0x1000) = 0xFFFFFFFFu;",
         "eax == 1"),
        ("esi = 0x1000; eax = 1; *(uint32_t *)(g_ram + 0x1000) = 1;",
         "eax == 0"),
    ]),
    # xadd cx, dx ; jc -- 16-bit register form
    "xadd16_jc": ("660fc1d1 " + TAIL, [
        ("ecx = 0x0000FFFFu; edx = 1;", "eax == 1"),
        ("ecx = 0xFFFF0001u; edx = 1;", "eax == 0"),
    ]),
}


class CarryProducersTest(unittest.TestCase):
    def setUp(self):
        if not gnu_cc():
            self.skipTest("no GNU C compiler available")

    def test_compiled_at_O2(self):
        functions, body = [], ["    int bad = 0;"]
        for name, (hexcode, rows) in CASES.items():
            code = translate(hexcode, name=name)
            self.assertNotIn("_flags /*", code, code)
            functions.append(code)
            for setup, check in rows:
                body.append("    guest_reset(); %s %s();" % (setup, name))
                body.append('    if (!(%s)) { printf("%s: %s fails: eax=%%08X'
                            ' ecx=%%08X edx=%%08X\\n", eax, ecx, edx); bad++; }'
                            % (check, name, setup.replace('"', "")))
        body.append("    return bad != 0;")
        rc, out = build_and_run(functions, "\n".join(body))
        self.assertEqual(rc, 0, out)

    def test_a_rotate_keeps_the_zero_flag_of_the_setter_before_it(self):
        code = translate(CASES["test_rol_jz"][0])
        self.assertIn("CMP_EQ(_fa, _fb)", code)

    def test_a_rotate_by_an_immediate_zero_changes_nothing(self):
        # rol eax, 0 is not an encoding assemblers emit, but 0x20 masks to 0:
        # test eax, eax ; rol ecx, 0x20 ; jz -- still the test's ZF.
        code = translate("85c0 c1c120 " + JZ_TAIL)
        self.assertNotIn("_cf = (int)((uint32_t)(ecx)", code)
        self.assertIn("CMP_EQ(_fa, _fb)", code)


if __name__ == "__main__":
    unittest.main()
