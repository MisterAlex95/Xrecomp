"""Tests of the rest of VMX / VMX128 (XBA-270): the integer element ops, saturating packs and sums, unpacks, the
float estimates, non-Java mode (VSCR[NJ]: denormals flushed) and the D3D pack ops vpkd3d128 / vupkd3d128.

Each instruction under test is one leaf function in a synthetic XEX2 (tools/xex/synth.py): it loads vA, vB, vC, vD
and VSCR from memory, runs the instruction, stores vD and VSCR (mfvscr) and returns CR in r3. The functions are
lifted, built with the host cc against consoles/x360/runtime and run on several inputs; every expected value is
computed here in Python from the instruction definitions (AltiVec PEM; for vpkd3d128 / vupkd3d128 Xenia's
description of them, BSD, read for the semantics), never from the runtime's C. No toolchain, no console data."""

import math
import random
import shutil
import struct
import subprocess
import sys

import pytest

from . import CONSOLE  # noqa: F401
from tools.xex.synth import build_xex
from tools.xex.xex import parse_xex

from . import asm as A
from .recomp import RUNTIME_DIRS, recompile
from .test_x360_tools import HARNESS, MEM, MEM_SIZE, c_case, parse_out
from .xenon import OP5, OP6, OP6_3, OP6_R, VA4, VC4, VX4, VX128_1, X31_VMX, XenonLifter

NJ = 0x00010000
SAT = 1

# registers: classic AltiVec ops use v1..v4, VMX128 ops registers past v31 (split fields)
CLASSIC = dict(a=1, b=2, c=3, d=4)
WIDE = dict(a=70, b=100, c=3, d=33)


# ---------------------------------------------------------------- element views

def u8s(v): return list(v)
def u16s(v): return list(struct.unpack(">8H", v))
def u32s(v): return list(struct.unpack(">4I", v))
def from8(xs): return bytes(x & 0xFF for x in xs)
def from16(xs): return struct.pack(">8H", *[x & 0xFFFF for x in xs])
def from32(xs): return struct.pack(">4I", *[x & 0xFFFFFFFF for x in xs])
def s8(x): return x - 256 if x & 0x80 else x
def s16(x): return x - 65536 if x & 0x8000 else x
def s32(x): return x - (1 << 32) if x & 0x80000000 else x


class Sat:
    """Clamps, remembering whether any did (VSCR[SAT])."""

    def __init__(self):
        self.hit = False

    def __call__(self, v, lo, hi):
        if v < lo or v > hi:
            self.hit = True
        return min(max(v, lo), hi)


# ---------------------------------------------------------------- references (from the PEM definitions)

def ew(view, pack, fn):
    """An element-wise op: fn(x, y, sat) on unsigned elements."""
    def ref(a, b, c, d, st):
        return pack([fn(x, y, st) for x, y in zip(view(a), view(b))])
    return ref


def satop(view, pack, sgn, width, op):
    lo, hi = (-(1 << (width - 1)), (1 << (width - 1)) - 1) if sgn else (0, (1 << width) - 1)
    cv = {8: s8, 16: s16, 32: s32}[width] if sgn else (lambda x: x)
    return ew(view, pack, lambda x, y, st: st(op(cv(x), cv(y)), lo, hi))


def rot(x, n, w):
    n %= w
    return ((x << n) | (x >> (w - n))) & ((1 << w) - 1) if n else x


def merge(view, pack, lo):
    def ref(a, b, c, d, st):
        xa, xb = view(a), view(b)
        h = len(xa) // 2
        xa, xb = (xa[h:], xb[h:]) if lo else (xa[:h], xb[:h])
        return pack([v for pair in zip(xa, xb) for v in pair])
    return ref


def mul(view, pack, odd, sgn, cv):
    def ref(a, b, c, d, st):
        xa, xb = view(a), view(b)
        return pack([(cv(xa[k]) if sgn else xa[k]) * (cv(xb[k]) if sgn else xb[k])
                     for k in range(odd, len(xa), 2)])
    return ref


def pack_h(mode):
    def ref(a, b, c, d, st):
        out = []
        for x in u16s(a) + u16s(b):
            out.append(x & 0xFF if mode == 0 else st(x, 0, 255) if mode == 1 else
                       st(s16(x), 0, 255) if mode == 2 else st(s16(x), -128, 127))
        return from8(out)
    return ref


def pack_w(mode):
    def ref(a, b, c, d, st):
        out = []
        for x in u32s(a) + u32s(b):
            out.append(x & 0xFFFF if mode == 0 else st(x, 0, 65535) if mode == 1 else
                       st(s32(x), 0, 65535) if mode == 2 else st(s32(x), -32768, 32767))
        return from16(out)
    return ref


