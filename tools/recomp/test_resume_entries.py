"""Resume entries in lifted code (XBA-250).

A save state loaded in a fresh process has no host stacks for its threads:
lifted code keeps the return path in host frames. Each thread rebuilds them by
calling its saved chain again (as Emscripten's Asyncify rewinds): every
function that may park enters through RECOMP_FRAME_ENTRY_R, which, while the
thread is resuming, jumps to the function's resume block; the block asks the
runtime (src/kernel/xbox_resume.c) where the function was and jumps to the
label before that call (`_rs_<return VA>`), or after that host wait
(`_rh_<VA>`); the call is made again into the next function of the chain.

The C fixture lifts a chain four calls deep (f0 with a frame and a register
set before its call, f1 with a scratch ebp, f2 calling f3 through a register,
f3 with a frame and a local, making the « kernel call » through memory),
saves the state from inside the kernel call in one process (frames,
registers, guest memory; the process then exits) and resumes it in a second
process with the real resume engine: the chain continues from the same point
with the same registers, ebp values and memory, and ends as an uninterrupted
run does. Both kinds of park record: still in the call (made again) and
returned (its status given back).
"""
import os
import re
import shutil
import subprocess

import pytest

from tools.recomp.disasm import Operand
from tools.recomp.lifter import Lifter
from tools.recomp.test_frame_shadow import _call, _install
from tools.recomp.translator import FunctionTranslator

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RUNTIME = os.path.join(ROOT, "templates", "runtime")
ENGINE = os.path.join(ROOT, "src", "kernel", "xbox_resume.c")


def _lifter():
    lifter = Lifter(func_db={0x11000: {"name": "sub_00011000"},
                             0x11100: {"name": "sub_00011100"}})
    lifter.func_start = 0x10000
    lifter.has_ebp_local = True
    return lifter


def test_a_direct_call_gets_a_label_before_the_call():
    lifter = _lifter()
    text = "\n".join(lifter.lift_instruction(_call(0x10010, 0x11000)))
    assert "PUSH32(esp, 0x00010015u); _rs_00010015: RECOMP_ABI_CALL(0x00011000u, sub_00011000);" in text
    assert lifter.resume_sites == {0x10015: "_rs_00010015"}


def test_an_indirect_call_takes_its_target_from_the_runtime_before_its_arms():
    for recorded in (None, [0x11000, 0x11100]):
        lifter = _lifter()
        if recorded:
            lifter.icall_site_targets = {0x10020: recorded}
        text = "\n".join(lifter.lift_instruction(_call(0x10020, op=Operand(type="reg", reg="eax"))))
        entry = "if (0) { _rs_00010022: _icall_target = eax; }"
        assert text.count(entry) == 1, text
        # After the return address is pushed, before the arms and the fallback.
        assert text.index("PUSH32(esp, 0x00010022u)") < text.index(entry)
        assert text.index(entry) < text.index("RECOMP_ICALL_SAFE_AT")
        assert text.count("_rs_00010022") == 1
        assert text.count("{") == text.count("}"), text


def test_setjmp_longjmp_and_hand_written_callees_get_no_entry():
    lifter = _lifter()
    lifter.SETJMP_FN = 0x11000
    lifter.manual_functions = {0x11100}
    text = "\n".join(lifter.lift_instruction(_call(0x10010, 0x11000))
                     + lifter.lift_instruction(_call(0x10020, 0x11100)))
    assert "_rs_" not in text
    assert lifter.call_site_table[(0x10015, 0x10000)] & Lifter.CALL_SITE_JMPBUF
    assert lifter.call_site_table[(0x10025, 0x10000)] & Lifter.CALL_SITE_MANUAL
    assert lifter.resume_sites == {}


def test_the_same_call_lifted_twice_keeps_one_entry():
    """The translator keeps the first label of a call emitted twice, and a
    site whose label was never emitted leaves the block (NO_RESUME)."""
    tr = object.__new__(FunctionTranslator)
    tr.lifter = _lifter()
    a = "    " + "\n".join(tr.lifter.lift_instruction(_call(0x10010, 0x11000)))
    b = "    " + "\n".join(tr.lifter.lift_instruction(_call(0x10010, 0x11000)))
    c = "    " + "\n".join(tr.lifter.lift_instruction(_call(0x10020, op=Operand(type="reg", reg="eax"))))
    tr.lifter.resume_sites[0x10099] = "_rs_00010099"
    body = tr._resume_dedupe(0x10000, [a, b, c, c])
    text = "\n".join(body)
    assert text.count("_rs_00010015:") == 1 and text.count("RECOMP_ABI_CALL") == 2
    assert text.count("_rs_00010022:") == 1 and text.count("_rs_00010022: _icall_target = eax") == 1
    assert text.count("RECOMP_ICALL_SAFE_AT") == 2 and text.count("{") == text.count("}")
    assert 0x10099 not in tr.lifter.resume_sites
    assert tr.lifter.call_site_table[(0x10099, 0x10000)] & Lifter.CALL_SITE_NO_RESUME


