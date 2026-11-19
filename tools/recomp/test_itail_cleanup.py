"""An indirect tail jmp whose target cannot run leaves as the jumper's ret would.

The shape (Fable, WP8 of the profile-free report): a vtable thunk

    mov ecx, [ecx+8] / test ecx, ecx / je L / mov eax, [ecx] / jmp [eax+0x38]
    L: ret 0x18

When the slot is null, RECOMP_ITAIL used to pop the return address only. The
caller had pushed six arguments for a stdcall, so its esp came back 24 bytes
low at every null slot, and the frameless callers above it then popped object
pointers as return addresses: the wild indirect calls that went into the
discovery file. The jumper's own `ret 0x18` says what the target would have
popped, since the target runs on the jumper's frame.

Two halves, on synthetic bytes only: the translator passes the function's ret
amount to the dispatch (RECOMP_ITAIL_RET), and the dispatch, taken verbatim
from the runtime header and compiled, leaves esp where the jumper's ret would
for every kind of target (null, wild, unresolved, resolved).
"""
import os
import re
import subprocess
import sys
import tempfile

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402
from tools.recomp.test_icall_guarded_runtime import install_stub  # noqa: E402
from tools.recomp.stack_fast import FIXTURE_FALLBACK  # XBA-307

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HEADER = os.path.join(ROOT, "templates", "runtime", "recomp_types.h")
BASE = 0x00010000

THUNK = (b"\x8B\x49\x08"        # mov ecx, [ecx+8]
         b"\x85\xC9"            # test ecx, ecx
         b"\x74\x05"            # je +5 (the ret)
         b"\x8B\x01"            # mov eax, [ecx]
         b"\xFF\x60\x38"        # jmp [eax+0x38]
         b"\xC2\x18\x00")       # ret 0x18


def _translate(image, va, size):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="itail-cleanup-test")
    db = {va: {"start": f"0x{va:08X}", "end": va + size, "_addr": va, "size": size}}
    return FunctionTranslator(image, db).translate_function(va, db[va])


def _tail_line(c):
    m = re.search(r"^.*RECOMP_ITAIL.*$", c, re.M)
    assert m, c
    return m.group(0).strip()


def test_thunk_passes_its_ret_amount():
    c = _translate(THUNK, BASE, len(THUNK))
    line = _tail_line(c)
    assert "RECOMP_ITAIL_RET(MEM32(eax + 0x38), 24u);" in line, c
    assert "esp += 28; RECOMP_FRAME_LEAVE(); RECOMP_STK_FENCE(); return; /* ret 24 */" in c, c   # XBA-247: it may park


def test_plain_ret_keeps_the_plain_dispatch():
    image = THUNK[:-3] + b"\xC3"                      # ret
    c = _translate(image, BASE, len(image))
    assert "RECOMP_ITAIL(MEM32(eax + 0x38));" in _tail_line(c), c


def test_disagreeing_rets_keep_the_plain_dispatch():
    # One branch to a `ret 4`, another to a `ret 8`, the fall-through a
    # `jmp [eax+0x38]`: nothing says which one the target would match.
    image = (b"\x85\xC9"            # test ecx, ecx
             b"\x74\x09"            # je +9 (ret 4)
             b"\x85\xD2"            # test edx, edx
             b"\x74\x08"            # je +8 (ret 8)
             b"\x8B\x01"            # mov eax, [ecx]
             b"\xFF\x60\x38"        # jmp [eax+0x38]
             b"\xC2\x04\x00"        # ret 4
             b"\xC2\x08\x00")       # ret 8
    c = _translate(image, BASE, len(image))
    assert "RECOMP_ITAIL(MEM32(eax + 0x38));" in _tail_line(c), c
    assert "RECOMP_ITAIL_RET" not in c, c
    assert "/* ret 4 */" in c and "/* ret 8 */" in c, c


def test_no_ret_keeps_the_plain_dispatch():
    image = b"\x8B\x01\xFF\x60\x38"                   # mov eax, [ecx]; jmp [eax+0x38]
    c = _translate(image, BASE, len(image))
    assert "RECOMP_ITAIL(MEM32(eax + 0x38));" in _tail_line(c), c