def ref_pkpx(a, b, c, d, st):
    out = []
    for w in u32s(a) + u32s(b):
        # PEM: halfword = bit 7 || bits 8:12 || bits 16:20 || bits 24:28 (bit 0 = most significant)
        bit = lambda n: (w >> (31 - n)) & 1  # noqa: E731
        field = lambda lo, hi: sum(bit(n) << (hi - n) for n in range(lo, hi + 1))  # noqa: E731
        out.append(bit(7) << 15 | field(8, 12) << 10 | field(16, 20) << 5 | field(24, 28))
    return from16(out)


def unpack(view, pack, lo, cv):
    def ref(a, b, c, d, st):
        x = view(b)
        h = len(x) // 2
        return pack([cv(v) for v in (x[h:] if lo else x[:h])])
    return ref


def ref_upkpx(lo):
    def ref(a, b, c, d, st):
        out = []
        for h in (u16s(b)[4:] if lo else u16s(b)[:4]):
            out.append((0xFF if h >> 15 else 0) << 24 | ((h >> 10) & 31) << 16 | ((h >> 5) & 31) << 8 | (h & 31))
        return from32(out)
    return ref


def ref_sum4(kind):
    def ref(a, b, c, d, st):
        out = []
        for k, w in enumerate(u32s(b)):
            if kind == "ubs":
                out.append(st(w + sum(u8s(a)[4 * k:4 * k + 4]), 0, (1 << 32) - 1))
            elif kind == "sbs":
                out.append(st(s32(w) + sum(s8(x) for x in u8s(a)[4 * k:4 * k + 4]), -(1 << 31), (1 << 31) - 1))
            else:
                out.append(st(s32(w) + sum(s16(x) for x in u16s(a)[2 * k:2 * k + 2]), -(1 << 31), (1 << 31) - 1))
        return from32(out)
    return ref


def ref_sum2sws(a, b, c, d, st):
    xa, xb = [s32(x) for x in u32s(a)], [s32(x) for x in u32s(b)]
    lim = (-(1 << 31), (1 << 31) - 1)
    return from32([0, st(xa[0] + xa[1] + xb[1], *lim), 0, st(xa[2] + xa[3] + xb[3], *lim)])


def ref_sumsws(a, b, c, d, st):
    return from32([0, 0, 0, st(sum(s32(x) for x in u32s(a)) + s32(u32s(b)[3]), -(1 << 31), (1 << 31) - 1)])


def ref_mhadd(rnd):
    def ref(a, b, c, d, st):
        return from16([st(((s16(x) * s16(y) + (0x4000 if rnd else 0)) >> 15) + s16(z), -32768, 32767)
                       for x, y, z in zip(u16s(a), u16s(b), u16s(c))])
    return ref


def ref_mladduhm(a, b, c, d, st):
    return from16([x * y + z for x, y, z in zip(u16s(a), u16s(b), u16s(c))])


def ref_msumb(sgn):
    def ref(a, b, c, d, st):
        xa, xb = u8s(a), u8s(b)
        return from32([w + sum((s8(xa[4 * k + j]) if sgn else xa[4 * k + j]) * xb[4 * k + j] for j in range(4))
                       for k, w in enumerate(u32s(c))])
    return ref


def ref_msumh(sgn, sat):
    def ref(a, b, c, d, st):
        xa, xb = u16s(a), u16s(b)
        out = []
        for k, w in enumerate(u32s(c)):
            if sgn:
                s = s32(w) + sum(s16(xa[2 * k + j]) * s16(xb[2 * k + j]) for j in range(2))
                out.append(st(s, -(1 << 31), (1 << 31) - 1) if sat else s)
            else:
                s = w + sum(xa[2 * k + j] * xb[2 * k + j] for j in range(2))
                out.append(st(s, 0, (1 << 32) - 1) if sat else s)
        return from32(out)
    return ref


def ref_shift_bits(left):
    def ref(a, b, c, d, st):
        sh = b[15] & 7
        v = int.from_bytes(a, "big")
        v = (v << sh) if left else (v >> sh)
        return (v & ((1 << 128) - 1)).to_bytes(16, "big")
    return ref