def test_a_function_with_an_seh_anchor_flags_its_sites():
    lifter = _lifter()
    lifter.seh_anchor_after = 0x10004
    lifter.lift_instruction(_call(0x10010, 0x11000))
    assert lifter.call_site_table[(0x10015, 0x10000)] & Lifter.CALL_SITE_SEH_ANCHOR


def test_resume_block_lists_every_entry_and_host_waits(monkeypatch):
    """A function with a call, a hlt and a poll loop: one case per site in
    its resume block, the host waits keyed 0x80000000 | VA in the call-site
    table; the call and the hlt (outside any loop) have their labels in
    place, the poll loop is entered at its header (no label in the loop); a
    function that can never park has neither entry nor block."""
    image, db = _install(monkeypatch, {
        0x00: "e8 3b000000 f4 f7050020000001000000 75f4 c3",  # call icaller; hlt; poll: test [0x2000],1; jne poll; ret
        0x40: "ffd0 c3",                                       # icaller: call eax
        0x80: "40 c3",                                         # leaf
    })
    base = 0x10000
    tr = FunctionTranslator(image, db)
    code = tr.translate_function(base, db[base])
    assert "RECOMP_FRAME_ENTRY_R();" in code
    assert "_rs_00010005: RECOMP_ABI_CALL(0x00010040u, sub_00010040);" in code
    assert "xbox_Halt(0x00010005u); _rh_00010005: ;" in code
    assert re.search(r"RECOMP_POLL_WAIT\([^;]*0x00010010u\); goto loc_00010006; }", code), code
    assert "_rh_00010010" not in code
    block = code[code.index("_recomp_resume:"):]
    assert "recomp_resume_site(RECOMP_OPAQUE32(0x00010000u))" in block
    assert "ebp = g_ebp;" in block
    assert "case 0x00010005u: goto _rs_00010005;" in block
    assert "case 0x80010005u: goto _rh_00010005;" in block
    assert "case 0x80010010u: goto loc_00010006;" in block
    assert "recomp_resume_lost(0x00010000u, _rs_site);" in block
    table = tr.lifter.call_site_table
    assert table[(0x80010005, base)] & Lifter.CALL_SITE_HOST_WAIT
    assert table[(0x80010010, base)] & Lifter.CALL_SITE_HOST_WAIT
    leaf = tr.translate_function(base + 0x80, db[base + 0x80])
    assert "_recomp_resume" not in leaf and "RECOMP_FRAME" not in leaf


def test_a_back_edge_publishes_ebp_and_enters_at_its_header(monkeypatch):
    # push ebp; mov ebp, esp; L: dec ecx; jnz L; pop ebp; ret -- with --back-edge-yield
    image, db = _install(monkeypatch, {0x00: "55 8bec 49 75fd 5d c3"})
    tr = FunctionTranslator(image, db)
    tr.back_edge_yield = True
    code = tr.translate_function(0x10000, db[0x10000])
    assert "{ RECOMP_BACK_EDGE_EBP(ebp, 0x00010004u); goto loc_00010003; }" in code, code
    assert "case 0x80010004u: goto loc_00010003;" in code


