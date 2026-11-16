"""Tests of the GameCube spike's recompiler (XBA-225): DOL, the Gekko layer, and a synthetic program run both in the
interpreter and recompiled (gc_run and the pack built from source with the host cc). No devkitPPC needed."""

import os
import shutil
import struct
import subprocess
import sys

import pytest

from . import CONSOLE  # noqa: F401
from tools.ppc import asm

from .dol import DolError, Image, build
from .gekko import GekkoDecoder
from .recomp import code_constants, recompile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
RUNTIME = os.path.join(REPO, "consoles", "gc", "runtime")
TEXT = 0x80003100
DATA = 0x80004000
OUT = 0x80005000


def f32(*vals):
    return struct.pack(">%df" % len(vals), *vals)


def program():
    """Paired singles, quantisation, FP, a counted loop, a call, carries, CR logic; results at OUT, then idle."""
    code = []

    def at():
        return TEXT + 4 * len(code)

    code += [asm.lis(3, 0x8000), asm.ori(3, 3, 0x4000), asm.lis(4, 0x8000), asm.ori(4, 4, 0x5000),
             asm.li(5, 0), asm.mtspr(914, 5),                                     # GQR2: f32
             asm.lis(5, 0x0204), asm.ori(5, 5, 0x0407), asm.mtspr(915, 5),        # GQR3: ld u8 >>2, st s16 <<4
             asm.psq_l(1, 0, 3, 0, 2), asm.psq_l(2, 8, 3, 0, 2),
             asm.ps_madd(3, 1, 2, 2), asm.psq_st(3, 0, 4, 0, 2),                  # (10, -0.25)
             asm.ps_merge10(4, 1, 2), asm.psq_st(4, 8, 4, 0, 2),                  # (-2, 4)
             asm.psq_l(5, 16, 3, 0, 3), asm.psq_st(5, 16, 4, 0, 3),               # s16 (40, 800)
             asm.psq_st(5, 20, 4, 0, 2),                                          # (2.5, 50)
             asm.lfs(6, 0, 3), asm.fadds(7, 6, 6), asm.stfs(7, 28, 4),            # 3.0
             asm.psq_st(7, 32, 4, 0, 2),                                          # single fills both slots
             asm.lwz(7, 24, 3), asm.rlwinm(6, 7, 8, 16, 23), asm.stw(6, 40, 4),  # 0x7800
             asm.li(8, 100), asm.mtctr(8), asm.li(9, 0), asm.li(10, 0)]
    loop = at()
    code += [asm.addi(10, 10, 1), asm.add(9, 9, 10)]
    code += [asm.bdnz(at(), loop), asm.stw(9, 44, 4)]                             # 5050
    code += [asm.li(11, 7)]
    call_at = len(code)
    code += [0, asm.stw(11, 48, 4)]                                               # 42
    code += [asm.lis(12, 0xF000), asm.ori(12, 12, 1), asm.srawi(13, 12, 4), asm.li(14, 0), asm.addze(14, 14),
             asm.stw(13, 52, 4), asm.stw(14, 56, 4),                              # 0xFF000000, 1
             asm.fmadd(8, 1, 2, 6), asm.fctiwz(9, 8), asm.li(15, 60), asm.stfiwx(9, 4, 15),  # 7
             asm.cmpw(6, 12, 7), asm.cmplw(7, 12, 7), asm.crxor(2, 25, 29), asm.mfcr(16),
             asm.stw(16, 64, 4),                                                  # 0x20000084
             # negated multiply-adds whose sum is an exact zero give -0 (XBA-254; AArch64 folding gave +0)
             asm.psq_l(20, 28, 3, 0, 2), asm.psq_l(21, 36, 3, 0, 2),              # (2, 1.5), (3, -3)
             asm.ps_merge10(22, 20, 20), asm.ps_merge10(29, 21, 21),              # (1.5, 2), (-3, 3)
             asm.fnmsub(23, 20, 22, 21), asm.stfs(23, 68, 4),                     # -(2 * 1.5 - 3)
             asm.fnmadd(24, 20, 22, 29), asm.stfs(24, 72, 4),                     # -(2 * 1.5 + -3)
             asm.fnmsubs(25, 20, 22, 21), asm.stfs(25, 76, 4),
             asm.fnmadds(26, 20, 22, 29), asm.stfs(26, 80, 4),
             asm.ps_nmsub(27, 20, 22, 21), asm.psq_st(27, 84, 4, 0, 2),           # (-0, -6)
             asm.ps_nmadd(28, 20, 22, 21), asm.psq_st(28, 92, 4, 0, 2)]           # (-6, -0)
    idle = at()
    code += [asm.b(idle, idle)]
    fn = at()
    code += [asm.mulli(11, 11, 6), asm.blr()]
    code[call_at] = asm.bl(TEXT + 4 * call_at, fn)
    data = f32(1.5, -2.0, 4.0, 0.25) + bytes([10, 200, 0, 0, 0, 0, 0, 0]) + struct.pack(">I", 0x12345678)
    data += f32(2.0, 1.5, 3.0, -3.0)
    return build([(TEXT, struct.pack(">%dI" % len(code), *code))], [(DATA, data)])


