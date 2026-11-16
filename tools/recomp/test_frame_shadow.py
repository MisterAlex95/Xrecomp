"""The guest frame shadow stack and the call-site table (XBA-247).

A lifted function that may reach a park point records itself on entry
(RECOMP_FRAME_ENTRY: its entry esp and its caller's published ebp) and drops
its frame at every way out (RECOMP_FRAME_LEAVE); functions that can never
park (FunctionTranslator._park_plan) record nothing, and nothing is emitted at
a call. Call sites into functions that may park are noted in the lifter's
call-site table, which the batch translator writes into recomp_dispatch.c and
recomp_dispatch_init hands to the runtime.

The C fixture lifts a chain three calls deep (a frame function, an FPO
function using ebp as a scratch register, a function that never touches
ebp) into a hand-written « kernel call », against the real recomp_types.h:
the callee reads the chain from g_cpu (frame count, each frame's esp and
return address, each ebp), and every frame is gone once the chain returns.
"""
import os
import shutil
import subprocess

import pytest

from tools.recomp import config
from tools.recomp.disasm import Instruction, Operand
from tools.recomp.lifter import Lifter
from tools.recomp.translator import BatchTranslator, FunctionTranslator

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RUNTIME = os.path.join(ROOT, "templates", "runtime")

_CONFIG_KEYS = ('_SECTIONS', 'SECTIONS', '_configured_from', 'TEXT_VA_START',
                'TEXT_VA_END', 'RDATA_VA_START', 'RDATA_VA_END', 'DATA_VA_START',
                'DATA_VA_END', 'KERNEL_THUNK_ADDR', 'ENTRY_POINT')


def _call(addr, target=None, op=None):
    i = Instruction(addr, 5 if target else 2, "call", "", "")
    i.call_target = target
    i.operands = [op] if op else [Operand(type="imm", imm=target)]
    return i


