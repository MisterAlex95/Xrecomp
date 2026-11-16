"""pytest for the PS2 spike's recompiler (XBA-226): the R5900 decoder (against ps1_tools' where MIPS I keeps its
meaning), the MMI table shared with ps2_alu.h, ELF loading, the lifter's R5900 semantics (lifted C compiled with the
host cc and run). Programs are built here from encodings, no toolchain; the whole chain on real PS2SDK code is
checked by consoles/ps2/tests (ps2_rc_hello)."""

import json
import os
import re
import shutil
import struct
import subprocess

import pytest

from ps1_tools import isa as ps1_isa
from ps2_tools.elf import Ps2Error, Executable
from ps2_tools.isa import MMI_OPS, decode
from ps2_tools.recomp import main as recomp_main

RUNTIME = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "runtime")
BASE = 0x00100000


# ---- encoders ----
def r_type(rs, rt, rd, sa, funct, op=0):
    return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct


def i_type(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def mmi(group, rs, rt, rd, sa):
    return r_type(rs, rt, rd, sa, group, op=0x1C)


def cop1(fmt, rt_ft, fs, fd, funct):
    return (0x11 << 26) | (fmt << 21) | (rt_ft << 16) | (fs << 11) | (fd << 6) | funct


NOP = 0
JR_RA = r_type(31, 0, 0, 0, 0x08)


def make_elf(code, sections=True, etype=2):
    """A minimal PS2SDK-like executable: one RWX PT_LOAD at BASE holding the code, a .text section header."""
    body = b"".join(struct.pack("<I", w) for w in code)
    ehdr_size, ph_size, sh_size = 52, 32, 40
    off = 0x80
    shoff = off + len(body) if sections else 0
    shnum = 2 if sections else 0
    out = bytearray(b"\x7fELF" + bytes([1, 1, 1]) + bytes(9))
    out += struct.pack("<HHIIIIIHHHHHH", etype, 8, 1, BASE, ehdr_size, shoff, 0x20920025, ehdr_size, ph_size, 1,
                       sh_size, shnum, 0)
    out += struct.pack("<8I", 1, off, BASE, BASE, len(body), len(body) + 64, 7, 0x80)
    out += bytes(off - len(out)) + body
    if sections:
        out += bytes(sh_size)  # the null section
        out += struct.pack("<10I", 0, 1, 0x6, BASE, off, len(body), 0, 0, 16, 0)
    return bytes(out)


# ---- decoder -----------------------------------------------------------------------------------------------------
def test_decoder_r5900():
    cases = {
        r_type(4, 5, 6, 0, 0x18): ("mult", "muldiv"),       # three-operand MULT (rd = LO)
        r_type(4, 5, 6, 0, 0x2D): ("daddu", "alu"),
        r_type(0, 5, 6, 3, 0x3C): ("dsll32", "alu"),
        r_type(4, 5, 6, 0, 0x0A): ("movz", "alu"),
        i_type(0x1E, 29, 4, 16): ("lq", "load"),
        i_type(0x1F, 29, 4, 16): ("sq", "store"),
        i_type(0x37, 29, 4, 8): ("ld", "load"),
        i_type(0x3F, 29, 4, 8): ("sd", "store"),
        i_type(0x27, 29, 4, 8): ("lwu", "load"),
        i_type(0x19, 4, 5, -1): ("daddiu", "alu"),
        i_type(0x15, 4, 5, 2): ("bnel", "branch"),
        i_type(0x01, 4, 0x02, 2): ("bltzl", "branch"),
        mmi(0x08, 4, 5, 6, 0x00): ("paddw", "mmi"),
        mmi(0x09, 4, 5, 6, 0x0E): ("pcpyld", "mmi"),
        mmi(0x29, 4, 5, 6, 0x0E): ("pcpyud", "mmi"),
        mmi(0x09, 0, 0, 6, 0x08): ("pmfhi", "mmi"),
        mmi(0x30, 0, 0, 6, 0x00): ("pmfhl.lw", "mmi"),
        mmi(0x18, 4, 5, 6, 0): ("mult1", "muldiv"),
        mmi(0x12, 0, 0, 6, 0): ("mflo1", "alu"),
        mmi(0x04, 4, 0, 6, 0): ("plzcw", "alu"),
        mmi(0x09, 4, 5, 6, 0x0C): ("mmi2.0c", "interp"),    # PMULTW: not in the spike
        r_type(0, 0, 0, 0, 0x0C): ("syscall", "interp"),
        r_type(4, 5, 0, 7, 0x34): ("teq", "trap"),
        cop1(16, 2, 1, 0, 0x1A): ("mula.s", "fpu"),
        cop1(16, 2, 1, 3, 0x1C): ("madd.s", "fpu"),
        cop1(16, 2, 1, 3, 0x28): ("max.s", "fpu"),
        cop1(16, 2, 1, 0, 0x34): ("c.lt.s", "fpu"),
        cop1(20, 0, 1, 2, 0x20): ("cvt.s.w", "fpu"),
        (0x11 << 26) | (8 << 21) | (3 << 16) | 2: ("bc1tl", "branch"),
        (0x10 << 26) | (0x10 << 21) | 0x38: ("ei", "interp"),
        (0x12 << 26) | (1 << 25): ("cop2", "interp"),
        i_type(0x36, 4, 1, 0): ("lqc2", "interp"),
    }
    for word, (name, cls) in cases.items():
        i = decode(word, BASE)
        assert (i.name, i.cls) == (name, cls), "%08X: %s %s, want %s %s" % (word, i.name, i.cls, name, cls)
    b = decode(i_type(0x15, 4, 5, 2), BASE)
    assert b.likely and b.target == BASE + 12
    assert decode(i_type(0x14, 0, 0, 1), BASE).is_unconditional()
    assert not decode(i_type(0x01, 0, 0x13, 1), BASE).is_unconditional()  # BGEZALL zero is a call
    assert decode(r_type(4, 5, 6, 0, 0x18)).dest() == 6
    assert decode(i_type(0x1E, 29, 4, 16)).dest() == 4 and decode(i_type(0x1E, 29, 4, 16)).ldest() is None


def test_decoder_keeps_mips1_meaning():
    """Where MIPS I and the R5900 agree, both decoders give the same name, registers and target."""
    words = [r_type(4, 5, 6, 0, f) for f in (0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x10, 0x12, 0x1A, 0x1B, 0x20, 0x21,
                                             0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B)]
    words += [i_type(op, 4, 5, 0x8004) for op in (0x04, 0x05, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x20,
                                                 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E)]
    words += [i_type(0x06, 4, 0, 3), i_type(0x07, 4, 0, 3), i_type(0x01, 4, 0, 3), i_type(0x01, 4, 0x11, 3),
              (0x03 << 26) | 0x40000, r_type(31, 0, 0, 0, 0x08), r_type(4, 0, 31, 0, 0x09)]
    for w in words:
        a, b = ps1_isa.decode(w, BASE), decode(w, BASE)
        assert (a.name, a.rs, a.rt, a.rd, a.target, a.link) == (b.name, b.rs, b.rt, b.rd, b.target, b.link), \
            "%08X: %s vs %s" % (w, a.name, b.name)


def test_mmi_table_matches_runtime():
    """ps2_tools.isa.MMI_OPS and PS2_MMI_LIST (ps2_alu.h) list the same (funct, sa, name) triples."""
    with open(os.path.join(RUNTIME, "ps2_alu.h"), encoding="utf-8") as f:
        text = f.read()
    body = text[text.index("#define PS2_MMI_LIST(X)"):]
    body = body[:body.index("\n\n")]
    c_side = {(int(f, 16), int(s, 16), n) for f, s, n in re.findall(r"X\((0x\w+), (0x\w+), (\w+)\)", body)}
    py_side = {(f, s, n) for f, ops in MMI_OPS.items() for s, n in ops.items()}
    assert c_side == py_side
    for _, _, n in py_side:
        assert "ps2_mmi_%s(" % n in text.replace("PS2_MMI_LANEWISE(%s," % n, "ps2_mmi_%s(" % n).replace(
            "PS2_MMI_SHUFFLE(%s," % n, "ps2_mmi_%s(" % n), n


# ---- ELF -----------------------------------------------------------------------------------------------------------
def test_elf_loading():
    code = [JR_RA, NOP, 0x11223344]
    exe = Executable("t.elf", make_elf(code))
    assert exe.entry == BASE and (exe.code_lo, exe.code_hi) == (BASE, BASE + 12)
    assert exe.code() == struct.pack("<3I", *code)
    assert len(exe.image) == 12 + 64 and exe.image[12:] == bytes(64)  # memsz beyond filesz is zeros
    exe = Executable("t.elf", make_elf(code, sections=False))
    assert (exe.code_lo, exe.code_hi) == (BASE, BASE + 12 + 64)  # no sections: the executable segment
    with pytest.raises(Ps2Error):
        Executable("t.elf", make_elf(code, etype=1))  # relocatable (an IRX): refused
    with pytest.raises(Ps2Error):
        Executable("t.elf", b"\x7fELF" + bytes(60))


# ---- lifted semantics --------------------------------------------------------------------------------------------
PROGRAM = [
    i_type(0x0F, 0, 8, 0x8000),             # lui t0, 0x8000          -> FFFFFFFF80000000
    i_type(0x19, 0, 9, -1),                 # daddiu t1, zero, -1     -> FFFFFFFFFFFFFFFF
    r_type(0, 9, 10, 0, 0x3E),              # dsrl32 t2, t1, 0        -> 00000000FFFFFFFF
    r_type(10, 0, 11, 0, 0x21),             # addu t3, t2, zero       -> FFFFFFFFFFFFFFFF (sign-extended)
    r_type(0, 10, 12, 4, 0x3C),             # dsll32 t4, t2, 4        -> FFFFFFF000000000
    r_type(0, 12, 13, 4, 0x03),             # sra t5, t4, 4           -> 0 (the low word)
    r_type(8, 10, 14, 0, 0x2D),             # daddu t6, t0, t2        -> 000000007FFFFFFF
    r_type(10, 10, 15, 0, 0x18),            # mult t7, t2, t2         -> 1 (rd = LO)
    i_type(0x3F, 29, 12, 0),                # sd t4, 0(sp)
    i_type(0x27, 29, 16, 4),                # lwu s0, 4(sp)           -> 00000000FFFFFFF0
    i_type(0x23, 29, 17, 4),                # lw s1, 4(sp)            -> FFFFFFFFFFFFFFF0
    mmi(0x09, 12, 10, 4, 0x0E),             # pcpyld a0, t4, t2       -> hi t4, lo t2
    i_type(0x1F, 29, 4, 16 + 5),            # sq a0, 21(sp)           (the low 4 address bits are ignored)
    i_type(0x1E, 29, 5, 16),                # lq a1, 16(sp)
    mmi(0x29, 5, 5, 6, 0x0E),               # pcpyud a2, a1, a1       -> lo = hi = a1.hi
    mmi(0x08, 5, 5, 7, 0x00),               # paddw a3, a1, a1
    i_type(0x0F, 0, 18, 0x7F80),            # lui s2, 0x7F80          (2^128 on the R5900)
    cop1(4, 18, 1, 0, 0),                   # mtc1 s2, $f1
    i_type(0x0F, 0, 19, 0x4000),            # lui s3, 0x4000          (2.0)
    cop1(4, 19, 2, 0, 0),                   # mtc1 s3, $f2
    cop1(16, 2, 1, 3, 0x02),                # mul.s $f3, $f1, $f2     -> clamped: 7FFFFFFF
    cop1(0, 20, 3, 0, 0),                   # mfc1 s4, $f3
    cop1(16, 0, 2, 4, 0x03),                # div.s $f4, $f2, $f0     -> 2 / 0: 7FFFFFFF
    cop1(0, 21, 4, 0, 0),                   # mfc1 s5, $f4
    i_type(0x15, 0, 0, 2),                  # bnel zero, zero, +2 (not taken: slot skipped)
    i_type(0x09, 0, 22, 5),                 #   addiu s6, zero, 5 (skipped)
    i_type(0x09, 0, 23, 7),                 # addiu s7, zero, 7
    JR_RA,
    NOP,
]


def expected():
    t4 = 0xFFFFFFF000000000
    lo = {8: 0xFFFFFFFF80000000, 9: 0xFFFFFFFFFFFFFFFF, 10: 0x00000000FFFFFFFF, 11: 0xFFFFFFFFFFFFFFFF, 12: t4,
          13: 0, 14: 0x7FFFFFFF, 15: 1, 16: 0xFFFFFFF0, 17: 0xFFFFFFFFFFFFFFF0, 4: 0xFFFFFFFF, 5: 0xFFFFFFFF,
          6: t4, 7: 0x00000000FFFFFFFE, 20: 0x7FFFFFFF, 21: 0x7FFFFFFF, 22: 0, 23: 7}
    hi = {4: t4, 5: t4, 6: t4, 7: 0xFFFFFFE000000000}
    return lo, hi


HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include "ps2_recomp_rt.h"
struct ps2_machine { ps2_rc_head h; };
int ps2_rc_read(ps2_machine *m, uint32_t a, int w, void *v) { (void)m; (void)a; (void)w; (void)v; return 1; }
int ps2_rc_write(ps2_machine *m, uint32_t a, int w, const void *v) { (void)m; (void)a; (void)w; (void)v; return 1; }
int ps2_rc_unaligned(ps2_machine *m, int k, uint32_t a, uint64_t r, uint64_t *o)
{ (void)m; (void)k; (void)a; (void)r; (void)o; return 1; }
RC_FN ps2_rc_f_00100000(ps2_machine *m_);
int main(void)
{
    static struct ps2_machine m;
    m.h.ram = calloc(1, 0x02000000);
    m.h.pc = 0x00100000u;
    m.h.next_pc = m.h.pc + 4;
    m.h.next_event = ~0ull;
    m.h.r[29] = 0x01000000u;
    m.h.r[31] = 0x00200000u;
    ps2_rc_f_00100000(&m);
    printf("exit %u pc %08X\n", m.h.rc_exit, m.h.pc);
    for (int r = 1; r < 32; r++)
        printf("r%d %016llX %016llX\n", r, (unsigned long long)m.h.r[r], (unsigned long long)m.h.rh[r]);
    return 0;
}
"""

CFLAGS = ["-std=c11", "-O1", "-ffp-contract=off", "-Wall", "-Werror", "-Wno-unused-label", "-Wno-unused-variable",
          "-Wno-unused-but-set-variable"]


@pytest.mark.skipif(shutil.which("cc") is None, reason="no C compiler")
def test_lifted_r5900_runs(tmp_path):
    elf = tmp_path / "t.elf"
    elf.write_bytes(make_elf(PROGRAM))
    out = tmp_path / "src"
    assert recomp_main([str(elf), str(out), "--report", str(tmp_path / "r.json")]) == 0
    r = json.loads((tmp_path / "r.json").read_text())
    assert r["functions"] == 1 and r["interp_fallback_insns"] == 0
    assert r["mmi_and_cop2"] == {"paddw": 1, "pcpyld": 1, "pcpyud": 1}
    (tmp_path / "harness.c").write_text(HARNESS)
    exe = tmp_path / "run"
    subprocess.run(["cc"] + CFLAGS + ["-I", RUNTIME, "-o", str(exe), str(tmp_path / "harness.c")]
                   + [str(p) for p in sorted(out.iterdir())] + ["-lm"], check=True)
    res = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split("\n")
    assert res[0] == "exit 0 pc 00200000"
    regs = {int(ln.split()[0][1:]): (int(ln.split()[1], 16), int(ln.split()[2], 16)) for ln in res[1:] if ln}
    lo, hi = expected()
    for k, v in lo.items():
        assert regs[k][0] == v, "r%d = %016X, want %016X" % (k, regs[k][0], v)
    for k, v in hi.items():
        assert regs[k][1] == v, "r%d high = %016X, want %016X" % (k, regs[k][1], v)


@pytest.mark.skipif(shutil.which("cc") is None, reason="no C compiler")
def test_cli_writes_c_that_compiles(tmp_path):
    elf = tmp_path / "t.elf"
    code = [(0x03 << 26) | ((BASE + 0x10) >> 2), NOP, JR_RA, NOP,
            r_type(0, 0, 0, 0, 0x0C), JR_RA, NOP,                 # a function with a SYSCALL: to the interpreter
            mmi(0x09, 4, 5, 6, 0x0C), JR_RA, NOP]                 # PMULTW: not in the spike, to the interpreter
    elf.write_bytes(make_elf(code))
    out = tmp_path / "src"
    assert recomp_main([str(elf), str(out), "--report", str(tmp_path / "r.json")]) == 0
    r = json.loads((tmp_path / "r.json").read_text())
    assert r["elf"]["entry"] == "0x00100000"
    files = sorted(p.name for p in out.iterdir())
    assert files[-1] == "ps2_rc_table.c" and files[0].startswith("ps2_rc_00100000")
    for name in files:
        subprocess.run(["cc", "-fsyntax-only"] + CFLAGS[3:] + ["-I", RUNTIME, str(out / name)], check=True)