INT_OPS = {
    # name: (encoder (vd, va, vb, vc) -> word, reference)
    "vaddubm": ew(u8s, from8, lambda x, y, st: x + y),
    "vadduhm": ew(u16s, from16, lambda x, y, st: x + y),
    "vsububm": ew(u8s, from8, lambda x, y, st: x - y),
    "vsubuhm": ew(u16s, from16, lambda x, y, st: x - y),
    "vaddubs": satop(u8s, from8, False, 8, lambda x, y: x + y),
    "vadduhs": satop(u16s, from16, False, 16, lambda x, y: x + y),
    "vadduws": satop(u32s, from32, False, 32, lambda x, y: x + y),
    "vaddsbs": satop(u8s, from8, True, 8, lambda x, y: x + y),
    "vaddshs": satop(u16s, from16, True, 16, lambda x, y: x + y),
    "vaddsws": satop(u32s, from32, True, 32, lambda x, y: x + y),
    "vsububs": satop(u8s, from8, False, 8, lambda x, y: x - y),
    "vsubuhs": satop(u16s, from16, False, 16, lambda x, y: x - y),
    "vsubuws": satop(u32s, from32, False, 32, lambda x, y: x - y),
    "vsubsbs": satop(u8s, from8, True, 8, lambda x, y: x - y),
    "vsubshs": satop(u16s, from16, True, 16, lambda x, y: x - y),
    "vsubsws": satop(u32s, from32, True, 32, lambda x, y: x - y),
    "vaddcuw": ew(u32s, from32, lambda x, y, st: (x + y) >> 32),
    "vsubcuw": ew(u32s, from32, lambda x, y, st: 1 if x >= y else 0),
    "vmaxub": ew(u8s, from8, lambda x, y, st: max(x, y)),
    "vmaxuh": ew(u16s, from16, lambda x, y, st: max(x, y)),
    "vmaxuw": ew(u32s, from32, lambda x, y, st: max(x, y)),
    "vmaxsb": ew(u8s, from8, lambda x, y, st: max(s8(x), s8(y))),
    "vmaxsh": ew(u16s, from16, lambda x, y, st: max(s16(x), s16(y))),
    "vminub": ew(u8s, from8, lambda x, y, st: min(x, y)),
    "vminuh": ew(u16s, from16, lambda x, y, st: min(x, y)),
    "vminuw": ew(u32s, from32, lambda x, y, st: min(x, y)),
    "vminsb": ew(u8s, from8, lambda x, y, st: min(s8(x), s8(y))),
    "vminsh": ew(u16s, from16, lambda x, y, st: min(s16(x), s16(y))),
    "vavgub": ew(u8s, from8, lambda x, y, st: (x + y + 1) >> 1),
    "vavguh": ew(u16s, from16, lambda x, y, st: (x + y + 1) >> 1),
    "vavguw": ew(u32s, from32, lambda x, y, st: (x + y + 1) >> 1),
    "vavgsb": ew(u8s, from8, lambda x, y, st: (s8(x) + s8(y) + 1) >> 1),
    "vavgsh": ew(u16s, from16, lambda x, y, st: (s16(x) + s16(y) + 1) >> 1),
    "vavgsw": ew(u32s, from32, lambda x, y, st: (s32(x) + s32(y) + 1) >> 1),
    "vrlb": ew(u8s, from8, lambda x, y, st: rot(x, y & 7, 8)),
    "vrlh": ew(u16s, from16, lambda x, y, st: rot(x, y & 15, 16)),
    "vslb": ew(u8s, from8, lambda x, y, st: x << (y & 7)),
    "vslh": ew(u16s, from16, lambda x, y, st: x << (y & 15)),
    "vsrb": ew(u8s, from8, lambda x, y, st: x >> (y & 7)),
    "vsrh": ew(u16s, from16, lambda x, y, st: x >> (y & 15)),
    "vsrab": ew(u8s, from8, lambda x, y, st: s8(x) >> (y & 7)),
    "vsrah": ew(u16s, from16, lambda x, y, st: s16(x) >> (y & 15)),
    "vsl": ref_shift_bits(True),
    "vsr": ref_shift_bits(False),
    "vmrghb": merge(u8s, from8, False),
    "vmrglb": merge(u8s, from8, True),
    "vmrghh": merge(u16s, from16, False),
    "vmrglh": merge(u16s, from16, True),
    "vmuleub": mul(u8s, from16, 0, False, s8),
    "vmuloub": mul(u8s, from16, 1, False, s8),
    "vmulesb": mul(u8s, from16, 0, True, s8),
    "vmulosb": mul(u8s, from16, 1, True, s8),
    "vmuleuh": mul(u16s, from32, 0, False, s16),
    "vmulouh": mul(u16s, from32, 1, False, s16),
    "vmulesh": mul(u16s, from32, 0, True, s16),
    "vmulosh": mul(u16s, from32, 1, True, s16),
    "vpkuhum": pack_h(0), "vpkuhus": pack_h(1), "vpkshus": pack_h(2), "vpkshss": pack_h(3),
    "vpkuwum": pack_w(0), "vpkuwus": pack_w(1), "vpkswus": pack_w(2), "vpkswss": pack_w(3),
    "vpkpx": ref_pkpx,
    "vupkhsb": unpack(u8s, from16, False, s8), "vupklsb": unpack(u8s, from16, True, s8),
    "vupkhsh": unpack(u16s, from32, False, s16), "vupklsh": unpack(u16s, from32, True, s16),
    "vupkhpx": ref_upkpx(False), "vupklpx": ref_upkpx(True),
    "vsum4ubs": ref_sum4("ubs"), "vsum4sbs": ref_sum4("sbs"), "vsum4shs": ref_sum4("shs"),
    "vsum2sws": ref_sum2sws, "vsumsws": ref_sumsws,
    "vmhaddshs": ref_mhadd(False), "vmhraddshs": ref_mhadd(True), "vmladduhm": ref_mladduhm,
    "vmsumubm": ref_msumb(False), "vmsummbm": ref_msumb(True),
    "vmsumuhm": ref_msumh(False, False), "vmsumuhs": ref_msumh(False, True),
    "vmsumshm": ref_msumh(True, False), "vmsumshs": ref_msumh(True, True),
}
CMP_OPS = {
    "vcmpequb": ew(u8s, from8, lambda x, y, st: 0xFF if x == y else 0),
    "vcmpequh": ew(u16s, from16, lambda x, y, st: 0xFFFF if x == y else 0),
    "vcmpgtub": ew(u8s, from8, lambda x, y, st: 0xFF if x > y else 0),
    "vcmpgtuh": ew(u16s, from16, lambda x, y, st: 0xFFFF if x > y else 0),
    "vcmpgtsb": ew(u8s, from8, lambda x, y, st: 0xFF if s8(x) > s8(y) else 0),
    "vcmpgtsh": ew(u16s, from16, lambda x, y, st: 0xFFFF if s16(x) > s16(y) else 0),
}
# VMX128 forms of the packs / unpacks (the same references)
WIDE_OPS = {"vpkshss128": "vpkshss", "vpkshus128": "vpkshus", "vpkswss128": "vpkswss", "vpkswus128": "vpkswus",
            "vpkuhum128": "vpkuhum", "vpkuhus128": "vpkuhus", "vpkuwum128": "vpkuwum", "vpkuwus128": "vpkuwus",
            "vupkhsb128": "vupkhsb", "vupklsb128": "vupklsb",
            "vupkhsh128": "vupkhsh", "vupklsh128": "vupklsh"}  # the byte opcodes with VA = 0x60

