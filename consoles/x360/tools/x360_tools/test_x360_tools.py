"""Tests of the Xbox 360 spike (XBA-230): the Xenon decoder (64-bit, VMX, VMX128), and leaf functions written in
PowerPC assembly here, packed into a synthetic XEX2 (tools/xex/synth.py: basic compression, encrypted with a made-up
test key), parsed, discovered from .pdata, lifted, compiled with the host cc against consoles/x360/runtime and run.
Every expected value is computed here in Python from the instruction set's definition, not from the runtime's C.
No toolchain, no console data."""

import os
import shutil
import struct
import subprocess
import sys
from fractions import Fraction

import pytest

from . import CONSOLE  # noqa: F401
from tools.xex.synth import TEST_KEY, build_xex
from tools.xex.xex import parse_xex

from . import asm as A
from .recomp import RUNTIME_DIRS, recompile
from .xenon import OP5, OP6, OP6_3, OP6_R, VX128_1, XenonDecoder

M64 = (1 << 64) - 1
MEM = 0x40000000          # the harness's guest memory window
MEM_SIZE = 0x10000


# ---------------------------------------------------------------- decoder

def test_decode_64bit_words():
    d = XenonDecoder()
    cases = {
        0xE8640008: ("ld", "ld r3, 8(r4)"),
        0xFBE1FFF8: ("std", "std r31, -8(r1)"),
        0xF821FF91: ("stdu", "stdu r1, -112(r1)"),
        0xE8030002: ("lwa", "lwa r0, 0(r3)"),
        0x78630020: ("rldicl", "rldicl r3, r3, 0, 32"),
        0x7C6421D2: ("mulld", "mulld r3, r4, r4"),
        0x7C631E74: ("sradi", "sradi r3, r3, 3"),
        0x7C6307B4: ("extsw", "extsw r3, r3"),
        0x7C632074: ("cntlzd", "cntlzd r3, r3"),
        0x7C6403D2: ("divd", None),
        0x7C2307EC: ("dcbz128", None),
        0xFC20065E: ("fctidz", None),
        0xFC20069C: ("fcfid", None),
    }
    for w, (name, text) in cases.items():
        i = d.decode(w, 0x82000000)
        assert i.name == name, (hex(w), i.name)
        if text:
            assert i.text == text, (hex(w), i.text)
    i = d.decode(A.cmpd(1, 3, 4), 0)
    assert i.name == "cmp" and i.rd & 1 and i.crfd == 1
    i = d.decode(A.rldicr(5, 6, 32, 31), 0)
    assert i.ext.sh == 32 and i.ext.mb == 31
    i = d.decode(A.sradi(3, 4, 40), 0)
    assert i.ext.sh == 40


def test_decode_altivec():
    d = XenonDecoder()
    assert d.decode(0x1001100A, 0).text == "vaddfp v0, v1, v2"
    assert d.decode(0x100110EB, 0).text == "vperm v0, v1, v2, v3"
    assert d.decode(0x7C0320CE, 0).text == "lvx v0, r3, r4"
    assert d.decode(0x7C0321CE, 0).text == "stvx v0, r3, r4"
    i = d.decode(A.vcmpgtfp(5, 6, 7, rc=1), 0)
    assert i.name == "vcmpgtfp" and i.ext.rc == 1
    assert d.decode(A.vspltisw(4, -3), 0).text == "vspltisw v4, -3"


