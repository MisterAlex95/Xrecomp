"""Instructions the Pentium III has and the lifter lacked (XBA-71): cwd,
aam/aad, enter N,0, and SSE1's MMX extensions pminub/pmaxub/pmulhuw.
Real bytes through Capstone and the lifter; the C runs against minimal
stand-ins for the runtime's register and MMX helpers (same definitions as
recomp_types.h's portable path)."""

import os
import re
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

pytest.importorskip("capstone")

from tools.recomp.disasm import Disassembler      # noqa: E402
from tools.recomp.lifter import Lifter            # noqa: E402


def lift(code):
    insn = next(Disassembler()._cfg_stream(code, 0, len(code), 0x1000))
    return "\n".join(Lifter().lift_instruction(insn)), insn.mnemonic


@pytest.mark.parametrize("code,mnemonic", [
    (b"\x66\x99", "cwd"),
    (b"\xd4\x0a", "aam"),
    (b"\xd5\x0a", "aad"),
    (b"\xc8\x10\x00\x00", "enter"),
    (b"\x0f\xda\xc1", "pminub"),
    (b"\x0f\xde\xc1", "pmaxub"),
    (b"\x0f\xe4\xc1", "pmulhuw"),
])
def test_lifts(code, mnemonic):
    out, m = lift(code)
    assert m == mnemonic and "RECOMP_UNIMPL" not in out, out


def test_enter_with_a_nesting_level_lifts():
    # XBA-111: the nesting forms lift too (semantics in test_lifter_system.py).
    out, _ = lift(b"\xc8\x10\x00\x01")
    assert "RECOMP_UNIMPL" not in out and "_enter_frame" in out, out


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint8_t g_mem[4096];
#define MEM32(a) (*(uint32_t *)(g_mem + (uint32_t)(a)))
#define PUSH32(sp, v) do { (sp) -= 4; MEM32(sp) = (uint32_t)(v); } while (0)
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(v) & 0xFFFFu))
typedef union { uint64_t q; uint8_t ub[8]; uint16_t uw[4]; } mmx_t;
static mmx_t mm(uint64_t q) { mmx_t m; m.q = q; return m; }
static mmx_t MMX_PMINUB(mmx_t a, mmx_t b) { for (int i = 0; i < 8; i++) a.ub[i] = a.ub[i] < b.ub[i] ? a.ub[i] : b.ub[i]; return a; }
static mmx_t MMX_PMAXUB(mmx_t a, mmx_t b) { for (int i = 0; i < 8; i++) a.ub[i] = a.ub[i] > b.ub[i] ? a.ub[i] : b.ub[i]; return a; }
static mmx_t MMX_PMULHUW(mmx_t a, mmx_t b) { for (int i = 0; i < 4; i++) a.uw[i] = (uint16_t)(((uint32_t)a.uw[i] * b.uw[i]) >> 16); return a; }
int main(void)
{
    uint32_t eax, edx, esp = 0x800, ebp = 0x12345678;
    mmx_t mm0, mm1;
    eax = 0x00008001; edx = 0xAAAA5555; %(cwd)s printf("%%08X ", edx);
    eax = 0x1234003F; %(aam)s printf("%%08X ", eax);
    eax = 0x12340603; %(aad)s printf("%%08X ", eax);
    %(enter)s printf("%%X %%X %%08X ", esp, ebp, MEM32(0x7FC));
    mm0 = mm(0x0102F0FF10200304ull); mm1 = mm(0x02018080FF100403ull);
    { mmx_t s = mm0; %(pminub)s printf("%%016llX ", (unsigned long long)mm0.q); mm0 = s; }
    { mmx_t s = mm0; %(pmaxub)s printf("%%016llX ", (unsigned long long)mm0.q); mm0 = s; }
    mm0 = mm(0xFFFF800000020001ull); mm1 = mm(0xFFFF000200030004ull); %(pmulhuw)s
    printf("%%016llX\n", (unsigned long long)mm0.q);
    return 0;
}
"""


def test_semantics(tmp_path):
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("no cc")
    parts = {k: lift(v)[0] for k, v in (("cwd", b"\x66\x99"), ("aam", b"\xd4\x0a"), ("aad", b"\xd5\x0a"),
                                         ("enter", b"\xc8\x10\x00\x00"), ("pminub", b"\x0f\xda\xc1"),
                                         ("pmaxub", b"\x0f\xde\xc1"), ("pmulhuw", b"\x0f\xe4\xc1"))}
    parts["enter"] = parts["enter"].replace("g_ebp = ebp;", "")
    (tmp_path / "t.c").write_text(HARNESS % parts)
    subprocess.check_call([cc, "-std=gnu11", "-w", "-o", str(tmp_path / "t"), str(tmp_path / "t.c")])
    out = subprocess.check_output([str(tmp_path / "t")], text=True).split()
    assert out == ["AAAAFFFF",            # cwd: ax negative -> dx = FFFF
                   "12340603",            # aam: 63 -> ah 6, al 3
                   "1234003F",            # aad: 6*10 + 3 = 63, ah 0
                   "7EC", "7FC", "12345678",   # enter 0x10,0: push ebp, ebp = esp, esp -= 16
                   "0101808010100303",    # pminub
                   "0202F0FFFF200404",    # pmaxub
                   "FFFE000100000000"]    # pmulhuw: high words (0xFFFF*0xFFFF, 0x8000*2, 2*3, 1*4)


def test_a_function_whose_only_ebp_is_enter_declares_it():
    # Fable 0x002EE2CE: mov edi, edi; enter 0x2EE2, 0; ret. enter names ebp
    # through no operand, so the translator's register scan missed it and the
    # C used an undeclared ebp.
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator
    base = 0x10000
    image = bytes.fromhex("8bffc8e22e00c3")
    config._install([config.Section(".text", base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin="enter-test")
    db = {base: {"start": hex(base), "end": base + len(image),
                 "_addr": base, "size": len(image)}}
    code = FunctionTranslator(image, db).translate_function(base, db[base])
    assert "/* enter */" in code, code
    assert "uint32_t ebp" in code, code