EXPECTED = [0x41200000, 0xBE800000, 0xC0000000, 0x40800000, 0x00280320, 0x40200000, 0x42480000, 0x40400000,
            0x40400000, 0x40400000, 0x00007800, 5050, 42, 0xFF000000, 1, 7, 0x20000084,
            0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0xC0C00000, 0xC0C00000, 0x80000000]


def test_dol_round_trip():
    raw = program()
    img = Image(raw)
    assert img.entry == TEXT and len(img.text) == 1
    assert img.u32(TEXT) == asm.lis(3, 0x8000)
    assert img.in_code(TEXT) and not img.in_code(DATA)
    assert img.span_hash(TEXT, TEXT + 8) == img.span_hash(TEXT, TEXT + 8)


def test_bad_dols():
    with pytest.raises(DolError):
        Image(b"\0" * 16)
    with pytest.raises(DolError):
        Image(build([(TEXT, b"\0" * 8)], entry=0x80000000))


def test_gekko_decode():
    d = GekkoDecoder()
    i = d.decode(asm.psq_l(1, -8, 3, 1, 5), TEXT)
    assert i.name == "psq_l" and i.ext.w == 1 and i.ext.i == 5 and i.ext.d == -8
    assert i.text == "psq_l f1, -8(r3), 1, 5"
    assert d.decode(asm.ps_madd(3, 1, 2, 4), 0).text == "ps_madd f3, f1, f2, f4"
    assert d.decode(asm.ps_merge10(4, 1, 2), 0).name == "ps_merge10"
    assert d.decode(asm.ps_sum0(4, 3, 2, 1), 0).name == "ps_sum0"
    psq_lx = (4 << 26) | (1 << 21) | (3 << 16) | (4 << 11) | (1 << 10) | (2 << 7) | (6 << 1)
    i = d.decode(psq_lx, 0)
    assert i.name == "psq_lx" and i.ext.w == 1 and i.ext.i == 2


def test_code_constants_seed_callbacks():
    handler = TEXT + 0x10
    words = [asm.lis(4, 0x8000), asm.addi(4, 4, 0x3110), asm.blr(), 0, asm.blr()]
    img = Image(build([(TEXT, struct.pack(">5I", *words))]))
    assert handler in code_constants(img, GekkoDecoder())


def test_recompile_lifts_the_program(tmp_path):
    s = recompile(Image(program()), str(tmp_path))
    assert s["functions"] == 2 and s["interp_fallback_insns"] == 0
    text = (tmp_path / "gc_rc_000.c").read_text()
    assert "RC_PSQ_L(1, ea_, 0, 2);" in text
    assert "RC_MTSPR(915, R(5)); RC_CHECK(0x80003124u);" in text


def _build_gc_run(tmp_path):
    srcs = [os.path.join(RUNTIME, f) for f in ("gc_core.c", "gc_cpu.c", "gc_bus.c", "gc_recomp.c")]
    exe = str(tmp_path / "gc_run")
    subprocess.run(["cc", "-O1", "-Wall", "-Werror", "-rdynamic", "-I", RUNTIME, "-o", exe,
                    os.path.join(RUNTIME, "tools", "gc_run.c")] + srcs + ["-ldl", "-lm"], check=True)
    return exe


def _peek(out):
    words = []
    for line in out.splitlines():
        if line.startswith("peek ") or (words and line and all(c in "0123456789ABCDEF " for c in line)):
            words += [int(x, 16) for x in line.replace("peek ", "").split()]
    return words


@pytest.mark.skipif(not shutil.which("cc") or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_interpreter_and_recompiled_agree(tmp_path):
    dol = tmp_path / "t.dol"
    dol.write_bytes(program())
    exe = _build_gc_run(tmp_path)
    peek = ["--peek", "%08X:%d" % (OUT, len(EXPECTED))]
    r = subprocess.run([exe, str(dol), "--frames", "1", "--stats"] + peek, capture_output=True, text=True,
                       timeout=60)
    assert r.returncode == 0, r.stderr
    assert _peek(r.stdout) == EXPECTED
    env = dict(os.environ, PYTHONPATH=os.path.dirname(HERE))
    subprocess.run([sys.executable, "-m", "gc_tools.recomp", str(dol), str(tmp_path / "src")], env=env, check=True,
                   capture_output=True)
    pack = str(tmp_path / "libt.so")
    srcs = sorted(str(p) for p in (tmp_path / "src").glob("*.c"))
    subprocess.run(["cc", "-shared", "-fPIC", "-O1", "-Wall", "-Werror", "-Wno-unused-label", "-DGC_RC_COUNT",
                    "-I", RUNTIME, "-o", pack] + srcs + ["-lm"], check=True)
    r = subprocess.run([exe, str(dol), "--frames", "1", "--stats", "--pack", pack] + peek, capture_output=True,
                       text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert _peek(r.stdout) == EXPECTED
    stats = [ln for ln in r.stdout.splitlines() if ln.startswith("stats:")][0].split()
    native, interp = int(stats[2]), int(stats[4])
    assert native > 300 and interp == 0, stats