VX_XO = {v: k for k, v in VX4.items()}
VC_XO = {v: k for k, v in VC4.items()}
VA_XO = {v: k for k, v in VA4.items()}
OP5_XO = {v: k for k, v in OP5.items()}
OP6_XO = {v: k for k, v in OP6.items()}


def encode(name, r, rc=0):
    if name in VA_XO:
        return A._va(VA_XO[name], r["d"], r["a"], r["b"], r["c"])
    if name in VC_XO:
        return A._vx(VC_XO[name] | (rc << 10), r["d"], r["a"], r["b"])
    if name in ("vupkhsb", "vupklsb", "vupkhsh", "vupklsh", "vupkhpx", "vupklpx"):
        return A._vx(VX_XO[name], r["d"], 0, r["b"])
    if name in VX_XO:
        return A._vx(VX_XO[name], r["d"], r["a"], r["b"])
    if name in OP5_XO:
        return A._op5(OP5_XO[name], r["d"], r["a"], r["b"])
    if name in ("vupkhsh128", "vupklsh128"):
        return A._op6(OP6_XO[name.replace("sh128", "sb128")], r["d"], 0x60, r["b"])
    if name in OP6_XO:
        return A._op6(OP6_XO[name], r["d"], 0, r["b"])
    raise KeyError(name)


# ---------------------------------------------------------------- floats

def f32(x):
    return struct.unpack(">f", struct.pack(">f", x))[0]


def fbits(x):
    return struct.unpack(">I", struct.pack(">f", x))[0]


def bitsf(u):
    return struct.unpack(">f", struct.pack(">I", u))[0]


def daz(u, nj):
    return u & 0x80000000 if nj and (u & 0x7F800000) == 0 else u


def fl(xs):
    return from32([fbits(x) if isinstance(x, float) else x for x in xs])


def fref(fn, nargs):
    """A float op: inputs and result flushed in non-Java mode (VSCR[NJ] in st.vscr)."""
    def ref(a, b, c, d, st):
        nj = st.vscr & NJ
        cols = [u32s(v) for v in (a, b, c, d)[:nargs]]
        out = []
        for k in range(4):
            args = [bitsf(daz(col[k], nj)) for col in cols]
            out.append(daz(fbits(fn(*args)), nj))
        return from32(out)
    return ref


def _fmax(x, y):
    """PEM: +0 is greater than -0."""
    if x == y:
        return x if math.copysign(1, x) > 0 else y
    return max(x, y)


def _fmin(x, y):
    if x == y:
        return x if math.copysign(1, x) < 0 else y
    return min(x, y)


def _fmaf(x, y, z):
    from fractions import Fraction
    return f32(float(Fraction(x) * Fraction(y) + Fraction(z)))


DENORM = 0x00012345                # a positive denormal
NDENORM = 0x80000F00               # a negative one
TINY = 1.5 * 2.0 ** -126           # normal; TINY - 2^-126 is a denormal result


# ---------------------------------------------------------------- D3D pack / unpack (Xenia's description)

def biased(n):
    return 0x40400000 + n


