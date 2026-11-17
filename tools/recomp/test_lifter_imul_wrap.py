"""The 2/3-operand imul wraps, as x86 does, at -O2.

The product used to be written as `(uint32_t)((int32_t)a * (int32_t)b)`. A
signed C multiply that overflows is undefined, and titles are built at -O2
without -fwrapv, so gcc could take the product as overflow-free: with
`imul eax, ecx, 3; test eax, eax; js` it folded the sign test into
`ecx < 0`, and for ecx = 0x30000000 (product 0x90000000, negative) the branch
went the other way. The 16-bit form multiplied zero-extended LO16() reads, so
`imul ax, bx` with 0xFFFF * 0xFFFF overflowed int although the x86 result
(1) is ordinary.

Real bytes, translated as a title's function, compiled at -O2 against the
real runtime header. The inputs come from a volatile table so the constant
folder cannot answer instead of the emitted code.
"""
import unittest

from .lift_fixture import build_and_run, gnu_cc, translate

# imul eax, ecx, 3 ; mov edx, eax ; test eax, eax ; js L ;
# mov eax, 0 ; ret ; L: mov eax, 1 ; ret
IMUL32 = "6bc103 89c2 85c0 7806 b800000000 c3 b801000000 c3"
# imul ax, bx ; mov edx, eax ; test ax, ax ; js L ; (same tail)
IMUL16 = "660fafc3 89c2 6685c0 7806 b800000000 c3 b801000000 c3"


class ImulEmissionTest(unittest.TestCase):
    def test_no_signed_multiply_is_emitted(self):
        for hexcode in (IMUL32, IMUL16):
            code = translate(hexcode)
            line = next(ln for ln in code.splitlines() if "edx" not in ln
                        and "* (uint32_t)" in ln and "_fa =" not in ln)
            self.assertNotIn("(int32_t)", line, line)


class ImulWrapsAtO2Test(unittest.TestCase):
    def setUp(self):
        if not gnu_cc():
            self.skipTest("no GNU C compiler available")

    def _run(self, hexcode, setup, cases):
        body = ["    int bad = 0; unsigned i;",
                "    static volatile uint32_t in[] = {%s};"
                % ", ".join("0x%08Xu" % c[0] for c in cases),
                "    static const uint32_t prod[] = {%s};"
                % ", ".join("0x%08Xu" % c[1] for c in cases),
                "    static const uint32_t sign[] = {%s};"
                % ", ".join(str(c[2]) for c in cases),
                "    for (i = 0; i < %d; i++) {" % len(cases),
                "        guest_reset();",
                "        " + setup,
                "        lifted();",
                "        if (edx != prod[i] || eax != sign[i]) {",
                '            printf("in=%08X edx=%08X want %08X sign=%u '
                'want %u\\n", in[i], edx, prod[i], eax, sign[i]);',
                "            bad++;",
                "        }",
                "    }",
                "    return bad != 0;"]
        code = translate(hexcode)
        rc, out = build_and_run([code], "\n".join(body))
        self.assertEqual(rc, 0, out)
        # Whether a given gcc exploits the overflow depends on what it can
        # see (gcc 13 folded the sign test of a register-local shape; gcc 14
        # keeps the g_cpu store here), so the answers alone cannot pin the
        # defect. UBSan (trapping, so no runtime library is needed) pins its cause: the emitted product must not be a
        # signed overflow on these inputs whatever the optimiser does.
        rc, out = build_and_run(
            [code], "\n".join(body),
            extra_flags=("-fsanitize=signed-integer-overflow",
                         "-fsanitize-trap=all"))
        self.assertEqual(rc, 0, "the emitted product overflows a signed "
                         "multiply (UBSan trapped):\n" + out)

    def test_imul_r32_imm_wraps(self):
        # (ecx, edx = ecx * 3 mod 2^32, js taken)
        self._run(IMUL32, "eax = 0; ecx = in[i];", [
            (0x30000000, 0x90000000, 1),   # the overflowing case
            (0x60000000, 0x20000000, 0),
            (0xD5555556, 0x80000002, 1),
            (0x00000005, 0x0000000F, 0),
            (0xFFFFFFFF, 0xFFFFFFFD, 1),
        ])

    def test_imul_r16_wraps(self):
        # ax = bx = in; the high halves of eax/edx keep 0xAAAA/eax's.
        self._run(IMUL16, "eax = 0xAAAA0000u | in[i]; ebx = in[i]; edx = 0;", [
            (0xFFFF, 0xAAAA0001, 0),       # (-1) * (-1) = 1
            (0x00B6, 0xAAAA8164, 1),       # 182^2 = 33124 wraps negative
            (0x8000, 0xAAAA0000, 0),
            (0x0003, 0xAAAA0009, 0),
        ])


if __name__ == "__main__":
    unittest.main()
