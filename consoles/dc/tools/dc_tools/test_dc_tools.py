"""pytest for the Dreamcast spike's tools (XBA-227): the SH-4 table, its generated C header, the disassembler, the
lifter's discovery (calls, jump tables, register branches, exception vectors) and emission. Programs are built here
from opcodes (SH-4 manual encodings), no toolchain; the lifted code's behaviour is checked by consoles/dc/tests
(dc_rc_sh4_ops on synthetic programs, dc_rc_hello on our KallistiOS hello world)."""

import json
import os
import struct

from dc_tools import sh4
from dc_tools.image import BIN_BASE, load_bin
from dc_tools.lift import Lifter, gpr_writes
from dc_tools.recomp import fnv1a, main as recomp_main

RUNTIME = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "runtime")


def test_generated_header_is_current():
    with open(os.path.join(RUNTIME, "sh4_optab.h"), encoding="utf-8") as f:
        assert f.read() == sh4.gen_c_header(), "regenerate: python3 -m dc_tools.sh4 --gen-c runtime/sh4_optab.h"


def test_table_is_unambiguous_and_names_unique():
    names = [o[2] for o in sh4.OPS]
    assert len(names) == len(set(names))
    for mask, match, name, _fmt, _flags in sh4.OPS:
        assert match & ~mask == 0, name
        assert sh4.decode(match).name == name, name


def test_decode_precedence():
    assert sh4.decode(0x0009).name == "NOP"
    assert sh4.decode(0x000B).name == "RTS"
    assert sh4.decode(0xF3FD).name == "FSCHG"
    assert sh4.decode(0xFBFD).name == "FRCHG"
    assert sh4.decode(0xF1FD).name == "FTRV"  # fv0
    assert sh4.decode(0xFDFD).name == "FTRV"  # fv12
    assert sh4.decode(0xF0FD).name == "FSCA"  # dr0
    assert sh4.decode(0xFEFD).name == "FSCA"  # dr14
    assert sh4.decode(0xF4ED).name == "FIPR"
    assert sh4.decode(0x0082).name == "STC_BANK"
    assert sh4.decode(0x4F87).name == "LDCL_BANK"
    assert sh4.decode(0xFFFF) is None
    assert sh4.decode(0x0173) is None  # movco.l (SH-4A): not on the SH7091
    assert sum(1 for op in range(65536) if sh4.decode(op)) > 58000


def test_disasm():
    assert sh4.disasm(0x8C010000, 0xD037) == "mov.l\t@(0x8c0100e0),r0"
    assert sh4.disasm(0x8C0100A4, 0xD00B) == "mov.l\t@(0x8c0100d4),r0"
    assert sh4.disasm(0x8C010000, 0x8D01) == "bt/s\t0x8c010006"
    assert sh4.disasm(0x8C010000, 0x1E31) == "mov.l\tr3,@(4,r14)"
    assert sh4.disasm(0x8C010000, 0x0582) == "stc\tr0_bank,r5"
    assert sh4.disasm(0x8C010000, 0xF4FD) == "fsca\tfpul,dr4"
    assert sh4.disasm(0x8C010000, 0xFFFF) == ".word 0xffff"


def test_gpr_writes():
    assert gpr_writes("MOVL_S", 0x2412) == set()
    assert gpr_writes("MOVL_LP", 0x6146) == {1, 4}
    assert gpr_writes("MOVA", 0xC701) == {0}
    assert gpr_writes("ADD", 0x312C) == {1}
    assert gpr_writes("FMOV_LP", 0xF119) == {1}


class Asm:
    """A tiny SH-4 program at BIN_BASE: words, labels and a literal pool."""

    def __init__(self):
        self.w = []

    @property
    def pc(self):
        return BIN_BASE + 2 * len(self.w)

    def op(self, *ops):
        self.w.extend(ops)

    def image(self, size=0x400):
        b = bytearray(size)
        for i, v in enumerate(self.w):
            struct.pack_into("<H", b, 2 * i, v)
        return b