def _clamp(u, lo, hi):
    f = bitsf(u)
    if not f >= bitsf(lo):
        return lo
    return hi if f > bitsf(hi) else u


def _f2h_rtz(u):
    """float -> half rounding toward zero, from the IEEE formats (overflow: largest finite)."""
    f = bitsf(u)
    sign = 0x8000 if u >> 31 else 0
    if f != f:
        return sign | 0x7E00 | ((u >> 13) & 0x1FF)
    if math.isinf(f):
        return sign | 0x7C00
    a = abs(f)
    if a >= 65520.0 or a > 65504.0:
        return sign | 0x7BFF
    if a < 2.0 ** -14:                       # half denormal: units of 2^-24, truncated
        return sign | int(a / 2.0 ** -24)
    e = math.floor(math.log2(a))
    if 2.0 ** e > a:
        e -= 1
    m = int((a / 2.0 ** e - 1.0) * 1024)     # truncated 10-bit fraction (exact in binary)
    return sign | (e + 15) << 10 | m


def _h2f(h):
    sign = -1.0 if h & 0x8000 else 1.0
    e, m = (h >> 10) & 31, h & 0x3FF
    if e == 31:
        return (0xFF800000 if h & 0x8000 else 0x7F800000) | (m << 13)
    v = sign * (m * 2.0 ** -24 if e == 0 else (1 + m / 1024.0) * 2.0 ** (e - 15))
    return fbits(v)


def d3d_pack(v, typ):
    x = u32s(v)
    r = [0, 0, 0, 0]
    if typ == 0:
        c = [_clamp(u, 0x40400000, 0x404000FF) & 0xFF for u in x]
        r[3] = c[3] << 24 | c[0] << 16 | c[1] << 8 | c[2]
    elif typ in (1, 4):
        c = [_clamp(u, 0x403F8001, 0x40407FFF) & 0xFFFF for u in x]
        if typ == 1:
            r[3] = c[0] << 16 | c[1]
        else:
            r[2], r[3] = c[0] << 16 | c[1], c[2] << 16 | c[3]
    elif typ == 2:
        c = [_clamp(u, 0x403FFE01, 0x404001FF) & 0x3FF for u in x[:3]] + [_clamp(x[3], 0x40400000, 0x40400003) & 3]
        r[3] = c[0] | c[1] << 10 | c[2] << 20 | c[3] << 30
    elif typ == 3:
        r[3] = _f2h_rtz(x[0]) << 16 | _f2h_rtz(x[1])
    elif typ == 5:
        r[2], r[3] = _f2h_rtz(x[0]) << 16 | _f2h_rtz(x[1]), _f2h_rtz(x[2]) << 16 | _f2h_rtz(x[3])
    elif typ == 6:
        c = [_clamp(u, 0x40380001, 0x4047FFFF) & 0xFFFFF for u in x[:3]] + [_clamp(x[3], 0x40400000, 0x4040000F) & 15]
        q = c[0] | c[1] << 20 | c[2] << 40 | c[3] << 60
        r[2], r[3] = q >> 32, q & 0xFFFFFFFF
    return r


# (source, element) per result element, source 0 = vD, 1 = the packed value: Xenia's MakePermuteMask tables
PKD3D_PERM = {
    1: [(0, 0, 0, 1, 0, 2, 1, 3), (0, 0, 0, 1, 1, 3, 0, 3), (0, 0, 1, 3, 0, 2, 0, 3), (1, 3, 0, 1, 0, 2, 0, 3)],
    2: [(0, 0, 0, 1, 1, 2, 1, 3), (0, 0, 1, 2, 1, 3, 0, 3), (1, 2, 1, 3, 0, 2, 0, 3), (1, 3, 0, 1, 0, 2, 0, 3)],
    3: [(0, 0, 0, 1, 1, 2, 1, 3), (0, 0, 1, 2, 1, 3, 0, 3), (1, 2, 1, 3, 0, 2, 0, 3), (0, 0, 0, 1, 0, 2, 1, 2)],
}


def ref_pkd3d(typ, pack, shift):
    def ref(a, b, c, d, st):
        p, dv = d3d_pack(b, typ), u32s(d)
        t = PKD3D_PERM[pack][shift]
        return from32([p[t[2 * k + 1]] if t[2 * k] else dv[t[2 * k + 1]] for k in range(4)])
    return ref


