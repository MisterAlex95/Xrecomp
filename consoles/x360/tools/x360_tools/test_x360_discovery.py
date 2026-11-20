"""Tests of the Xbox 360 discovery additions (XBA-270): MSVC's register save / restore helpers inlined at their call
sites, bctr jump tables (absolute, byte offsets + base, GCC's relative) as switches, calls returning through the
resume-point table. Leaf functions written in assembly here, in the shapes XenonRecomp / Xenia describe, packed in a
synthetic XEX2 (LZX "normal" compression), lifted, built with the host cc and run; expected values from Python."""

import shutil
import struct
import subprocess
import sys

import pytest

from . import CONSOLE  # noqa: F401
from tools.xex.synth import build_xex
from tools.xex.xex import parse_xex

from . import asm as A
from .discover360 import JumpTables, find_helpers, gpr_writes, is_call, pointer_seeds
from .recomp import RUNTIME_DIRS, CodeView, recompile
from .prog import Prog
from .test_x360_tools import M64, c_case, parse_out
from .xenon import XenonDecoder

TEXT = 0x82001000
MEM = 0x82000000         # the harness's memory window holds the image (jump tables are read from it)
MEM_SIZE = 0x20000
STACK = MEM + 0x1F000


def cmplwi(crf, ra, uimm):
    return (10 << 26) | ((crf << 2) << 21) | (ra << 16) | (uimm & 0xFFFF)


def lbzx(rd, ra, rb):
    return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (87 << 1)


def lhzx(rd, ra, rb):
    return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (279 << 1)


def stwu(rs, d, ra):
    return (37 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)


def ha(t):
    return ((t + 0x8000) >> 16) & 0xFFFF


def lo(t):
    return t & 0xFFFF


def helpers(p):
    """MSVC's helpers: GPR + LR (r14..r31 at -0x98..-0x10 from r1, LR word at -8), FPR (f14..f31 at -0x90..-8 from
    r12), VMX (v14..v31, li r11 / stvx from r12) and VMX128 (v64..v127)."""
    p.func("__savegprlr")
    for r in range(14, 32):
        p.label("__savegprlr_%d" % r)
        p(A.std(r, -0x98 + 8 * (r - 14), 1))
    p(A.stw(12, -8, 1), A.blr())
    p.end()
    p.func("__restgprlr")
    for r in range(14, 32):
        p.label("__restgprlr_%d" % r)
        p(A.ld(r, -0x98 + 8 * (r - 14), 1))
    p(A.lwz(12, -8, 1), A.mtlr(12), A.blr())
    p.end()
    p.func("__savefpr")
    for r in range(14, 32):
        p.label("__savefpr_%d" % r)
        p(A.stfd(r, -0x90 + 8 * (r - 14), 12))
    p(A.blr())
    p.end()
    p.func("__restfpr")
    for r in range(14, 32):
        p.label("__restfpr_%d" % r)
        p(A.lfd(r, -0x90 + 8 * (r - 14), 12))
    p(A.blr())
    p.end()
    for kind, op, lo_, hi_ in (("save", A.stvx, 14, 31), ("rest", A.lvx, 14, 31), ("save", A.stvx128, 64, 127),
                               ("rest", A.lvx128, 64, 127)):
        name = "__%svmx_%d" % (kind, lo_)
        p.func(name)
        for r in range(lo_, hi_ + 1):
            p.label("__%svmx_%d" % (kind, r))
            p(A.li(11, -16 * (hi_ + 1 - r)), op(r, 11, 12))
        p(A.blr())
        p.end()