def make_program(size=0x900):
    """main: bsr sub; jsr @r1 (literal: other); a 3-case switch on r4 (GCC's mova / mov.b / extu.b / braf, bounded
    by cmp/hi); ldc r0,vbr (literal: the vectors at VBR + 0x100 / 0x400 / 0x600); rts."""
    a = Asm()
    a.op(0x4F22)  # 000 sts.l pr,@-r15
    a.op(0xB000)  # 002 bsr sub (patched)
    a.op(0x0009)  # 004 nop
    a.op(0xD100)  # 006 mov.l @(lit_other),r1 (patched)
    a.op(0x410B)  # 008 jsr @r1
    a.op(0x0009)  # 00a nop
    a.op(0xE202)  # 00c mov #2,r2
    a.op(0x3426)  # 00e cmp/hi r2,r4
    a.op(0x8900)  # 010 bt end (patched)
    a.op(0xC700)  # 012 mova table,r0 (patched)
    a.op(0x044C)  # 014 mov.b @(r0,r4),r4
    a.op(0x644C)  # 016 extu.b r4,r4
    a.op(0x0423)  # 018 braf r4
    a.op(0x0009)  # 01a nop
    a.op(0x0A04, 0x0010)  # 01c table: offsets 4, 10, 16 from 0x01c -> 0x020, 0x026, 0x02c
    a.op(0xE30A)  # 020 case 0: mov #10,r3
    a.op(0xA004)  # 022 bra end
    a.op(0x0009)  # 024 nop
    a.op(0xE314)  # 026 case 1: mov #20,r3
    a.op(0xA001)  # 028 bra end
    a.op(0x0009)  # 02a nop
    a.op(0xE31E)  # 02c case 2: mov #30,r3 (falls into end)
    a.op(0xD000)  # 02e end: mov.l @(lit_vbr),r0 (patched)
    a.op(0x402E)  # 030 ldc r0,vbr
    a.op(0x4F26)  # 032 lds.l @r15+,pr
    a.op(0x000B)  # 034 rts
    a.op(0x0009)  # 036 nop
    a.op(0, 0)  # 038 lit_other
    a.op(0, 0)  # 03c lit_vbr
    a.op(0x000B, 0x0009)  # 040 sub: rts; nop
    a.op(0x000B, 0x0009)  # 044 other: rts; nop
    b = BIN_BASE
    info = dict(sub=b + 0x40, other=b + 0x44, end=b + 0x2E, vbr=b + 0x200)
    img = a.image(size)

    def patch(at, v):
        struct.pack_into("<H", img, at, struct.unpack_from("<H", img, at)[0] | v)

    patch(0x002, ((0x040 - 0x006) // 2) & 0xFFF)
    patch(0x006, (0x038 - 0x004 - 4) // 4)
    patch(0x010, ((0x02E - 0x014) // 2) & 0xFF)
    patch(0x012, (0x01C - 0x010 - 4) // 4)
    patch(0x02E, (0x03C - 0x02C - 4) // 4)
    struct.pack_into("<I", img, 0x038, info["other"])
    struct.pack_into("<I", img, 0x03C, info["vbr"])
    for off in (0x100, 0x400, 0x600):
        struct.pack_into("<HH", img, 0x200 + off, 0x002B, 0x0009)  # rte; nop
    return bytes(img), info


def test_discovery_calls_tables_vectors():
    data, info = make_program()
    lf = Lifter(load_bin(data))
    lf.run()
    main = lf.funcs[BIN_BASE]
    assert info["sub"] in lf.funcs and info["other"] in lf.funcs
    assert main.guesses[BIN_BASE + 0x008] == info["other"]
    assert BIN_BASE + 0x00C in main.table  # the JSR's return site
    assert BIN_BASE + 0x006 in main.table  # the BSR's return site
    cases = main.jtables[BIN_BASE + 0x018]
    assert cases == [BIN_BASE + 0x020, BIN_BASE + 0x026, BIN_BASE + 0x02C]
    for v in (0x100, 0x400, 0x600):
        assert info["vbr"] + v in lf.funcs


def test_emission_shapes():
    data, info = make_program()
    lf = Lifter(load_bin(data))
    lf.run()
    src, n = lf.emit_function(lf.funcs[BIN_BASE], comments=True)
    assert "RC_CALL(0x%08xu, dc_fn_%08x, 0x%08xu, L_%08x);" % (
        info["sub"], info["sub"], BIN_BASE + 6, BIN_BASE + 6) in src
    assert "RC_CALL_DYN_G(t_, 0x%08xu, dc_fn_%08x" % (info["other"], info["other"]) in src
    assert "case 0x%08xu: goto L_%08x;" % (BIN_BASE + 0x026, BIN_BASE + 0x026) in src
    assert "RC_RET(t_);" in src
    assert "sh4i_LDC_VBR(c, " in src
    assert n > 10


def test_recomp_writes_a_pack(tmp_path):
    data, _info = make_program()
    prog = tmp_path / "prog.bin"
    prog.write_bytes(data)
    disc = tmp_path / "disc.txt"
    disc.write_text("# dc_run --missed\n%08X\n" % (BIN_BASE + 0x024))
    out = tmp_path / "out"
    assert recomp_main([str(prog), str(out), "--discovered", str(disc), "--report", str(tmp_path / "r.json")]) == 0
    table = (out / "table.c").read_text()
    addrs = [int(line.split("{")[1].split(",")[0].rstrip("u"), 16) for line in table.splitlines() if line.startswith("    {0x8c")
             and "u, dc_fn_" in line]
    assert addrs == sorted(addrs) and BIN_BASE in addrs and BIN_BASE + 0x024 in addrs
    assert "0x%08xu, 0x%08xu, 0x%08xu" % (BIN_BASE, BIN_BASE + len(data), fnv1a(data)) in table
    rep = json.loads((tmp_path / "r.json").read_text())
    assert rep["jump_tables"] == 1 and rep["functions"] >= 6
    assert (out / "title.h").exists() and list(out.glob("rc_*.c"))