# Opcode constants of every VMX128 instruction (register fields zero), from Xenia's tables: the decoder must name each.
VMX128_CONSTANTS = {
    0x10000003: "lvsl128", 0x10000043: "lvsr128", 0x10000083: "lvewx128", 0x100000C3: "lvx128",
    0x10000183: "stvewx128", 0x100001C3: "stvx128", 0x100002C3: "lvxl128", 0x100003C3: "stvxl128",
    0x10000403: "lvlx128", 0x10000443: "lvrx128", 0x10000503: "stvlx128", 0x10000543: "stvrx128",
    0x10000603: "lvlxl128", 0x10000643: "lvrxl128", 0x10000703: "stvlxl128", 0x10000743: "stvrxl128",
    0x10000010: "vsldoi128", 0x14000000: "vperm128", 0x14000010: "vaddfp128", 0x14000050: "vsubfp128",
    0x14000090: "vmulfp128", 0x140000D0: "vmaddfp128", 0x14000110: "vmaddcfp128", 0x14000150: "vnmsubfp128",
    0x14000190: "vmsum3fp128", 0x140001D0: "vmsum4fp128", 0x14000200: "vpkshss128", 0x14000210: "vand128",
    0x14000240: "vpkshus128", 0x14000250: "vandc128", 0x14000280: "vpkswss128", 0x14000290: "vnor128",
    0x140002C0: "vpkswus128", 0x140002D0: "vor128", 0x14000300: "vpkuhum128", 0x14000310: "vxor128",
    0x14000340: "vpkuhus128", 0x14000350: "vsel128", 0x14000380: "vpkuwum128", 0x14000390: "vslo128",
    0x140003C0: "vpkuwus128", 0x140003D0: "vsro128", 0x18000000: "vcmpeqfp128", 0x18000050: "vrlw128",
    0x18000080: "vcmpgefp128", 0x180000D0: "vslw128", 0x18000100: "vcmpgtfp128", 0x18000150: "vsraw128",
    0x18000180: "vcmpbfp128", 0x180001D0: "vsrw128", 0x18000200: "vcmpequw128", 0x18000210: "vpermwi128",
    0x18000230: "vcfpsxws128", 0x18000270: "vcfpuxws128", 0x18000280: "vmaxfp128", 0x180002B0: "vcsxwfp128",
    0x180002C0: "vminfp128", 0x180002F0: "vcuxwfp128", 0x18000300: "vmrghw128", 0x18000330: "vrfim128",
    0x18000340: "vmrglw128", 0x18000370: "vrfin128", 0x18000380: "vupkhsb128", 0x180003B0: "vrfip128",
    0x180003C0: "vupklsb128", 0x180003F0: "vrfiz128", 0x18000610: "vpkd3d128", 0x18000630: "vrefp128",
    0x18000670: "vrsqrtefp128", 0x180006B0: "vexptefp128", 0x180006F0: "vlogefp128", 0x18000710: "vrlimi128",
    0x18000730: "vspltw128", 0x18000770: "vspltisw128", 0x180007F0: "vupkd3d128",
}


def test_every_vmx128_opcode_decodes():
    d = XenonDecoder()
    for w, name in VMX128_CONSTANTS.items():
        assert d.decode(w, 0).name == name, (hex(w), d.decode(w, 0).name)
    names = set(VX128_1.values()) | set(OP5.values()) | set(OP6.values()) | set(OP6_3.values()) | \
        set(OP6_R.values()) | {"vperm128", "vsldoi128", "vpermwi128", "vrlimi128", "vpkd3d128"}
    assert names == set(VMX128_CONSTANTS.values())


def test_vmx128_register_fields_round_trip():
    d = XenonDecoder()
    for vd, va, vb in ((0, 0, 0), (127, 127, 127), (64, 33, 96), (5, 100, 31), (99, 64, 1)):
        i = d.decode(A.vaddfp128(vd, va, vb), 0)
        assert (i.name, i.ext.vd, i.ext.va, i.ext.vb) == ("vaddfp128", vd, va, vb)
        i = d.decode(A.vcmpgtfp128(vd, va, vb, rc=1), 0)
        assert (i.name, i.ext.vd, i.ext.va, i.ext.vb, i.ext.rc) == ("vcmpgtfp128", vd, va, vb, 1)
        i = d.decode(A.vperm128(vd, va, vb, 5), 0)
        assert (i.name, i.ext.vd, i.ext.va, i.ext.vb, i.ext.vc) == ("vperm128", vd, va, vb, 5)
        i = d.decode(A.vsldoi128(vd, va, vb, 12), 0)
        assert (i.name, i.ext.vd, i.ext.va, i.ext.vb, i.ext.sh) == ("vsldoi128", vd, va, vb, 12)
        i = d.decode(A.lvx128(vd, 3, 4), 0)
        assert (i.name, i.ext.vd, i.ra, i.rb) == ("lvx128", vd, 3, 4)
        i = d.decode(A.vpermwi128(vd, vb, 0xE4), 0)
        assert (i.name, i.ext.vd, i.ext.vb, i.ext.imm) == ("vpermwi128", vd, vb, 0xE4)
        i = d.decode(A.vrlimi128(vd, vb, 0b0110, 3), 0)
        assert (i.name, i.ext.vd, i.ext.vb, i.ext.imm, i.ext.z) == ("vrlimi128", vd, vb, 6, 3)
        i = d.decode(A.vspltisw128(vd, -5), 0)
        assert (i.name, i.ext.vd) == ("vspltisw128", vd) and i.text.endswith(", -5")