def program():
    p = Prog(TEXT)
    helpers(p)

    # sum4(a, b, c, d) = 2a + 2b + c + d, through r28..r31 saved by the helpers and two calls
    p.func("sum4")
    p(A.mflr(12))
    p.branch(A.bl, "__savegprlr_28")
    p(stwu(1, -0x60, 1), A.mr(28, 3), A.mr(29, 4), A.mr(30, 5), A.mr(31, 6), A.mr(3, 28))
    p.branch(A.bl, "twice")
    p(A.add(31, 31, 3), A.mr(3, 29))
    p.branch(A.bl, "twice")
    p(A.add(31, 31, 3), A.add(3, 31, 30), A.addi(1, 1, 0x60))
    p.branch(A.b, "__restgprlr_28")
    p.end()
    p.func("twice")
    p(A.add(3, 3, 3), A.blr())
    p.end()

    # fsum(f1, f2) = f1 * f2 + f1 in f30 / f31, saved and restored by the FPR helpers; and v30 / v31, v126 / v127
    p.func("fsum")
    p(A.mflr(10), A.addi(12, 1, -8))
    p.branch(A.bl, "__savefpr_30")
    p(A.addi(12, 1, -0x200))
    p.branch(A.bl, "__savevmx_30")
    p(A.addi(12, 1, -0x400))
    p.branch(A.bl, "__savevmx_126")
    p(A.fmr(30, 1), A.fmr(31, 2), A.fmul(1, 30, 31), A.fadd(1, 1, 30),
      A.vxor(30, 30, 30), A.vxor(31, 31, 31), A.vxor128(126, 126, 126), A.vxor128(127, 127, 127),
      A.addi(12, 1, -0x400))
    p.branch(A.bl, "__restvmx_126")
    p(A.addi(12, 1, -0x200))
    p.branch(A.bl, "__restvmx_30")
    p(A.addi(12, 1, -8))
    p.branch(A.bl, "__restfpr_30")
    p(A.mtlr(10), A.blr())
    p.end()

    # jt_abs(i): MSVC's absolute table (lwzx of addresses), five cases and a default
    p.func("jt_abs")
    p(cmplwi(6, 3, 4))
    p.branch(lambda at, t: A.bc(at, 12, 4 * 6 + 1, t), "abs_def")
    p.branch(lambda at, t: A.lis(12, ha(t)), "abs_tbl")
    p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), "abs_tbl")
    p(A.rlwinm(0, 3, 2, 0, 29), A.lwzx(0, 12, 0), A.mtctr(0), A.bctr())
    p.label("abs_tbl")
    for k in (0, 1, 2, 3, 4):
        p.branch(lambda at, t: t, "abs_c%d" % k)
    for k, v in enumerate((10, 20, 30, 40, 50)):
        p.label("abs_c%d" % k)
        p(A.li(3, v), A.blr())
    p.label("abs_def")
    p(A.li(3, -1), A.blr())
    p.end()

    # jt_byte(i): MSVC's compact table: byte offsets (in words) from a base, six entries, two cases shared
    p.func("jt_byte")
    p(cmplwi(7, 3, 5))
    p.branch(lambda at, t: A.bc(at, 12, 4 * 7 + 1, t), "byte_def")
    p.branch(lambda at, t: A.lis(12, ha(t)), "byte_tbl")
    p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), "byte_tbl")
    p(lbzx(0, 12, 3))
    p.branch(lambda at, t: A.lis(12, ha(t)), "byte_base")
    p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), "byte_base")
    p(A.rlwinm(0, 0, 2, 0, 29), A.add(12, 12, 0), A.mtctr(12), A.bctr())
    p.label("byte_base")
    cases = [(7, 2), (8, 2), (9, 2), (100, 2)]   # (value, words)
    offs = []
    k = 0
    for v, n in cases:
        offs.append(k)
        p(A.li(3, v), A.blr())
        k += n
    p.label("byte_tbl")
    order = [0, 1, 1, 2, 3, 0]
    tbl = bytes(offs[j] for j in order) + bytes(2)
    p(*struct.unpack(">2I", tbl))
    p.label("byte_def")
    p(A.li(3, -2), A.blr())
    p.end()

    # jt_gcc(i): GCC's relative table: entries are signed offsets from the table itself
    p.func("jt_gcc")
    p(cmplwi(0, 3, 2))
    p.branch(lambda at, t: A.bc(at, 12, 1, t), "gcc_def")
    p.branch(lambda at, t: A.lis(9, ha(t)), "gcc_tbl")
    p.branch(lambda at, t: A.addi(9, 9, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), "gcc_tbl")
    p(A.rlwinm(10, 3, 2, 0, 29), A.lwzx(10, 9, 10), A.add(10, 10, 9), A.mtctr(10), A.bctr())
    p.label("gcc_c0")
    p(A.li(3, 111), A.blr())
    p.label("gcc_tbl")
    p.branch(lambda at, t: (t - p.labels["gcc_tbl"]) & 0xFFFFFFFF, "gcc_c0")
    p.branch(lambda at, t: (t - p.labels["gcc_tbl"]) & 0xFFFFFFFF, "gcc_c1")
    p.branch(lambda at, t: (t - p.labels["gcc_tbl"]) & 0xFFFFFFFF, "gcc_c2")
    p.label("gcc_c1")
    p(A.li(3, 222), A.blr())
    p.label("gcc_c2")
    p(A.li(3, 333), A.blr())
    p.label("gcc_def")
    p(A.li(3, -3), A.blr())
    p.end()

    # jt_min(i): cases 10..13. MSVC subtracts the lowest case before the bound check, into another register: the
    # table is indexed by the value that was compared
    p.func("jt_min")
    p(A.addi(11, 3, -10), cmplwi(6, 11, 3))
    p.branch(lambda at, t: A.bc(at, 12, 4 * 6 + 1, t), "min_def")
    p.branch(lambda at, t: A.lis(12, ha(t)), "min_tbl")
    p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), "min_tbl")
    p(A.rlwinm(0, 11, 2, 0, 29), A.lwzx(0, 12, 0), A.mtctr(0), A.bctr())
    p.label("min_tbl")
    for k in range(4):
        p.branch(lambda at, t: t, "min_c%d" % k)
    for k, v in enumerate((1010, 1011, 1012, 1013)):
        p.label("min_c%d" % k)
        p(A.li(3, v), A.blr())
    p.label("min_def")
    p(A.li(3, -4), A.blr())
    p.end()

    # jt_half(i): cases 1..3, the index adjusted in place; halfword offsets (in bytes) from a base, and the nop MSVC
    # leaves between mtctr and bctr
    p.func("jt_half")
    p(A.addi(3, 3, -1), cmplwi(6, 3, 2))
    p.branch(lambda at, t: A.bc(at, 12, 4 * 6 + 1, t), "half_def")
    p.branch(lambda at, t: A.lis(12, ha(t)), "half_tbl")
    p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), "half_tbl")
    p(A.rlwinm(0, 3, 1, 0, 30), lhzx(0, 12, 0))
    p.branch(lambda at, t: A.lis(12, ha(t)), "half_base")
    p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), "half_base")
    p(A.add(12, 12, 0), A.mtctr(12), A.ori(0, 0, 0), A.bctr())
    p.label("half_base")
    for v in (2001, 2002, 2003):
        p(A.li(3, v), A.blr())
    p.label("half_tbl")
    p(*struct.unpack(">2I", struct.pack(">4H", 0, 8, 16, 0)))
    p.label("half_def")
    p(A.li(3, -5), A.blr())
    p.end()

    # jt_open(i): no bound check (the compiler knew the range): an absolute table in the function's own code, which
    # ends at the first case label
    p.func("jt_open")
    p.branch(lambda at, t: A.lis(12, ha(t)), "open_tbl")
    p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), "open_tbl")
    p(A.rlwinm(0, 3, 2, 0, 29), A.lwzx(0, 12, 0), A.mtctr(0), A.bctr())
    p.label("open_tbl")
    for k in (0, 1, 2):
        p.branch(lambda at, t: t, "open_c%d" % k)
    for k, v in enumerate((3001, 3002, 3003)):
        p.label("open_c%d" % k)
        p(A.li(3, v), A.blr())
    p.end()

    # The bound check ends the block before the table code in two more ways: jt_ret returns when the index is out
    # of range (`bgtlr`), jt_inv branches to the table code when it is in range (`ble` over a `b default`). Byte
    # tables: an absolute one would be read without its bound
    for name, base in (("ret", 4001), ("inv", 5001)):
        p.func("jt_" + name)
        p(cmplwi(6, 3, 2))
        if name == "ret":
            p(0x4D990020)  # bgtlr cr6
        else:
            p.branch(lambda at, t: A.bc(at, 4, 4 * 6 + 1, t), "inv_go")
            p.branch(lambda at, t: A.b(at, t), "inv_def")
            p.label("inv_go")
        p.branch(lambda at, t: A.lis(12, ha(t)), name + "_tbl")
        p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), name + "_tbl")
        p(lbzx(0, 12, 3))
        p.branch(lambda at, t: A.lis(12, ha(t)), name + "_base")
        p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), name + "_base")
        p(A.rlwinm(0, 0, 2, 0, 29), A.add(12, 12, 0), A.mtctr(12), A.bctr())
        p.label(name + "_base")
        for k in (0, 1, 2):
            p(A.li(3, base + k), A.blr())
        p.label(name + "_tbl")
        p(*struct.unpack(">I", bytes((0, 2, 4, 0))))
        if name == "inv":
            p.label("inv_def")
            p(A.li(3, -7), A.blr())
        p.end()

    # Code built without optimisation keeps the index in a stack slot and loads it twice, once for the bound check and
    # once for the table: jt_slot. In jt_slot_st the slot is written between the two loads: the bound says nothing
    # of the second value, and the table stays unresolved
    for name in ("slot", "slot_st"):
        p.func("jt_" + name)
        p(stwu(1, -0x40, 1), A.stw(3, 0x20, 1), A.lwz(7, 0x20, 1), cmplwi(6, 7, 2))
        p.branch(lambda at, t: A.bc(at, 12, 4 * 6 + 1, t), name + "_def")
        if name == "slot_st":
            p(A.stw(4, 0x20, 1))
        p(A.lwz(11, 0x20, 1))
        p.branch(lambda at, t: A.lis(12, ha(t)), name + "_tbl")
        p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), name + "_tbl")
        p(A.rlwinm(0, 11, 2, 0, 29), A.lwzx(0, 12, 0), A.mtctr(0), A.bctr())
        p.label(name + "_tbl")
        for k in (0, 1, 2):
            p.branch(lambda at, t: t, "%s_c%d" % (name, k))
        for k in (0, 1, 2):
            p.label("%s_c%d" % (name, k))
            p(A.li(3, 6001 + k), A.addi(1, 1, 0x40), A.blr())
        p.label(name + "_def")
        p(A.li(3, -8), A.addi(1, 1, 0x40), A.blr())
        p.end()

    # One bound check for two tables: a switch on r11, and in two of its cases a second switch on r11 again, emitted
    # without a check (halfword offsets, so that nothing but the bound delimits the table). jt_two: the second
    # table has the first one's bound. Not so in jt_two_w, where a case writes r11 on the way, nor in jt_two_in, where
    # a branch from before the check reaches the second switch: both stay unresolved
    for name in ("two", "two_w", "two_in"):
        p.func("jt_" + name)
        if name == "two_in":
            p(cmplwi(0, 4, 9))
            p.branch(lambda at, t: A.bc(at, 12, 2, t), name + "_second")
        p(A.mr(11, 3), cmplwi(6, 11, 2))
        p.branch(lambda at, t: A.bc(at, 12, 4 * 6 + 1, t), name + "_def")
        p.branch(lambda at, t: A.lis(12, ha(t)), name + "_tbl1")
        p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), name + "_tbl1")
        p(lbzx(0, 12, 11))
        p.branch(lambda at, t: A.lis(12, ha(t)), name + "_base1")
        p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), name + "_base1")
        p(A.rlwinm(0, 0, 2, 0, 29), A.add(12, 12, 0), A.mtctr(12), A.bctr())
        p.label(name + "_base1")
        p(A.li(5, 100))
        p.branch(lambda at, t: A.b(at, t), name + "_second")
        p(A.li(11, 0) if name == "two_w" else A.li(5, 200))
        p.branch(lambda at, t: A.b(at, t), name + "_second")
        p(A.li(3, 7777), A.blr())
        p.label(name + "_second")
        p.branch(lambda at, t: A.lis(12, ha(t)), name + "_tbl2")
        p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), name + "_tbl2")
        p(A.rlwinm(0, 11, 1, 0, 30), lhzx(0, 12, 0))
        p.branch(lambda at, t: A.lis(12, ha(t)), name + "_base2")
        p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), name + "_base2")
        p(A.add(12, 12, 0), A.mtctr(12), A.bctr())
        p.label(name + "_base2")
        for k in (0, 1, 2):
            p.label("%s_d%d" % (name, k))
            p(A.li(3, 7001 + k), A.add(3, 3, 5), A.blr())
        p.label(name + "_tbl1")
        p(*struct.unpack(">I", bytes((0, 2, 4, 0))))
        p.label(name + "_tbl2")
        p(*struct.unpack(">2I", struct.pack(">4H", 0, 12, 24, 12)))   # the fourth: what follows, a plausible offset
        p.label(name + "_def")
        p(A.li(3, -9), A.blr())
        p.end()

    # The same with what a larger function has on the way: the default label stands between the first switch's cases
    # and the second switch (other code branches there, none of it reaches the table), and a case calls a function
    # before the second switch. jt_nv keeps the index in r31, which a callee preserves: resolved. jt_vol keeps it in
    # r11, which a call may overwrite: unresolved
    for name, reg in (("nv", 31), ("vol", 11)):
        p.func("jt_" + name)
        p(A.mflr(10), A.mr(reg, 3), cmplwi(6, reg, 2))
        p.branch(lambda at, t: A.bc(at, 12, 4 * 6 + 1, t), name + "_def")
        p.branch(lambda at, t: A.lis(12, ha(t)), name + "_tbl1")
        p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), name + "_tbl1")
        p(lbzx(0, 12, reg))
        p.branch(lambda at, t: A.lis(12, ha(t)), name + "_base1")
        p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), name + "_base1")
        p(A.rlwinm(0, 0, 2, 0, 29), A.add(12, 12, 0), A.mtctr(12), A.bctr())
        p.label(name + "_base1")
        p(A.li(3, 50))
        p.branch(A.bl, "twice")
        p(A.mr(5, 3))
        p.branch(lambda at, t: A.b(at, t), name + "_second")
        p(A.li(5, 200))
        p.branch(lambda at, t: A.b(at, t), name + "_second")
        p(A.li(3, 7777), A.mtlr(10), A.blr())
        p.label(name + "_def")
        p(A.li(3, -10), A.mtlr(10), A.blr())
        p.label(name + "_second")
        p.branch(lambda at, t: A.lis(12, ha(t)), name + "_tbl2")
        p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), name + "_tbl2")
        p(A.rlwinm(0, reg, 1, 0, 30), lhzx(0, 12, 0))
        p.branch(lambda at, t: A.lis(12, ha(t)), name + "_base2")
        p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), name + "_base2")
        p(A.add(12, 12, 0), A.mtctr(12), A.bctr())
        p.label(name + "_base2")
        for k in (0, 1, 2):
            p.label("%s_d%d" % (name, k))
            p(A.li(3, 9001 + k), A.add(3, 3, 5), A.mtlr(10), A.blr())
        p.label(name + "_tbl1")
        p(*struct.unpack(">I", bytes((0, 4, 6, 0))))
        p.label(name + "_tbl2")
        p(*struct.unpack(">2I", struct.pack(">4H", 0, 16, 32, 16)))
        p.end()

    # Leaf functions MSVC gives no .pdata entry, never called directly: leaf_data is named by a word of .rdata (a
    # vtable slot), leaf_code by the lis / addi pair in take_ptr (a callback handed over)
    p.func("leaf_data")
    p(A.li(3, 77), A.blr())
    p.end()
    p.func("leaf_code")
    p(A.li(3, 88), A.blr())
    p.end()
    p.func("take_ptr")
    p.branch(lambda at, t: A.lis(3, ha(t)), "leaf_code")
    p.branch(lambda at, t: A.addi(3, 3, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), "leaf_code")
    p(A.blr())
    p.end()
    # leaf_far: its two halves are further apart (several addresses prepared together), down a straight run that
    # keeps the register; leaf_stale is no function: the register holds something else by the time of the addi
    for name, v in (("leaf_far", 99), ("leaf_stale", 55)):
        p.func(name)
        p(A.li(3, v), A.blr())
        p.end()
    p.func("take_far")
    p.branch(lambda at, t: A.lis(11, ha(t)), "leaf_far")
    p(A.li(3, 1), A.li(5, 2), A.li(6, 3), A.li(7, 4), A.li(8, 5), A.li(9, 6), A.li(10, 7))
    p.branch(lambda at, t: A.addi(4, 11, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), "leaf_far")
    p(A.blr())
    p.end()
    p.func("take_stale")
    p.branch(lambda at, t: A.lis(11, ha(t)), "leaf_stale")
    p(A.li(11, 0))
    p.branch(lambda at, t: A.addi(4, 11, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), "leaf_stale")
    p(A.blr())
    p.end()
    # leaf_va / leaf_vb: named by a table of addresses in the code section itself (a vtable merged into .text),
    # outside every .pdata function; leaf_lone's address stands alone between instructions: that is no table
    for name, v in (("leaf_va", 41), ("leaf_vb", 42), ("leaf_lone", 43)):
        p.func(name)
        p(A.li(3, v), A.blr())
        p.end()
    p.func("leaf_table")
    p.branch(lambda at, t: t, "leaf_va")
    p.branch(lambda at, t: t, "leaf_vb")
    p(0, A.blr())
    p.branch(lambda at, t: t, "leaf_lone")
    p(A.blr())
    p.end()
    words = p.link()
    funcs = {name: (a, b) for name, a, b in p.funcs}
    return words, {n: (TEXT + 4 * a, TEXT + 4 * b) for n, (a, b) in funcs.items()}, p.labels