def test_a_call_inside_a_loop_is_entered_in_a_copy_of_the_loop(monkeypatch):
    """The label of a call inside a loop would be a second entry into the
    loop (irreducible: the compiler no longer hoists its invariant loads).
    The loop has no label; the resume block enters a copy of the loop,
    placed after it, whose labels are renamed and which leaves to the code
    after the loop. A call before the loop keeps its label in place."""
    image, db = _install(monkeypatch, {
        # call icaller; L: call icaller; dec ecx; jnz L; call icaller; ret
        0x00: "e8 3b000000 e8 36000000 49 75f8 e8 2e000000 c3",
        0x40: "ffd0 c3",
    })
    tr = FunctionTranslator(image, db)
    code = tr.translate_function(0x10000, db[0x10000])
    head, block = code.split("_recomp_resume:", 1)
    assert "_rs_00010005: RECOMP_ABI_CALL" in head                 # before the loop: in place
    assert "_rs_0001000A" not in head                              # in the loop: not in the original
    assert "_rs_00010012: RECOMP_ABI_CALL" in head                 # after the loop: in place
    assert "case 0x0001000Au: goto _rs_0001000A_d0;" in block, block
    copy = block[block.index("again, entered by a resume"):]
    assert "loc_00010005_d0: ;" in copy and "_rs_0001000A_d0: RECOMP_ABI_CALL" in copy
    assert "goto loc_00010005_d0;" in copy                        # its back edge stays in the copy
    assert "goto loc_0001000D;" in copy                          # it leaves to the code after the loop


def test_a_call_in_nested_loops_copies_the_outermost(monkeypatch):
    """A call in an inner loop: the copy is the outer loop (the inner one in
    it), so the original outer loop gets no second entry either."""
    image, db = _install(monkeypatch, {
        # mov edi, 2; L1: mov ecx, 3; L2: call icaller; dec ecx; jnz L2; dec edi; jnz L1; ret
        0x00: "bf02000000 b903000000 e8 31000000 49 75f8 4f 75f0 c3",
        0x40: "ffd0 c3",
    })
    tr = FunctionTranslator(image, db)
    code = tr.translate_function(0x10000, db[0x10000])
    head, block = code.split("_recomp_resume:", 1)
    assert "_rs_0001000F" not in head
    assert "goto _rs_0001000F_d0;" in block
    copy = block[block.index("again, entered by a resume"):]
    assert "the loop 0x00010005..0x" in block
    for label in ("loc_00010005_d0: ;", "loc_0001000A_d0: ;", "goto loc_0001000A_d0;", "goto loc_00010005_d0;",
                  "goto loc_00010015;"):
        assert label in copy, copy
    assert "goto loc_00010005;" not in copy and "goto loc_0001000A;" not in copy


# f0 0x10000: push ebp; mov ebp, esp; mov esi, 0x5150; call f1; mov [0x2008], ebp; mov [0x200C], esi; pop ebp; ret
# f1 0x10030: mov ebp, 0x11111111; mov edi, 2; L: call f2; dec edi; jnz L; mov [0x2000], ebp; ret
#             (the call inside a loop: entered through the loop's copy)
# f2 0x10060: mov eax, [0x2104]; call eax; ret                    (no ebp; an indirect call into f3)
# f3 0x10090: push ebp; mov ebp, esp; sub esp, 8; mov dword [ebp-4], 0x77; call [0x2100] (the kernel call);
#             add eax, [ebp-4]; mov [0x2010], eax; mov esp, ebp; pop ebp; ret
F0 = "55 8bec be50510000 e8 23000000 892d08200000 89350c200000 5d c3"
F1 = "bd11111111 bf02000000 e8 21000000 4f 75f8 892d00200000 c3"
F2 = "a104210000 ffd0 c3"
F3 = "55 8bec 83ec08 c745fc77000000 ff1500210000 0345fc a310200000 8be5 5d c3"
KERNEL_RET = 0x100A3