# ---------------------------------------------------------------- assembler with labels

from .prog import Prog  # noqa: E402,F401  (kept importable from here)


def program(text_base):
    p = Prog(text_base)

    # FNV-1a 64 over r4 bytes at r3 -> r3
    p.func("fnv")
    p(A.li64(5, 0xCBF29CE484222325), A.li64(6, 0x100000001B3), A.cmpdi(0, 4, 0))
    p.branch(lambda at, t: A.bc(at, 12, 2, t), "fnv_done")
    p(A.mtctr(4), A.addi(3, 3, -1))
    p.label("fnv_loop")
    p(A.lbzu(7, 1, 3), A.xor(5, 5, 7), A.mulld(5, 5, 6))
    p.branch(A.bdnz, "fnv_loop")
    p.label("fnv_done")
    p(A.mr(3, 5), A.blr())
    p.end()

    # 64-bit mix: r3 = a, r4 = b, r5 = n, r6 = out (13 doublewords) -> r3
    p.func("mix")
    p(A.li64(7, 0x9E3779B97F4A7C15),
      A.mulld(8, 4, 7), A.rotldi(9, 3, 13), A.xor(3, 9, 8), A.mulhdu(10, 3, 7), A.sradi(11, 3, 7),
      A.divd(12, 11, 5), A.cntlzd(14, 4),
      A.std(3, 0, 6), A.std(10, 8, 6), A.std(12, 16, 6),
      A.rlwinm(15, 4, 4, 28, 3), A.std(15, 24, 6),
      A.extsw(16, 11), A.std(16, 32, 6),
      A.lwa(17, 4, 6), A.std(17, 40, 6),
      A.srd(18, 3, 14), A.std(18, 48, 6),
      A.cmpd(1, 3, 4), A.mfcr(19), A.rlwinm(19, 19, 8, 28, 31), A.std(19, 56, 6),
      A.cmpw(2, 3, 4), A.mfcr(20), A.rlwinm(20, 20, 12, 28, 31), A.std(20, 64, 6),
      A.addc(21, 3, 4), A.adde(22, 10, 12), A.std(22, 72, 6),
      A.sld(23, 4, 5), A.std(23, 80, 6),
      A.mr(24, 3), A.rldimi(24, 4, 16, 8), A.std(24, 88, 6),
      A.addi(25, 0, -1), A.rlwinm(25, 25, 0, 16, 31), A.std(25, 96, 6),
      A.add(3, 3, 14), A.blr())
    p.end()

    # Horner: r3 = coefficients (doubles, highest degree first), r4 = count (>= 2), f1 = x, r5 = out -> f1
    p.func("poly")
    p(A.lfd(2, 0, 3), A.addi(4, 4, -1), A.mtctr(4))
    p.label("poly_loop")
    p(A.lfdu(3, 8, 3), A.fmadd(2, 2, 1, 3))
    p.branch(A.bdnz, "poly_loop")
    p(A.fctidz(4, 2), A.stfd(4, 0, 5), A.fcfid(5, 4), A.fsub(6, 2, 5), A.fabs_(7, 6), A.fsel(8, 6, 2, 7),
      A.stfd(5, 8, 5), A.stfd(7, 16, 5), A.stfd(8, 24, 5), A.fmr(1, 2), A.blr())
    p.end()

    # VMX128 4x4 transform: r3 = matrix rows, r4 = input vec4s, r5 = output, r6 = count
    p.func("xform")
    p(A.lvx128(64, 0, 3), A.li(7, 16), A.lvx128(65, 3, 7), A.li(8, 32), A.lvx128(66, 3, 8), A.li(9, 48),
      A.lvx128(67, 3, 9), A.mtctr(6))
    p.label("xform_loop")
    p(A.lvx128(100, 0, 4), A.vmsum4fp128(101, 100, 64), A.vmsum4fp128(102, 100, 65), A.vmsum4fp128(103, 100, 66),
      A.vmsum4fp128(104, 100, 67), A.vrlimi128(101, 102, 0b0100, 0), A.vrlimi128(101, 103, 0b0010, 0),
      A.vrlimi128(101, 104, 0b0001, 0), A.stvx128(101, 0, 5), A.addi(4, 4, 16), A.addi(5, 5, 16))
    p.branch(A.bdnz, "xform_loop")
    p(A.blr())
    p.end()

    # VMX128 grab bag: r3 = unaligned source (4 floats), r4 = unaligned destination, r5 = aligned scratch -> r3 = CR
    p.func("vec128")
    p(A.li(6, 16), A.lvlx128(70, 0, 3), A.lvrx128(71, 3, 6), A.vor128(72, 70, 71),
      A.lvx128(80, 0, 5), A.vspltisw128(90, 1), A.vcsxwfp128(90, 90, 0), A.vor128(73, 90, 90),
      A.vmaddfp128(73, 72, 80), A.vpermwi128(74, 73, 0xE4), A.vcmpgtfp128(75, 74, 90, rc=1),
      A.vor128(76, 75, 75), A.vsel128(76, 90, 74), A.vcfpsxws128(77, 76, 2), A.vspltisw128(91, 3),
      A.vslw128(78, 77, 91), A.vmrghw128(79, 77, 78), A.stvlx128(79, 0, 4), A.stvrx128(79, 4, 6),
      A.vsldoi128(81, 72, 80, 4), A.li(7, 32), A.stvx128(81, 5, 7),
      A.vnmsubfp128(73, 72, 80), A.li(8, 48), A.stvx128(73, 5, 8),
      A.mfcr(3))
    p.branch(lambda at, t: A.bc(at, 12, 24, t), "vec128_skip")
    p(A.li(9, 0x55), A.stw(9, 64, 5))
    p.label("vec128_skip")
    p(A.blr())
    p.end()

    # classic AltiVec: r3 = aligned inputs, r4 = unaligned pointer, r5 = aligned output -> r3 = CR
    p.func("altivec")
    p(A.lvx(1, 0, 3), A.li(6, 16), A.lvx(2, 3, 6), A.vperm(3, 1, 1, 2), A.stvx(3, 0, 5),
      A.li(7, 32), A.lvx(4, 3, 7), A.li(8, 48), A.lvx(5, 3, 8), A.vnmsubfp(6, 4, 5, 1), A.li(9, 16),
      A.stvx(6, 5, 9),
      A.vspltw(7, 4, 2), A.vmrglw(8, 7, 5), A.li(10, 32), A.stvx(8, 5, 10),
      A.lvsl(9, 0, 4), A.lvx(10, 0, 4), A.li(11, 16), A.lvx(11, 4, 11), A.vperm(12, 10, 11, 9), A.li(12, 48),
      A.stvx(12, 5, 12),
      A.vspltisw(13, -2), A.vadduwm(14, 3, 13), A.li(14, 64), A.stvx(14, 5, 14),
      A.vctsxs(15, 6, 1), A.vcfsx(16, 15, 1), A.vmaxfp(17, 16, 4), A.li(15, 80), A.stvx(17, 5, 15),
      A.vsldoi(18, 4, 5, 12), A.li(16, 96), A.stvx(18, 5, 16),
      A.li(17, 120), A.stvewx(5, 5, 17),
      A.vcmpgtfp(19, 4, 5, rc=1), A.mfcr(3), A.blr())
    p.end()
    words = p.link()
    funcs = {name: (text_base + 4 * a, text_base + 4 * b) for name, a, b in p.funcs}
    return words, funcs


