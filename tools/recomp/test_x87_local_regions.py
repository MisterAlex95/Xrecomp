"""x87 runs of a basic block on C locals (lifter._x87_local_regions, XBA-238).

What the pass emits: where a region starts and ends, what ends it, which
slots it writes back. Then the differential check: real bytes through
Capstone and lift_basic_block twice, with the regions and without, both
compiled against templates/runtime/recomp_types.h with the translator's
accessors, run from the same random x87 state (every TOP, random slots,
random guest memory) -- the x87 stack, TOP, condition codes, eax and guest
memory must come out bit for bit the same.
"""

import os
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

pytest.importorskip("capstone")

from tools.recomp.c_fixture import command, compiler       # noqa: E402
from tools.recomp.disasm import BasicBlock, Disassembler    # noqa: E402
from tools.recomp.lifter import (Lifter, _x87_stack_effect,  # noqa: E402
                                 lift_basic_block)
from tools.recomp.translator import FP_STACK_LOCALS, FP_STACK_MACROS  # noqa: E402

VA = 0x00011000


def block(hexbytes):
    code = bytes.fromhex(hexbytes.replace(" ", ""))
    insns = list(Disassembler()._cfg_stream(code, 0, len(code), VA))
    assert sum(i.size for i in insns) == len(code), [i.mnemonic for i in insns]
    return BasicBlock(start=VA, instructions=insns)


def lifted(hexbytes, regions=True):
    lifter = Lifter()
    lifter.x87_locals = regions
    stmts, _ = lift_basic_block(lifter, block(hexbytes))
    return stmts


# fld dword [esi]; fmul dword [ebx+0x10]; fld dword [esi+4];
# fmul dword [ebx+0x14]; faddp st(1); fstp dword [edi]
MATRIX_ROW = "d906 d84b10 d94604 d84b14 dec1 d91f"


def test_a_run_becomes_one_region():
    out = lifted(MATRIX_ROW)
    assert out[0].startswith("fpl_load();"), out
    assert "fp_" not in " ".join(s.replace("fpl_", "") for s in out[1:-1]), out
    # Two pushes, one faddp, one fstp: TOP is back where it was, and the
    # slots written are st(-1) and st(-2) of the load, _fs[7] and _fs[6].
    assert out[-1] == "fpl_store(6); fpl_store(7); /* x87 locals back */", out


def test_without_the_switch_the_accessors_are_g_cpu():
    out = lifted(MATRIX_ROW, regions=False)
    assert not any("fpl_" in s for s in out), out


def test_integer_instructions_stay_inside_a_call_ends_it():
    # fld1; add esi, 4; fld1; faddp; call 0x1234; fld1; fstp st(1)
    out = lifted("d9e8 83c604 d9e8 dec1 e8 2f020000 d9e8 ddd9")
    text = "\n".join(out)
    first = text.index("fpl_load")
    call = text.index("sub_")
    assert first < text.index("esi = esi + 4") < text.index("x87 locals back") < call, text
    assert text.count("fpl_load") == 2, text       # one region before the call, one after
    assert "fpl_settop();" in text, text            # fld1 x2 + faddp leaves one more slot


def test_a_lone_x87_instruction_keeps_the_g_cpu_accessors():
    out = lifted("d9e8 83c604")                     # fld1; add esi, 4
    assert not any("fpl_" in s for s in out), out


@pytest.mark.parametrize("barrier", ["d9f6",       # fdecstp: writes g_fp_top
                                     "d9fb",       # fsincos: conditional push
                                     "d9f2",       # fptan: conditional push
                                     "dbe3"])      # fninit: runtime helper
def test_barriers_split_the_run(barrier):
    out = lifted("d9e8 d9e8 dec1 %s d9e8 dec1 d9e1" % barrier)
    text = "\n".join(out)
    assert text.count("fpl_load") == 2, text
    k = next(i for i, s in enumerate(out) if "x87 locals back" in s)
    assert not any("fpl_" in s for s in out[k + 1:k + 2]), text   # the barrier itself