def build():
    words, funcs, labels = program()
    # helpers included, like a real .pdata; leaf functions are not in it
    pdata = [(lo_ - TEXT, hi - lo_) for n, (lo_, hi) in funcs.items() if not n.startswith("leaf_")]
    rdata = struct.pack(">3I", 0x12345678, funcs["leaf_data"][0], funcs["jt_abs"][0] + 8)  # the last: into a function
    data, lay = build_xex(words, rdata=rdata, functions=pdata, compression="normal",
                          entry=funcs["sum4"][0] - TEXT)
    img = parse_xex(data)
    assert img.image == lay.image
    return img, funcs, labels


def test_helpers_are_recognised():
    img, funcs, labels = build()
    view = CodeView(img)
    h = find_helpers(view, XenonDecoder(), img.code_ranges())
    kinds = {}
    for e, (hh, reg) in h.items():
        kinds.setdefault(hh.kind, set()).add(reg)
    assert kinds["savegpr"] == kinds["restgpr"] == kinds["savefpr"] == kinds["restfpr"] == set(range(14, 32))
    assert kinds["savevmx"] == kinds["restvmx"] == set(range(14, 32)) | set(range(64, 128))
    assert h[labels["__savegprlr_28"]][1] == 28 and h[labels["__restvmx_126"]][1] == 126
    # nothing else in the program looks like one
    assert all(any(lo_ <= e < hi for n, (lo_, hi) in funcs.items() if n.startswith("__")) for e in h)


