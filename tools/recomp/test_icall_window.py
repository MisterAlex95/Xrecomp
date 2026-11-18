"""The indirect-call window (XBA-277) changes no call's outcome.

recomp_dispatch_init publishes only the part of the flat table inside the
code range, and the macros call a hit at once, before the trace note,
RECOMP_ICALL_IS_CODE and the lookups. This runs every indirect-call macro
(RECOMP_ICALL, _SAFE, _SAFE_CC, _SAFE_AT, _SAFE_AT_CC, RECOMP_ITAIL_RET) on
targets of every kind -- translated inside and outside the code range, with
and without a manual override, an override alone, unresolved, below the
table, kernel thunks resolved or not, wild -- under each state the runtime
can be in (no table, range not set, the range covering all or part of the
table or none of it, trace on or off, feedback build or not), once with the
macros as they are and once with the macros before the window (frozen
below, with the whole table published as recomp_dispatch_init did then),
and compares what happened: the function called, esp, eax, the logs, the
records and the trace ring.

Skips itself if no C compiler is on PATH.
"""
import os
import re
import subprocess
import sys
import tempfile

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.translator import BatchTranslator  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HEADER = os.path.join(ROOT, "templates", "runtime", "recomp_types.h")

TRANSLATIONS = [
    (0x00011000, "f_a", None),
    (0x00011004, "f_b", None),      # has a manual override (f_m)
    (0x00012000, "f_c", None),
    (0x000203FC, "f_d", None),
]

# recomp_types.h before XBA-277: the flat table published whole, every call
# through the trace note and RECOMP_ICALL_IS_CODE first.
OLD_MACROS = r"""
#define RECOMP_ICALL_IS_CODE(_va) \
    ((_va) >= 0xFE000000u || g_xbox_code_hi == 0u || \
     ((_va) >= g_xbox_code_lo && (_va) < g_xbox_code_hi))
#define RECOMP_FLAT_LOOKUP(_va) \
    (((uint32_t)(_va) - g_recomp_flat.base) < g_recomp_flat.span \
        ? g_recomp_flat.table[(uint32_t)(_va) - g_recomp_flat.base] : (void (*)(void))0)
#define RECOMP_ICALL_TRACE_NOTE(_va) do { \
    if (RECOMP_UNLIKELY(g_recomp_flat.trace)) { \
        g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = (_va); \
        g_icall_trace_idx++; \
        g_icall_count++; \
    } \
} while (0)
#define RECOMP_ICALL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    RECOMP_ICALL_TRACE_NOTE(_va); \
    /* Skip garbage VAs outside code section + kernel thunk range */ \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp += 4; eax = 0; break; \
    } \
    recomp_func_t _fn = RECOMP_FLAT_LOOKUP(_va); \
    if (!_fn) _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; eax = 0; } \
} while(0)
#define RECOMP_ICALL_SAFE(xbox_va, saved_esp) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    RECOMP_ICALL_TRACE_NOTE(_va); \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp = (saved_esp); eax = 0; break; \
    } \
    recomp_func_t _fn = RECOMP_FLAT_LOOKUP(_va); \
    if (!_fn) _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp = (saved_esp); eax = 0; } \
} while(0)
#define RECOMP_ICALL_SAFE_CC(xbox_va, saved_esp) do { \
    (void)(saved_esp); \
    uint32_t _va = (uint32_t)(xbox_va); \
    RECOMP_ICALL_TRACE_NOTE(_va); \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp += 4; eax = 0; break; \
    } \
    recomp_func_t _fn = RECOMP_FLAT_LOOKUP(_va); \
    if (!_fn) _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; eax = 0; } \
} while(0)
#define RECOMP_ICALL_SAFE_AT(xbox_va, saved_esp, site) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    RECOMP_ICALL_TRACE_NOTE(_va); \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp = (saved_esp); eax = 0; break; \
    } \
    RECOMP_ICALL_OBSERVE_SITE((site), _va); \
    recomp_func_t _fn = RECOMP_FLAT_LOOKUP(_va); \
    if (!_fn) _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp = (saved_esp); eax = 0; } \
} while(0)
#define RECOMP_ICALL_SAFE_AT_CC(xbox_va, saved_esp, site) do { \
    (void)(saved_esp); \
    uint32_t _va = (uint32_t)(xbox_va); \
    RECOMP_ICALL_TRACE_NOTE(_va); \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp += 4; eax = 0; break; \
    } \
    RECOMP_ICALL_OBSERVE_SITE((site), _va); \
    recomp_func_t _fn = RECOMP_FLAT_LOOKUP(_va); \
    if (!_fn) _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; eax = 0; } \
} while(0)
#define RECOMP_ITAIL_RET(xbox_va, ret_imm) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp += 4u + (uint32_t)(ret_imm); g_eax = 0; break; \
    } \
    recomp_func_t _fn = RECOMP_FLAT_LOOKUP(_va); \
    if (!_fn) _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); \
           g_esp += 4u + (uint32_t)(ret_imm); g_eax = 0; } \
} while(0)
"""