@pytest.mark.parametrize("text,expect", [
    ("fp_push(MEMF(esi)); fp_top() = RECOMP_FP_PC(fp_top() * MEMF(ebx));", ({7}, 7)),
    ("fp_st1() = RECOMP_FP_PC(fp_st1() + fp_top()); fp_pop();", ({1}, 1)),
    ("{ double _t = fp_st(3); fp_push(_t); } /* fld st(3) */", ({7}, 7)),
    ("{ double _t = fp_top(); fp_top() = fp_st(2); fp_st(2) = _t; }", ({0, 2}, 0)),
    ("g_fp_cmp = RECOMP_FCMP(fp_top(), fp_st1()); fp_pop(); fp_pop();", (set(), 2)),
    ("if (x == 1) fp_top() = fp_st(4); /* fcmove */", ({0}, 0)),
    ("eax = (uint32_t)((fp_abstop() << 11) | g_fp_cc);", (set(), 0)),
    ("fp_push(1.0); fp_push(fp_top()); fp_st(2) = fp_top();", ({6, 7, 0}, 6)),
])
def test_stack_effect(text, expect):
    assert _x87_stack_effect(text) == expect


def test_stack_effect_refuses_what_it_cannot_follow():
    assert _x87_stack_effect("fp_push(MEMF(esi);") is None
    assert _x87_stack_effect("fp_top(1) = 0.0;") is None


# ── differential: regions vs g_cpu accessors, compiled and run ──

BLOCKS = {
    "matrix": MATRIX_ROW + " d94608 d84b18 dec1",
    # fld st(2); fxch st(1); fsubrp st(1); fst qword [edi+8]; add esi, 4;
    # fld1; fdiv st(0), st(3); fcom st(1); fnstsw ax; fstp st(0);
    # fistp dword [edi+16]; fabs; fchs; fsqrt; fcomp qword [edi+8];
    # fldz; fstp st(3); fxam; fnstsw ax
    "mixed": "d9c2 d9c9 dee9 dd5708 83c604 d9e8 d8f3 d8d1 dfe0 ddd8 db5f10"
             " d9e1 d9e0 d9fa dc5f08 d9ee dddb d9e5 dfe0",
    # Barriers in the middle: fdecstp, fincstp, fsincos, fptan.
    "barriers": "d9e8 d906 dec9 d9f6 d9e8 dec1 d9f7 d906 d9fb dec1 d9e8 d9f2"
                " dec1 dec1 d95f04",
    # Loads and stores of every width; fild/fist; reversed ops.
    "widths": "dd06 dc4b08 da4604 de7e10 df4610 dd5f18 db5f20 d9ee dec1"
              " d9c0 dee1 dd5728",
}


def _harness(blocks):
    fns = []
    for name, (plain, local) in blocks.items():
        for suffix, stmts in (("plain", plain), ("local", local)):
            fns.append("static void %s_%s(void) {\n    uint32_t _fa = 0, _fb = 0;"
                       " int32_t _fas = 0, _fbs = 0; int _flags = 0, _cf = 0;\n"
                       "    (void)_fa; (void)_fb; (void)_fas; (void)_fbs; (void)_flags;"
                       " (void)_cf;\n%s\n%s\n}\n"
                       % (name, suffix, "\n".join(FP_STACK_LOCALS),
                          "\n".join("    " + s for s in stmts)))
    table = ",\n".join("    { \"%s\", %s_plain, %s_local }" % (n, n, n) for n in blocks)
    return (HEAD + "\n".join(FP_STACK_MACROS) + "\n" + "\n".join(fns)
            + "static const struct { const char *name; void (*plain)(void);"
              " void (*local)(void); } blocks[] = {\n" + table + "\n};\n" + MAIN)


HEAD = r"""
#define RECOMP_GENERATED_CODE 1
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "recomp_types.h"

RECOMP_TLS RecompCpu g_cpu;
static uint8_t g_mem[0x10000];
ptrdiff_t g_xbox_mem_offset;
void recomp_divide_error(uint32_t va, uint32_t divisor) { (void)va; (void)divisor; }
"""

