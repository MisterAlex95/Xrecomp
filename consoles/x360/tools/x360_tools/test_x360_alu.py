"""Instruction-level differential test of the Xenon lifter (XBA-270): about seventy integer and floating-point
instructions, each in its own function of a synthetic XEX2, lifted, built with the host cc and run on edge values and
random operands; every result is checked against a reference written here from the PowerPC architecture's definitions
as a title runs them: 64-bit registers and results in 32-bit mode (MSR[SF] = 0: CR0 from the low word, the carry out of
the low word; Book I), word instructions on the low word, rlw* replicating the low word, not from the runtime's C. Where the architecture leaves bits undefined
(the high word of mulhw / mulhwu / divw / divwu, of fctiw), only the defined bits are compared."""

import math
import random
import shutil
import struct
import subprocess
import sys
from fractions import Fraction

import pytest

from . import CONSOLE  # noqa: F401
from tools.xex.synth import build_xex
from tools.xex.xex import parse_xex

from .recomp import RUNTIME_DIRS, recompile

M64 = (1 << 64) - 1
M32 = 0xFFFFFFFF
TEXT = 0x82001000


def xo31(rd, ra, rb, xo, oe=0, rc=0):
    return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (oe << 10) | (xo << 1) | rc


def x31(rs, ra, rb, xo, rc=0):
    return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc


def d_form(op, rt, ra, imm):
    return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)