def _macro(name, nth=0):
    """The nth #define of name in the runtime header, continuation lines included."""
    lines = open(HEADER, encoding="utf-8").read().splitlines()
    for i, line in enumerate(lines):
        if re.match(r"#define\s+%s\(" % re.escape(name), line):
            if nth:
                nth -= 1
                continue
            out = [line]
            while out[-1].rstrip().endswith("\\"):
                i += 1
                out.append(lines[i])
            return "\n".join(out)
    raise AssertionError(f"{name} not defined in {HEADER}")


def _new_macros(feedback):
    # RECOMP_ICALL_HIT_ has two definitions: without and with RECOMP_ICALL_FEEDBACK.
    hit = _macro("RECOMP_ICALL_HIT_", 1 if feedback else 0)
    names = ("RECOMP_LIKELY", "RECOMP_ICALL_IS_CODE", "RECOMP_FLAT_LOOKUP", "RECOMP_ICALL_TRACE_NOTE",
             "RECOMP_ICALL_DISPATCH_", "RECOMP_ICALL", "RECOMP_ICALL_SAFE",
             "RECOMP_ICALL_SAFE_CC", "RECOMP_ICALL_SAFE_AT", "RECOMP_ICALL_SAFE_AT_CC", "RECOMP_ITAIL_RET")
    return hit + "\n" + "\n".join(_macro(n) for n in names)


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void (*recomp_func_t)(void);
#define ICALL_TRACE_SIZE 16
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_trace_idx;
volatile uint64_t g_icall_count;
uint32_t g_xbox_code_lo, g_xbox_code_hi;
uint32_t g_esp, g_eax;
#define eax g_eax
#define RECOMP_UNLIKELY(x) (x)
#define RECOMP_ABI_CALL(va, fn) (fn)()
typedef struct recomp_flat_dispatch { void (**table)(void); uint32_t base, span, trace; } recomp_flat_dispatch_t;
extern recomp_flat_dispatch_t g_recomp_flat;
recomp_func_t recomp_lookup(uint32_t);
int recomp_dispatch_init(void);

static char logbuf[4096];
static void note(const char *fmt, uint32_t v) {
    size_t n = strlen(logbuf);
    snprintf(logbuf + n, sizeof logbuf - n, fmt, v);
}
#ifdef RECOMP_ICALL_FEEDBACK
#define RECOMP_ICALL_SEEN_RESOLVED 1u
#define RECOMP_ICALL_SEEN_UNRESOLVED 2u
#define RECOMP_ICALL_OBSERVE(va, flags) (note(" obs%X", (va)), note("/%u", (flags)))
#define RECOMP_ICALL_OBSERVE_SITE(site, va) (note(" site%X", (site)), note("/%X", (va)))
#else
#define RECOMP_ICALL_OBSERVE(va, flags) ((void)0)
#define RECOMP_ICALL_OBSERVE_SITE(site, va) ((void)0)
#endif
#define F(n) void f_##n(void) { note(" call_" #n, 0); g_esp += 4; }
F(a) F(b) F(c) F(d) F(m) F(n) F(k)
recomp_func_t recomp_lookup_manual(uint32_t va) {
    return va == 0x00011004u ? f_m : va == 0x00011008u ? f_n : (recomp_func_t)0;
}
recomp_func_t recomp_lookup_kernel(uint32_t va) { return va == 0xFE000010u ? f_k : (recomp_func_t)0; }
void recomp_icall_fail_log(uint32_t va) { note(" fail%X", va); }
void recomp_icall_not_code_log(uint32_t va) { note(" notcode%X", va); }

MACROS