def _sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def ref_upkd3d(typ):
    def ref(a, b, c, d, st):
        z, w = u32s(b)[2], u32s(b)[3]
        one = 0x3F800000

        def unb(n, bits):
            return 0x7FC00000 if n == -(1 << (bits - 1)) else (0x40400000 + n) & 0xFFFFFFFF
        if typ == 0:
            return from32([one | (w >> 16) & 0xFF, one | (w >> 8) & 0xFF, one | w & 0xFF, one | w >> 24])
        if typ == 1:
            return from32([unb(_sext(w >> 16, 16), 16), unb(_sext(w & 0xFFFF, 16), 16), 0, one])
        if typ == 4:
            return from32([unb(_sext(z >> 16, 16), 16), unb(_sext(z & 0xFFFF, 16), 16),
                           unb(_sext(w >> 16, 16), 16), unb(_sext(w & 0xFFFF, 16), 16)])
        if typ == 2:
            return from32([unb(_sext((w >> (10 * k)) & 0x3FF, 10), 10) for k in range(3)] + [one + (w >> 30)])
        if typ == 3:
            return from32([_h2f(w >> 16), _h2f(w & 0xFFFF), 0, one])
        if typ == 5:
            return from32([_h2f(z >> 16), _h2f(z & 0xFFFF), _h2f(w >> 16), _h2f(w & 0xFFFF)])
        q = z << 32 | w
        return from32([unb(_sext((q >> (20 * k)) & 0xFFFFF, 20), 20) for k in range(3)] + [one + (q >> 60)])
    return ref


# ---------------------------------------------------------------- the cases

def _rand_vec(rng):
    edges = [0x00, 0x01, 0x7F, 0x80, 0x81, 0xFE, 0xFF]
    return bytes(rng.choice(edges) if rng.random() < 0.4 else rng.randrange(256) for _ in range(16))


class Case:
    def __init__(self, func, ref, inputs, vscr=0, rc=False):
        self.func, self.ref, self.inputs, self.vscr, self.rc = func, ref, inputs, vscr, rc


