"""pytest for the GBA recompiler (XBA-150 / XBA-151): header, discovery on hand-encoded ARM / Thumb, the CLI.

The ROMs are built here from opcodes (ARMv4T encodings, GBATEK), no toolchain needed. The lifted code's behaviour is
checked against the interpreter by consoles/gba/tests (gba_rc_*: lockstep runs of real and random programs).
"""

import json
import os
import shutil
import struct
import subprocess
import sys

import pytest

from gba_tools.discover import Discovery, islands
from gba_tools.recomp import main as recomp_main, read_discovered
from gba_tools.rom import Rom

BASE = 0x08000000


class Img:
    def __init__(self, size=0x400):
        self.b = bytearray(size)

    def w32(self, addr, v):
        struct.pack_into("<I", self.b, addr - BASE, v & 0xFFFFFFFF)

    def w16(self, addr, v):
        struct.pack_into("<H", self.b, addr - BASE, v & 0xFFFF)

    def rom(self):
        return Rom(bytes(self.b))


def arm_b(at, to, link=False):
    return (0xEB000000 if link else 0xEA000000) | (((to - at - 8) >> 2) & 0xFFFFFF)


def thumb_bl(at, to):
    off = to - (at + 4)
    return 0xF000 | ((off >> 12) & 0x7FF), 0xF800 | ((off >> 1) & 0x7FF)


def header_img():
    im = Img()
    im.w32(BASE, arm_b(BASE, BASE + 0xC0))
    im.b[0xA0:0xAC] = b"TESTGAME\0\0\0\0"
    im.b[0xAC:0xB0] = b"ATST"
    im.b[0xB0:0xB2] = b"01"
    im.b[0xB2] = 0x96
    im.b[0xBD] = (-(sum(im.b[0xA0:0xBD]) + 0x19)) & 0xFF
    return im


def keys(d):
    return {f.key for f in d.functions}


def test_header():
    r = header_img().rom()
    h = r.header
    assert h.title == "TESTGAME" and h.game_code == "ATST" and h.maker == "01"
    assert h.checksum_ok
    assert h.entry == BASE + 0xC0
    assert len(r.sha256) == 64


def test_arm_entry_bx_veneer_and_thumb_bl():
    im = header_img()
    a = BASE + 0xC0
    im.w32(a, 0xE28F0001)        # add r0, pc, #1   -> 0x080000C9 (Thumb)
    im.w32(a + 4, 0xE12FFF10)    # bx r0
    t = a + 8                    # Thumb from here
    hi, lo = thumb_bl(t, BASE + 0x200)
    im.w16(t, hi)
    im.w16(t + 2, lo)
    im.w16(t + 4, 0xE7FE)        # b .
    im.w16(BASE + 0x200, 0x2001)  # movs r0, #1
    im.w16(BASE + 0x202, 0x4770)  # bx lr
    d = Discovery(im.rom())
    d.run()
    ks = keys(d)
    assert BASE in ks                       # the cartridge entry, ARM
    assert (t | 1) in ks                    # BX to a known constant, Thumb by bit 0
    assert ((BASE + 0x200) | 1) in ks       # Thumb BL target
    f = next(f for f in d.functions if f.key == t | 1)
    assert f.addrs == {t, t + 2, t + 4}     # stops after `b .`


def test_literal_pool_is_data_and_thumb_pointer_is_weak_entry():
    im = header_img()
    a = BASE + 0xC0
    im.w32(a, 0xE59F0000)        # ldr r0, [pc]      (pool at a+8)
    im.w32(a + 4, 0xEAFFFFFE)    # b .               (the pool follows: never code)
    im.w32(a + 8, BASE + 0x301)  # .word thumb_fn + 1
    im.w16(BASE + 0x300, 0x2002)  # movs r0, #2
    im.w16(BASE + 0x302, 0x4770)  # bx lr
    d = Discovery(im.rom())
    d.run()
    assert (BASE + 0x301) in keys(d)
    assert d.entries[BASE + 0x301] == "weak"
    assert all(a + 8 not in f.addrs for f in d.functions)
    assert d.is_data(a + 8, 4)


