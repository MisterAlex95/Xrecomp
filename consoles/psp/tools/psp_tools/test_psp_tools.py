"""pytest for the PSP spike's recompiler (XBA-224): the Allegrex decoder over ps1_tools', PRX loading (relocations,
module info, import stubs, EBOOT.PBP), the lifter's Allegrex semantics (lifted C compiled with the host cc and run).
Programs are built here from encodings, no toolchain; the whole chain on real PSPSDK code is checked by
consoles/psp/tests (psp_rc_hello)."""

import json
import os
import shutil
import struct
import subprocess

import pytest

from ps1_tools import isa as ps1_isa
from psp_tools.isa import decode
from psp_tools.prx import PSP_LOAD_BASE, PspError, load
from psp_tools.recomp import main as recomp_main

RUNTIME = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "runtime")


# ---- encoders ----
def r_type(rs, rt, rd, sa, funct, op=0):
    return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct


def i_type(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def ext(rt, rs, pos, size):
    return r_type(rs, rt, size - 1, pos, 0x00, op=0x1F)


def ins(rt, rs, pos, size):
    return r_type(rs, rt, pos + size - 1, pos, 0x04, op=0x1F)


def bshfl(rd, rt, sa):
    return r_type(0, rt, rd, sa, 0x20, op=0x1F)


def cop1(fmt, ft, fs, fd, funct):
    return (0x11 << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct


JR_RA, NOP = 0x03E00008, 0


def test_decoder_allegrex_over_ps1():
    cases = {
        ext(5, 4, 4, 8): "ext", ins(7, 6, 8, 12): "ins", bshfl(8, 6, 0x18): "seh", bshfl(8, 6, 0x10): "seb",
        bshfl(9, 4, 0x02): "wsbh", bshfl(9, 4, 0x03): "wsbw", bshfl(10, 6, 0x14): "bitrev",
        r_type(6, 0, 11, 0, 0x16): "clz", r_type(6, 0, 11, 0, 0x17): "clo", r_type(1, 4, 12, 8, 0x02): "rotr",
        r_type(5, 4, 12, 1, 0x06): "rotrv", r_type(6, 0, 13, 0, 0x2C): "max", r_type(6, 8, 14, 0, 0x2D): "min",
        r_type(4, 0, 15, 0, 0x0A): "movz", r_type(4, 1, 15, 0, 0x0B): "movn", r_type(6, 6, 0, 0, 0x1C): "madd",
        r_type(6, 6, 0, 0, 0x2F): "msubu", i_type(0x14, 0, 0, 2): "beql", i_type(0x15, 1, 2, 2): "bnel",
        i_type(0x01, 3, 0x13, 2): "bgezall", i_type(0x01, 3, 0x02, 2): "bltzl", i_type(0x30, 4, 5, 0): "ll",
        i_type(0x38, 4, 5, 0): "sc", i_type(0x31, 29, 2, 8): "lwc1", cop1(16, 2, 1, 3, 0): "add.s",
        cop1(16, 3, 2, 0, 0x3C): "c.lt.s", (0x11 << 26) | (8 << 21) | (1 << 16) | 4: "bc1t",
        cop1(20, 0, 1, 2, 0x20): "cvt.s.w", cop1(16, 0, 3, 4, 0x0D): "trunc.w.s", 0x0000000F: "sync",
        i_type(0x2F, 4, 0x14, 0): "cache", i_type(0x12, 0, 0, 0): "vfpu", i_type(0x32, 4, 0, 0): "vfpu",
        i_type(0x3A, 4, 0, 0): "vfpu", i_type(0x36, 4, 0, 0): "vfpu",
    }
    for word, name in cases.items():
        assert decode(word, 0x08804000).name == name, hex(word)
    # likely branches: the slot runs only when taken; branch targets as the R3000's
    b = decode(i_type(0x14, 1, 2, 3), 0x08804000)
    assert b.cls == "branch" and b.likely and b.target == 0x08804010
    assert decode(i_type(0x01, 3, 0x12, 1), 0).link == 31 and decode(i_type(0x01, 3, 0x02, 1), 0).link is None
    assert decode(i_type(0x14, 5, 5, 4), 0).is_unconditional()  # b likely
    # the encodings ps1_tools gives another meaning (unchanged there: the PS1's output stays the same)
    assert ps1_isa.decode(i_type(0x14, 0, 0, 2)).name == "reserved"
    assert ps1_isa.decode(i_type(0x32, 4, 0, 0)).name == "lwc2"
    assert ps1_isa.decode(i_type(0x12, 0, 0, 0)).cls == "cop2"
    assert decode(ext(5, 4, 4, 8)).dest() == 5 and decode(ins(7, 6, 8, 12)).reads() == {6, 7}
    assert decode(r_type(4, 0, 15, 0, 0x0A)).reads() == {4, 0, 15}
    assert "ext a1, a0, 4, 8" == decode(ext(5, 4, 4, 8)).text()


# ---- a synthetic PRX ----
def make_prx(code, data=(), relocs=(), imports=(), entry=0, pbp=False):
    """A PRX: code words at vaddr 0, then the import stubs (8 bytes each), the NID table, the library name, the
    .lib.stub entry, the module info, data words. relocs: (vaddr, type). imports: NIDs of library "TestLib"."""
    text = list(code)
    stub_at = 4 * len(text)
    text += [JR_RA, NOP] * len(imports)
    nids_at = 4 * len(text)
    text += list(imports)
    name_at = 4 * len(text)
    text += list(struct.unpack("<2I", b"TestLib\0"))
    stubent_at = 4 * len(text)
    text += [name_at, 0x00010000 | 0x0011, 5 | (len(imports) << 16), nids_at, stub_at] if imports else []
    mi_at = 4 * len(text)
    gp = 0x8000
    modinfo = struct.pack("<HH28sIIIII", 0, 0x0101, b"XBA test", gp, 0, 0, stubent_at,
                          stubent_at + (20 if imports else 0))
    text += list(struct.unpack("<13I", modinfo))
    text += list(data)
    seg = struct.pack("<%dI" % len(text), *text)
    relocs = list(relocs) + ([(stubent_at, 2), (stubent_at + 12, 2), (stubent_at + 16, 2)] if imports else [])
    relocs += [(mi_at + 32, 2), (mi_at + 44, 2), (mi_at + 48, 2)]  # gp, stub_top, stub_end
    rel = b"".join(struct.pack("<II", off, typ) for off, typ in relocs)
    shstr = b"\0.text\0.rel.text\0.rodata.sceModuleInfo\0.shstrtab\0"
    ehsize, phoff, seg_off = 52, 52, 0x60
    rel_off = seg_off + len(seg)
    str_off = rel_off + len(rel)
    sh_off = (str_off + len(shstr) + 3) & ~3
    shs = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 1, 6, 0, seg_off, stub_at + 8 * len(imports), 0, 0, 4, 0),       # .text (code + stubs)
        (7, 0x700000A0, 0, 0, rel_off, len(rel), 0, 1, 4, 8),                 # .rel.text
        (17, 1, 2, mi_at, seg_off + mi_at, 52, 0, 0, 4, 0),                   # .rodata.sceModuleInfo
        (39, 3, 0, 0, str_off, len(shstr), 0, 0, 1, 0),                       # .shstrtab
    ]
    hdr = b"\x7fELF\x01\x01\x01" + b"\0" * 9
    hdr += struct.pack("<HHIIIIIHHHHHH", 0xFFA0, 8, 1, entry, phoff, sh_off, 0x10A23001, ehsize, 32, 1, 40,
                       len(shs), 4)
    ph = struct.pack("<8I", 1, seg_off, 0, seg_off + mi_at, len(seg), len(seg) + 64, 5, 16)
    out = bytearray(hdr + ph)
    out += b"\0" * (seg_off - len(out)) + seg + rel + shstr
    out += b"\0" * (sh_off - len(out))
    for sh in shs:
        out += struct.pack("<10I", *sh)
    out = bytes(out)
    if pbp:
        head = b"\0PBP" + struct.pack("<I", 0x10000) + struct.pack("<8I", *([40] * 6 + [40, 40 + len(out)]))
        out = head + out
    return out


