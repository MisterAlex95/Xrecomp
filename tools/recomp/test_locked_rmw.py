"""Locked read-modify-writes and xchg with memory are atomic (XBA-78).

`lock inc/dec/add/... [mem]` used to lift to RECOMP_UNIMPL (a no-op) and
`xchg [mem], reg` to a plain load and store. Here real instruction bytes go
through Capstone and the lifter, and the C they lift to runs on four host
threads against the runtime's own RECOMP_ATOMIC_* macros: a lost update shows
as a wrong total.
"""

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


def lift(code, needs_cf=True):
    insn = next(Disassembler()._cfg_stream(code, 0, len(code), 0x1000))
    lifter = Lifter()
    lifter.needs_cf = needs_cf
    return "\n".join(lifter.lift_instruction(insn)), insn.mnemonic


@pytest.mark.parametrize("code,mnemonic", [
    (b"\xf0\xff\x00", "lock inc"),            # lock inc dword ptr [eax]
    (b"\xf0\xff\x08", "lock dec"),            # lock dec dword ptr [eax]
    (b"\xf0\x01\x08", "lock add"),            # lock add [eax], ecx
    (b"\xf0\x29\x08", "lock sub"),            # lock sub [eax], ecx
    (b"\xf0\x09\x08", "lock or"),             # lock or [eax], ecx
    (b"\xf0\x21\x08", "lock and"),            # lock and [eax], ecx
    (b"\xf0\x31\x08", "lock xor"),            # lock xor [eax], ecx
    (b"\xf0\x80\x00\x01", "lock add"),        # lock add byte ptr [eax], 1
    (b"\xf0\x66\xff\x00", "lock inc"),        # lock inc word ptr [eax]
    (b"\xf0\xf7\x18", "lock neg"),            # lock neg dword ptr [eax]
    (b"\xf0\x0f\xba\x28\x03", "lock bts"),    # lock bts dword ptr [eax], 3
])
def test_locked_forms_are_compare_and_swap_loops(code, mnemonic):
    out, m = lift(code)
    assert m == mnemonic
    assert "RECOMP_UNIMPL" not in out, out
    assert re.search(r"RECOMP_ATOMIC_CAS(8|16|32)\(XBOX_PTR\(eax\), _lko, _lk\) != _lko", out), out


def test_bit_string_is_a_fetch_or():
    out, m = lift(b"\xf0\x0f\xab\x08")        # lock bts [eax], ecx: ecx runs over a bit string
    assert m == "lock bts"
    assert "RECOMP_ATOMIC_OR32(XBOX_PTR(_lka), _lkm)" in out and "_cf = " in out, out
    out, _ = lift(b"\xf0\x0f\xb3\x08")        # lock btr
    assert "RECOMP_ATOMIC_AND32(XBOX_PTR(_lka), ~_lkm)" in out, out


def test_xchg_with_memory_is_an_exchange_with_or_without_the_prefix():
    for code in (b"\x87\x08", b"\xf0\x87\x08"):  # xchg [eax], ecx
        out, _ = lift(code)
        assert "RECOMP_ATOMIC_XCHG32(XBOX_PTR(eax), (uint32_t)(ecx))" in out, out
    out, _ = lift(b"\x86\x08")                # xchg byte ptr [eax], cl
    assert "RECOMP_ATOMIC_XCHG8(" in out, out
    out, _ = lift(b"\x87\xc8")                # xchg eax, ecx: registers only, unchanged
    assert "RECOMP_ATOMIC" not in out, out


def test_flags_as_the_unlocked_form():
    locked, _ = lift(b"\xf0\x01\x08")
    plain, _ = lift(b"\x01\x08")
    # The same flag statements, on the local copy instead of memory.
    want = [l.strip() for l in plain.replace("MEM32(eax)", "_lk").splitlines()]
    got = [l.strip() for l in locked.splitlines()]
    assert got[3:3 + len(want)] == want, (plain, locked)