def test_weak_pointer_to_data_is_refused():
    im = header_img()
    a = BASE + 0xC0
    im.w32(a, 0xE59F0000)        # ldr r0, [pc]
    im.w32(a + 4, 0xEAFFFFFE)    # b .
    im.w32(a + 8, BASE + 0x301)  # points at garbage
    im.w16(BASE + 0x300, 0xE800)  # an undefined Thumb encoding (BLX suffix on ARMv5)
    d = Discovery(im.rom())
    d.run()
    assert (BASE + 0x301) not in keys(d)
    assert "undefined" in d.rejected[BASE + 0x301]


def test_irq_handler_store():
    im = header_img()
    a = BASE + 0xC0
    im.w32(a, 0xE59F0008)        # ldr r0, [pc, #8]  -> 0x03007FFC
    im.w32(a + 4, 0xE59F1008)    # ldr r1, [pc, #8]  -> handler
    im.w32(a + 8, 0xE5801000)    # str r1, [r0]
    im.w32(a + 12, 0xEAFFFFFE)   # b .
    im.w32(a + 16, 0x03007FFC)
    im.w32(a + 20, BASE + 0x340)
    im.w32(BASE + 0x340, 0xE12FFF1E)  # bx lr
    d = Discovery(im.rom())
    d.run()
    assert (BASE + 0x340) in keys(d)


def test_arm_ldr_pc_jump_table():
    im = header_img()
    a = BASE + 0xC0
    im.w32(a, 0xE3500002)        # cmp r0, #2
    im.w32(a + 4, 0x979FF100)    # ldrls pc, [pc, r0, lsl #2]
    im.w32(a + 8, 0xEAFFFFFE)    # b .  (default)
    cases = [BASE + 0x100, BASE + 0x108, BASE + 0x110]
    for i, c in enumerate(cases):
        im.w32(a + 12 + 4 * i, c)
        im.w32(c, 0xE3A01000 | i)     # mov r1, #i
        im.w32(c + 4, 0xEAFFFFFE)     # b .
    d = Discovery(im.rom())
    d.run()
    f = next(f for f in d.functions if f.key == BASE)
    for c in cases:
        assert c in f.addrs
    assert all(not ((a + 12 + 4 * i) in f.addrs) for i in range(3))


def test_thumb_case_helper_table():
    im = header_img()
    a = BASE + 0xC0
    im.w32(a, 0xE28F0001)        # add r0, pc, #1
    im.w32(a + 4, 0xE12FFF10)    # bx r0
    t = a + 8
    helper = BASE + 0x380
    im.w16(t, 0x2802)            # cmp r0, #2
    im.w16(t + 2, 0xD806)        # bhi default
    hi, lo = thumb_bl(t + 4, helper)
    im.w16(t + 4, hi)
    im.w16(t + 6, lo)
    ret = t + 8
    im.b[ret - BASE:ret - BASE + 4] = bytes([2, 3, 4, 0])  # uqi table: cases at ret + 2 * entry
    for i in range(3):
        im.w16(ret + 4 + 2 * i, 0xE7FE)  # b .
    # __gnu_thumb1_case_uqi
    for i, op in enumerate([0xB402, 0x4671, 0x0849, 0x0049, 0x5C09, 0x0049, 0x448E, 0xBC02, 0x4770]):
        im.w16(helper + 2 * i, op)
    d = Discovery(im.rom())
    d.run()
    assert d.case_helper(helper) == "uqi"
    f = next(f for f in d.functions if f.key == t | 1)
    for e in (2, 3, 4):
        assert ret + 2 * e in f.addrs
    assert ret not in f.addrs and ret + 2 not in f.addrs   # the table itself is data


def test_discovered_entries_and_islands(tmp_path):
    im = header_img()
    a = BASE + 0xC0
    im.w32(a, 0xE12FFF10)        # bx r0 (unknown)
    im.w16(BASE + 0x200, 0x2001)
    im.w16(BASE + 0x202, 0x4770)
    p = tmp_path / "disc.txt"
    p.write_text("# from a run\n0x08000201\n\n")
    addrs = read_discovered(str(p))
    assert addrs == [0x08000201]
    d = Discovery(im.rom(), addrs)
    d.run()
    assert d.entries[0x08000201] == "discovered"
    ents = islands(d.functions)
    assert [e[0] for e in ents] == sorted(e[0] for e in ents)
    # runs of one state never overlap
    for th in (False, True):
        starts = [e[0] for e in ents if e[1] == th]
        assert len(starts) == len(set(starts))