def build_cases():
    """[(function name, [instruction words])], [Case]."""
    rng = random.Random(270)
    funcs, cases = [], []

    def add(name, word, ref, inputs_list, vscr=0, rc=False):
        funcs.append((name, word))
        for inputs in inputs_list:
            cases.append(Case(name, ref, inputs, vscr, rc))

    def rnd(n=3):
        return [tuple(_rand_vec(rng) for _ in range(4)) for _ in range(n)]

    for name, ref in INT_OPS.items():
        add(name, encode(name, CLASSIC), ref, rnd())
    for name, ref in CMP_OPS.items():
        add(name, encode(name, CLASSIC), ref, rnd(2))
        # record form: CR6 all / none, with equal and with all-greater inputs
        same = _rand_vec(rng)
        add(name + ".", encode(name, CLASSIC, rc=1), ref, [(same, same, same, same),
                                                           (bytes([0x70] * 16), bytes([0x10] * 16), same, same)],
            rc=True)
    for name, base in WIDE_OPS.items():
        add(name, encode(name, WIDE), INT_OPS[base], rnd(2))

    # floats: non-Java mode on and off
    fa = fl([DENORM, TINY, -1.5, NDENORM])
    fb = fl([1.0, -2.0 ** -126, 0.25, 0.0])
    fc = fl([2.0, 0.5, -4.0, 3.0])
    fd = fl([NDENORM, 1.0, 8.0, DENORM])
    floats = {
        "vaddfp": (A._vx(VX_XO["vaddfp"], 4, 1, 2), fref(lambda x, y: f32(x + y), 2)),
        "vsubfp": (A._vx(VX_XO["vsubfp"], 4, 1, 2), fref(lambda x, y: f32(x - y), 2)),
        "vmaxfp": (A._vx(VX_XO["vmaxfp"], 4, 1, 2), fref(_fmax, 2)),
        "vminfp": (A._vx(VX_XO["vminfp"], 4, 1, 2), fref(_fmin, 2)),
        "vmaddfp": (A._va(VA_XO["vmaddfp"], 4, 1, 2, 3), fref(lambda x, y, z: _fmaf(x, z, y), 3)),
        "vrfim": (A._vx(VX_XO["vrfim"], 4, 0, 1), None),
        "vmulfp128": (A.vmulfp128(33, 70, 100), fref(lambda x, y: f32(x * y), 2)),
        "vaddfp128": (A.vaddfp128(33, 70, 100), fref(lambda x, y: f32(x + y), 2)),
    }
    for name, (word, ref) in floats.items():
        if name == "vrfim":
            # vrfim takes vB: its input is the second vector
            def ref(a, b, c, d, st):
                nj = st.vscr & NJ
                return from32([daz(fbits(float(math.floor(bitsf(daz(u, nj))))) if not (
                    bitsf(daz(u, nj)) == 0) else daz(u, nj), nj) for u in u32s(b)])
            ins = [(fa, fa, fc, fd)]
        else:
            ins = [(fa, fb, fc, fd), (fd, fa, fb, fc)]
        add(name, word, ref, ins, vscr=0)
        add(name + "_nj", word, ref, ins, vscr=NJ)
    cmpref = ew(u32s, from32, lambda x, y, st: 0xFFFFFFFF if bitsf(daz(x, st.vscr & NJ)) > bitsf(daz(y, st.vscr & NJ))
                else 0)
    for vscr in (0, NJ):
        add("vcmpgtfp_%x" % vscr, A.vcmpgtfp(4, 1, 2), cmpref,
            [(fl([DENORM, 0.0, NDENORM, 1.0]), fl([0.0, NDENORM, 0.0, DENORM]), fc, fd)], vscr=vscr)

    # estimates (exact inputs: the results are exact, so they match a hardware estimate's ideal value)
    est = {"vexptefp": (lambda x: 2.0 ** x, [0.0, 3.0, -2.0, 10.0]), "vlogefp": (math.log2, [1.0, 8.0, 0.25, 1024.0]),
           "vrefp": (lambda x: 1.0 / x, [2.0, 4.0, -0.5, 1.0]),
           "vrsqrtefp": (lambda x: 1.0 / math.sqrt(x), [4.0, 16.0, 0.25, 1.0])}
    for name, (fn, xs) in est.items():
        ref = fref(fn, 1)

        def wrap(ref=ref):
            return lambda a, b, c, d, st: ref(b, b, c, d, st)  # one input: vB
        add(name, A._vx(VX_XO[name], 4, 0, 2), wrap(), [(fl(xs), fl(xs), fc, fd)], vscr=NJ)
        word = A._vx128_3({v: k for k, v in OP6_3.items()}[name + "128"], 33, 100, 0)
        add(name + "128", word, wrap(), [(fl(xs), fl(xs), fc, fd)], vscr=NJ)

    # D3D pack: biased inputs in range, past the bounds, NaN; vD keeps the elements the merge does not replace
    def pk_inputs(typ):
        if typ == 0:
            vs = [[biased(0x11), biased(0x22), biased(0x33), biased(0x44)],
                  [biased(0x1FF), 0x3F800000, 0x7FC00000, biased(0xFF)]]
        elif typ in (1, 4):
            vs = [[biased(1234), biased(-1234 & 0xFFFFFFFF), biased(0x7FFF), biased(-0x7FFF & 0xFFFFFFFF)],
                  [biased(0x8000), 0x40000000, 0x7FC00000, biased(5)]]
        elif typ == 2:
            vs = [[biased(100), biased(-5 & 0xFFFFFFFF), biased(511), biased(2)],
                  [biased(600), biased(-600 & 0xFFFFFFFF), 0x7FC00000, biased(9)]]
        elif typ == 6:
            vs = [[biased(0x12345), biased(-0x23456 & 0xFFFFFFFF), biased(0x7FFFF), biased(11)],
                  [biased(0x90000), 0x40300000, 0x7FC00000, biased(99)]]
        else:
            vs = [[fbits(1.0), fbits(-2.5), fbits(1.0 / 3.0), fbits(65536.0)],
                  [fbits(6.0e-5), fbits(3.0e-8), 0x7F800000, fbits(-0.0)]]
        dvec = from32([0xD0D0D0D0, 0xD1D1D1D1, 0xD2D2D2D2, 0xD3D3D3D3])
        return [(fc, from32(v), fc, dvec) for v in vs]

    for typ in range(7):
        for pack, shift in ((1, 0), (1, 3), (2, 0), (2, 2), (3, 1), (3, 3)) if typ in (0, 2) else \
                ((1, 0), (2, 0), (2, 1), (3, 3)):
            add("vpkd3d128_%d_%d_%d" % (typ, pack, shift), A.vpkd3d128(33, 100, typ, pack, shift),
                ref_pkd3d(typ, pack, shift), pk_inputs(typ))
    up = {0: [0x44112233, 0xFF00807F], 1: [0x04D2FB2E, 0x80007FFF], 2: [0x8FF7FE64, 0x60080200],
          3: [0x3C00C100, 0x0001FBFF], 4: [0x04D2FB2E, 0x80007FFF], 5: [0x3C00C100, 0x7C00FC00],
          6: [0xB1234567, 0x80000001]}
    for typ, ws in up.items():
        add("vupkd3d128_%d" % typ, A.vupkd3d128(33, 100, typ << 2), ref_upkd3d(typ),
            [(fc, from32([0, 0, ws[0], ws[1]]), fc, fd), (fc, from32([0, 0, ws[1], ws[0]]), fc, fd)])
    return funcs, cases


def program(base, funcs):
    words, entries = [], {}
    for name, op in funcs:
        entries[name] = (base + 4 * len(words), None)
        words += [A.li(5, 16), A.li(6, 32), A.li(7, 48), A.li(8, 64),
                  A.lvx128(CLASSIC["a"], 0, 3), A.lvx128(CLASSIC["b"], 3, 5), A.lvx128(CLASSIC["c"], 3, 6),
                  A.lvx128(CLASSIC["d"], 3, 7),
                  A.lvx128(WIDE["a"], 0, 3), A.lvx128(WIDE["b"], 3, 5), A.lvx128(WIDE["d"], 3, 7),
                  A.lvx128(5, 3, 8), A._vx(0x644, 0, 0, 5),        # mtvscr v5
                  op,
                  A.stvx128(CLASSIC["d"], 0, 4), A.stvx128(WIDE["d"], 4, 6),
                  A._vx(0x604, 5, 0, 0), A.stvx128(5, 4, 5),       # mfvscr v5
                  A.mfcr(3), A.blr()]
        entries[name] = (entries[name][0], base + 4 * len(words))
    return words, entries


