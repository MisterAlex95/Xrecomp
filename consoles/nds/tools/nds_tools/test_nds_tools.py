"""pytest for the DS spike's recompiler (XBA-223): .nds header, calico's MOD9 load list applied statically, the
ARMv5TE additions to the GBA discovery and lifter. Programs are built here from opcodes (ARM ARM encodings), no
toolchain; the lifted code's behaviour on real code is checked by consoles/nds/tests (nds_rc_hello)."""

import json
import os
import shutil
import struct
import subprocess

import pytest

from nds_tools.lift import NdsDiscovery, NdsLifter, arm_v5_class
from nds_tools.nds import MOD9, Image, NdsError, crc16
from nds_tools.recomp import main as recomp_main

RUNTIME = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "runtime")


def make_nds(arm9: bytes, ram=0x02000000, entry=None):
    """A .nds with ARM9 = arm9 at ram (entry defaults to ram) and a 4-byte ARM7."""
    rom = bytearray(0x200)
    rom[0:12] = b"XBATEST".ljust(12, b"\0")
    rom[12:16] = b"XBAT"
    arm7 = b"\xfe\xff\xff\xea"  # b .
    struct.pack_into("<IIII", rom, 0x20, 0x200, entry if entry is not None else ram, ram, len(arm9))
    struct.pack_into("<IIII", rom, 0x30, 0x200 + len(arm9), 0x02380000, 0x02380000, len(arm7))
    struct.pack_into("<H", rom, 0x15E, crc16(bytes(rom[:0x15E])))
    return bytes(rom) + arm9 + arm7


class Code:
    def __init__(self, base, size=0x400):
        self.base = base
        self.b = bytearray(size)

    def w32(self, addr, v):
        struct.pack_into("<I", self.b, addr - self.base, v & 0xFFFFFFFF)

    def w16(self, addr, v):
        struct.pack_into("<H", self.b, addr - self.base, v & 0xFFFF)


def test_header_and_plain_image():
    c = Code(0x02000000, 0x10)
    c.w32(0x02000000, 0xEAFFFFFE)  # b .
    img = Image(make_nds(bytes(c.b)))
    d = img.header.as_dict()
    assert d["title"] == "XBATEST" and d["game_code"] == "XBAT" and d["header_crc_ok"]
    assert d["arm9"]["ram"] == "0x02000000"
    assert img.loader == "plain"
    assert img.seeds == [(0x02000000, False)]
    assert img.u32(0x02000000) == 0xEAFFFFFE and img.u32(0x02000010) is None


def test_bad_header():
    with pytest.raises(NdsError):
        Image(b"\0" * 0x100)
    rom = bytearray(make_nds(b"\0" * 16))
    struct.pack_into("<I", rom, 0x2C, 0x100000)  # ARM9 size past the end of the file
    with pytest.raises(NdsError):
        Image(bytes(rom))


def test_mod9_load_list():
    """A calico-shaped boot stub: crt0 copied by the stub, then two load-list segments (one to ITCM)."""
    ram, stub = 0x02004000, 0x02004800
    c = Code(ram, 0x1000)
    crt0_src, crt0_dst, crt0_end = 0x02004A00, 0x02001000, 0x02001010
    cursor = 0x02004C00
    lst = 0x02004E00
    c.w32(stub, 0xEA000007)                       # b over the header
    c.w32(stub + 4, MOD9)
    c.w32(stub + 8, 0x00280001)
    c.w32(stub + 12, cursor)
    c.w32(stub + 16, lst)
    c.w32(stub + 20, lst + 24)
    # stub body at +0x24: add ip, pc, #8 ; ldm ip, {r0, r1, r2} ; ldr pc, [pc, #8] ; then the triple, the literal
    c.w32(stub + 0x24, 0xE28FC008)
    c.w32(stub + 0x28, 0xE89C0007)
    c.w32(stub + 0x2C, 0xE59FF008)                # ldr pc, [pc, #8] -> stub + 0x3C
    c.w32(stub + 0x34, crt0_src)
    c.w32(stub + 0x38, crt0_dst)
    c.w32(stub + 0x3C, crt0_end)
    # (the triple sits at stub+0x24+8+8 = stub+0x34; the ldr pc literal at stub+0x2C+8+8 = stub+0x3C: crt0_end,
    # not code -> no seed from it)
    c.w32(crt0_src, 0xE12FFF1E)                   # crt0: bx lr
    # load list: ITCM 0x01FF8000..+8, main 0x02000100..+8 (+ bss)
    c.w32(lst, 0x01FF8000)
    c.w32(lst + 4, 0x01FF8008)
    c.w32(lst + 8, 0x01FF8008)
    c.w32(lst + 12, 0x02000100)
    c.w32(lst + 16, 0x02000108)
    c.w32(lst + 20, 0x02000200)
    c.w32(cursor, 0xE1A00000)                     # ITCM: nop ; bx lr
    c.w32(cursor + 4, 0xE12FFF1E)
    c.w32(cursor + 8, 0xE3A00001)                 # main: mov r0, #1 ; bx lr
    c.w32(cursor + 12, 0xE12FFF1E)
    img = Image(make_nds(bytes(c.b), ram=ram, entry=stub))
    assert img.loader == "calico"
    segs = {(s.vma, len(s.data)) for s in img.segments}
    assert segs == {(0x02001000, 0x10), (0x01FF8000, 8), (0x02000100, 8)}
    assert img.u32(0x02000100) == 0xE3A00001 and img.u32(0x01FF8004) == 0xE12FFF1E
    assert img.u32(stub) is None                  # the stub itself is not run-time code


