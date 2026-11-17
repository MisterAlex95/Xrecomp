"""REL modules (XBA-264): the relocator, the runtime's module records, and a module lifted at its load address.

A synthetic DOL holds a REL module of ours already linked at BASE (as OSLink leaves it: relocated, its section table
in addresses, on the module list at 0x800030C8) and calls its prolog, which calls back into the DOL (a module-0
reference), reads a pointer to its own function from its data (a self reference) and calls it. The runtime records
the module (gc_run --discovered); gc_tools.recomp relocates the REL file at that base, checks the bytes match and
lifts it; the pack then runs the module's code natively with the same result. No devkitPPC needed."""

import json
import os
import shutil
import struct
import subprocess
import sys

import pytest

from . import CONSOLE  # noqa: F401
from tools.ppc import asm

from . import rel
from .dol import Image, build
from .recomp import load_rels, read_discovered, read_rel_records, recompile
from .test_gc_tools import CC, CFLAGS, RUNTIME, _build_gc_run, _peek

TEXT = 0x80003100
SVC = TEXT + 0x100
BASE = 0x80100000
BSS = 0x80108000
OUT = 0x80005000
REL_ID = 5


def words(ws):
    return struct.pack(">%dI" % len(ws), *ws)


def module():
    """REL id 5: section 1 text (prolog, helper), section 2 data (a pointer to helper), section 3 bss."""
    code = [asm.mflr(30), asm.li(3, 7), 0,                       # bl SVC: module 0 (r3 = 22)
            asm.lis(4, 0), asm.addi(4, 4, 0),                     # r4 = &data (self, ADDR16_HA / LO)
            asm.lwz(5, 0, 4), asm.mtctr(5), asm.bctrl(),          # helper (r3 = 122)
            asm.mtlr(30), asm.blr()]
    helper = 4 * len(code)
    code += [asm.addi(3, 3, 100), asm.blr()]
    code[2] = asm.bl(0, 0) & ~0x03FFFFFC | 1                      # a bl with its offset left for the relocation
    imports = {
        0: [(1, 8, rel.R_PPC_REL24, 0, SVC)],
        REL_ID: [(1, 14, rel.R_PPC_ADDR16_HA, 2, 0), (1, 18, rel.R_PPC_ADDR16_LO, 2, 0),
                 (2, 0, rel.R_PPC_ADDR32, 1, helper)],
    }
    return rel.build(REL_ID, [(words(code), True), (words([0, 0x12345678]), False), (None, False)], imports,
                     prolog=(1, 0), bss_size=0x20)