HARNESS = r'''
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS RecompCpu g_cpu = { .r_eflags = 0x202u };
recomp_flat_dispatch_t g_recomp_flat;
uint32_t g_xbox_code_lo, g_xbox_code_hi;
static uint8_t g_mem[0x4000];
static int bad;
#define WANT(c) do { if (!(c)) { printf("FAIL %%s\n", #c); bad++; } } while (0)

/* The engine (src/kernel/xbox_resume.c), as the runtime has it. */
typedef struct { uint32_t f_ret, f_esp, f_ebp; } ChainFrame;
typedef int (*Lookup)(uint32_t, uint32_t *, uint32_t *);
typedef struct { uint32_t n, at, entry_esp, entry_ebp, site_kind, ordinal, returned, call_esp;
                 RecompCpuRegs regs; volatile int32_t done; } PlanHead;
void *xbox_ResumePlanBuild(const ChainFrame *fr, uint32_t n, uint32_t site_kind, uint32_t site_va, uint32_t ordinal,
                           uint32_t kernel_target, int returned, uint32_t call_esp, const RecompCpuRegs *regs,
                           Lookup lookup, char *why, size_t why_n);
void xbox_ResumeBegin(void *plan);
int xbox_ResumeKernelPending(void);
int xbox_ResumeKernelArrive(uint32_t ordinal);
uint32_t xbox_ResumeCompleted(void);

static const uint32_t SITES[] = {
%(sites)s
};
static int lookup(uint32_t key, uint32_t *func, uint32_t *flags)
{
    unsigned i;
    for (i = 0; i < sizeof SITES / sizeof SITES[0]; i += 3)
        if (SITES[i] == key) { *func = SITES[i + 1]; *flags = SITES[i + 2]; return 1; }
    return 0;
}

#define THUNK 0xFE000004u
#define ORD 99u
#define STACK 0x3000u
typedef struct { uint32_t n; ChainFrame f[8]; RecompCpuRegs regs; uint32_t returned, call_esp; uint8_t mem[0x4000]; } Saved;
static const char *g_mode, *g_path;
static Saved g_saved;

static void save_and_die(uint32_t returned, uint32_t call_esp)
{
    Saved *s = &g_saved;
    uint32_t i;
    FILE *f;
    s->n = (uint32_t)(g_cpu.r_ftop - g_cpu.r_fbase);
    for (i = 0; i < s->n && i < 8; i++) {
        s->f[i].f_esp = g_cpu.r_fbase[i].f_esp;
        s->f[i].f_ebp = g_cpu.r_fbase[i].f_ebp;
        s->f[i].f_ret = MEM32(s->f[i].f_esp);
    }
    memcpy(&s->regs, &g_cpu, sizeof s->regs);
    s->returned = returned;
    s->call_esp = call_esp;
    memcpy(s->mem, g_mem, sizeof g_mem);
    f = fopen(g_path, "wb");
    if (!f || fwrite(s, sizeof *s, 1, f) != 1) exit(3);
    fclose(f);
    exit(0);                                              /* the process ends inside the kernel call */
}

/* The kernel call: KeDelayExecutionThread-like, no arguments, returns 0x1000. */
static int g_kcalls;
static void kernel(void)
{
    uint32_t call_esp = g_esp;
    g_kcalls++;
    if (xbox_ResumeKernelPending()) {
        int returned = xbox_ResumeKernelArrive(ORD);
        WANT(g_esp == g_saved.call_esp + (returned ? 4u : 0u));
        WANT(g_esi == 0x5150u && g_ebp == g_saved.regs.r_ebp);
        if (returned) return;                              /* the status it had returned, given back */
        WANT(MEM32(g_esp) == 0x%(kret)08Xu);              /* made again from its call site */
    }
    WANT(MEM32(g_esp) == 0x%(kret)08Xu);
    if (!strcmp(g_mode, "save-in") && g_kcalls == 1)
        save_and_die(0, call_esp);
    g_eax = 0x1000u;
    g_esp += 4;                                           /* ret */
    if (!strcmp(g_mode, "save-ret") && g_kcalls == 1)
        save_and_die(1, call_esp);
}

void sub_00010000(void); void sub_00010030(void); void sub_00010060(void); void sub_00010090(void);
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
recomp_func_t recomp_lookup(uint32_t va)
{
    return va == 0x10000u ? sub_00010000 : va == 0x10030u ? sub_00010030 : va == 0x10060u ? sub_00010060
         : va == 0x10090u ? sub_00010090 : 0;
}
recomp_func_t recomp_lookup_kernel(uint32_t va) { return va == THUNK ? kernel : 0; }
void recomp_icall_fail_log(uint32_t va) { printf("FAIL lookup %%x\n", va); bad++; }
void recomp_icall_not_code_log(uint32_t va) { printf("FAIL not code %%x\n", va); bad++; }
recomp_func_t recomp_icall_install(uint32_t va, recomp_func_t fn) { (void)va; return fn; }   /* the generated dispatch's (XBA-304) */
%(bodies)s
int main(int argc, char **argv)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_mem;
    g_mode = argc > 1 ? argv[1] : "plain";
    g_path = argc > 2 ? argv[2] : "state.bin";
    MEM32(0x2100u) = THUNK;
    MEM32(0x2104u) = 0x10090u;
    g_ebp = 0x0BADF00Du;
    g_esp = STACK;
    MEM32(g_esp - 4) = 0;
    g_esp -= 4;                                           /* the routine's return address: the runtime's 0 */
    if (!strcmp(g_mode, "resume")) {
        char why[200];
        void *plan;
        FILE *f = fopen(g_path, "rb");
        if (!f || fread(&g_saved, sizeof g_saved, 1, f) != 1) return 4;
        fclose(f);
        WANT(g_saved.n == 4);
        memcpy(g_mem, g_saved.mem, sizeof g_mem);         /* the state's RAM */
        plan = xbox_ResumePlanBuild(g_saved.f, g_saved.n, 0, 0x%(kret)08Xu, ORD, THUNK, (int)g_saved.returned,
                                    g_saved.call_esp, &g_saved.regs, lookup, why, sizeof why);
        if (!plan) { printf("FAIL plan: %%s\n", why); return 5; }
        /* Nothing of the first process survives: registers garbage, frames none. */
        g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0xDEADBEEFu;
        g_ebp = g_seh_ebp = 0xDEADBEEFu;
        xbox_ResumeBegin(plan);
        recomp_lookup(((const PlanHead *)plan)->n ? 0x10000u : 0)();   /* frame 0's function, the routine */
        WANT(((const PlanHead *)plan)->done == 1 && xbox_ResumeCompleted() == 1);
        WANT(g_cpu.r_resume == 0 && g_cpu.r_flimit == g_cpu.r_fbase + RECOMP_FRAME_SLOTS);
    } else {
        sub_00010000();
    }
    /* The end of an uninterrupted run. */
    WANT(MEM32(0x2010u) == 0x1077u);                      /* f3: the status plus its local */
    WANT(g_kcalls == 2);                                  /* f1's loop: the call re-entered, then its second turn */
    WANT(g_edi == 0u);
    WANT(MEM32(0x2000u) == 0x11111111u);                  /* f1 kept its scratch ebp */
    WANT(MEM32(0x2008u) == STACK - 8u);                   /* f0 its frame */
    WANT(MEM32(0x200Cu) == 0x5150u);                      /* esi from before the call */
    WANT(g_esp == STACK && g_cpu.r_ftop == g_cpu.r_fbase);
    return bad;
}
'''