# ---------------------------------------------------------------- independent references

def f32(x):
    return struct.unpack(">f", struct.pack(">f", x))[0]


def f32b(*xs):
    return struct.pack(">%df" % len(xs), *xs)


def rotl64(v, n):
    return ((v << n) | (v >> (64 - n))) & M64 if n else v


def s64(v):
    return v - (1 << 64) if v & (1 << 63) else v


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & (1 << 31) else v


def cr_field(a, b):
    return 8 if a < b else 4 if a > b else 2


def ref_fnv(data):
    h = 0xCBF29CE484222325
    for byte in data:
        h = ((h ^ byte) * 0x100000001B3) & M64
    return h


def ref_mix(a, b, n):
    K = 0x9E3779B97F4A7C15
    x = rotl64(a, 13) ^ ((b * K) & M64)
    hi = (x * K) >> 64
    s = s64(x) >> 7
    q = abs(s) // abs(n) * (1 if (s < 0) == (n < 0) else -1)     # truncating division
    clz = 64 - b.bit_length()
    r32 = (((b & 0xFFFFFFFF) << 4) | ((b & 0xFFFFFFFF) >> 28)) & 0xFFFFFFFF
    m32 = 0xF000000F                                             # MASK(28, 3), wrapping
    rlw = (r32 << 32) | (r32 & m32)                              # 64-bit mode: high word replicated
    carry = 1 if x + b > M64 else 0
    m = ((M64 >> 8) & ~(M64 >> 48)) & M64                        # MASK(8, 47)
    ins = (rotl64(b, 16) & m) | (x & ~m & M64)
    out = [x, hi, q & M64, rlw, s32(s) & M64, s32(x) & M64, x >> clz if clz < 64 else 0,
           cr_field(s64(x), s64(b)), cr_field(s32(x), s32(b)), (hi + (q & M64) + carry) & M64,
           (b << n) & M64 if n < 64 else 0, ins, 0xFFFF]
    return (x + clz) & M64, out


