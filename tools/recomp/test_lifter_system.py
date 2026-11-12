"""The instructions that were still lifted as RECOMP_UNIMPL, or as a comment
that compiles to nothing (XBA-111).

cli/sti, hlt, enter with a nesting level, the x87 housekeeping (fninit,
fnclex, ffree/ffreep, fincstp/fdecstp, fxtract, fbld/fbstp, fldenv/fnstenv,
fxsave/fxrstor), maskmovq, sgdt/sidt/lgdt/lidt, invlpg, pause, far jumps,
far calls and the far return.

Real bytes go through Capstone and the lifter; the C is compiled against the
real templates/runtime/recomp_types.h with the translator's own x87 stack
macros, and run. The runtime entry points the lifted code calls
(xbox_GuestInterruptFlag, xbox_Halt, xbox_ReadDtr/WriteDtr,
recomp_divide_error) are stubs here that record their calls; their own
behaviour is tested in tests/guest_cpu_ops. Expected values come from the
SDM, written out below.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

pytest.importorskip("capstone")

from tools.recomp.c_fixture import command, compiler      # noqa: E402
from tools.recomp.disasm import Disassembler               # noqa: E402
from tools.recomp.lifter import Lifter                     # noqa: E402
from tools.recomp.translator import FP_STACK_MACROS        # noqa: E402

VA = 0x00011000


def lift(hexbytes, va=VA):
    code = bytes.fromhex(hexbytes)
    insn = next(Disassembler()._cfg_stream(code, 0, len(code), va))
    lifter = Lifter()
    out = "\n".join(lifter.lift_instruction(insn))
    return out, lifter, insn


# Every instruction of the ticket, by its bytes: none may reach the
# unimplemented path or lift to a bare comment.
COVERED = {
    "cli": "fa", "sti": "fb", "hlt": "f4",
    "enter 0x10, 1": "c8100001", "enter 0x10, 3": "c8100003",
    "fninit": "dbe3", "fnclex": "dbe2", "ffree": "ddc1", "ffreep": "dfc1",
    "fincstp": "d9f7", "fdecstp": "d9f6", "fxtract": "d9f4",
    "fbld": "df20", "fbstp": "df30", "fldenv": "d920", "fnstenv": "d930",
    "fxsave": "0fae00", "fxrstor": "0fae08", "maskmovq": "0ff7c1",
    "sgdt": "0f0100", "sidt": "0f0108", "lgdt": "0f0110", "lidt": "0f0118",
    "invlpg": "0f0138", "pause": "f390", "fnop": "d9d0",
    "ljmp": "ea001000000800", "lcall": "9a001000000800",
}


@pytest.mark.parametrize("name", sorted(COVERED))
def test_lifts_without_marker(name):
    out, lifter, _ = lift(COVERED[name])
    assert "RECOMP_UNIMPL" not in out and lifter.unimplemented == {}, out
    assert "FPU:" not in out, out
    # A statement, not only a comment.
    code = out
    while "/*" in code:
        a = code.index("/*")
        code = code[:a] + code[code.index("*/", a) + 2:]
    assert code.strip(), out


def test_an_unknown_x87_mnemonic_is_marked_not_dropped():
    # The x87 fallback was a comment that compiled to nothing; whatever the
    # lifter does not know now reaches the unimplemented path.
    from tools.recomp.disasm import Instruction
    lifter = Lifter()
    out = "\n".join(lifter.lift_instruction(Instruction(VA, 2, "fnotreal", "", "")))
    assert 'RECOMP_UNIMPL("fnotreal"' in out and "fnotreal" in lifter.unimplemented


def test_far_call_and_return_frames():
    out, _, _ = lift("9a001000000800")
    assert "PUSH32(esp, 0x8u); PUSH32(esp, 0x00011007u);" in out
    assert "RECOMP_ICALL_SAFE_AT(_icall_target" in out
    out, _, _ = lift("ff18")                      # call far [eax]
    assert "PUSH32(esp, (uint32_t)MEM16((eax) + 4)); PUSH32(esp, 0x00011002u);" in out
    out, _, _ = lift("ff10")                      # call near [eax]: no selector
    assert "MEM16" not in out
    assert "esp += 8; return;" in lift("cb")[0]
    assert "esp += 16; return;" in lift("ca0800")[0]
    assert "esp += 4; return;" in lift("c3")[0]
    out, _, _ = lift("ea001000000800")
    assert "RECOMP_ITAIL(0x00001000u); return;" in out


HARNESS_HEAD = r"""
#define RECOMP_GENERATED_CODE 1
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "recomp_types.h"