#define SITE(name, stmt) static void s_##name(uint32_t va) { stmt; }
SITE(icall, RECOMP_ICALL(va))
SITE(safe, RECOMP_ICALL_SAFE(va, 0x1008u))
SITE(safe_cc, RECOMP_ICALL_SAFE_CC(va, 0x1008u))
SITE(safe_at, RECOMP_ICALL_SAFE_AT(va, 0x1008u, 0x00010040u))
SITE(safe_at_cc, RECOMP_ICALL_SAFE_AT_CC(va, 0x1008u, 0x00010040u))
SITE(itail, RECOMP_ITAIL_RET(va, 8u))

int main(int argc, char **argv) {
    static const uint32_t vas[] = { 0x00011000u, 0x00011004u, 0x00012000u, 0x000203FCu,
        0x00011008u, 0x00011001u, 0x00000000u, 0x00010FFFu, 0x00012001u, 0x000203FDu,
        0x00020400u, 0xFE000010u, 0xFE000020u, 0xFDFFFFFFu, 0xFFFFFFFFu };
    static void (*const sites[])(uint32_t) = { s_icall, s_safe, s_safe_cc, s_safe_at, s_safe_at_cc, s_itail };
    size_t i, j;
    if (argc != 5) return 2;
    g_xbox_code_lo = (uint32_t)strtoul(argv[1], 0, 0);
    g_xbox_code_hi = (uint32_t)strtoul(argv[2], 0, 0);
    if (atoi(argv[3])) setenv("RECOMP_ICALL_TRACE", "1", 1);
    if (atoi(argv[4])) {
        recomp_dispatch_init();
#ifdef OLD
        {   /* as recomp_dispatch_init published it before XBA-277: the whole table */
            static recomp_func_t full[0x203FD - 0x11000];
            full[0x00000] = f_a; full[0x00004] = f_m; full[0x01000] = f_c; full[0x0F3FC] = f_d;
            g_recomp_flat.table = full; g_recomp_flat.base = 0x00011000u;
            g_recomp_flat.span = 0x203FDu - 0x11000u;
        }
#endif
    }
    for (j = 0; j < sizeof sites / sizeof sites[0]; j++)
        for (i = 0; i < sizeof vas / sizeof vas[0]; i++) {
            logbuf[0] = 0;
            g_esp = 0x1000u - 4u;       /* the return address pushed */
            g_eax = 0x5A5A5A5Au;
            sites[j](vas[i]);
            printf("%u %08X:%s esp=%X eax=%X trace=%u/%u\n", (unsigned)j, vas[i], logbuf, g_esp, g_eax,
                   (unsigned)g_icall_trace_idx, (unsigned)g_icall_count);
        }
    for (i = 0; i < ICALL_TRACE_SIZE; i++) printf("%X ", g_icall_trace[i]);
    printf("\n");
    return 0;
}
"""

STATES = [
    # code_lo, code_hi, trace, init
    (0x00010000, 0x00030000, 0, 1),     # the range covers the table
    (0x00011004, 0x00012001, 0, 1),     # part of it: f_a and f_d outside
    (0x00011001, 0x00011004, 0, 1),     # inside the table, no translated function
    (0x00040000, 0x00050000, 0, 1),     # none of it
    (0x00000000, 0x00000000, 0, 1),     # range not set: everything allowed
    (0x00010000, 0x00030000, 1, 1),     # trace on
    (0x00011004, 0x00012001, 1, 1),
    (0x00010000, 0x00030000, 0, 0),     # table not built
    (0x00000000, 0x00000000, 1, 0),
]


def _dispatch(tmp):
    with open(os.path.join(tmp, "recomp_funcs.h"), "w") as f:
        f.write("#include <stdint.h>\n#include <stddef.h>\n"
                "void f_a(void); void f_b(void); void f_c(void); void f_d(void);\n")
    disp = os.path.join(tmp, "recomp_dispatch.c")
    BatchTranslator._write_dispatch_table(
        object.__new__(BatchTranslator), TRANSLATIONS, disp, "recomp_funcs.h")
    return disp


def _env():
    return {k: v for k, v in os.environ.items() if k != "RECOMP_ICALL_TRACE"}


@pytest.mark.parametrize("feedback", [False, True])
def test_window_changes_no_outcome(feedback):
    from tools.recomp.c_fixture import command, compiler
    cc = compiler()
    if not cc:
        pytest.skip("no C compiler on PATH")
    with tempfile.TemporaryDirectory() as tmp:
        disp = _dispatch(tmp)
        out = {}
        for side, macros in (("old", OLD_MACROS), ("new", _new_macros(feedback))):
            src = os.path.join(tmp, side + ".c")
            with open(src, "w") as f:
                f.write(HARNESS.replace("MACROS", macros))
            exe = os.path.join(tmp, side + ".exe")
            defines = (["OLD"] if side == "old" else []) + (["RECOMP_ICALL_FEEDBACK"] if feedback else [])
            r = subprocess.run(command(cc, [src, disp], exe, include_dirs=[tmp], defines=defines),
                               cwd=tmp, capture_output=True, text=True)
            assert r.returncode == 0, (r.stdout + r.stderr)[-3000:]
            runs = []
            for lo, hi, trace, init in STATES:
                r = subprocess.run([exe, hex(lo), hex(hi), str(trace), str(init)],
                                   capture_output=True, text=True, env=_env())
                assert r.returncode == 0, r.stdout + r.stderr
                runs.append(r.stdout)
            out[side] = runs
        for state, old, new in zip(STATES, out["old"], out["new"]):
            assert old == new, "state %r:\n--- old\n%s\n--- new\n%s" % (state, old, new)
        # The cases are live: calls resolved, refused and failed all happen.
        joined = "".join(out["new"])
        for what in ("call_a", "call_m", "call_n", "call_k", "call_d", "notcode", "fail", "trace=1/1"):
            assert what in joined, what


WINDOW = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef void (*recomp_func_t)(void);
typedef struct recomp_flat_dispatch { void (**table)(void); uint32_t base, span, trace; } recomp_flat_dispatch_t;
extern recomp_flat_dispatch_t g_recomp_flat;
int recomp_dispatch_init(void);
uint32_t g_xbox_code_lo, g_xbox_code_hi;
void f_a(void) {} void f_b(void) {} void f_c(void) {} void f_d(void) {}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
static int window(uint32_t lo, uint32_t hi, uint32_t base, uint32_t span, recomp_func_t first) {
    g_xbox_code_lo = lo; g_xbox_code_hi = hi;
    recomp_dispatch_init();
    if (g_recomp_flat.span != span ||
            (span && (g_recomp_flat.base != base || g_recomp_flat.table[0] != first))) {
        printf("FAIL [%X,%X): base %X span %X\n", lo, hi, g_recomp_flat.base, g_recomp_flat.span);
        return 1;
    }
    return 0;
}
int main(void) {
    int bad = 0;
    bad |= window(0, 0, 0, 0, 0);                                  /* range not set */
    bad |= window(0x10000, 0x30000, 0x11000, 0xF3FD, f_a);         /* all of it */
    bad |= window(0x12000, 0x20000, 0x12000, 0xE000, f_c);         /* moved: computed anew */
    bad |= window(0x11004, 0x203FC, 0x11004, 0xF3F8, f_b);         /* f_d's slot just outside */
    bad |= window(0x30000, 0x40000, 0, 0, 0);                      /* none of it */
    setenv("RECOMP_ICALL_TRACE", "1", 1);
    bad |= window(0x10000, 0x30000, 0, 0, 0);                      /* trace on */
    if (!bad) printf("OK\n");
    return bad;
}
"""


def test_window_is_the_code_range_part_of_the_table():
    """The window recomp_dispatch_init publishes, state by state."""
    from tools.recomp.c_fixture import command, compiler
    cc = compiler()
    if not cc:
        pytest.skip("no C compiler on PATH")
    with tempfile.TemporaryDirectory() as tmp:
        disp = _dispatch(tmp)
        src = os.path.join(tmp, "h.c")
        with open(src, "w") as f:
            f.write(WINDOW)
        exe = os.path.join(tmp, "h.exe")
        r = subprocess.run(command(cc, [src, disp], exe, include_dirs=[tmp]), cwd=tmp,
                           capture_output=True, text=True)
        assert r.returncode == 0, (r.stdout + r.stderr)[-3000:]
        r = subprocess.run([exe], capture_output=True, text=True, env=_env())
        assert r.returncode == 0 and r.stdout.startswith("OK"), r.stdout + r.stderr