def ref_poly(coeffs, x):
    acc = Fraction(coeffs[0])
    for c in coeffs[1:]:
        acc = acc * Fraction(x) + Fraction(c)
    p = float(acc)
    assert Fraction(p) == acc  # the inputs keep every step exact: no rounding question
    t = int(p)                 # fctidz: toward zero
    frac = p - float(t)
    return p, [t & M64, struct.unpack(">Q", struct.pack(">d", float(t)))[0],
               struct.unpack(">Q", struct.pack(">d", abs(frac)))[0],
               struct.unpack(">Q", struct.pack(">d", p if frac >= 0 else abs(frac)))[0]]


# ---------------------------------------------------------------- the harness

HARNESS = r"""
#include <stdio.h>
#include "xenon_rt.h"
extern const xe_rc_entry xe_rc_table[];
extern const unsigned xe_rc_count;
static uint8_t mem[%(size)d];
static xe_machine M;

static int run(uint32_t entry)
{
    M.cpu.pc = entry;
    M.cpu.lr = 0xDEAD0000u;
    for (int n = 0; n < 100000; n++) {
        if (M.cpu.pc == 0xDEAD0000u)
            return 0;
        xe_rc_fn f = NULL;
        for (unsigned k = 0; k < xe_rc_count; k++)
            if (xe_rc_table[k].entry == M.cpu.pc)
                f = xe_rc_table[k].fn;
        if (!f)
            return 2;
        M.rc_exit = 0;
        f(&M);
        if (M.rc_exit)
            return 10 + M.rc_exit;
    }
    return 3;
}

static void dump(const char *name, uint32_t a, uint32_t n)
{
    printf("%%s m %%08X ", name, a);
    for (uint32_t k = 0; k < n; k++)
        printf("%%02x", mem[a - %(base)du + k]);
    printf("\n");
}

int main(void)
{
    M.mem = mem;
    M.mem_base = %(base)du;
    M.mem_size = sizeof mem;
%(cases)s
    return 0;
}
"""