def test_discovery_and_lifting(tmp_path):
    img, funcs, labels = build()
    s = recompile(img, str(tmp_path))
    assert s["interp_fallback_insns"] == 0, s["unlifted"]
    # the helpers are not functions; the twenty-one of .pdata are, and the five leaf functions only a pointer names (a
    # word of .rdata, a lis / addi pair, close or apart, a table in the code section); a pointer into the middle of
    # a .pdata function is not a function, nor is what a register no longer points at, nor a lone word between
    # instructions
    assert s["functions"] == 26 and s["pointer_seeds"] == 5
    view, dec = CodeView(img), XenonDecoder()
    assert pointer_seeds(img, view, lambda a: dec.decode(view.u32(a), a)) \
        == {funcs[n][0] for n in ("leaf_data", "leaf_code", "leaf_far", "leaf_va", "leaf_vb")}
    assert len(s["helpers"]) == 8 and s["helper_calls_inlined"] == 8
    assert s["jump_tables"] == 16 and s["jump_table_targets"] == 5 + 6 + 3 + 4 + 3 + 3 + 3 + 3 + 3 + 4 * 3 + 3 * 3
    # jt_slot_st's, the second of jt_two_w, of jt_two_in and of jt_vol, and only those
    assert s["unresolved_bctr"] == 4
    jt = JumpTables(view, lambda a: dec.decode(view.u32(a), a))
    bctrs = {n: next(a for a in range(*funcs[n], 4) if view.u32(a) == A.bctr()) for n in ("jt_slot", "jt_slot_st")}
    assert jt.resolve(bctrs["jt_slot"]) == [labels["slot_c%d" % k] for k in range(3)]
    assert not jt.resolve(bctrs["jt_slot_st"])
    for n in ("two", "two_w", "two_in", "nv", "vol"):
        first, second = (a for a in range(*funcs["jt_" + n], 4) if view.u32(a) == A.bctr())
        assert jt.resolve(first) == [labels[n + "_base1"] + 4 * k for k in ((0, 4, 6) if n in ("nv", "vol")
                                                                              else (0, 2, 4))]
        assert jt.resolve(second) == ([labels["%s_d%d" % (n, k)] for k in range(3)] if n in ("two", "nv") else ())
    text = "".join(p.read_text() for p in sorted(tmp_path.glob("x360_rc_[0-9]*.c")))
    assert "savegpr" in text and "switch (t_)" in text
    table = (tmp_path / "x360_rc_table.c").read_text()
    assert "xe_rc_resume[]" in table and "xe_rc_image_crc" in table