def program(linked, prolog):
    """The DOL: puts the module on the list, calls its prolog, stores r3 at OUT, idles. SVC: r3 = r3 * 3 + 1."""
    code = [asm.lis(3, BASE >> 16), asm.ori(3, 3, BASE & 0xFFFF),
            asm.lis(4, 0x8000), asm.ori(4, 4, 0x30C8), asm.stw(3, 0, 4), asm.stw(3, 4, 4),
            asm.lis(12, prolog >> 16), asm.ori(12, 12, prolog & 0xFFFF), asm.mtctr(12), asm.bctrl(),
            asm.lis(4, OUT >> 16), asm.ori(4, 4, OUT & 0xFFFF), asm.stw(3, 0, 4)]
    loop = TEXT + 4 * len(code)
    code += [asm.b(loop, loop)]
    code += [0] * ((SVC - TEXT) // 4 - len(code))
    code += [asm.mulli(3, 3, 3), asm.addi(3, 3, 1), asm.blr()]
    return build([(TEXT, words(code))], [(BASE, linked)])


def test_relocate_and_record_format():
    r = rel.Rel(module())
    lo = rel.Loaded(r, BASE, BSS).link()
    assert lo.skipped == [] and r.id == REL_ID
    text = lo.section_bytes(1)
    bl = struct.unpack_from(">I", text, 8)[0]
    assert (bl & 0x03FFFFFC) == ((SVC - (lo.addrs[1] + 8)) & 0x03FFFFFC)
    assert struct.unpack_from(">H", text, 14)[0] == (lo.addrs[2] >> 16) + (1 if lo.addrs[2] & 0x8000 else 0)
    assert struct.unpack_from(">I", lo.section_bytes(2), 0)[0] == lo.addrs[1] + 40
    line = rel.format_record(REL_ID, BASE, BSS, lo.exec_hash(), [(7, 0x80200000)])
    assert rel.parse_record(line) == (REL_ID, BASE, BSS, lo.exec_hash(), ((7, 0x80200000),))
    # a co-loaded module this one does not import is ignored; one it imports but not loaded is skipped
    assert rel.Loaded(r, BASE, BSS).link([]).exec_hash() == lo.exec_hash()


def test_load_rels_checks_the_hash():
    raw = module()
    lo = rel.Loaded(rel.Rel(raw), BASE, BSS).link()
    good = (REL_ID, BASE, BSS, lo.exec_hash(), ())
    bad = (REL_ID, BASE + 0x1000, BSS, lo.exec_hash(), ())
    missing = (9, BASE, BSS, 0, ())
    out, skipped = load_rels([good, bad, missing], {"a.rel": raw})
    assert [o.base for o in out] == [BASE]
    assert len(skipped) == 2 and "differ" in skipped[0] and "not among" in skipped[1]


@pytest.mark.skipif(not shutil.which(CC) or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_module_recorded_then_lifted(tmp_path):
    raw = module()
    lo = rel.Loaded(rel.Rel(raw), BASE, BSS).link()
    prolog = lo.entry_points()[0]
    dol = tmp_path / "t.dol"
    dol.write_bytes(program(lo.os_image(), prolog))
    (tmp_path / "m.rel").write_bytes(raw)
    exe = _build_gc_run(tmp_path)
    disc = tmp_path / "discovered.txt"
    peek = ["--peek", "%08X:1" % OUT]
    r = subprocess.run(exe + [str(dol), "--frames", "3", "--discovered", str(disc)] + peek, capture_output=True,
                       text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert _peek(r.stdout) == [122]
    recs = read_rel_records(str(disc))
    assert recs == [(REL_ID, BASE, BSS, lo.exec_hash(), ())], disc.read_text()
    # the discovered file merges: a second run adds nothing
    r = subprocess.run(exe + [str(dol), "--frames", "3", "--discovered", str(disc)], capture_output=True, text=True,
                       timeout=60)
    assert "discovered +0" in r.stdout, r.stdout
    rels, skipped = load_rels(recs, {"m.rel": raw})
    assert skipped == [] and len(rels) == 1
    s = recompile(Image(dol.read_bytes()), str(tmp_path / "src"), read_discovered(str(disc)), True, rels)
    assert len(s["rels"]) == 1 and s["rels"][0]["functions"] >= 2, json.dumps(s["rels"])
    pack = str(tmp_path / "libt.so")
    srcs = sorted(str(p) for p in (tmp_path / "src").glob("*.c"))
    subprocess.run([CC, "-shared", "-fPIC"] + CFLAGS + ["-Wno-unused-label", "-DGC_RC_COUNT",
                    "-I", RUNTIME, "-o", pack] + srcs + ["-lm"], check=True)
    r = subprocess.run(exe + [str(dol), "--frames", "3", "--stats", "--pack", pack, "--missed",
                              str(tmp_path / "missed.txt")] + peek, capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert _peek(r.stdout) == [122]
    stats = [ln for ln in r.stdout.splitlines() if ln.startswith("stats:")][0].split()
    native, interp = int(stats[2]), int(stats[4])
    assert native > 20 and interp == 0, stats


@pytest.mark.skipif(not shutil.which(CC) or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_cli_lifts_the_recorded_module(tmp_path):
    raw = module()
    lo = rel.Loaded(rel.Rel(raw), BASE, BSS).link()
    dol = tmp_path / "t.dol"
    dol.write_bytes(program(lo.os_image(), lo.entry_points()[0]))
    (tmp_path / "m.rel").write_bytes(raw)
    disc = tmp_path / "d.txt"
    disc.write_text("# test\n80003100\n" + rel.format_record(REL_ID, BASE, BSS, lo.exec_hash()) + "\n"
                    + rel.format_record(REL_ID, BASE, BSS, 0x1234) + "\n")
    env = dict(os.environ, PYTHONPATH=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    r = subprocess.run([sys.executable, "-m", "gc_tools.recomp", str(dol), str(tmp_path / "src"), "--discovered",
                        str(disc), "--rel", str(tmp_path / "m.rel")], env=env, capture_output=True, text=True,
                       check=True)
    s = json.loads(r.stdout)
    assert len(s["rels"]) == 1 and len(s["rel_skipped"]) == 1
    tab = (tmp_path / "src" / "gc_rc_table.c").read_text()
    assert "gc_rc_u1_f_%08X" % lo.entry_points()[0] in tab