def _macro(name):
    lines = open(HEADER, encoding="utf-8").read().splitlines()
    for i, line in enumerate(lines):
        if re.match(r"#define\s+%s\(" % re.escape(name), line):
            out = [line]
            while out[-1].rstrip().endswith("\\"):
                i += 1
                out.append(lines[i])
            return "\n".join(out)
    raise AssertionError(f"{name} not defined in {HEADER}")


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
typedef void (*recomp_func_t)(void);
uint32_t g_xbox_code_lo = 0x00010000u, g_xbox_code_hi = 0x00020000u;
uint32_t g_esp, g_eax, g_seh_ebp, slot;
#define eax g_eax
#define esp g_esp
#define MEM32(a) ((void)(a), slot)
#define RECOMP_ICALL_OBSERVE(va, flags) ((void)0)
#define RECOMP_ABI_CALL(va, fn) (fn)()
#define RECOMP_FRAME_ENTRY() (void)0   /* the frame shadow stack (XBA-247) is not what this checks */
#define RECOMP_FRAME_ENTRY_R() if (0) goto _recomp_resume
static uint32_t recomp_resume_site(uint32_t f) { (void)f; return 0; }
static uint32_t recomp_resume_ebp(void) { return 0; }
#define RECOMP_OPAQUE32(x) (x)
static uint32_t recomp_resume_target(void) { return 0; }
static void recomp_resume_lost(uint32_t f, uint32_t s) { (void)f; (void)s; }
#define RECOMP_FRAME_LEAVE() (void)0
typedef struct recomp_flat_dispatch { void (**table)(void); uint32_t base, span, trace; } recomp_flat_dispatch_t;
recomp_flat_dispatch_t g_recomp_flat;   /* span 0: the slow path */
FLAT_LOOKUP
static int called, fails, not_code;
/* The target the slot names when resolved: a guest `ret 0x18`. */
void sub_T(void) { called++; g_esp += 28; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
recomp_func_t recomp_lookup(uint32_t va) { return va == 0x00011000u ? sub_T : 0; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return 0; }
void recomp_icall_fail_log(uint32_t va) { (void)va; fails++; }
void recomp_icall_not_code_log(uint32_t va) { (void)va; not_code++; }
IS_CODE
ITAIL_RET
static void thunk(void) {
    uint32_t ebp = 0;
    LIFTED
}
static int check(const char *what, uint32_t target, int c, int f, int n) {
    called = fails = not_code = 0;
    g_esp = 0x1000u; slot = target; g_eax = 0x5A5A5A5Au;
    thunk();
    /* The caller pushed 6 arguments and its return address: whatever the
     * target, it gets esp back 28 bytes up, as from the thunk's ret 0x18. */
    if (g_esp != 0x1000u + 28u || called != c || fails != f || not_code != n) {
        printf("FAIL %s: esp=+%d called=%d fails=%d not_code=%d\n",
               what, (int)(g_esp - 0x1000u), called, fails, not_code);
        return 1;
    }
    return 0;
}
int main(void) {
    int bad = 0;
    bad |= check("null slot",  0x00000000u, 0, 0, 1);
    bad |= check("wild",       0xFFDFB889u & 0x7FFFFFFFu, 0, 0, 1);
    bad |= check("unresolved", 0x00011300u, 0, 1, 0);
    bad |= check("resolved",   0x00011000u, 1, 0, 0);
    if (!bad) printf("OK\n");
    return bad;
}
"""


def test_failed_tail_target_pops_what_the_jumpers_ret_pops():
    from tools.recomp.c_fixture import command, compiler
    cc = compiler()
    if not cc:
        pytest.skip("no C compiler on PATH")
    lifted = _tail_line(_translate(THUNK, BASE, len(THUNK)))
    src = (HARNESS.replace("FLAT_LOOKUP", "#define RECOMP_LIKELY(x) (x)\n" + _macro("RECOMP_FLAT_LOOKUP"))
                  .replace("IS_CODE", _macro("RECOMP_ICALL_IS_CODE"))
                  # the dispatch the macros share (XBA-277), after IS_CODE: it names it
                  .replace("ITAIL_RET", _macro("RECOMP_ICALL_HIT_") + "\n" + install_stub() +
                           _macro("RECOMP_ICALL_DISPATCH_") +
                           "\n" + _macro("RECOMP_ITAIL_RET"))
                  .replace("LIFTED", lifted))
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "t.c")
        exe = os.path.join(tmp, "t.exe")
        with open(p, "w") as f:
            f.write(FIXTURE_FALLBACK + src)
        r = subprocess.run(command(cc, [p], exe), cwd=tmp, capture_output=True, text=True)
        assert r.returncode == 0, (r.stdout + r.stderr)[-3000:] + "\n" + src
        r = subprocess.run([exe], capture_output=True, text=True, cwd=tmp)
        assert r.returncode == 0 and r.stdout.startswith("OK"), r.stdout + r.stderr + "\n" + src


if __name__ == "__main__":
    test_thunk_passes_its_ret_amount()
    test_plain_ret_keeps_the_plain_dispatch()
    test_disagreeing_rets_keep_the_plain_dispatch()
    test_no_ret_keeps_the_plain_dispatch()
    test_failed_tail_target_pops_what_the_jumpers_ret_pops()
    print("itail_cleanup: ALL PASS")