def md(xo, rs, ra, sh, mbe, rc=0):
    return (30 << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | ((mbe & 31) << 6) | ((mbe >> 5) << 5) | \
        (xo << 2) | ((sh >> 5) << 1) | rc


def mds(xo, rs, ra, rb, mbe, rc=0):
    return (30 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | ((mbe & 31) << 6) | ((mbe >> 5) << 5) | (xo << 1) | rc


def rlw(op, rs, ra, sh, mb, me, rc=0):
    return (op << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc


def fa(op, xo, d, a, b, c=0, rc=0):
    return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1) | rc


def fx(xo, d, a, b, rc=0):
    return (63 << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1) | rc


MFCR_R8 = x31(8, 0, 0, 19)
MFXER_R9 = (31 << 26) | (9 << 21) | (1 << 16) | (339 << 1)
BLR = 0x4E800020


def s64(v):
    v &= M64
    return v - (1 << 64) if v >> 63 else v


def s32(v):
    v &= M32
    return v - (1 << 32) if v >> 31 else v


def sext32(v):
    return s32(v) & M64


def rotl64(v, n):
    n &= 63
    return ((v << n) | (v >> (64 - n))) & M64 if n else v & M64


def mask64(mb, me):
    m1 = M64 >> mb
    m2 = (M64 << (63 - me)) & M64
    return (m1 & m2) if mb <= me else (m1 | m2)


def rotl32x2(v, n):
    w = v & M32
    return rotl64(w | (w << 32), n)


def cr_of(v):
    """CR0 of a record form: titles run in 32-bit mode, the low word compared with 0."""
    v = s32(v)
    return 8 if v < 0 else 4 if v > 0 else 2


def cmp_bits(a, b):
    return 8 if a < b else 4 if a > b else 2


def carry_add(*xs):
    """XER[CA] in 32-bit mode: the carry out of the low words."""
    return 1 if sum(x & M32 for x in xs) > M32 else 0


# ---- the integer instructions: (name, word, reference(a, b, c, ca) -> (r7, ca_out, cr_field_value, low32_only))
# r7 is the destination; inputs r3 = a, r4 = b, r7 = c before (for inserts), CA = ca. cr: CR0 for record forms, or
# a compare's field (crf 3); None: CR unchanged.

def int_cases():
    T = []

    def add(name, word, fn, rec=False, low=False):
        T.append((name, word, fn, rec, low))

    add("add", xo31(7, 3, 4, 266), lambda a, b, c, ca: ((a + b) & M64, ca))
    add("add.", xo31(7, 3, 4, 266, rc=1), lambda a, b, c, ca: ((a + b) & M64, ca), rec=True)
    add("addc", xo31(7, 3, 4, 10), lambda a, b, c, ca: ((a + b) & M64, carry_add(a, b)))
    add("adde", xo31(7, 3, 4, 138), lambda a, b, c, ca: ((a + b + ca) & M64, carry_add(a, b, ca)))
    add("addze", xo31(7, 3, 0, 202), lambda a, b, c, ca: ((a + ca) & M64, carry_add(a, ca)))
    add("addme", xo31(7, 3, 0, 234), lambda a, b, c, ca: ((a + ca + M64) & M64, carry_add(a, ca, M64)))
    add("subf", xo31(7, 3, 4, 40), lambda a, b, c, ca: ((b - a) & M64, ca))
    add("subfc", xo31(7, 3, 4, 8), lambda a, b, c, ca: ((b - a) & M64, carry_add(~a & M64, b, 1)))
    add("subfe", xo31(7, 3, 4, 136), lambda a, b, c, ca: ((~a + b + ca) & M64, carry_add(~a & M64, b, ca)))
    add("subfze", xo31(7, 3, 0, 200), lambda a, b, c, ca: ((~a + ca) & M64, carry_add(~a & M64, ca)))
    add("subfme", xo31(7, 3, 0, 232), lambda a, b, c, ca: ((~a + ca + M64) & M64, carry_add(~a & M64, ca, M64)))
    add("neg", xo31(7, 3, 0, 104), lambda a, b, c, ca: ((-a) & M64, ca))
    add("neg.", xo31(7, 3, 0, 104, rc=1), lambda a, b, c, ca: ((-a) & M64, ca), rec=True)
    add("mullw", xo31(7, 3, 4, 235), lambda a, b, c, ca: ((s32(a) * s32(b)) & M64, ca))
    add("mulhw", xo31(7, 3, 4, 75), lambda a, b, c, ca: (((s32(a) * s32(b)) >> 32) & M32, ca), low=True)
    add("mulhwu", xo31(7, 3, 4, 11), lambda a, b, c, ca: ((((a & M32) * (b & M32)) >> 32) & M32, ca), low=True)
    add("mulld", xo31(7, 3, 4, 233), lambda a, b, c, ca: ((a * b) & M64, ca))
    add("mulhd", xo31(7, 3, 4, 73), lambda a, b, c, ca: (((s64(a) * s64(b)) >> 64) & M64, ca))
    add("mulhdu", xo31(7, 3, 4, 9), lambda a, b, c, ca: (((a * b) >> 64) & M64, ca))
    add("divw", xo31(7, 3, 4, 491), lambda a, b, c, ca: (div_t(s32(a), s32(b), 32) & M32, ca), low=True)
    add("divwu", xo31(7, 3, 4, 459), lambda a, b, c, ca: (((a & M32) // (b & M32)) & M32 if b & M32 else 0, ca),
        low=True)
    add("divd", xo31(7, 3, 4, 489), lambda a, b, c, ca: (div_t(s64(a), s64(b), 64) & M64, ca))
    add("divdu", xo31(7, 3, 4, 457), lambda a, b, c, ca: ((a // b) & M64 if b else 0, ca))
    add("and", x31(3, 7, 4, 28), lambda a, b, c, ca: (a & b, ca))
    add("and.", x31(3, 7, 4, 28, 1), lambda a, b, c, ca: (a & b, ca), rec=True)
    add("or", x31(3, 7, 4, 444), lambda a, b, c, ca: (a | b, ca))
    add("xor", x31(3, 7, 4, 316), lambda a, b, c, ca: (a ^ b, ca))
    add("nand", x31(3, 7, 4, 476), lambda a, b, c, ca: (~(a & b) & M64, ca))
    add("nor", x31(3, 7, 4, 124), lambda a, b, c, ca: (~(a | b) & M64, ca))
    add("eqv", x31(3, 7, 4, 284), lambda a, b, c, ca: (~(a ^ b) & M64, ca))
    add("andc", x31(3, 7, 4, 60), lambda a, b, c, ca: (a & ~b & M64, ca))
    add("orc", x31(3, 7, 4, 412), lambda a, b, c, ca: ((a | ~b) & M64, ca))
    add("slw", x31(3, 7, 4, 24), lambda a, b, c, ca: (((a & M32) << (b & 63)) & M32 if not b & 32 else 0, ca))
    add("srw", x31(3, 7, 4, 536), lambda a, b, c, ca: ((a & M32) >> (b & 63) if not b & 32 else 0, ca))
    add("sraw", x31(3, 7, 4, 792), lambda a, b, c, ca: sraw(a, b & 63))
    add("sraw.", x31(3, 7, 4, 792, 1), lambda a, b, c, ca: sraw(a, b & 63), rec=True)
    for sh in (0, 1, 5, 31):
        add("srawi %d" % sh, x31(3, 7, sh, 824), lambda a, b, c, ca, sh=sh: sraw(a, sh))
    add("sld", x31(3, 7, 4, 27), lambda a, b, c, ca: ((a << (b & 127)) & M64 if not b & 64 else 0, ca))
    add("srd", x31(3, 7, 4, 539), lambda a, b, c, ca: (a >> (b & 127) if not b & 64 else 0, ca))
    add("srad", x31(3, 7, 4, 794), lambda a, b, c, ca: srad(a, b & 127))
    for sh in (0, 3, 32, 63):
        add("sradi %d" % sh, (31 << 26) | (3 << 21) | (7 << 16) | ((sh & 31) << 11) | (413 << 2) | ((sh >> 5) << 1),
            lambda a, b, c, ca, sh=sh: srad(a, sh))
    add("cntlzw", x31(3, 7, 0, 26), lambda a, b, c, ca: (32 - (a & M32).bit_length(), ca))
    add("cntlzd", x31(3, 7, 0, 58), lambda a, b, c, ca: (64 - a.bit_length(), ca))
    add("extsb", x31(3, 7, 0, 954), lambda a, b, c, ca: (((a & 0xFF) ^ 0x80) - 0x80 & M64, ca))
    add("extsh", x31(3, 7, 0, 922), lambda a, b, c, ca: (((a & 0xFFFF) ^ 0x8000) - 0x8000 & M64, ca))
    add("extsw", x31(3, 7, 0, 986), lambda a, b, c, ca: (sext32(a), ca))
    add("extsw.", x31(3, 7, 0, 986, 1), lambda a, b, c, ca: (sext32(a), ca), rec=True)
    for sh, mb, me in ((0, 0, 31), (4, 28, 3), (16, 16, 31), (31, 0, 0), (8, 24, 7)):
        add("rlwinm %d,%d,%d" % (sh, mb, me), rlw(21, 3, 7, sh, mb, me),
            lambda a, b, c, ca, sh=sh, mb=mb, me=me: (rotl32x2(a, sh) & mask64(mb + 32, me + 32), ca))
        add("rlwimi %d,%d,%d" % (sh, mb, me), rlw(20, 3, 7, sh, mb, me),
            lambda a, b, c, ca, sh=sh, mb=mb, me=me: ((rotl32x2(a, sh) & mask64(mb + 32, me + 32)) |
                                                      (c & ~mask64(mb + 32, me + 32) & M64), ca))
    add("rlwnm", rlw(23, 3, 7, 4, 8, 23), lambda a, b, c, ca: (rotl32x2(a, b & 31) & mask64(40, 55), ca))
    for sh, m in ((0, 32), (13, 0), (48, 16), (63, 63)):
        add("rldicl %d,%d" % (sh, m), md(0, 3, 7, sh, m), lambda a, b, c, ca, sh=sh, m=m: (rotl64(a, sh) &
                                                                                         mask64(m, 63), ca))
        add("rldicr %d,%d" % (sh, m), md(1, 3, 7, sh, m), lambda a, b, c, ca, sh=sh, m=m: (rotl64(a, sh) &
                                                                                         mask64(0, m), ca))
    for sh, mb in ((8, 8), (16, 0), (40, 4)):
        add("rldic %d,%d" % (sh, mb), md(2, 3, 7, sh, mb),
            lambda a, b, c, ca, sh=sh, mb=mb: (rotl64(a, sh) & mask64(mb, 63 - sh), ca))
        add("rldimi %d,%d" % (sh, mb), md(3, 3, 7, sh, mb),
            lambda a, b, c, ca, sh=sh, mb=mb: ((rotl64(a, sh) & mask64(mb, 63 - sh)) |
                                               (c & ~mask64(mb, 63 - sh) & M64), ca))
    add("rldcl", mds(8, 3, 7, 4, 16), lambda a, b, c, ca: (rotl64(a, b & 63) & mask64(16, 63), ca))
    add("rldcr", mds(9, 3, 7, 4, 40), lambda a, b, c, ca: (rotl64(a, b & 63) & mask64(0, 40), ca))
    add("addi", d_form(14, 7, 3, -1234), lambda a, b, c, ca: ((a - 1234) & M64, ca))
    add("addis", d_form(15, 7, 3, 0x8001), lambda a, b, c, ca: ((a + (s32(0x80010000))) & M64, ca))
    add("addic", d_form(12, 7, 3, -5), lambda a, b, c, ca: ((a - 5) & M64, carry_add(a, (-5) & M64)))
    add("addic.", d_form(13, 7, 3, 7), lambda a, b, c, ca: ((a + 7) & M64, carry_add(a, 7)), rec=True)
    add("subfic", d_form(8, 7, 3, 100), lambda a, b, c, ca: ((100 - a) & M64, carry_add(~a & M64, 100, 1)))
    add("mulli", d_form(7, 7, 3, -3), lambda a, b, c, ca: ((s64(a) * -3) & M64, ca))
    add("ori", d_form(24, 3, 7, 0x8001), lambda a, b, c, ca: (a | 0x8001, ca))
    add("oris", d_form(25, 3, 7, 0x8001), lambda a, b, c, ca: (a | 0x80010000, ca))
    add("xori", d_form(26, 3, 7, 0xF0F0), lambda a, b, c, ca: (a ^ 0xF0F0, ca))
    add("xoris", d_form(27, 3, 7, 0xF0F0), lambda a, b, c, ca: (a ^ 0xF0F00000, ca))
    add("andi.", d_form(28, 3, 7, 0x8FF1), lambda a, b, c, ca: (a & 0x8FF1, ca), rec=True)
    add("andis.", d_form(29, 3, 7, 0x8FF1), lambda a, b, c, ca: (a & 0x8FF10000, ca), rec=True)
    return T


def div_t(a, b, bits):
    if b == 0 or (a == -(1 << (bits - 1)) and b == -1):
        return 0  # undefined; the runtime's choice is 0 (checked so a change is noticed)
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def sraw(a, n):
    w = s32(a)
    if n & 32:
        r = -1 if w < 0 else 0
        return r & M64, 1 if w < 0 and (a & M32) else 0
    r = w >> n
    ca = 1 if w < 0 and (w & ((1 << n) - 1)) else 0
    return r & M64, ca


def srad(a, n):
    v = s64(a)
    if n & 64:
        return (-1 if v < 0 else 0) & M64, 1 if v < 0 else 0
    r = v >> n
    return r & M64, 1 if v < 0 and (v & ((1 << n) - 1)) else 0


CMP_CASES = [  # name, word (into CR field 3), reference(a, b) -> field
    ("cmpw", x31(3 << 2, 3, 4, 0), lambda a, b: cmp_bits(s32(a), s32(b))),
    ("cmpd", x31((3 << 2) | 1, 3, 4, 0), lambda a, b: cmp_bits(s64(a), s64(b))),
    ("cmplw", x31(3 << 2, 3, 4, 32), lambda a, b: cmp_bits(a & M32, b & M32)),
    ("cmpld", x31((3 << 2) | 1, 3, 4, 32), lambda a, b: cmp_bits(a, b)),
    ("cmpwi", d_form(11, 3 << 2, 3, -7), lambda a, b: cmp_bits(s32(a), -7)),
    ("cmpdi", d_form(11, (3 << 2) | 1, 3, -7), lambda a, b: cmp_bits(s64(a), -7)),
    ("cmplwi", d_form(10, 3 << 2, 3, 0x8000), lambda a, b: cmp_bits(a & M32, 0x8000)),
    ("cmpldi", d_form(10, (3 << 2) | 1, 3, 0x8000), lambda a, b: cmp_bits(a, 0x8000)),
]


# ---- floating point (f1, f2, f3 -> f7; RN, no exceptions enabled)

def d2u(x):
    return struct.unpack(">Q", struct.pack(">d", x))[0]


def to_single(fr):
    """Round an exact rational to the nearest single (ties to even), as a double."""
    if fr == 0:
        return 0.0
    sign = -1 if fr < 0 else 1
    a = abs(fr)
    e = math.floor(math.log2(a.numerator) - math.log2(a.denominator))
    while Fraction(2) ** e > a:
        e -= 1
    while Fraction(2) ** (e + 1) <= a:
        e += 1
    e = max(e, -126)
    scaled = a / Fraction(2) ** (e - 23)
    q, r = divmod(scaled.numerator, scaled.denominator)
    if 2 * r > scaled.denominator or (2 * r == scaled.denominator and q & 1):
        q += 1
    return sign * float(Fraction(q) * Fraction(2) ** (e - 23))


def fround_d(fr, z=0.0):
    """An exact result rounded to double; z: the IEEE zero (its sign) when the exact result is zero."""
    return float(fr) if fr != 0 else z


def zs(fr, z):
    return to_single(fr) if fr != 0 else z


FP_CASES = [  # name, word, reference(a, b, c) -> double, single-rounded inputs only
    ("fadd", fa(63, 21, 7, 1, 2), lambda a, b, c: fround_d(Fraction(a) + Fraction(b), a + b)),
    ("fsub", fa(63, 20, 7, 1, 2), lambda a, b, c: fround_d(Fraction(a) - Fraction(b), a - b)),
    ("fmul", fa(63, 25, 7, 1, 0, 3), lambda a, b, c: fround_d(Fraction(a) * Fraction(c), a * c)),
    ("fdiv", fa(63, 18, 7, 1, 2), lambda a, b, c: fround_d(Fraction(a) / Fraction(b), a / b)),
    ("fmadd", fa(63, 29, 7, 1, 2, 3), lambda a, b, c: fround_d(Fraction(a) * Fraction(c) + Fraction(b), a * c + b)),
    ("fmsub", fa(63, 28, 7, 1, 2, 3), lambda a, b, c: fround_d(Fraction(a) * Fraction(c) - Fraction(b), a * c - b)),
    ("fnmadd", fa(63, 31, 7, 1, 2, 3), lambda a, b, c: -fround_d(Fraction(a) * Fraction(c) + Fraction(b), a * c + b)),
    ("fnmsub", fa(63, 30, 7, 1, 2, 3), lambda a, b, c: -fround_d(Fraction(a) * Fraction(c) - Fraction(b), a * c - b)),
    ("fadds", fa(59, 21, 7, 1, 2), lambda a, b, c: zs(Fraction(a) + Fraction(b), a + b)),
    ("fsubs", fa(59, 20, 7, 1, 2), lambda a, b, c: zs(Fraction(a) - Fraction(b), a - b)),
    ("fmuls", fa(59, 25, 7, 1, 0, 3), lambda a, b, c: zs(Fraction(a) * Fraction(c), a * c)),
    ("fdivs", fa(59, 18, 7, 1, 2), lambda a, b, c: zs(Fraction(a) / Fraction(b), a / b)),
    ("fmadds", fa(59, 29, 7, 1, 2, 3), lambda a, b, c: zs(Fraction(a) * Fraction(c) + Fraction(b), a * c + b)),
    ("fnmsubs", fa(59, 30, 7, 1, 2, 3), lambda a, b, c: -zs(Fraction(a) * Fraction(c) - Fraction(b), a * c - b)),
    ("fsqrt", fa(63, 22, 7, 0, 2), lambda a, b, c: math.sqrt(b) if b >= 0 else None),
    ("frsp", fx(12, 7, 0, 2), lambda a, b, c: zs(Fraction(b), b)),
    ("fsel", fa(63, 23, 7, 1, 2, 3), lambda a, b, c: c if a >= 0 else b),
    ("fabs", fx(264, 7, 0, 2), lambda a, b, c: abs(b)),
    ("fnabs", fx(136, 7, 0, 2), lambda a, b, c: -abs(b)),
    ("fneg", fx(40, 7, 0, 2), lambda a, b, c: -b),
    ("fmr", fx(72, 7, 0, 2), lambda a, b, c: b),
]
FCONV = [  # name, word, reference(b) -> 64-bit integer pattern compared (mask)
    ("fctiwz", fx(15, 7, 0, 2), lambda b: (sat(math.trunc(b), 32) & M32, M32)),
    ("fctiw", fx(14, 7, 0, 2), lambda b: (sat(round_even(b), 32) & M32, M32)),
    ("fctidz", fx(815, 7, 0, 2), lambda b: (sat(math.trunc(b), 64) & M64, M64)),
    ("fctid", fx(814, 7, 0, 2), lambda b: (sat(round_even(b), 64) & M64, M64)),
]


def round_even(x):
    return int(Fraction(x).__round__())  # Fraction rounds half to even


def sat(v, bits):
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    return min(max(v, lo), hi)


EDGE64 = [0, 1, M64, 0x7FFFFFFF, 0x80000000, M32, 0xFFFFFFFF80000000, 0x8000000000000000, 0x7FFFFFFFFFFFFFFF,
          0x00000001FFFFFFFF, 0x123456789ABCDEF0, 0xFEDCBA9876543210, 31, 32, 63, 64, 65, 127]


def int_inputs(rng):
    vals = list(EDGE64)
    vals += [rng.getrandbits(64) for _ in range(6)] + [rng.getrandbits(32) for _ in range(3)]
    pairs = [(a, b) for a in vals[:12] for b in vals[:12]]
    pairs += [(rng.choice(vals), rng.choice(vals)) for _ in range(40)]
    return pairs


HARNESS = r"""
#include <stdio.h>
#include "xenon_rt.h"
extern const xe_rc_entry xe_rc_resume[];
extern const unsigned xe_rc_resume_count;
static uint8_t mem[0x1000];
static xe_machine M;
static int run(uint32_t entry)
{
    M.cpu.pc = entry;
    M.cpu.lr = 0xDEAD0000u;
    for (int n = 0; n < 100; n++) {
        if (M.cpu.pc == 0xDEAD0000u)
            return 0;
        xe_rc_fn f = NULL;
        for (unsigned k = 0; k < xe_rc_resume_count; k++)
            if (xe_rc_resume[k].entry == M.cpu.pc)
                f = xe_rc_resume[k].fn;
        if (!f)
            return 2;
        M.rc_exit = 0;
        f(&M);
        if (M.rc_exit)
            return 10 + M.rc_exit;
    }
    return 3;
}
static void ci(uint32_t e, uint64_t a, uint64_t b, uint64_t c, uint32_t ca)
{
    memset(&M.cpu, 0, sizeof M.cpu);
    M.cpu.gpr[3] = a, M.cpu.gpr[4] = b, M.cpu.gpr[7] = c, M.cpu.xer_ca = ca;
    int rc = run(e);
    printf("%%d %%016llx %%08x %%08x\n", rc, (unsigned long long)M.cpu.gpr[7], (unsigned)M.cpu.gpr[8],
           (unsigned)M.cpu.gpr[9]);
}
static void cf(uint32_t e, uint64_t a, uint64_t b, uint64_t c)
{
    memset(&M.cpu, 0, sizeof M.cpu);
    memcpy(&M.cpu.fpr[1], &a, 8), memcpy(&M.cpu.fpr[2], &b, 8), memcpy(&M.cpu.fpr[3], &c, 8);
    int rc = run(e);
    uint64_t r;
    memcpy(&r, &M.cpu.fpr[7], 8);
    printf("%%d %%016llx\n", rc, (unsigned long long)r);
}
int main(void)
{
    M.mem = mem;
    M.mem_base = 0x40000000u;
    M.mem_size = sizeof mem;
%(calls)s
    return 0;
}
"""


@pytest.mark.skipif(not shutil.which("cc") or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_alu_against_the_architecture(tmp_path):
    rng = random.Random(270)
    icases = int_cases()
    words, funcs = [], []
    for name, word, *_ in icases:
        funcs.append((4 * len(words), 16))
        words += [word, MFCR_R8, MFXER_R9, BLR]
    for name, word, _ in CMP_CASES:
        funcs.append((4 * len(words), 16))
        words += [word, MFCR_R8, MFXER_R9, BLR]
    fbase = len(funcs)
    for name, word, _ in FP_CASES + FCONV:
        funcs.append((4 * len(words), 8))
        words += [word, BLR]
    data, _ = build_xex(words, functions=funcs, compression="none")
    img = parse_xex(data)
    s = recompile(img, str(tmp_path / "src"))
    assert s["interp_fallback_insns"] == 0, s["unlifted"]
    calls, checks = [], []
    pairs = int_inputs(rng)
    for k, (name, word, fn, rec, low) in enumerate(icases):
        entry = TEXT + funcs[k][0]
        for a, b in pairs:
            c = rng.getrandbits(64)
            ca = rng.getrandbits(1)
            if name.startswith(("divw", "divd")) and (b == 0 or (b & M32) == 0):
                continue  # undefined: skipped
            if name in ("divw",) and s32(a) == -(1 << 31) and s32(b) == -1:
                continue
            if name in ("divd",) and s64(a) == -(1 << 63) and s64(b) == -1:
                continue
            r, ca_out = fn(a, b, c, ca)
            cr = (cr_of(r) << 28) if rec else 0
            calls.append("    ci(0x%08Xu, 0x%016XULL, 0x%016XULL, 0x%016XULL, %d);" % (entry, a, b, c, ca))
            checks.append((name, a, b, ca, r, ca_out, cr, low))
    for j, (name, word, fn) in enumerate(CMP_CASES):
        entry = TEXT + funcs[len(icases) + j][0]
        for a, b in pairs:
            calls.append("    ci(0x%08Xu, 0x%016XULL, 0x%016XULL, 0, 0);" % (entry, a, b))
            checks.append((name, a, b, 0, None, 0, fn(a, b) << (28 - 4 * 3), False))
    fvals = [0.0, -0.0, 1.0, -1.5, 2.5, 3.5, 0.1, 1e10, -7.25, 1e-3, 123456.789, 2.0 ** 31, -2.0 ** 31 - 1, 2.0 ** 52,
             0.5, -0.5, 1.5]
    fvals = [to_single(Fraction(v)) for v in fvals] + [rng.uniform(-1000, 1000) for _ in range(6)]
    fvals_single = [to_single(Fraction(v)) for v in fvals]
    for j, (name, word, fn) in enumerate(FP_CASES):
        entry = TEXT + funcs[fbase + j][0]
        vals = fvals_single
        for _ in range(60):
            a, b, c = rng.choice(vals), rng.choice(vals), rng.choice(vals)
            if name.startswith("fdiv") and b == 0:
                continue
            want = fn(a, b, c)
            if want is None:
                continue
            calls.append("    cf(0x%08Xu, 0x%016XULL, 0x%016XULL, 0x%016XULL);" % (entry, d2u(a), d2u(b), d2u(c)))
            checks.append((name, a, b, c, d2u(want), None, None, "fp"))
    for j, (name, word, fn) in enumerate(FCONV):
        entry = TEXT + funcs[fbase + len(FP_CASES) + j][0]
        for b in fvals + [2.0 ** 63, -2.0 ** 63, 1e30, -1e30, 2.5, -2.5, 0.5]:
            want, m = fn(b)
            calls.append("    cf(0x%08Xu, 0, 0x%016XULL, 0);" % (entry, d2u(b)))
            checks.append((name, None, b, None, want, m, None, "conv"))
    harness = tmp_path / "h.c"
    harness.write_text(HARNESS % {"calls": "\n".join(calls)})
    inc = []
    for d in RUNTIME_DIRS:
        inc += ["-I", d]
    exe = str(tmp_path / "t")
    subprocess.run(["cc", "-std=gnu11", "-O1", "-w", "-ffp-contract=off"] + inc + ["-o", exe, str(harness)] +
                   sorted(str(p) for p in (tmp_path / "src").glob("*.c")) + ["-lm"], check=True)
    out = subprocess.run([exe], capture_output=True, text=True, timeout=120).stdout.splitlines()
    assert len(out) == len(checks)
    bad = []
    for line, (name, a, b, ca, want, ca_out, cr, kind) in zip(out, checks):
        f = line.split()
        if f[0] != "0":
            bad.append((name, "rc %s" % f[0]))
            continue
        if kind == "fp":
            if int(f[1], 16) != want:
                bad.append((name, a, b, ca, "got %s want %016X" % (f[1], want)))
            continue
        if kind == "conv":
            if int(f[1], 16) & ca_out != want:
                bad.append((name, b, "got %s want %X" % (f[1], want)))
            continue
        got, crv, xer = int(f[1], 16), int(f[2], 16), int(f[3], 16)
        if want is not None:
            m = M32 if kind else M64
            if got & m != want & m:
                bad.append((name, hex(a), hex(b), ca, "r %016X want %016X" % (got, want)))
        if ca_out is not None and (xer >> 29) & 1 != ca_out:
            bad.append((name, hex(a), hex(b), ca, "CA %d want %d" % ((xer >> 29) & 1, ca_out)))
        if crv != cr:
            bad.append((name, hex(a), hex(b), "CR %08X want %08X" % (crv, cr)))
    assert not bad, "%d mismatches, first: %s" % (len(bad), bad[:12])