def test_every_vector_instruction_is_lifted():
    names = (set(VX4.values()) | set(VC4.values()) | set(VA4.values()) | set(VX128_1.values()) | set(OP5.values())
             | set(OP6.values()) | set(OP6_3.values()) | set(OP6_R.values()) | set(X31_VMX.values())
             | {"vperm128", "vsldoi128", "vpermwi128", "vrlimi128", "vpkd3d128"})
    assert sorted(n for n in names if not hasattr(XenonLifter, "op_" + n)) == []


def test_known_d3d_values():
    """Fixed points of the D3D formats, independent of the tables above."""
    v = from32([biased(0x11), biased(0x22), biased(0x33), biased(0x44)])
    assert d3d_pack(v, 0)[3] == 0x44112233                                  # ARGB
    assert u32s(ref_upkd3d(0)(None, from32([0, 0, 0, 0x44112233]), None, None, None)) == \
        [0x3F800011, 0x3F800022, 0x3F800033, 0x3F800044]                    # 1.0 + n * 2^-23
    assert d3d_pack(from32([fbits(1.0), fbits(-2.5), 0, 0]), 3)[3] == 0x3C00C100
    assert _f2h_rtz(fbits(1.0 / 3.0)) == 0x3555 and _f2h_rtz(fbits(65536.0)) == 0x7BFF   # toward zero
    assert _f2h_rtz(fbits(2.0 ** -24)) == 0x0001 and _h2f(0x0001) == fbits(2.0 ** -24)


@pytest.mark.skipif(not shutil.which("cc") or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_vector_ops_match_references(tmp_path):
    funcs, cases = build_cases()
    text_base = 0x82001000
    words, entries = program(text_base, funcs)
    pdata = [(lo - text_base, hi - lo) for lo, hi in entries.values()]
    data, lay = build_xex(words, functions=pdata, compression="none", entry=0)
    img = parse_xex(data)
    src = tmp_path / "src"
    s = recompile(img, str(src))
    assert s["interp_fallback_insns"] == 0, s["unlifted"]

    lines, expect = [], []
    for n, case in enumerate(cases):
        a, b, c, d = case.inputs
        mem = a + b + c + d + struct.pack(">4I", 0, 0, 0, case.vscr)
        tag = "c%d" % n
        lines.append(c_case(tag, entries[case.func][0], mem, gpr={3: MEM, 4: MEM + 0x100}, out_gpr=[3],
                            out_mem=[(MEM + 0x100, 48)]))
        st = Sat()
        st.vscr = case.vscr
        out = case.ref(a, b, c, d, st)
        vscr = case.vscr | (SAT if st.hit else 0)
        expect.append((tag, case, out, vscr))
    harness = tmp_path / "harness.c"
    harness.write_text(HARNESS % {"size": MEM_SIZE, "base": MEM, "cases": "\n".join(lines)})
    exe = str(tmp_path / "vmx_test")
    inc = []
    for dname in RUNTIME_DIRS:
        inc += ["-I", dname]
    subprocess.run(["cc", "-std=gnu11", "-O1", "-Wall", "-Werror", "-Wno-unused-label", "-ffp-contract=off"] + inc
                   + ["-o", exe, str(harness)] + sorted(str(p) for p in src.glob("*.c")) + ["-lm"], check=True)
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    res = parse_out(r.stdout)
    bad = []
    for tag, case, out, vscr in expect:
        got = res[tag]
        assert got["rc"] == 0 and got["faults"] == 0, (case.func, got)
        blob = got["mem"][MEM + 0x100]
        vd = blob[32:48] if _wide(case.func) else blob[0:16]   # classic vD at +0, VSCR at +16, wide vD at +32
        got_vscr = struct.unpack(">I", blob[28:32])[0]
        if vd != out or got_vscr != vscr:
            bad.append((case.func, [x.hex() for x in case.inputs], "got", vd.hex(), hex(got_vscr), "want",
                        out.hex(), hex(vscr)))
        if case.rc:
            all_t = out == b"\xFF" * 16
            none_t = out == bytes(16)
            cr6 = (got["r3"] >> 4) & 15  # CR field 6: bits 4..7 of the 32-bit CR (from the right)
            if cr6 != (8 if all_t else 0) | (2 if none_t else 0):
                bad.append((case.func, "cr6", cr6))
    assert not bad, "\n".join(map(str, bad[:20])) + "\n(%d mismatches)" % len(bad)


def _wide(name):
    return name.endswith("128") or "128_" in name or name.startswith(("vmulfp128", "vaddfp128"))