def test_shared_tail_lifted_once():
    im = header_img()
    a = BASE + 0xC0
    im.w32(a, arm_b(a, BASE + 0x100, link=True))       # bl f1
    im.w32(a + 4, arm_b(a + 4, BASE + 0x200, link=True))  # bl f2
    im.w32(a + 8, 0xEAFFFFFE)
    im.w32(BASE + 0x100, 0xE3A00001)                    # f1: mov r0, #1 ; b tail
    im.w32(BASE + 0x104, arm_b(BASE + 0x104, BASE + 0x300))
    im.w32(BASE + 0x200, 0xE3A00002)                    # f2: mov r0, #2 ; b tail
    im.w32(BASE + 0x204, arm_b(BASE + 0x204, BASE + 0x300))
    im.w32(BASE + 0x300, 0xE2800001)                    # tail: add r0, r0, #1 ; bx lr
    im.w32(BASE + 0x304, 0xE12FFF1E)
    d = Discovery(im.rom())
    d.run()
    owners = [f for f in d.functions if BASE + 0x300 in f.addrs]
    assert len(owners) == 1 and owners[0].entry == BASE + 0x300


def test_cli_writes_c_and_table(tmp_path):
    im = header_img()
    a = BASE + 0xC0
    im.w32(a, 0xE3A00005)        # mov r0, #5
    im.w32(a + 4, 0xEAFFFFFE)    # b .
    rom = tmp_path / "t.gba"
    rom.write_bytes(bytes(im.b))
    out = tmp_path / "out"
    out.mkdir()
    (out / "gba_rc_999.c").write_text("stale")
    assert recomp_main([str(rom), str(out)]) == 0
    files = sorted(os.listdir(out))
    assert "gba_rc_table.c" in files and "gba_rc_08000000a.c" in files and "gba_rc_999.c" not in files
    assert all(f.endswith(".c") for f in files)
    table = (out / "gba_rc_table.c").read_text()
    assert "gba_recomp_title_table" in table and Rom(bytes(im.b)).sha256 in table


@pytest.mark.skipif(not shutil.which("cc"), reason="no C compiler")
def test_generated_c_compiles(tmp_path):
    im = header_img()
    a = BASE + 0xC0
    ops = [0xE0910312, 0xE1B00351, 0xE0C10392, 0xE8BD4010, 0xE59F1004, 0x15C02003, 0xE12FFF1E]
    for i, op in enumerate(ops):
        im.w32(a + 4 * i, op)
    rom = tmp_path / "t.gba"
    rom.write_bytes(bytes(im.b))
    out = tmp_path / "out"
    assert recomp_main([str(rom), str(out), "--no-comments"]) == 0
    runtime = os.path.join(os.path.dirname(__file__), "..", "..", "runtime")
    for f in os.listdir(out):
        r = subprocess.run(["cc", "-O1", "-fPIC", "-Wall", "-Werror", "-Wno-unused-variable",
                            "-Wno-unused-but-set-variable", "-I", runtime, "-c", str(out / f), "-o", str(tmp_path / (f + ".o"))],
                           capture_output=True, text=True, check=False)
        assert r.returncode == 0, r.stderr


def test_module_entry_point(tmp_path):
    im = header_img()
    im.w32(BASE + 0xC0, 0xEAFFFFFE)
    rom = tmp_path / "t.gba"
    rom.write_bytes(bytes(im.b))
    env = dict(os.environ)
    env["PYTHONPATH"] = os.path.join(os.path.dirname(__file__), "..") + os.pathsep + env.get("PYTHONPATH", "")
    r = subprocess.run([sys.executable, "-m", "gba_tools.recomp", str(rom), str(tmp_path / "o")], env=env,
                       capture_output=True, text=True, check=True)
    summary = json.loads(r.stdout)
    assert summary["functions"] >= 1 and summary["header"]["game_code"] == "ATST"