def lift_one(img, entry, thumb):
    disc = NdsDiscovery(img)
    funcs = disc.run()
    f = next(f for f in funcs if f.entry == entry and f.thumb == thumb)
    return funcs, NdsLifter(img, thumb, f.addrs).function("f", 2, comments=False)


def test_v5_discovery_and_lift():
    base = 0x02000000
    c = Code(base)
    # ARM entry: blx thumb_fn ; clz r1, r2 ; ldrd r4, [r0, #8] ; strd r4, [r0, #16] ; qadd r3, r1, r2 ;
    #            smulbb r5, r1, r2 ; ldr pc, =thumb_tail (odd: interworking)
    thumb_fn, thumb_tail, arm_fn = 0x02000100, 0x02000140, 0x02000200
    c.w32(base, 0xFA000000 | (((thumb_fn - base - 8) >> 2) & 0xFFFFFF))
    c.w32(base + 4, 0xE16F1F12)
    c.w32(base + 8, 0xE1C040D8)
    c.w32(base + 12, 0xE1C041F0)
    c.w32(base + 16, 0xE1023051)
    c.w32(base + 20, 0xE1650281)
    c.w32(base + 24, 0xE59FF000)                  # ldr pc, [pc, #0] -> base + 32
    c.w32(base + 32, thumb_tail | 1)
    # Thumb fn: push {lr} ; blx arm_fn (BL prefix + BLX suffix) ; blx r3 ; pop {pc}
    off = arm_fn - (thumb_fn + 2 + 4)
    c.w16(thumb_fn, 0xB500)
    c.w16(thumb_fn + 2, 0xF000 | ((off >> 12) & 0x7FF))
    c.w16(thumb_fn + 4, 0xE800 | ((off >> 1) & 0x7FF))
    c.w16(thumb_fn + 6, 0x4798)
    c.w16(thumb_fn + 8, 0xBD00)
    c.w16(thumb_tail, 0x4770)                     # bx lr
    c.w32(arm_fn, 0xE12FFF1E)                     # bx lr
    img = Image(make_nds(bytes(c.b), ram=base))
    funcs, arm_c = lift_one(img, base, False)
    keys = {(f.entry, f.thumb) for f in funcs}
    assert {(base, False), (thumb_fn, True), (thumb_tail, True), (arm_fn, False)} <= keys
    assert "RC_BX(0x02000101u);" in arm_c          # BLX imm to Thumb
    assert "r1 = rc_clz(r2);" in arm_c
    assert "RD32(ad_ + 4, 1, 0u)" in arm_c and "WR32(ad_ + 4, r5, 1, 0u)" in arm_c
    assert "rc_qadd" in arm_c and "h->cpu.q |= q_;" in arm_c
    assert "(int32_t)(int16_t)r1 * (int32_t)(int16_t)r2" in arm_c
    assert "RC_BX(0x02000141u);" in arm_c          # LDR pc, =odd: interworking
    _, t_c = lift_one(img, thumb_fn, True)
    assert "uint32_t t_ = (r14 + " in t_c and "RC_BX(t_);" in t_c  # BLX suffix
    assert t_c.count("RC_BX(") >= 3                # BLX suffix, BLX r3, POP {pc}
    assert arm_v5_class(0xE1200070) == "bkpt" and arm_v5_class(0xE0810002) is None


@pytest.mark.skipif(shutil.which("cc") is None, reason="no C compiler")
def test_cli_writes_c_that_compiles(tmp_path):
    base = 0x02000000
    c = Code(base, 0x40)
    c.w32(base, 0xE16F1F12)       # clz r1, r2
    c.w32(base + 4, 0xE1C040D8)   # ldrd r4, [r0, #8]
    c.w32(base + 8, 0xEE070F90)   # mcr p15, 0, r0, c7, c0, 4 (interpreter)
    c.w32(base + 12, 0xE12FFF1E)  # bx lr
    rom = tmp_path / "t.nds"
    rom.write_bytes(make_nds(bytes(c.b), ram=base))
    out = tmp_path / "src"
    assert recomp_main([str(rom), str(out), "--report", str(tmp_path / "r.json")]) == 0
    r = json.loads((tmp_path / "r.json").read_text())
    assert r["functions"] == 1 and r["interp_fallback_insns"] == 1
    assert r["spans"] == [{"lo": "0x02000000", "hi": "0x02000010", "fnv1a": r["spans"][0]["fnv1a"]}]
    files = sorted(p.name for p in out.iterdir())
    assert files == ["nds_rc_02000000a.c", "nds_rc_table.c"]
    for name in files:
        subprocess.run(["cc", "-fsyntax-only", "-Wall", "-Werror", "-Wno-unused-label", "-Wno-unused-variable",
                        "-Wno-unused-but-set-variable", "-I", RUNTIME, str(out / name)], check=True)