def test_a_chain_four_deep_saved_in_one_process_resumes_in_another(tmp_path, monkeypatch):
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc or os.name == "nt":
        pytest.skip("gcc or clang required (POSIX)")
    base = 0x10000
    image, db = _install(monkeypatch, {0: F0, 0x30: F1, 0x60: F2, 0x90: F3})
    tr = FunctionTranslator(image, db)
    bodies = "\n".join(tr.translate_function(a, db[a]) for a in (base, base + 0x30, base + 0x60, base + 0x90))
    assert bodies.count("RECOMP_FRAME_ENTRY_R();") == 4
    assert "goto _rs_0001003F_d0;" in bodies     # f1's call in its loop: through the copy
    table = tr.lifter.call_site_table
    assert KERNEL_RET in {ret for ret, _ in table}
    sites = "\n".join("    0x%08Xu, 0x%08Xu, 0x%02Xu," % (ret, func, flags)
                      for (ret, func), flags in sorted(table.items()))
    src = tmp_path / "resume.c"
    src.write_text(HARNESS % {"bodies": bodies, "sites": sites, "kret": KERNEL_RET})
    exe = tmp_path / "resume"
    for opt in ("-O0", "-O2"):
        r = subprocess.run([cc, "-std=gnu11", opt, "-w", "-I", RUNTIME, "-I", os.path.join(ROOT, "src"),
                            "-I", os.path.join(ROOT, "src", "kernel"), str(src), ENGINE, "-o", str(exe), "-lm",
                            "-Wl,--unresolved-symbols=ignore-all"], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr[-3000:]
        run = subprocess.run([str(exe), "plain"], capture_output=True, text=True, timeout=30)
        assert run.returncode == 0, run.stdout
        for mode in ("save-in", "save-ret"):
            state = tmp_path / ("%s%s.bin" % (mode, opt))
            run = subprocess.run([str(exe), mode, str(state)], capture_output=True, text=True, timeout=30)
            assert run.returncode == 0 and state.exists(), run.stdout
            run = subprocess.run([str(exe), "resume", str(state)], capture_output=True, text=True, timeout=30)
            assert run.returncode == 0, "%s %s: %s\n%s" % (opt, mode, run.stdout, bodies)