def c_case(name, entry, mem, gpr=None, fpr=None, out_gpr=(), out_fpr=(), out_mem=()):
    lines = ["    memset(&M.cpu, 0, sizeof M.cpu); memset(mem, 0, sizeof mem); M.faults = 0;"]
    for off in range(0, len(mem), 32):
        chunk = mem[off:off + 32]
        lines.append('    memcpy(mem + %d, "%s", %d);' % (off, "".join("\\x%02x" % c for c in chunk), len(chunk)))
    for r, v in (gpr or {}).items():
        lines.append("    M.cpu.gpr[%d] = 0x%016XULL;" % (r, v & M64))
    for r, v in (fpr or {}).items():
        lines.append("    M.cpu.fpr[%d] = %r;" % (r, v))
    lines.append('    { int rc = run(0x%08Xu); printf("%s rc %%d faults %%u native %%llu\\n", rc, M.faults, '
                 '(unsigned long long)M.rc_native); }' % (entry, name))
    for r in out_gpr:
        lines.append('    printf("%s r%d %%016llx\\n", (unsigned long long)M.cpu.gpr[%d]);' % (name, r, r))
    for r in out_fpr:
        lines.append('    { uint64_t u; memcpy(&u, &M.cpu.fpr[%d], 8); printf("%s f%d %%016llx\\n", '
                     '(unsigned long long)u); }' % (r, name, r))
    for a, n in out_mem:
        lines.append('    dump("%s", 0x%08Xu, %d);' % (name, a, n))
    return "\n".join(lines)


def parse_out(text):
    res = {}
    for line in text.splitlines():
        parts = line.split()
        name = parts[0]
        d = res.setdefault(name, {"mem": {}})
        if parts[1] == "rc":
            d["rc"], d["faults"], d["native"] = int(parts[2]), int(parts[4]), int(parts[6])
        elif parts[1] == "m":
            d["mem"][int(parts[2], 16)] = bytes.fromhex(parts[3])
        else:
            d[parts[1]] = int(parts[2], 16)
    return res


def build_title(text_base_hint=None):
    text_base = 0x82001000  # synth.py's fixed layout: .text at base + 0x1000
    words, funcs = program(text_base)
    order = list(funcs.items())
    pdata = [(lo - text_base, hi - lo) for _, (lo, hi) in order]
    data, lay = build_xex(words, imports={"xboxkrnl.exe": [(0x10, "thunk"), (0x1B, "variable")],
                                          "xam.xex": [(0x200, "thunk")]},
                          functions=pdata, compression="basic", key=TEST_KEY, entry=0)
    assert lay.text == text_base
    return data, funcs


def test_synthetic_title_parses_and_lifts(tmp_path):
    data, funcs = build_title()
    img = parse_xex(data, [bytes(16), TEST_KEY])
    assert img.key_used == 1 and img.image is not None
    assert [s for s, _ in img.pdata] == [lo for lo, _ in funcs.values()]
    assert {lib.name for lib in img.libraries} == {"xboxkrnl.exe", "xam.xex"}
    s = recompile(img, str(tmp_path))
    assert s["functions"] == len(funcs)
    assert s["interp_fallback_insns"] == 0, s["unlifted"]
    text = (tmp_path / "x360_rc_000.c").read_text()
    assert "xv_msum4fp(xv_daz(c->vscr, V(100)), xv_daz(c->vscr, V(64)))" in text
    assert ("V(73) = xv_daz(c->vscr, xv_maddfp(xv_daz(c->vscr, V(72)), xv_daz(c->vscr, V(80)), "
            "xv_daz(c->vscr, V(73))));") in text  # vmaddfp128: vD = vA * vB + vD