@pytest.mark.skipif(not shutil.which("cc"), reason="no C compiler")
def test_prepare_builds_a_pack_with_the_real_lifter(tmp_path):
    """tools.prepare (the app's Prepare, desktop mode) end to end on a synthetic ROM with this lifter."""
    im = header_img()
    a = BASE + 0xC0
    im.w32(a, 0xE28F0001)        # add r0, pc, #1
    im.w32(a + 4, 0xE12FFF10)    # bx r0
    im.w16(a + 8, 0x2001)        # movs r0, #1
    im.w16(a + 10, 0xE7FE)       # b .
    rom = tmp_path / "t.gba"
    rom.write_bytes(bytes(im.b))
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
    r = subprocess.run([sys.executable, "-m", "tools.prepare", str(rom), "--work", str(tmp_path / "work"),
                        "--pack-dir", str(tmp_path / "packs")], cwd=repo, capture_output=True, text=True,
                       check=False, timeout=600)
    assert "XBA_RESULT ok" in r.stdout, r.stdout + r.stderr
    packs = os.listdir(tmp_path / "packs")
    assert len(packs) == 1 and packs[0].startswith("gba-ATST-")
    meta = json.loads((tmp_path / "packs" / packs[0] / "pack.json").read_text())
    assert meta["console"] == "gba" and meta["rom_sha256"] == Rom(bytes(im.b)).sha256


def test_blocks_resume_points_and_event_checks():
    """Resume points (lift.py « blocks »): the entry, a local jump target, a call's return point; the instructions
    inside a block are answered from the mid-block table (instructions to the block's end); one check per segment."""
    from gba_tools.lift import FnLifter
    im = header_img()
    t = BASE + 0x200
    hi, lo = thumb_bl(t + 6, BASE + 0x300)
    code = [0x2000,          # t+0  movs r0, #0          entry
            0x3001,          # t+2  adds r0, #1          loop
            0x6809,          # t+4  ldr r1, [r1]         (memory: a segment ends here)
            hi, lo,          # t+6  bl                   (leaves: t+10 is a return point)
            0x2803,          # t+10 cmp r0, #3
            0xD1F9,          # t+12 bne t+2
            0x4770]          # t+14 bx lr
    for i, op in enumerate(code):
        im.w16(t + 2 * i, op)
    lifter = FnLifter(im.rom(), True, [t + 2 * i for i in range(len(code))])
    c = lifter.function("f", 8, comments=False)
    for lead in (t, t + 2, t + 10):
        assert "case 0x%08Xu: goto L_%08X;" % (lead, lead) in c
        assert "RC_STOP(0x%08Xu" % lead in c
    assert c.count("RC_STOP(") == 3 and "case 0x%08Xu:" % (t + 4) not in c
    # t+2 .. t+8: a block of 4 (adds, ldr, bl hi, bl lo); t+10 .. t+12, t+14 after the branch
    assert "mid_[8] = {0, 0, 3, 2, 1, 0, 2, 1}" in c
    assert "RC_CHECK(0x%08Xu, 2, 0, 2);" % (t + 6) in c   # after the load: the BL prefix half, then the BL's fetch
    assert "RC_STOP(0x%08Xu, 2, 0, 4);" % (t + 2) in c     # adds (1 fetch) and the load's fetch


def test_files_stay_the_same_when_functions_are_added():
    """A function added (or split) by the discovery loop changes the file it lands in, not the others (names and
    contents), so Prepare recompiles one file, not every one after it."""
    import random
    from gba_tools import recomp

    class F:
        def __init__(self, entry, n):
            self.entry, self.thumb, self.addrs = entry, True, set(range(entry, entry + 2 * n, 2))

    rnd = random.Random(5)
    funcs, a = [], BASE
    for _ in range(600):
        n = rnd.randint(5, 400)
        funcs.append(F(a, n))
        a += 2 * n + 2 * rnd.randint(0, 40)
    before = recomp.chunks_of(funcs)
    assert len(before) > 20 and all(sum(len(f.addrs) for f in c) <= recomp.CHUNK_MAX + 400 for c in before)
    key = lambda chunks: {(c[0].entry, tuple(f.entry for f in c)) for c in chunks}
    changed = []
    for k in range(len(funcs) - 1):
        extra = F(funcs[k].entry + 2 * len(funcs[k].addrs), 1)   # in the gap after function k
        changed.append(len(key(recomp.chunks_of(funcs + [extra])) - key(before)))
    assert max(changed) <= 4 and sum(changed) / len(changed) < 1.2