def block(code):
    """A basic block lifted with flag tracking, as the translator does."""
    from tools.recomp.disasm import BasicBlock
    from tools.recomp.lifter import lift_basic_block
    insns = list(Disassembler()._cfg_stream(code, 0, len(code), 0x1000))
    out, state = lift_basic_block(Lifter(), BasicBlock(start=0x1000, instructions=insns))
    return "\n".join(out), state


@pytest.mark.parametrize("code,setter", [
    (b"\xf0\xff\x08\x75\x10", "dec"),          # lock dec [eax]; jnz: InterlockedDecrement
    (b"\xf0\xff\x00\x75\x10", "inc"),
    (b"\xf0\x01\x08\x75\x10", "add"),
    (b"\xf0\x0f\xc7\x08\x75\x10", "cmpxchg8b"),
])
def test_a_jcc_after_a_locked_form_reads_its_flags(code, setter):
    """The jnz after `lock dec [x]` read the unassigned _flags: the tracking
    did not know the prefixed mnemonic (fixed with XBA-71)."""
    out, state = block(code)
    assert state and state[0] == setter, state
    assert "if (_flags" not in out, out


@pytest.mark.parametrize("code,want", [
    (b"\xf0\x0f\xc0\x08", "RECOMP_ATOMIC_CAS8"),       # lock xadd byte [eax], cl
    (b"\xf0\x66\x0f\xc1\x08", "RECOMP_ATOMIC_CAS16"), # lock xadd word [eax], cx
    (b"\xf0\x0f\xb0\x08", "RECOMP_ATOMIC_CAS8"),       # lock cmpxchg byte [eax], cl
    (b"\xf0\x66\x0f\xb1\x08", "RECOMP_ATOMIC_CAS16"), # lock cmpxchg word [eax], cx
    (b"\xf0\x0f\xc7\x08", "RECOMP_ATOMIC_CAS64"),      # lock cmpxchg8b [eax]
    (b"\x0f\xc1\xc8", "_s = (uint32_t)(_a + _b)"),      # xadd eax, ecx
    (b"\x0f\xb1\xd1", "if (_old == _cmp)"),             # cmpxchg ecx, edx
])
def test_the_other_atomic_forms_lift(code, want):
    out, _ = lift(code)
    assert "RECOMP_UNIMPL" not in out and want in out, out


@pytest.mark.parametrize("code", [
    b"\xf0\x0f\xba\x28\x00\x72\x10",      # lock bts dword [eax], 0 / jc: a spinlock
    b"\xf0\x0f\xba\x30\x00\x73\x10",      # lock btr dword [eax], 0 / jnc
    b"\xf0\x01\x08\x72\x10",                # lock add [eax], ecx / jc
])
def test_a_carry_test_after_a_locked_form_declares_cf(code):
    """The translator decides whether a function declares _cf from the
    mnemonics; it did not know the lock-prefixed ones, so a `jc` after
    `lock bts` emitted an undeclared _cf and the title did not compile."""
    from tools.recomp.translator import FunctionTranslator
    insns = list(Disassembler()._cfg_stream(code, 0, len(code), 0x1000))
    assert FunctionTranslator._function_needs_cf(insns)


def test_adc_stays_unhandled():
    out, _ = lift(b"\xf0\x11\x08")            # lock adc: reads CF, which a retry would see changed
    assert "RECOMP_UNIMPL" in out, out


def _atomic_macros():
    """The POSIX RECOMP_ATOMIC_* definitions from the runtime header, and the
    cmpxchg8b helper they use."""
    with open(os.path.join(ROOT, "templates", "runtime", "recomp_types.h")) as f:
        text = f.read()
    lines = [l for l in text.splitlines() if l.startswith("#define RECOMP_ATOMIC_") and "_Interlocked" not in l]
    assert len(lines) >= 11
    helper = re.search(r"static inline uint64_t recomp_atomic_cas64\(.*?\n\}\n", text, re.S).group(0)
    return "volatile int g_recomp_cas64_lock;\n" + "\n".join(lines) + "\n" + helper