HARNESS = r"""
#include <stdio.h>
#include "xenon_rt.h"
extern const xe_rc_entry xe_rc_resume[];
extern const unsigned xe_rc_resume_count;
static uint8_t mem[%(size)d];
static const uint8_t image[] = {%(image)s};
static xe_machine M;

static xe_rc_fn lookup(uint32_t pc)
{
    unsigned lo = 0, hi = xe_rc_resume_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (xe_rc_resume[mid].entry < pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < xe_rc_resume_count && xe_rc_resume[lo].entry == pc ? xe_rc_resume[lo].fn : NULL;
}

static int run(uint32_t entry)
{
    M.cpu.pc = entry;
    M.cpu.lr = 0xDEAD0000u;
    for (int n = 0; n < 100000; n++) {
        if (M.cpu.pc == 0xDEAD0000u)
            return 0;
        xe_rc_fn f = lookup(M.cpu.pc);
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


def vreg_case(lines, regs):
    for r in regs:
        lines.append("    for (int k = 0; k < 4; k++) M.cpu.vr[%d].u32[k] = 0x%02X000000u + k;" % (r, r))


@pytest.mark.skipif(not shutil.which("cc") or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_helpers_and_tables_run(tmp_path):
    img, funcs, labels = build()
    src = tmp_path / "src"
    recompile(img, str(src))
    cases, checks = [], []
    saved = {28: 0x1111222233334444, 29: 0x5555666677778888, 30: 0x99990000AAAA0000, 31: 0x0123456789ABCDEF}
    a, b, c, d = 0x10, 0x7FFFFFFF00000001, 3, 5
    gpr = dict(saved)
    gpr.update({1: STACK, 3: a, 4: b, 5: c, 6: d})
    cases.append(c_case("sum4", funcs["sum4"][0], b"", gpr=gpr, out_gpr=[1, 3, 28, 29, 30, 31],
                        out_mem=[(STACK - 0x98 + 8 * 14, 8 * 4 + 4)]))
    checks += [("sum4", "r3", (2 * a + 2 * b + c + d) & M64), ("sum4", "r1", STACK)]
    checks += [("sum4", "r%d" % r, v) for r, v in saved.items()]
    area = b"".join(struct.pack(">Q", saved[r]) for r in range(28, 32)) + struct.pack(">I", 0xDEAD0000)
    checks.append(("sum4", ("mem", STACK - 0x98 + 8 * 14), area))
    # fsum: f30 / f31 and v30 / v31 / v126 / v127 keep their values
    cs = c_case("fsum", funcs["fsum"][0], b"", gpr={1: STACK}, fpr={1: 1.5, 2: -4.0, 30: 7.25, 31: -0.125},
                out_fpr=[1, 30, 31], out_mem=[(STACK - 8 - 0x10, 16)])
    head, rest = cs.split("    { int rc = run", 1)
    lines = [head.rstrip("\n")]
    vreg_case(lines, (30, 31, 126, 127))
    cs = "\n".join(lines) + "\n    { int rc = run" + rest
    cs += "\n" + "\n".join('    printf("fsum v%d %%08x%%08x%%08x%%08x\\n", M.cpu.vr[%d].u32[0], M.cpu.vr[%d].u32[1], '
                           'M.cpu.vr[%d].u32[2], M.cpu.vr[%d].u32[3]);' % (r, r, r, r, r) for r in (30, 31, 126, 127))
    cases.append(cs)
    checks.append(("fsum", "f1", struct.unpack(">Q", struct.pack(">d", 1.5 * -4.0 + 1.5))[0]))
    checks.append(("fsum", "f30", struct.unpack(">Q", struct.pack(">d", 7.25))[0]))
    checks.append(("fsum", "f31", struct.unpack(">Q", struct.pack(">d", -0.125))[0]))
    checks.append(("fsum", ("mem", STACK - 0x18), struct.pack(">2d", 7.25, -0.125)))
    for r in (30, 31, 126, 127):
        checks.append(("fsum", "v%d" % r, int("".join("%08x" % ((r << 24) + k) for k in range(4)), 16)))
    for name, inputs, expect in (("jt_abs", range(-1, 7), {0: 10, 1: 20, 2: 30, 3: 40, 4: 50}),
                                 ("jt_byte", range(0, 8), {0: 7, 1: 8, 2: 8, 3: 9, 4: 100, 5: 7}),
                                 ("jt_gcc", range(0, 4), {0: 111, 1: 222, 2: 333}),
                                 ("jt_min", range(8, 16), {10: 1010, 11: 1011, 12: 1012, 13: 1013}),
                                 ("jt_half", range(0, 6), {1: 2001, 2: 2002, 3: 2003}),
                                 ("jt_open", range(0, 3), {0: 3001, 1: 3002, 2: 3003}),
                                 ("jt_ret", range(0, 5), {0: 4001, 1: 4002, 2: 4003, 3: 3, 4: 4}),
                                 ("jt_inv", range(0, 5), {0: 5001, 1: 5002, 2: 5003}),
                                 ("jt_slot", range(0, 5), {0: 6001, 1: 6002, 2: 6003}),
                                 ("jt_two", range(0, 5), {0: 7101, 1: 7202, 2: 7777}),
                                 ("jt_nv", range(0, 5), {0: 9101, 1: 9202, 2: 7777})):
        default = {"jt_abs": -1, "jt_byte": -2, "jt_gcc": -3, "jt_min": -4, "jt_half": -5, "jt_open": 0,
                   "jt_ret": 0, "jt_inv": -7, "jt_slot": -8, "jt_two": -9, "jt_nv": -10}[name]
        for x in inputs:
            tag = "%s_%d" % (name, x & 0xFF)
            cases.append(c_case(tag, funcs[name][0], b"", gpr={1: STACK, 3: x & 0xFFFFFFFF if x >= 0 else x & M64},
                                out_gpr=[3]))
            checks.append((tag, "r3", expect.get(x, default) & M64))
    # the leaf functions are in the pack: entered at their own address, as a call through the pointer would
    for name, value in (("leaf_data", 77), ("leaf_code", 88), ("leaf_far", 99), ("leaf_va", 41), ("leaf_vb", 42)):
        cases.append(c_case(name, funcs[name][0], b"", gpr={3: 0}, out_gpr=[3]))
        checks.append((name, "r3", value))

    harness = tmp_path / "harness.c"
    body = "\n".join(cases).replace("memset(mem, 0, sizeof mem);",
                                    "memset(mem, 0, sizeof mem); memcpy(mem, image, sizeof image);")
    harness.write_text(HARNESS % {"size": MEM_SIZE, "base": MEM, "cases": body,
                                  "image": ",".join(str(x) for x in img.image)})
    exe = str(tmp_path / "t")
    inc = []
    for dd in RUNTIME_DIRS:
        inc += ["-I", dd]
    subprocess.run(["cc", "-std=gnu11", "-O1", "-Wall", "-Werror", "-Wno-unused-label", "-ffp-contract=off"] + inc
                   + ["-o", exe, str(harness)] + sorted(str(x) for x in src.glob("*.c")) + ["-lm"], check=True)
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    res = parse_out(r.stdout)
    for name, dd in res.items():
        assert dd["rc"] == 0 and dd["faults"] == 0, (name, dd)
    for name, what, expect in checks:
        if isinstance(what, tuple):
            got = res[name]["mem"][what[1]]
            assert got == expect, (name, got.hex(), expect.hex())
        else:
            assert res[name][what] == expect, (name, what, hex(res[name][what]), hex(expect))


def test_gpr_writes():
    """What the shared-bound proof takes an instruction to write: the destination field differs by form, and what is
    not known (a call among them) is None."""
    assert gpr_writes(A.addi(9, 3, 1)) == (9,) and gpr_writes(A.lwz(9, 4, 3)) == (9,)
    assert gpr_writes(A.rlwinm(11, 9, 2, 0, 29)) == (11,) and gpr_writes(A.ori(11, 9, 1)) == (11,)
    assert gpr_writes(A.add(3, 4, 5)) == (3,) and gpr_writes(A.mr(11, 9)) == (11,)
    assert gpr_writes(lbzx(0, 12, 9)) == (0,) and gpr_writes(A.lwzu(9, 4, 3)) == (9, 3)
    assert gpr_writes(stwu(9, -16, 1)) == (1,) and gpr_writes(A.stw(9, 0, 1)) == ()
    assert gpr_writes(cmplwi(2, 9, 14)) == () and gpr_writes(A.cmpdi(2, 9, 1)) == ()
    assert gpr_writes(A.mtctr(9)) == () and gpr_writes(A.bctr()) == () and gpr_writes(A.blr()) == ()
    assert gpr_writes(A.b(0x1000, 0x1010)) == () and gpr_writes(A.bl(0x1000, 0x1010)) is None
    assert gpr_writes(A.bctr() | 1) is None and gpr_writes(0) is None and gpr_writes(A.vxor(9, 9, 9)) is None
    assert is_call(A.bl(0x1000, 0x1010)) and is_call(A.bctr() | 1) and is_call(A.blr() | 1)
    assert not is_call(A.b(0x1000, 0x1010)) and not is_call(A.bctr()) and not is_call(0) and not is_call(A.li(3, 1) | 1)


def test_helper_words_match_xenonrecomp():
    """The first words of each helper as assembled here equal the byte patterns XenonRecomp's README publishes for
    MSVC's helpers (hedge-dev/XenonRecomp, MIT): an outside reference for the shapes find_helpers accepts."""
    published = {"__restgprlr_14": "e9c1ff68", "__savegprlr_14": "f9c1ff68", "__restfpr_14": "c9ccff70",
                 "__savefpr_14": "d9ccff70", "__restvmx_14": "3960fee07dcb60ce", "__savevmx_14": "3960fee07dcb61ce",
                 "__restvmx_64": "3960fc00100b60cb", "__savevmx_64": "3960fc00100b61cb"}
    words, funcs, labels = program()
    for name, hexa in published.items():
        k = (labels[name] - TEXT) // 4
        got = "".join("%08x" % w for w in words[k:k + len(hexa) // 8])
        assert got == hexa, (name, got, hexa)


def test_resume_table_in_parts(tmp_path):
    """XBA-284: past resume_split points the table is cut into parts by function address; together they hold the
    same entries as the one array, each address once, every part sorted, and the files link into one library."""
    import re
    img, _, _ = build()
    one, split = tmp_path / "one", tmp_path / "split"
    recompile(img, str(one))
    s = recompile(img, str(split), resume_split=1, resume_part_bytes=0x40)
    row = re.compile(r"\{0x([0-9A-F]{8})u, (\w+)\}")
    whole = row.findall((one / "x360_rc_table.c").read_text().split("xe_rc_resume[]")[1].split("};")[0])
    table = (split / "x360_rc_table.c").read_text()
    assert "xe_rc_resume[]" not in table and "xe_rc_resume_parts[]" in table
    parts, got = sorted(split.glob("x360_rc_resume_*.c")), []
    assert len(parts) > 1 and "xe_rc_resume_part_count = %du;" % len(parts) in table
    assert {p.name for p in parts} <= set(s["files"])
    for p in parts:
        rows = row.findall(p.read_text())
        assert rows == sorted(rows) and "{xe_rc_resume_%s, %du}" % (p.stem[-5:], len(rows)) in table
        got += rows
    assert sorted(got) == sorted(whole) and len({a for a, _ in got}) == len(got)
    assert "xe_rc_resume_count = %du;" % len(whole) in table
    if shutil.which("cc") and sys.platform.startswith("linux"):
        inc = sum((["-I", d] for d in RUNTIME_DIRS), [])
        subprocess.run(["cc", "-shared", "-fPIC", "-O0", "-DXE_RUNTIME=1", "-Wl,--no-undefined"] + inc +
                       ["-o", str(tmp_path / "p.so")] + sorted(str(p) for p in split.glob("*.c")) + ["-lm"],
                       check=True)


def test_discovery_keeps_no_object_per_word(monkeypatch, tmp_path):
    """XBA-293: the decoded-instruction cache stays bounded and a finished function is an array; the lift is the same
    with a cache far smaller than the title."""
    from . import discover360

    img, _, _ = build()
    full = tmp_path / "full"
    small = tmp_path / "small"
    recompile(img, str(full))
    monkeypatch.setattr(discover360, "INSN_CACHE", 8)
    seen = []
    insn = discover360.XenonDiscovery.insn

    def bounded(self, a):
        seen.append(len(self._cache))
        return insn(self, a)

    monkeypatch.setattr(discover360.XenonDiscovery, "insn", bounded)
    recompile(img, str(small))
    assert seen and max(seen) <= 8
    names = sorted(p.name for p in full.iterdir())
    assert names == sorted(p.name for p in small.iterdir())
    assert all((full / n).read_bytes() == (small / n).read_bytes() for n in names)


def test_packed_function_is_the_explored_set():
    from tools.ppc.discover import Function

    from .discover360 import PackedFunction

    f = Function(0x82000000)
    f.addrs.update({0x82000000, 0x82000004, 0x82000010})
    f.calls.add(0x82000100)
    p = PackedFunction(f)
    assert list(p.packed) == list(f.addrs) and p.addrs == f.addrs and p.calls == f.calls
    p.release()
    assert p.addrs == f.addrs


def test_helper_prefilter_rejects_only_what_cannot_match():
    """_match looks at the raw words first (XBA-293): a run with a gap in its registers, or shorter than MIN_REGS, is
    no helper; the same run without the gap is."""
    def found(words):
        data, _ = build_xex(words + [A.blr()], functions=[(0, 4 * (len(words) + 1))], compression="none")
        img = parse_xex(data)
        return find_helpers(CodeView(img), XenonDecoder(text=False), img.code_ranges())

    run = [A.std(r, -0x98 + 8 * (r - 14), 1) for r in range(14, 32)] + [A.stw(12, -8, 1), A.blr()]
    assert len(found(run)) == 18
    gap = list(run)
    gap[1] = A.std(16, -0x90, 1)
    assert 14 not in {reg for _, reg in found(gap).values()}
    assert not found(run[-9:])  # r25..r31: one register short of MIN_REGS


def _nop_tables():
    """MSVC's table code with `nop` (`ori r0, r0, 0`) between its instructions, as Adventure Time has it (82865F80:
    byte offsets, 82B81BA8: halfword offsets): the `nop` names r0, the register the offset was loaded into."""
    nop = A.ori(0, 0, 0)
    p = Prog(TEXT)

    def table_code(name, crf, load, scale):
        p.func(name)
        p(A.addi(11, 3, -2), cmplwi(crf, 11, 4))
        p.branch(lambda at, t: A.bc(at, 12, 4 * crf + 1, t), name + "_def")
        p.branch(lambda at, t: A.lis(12, ha(t)), name + "_tbl")
        p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), name + "_tbl")
        if scale:
            p(A.rlwinm(11, 11, scale, 0, 31 - scale))
        p(load(0, 12, 11))
        p.branch(lambda at, t: A.lis(12, ha(t)), name + "_base")
        p(nop)
        p.branch(lambda at, t: A.addi(12, 12, lo(t) - (0x10000 if lo(t) & 0x8000 else 0)), name + "_base")
        p(nop, A.add(12, 12, 0), A.mtctr(12), A.bctr())
        p.label(name + "_base")
        for v in range(4):
            p.label("%s_c%d" % (name, v))
            p(A.li(3, v), A.blr())
        p.label(name + "_tbl")
        offs = [0, 8, 8, 16, 24]     # byte offsets from the base, two cases shared
        if scale:
            raw = struct.pack(">5H", *offs) + bytes(2)
        else:
            raw = bytes(offs) + bytes(3)
        p(*struct.unpack(">%dI" % (len(raw) // 4), raw))
        p.label(name + "_def")
        p(A.li(3, -1), A.blr())
        p.end()

    table_code("jt_nop_b", 6, lbzx, 0)
    table_code("jt_nop_h", 7, lhzx, 1)
    words = p.link()
    funcs = {n: (TEXT + 4 * a, TEXT + 4 * b) for n, a, b in p.funcs}
    data, _ = build_xex(words, functions=[(a - TEXT, b - a) for a, b in funcs.values()],
                        entry=funcs["jt_nop_b"][0] - TEXT)
    return parse_xex(data), funcs, p.labels


def test_tables_behind_a_nop():
    img, funcs, labels = _nop_tables()
    view, dec = CodeView(img), XenonDecoder()
    jt = JumpTables(view, lambda a: dec.decode(view.u32(a), a))
    for n in ("jt_nop_b", "jt_nop_h"):
        bctr = next(a for a in range(*funcs[n], 4) if view.u32(a) == A.bctr())
        want = [labels["%s_c%d" % (n, c)] for c in (0, 1, 1, 2, 3)]
        assert jt.resolve(bctr) == want, n
    # a nop writes nothing; `ori rX, rY, 0` is a move and does
    assert gpr_writes(A.ori(0, 0, 0)) == () and gpr_writes(A.ori(9, 9, 0)) == ()
    assert gpr_writes(A.ori(9, 3, 0)) == (9,) and gpr_writes(A.ori(9, 9, 1)) == (9,)


def test_spin_hints_lifted(tmp_path):
    """`or 31,31,31` / `or 1,1,1` (the SMT priority hints of a spin-wait) become XE_SPIN_HINT(); `or 2,2,2` (back to
    normal priority), the record form and a plain `or` stay ors."""
    p = Prog(TEXT)
    p.func("spin")
    p(A.li(11, 4), A.mtctr(11))
    p.label("spin_loop")
    p(A.or_(31, 31, 31), A.or_(31, 31, 31), A.or_(1, 1, 1))
    p.branch(lambda at, t: A.bc(at, 16, 0, t), "spin_loop")    # bdnz
    p(A.or_(2, 2, 2), A.or_(31, 31, 31) | 1, A.or_(3, 4, 5), A.blr())
    p.end()
    words = p.link()
    a, b = p.funcs[0][1:]
    data, _ = build_xex(words, functions=[(4 * a, 4 * (b - a))], entry=4 * a)
    s = recompile(parse_xex(data), str(tmp_path))
    assert s["interp_fallback_insns"] == 0
    text = "".join(f.read_text() for f in sorted(tmp_path.glob("x360_rc_[0-9]*.c")))
    assert text.count("XE_SPIN_HINT();") == 3
    assert "R(2) = R(2) | R(2);" in text and "R(31) = R(31) | R(31); RC_CR0(R(31));" in text
    assert "R(3) = R(4) | R(5);" in text