@pytest.mark.skipif(not shutil.which("cc") or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_lifted_functions_match_references(tmp_path):
    data, funcs = build_title()
    img = parse_xex(data, [TEST_KEY])
    src = tmp_path / "src"
    recompile(img, str(src))

    cases, checks = [], []
    # fnv
    blob = bytes((7 * k + 3) & 0xFF for k in range(100))
    cases.append(c_case("fnv", funcs["fnv"][0], blob, gpr={3: MEM, 4: len(blob)}, out_gpr=[3]))
    checks.append(("fnv", "r3", ref_fnv(blob)))
    cases.append(c_case("fnv0", funcs["fnv"][0], b"", gpr={3: MEM, 4: 0}, out_gpr=[3]))
    checks.append(("fnv0", "r3", 0xCBF29CE484222325))
    # mix
    for tag, a, b, n in (("mix1", 0x0123456789ABCDEF, 0xFEDCBA9876543210, 3),
                         ("mix2", 0xFFFFFFFF00000001, 0x00000000DEADBEEF, 7)):
        res, outs = ref_mix(a, b, n)
        cases.append(c_case(tag, funcs["mix"][0], b"", gpr={3: a, 4: b, 5: n, 6: MEM + 0x100},
                            out_gpr=[3], out_mem=[(MEM + 0x100, 8 * len(outs))]))
        checks.append((tag, "r3", res))
        checks.append((tag, ("mem", MEM + 0x100), struct.pack(">%dQ" % len(outs), *outs)))
    # poly
    coeffs = [1.5, -2.0, 0.25, 3.0, -7.75]
    x = -2.5
    p, outs = ref_poly(coeffs, x)
    cases.append(c_case("poly", funcs["poly"][0], struct.pack(">5d", *coeffs),
                        gpr={3: MEM, 4: len(coeffs), 5: MEM + 0x200}, fpr={1: x}, out_fpr=[1],
                        out_mem=[(MEM + 0x200, 32)]))
    checks.append(("poly", "f1", struct.unpack(">Q", struct.pack(">d", p))[0]))
    checks.append(("poly", ("mem", MEM + 0x200), struct.pack(">4Q", *outs)))
    # xform
    rows = [[1, 2, 0, 0.5], [0, 1, -1, 2], [3, 0, 0.25, 1], [-1, -2, 4, 0]]
    vecs = [[1, 1, 1, 1], [2, -1, 0.5, 4], [0, 0, 8, -0.5]]
    mem = f32b(*sum(rows, [])) + f32b(*sum(vecs, []))
    cases.append(c_case("xform", funcs["xform"][0], mem, gpr={3: MEM, 4: MEM + 64, 5: MEM + 0x300, 6: len(vecs)},
                        out_mem=[(MEM + 0x300, 16 * len(vecs))]))
    expect = b"".join(f32b(*[sum(Fraction(r[k]) * Fraction(v[k]) for k in range(4)) for r in rows]) for v in vecs)
    checks.append(("xform", ("mem", MEM + 0x300), expect))
    # vec128: unaligned source at +0x13, destination at +0x45, scratch at +0x400
    src_f = [0.5, 2.0, -1.0, 3.5]
    scale = [2.0, 0.25, 4.0, -1.0]
    mem = bytearray(0x500)
    mem[0x13:0x23] = f32b(*src_f)
    mem[0x400:0x410] = f32b(*scale)
    cases.append(c_case("vec128", funcs["vec128"][0], bytes(mem), gpr={3: MEM + 0x13, 4: MEM + 0x45, 5: MEM + 0x400},
                        out_gpr=[3], out_mem=[(MEM + 0x40, 0x20), (MEM + 0x420, 0x30)]))
    v73 = [f32(a * b + 1.0) for a, b in zip(src_f, scale)]           # vmaddfp128: v72 * v80 + v73 (1.0)
    v74 = v73[::-1]                                                  # vpermwi128 0xE4: w z y x
    gt = [v > 1.0 for v in v74]
    v76 = [v if g else 1.0 for v, g in zip(v74, gt)]                 # vsel128
    v77 = [int(v * 4) for v in v76]                                  # vcfpsxws128 2: truncate after * 4
    v78 = [(v << 3) & 0xFFFFFFFF for v in v77]
    v79 = struct.pack(">4I", v77[0] & 0xFFFFFFFF, v78[0], v77[1] & 0xFFFFFFFF, v78[1])
    dst = bytearray(0x20)
    dst[5:5 + 16] = v79
    checks.append(("vec128", ("mem", MEM + 0x40), bytes(dst)))
    cat = f32b(*src_f) + f32b(*scale)
    v73n = [f32(-(a * b - c)) for a, b, c in zip(src_f, scale, v73)]  # vnmsubfp128: -(v72 * v80 - v73)
    scratch = cat[4:20] + f32b(*v73n) + (b"" if all(gt) else struct.pack(">I", 0x55)) + bytes(0)
    scratch = scratch + bytes(0x30 - len(scratch))
    checks.append(("vec128", ("mem", MEM + 0x420), scratch))
    cr6 = (8 if all(gt) else 0) | (2 if not any(gt) else 0)
    checks.append(("vec128", "r3", cr6 << 4))
    # altivec: inputs at +0, unaligned pointer +0x107, output at +0x200
    a_f = [1.0, -2.0, 0.5, 8.0]
    ctl = bytes([3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12])
    b_f = [2.0, 3.0, -0.5, 0.25]
    c_f = [0.5, 1.5, 4.0, 2.0]
    un = bytes(range(0x80, 0xA0))
    mem = bytearray(0x200)
    mem[0:16] = f32b(*a_f)
    mem[16:32] = ctl
    mem[32:48] = f32b(*b_f)
    mem[48:64] = f32b(*c_f)
    mem[0x100:0x120] = un
    cases.append(c_case("altivec", funcs["altivec"][0], bytes(mem), gpr={3: MEM, 4: MEM + 0x107, 5: MEM + 0x200},
                        out_gpr=[3], out_mem=[(MEM + 0x200, 128)]))
    a_bytes = f32b(*a_f)
    swapped = b"".join(a_bytes[k:k + 4][::-1] for k in range(0, 16, 4))
    v6 = [f32(-(b * c - a)) for a, b, c in zip(a_f, b_f, c_f)]
    v8 = [b_f[2], c_f[2], b_f[2], c_f[3]]                            # vmrglw(vspltw(b, 2), c)
    v12 = un[7:23]                                                   # lvsl + vperm: the unaligned 16 bytes
    v14 = struct.pack(">4I", *[(w - 2) & 0xFFFFFFFF for w in struct.unpack(">4I", swapped)])
    v15 = [int(v * 2) for v in v6]                                   # vctsxs 1 (no value saturates here)
    v17 = [max(v / 2, b) for v, b in zip(v15, b_f)]
    v18 = (f32b(*b_f) + f32b(*c_f))[12:28]
    out = bytearray(128)
    out[0:16] = swapped
    out[16:32] = f32b(*v6)
    out[32:48] = f32b(*v8)
    out[48:64] = v12
    out[64:80] = v14
    out[80:96] = f32b(*v17)
    out[96:112] = v18
    out[120:124] = f32b(c_f[2])                                      # stvewx at +120: element 2
    checks.append(("altivec", ("mem", MEM + 0x200), bytes(out)))
    gtv = [b > c for b, c in zip(b_f, c_f)]
    checks.append(("altivec", "r3", ((8 if all(gtv) else 0) | (2 if not any(gtv) else 0)) << 4))

    harness = tmp_path / "harness.c"
    harness.write_text(HARNESS % {"size": MEM_SIZE, "base": MEM, "cases": "\n".join(cases)})
    exe = str(tmp_path / "x360_test")
    inc = []
    for d in RUNTIME_DIRS:
        inc += ["-I", d]
    srcs = sorted(str(p) for p in src.glob("*.c"))
    subprocess.run(["cc", "-std=gnu11", "-O1", "-Wall", "-Werror", "-Wno-unused-label", "-ffp-contract=off"] + inc
                   + ["-o", exe, str(harness)] + srcs + ["-lm"], check=True)
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    res = parse_out(r.stdout)
    for name, d in res.items():
        assert d["rc"] == 0 and d["faults"] == 0, (name, d)
    for name, what, expect in checks:
        if isinstance(what, tuple):
            got = res[name]["mem"][what[1]]
            assert got == expect, (name, got.hex(), expect.hex())
        else:
            assert res[name][what] == expect, (name, what, hex(res[name][what]), hex(expect))


def test_recomp_cli_on_a_synthetic_xex(tmp_path):
    data, _ = build_title()
    xex = tmp_path / "t.xex"
    xex.write_bytes(data)
    key = tmp_path / "key.txt"
    key.write_text(TEST_KEY.hex() + "\n")
    env = dict(os.environ, PYTHONPATH=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = [sys.executable, "-m", "x360_tools.recomp", str(xex), str(tmp_path / "out"), "--key-file", str(key),
            "--sweep"]
    if shutil.which("cc"):
        args.append("--compile")
    r = subprocess.run(args, env=env, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    assert "recomp: 6 functions" in r.stdout
    assert "0 unknown" in r.stdout