HARNESS = r"""
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
static uint8_t g_mem[4096] __attribute__((aligned(64)));
static uintptr_t g_xbox_mem_offset;
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))
#define LO8(x) ((uint8_t)(x))
#define SET_LO8(r, v) ((r) = ((r) & ~0xFFu) | ((uint32_t)(v) & 0xFFu))
%(macros)s
#define N 200000
static void *worker(void *arg)
{
    uint32_t eax, ecx, edx, ebx;
    uint32_t _fa, _fb; int32_t _fas, _fbs; int _cf;
    int t = (int)(intptr_t)arg;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs; (void)_cf;
    for (int i = 0; i < N; i++) {
        eax = 0x100; %(inc)s
        eax = 0x104; ecx = 3; %(add)s
        eax = 0x200; ecx = (uint32_t)(t * 64 + (i & 63)); %(bts)s
        eax = 0x140; ecx = 1; %(xadd8)s
        /* A 64-bit counter bumped with lock cmpxchg8b [esi], retried until ZF. */
        for (;;) {
            uint32_t esi = 0x150;
            uint64_t v = *(volatile uint64_t *)XBOX_PTR(esi), nv = v + 0x100000001ull;
            eax = (uint32_t)v; edx = (uint32_t)(v >> 32); ebx = (uint32_t)nv; ecx = (uint32_t)(nv >> 32);
            %(cas8b)s
            if (_fa == _fb)
                break;
        }
        /* A spinlock on xchg around a plain increment. */
        do { eax = 0x108; ecx = 1; %(xchg)s } while (ecx);
        MEM32(0x10C) = MEM32(0x10C) + 1;
        __atomic_store_n((volatile uint32_t *)XBOX_PTR(0x108), 0u, __ATOMIC_RELEASE);
    }
    return NULL;
}
int main(void)
{
    pthread_t th[4];
    g_xbox_mem_offset = (uintptr_t)g_mem;
    for (int t = 0; t < 4; t++) pthread_create(&th[t], NULL, worker, (void *)(intptr_t)t);
    for (int t = 0; t < 4; t++) pthread_join(th[t], NULL);
    unsigned bits = 0;
    for (int b = 0; b < 256; b++) bits += (MEM32(0x200 + (b >> 5) * 4) >> (b & 31)) & 1u;
    printf("%%u %%u %%u %%u %%u %%u %%u\n", MEM32(0x100), MEM32(0x104), bits, MEM32(0x10C), (unsigned)MEM8(0x140),
           MEM32(0x150), MEM32(0x154));
    return 0;
}
"""


def test_four_threads_lose_no_update(tmp_path):
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("no cc")
    src = HARNESS % {"macros": _atomic_macros(),
                     "inc": lift(b"\xf0\xff\x00")[0],
                     "add": lift(b"\xf0\x01\x08")[0],
                     "bts": lift(b"\xf0\x0f\xab\x08")[0],
                     "xchg": lift(b"\x87\x08")[0],
                     "xadd8": lift(b"\xf0\x0f\xc0\x08")[0],
                     "cas8b": lift(b"\xf0\x0f\xc7\x0e")[0]}       # lock cmpxchg8b [esi]
    (tmp_path / "t.c").write_text(src)
    exe = str(tmp_path / "t")
    subprocess.check_call([cc, "-O2", "-pthread", "-std=gnu11", "-w", "-o", exe, str(tmp_path / "t.c")])
    n = 200000 * 4
    # A spinlock on a non-atomic xchg can hang outright (the lifting before XBA-78 did).
    out = subprocess.check_output([exe], text=True, timeout=60)
    assert out.split() == [str(n), str(3 * n), "256", str(n), str(n & 0xFF), str(n), str(n)]