def _install(monkeypatch, funcs, base=0x10000):
    """funcs: {offset: hex}; returns (image, db) with exact function ends."""
    image = b""
    for off in sorted(funcs):
        image = image.ljust(off, b"\x90") + bytes.fromhex(funcs[off].replace(" ", ""))
    image = image.ljust(max(funcs) + 0x30, b"\x90")
    for key in _CONFIG_KEYS:
        monkeypatch.setattr(config, key, getattr(config, key))
    config._install([config.Section('.text', base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin='frame-shadow-test')
    db = {}
    for off, hx in funcs.items():
        n = len(bytes.fromhex(hx.replace(" ", "")))
        db[base + off] = {'start': hex(base + off), 'end': base + off + n, '_addr': base + off, 'size': n}
    return image, db


def test_nothing_at_a_call_the_site_is_noted():
    lifter = Lifter(func_db={0x11000: {"name": "sub_00011000"}})
    lifter.func_start = 0x10000
    lifter.has_ebp_local = True
    text = "\n".join(lifter.lift_instruction(_call(0x10010, 0x11000)))
    assert "RECOMP_FRAME" not in text
    assert "PUSH32(esp, 0x00010015u); _rs_00010015: RECOMP_ABI_CALL(0x00011000u, sub_00011000);" in text
    assert lifter.call_site_table == {(0x10015, 0x10000): 0}


def test_call_into_a_quiet_function_is_no_site():
    lifter = Lifter(func_db={0x11000: {"name": "sub_00011000"}})
    lifter.func_start = 0x10000
    lifter.quiet_functions = frozenset({0x11000})
    lifter.lift_instruction(_call(0x10010, 0x11000))
    assert lifter.call_site_table == {}


def test_caller_without_ebp_flags_its_sites():
    lifter = Lifter(func_db={0x11000: {"name": "sub_00011000"}})
    lifter.func_start = 0x10000
    lifter.lift_instruction(_call(0x10010, 0x11000))
    assert lifter.call_site_table == {(0x10015, 0x10000): Lifter.CALL_SITE_NO_EBP}


def test_indirect_calls_are_sites():
    for recorded in (None, [0x11000, 0x11100]):
        lifter = Lifter(func_db={0x11000: {"name": "sub_00011000"},
                                 0x11100: {"name": "sub_00011100"}})
        lifter.func_start = 0x10000
        lifter.has_ebp_local = True
        lifter.quiet_functions = frozenset({0x11000, 0x11100})
        if recorded:
            lifter.icall_site_targets = {0x10020: recorded}
        text = "\n".join(lifter.lift_instruction(
            _call(0x10020, op=Operand(type="reg", reg="eax"))))
        assert text.count("{") == text.count("}"), text
        # The fallback can reach anything: the site is noted even when every
        # guarded target is quiet.
        assert lifter.call_site_table == {(0x10022, 0x10000): Lifter.CALL_SITE_INDIRECT}


def test_seh_helper_and_stack_switch_flags():
    lifter = Lifter(func_db={0x11000: {"name": "sub_00011000"}})
    lifter.func_start = 0x10000
    lifter.has_ebp_local = True
    lifter.SEH_HELPERS = frozenset({0x11000})
    lifter.stack_switch_sites = frozenset({0x10004})
    lifter.lift_instruction(_call(0x10010, 0x11000))
    assert lifter.call_site_table[(0x10015, 0x10000)] == (
        Lifter.CALL_SITE_SEH_HELPER | Lifter.CALL_SITE_STACK_SWITCH)


def test_dispatch_table_carries_the_sorted_sites_and_registers_them(tmp_path):
    cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    (tmp_path / "recomp_funcs.h").write_text(
        "#include <stdint.h>\n#include <stddef.h>\nvoid f_a(void);\n")
    disp = tmp_path / "recomp_dispatch.c"
    sites = [(0x10005, 0x10000, 0), (0x10105, 0x10100, 3)]
    BatchTranslator._write_dispatch_table(
        object.__new__(BatchTranslator), [(0x10000, "f_a", "")], str(disp),
        "recomp_funcs.h", sites)
    src = disp.read_text()
    assert "0x00010005u, 0x00010000u, 0x00u," in src
    assert "g_recomp_call_site_count = 2u" in src
    harness = tmp_path / "h.c"
    harness.write_text(r'''
#include <stdint.h>
#include <stddef.h>
typedef void (*recomp_func_t)(void);
void f_a(void) {}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
int recomp_dispatch_init(void);
#ifdef HOOK
static const uint32_t *got; static uint32_t got_n;
void recomp_call_sites_register(const uint32_t *s, uint32_t n) { got = s; got_n = n; }
#endif
int main(void) {
    recomp_dispatch_init();
#ifdef HOOK
    if (got_n != 2 || got[3] != 0x10105u || got[5] != 3u) return 1;
#endif
    return 0;
}
''')
    for hook in (True, False):     # weak: a runtime without the hook still links
        exe = tmp_path / ("h%d" % hook)
        r = subprocess.run([cc, "-O1"] + (["-DHOOK"] if hook else []) +
                           [str(harness), str(disp), "-o", str(exe)],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        assert subprocess.run([str(exe)]).returncode == 0


def test_park_plan_records_only_functions_that_may_park(monkeypatch):
    """_park_plan: a leaf, a function that only calls leaves and a call into
    nowhere (a stub that only returns) can never park; a function with an
    indirect call (a kernel thunk is called that way), a hlt, a poll loop, or
    that reaches one of those, may. Only those record a frame."""
    image, db = _install(monkeypatch, {
        0x00: "e8 1b000000 e8 36000000 c3",   # caller: call leafer (0x20); call icaller (0x40)
        0x20: "e8 5b000000 c3",                # leafer: call leaf (0x80)
        0x40: "ffd0 c3",                       # icaller: call eax
        0x50: "f4 c3",                         # halter: hlt
        0x60: "f7050020000001000000 75f4 c3",  # poller: test dword [0x2000], 1; jne self
        0x80: "40 c3",                         # leaf: inc eax
        0x90: "e8 5b000000 c3",                # stubber: call 0x100F0 (no function there)
        0xA0: "e8 5c000000 c3",                # midder: call 0x10101, inside icaller2
        0x100: "ffd0 ffd0 c3",                 # icaller2
    })
    base = 0x10000
    tr = FunctionTranslator(image, db)
    assert tr._park_plan() == frozenset({base + 0x20, base + 0x80, base + 0x90})
    code = tr.translate_function(base, db[base])
    assert code.count("RECOMP_FRAME_ENTRY_R();") == 1
    assert "RECOMP_FRAME_LEAVE(); return;" in code
    assert (0x10005, base) not in tr.lifter.call_site_table      # into the quiet leafer
    assert (0x1000A, base) in tr.lifter.call_site_table          # into icaller
    assert "RECOMP_FRAME" not in tr.translate_function(base + 0x20, db[base + 0x20])
    halter = tr.translate_function(base + 0x50, db[base + 0x50])
    assert "RECOMP_FRAME_ENTRY_R();" in halter


# f0 0x10000: push ebp; mov ebp, esp; call f1; mov [0x2008], ebp; pop ebp; ret
# f1 0x10030: mov ebp, 0x11111111; call f2; mov [0x2000], ebp; ret
# f2 0x10060: call f3; ret                  (never touches ebp)
# f3 0x10090: the "kernel call", hand-written by the harness (a manual function: it may park, so f2, f1 and f0,
#             which reach it, may too)
F0 = "55 8bec e8 28000000 892d08200000 5d c3"
F1 = "bd11111111 e8 26000000 892d00200000 c3"
F2 = "e8 2b000000 c3"

HARNESS = r'''
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <stdio.h>
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS RecompCpu g_cpu = { .r_eflags = 0x202u };
recomp_flat_dispatch_t g_recomp_flat;
uint32_t g_xbox_code_lo, g_xbox_code_hi;
static uint8_t g_mem[0x4000];
static int bad;
#define WANT(c) do { if (!(c)) { printf("FAIL %%s\n", #c); bad++; } } while (0)
static void f3(void)
{
    RecompFrame *f = g_cpu.r_fbase;
    WANT(f && g_cpu.r_ftop == f + 3);                      /* f0, f1, f2 recorded */
    WANT(f[0].f_esp == 0x3000u - 4u && MEM32(f[0].f_esp) == 0u);   /* f0, entered by main */
    WANT(MEM32(f[1].f_esp) == 0x00010008u);              /* f1, called from f0 */
    WANT(MEM32(f[2].f_esp) == 0x0001003Au);              /* f2, called from f1 */
    WANT(g_esp == f[2].f_esp - 4u);                      /* this call, from f2 */
    WANT(f[1].f_ebp == 0x3000u - 8u);                    /* f0's frame, published before its call */
    WANT(f[2].f_ebp == 0x11111111u);                     /* f1's scratch ebp */
    g_esp += 4;                                          /* ret */
}
recomp_func_t recomp_lookup_manual(uint32_t va) { return va == 0x10090u ? f3 : 0; }
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return 0; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return 0; }
void recomp_icall_fail_log(uint32_t va) { printf("FAIL lookup %%x\n", va); bad++; }
void recomp_icall_not_code_log(uint32_t va) { printf("FAIL not code %%x\n", va); bad++; }
void sub_00010030(void); void sub_00010060(void);
%(bodies)s
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_mem;
    g_ebp = 0x0BADF00Du;
    g_esp = 0x3000u;
    MEM32(g_esp - 4) = 0;
    g_esp -= 4;                                           /* f0's return address */
    sub_00010000();
    WANT(MEM32(0x2000u) == 0x11111111u);                 /* f1 kept its ebp */
    WANT(MEM32(0x2008u) == 0x3000u - 8u);                /* f0 too */
    WANT(g_cpu.r_ftop == g_cpu.r_fbase && g_esp == 0x3000u);   /* every frame dropped */
    return bad;
}
'''


def test_three_deep_chain_in_c(tmp_path, monkeypatch):
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        pytest.skip("gcc or clang required")
    base = 0x10000
    image, db = _install(monkeypatch, {0: F0, 0x30: F1, 0x60: F2})
    tr = FunctionTranslator(image, db)
    tr.lifter.manual_functions = {base + 0x90}
    bodies = "\n".join(tr.translate_function(a, db[a]) for a in (base, base + 0x30, base + 0x60))
    assert tr._park_plan() == frozenset()
    assert bodies.count("RECOMP_FRAME_ENTRY_R();") == 3
    table = tr.lifter.call_site_table
    assert table[(0x10008, 0x10000)] == 0
    assert table[(0x1003A, 0x10030)] == 0
    assert (0x10065, 0x10060) in table
    src = tmp_path / "chain.c"
    src.write_text(HARNESS % {"bodies": bodies})
    exe = tmp_path / "chain"
    for opt in ("-O0", "-O2"):
        r = subprocess.run([cc, "-std=gnu11", opt, "-w", "-I", RUNTIME, str(src), "-o", str(exe), "-lm",
                            "-Wl,--unresolved-symbols=ignore-all"], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr[-3000:]
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        assert run.returncode == 0, run.stdout + bodies


MACROS = r'''
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <stdlib.h>
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS RecompCpu g_cpu;
static uint32_t deepest;
static int recurse(uint32_t n)
{
    RECOMP_FRAME_ENTRY();
    esp -= 4;
    if (n) {
        if (recurse(n - 1)) { RECOMP_FRAME_LEAVE(); return 1; }
    } else {
        deepest = (uint32_t)(g_cpu.r_ftop - g_cpu.r_fbase);
    }
    esp += 4;
    RECOMP_FRAME_LEAVE();
    return 0;
}
int main(void)
{
    esp = 0x100000u;
    recurse(9);                                  /* the array is allocated on the first entry */
    if (!g_cpu.r_fbase || g_cpu.r_ftop != g_cpu.r_fbase || deepest != 10)
        return 1;
    recurse(RECOMP_FRAME_SLOTS + 20);            /* past the slots: the spare, then back */
    if (g_cpu.r_ftop != g_cpu.r_fbase || deepest != RECOMP_FRAME_SLOTS + 2)
        return 2;
    return 0;
}
'''


def test_macros_allocate_and_survive_past_the_slots(tmp_path):
    """ENTRY allocates the thread's array on first use (g_cpu starts zeroed),
    LEAVE puts the top back; past RECOMP_FRAME_SLOTS frames the top stays
    past the limit (the chain says OVERFLOW) and comes back exactly."""
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        pytest.skip("gcc or clang required")
    src = tmp_path / "m.c"
    src.write_text(MACROS)
    exe = tmp_path / "m"
    r = subprocess.run([cc, "-std=gnu11", "-O2", "-w", "-I", RUNTIME, str(src), "-o", str(exe), "-lm",
                        "-Wl,--unresolved-symbols=ignore-all"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-3000:]
    assert subprocess.run([str(exe)]).returncode == 0
