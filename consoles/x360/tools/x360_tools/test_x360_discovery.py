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
from .discover360 import find_helpers
from .recomp import RUNTIME_DIRS, CodeView, recompile
from .test_x360_tools import M64, Prog, c_case, parse_out
from .xenon import XenonDecoder

TEXT = 0x82001000
MEM = 0x82000000         # the harness's memory window holds the image (jump tables are read from it)
MEM_SIZE = 0x20000
STACK = MEM + 0x1F000


def cmplwi(crf, ra, uimm):
    return (10 << 26) | ((crf << 2) << 21) | (ra << 16) | (uimm & 0xFFFF)


def lbzx(rd, ra, rb):
    return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (87 << 1)


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
    words = p.link()
    funcs = {name: (a, b) for name, a, b in p.funcs}
    return words, {n: (TEXT + 4 * a, TEXT + 4 * b) for n, (a, b) in funcs.items()}, p.labels


def build():
    words, funcs, labels = program()
    pdata = [(lo_ - TEXT, hi - lo_) for lo_, hi in funcs.values()]  # helpers included, like a real .pdata
    data, lay = build_xex(words, functions=pdata, compression="normal", entry=funcs["sum4"][0] - TEXT)
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
    # the helpers are not functions; the six real ones are
    assert s["functions"] == 6
    assert len(s["helpers"]) == 8 and s["helper_calls_inlined"] == 8
    assert s["jump_tables"] == 3 and s["jump_table_targets"] == 5 + 6 + 3 and s["unresolved_bctr"] == 0
    text = (tmp_path / "x360_rc_000.c").read_text()
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
                                 ("jt_gcc", range(0, 4), {0: 111, 1: 222, 2: 333})):
        default = {"jt_abs": -1, "jt_byte": -2, "jt_gcc": -3}[name]
        for x in inputs:
            tag = "%s_%d" % (name, x & 0xFF)
            cases.append(c_case(tag, funcs[name][0], b"", gpr={3: x & 0xFFFFFFFF if x >= 0 else x & M64},
                                out_gpr=[3]))
            checks.append((tag, "r3", expect.get(x, default) & M64))

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