RECOMP_TLS RecompCpu g_cpu = { .r_eflags = 0x202u, .r_mxcsr = 0x1F80u,
                               .r_fp_control_word = 0x037Fu };
static uint8_t g_mem[0x10000];
ptrdiff_t g_xbox_mem_offset;

static int if_calls[8], n_if_calls, halts, de_calls;
static uint32_t halt_va, de_va;
static uint64_t dtr[2] = { 0x1234567800FFull, 0x9ABCDEF007FFull };
void xbox_GuestInterruptFlag(int enabled) { if (n_if_calls < 8) if_calls[n_if_calls] = enabled; n_if_calls++; }
void xbox_Halt(uint32_t va) { halts++; halt_va = va; }
uint64_t xbox_ReadDtr(int which) { return dtr[which & 1]; }
void xbox_WriteDtr(int which, uint64_t v) { dtr[which & 1] = v; }
void recomp_divide_error(uint32_t va, uint32_t divisor) { de_calls++; de_va = va; (void)divisor; }

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
"""


def _harness(body):
    macros = "\n".join(line.strip() for line in FP_STACK_MACROS)
    return (HARNESS_HEAD + macros + "\nint main(void)\n{\n    uint32_t ebp = 0;\n"
            "    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_mem;\n"
            + body + "\n    (void)ebp;\n    printf(\"OK\\n\");\n    return 0;\n}\n")


def _run(body):
    cc = compiler()
    if not cc:
        pytest.skip("no C compiler")
    src = _harness(body)
    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, "t.c")
        exe = os.path.join(tmp, "t.exe")
        Path(c).write_text(src)
        r = subprocess.run(command(cc, [c], exe,
                                   include_dirs=[ROOT / "templates" / "runtime"],
                                   gnu_flags=["-std=c11"] + ([] if os.name == "nt" else ["-lm"])),
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr[-3000:] + "\n" + src
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
        assert r.returncode == 0 and "OK" in r.stdout, r.stdout + r.stderr + "\n" + body


def L(hexbytes, va=VA):
    return lift(hexbytes, va)[0]


def test_cli_sti_popfd_keep_if_and_tell_the_runtime():
    cli, sti = L("fa"), L("fb")
    pushfd, popfd = L("9c"), L("9d")
    _run(f"""
    esp = 0x8000;
    {cli}
    CHECK(!(g_eflags & 0x200u)); CHECK(n_if_calls == 1 && if_calls[0] == 0);
    {cli}
    CHECK(n_if_calls == 1);                       /* no change, no call */
    {pushfd}
    CHECK(!(MEM32(esp) & 0x200u));                /* pushfd sees IF clear */
    {sti}
    CHECK(g_eflags & 0x200u); CHECK(n_if_calls == 2 && if_calls[1] == 1);
    {sti}
    CHECK(n_if_calls == 2);
    {popfd}                                        /* pops the IF-clear image */
    CHECK(!(g_eflags & 0x200u)); CHECK(n_if_calls == 3 && if_calls[2] == 0);
    PUSH32(esp, 0x00000202u);
    {popfd}
    CHECK(g_eflags & 0x200u); CHECK(n_if_calls == 4 && if_calls[3] == 1);
    PUSH32(esp, 0x00000203u);
    {popfd}
    CHECK(n_if_calls == 4);                       /* IF unchanged */
    CHECK(esp == 0x8000);
    """)


def _enter_expected(n, level, esp0, ebp0, mem):
    """SDM ENTER, 32-bit: the stack after `enter n, level`."""
    mem = dict(mem)
    esp = esp0 - 4
    mem[esp] = ebp0
    frame = esp
    ebp = ebp0
    if level > 0:
        for _ in range(1, level):
            ebp -= 4
            esp -= 4
            mem[esp] = mem.get(ebp, 0)
        esp -= 4
        mem[esp] = frame
    return esp - n, frame, mem


@pytest.mark.parametrize("level", [0, 1, 2, 3, 5])
def test_enter_with_nesting_levels(level):
    code = "c81000%02x" % level
    ebp0, esp0 = 0x7000, 0x6000
    old = {ebp0 - 4 * i: 0x1110 * i for i in range(1, 8)}
    esp_w, ebp_w, mem_w = _enter_expected(0x10, level, esp0, ebp0, old)
    seed = "\n".join(f"    MEM32(0x{a:X}u) = 0x{v:X}u;" for a, v in old.items())
    checks = "\n".join(f"    CHECK(MEM32(0x{a:X}u) == 0x{v:X}u);"
                       for a, v in mem_w.items())
    _run(f"""
{seed}
    ebp = 0x{ebp0:X}u; esp = 0x{esp0:X}u;
    {L(code)}
    CHECK(esp == 0x{esp_w:X}u); CHECK(ebp == 0x{ebp_w:X}u);
{checks}
    """)


def test_x87_housekeeping():
    _run(f"""
    fp_push(1.0); fp_push(2.0); fp_push(3.0);
    g_fp_control_word = 0x0C7Fu; g_fp_cc = 0x4500u;
    {L("dbe3")}                                    /* fninit */
    CHECK(g_fp_control_word == 0x037Fu && g_fp_top == 0 && g_fp_cc == 0);
    fp_push(1.0); fp_push(2.0);                    /* top 6: st0 2, st1 1 */
    {L("dfc1")}                                    /* ffreep st(1): pops */
    CHECK(g_fp_top == 7 && fp_top() == 1.0);
    {L("ddc1")}                                    /* ffree: top unchanged */
    CHECK(g_fp_top == 7);
    {L("d9f6")}                                    /* fdecstp */
    CHECK(g_fp_top == 6 && fp_top() == 2.0);       /* the value is still there */
    {L("d9f7")}                                    /* fincstp */
    CHECK(g_fp_top == 7);
    {L("dbe2")} {L("d9d0")}                        /* fnclex, fnop */
    CHECK(g_fp_top == 7);
    """)


def test_fxtract():
    fx = L("d9f4")
    _run(f"""
    fp_push(10.0); {fx}
    CHECK(fp_top() == 1.25 && fp_st1() == 3.0);
    fp_push(-0.75); {fx}
    CHECK(fp_top() == -1.5 && fp_st1() == -1.0);
    fp_push(-0.0); {fx}
    CHECK(fp_top() == 0.0 && signbit(fp_top()) && isinf(fp_st1()) && fp_st1() < 0);
    fp_push(-INFINITY); {fx}
    CHECK(isinf(fp_top()) && fp_top() < 0 && fp_st1() == INFINITY);
    fp_push(1e-310); {fx}                          /* a double subnormal */
    CHECK(fp_top() >= 1.0 && fp_top() < 2.0 && fp_st1() < -1028.0);
    """)


def _bcd(value):
    digits = "%018d" % abs(value)
    b = bytearray(10)
    for i in range(9):
        lo = int(digits[17 - 2 * i])
        hi = int(digits[16 - 2 * i])
        b[i] = (hi << 4) | lo
    b[9] = 0x80 if value < 0 else 0
    return bytes(b)


def test_fbld_and_fbstp():
    fbld, fbstp = L("df20"), L("df30")      # [eax]
    cases = [123456789012345, -42, 0, 999999999999999]
    body = ["    eax = 0x100;"]
    for v in cases:
        bcd = _bcd(v)
        body.append("    { static const uint8_t b[10] = {%s};" % ",".join(str(x) for x in bcd))
        body.append(f"      memcpy(g_mem + 0x100, b, 10); {fbld}")
        body.append(f"      CHECK(fp_top() == {v}.0); memset(g_mem + 0x100, 0xEE, 10);")
        body.append(f"      {fbstp}")
        body.append("      CHECK(memcmp(g_mem + 0x100, b, 10) == 0); }")
    # Rounding under RC, and the indefinite.
    body.append(f"    fp_push(2.5); {fbstp} CHECK(g_mem[0x100] == 0x02);")
    body.append(f"    g_fp_control_word = 0x0B7Fu; fp_push(2.1); {fbstp} CHECK(g_mem[0x100] == 0x03);")
    body.append("    g_fp_control_word = 0x037Fu;")
    body.append(f"    fp_push(-1e18); {fbstp}")
    body.append("    { static const uint8_t ind[10] = {0,0,0,0,0,0,0,0xC0,0xFF,0xFF};"
                " CHECK(memcmp(g_mem + 0x100, ind, 10) == 0); }")
    body.append(f"    fp_push(NAN); {fbstp} CHECK(g_mem[0x108] == 0xFF && g_mem[0x107] == 0xC0);")
    body.append("    CHECK(g_fp_top == 0);")
    _run("\n".join(body))


def test_fnstenv_and_fldenv():
    _run(f"""
    eax = 0x200;
    fp_push(1.0); fp_push(2.0);                    /* top 6 */
    g_fp_cc = 0x4100u; g_fp_control_word = 0x0272u;
    memset(g_mem + 0x200, 0xEE, 40);
    {L("d930")}                                    /* fnstenv [eax], 28 bytes */
    CHECK(MEM16(0x200) == 0x0272u);
    CHECK(MEM16(0x204) == (uint16_t)((6u << 11) | 0x4100u));
    CHECK(MEM32(0x208) == 0 && MEM32(0x218) == 0);
    CHECK(g_mem[0x200 + 28] == 0xEE);              /* exactly 28 bytes */
    CHECK(g_fp_control_word == 0x027Fu);           /* exceptions masked */
    g_fp_control_word = 0x037Fu; g_fp_top = 0; g_fp_cc = 0;
    {L("d920")}                                    /* fldenv [eax] */
    CHECK(g_fp_control_word == 0x0272u && g_fp_top == 6 && g_fp_cc == 0x4100u);
    CHECK(g_fp_cmp == 0);                           /* C3 set: equal */
    memset(g_mem + 0x200, 0xEE, 40);
    {L("66d930")}                                  /* the 14-byte image */
    CHECK(MEM16(0x202) == (uint16_t)((6u << 11) | 0x4100u));
    CHECK(g_mem[0x200 + 13] == 0 && g_mem[0x200 + 14] == 0xEE);
    """)


def test_fxsave_and_fxrstor():
    _run(f"""
    eax = 0x400;
    memset(g_mem + 0x400, 0xEE, 512);
    fp_push(1.5); fp_push(-2.25);                  /* top 6 */
    g_fp_control_word = 0x027Fu; g_fp_cc = 0x0100u; g_mxcsr = 0x9FC0u;
    g_xmm1.u[0] = 0x11111111u; g_xmm1.u[3] = 0x44444444u;
    g_xmm7.f[2] = 3.5f;
    {L("0fae00")}                                  /* fxsave [eax] */
    CHECK(MEM16(0x400) == 0x027Fu);
    CHECK(MEM16(0x402) == (uint16_t)((6u << 11) | 0x0100u));
    CHECK(g_mem[0x404] == 0xFFu);
    CHECK(MEM32(0x418) == 0x9FC0u);
    CHECK(MEM32(0x41C) == 0);                      /* MXCSR_MASK: 0 on a P3 */
    CHECK(MEM16(0x420 + 8) == 0xC000u);            /* ST0 = -2.25: sign, exp 16384 */
    CHECK(MEM32(0x420 + 4) == 0x90000000u && MEM32(0x420) == 0);
    CHECK(MEM16(0x430 + 8) == 0x3FFFu && MEM32(0x430 + 4) == 0xC0000000u); /* ST1 = 1.5 */
    CHECK(MEM32(0x4B0) == 0x11111111u && MEM32(0x4BC) == 0x44444444u); /* xmm1 at 176 */
    CHECK(MEMF(0x400 + 160 + 7 * 16 + 8) == 3.5f);
    CHECK(g_mem[0x400 + 288] == 0xEE);            /* reserved tail untouched */
    g_fp_control_word = 0x037Fu; g_fp_top = 0; g_fp_cc = 0; g_mxcsr = 0x1F80u;
    fp_st(0) = 0; fp_st(1) = 0; memset(&g_xmm1, 0, 16); memset(&g_xmm7, 0, 16);
    {L("0fae08")}                                  /* fxrstor [eax] */
    CHECK(g_fp_control_word == 0x027Fu && g_fp_top == 6 && g_fp_cc == 0x0100u);
    CHECK(g_mxcsr == 0x9FC0u && fp_top() == -2.25 && fp_st1() == 1.5);
    CHECK(g_xmm1.u[0] == 0x11111111u && g_xmm1.u[3] == 0x44444444u && g_xmm7.f[2] == 3.5f);
    """)


def test_maskmovq_writes_only_the_selected_bytes():
    _run(f"""
    edi = 0x300;
    memset(g_mem + 0x300, 0xEE, 16);
    mm0.q = 0x8877665544332211ull;
    mm1.q = 0x80007F00FF000180ull;                 /* bytes 0, 3, 7 have bit 7 */
    {L("0ff7c1")}                                  /* maskmovq mm0, mm1 */
    CHECK(g_mem[0x300] == 0x11 && g_mem[0x301] == 0xEE && g_mem[0x302] == 0xEE);
    CHECK(g_mem[0x303] == 0x44 && g_mem[0x304] == 0xEE && g_mem[0x305] == 0xEE);
    CHECK(g_mem[0x306] == 0xEE && g_mem[0x307] == 0x88 && g_mem[0x308] == 0xEE);
    """)


def test_descriptor_tables_halt_and_aam0():
    _run(f"""
    eax = 0x500;
    memset(g_mem + 0x500, 0xEE, 8);
    {L("0f0100")}                                  /* sgdt [eax] */
    CHECK(MEM16(0x500) == 0x00FFu && MEM32(0x502) == 0x12345678u && g_mem[0x506] == 0xEE);
    {L("0f0108")}                                  /* sidt [eax] */
    CHECK(MEM16(0x500) == 0x07FFu && MEM32(0x502) == 0x9ABCDEF0u);
    MEM16(0x500) = 0x0123u; MEM32(0x502) = 0x80001000u;
    {L("0f0118")}                                  /* lidt [eax] */
    CHECK(dtr[1] == 0x800010000123ull);
    {L("0f0110")}                                  /* lgdt [eax] */
    CHECK(dtr[0] == 0x800010000123ull);
    {L("f4", 0x00012345)}
    CHECK(halts == 1 && halt_va == 0x00012345u);
    eax = 0x1234;
    {L("d400", 0x00023456)}                        /* aam 0 */
    CHECK(de_calls == 1 && de_va == 0x00023456u && eax == 0x1234);
    {L("0f0138")} {L("f390")}                      /* invlpg, pause */
    """)