MAIN = r"""
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t nxt(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double rnd_double(void) {
    /* Mostly moderate values, some zeros, negatives, large and tiny ones. */
    uint64_t r = nxt();
    double v = (double)(int64_t)(r >> 11) / (double)(1ull << 40);
    switch (r & 15) {
    case 0: return 0.0;
    case 1: return -v * 1e30;
    case 2: return v * 1e-30;
    case 3: return -0.0;
    default: return v;
    }
}

static void seed(unsigned top, uint64_t s) {
    unsigned i;
    rng = s * 0x2545F4914F6CDD1Dull + 1;
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.r_fp_control_word = (s & 1) ? 0x027Fu : 0x007Fu;   /* 53-bit or 24-bit */
    g_cpu.r_fp_cc = 0x4000u;
    g_fp_top = (int)top;
    for (i = 0; i < 8; i++) g_fp_stack[i] = rnd_double();
    for (i = 0; i < 0x200; i += 4) {
        float f = (float)rnd_double();
        double d = rnd_double();
        if ((i & 7) == 0 && i + 8 <= 0x200) { memcpy(&g_mem[0x1000 + i], &d, 8); i += 4; }
        else memcpy(&g_mem[0x1000 + i], &f, 4);
    }
    memcpy(&g_mem[0x1104], &(int16_t){ -1234 }, 2);
    memcpy(&g_mem[0x1110], &(int32_t){ 77777 }, 4);
    eax = 0xA5A5A5A5u; ebx = 0x1080; esi = 0x1000; edi = 0x1100; ecx = 3; edx = 9;
    esp = 0x8000;
}

int main(void) {
    static uint8_t mem_a[sizeof g_mem];
    RecompCpu cpu_a;
    unsigned b, top, s;
    long runs = 0;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_mem;
    for (b = 0; b < sizeof blocks / sizeof blocks[0]; b++) {
        for (top = 0; top < 8; top++) {
            for (s = 0; s < 64; s++) {
                seed(top, s * 8 + top + 1000u * b);
                blocks[b].plain();
                cpu_a = g_cpu; memcpy(mem_a, g_mem, sizeof g_mem);
                seed(top, s * 8 + top + 1000u * b);
                blocks[b].local();
                runs++;
                if (memcmp(&cpu_a, &g_cpu, sizeof g_cpu) || memcmp(mem_a, g_mem, sizeof g_mem)) {
                    unsigned i;
                    printf("FAIL %s top %u seed %u: g_fp_top %d/%d cc %04x/%04x eax %08x/%08x\n",
                           blocks[b].name, top, s, cpu_a.r_fp_top, g_cpu.r_fp_top,
                           cpu_a.r_fp_cc, g_cpu.r_fp_cc, cpu_a.r_eax, g_cpu.r_eax);
                    for (i = 0; i < 8; i++)
                        printf("  st[%u] %a / %a\n", i, cpu_a.r_fp_stack[i], g_cpu.r_fp_stack[i]);
                    return 1;
                }
            }
        }
    }
    printf("OK %ld\n", runs);
    return 0;
}
"""


def test_regions_compute_what_the_g_cpu_accessors_compute():
    cc = compiler()
    if not cc:
        pytest.skip("no C compiler")
    blocks = {}
    for name, hexbytes in BLOCKS.items():
        local = lifted(hexbytes)
        assert any("fpl_load" in s for s in local), (name, local)
        blocks[name] = (lifted(hexbytes, regions=False), local)
    src = _harness(blocks)
    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, "t.c")
        exe = os.path.join(tmp, "t.exe")
        Path(c).write_text(src)
        for opt in ("-O0", "-O2"):
            r = subprocess.run(command(cc, [c], exe,
                                       include_dirs=[ROOT / "templates" / "runtime"],
                                       gnu_flags=["-std=gnu11", opt, "-w"]
                                       + ([] if os.name == "nt" else ["-lm"])),
                               capture_output=True, text=True)
            assert r.returncode == 0, r.stderr[-3000:] + "\n" + src
            r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
            assert r.returncode == 0 and "OK" in r.stdout, r.stdout + r.stderr
