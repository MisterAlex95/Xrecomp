"""`div` and `idiv` at every operand width, and the divide error (#DE).

`_lift_muldiv` used to emit the 32-bit form whatever the operand: EDX:EAX
divided by the operand, quotient to EAX, remainder to EDX. `div bl` divides
AX and leaves the quotient in AL and the remainder in AH; `div cx` divides
DX:AX and writes AX and DX. So a byte divide read EDX (unrelated) into the
dividend, and both narrow forms overwrote all of EAX and EDX.

A zero divisor, or a quotient too wide for the destination, raises #DE on
x86. In C the first is undefined behaviour (a SIGFPE on x86 hosts, a silent
zero on arm64) and the second truncated silently. Both now reach one runtime
helper, recomp_divide_error, which leaves the registers as they were, as the
CPU does.

The cases are compiled from the lifter's own output and checked against
reference values computed here in Python, so the reference shares nothing
with the C under test.
"""

import os
import subprocess
import tempfile
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter

VA = 0x00012340

PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
#define LO8(r)  ((uint8_t)((r) & 0xFF))
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
#define LO16(r) ((uint16_t)((r) & 0xFFFF))
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))
#define RECOMP_UNLIKELY(x) (x)
static uint8_t g_ram[16];
#define MEM8(a)  (*(volatile uint8_t *)(g_ram + (a)))
#define MEM16(a) (*(volatile uint16_t *)(g_ram + (a)))
#define MEM32(a) (*(volatile uint32_t *)(g_ram + (a)))
static int g_de;
static uint32_t g_de_va, g_de_divisor;
static void recomp_divide_error(uint32_t va, uint32_t divisor)
{
    g_de++;
    g_de_va = va;
    g_de_divisor = divisor;
}
struct Case { uint32_t eax, edx, src, want_eax, want_edx; int de; };
"""

BODY = r"""
static const struct Case CASES[] = {
CASES_HERE
};
int main(void) {
    unsigned i, bad = 0;
    for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const struct Case *c = &CASES[i];
        uint32_t eax = c->eax, edx = c->edx, ebx = c->src, esi = 0;
        MEM32(0) = c->src;
        g_de = 0;
        STMT
        (void)esi;
        if (eax != c->want_eax || edx != c->want_edx || g_de != c->de
            || (c->de && (g_de_va != VA_HERE || g_de_divisor != DIVISOR_HERE))) {
            printf("FAIL case %u: eax=%08X edx=%08X src=%08X -> eax=%08X "
                   "edx=%08X de=%d, want eax=%08X edx=%08X de=%d\n",
                   i, c->eax, c->edx, c->src, eax, edx, g_de,
                   c->want_eax, c->want_edx, c->de);
            if (++bad > 8) return 1;
        }
    }
    if (bad) return 1;
    printf("OK %u\n", i);
    return 0;
}
"""


def _sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def _ref(signed, bits, eax, edx, src):
    """x86 div/idiv: (eax, edx, de) after the instruction."""
    mask = (1 << bits) - 1
    if bits == 8:
        n, d = eax & 0xFFFF, src & 0xFF
    elif bits == 16:
        n, d = ((edx & 0xFFFF) << 16) | (eax & 0xFFFF), src & 0xFFFF
    else:
        n, d = (edx << 32) | eax, src
    if signed:
        n, d = _sx(n, 2 * bits), _sx(d, bits)
    if d == 0:
        return eax, edx, 1
    q = abs(n) // abs(d)            # x86 truncates toward zero
    if (n < 0) != (d < 0):
        q = -q
    r = n - q * d
    lo, hi = ((-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed
              else (0, mask))
    if not lo <= q <= hi:
        return eax, edx, 1
    q &= mask
    r &= mask
    if bits == 8:
        return (eax & 0xFFFF0000) | (r << 8) | q, edx, 0
    if bits == 16:
        return (eax & 0xFFFF0000) | q, (edx & 0xFFFF0000) | r, 0
    return q, r, 0


_EDGE = [0, 1, 2, 3, 7, 0x7F, 0x80, 0x81, 0xFF, 0x100, 0x7FFF, 0x8000, 0x8001,
         0xFFFF, 0x10000, 0x12345, 0x7FFFFFFF, 0x80000000, 0x80000001,
         0xFFFFFFFE, 0xFFFFFFFF, 0xDEADBEEF]


def _cases(signed, bits):
    """Dividend/divisor pairs reaching every outcome: plain results of both
    signs, zero divisors, quotients just inside and just past the range."""
    mask = (1 << bits) - 1
    out = []
    srcs = sorted({v & mask for v in _EDGE} | {mask, mask - 1, 5, 10})
    # The bits above the operand in a register source must not be read.
    for src in srcs:
        for eax in _EDGE + [0xAAAA0000 | 0x7F05, 0x5555FF80]:
            for edx in (0, 1, 0xFFFFFFFF, 0x00000080, 0x7FFFFFFF,
                        0x80000000, 0x1234FFFF, 0xCAFE0000):
                reg_src = src | (0xA5A5A5A5 & ~mask)
                out.append((eax, edx, reg_src,
                            *_ref(signed, bits, eax, edx, reg_src)))
    return out


def _lift(mnemonic, operand):
    insn = Instruction(VA, 2, mnemonic, "", "00", operands=[operand])
    lifter = Lifter()
    out = lifter.lift_instruction(insn)
    return lifter, "\n".join(out)


_SRC = {
    8: [Operand(type="reg", reg="bl"),
        Operand(type="mem", mem_base="esi", mem_size=1)],
    16: [Operand(type="reg", reg="bx"),
         Operand(type="mem", mem_base="esi", mem_size=2)],
    32: [Operand(type="reg", reg="ebx"),
         Operand(type="mem", mem_base="esi", mem_size=4)],
}


class DivideTest(unittest.TestCase):
    def setUp(self):
        from .c_fixture import compiler
        self.cc = compiler()
        if not self.cc:
            self.skipTest("no C compiler on PATH")

    def _run(self, stmt, cases, bits):
        from .c_fixture import command
        rows = ",\n".join(
            "{0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,0x%08Xu,%d}" % c for c in cases)
        divisor = "(c->src & 0x%Xu)" % ((1 << bits) - 1)
        src = PRELUDE + (BODY.replace("CASES_HERE", rows)
                         .replace("STMT", stmt)
                         .replace("VA_HERE", "0x%08Xu" % VA)
                         .replace("DIVISOR_HERE", divisor))
        with tempfile.TemporaryDirectory() as tmp:
            c = os.path.join(tmp, "t.c")
            with open(c, "w") as f:
                f.write(src)
            exe = os.path.join(tmp, "t.exe")
            r = subprocess.run(command(self.cc, [c], exe),
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-2000:] + "\n" + stmt)
            r = subprocess.run([exe], capture_output=True, text=True,
                               timeout=60)
            return r.returncode, r.stdout + r.stderr

    def test_every_width_signed_and_unsigned(self):
        for mnemonic, signed in (("div", False), ("idiv", True)):
            for bits, operands in _SRC.items():
                cases = _cases(signed, bits)
                # Every outcome is present in the table, or the sweep proves
                # less than it claims.
                self.assertTrue(any(c[5] for c in cases))
                self.assertTrue(any(not c[5] for c in cases))
                for operand in operands:
                    with self.subTest(m=mnemonic, bits=bits, src=operand.type):
                        lifter, stmt = _lift(mnemonic, operand)
                        self.assertEqual(lifter.unimplemented, {})
                        rc, out = self._run(stmt, cases, bits)
                        self.assertEqual(rc, 0, out + "\n" + stmt)

    def test_reference_values(self):
        """Spot checks of the reference itself, from the SDM's tables."""
        # div bl: AX=0x0107 / 2 -> AL=0x83, AH=1
        self.assertEqual(_ref(False, 8, 0x0107, 0, 2), (0x0183, 0, 0))
        # idiv bl: AX=-7 / 2 -> AL=-3, AH=-1
        self.assertEqual(_ref(True, 8, 0xFFF9, 0, 2), (0xFFFD, 0, 0))
        # div cx: DX:AX = 0x0001:0x0000 / 2 -> AX=0x8000, DX=0
        self.assertEqual(_ref(False, 16, 0, 1, 2), (0x8000, 0, 0))
        # idiv ecx: EDX:EAX = -1 / -1 -> 1, rem 0
        self.assertEqual(_ref(True, 32, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF),
                         (1, 0, 0))
        # idiv: INT_MIN / -1 overflows at each width
        self.assertEqual(_ref(True, 8, 0xFF80, 0, 0xFF)[2], 1)
        self.assertEqual(_ref(True, 16, 0x8000, 0xFFFF, 0xFFFF)[2], 1)
        self.assertEqual(_ref(True, 32, 0, 0x80000000, 0xFFFFFFFF)[2], 1)
        # div: high half >= divisor overflows
        self.assertEqual(_ref(False, 8, 0x0200, 0, 2)[2], 1)

    def test_zero_divisor_reaches_the_runtime_helper(self):
        for mnemonic in ("div", "idiv"):
            for bits, operands in _SRC.items():
                _, stmt = _lift(mnemonic, operands[0])
                self.assertIn("recomp_divide_error(0x%08Xu" % VA, stmt)

    def test_helper_is_declared_and_defined(self):
        import pathlib
        root = pathlib.Path(__file__).resolve().parents[2]
        hdr = (root / "templates" / "runtime" / "recomp_types.h").read_text(
            encoding="utf-8")
        self.assertIn("void recomp_divide_error(uint32_t va, uint32_t divisor);",
                      hdr)
        self.assertIn("#define RECOMP_UNLIKELY", hdr)
        hal = (root / "src" / "kernel" / "kernel_hal.c").read_text(
            encoding="utf-8")
        self.assertIn("void recomp_divide_error(uint32_t va, uint32_t divisor)",
                      hal)


if __name__ == "__main__":
    unittest.main()