def test_prx_relocations_and_stubs():
    base = PSP_LOAD_BASE
    code = [
        i_type(0x0F, 0, 4, 0x0001),          # 0: lui a0, %hi(0xC000)    HI16
        i_type(0x09, 4, 4, 0xC000),          # 4: addiu a0, a0, %lo(0xC000) LO16 (negative: a carry)
        (0x03 << 26) | (0x10 >> 2),          # 8: jal 0x10                R_MIPS_26
        NOP,
        JR_RA, NOP,                           # 0x10: the callee
    ]
    m = load(make_prx(code + [0] * (0xC000 // 4 - len(code)), relocs=[(0, 5), (4, 6), (8, 4)],
                      imports=[0xDEADBEEF, 0x12345678]))
    target = base + 0xC000
    assert m.base == base and m.kind == "prx" and m.name == "XBA test"
    assert m.word(base) == i_type(0x0F, 0, 4, (target + 0x8000) >> 16)
    assert m.word(base + 4) == i_type(0x09, 4, 4, target & 0xFFFF)
    lo = target & 0xFFFF
    assert ((m.word(base) & 0xFFFF) << 16) + (lo - 0x10000 if lo & 0x8000 else lo) == target
    assert m.word(base + 8) == (0x03 << 26) | ((base + 0x10) >> 2) & 0x3FFFFFF
    assert [(i.lib, i.nid) for i in m.imports] == [("TestLib", 0xDEADBEEF), ("TestLib", 0x12345678)]
    for k, imp in enumerate(m.imports):
        assert m.word(imp.stub) == JR_RA and m.word(imp.stub + 4) == (k << 6) | 0x0C
    assert m.entry == base and m.code_lo == base
    # the same module inside an EBOOT.PBP
    p = load(make_prx(code, relocs=[(8, 4)], pbp=True))
    assert p.word(base + 8) == (0x03 << 26) | ((base + 0x10) >> 2) & 0x3FFFFFF
    with pytest.raises(PspError):
        load(b"~PSP" + b"\0" * 100)
    with pytest.raises(PspError):
        load(b"\x7fELF\x01\x01\x01" + b"\0" * 60)


# ---- lifted Allegrex code, compiled and run ----
PROGRAM = [
    i_type(0x0F, 0, 4, 0x1234),             # lui a0, 0x1234
    i_type(0x0D, 4, 4, 0x5678),             # ori a0, a0, 0x5678
    ext(5, 4, 4, 8),                        # ext a1, a0, 4, 8          -> 0x67
    i_type(0x0D, 0, 6, 0xF00F),             # ori a2, zero, 0xF00F
    r_type(4, 0, 7, 0, 0x25),               # or a3, a0, zero
    ins(7, 6, 8, 12),                       # ins a3, a2, 8, 12         -> 0x12300F78
    bshfl(8, 6, 0x18),                      # seh t0, a2                -> 0xFFFFF00F
    bshfl(9, 4, 0x02),                      # wsbh t1, a0               -> 0x34127856
    bshfl(10, 6, 0x14),                     # bitrev t2, a2             -> 0xF00F0000
    r_type(6, 0, 11, 0, 0x16),              # clz t3, a2                -> 16
    r_type(1, 4, 12, 8, 0x02),              # rotr t4, a0, 8            -> 0x78123456
    r_type(6, 8, 13, 0, 0x2C),              # max t5, a2, t0            -> 0xF00F
    r_type(6, 8, 14, 0, 0x2D),              # min t6, a2, t0            -> 0xFFFFF00F
    r_type(4, 0, 15, 0, 0x0A),              # movz t7, a0, zero         -> a0
    r_type(6, 0, 0, 0, 0x13),               # mtlo a2
    r_type(0, 0, 0, 0, 0x11),               # mthi zero
    r_type(6, 6, 0, 0, 0x1C),               # madd a2, a2
    r_type(0, 0, 16, 0, 0x12),              # mflo s0
    r_type(0, 0, 17, 0, 0x10),              # mfhi s1
    i_type(0x14, 0, 0, 2),                  # beql zero, zero, +2 (taken: its slot runs)
    i_type(0x09, 0, 18, 1),                 #   addiu s2, zero, 1
    i_type(0x09, 18, 18, 100),              # addiu s2, s2, 100 (skipped)
    i_type(0x15, 0, 0, 2),                  # bnel zero, zero, +2 (not taken: slot skipped)
    i_type(0x09, 0, 19, 5),                 #   addiu s3, zero, 5 (skipped)
    i_type(0x09, 0, 19, 7),                 # addiu s3, zero, 7
    cop1(4, 6, 1, 0, 0),                    # mtc1 a2, $f1
    cop1(20, 0, 1, 2, 0x20),                # cvt.s.w $f2, $f1          -> 61455.0
    cop1(16, 2, 2, 3, 0x00),                # add.s $f3, $f2, $f2       -> 122910.0
    cop1(16, 3, 2, 0, 0x3C),                # c.lt.s $f2, $f3           -> true
    (0x11 << 26) | (8 << 21) | (1 << 16) | 2,  # bc1t +2
    NOP,
    i_type(0x09, 0, 20, 99),                # addiu s4, zero, 99 (skipped)
    cop1(16, 0, 3, 4, 0x0D),                # trunc.w.s $f4, $f3
    cop1(0, 21, 4, 0, 0),                   # mfc1 s5, $f4              -> 122910
    i_type(0x2B, 29, 6, 0),                 # sw a2, 0(sp)
    i_type(0x23, 29, 22, 0),                # lw s6, 0(sp)
    r_type(31, 0, 0, 0, 0x08),              # jr ra
    i_type(0x23, 29, 23, 0),                #   lw s7, 0(sp) (a load in the slot of a JR: RC_EXIT_LD)
]


def expected_regs():
    a0, a2 = 0x12345678, 0xF00F
    acc = a2 + a2 * a2
    return {4: a0, 5: 0x67, 6: a2, 7: 0x12300F78, 8: 0xFFFFF00F, 9: 0x34127856, 10: 0xF00F0000, 11: 16,
            12: 0x78123456, 13: 0xF00F, 14: 0xFFFFF00F, 15: a0, 16: acc & 0xFFFFFFFF, 17: acc >> 32, 18: 1, 19: 7,
            20: 0, 21: 122910, 22: a2, 23: a2}


HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include "psp_recomp_rt.h"
struct psp_machine { psp_rc_head h; };
int psp_rc_read(psp_machine *m, uint32_t a, int w, uint32_t *v) { (void)m; (void)a; (void)w; (void)v; return 1; }
int psp_rc_write(psp_machine *m, uint32_t a, int w, uint32_t v) { (void)m; (void)a; (void)w; (void)v; return 1; }
int psp_rc_swlr(psp_machine *m, uint32_t a, uint32_t r, int x) { (void)m; (void)a; (void)r; (void)x; return 1; }
RC_FN psp_rc_f_08804000(psp_machine *m_);
int main(void)
{
    static struct psp_machine m;
    m.h.ram = calloc(1, 0x02000000);
    m.h.pc = 0x08804000u;
    m.h.next_pc = m.h.pc + 4;
    m.h.next_event = ~0ull;
    m.h.r[29] = 0x09000000u;
    m.h.r[31] = 0x08800000u;
    psp_rc_f_08804000(&m);
    if (m.h.ld_reg) { m.h.r[m.h.ld_reg] = m.h.ld_val; m.h.ld_reg = 0; }
    printf("exit %u pc %08X\n", m.h.rc_exit, m.h.pc);
    for (int r = 1; r < 32; r++) printf("r%d %08X\n", r, m.h.r[r]);
    return 0;
}
"""


@pytest.mark.skipif(shutil.which("cc") is None, reason="no C compiler")
def test_lifted_allegrex_runs(tmp_path):
    prx = tmp_path / "t.prx"
    prx.write_bytes(make_prx(PROGRAM))
    out = tmp_path / "src"
    assert recomp_main([str(prx), str(out), "--report", str(tmp_path / "r.json")]) == 0
    r = json.loads((tmp_path / "r.json").read_text())
    assert r["functions"] >= 1 and r["interp_fallback_insns"] == 0 and r["vfpu_insns"] == 0
    (tmp_path / "harness.c").write_text(HARNESS)
    exe = tmp_path / "run"
    subprocess.run(["cc", "-std=c11", "-O1", "-ffp-contract=off", "-Wall", "-Werror", "-Wno-unused-label",
                    "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-I", RUNTIME, "-o", str(exe),
                    str(tmp_path / "harness.c")] + [str(p) for p in sorted(out.iterdir())] + ["-lm"], check=True)
    res = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split("\n")
    assert res[0] == "exit 0 pc 08800000"
    regs = {int(line.split()[0][1:]): int(line.split()[1], 16) for line in res[1:] if line}
    for k, v in expected_regs().items():
        assert regs[k] == v, "r%d = %08X, want %08X" % (k, regs[k], v)


@pytest.mark.skipif(shutil.which("cc") is None, reason="no C compiler")
def test_cli_writes_c_that_compiles(tmp_path):
    prx = tmp_path / "t.prx"
    code = [(0x03 << 26) | (0x10 >> 2), NOP, JR_RA, NOP, JR_RA, NOP,
            i_type(0x12, 0, 0, 0), JR_RA, NOP]  # a VFPU word after the functions: not reached, not lifted
    prx.write_bytes(make_prx(code, relocs=[(0, 4)], imports=[0x05572A5F]))
    out = tmp_path / "src"
    assert recomp_main([str(prx), str(out), "--report", str(tmp_path / "r.json")]) == 0
    r = json.loads((tmp_path / "r.json").read_text())
    assert r["module"]["imports"] == 1
    files = sorted(p.name for p in out.iterdir())
    assert files[-1] == "psp_rc_table.c" and files[0].startswith("psp_rc_08804000")
    for name in files:
        subprocess.run(["cc", "-fsyntax-only", "-Wall", "-Werror", "-Wno-unused-label", "-Wno-unused-variable",
                        "-Wno-unused-but-set-variable", "-I", RUNTIME, str(out / name)], check=True)
