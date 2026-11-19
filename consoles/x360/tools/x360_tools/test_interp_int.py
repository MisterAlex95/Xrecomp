"""Differential test of the hand-written integer half of the Xenon interpreter (XBA-284, x360_interp.c exec) against
lifted code, as test_interp_vmx does for the vector half: random valid words of every integer, branch, CR,
load / store, system register and FPU form the decoder knows, each lifted into its own function (the word, then
blr), and the same word run by the interpreter on the same state. Each word runs from several register states biased
to the edges (0, -1, INT_MIN, carries, shift counts past 31, garbage in the high word, signed zeros, infinities,
NaNs, denormals); the base registers of loads and stores point into a window of random memory. GPRs, FPRs (bits),
LR, CTR, PC, CR, XER, FPSCR, MSR, the reservation and the window must come out the same.

Words the lifter hands to the interpreter (RC_INTERP) cannot be compared: their names must be exactly the expected
set (traps, FPSCR moves, the privileged instructions...), so a new deferral shows up here."""

import os
import random
import shutil
import subprocess
import sys

import pytest

from . import CONSOLE  # noqa: F401
from tools.ppc.isa import A59, A63, D_FORM, X19, X31, X63, XO31
from tools.xex.synth import build_xex
from tools.xex.xex import parse_xex

from .recomp import RUNTIME_DIRS, recompile
from .xenon import MD30, MDS30, X31_64, X63_64, XO31_64, XenonDecoder

TEXT = 0x82001000
BLR = 0x4E800020
HERE = os.path.dirname(os.path.abspath(__file__))
INTERP_C = os.path.normpath(os.path.join(HERE, "..", "..", "runtime", "x360_interp.c"))

# handed to the interpreter by the lifter (by design): not comparable here
DEFERRED = {
    "rfi", "sc", "tw", "twi", "td", "tdi", "mtsr", "mtsrin", "mfsr", "mfsrin", "lswi", "stswi", "lswx", "stswx",
    "eciwx", "ecowx", "mcrfs", "mtfsf", "mtfsb0", "mtfsb1", "mtfsfi", "mffs", "mcrxr", "tlbie", "tlbia", "tlbsync",
}
# deferred words the interpreter does not run either (supervisor instructions; the string forms MSVC's Xenon code
# generator does not emit): a title reaching one stops with the word and its address
NOT_INTERPRETED = {"rfi", "sc", "mtsr", "mtsrin", "mfsr", "mfsrin", "lswi", "stswi", "lswx", "stswx", "eciwx",
                   "ecowx", "tlbie", "tlbia", "tlbsync"}
OE_NAMES = set(XO31.values()) | set(XO31_64.values())
# FP record forms: CR1 from the FPSCR, which only the interpreter keeps
FP_REC = {n + "." for n in list(A59.values()) + list(A63.values()) + list(X63.values()) + list(X63_64.values())
          }

LOADS_U = {"lwzu", "lbzu", "lhzu", "lhau", "lwzux", "lbzux", "lhzux", "lhaux", "ldu", "ldux", "lwaux"}
STORES_U = {"stwu", "stbu", "sthu", "stwux", "stbux", "sthux", "stdu", "stdux", "lfsu", "lfdu", "stfsu", "stfdu",
            "lfsux", "lfdux", "stfsux", "stfdux"}
D_MEM = {"lwz", "lwzu", "lbz", "lbzu", "stw", "stwu", "stb", "stbu", "lhz", "lhzu", "lha", "lhau", "sth", "sthu",
         "lmw", "stmw", "lfs", "lfsu", "lfd", "lfdu", "stfs", "stfsu", "stfd", "stfdu", "ld", "ldu", "lwa", "std",
         "stdu"}
X_MEM = {"lwarx", "lwzx", "lwzux", "lbzx", "lbzux", "stwcx.", "stwx", "stwux", "stbx", "stbux", "lhzx", "lhzux",
         "lhax", "lhaux", "sthx", "sthux", "lwbrx", "stwbrx", "lhbrx", "sthbrx", "lfsx", "lfsux", "lfdx", "lfdux",
         "stfsx", "stfsux", "stfdx", "stfdux", "stfiwx", "dcbz", "dcbz128", "ldx", "ldux", "lwax", "lwaux", "stdx",
         "stdux", "ldarx", "stdcx.", "ldbrx", "stdbrx", "dcbst", "dcbf", "dcbt", "dcbtst", "icbi"}

HARNESS = r"""
#include <stdlib.h>
#include <sys/mman.h>
#include "x360_interp.c"

/* the runtime pieces exec() can reach */
static int failed;
void x360_fail(x360_machine *x, const char *fmt, ...) { (void)x, (void)fmt; failed = 1; }
void x360_note_missed(x360_machine *x, uint32_t pc) { (void)x, (void)pc; }
const x360_thunk *x360_thunk_at(x360_machine *x, uint32_t pc) { (void)x, (void)pc; return NULL; }
x360_thread *x360_cur(x360_machine *x) { (void)x; return NULL; }

extern const xe_rc_entry xe_rc_resume[];
extern const unsigned xe_rc_resume_count;

#define WIN 0x30000000u
#define WLO (WIN - 0x9000u)
#define WSZ 0x13000u
typedef struct { uint32_t entry, word; int base, small, resv; } tcase;
static const tcase cases[] = {
%(cases)s
};
static x360_machine X;
static uint8_t snap[WSZ], after[WSZ];
static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static const uint64_t gedge[] = {0, 1, 2, 31, 32, 63, 64, 0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFull,
    0xFFFFFFFF80000000ull, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, 0x100000000ull,
    0xFFFFFFFF7FFFFFFFull, 0x00000000FFFF8000ull, 0xFFFFFFFFFFFFFFFEull, 0x123456789ABCDEF0ull, 0xFFFF0000ull};
static const uint64_t fedge[] = {0, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
    0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x7FF4000000000000ull, 0x0000000000000001ull,
    0x8008000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x41E0000000000000ull, 0xC1E0000000200000ull, 0x3FE0000000000000ull,
    0x3FF8000000000000ull, 0x4004000000000000ull, 0x43E0000000000000ull, 0x380FFFFFFFFFFFFFull, 0x47EFFFFFE0000000ull,
    0x36A0000000000000ull};
static uint64_t g64(void)
{
    uint64_t r = rnd();
    switch (r & 7) {
    case 0: case 1: case 2: return gedge[(r >> 8) %% (sizeof gedge / 8)];
    case 3: return (uint64_t)(int64_t)(int32_t)(rnd());              /* sign-extended word */
    case 4: return (rnd() & 0xFFFFFFFF00000000ull) | gedge[(r >> 8) %% (sizeof gedge / 8)];
    case 5: return rnd() & 0xFF;
    default: return rnd();
    }
}
static uint64_t f64(void)
{
    uint64_t r = rnd();
    if ((r & 3) == 0)
        return fedge[(r >> 8) %% (sizeof fedge / 8)];
    if ((r & 3) == 1) { /* a single-precision value */
        float f = (float)((int32_t)rnd() >> 8) / 256.0f;
        double d = f;
        uint64_t v;
        memcpy(&v, &d, 8);
        return v;
    }
    if ((r & 3) == 2) { /* an ordinary double */
        double d = (double)(int64_t)rnd() / 4294967296.0;
        uint64_t v;
        memcpy(&v, &d, 8);
        return v;
    }
    return rnd();
}
static xe_cpu init_c;
static int cases_resv;
static uint32_t init_resv_addr;
static uint64_t init_resv_val;
static void init(const tcase *t, unsigned seed)
{
    rs = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xD1B54A32D192ED03ull) ^ t->word;
    for (int k = 0; k < 8; k++)
        rnd();
    xe_cpu *c = &init_c;
    memset(c, 0, sizeof *c);
    cases_resv = t->resv;
    for (int k = 0; k < 32; k++) {
        c->gpr[k] = g64();
        uint64_t f = f64();
        memcpy(&c->fpr[k], &f, 8);
    }
    c->lr = 0x90000000u + ((uint32_t)rnd() & 0xFFFFFC) + (rnd() & 1 ? 0 : 3);
    switch (rnd() & 3) { /* bdnz / bdz test the low word only (32-bit mode) */
    case 0: c->ctr = rnd() & 3; break;
    case 1: c->ctr = (rnd() << 32) | (rnd() & 3); break;
    case 2: c->ctr = g64(); break;
    default: c->ctr = 0x90000000u + ((uint32_t)rnd() & 0xFFFFFC); break;
    }
    c->cr = (uint32_t)rnd();
    c->xer_so = rnd() & 1;
    c->xer_ov = rnd() & 1;
    c->xer_ca = rnd() & 1;
    c->fpscr = (uint32_t)rnd() & 0x00000003u;
    c->msr = (uint32_t)rnd();
    if (t->base >= 0)
        c->gpr[t->base] = (rnd() << 32) | (WIN + ((uint32_t)rnd() & 0xFF));
    if (t->small >= 0)
        c->gpr[t->small] = (rnd() & 1) ? (rnd() & 0x7FF) : (uint64_t)-(int64_t)(rnd() & 0x7FF);
    if (t->resv && (rnd() & 3)) { /* a reservation on the word stwcx. / stdcx. stores to (or near it) */
        init_resv_addr = (uint32_t)(c->gpr[t->base] + (t->small >= 0 ? c->gpr[t->small] : 0)) & ~(uint32_t)(t->resv - 1);
        if (rnd() & 1)
            init_resv_addr += 8;
        init_resv_val = rnd();
        c->reserve = rnd() & 3 ? 1 : 0;
    } else {
        init_resv_addr = 0;
        init_resv_val = 0;
    }
    for (unsigned k = 0; k < WSZ; k += 8) {
        uint64_t v = rnd();
        memcpy(snap + k, &v, 8);
    }
}
static void load(void)
{
    X.m.cpu = init_c;
    X.m.resv_addr = init_resv_addr;
    X.m.resv_val = init_resv_val;
    if (init_resv_addr >= WLO && init_resv_addr + 8 <= WLO + WSZ && (init_resv_val & 1)) { /* the value matches */
        uint64_t v = 0;
        memcpy(&v, snap + (init_resv_addr - WLO), 8);
        X.m.resv_val = __builtin_bswap64(v);
        if (cases_resv == 4)
            X.m.resv_val >>= 32;
    }
    memcpy(X.m.mem + WLO, snap, WSZ);
    X.m.rc_native = 0;
    X.m.slice = 1ll << 40;
    X.m.rc_exit = 0;
    failed = 0;
    static const int modes[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
    fesetround(modes[X.m.cpu.fpscr & 3]);
}
static xe_rc_fn find(uint32_t pc)
{
    unsigned lo = 0, hi = xe_rc_resume_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (xe_rc_resume[mid].entry < pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < xe_rc_resume_count && xe_rc_resume[lo].entry == pc ? xe_rc_resume[lo].fn : NULL;
}
/* 0 ran, 1 the lifter handed the word to the interpreter, 2 no function */
static int lifted(const tcase *t)
{
    X.m.cpu.pc = t->entry;
    for (int n = 0; n < 4 && (X.m.cpu.pc == t->entry || X.m.cpu.pc == t->entry + 4); n++) {
        xe_rc_fn f = find(X.m.cpu.pc);
        if (!f)
            return 2;
        f(&X.m);
        if (X.m.rc_exit == XE_RC_EXIT_INTERP)
            return 1;
        if (X.m.rc_exit)
            return 2;
    }
    return 0;
}
/* 0 ran, 1 not interpreted, -1 failed (trap, sc) */
static int interp(const tcase *t)
{
    xe_cpu *c = &X.m.cpu;
    c->pc = t->entry;
    for (int n = 0; n < 4 && (c->pc == t->entry || c->pc == t->entry + 4); n++) {
        int r = exec(&X, c, c->pc, c->pc == t->entry ? t->word : 0x4E800020u);
        if (r)
            return r;
    }
    return 0;
}
static const char *diff(const xe_cpu *a, const xe_cpu *b, uint32_t ra, uint64_t va, uint32_t w)
{
    static char why[96];
    for (int k = 0; k < 32; k++)
        if (a->gpr[k] != b->gpr[k])
            return snprintf(why, sizeof why, "r%%d %%016llx/%%016llx", k, (unsigned long long)a->gpr[k],
                            (unsigned long long)b->gpr[k]), why;
    for (int k = 0; k < 32; k++)
        if (memcmp(&a->fpr[k], &b->fpr[k], 8)) {
            uint64_t x, y;
            memcpy(&x, &a->fpr[k], 8);
            memcpy(&y, &b->fpr[k], 8);
            return snprintf(why, sizeof why, "f%%d %%016llx/%%016llx", k, (unsigned long long)x,
                            (unsigned long long)y), why;
        }
#define F(f) if (a->f != b->f) return snprintf(why, sizeof why, #f " %%llx/%%llx", (unsigned long long)a->f, \
                                               (unsigned long long)b->f), why
    F(lr); F(ctr); F(pc); F(cr); F(xer_so); F(xer_ov); F(xer_ca); F(fpscr); F(msr); F(reserve);
#undef F
    if (ra != X.m.resv_addr || va != X.m.resv_val)
        return "reservation";
    if (memcmp(after, X.m.mem + WLO, WSZ))
        return "memory";
    if (memcmp(a->vr, b->vr, sizeof a->vr))
        return "vr";
    return NULL;
}
/* given vectors (stdin: case rA rB CA SO, the word reading r4 / r5 into r3): both runs' r3 XER and CR0 */
static void vectors(void)
{
    unsigned k, ca, so;
    unsigned long long a, b;
    while (scanf("%%u %%llx %%llx %%u %%u", &k, &a, &b, &ca, &so) == 5) {
        const tcase *t = &cases[k];
        init(t, 0);
        init_c.gpr[4] = a, init_c.gpr[5] = b, init_c.xer_ca = ca, init_c.xer_so = so, init_c.xer_ov = !so;
        init_c.cr = 0;
        for (int side = 0; side < 2; side++) {
            load();
            int r = side ? interp(t) : lifted(t);
            const xe_cpu *c = &X.m.cpu;
            printf("%%d %%016llx %%u %%u %%u %%x%%s", r, (unsigned long long)c->gpr[3], c->xer_ov, c->xer_so, c->xer_ca,
                   c->cr >> 28, side ? "\n" : " ");
        }
    }
}
int main(int argc, char **argv)
{
    unsigned states = argc > 1 ? (unsigned)atoi(argv[1]) : 16;
    X.m.mem = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (X.m.mem == MAP_FAILED)
        return 99;
    X.reservation = X.m.mem;
    X.m.cur = &X.m.cpu;
    X.m.rt = &X;
    if (states == 0) {
        vectors();
        return 0;
    }
    unsigned n = sizeof cases / sizeof cases[0];
    for (unsigned k = 0; k < n; k++) {
        const tcase *t = &cases[k];
        int skip = 0, bad = 0;
        for (unsigned s = 0; s < states && !skip && !bad; s++) {
            init(t, s);
            load();
            int a = lifted(t);
            if (a == 1) {
                load();
                int b = interp(t);
                printf("%%u deferred %%d\n", k, b);
                skip = 1;
                break;
            }
            if (a) {
                printf("%%u lifted %%d\n", k, a);
                bad = 1;
                break;
            }
            int fa = failed;
            xe_cpu ca = X.m.cpu;
            uint32_t resa = X.m.resv_addr;
            uint64_t resv = X.m.resv_val;
            memcpy(after, X.m.mem + WLO, WSZ);
            load();
            int b = interp(t);
            if (b || failed || fa) {
                printf("%%u interp %%d %%d\n", k, b, failed);
                bad = 1;
                break;
            }
            const char *why = diff(&ca, &X.m.cpu, resa, resv, t->word);
            if (why) {
                printf("%%u state %%u %%s\n", k, s, why);
                bad = 1;
            }
        }
        if (!skip && !bad)
            printf("%%u ok\n", k);
    }
    return 0;
}
"""


def templates():
    """[(name, op, fixed bits, fixed mask)]: every form the decoder knows outside VMX."""
    t = []
    for op, name in D_FORM.items():
        t.append((name, op, 0, 0))
    t += [("bc", 16, 0, 0), ("b", 18, 0, 0), ("sc", 17, 2, 2), ("tdi", 2, 0, 0)]
    for xo, name in X19.items():
        t.append((name, 19, xo << 1, 0x7FE))
    for xo, name in list(X31.items()) + list(X31_64.items()):
        t.append((name, 31, xo << 1, 0x7FE))
    t.append(("dcbz128", 31, (1 << 21) | (1014 << 1), (31 << 21) | 0x7FE))
    t.append(("sradi", 31, 413 << 2, 0x7FC))
    for xo, name in list(XO31.items()) + list(XO31_64.items()):
        t.append((name, 31, xo << 1, 0x3FE))
    for xo, name in A59.items():
        t.append((name, 59, xo << 1, 0x3E))
    for xo, name in A63.items():
        t.append((name, 63, xo << 1, 0x3E))
    for xo, name in list(X63.items()) + list(X63_64.items()):
        t.append((name, 63, xo << 1, 0x7FE))
    for k, name in MD30.items():
        t.append((name, 30, k << 2, 0x1C))
    for k, name in MDS30.items():
        t.append((name, 30, k << 1, 0x1E))
    for k, name in enumerate(("ld", "ldu", "lwa")):
        t.append((name, 58, k, 3))
    for k, name in enumerate(("std", "stdu")):
        t.append((name, 62, k, 3))
    return t


def valid(i):
    """Words whose result the architecture defines (invalid forms may legitimately differ)."""
    n = i.name
    if n in LOADS_U and (i.ra == 0 or i.ra == i.rd):
        return False
    if n in STORES_U and i.ra == 0:
        return False
    if n == "lmw" and (i.ra >= i.rd or i.ra == 0):
        return False
    if n in ("stwcx.", "stdcx.") and not i.rec:
        return False
    return True


def steer(i):
    """(base register, small register) of a load / store: the base points into the harness's window."""
    n = i.name
    if n in D_MEM:
        return (i.ra if i.ra else -1), -1
    if n in X_MEM:
        if i.ra == 0:
            return i.rb, -1
        if i.ra == i.rb:
            return i.ra, -1
        return i.ra, i.rb
    return -1, -1


def samples(rng, per_name=8):
    dec = XenonDecoder()
    out = []
    for name, op, fixed, fmask in templates():
        got, want = 0, (32 if name in ("bc", "bclr", "bcctr") else per_name)
        for _ in range(4000):
            w = (op << 26) | fixed | (rng.getrandbits(26) & ~fmask & 0x03FFFFFF)
            i = dec.decode(w, TEXT)
            if i.name != name or not valid(i):
                continue
            out.append((name, w))
            got += 1
            if got == want:
                break
        assert got, name
    return out


def place_branches(rng, cases):
    """Keeps branch targets out of the image (or on the case's own blr): no discovery of the other cases' words,
    and no branch to the word itself (a loop)."""
    dec = XenonDecoder()
    end = TEXT + 8 * len(cases)
    for k, (name, w) in enumerate(cases):
        if name not in ("b", "bc"):
            continue
        a = TEXT + 8 * k
        for _ in range(1000):
            i = dec.decode(w, a)
            t = i.target
            if t == a + 4 or not (TEXT - 0x100 <= t < end + 0x100):
                break
            w = (w & ~0x03FFFFFC) | (rng.getrandbits(24) << 2) if name == "b" else (w & ~0xFFFC) | (
                rng.getrandbits(14) << 2)
        cases[k] = (name, w)
    return cases


def build(tmp_path, cases):
    words, funcs = [], []
    for _, w in cases:
        funcs.append((4 * len(words), 8))
        words += [w, BLR]
    data, _ = build_xex(words, functions=funcs, compression="none")
    src = tmp_path / "src"
    s = recompile(parse_xex(data), str(src))
    dec = XenonDecoder()
    rows = []
    for k, ((off, _), (_, w)) in enumerate(zip(funcs, cases)):
        base, small = steer(dec.decode(w, TEXT + off))
        resv = {"stwcx.": 4, "stdcx.": 8}.get(cases[k][0], 0)
        rows.append("    {0x%08Xu, 0x%08Xu, %d, %d, %d}," % (TEXT + off, w, base, small, resv))
    (tmp_path / "h.c").write_text(HARNESS % {"cases": "\n".join(rows)})
    inc = []
    for d in RUNTIME_DIRS:
        inc += ["-I", d]
    exe = str(tmp_path / "t")
    subprocess.run(["cc", "-std=gnu11", "-O1", "-w", "-ffp-contract=off", "-DXE_RUNTIME=1", "-D_GNU_SOURCE"] + inc +
                   ["-o", exe, str(tmp_path / "h.c")] + sorted(str(p) for p in src.glob("x360_rc_*.c")) + ["-lm"],
                   check=True)
    return exe, s


def key(name, w):
    """The form a word stands for: OE and FP record forms are deferred on their own."""
    op = w >> 26
    if op == 31 and name in OE_NAMES and (w >> 10) & 1:
        name += "o"
    if op in (59, 63) and w & 1:
        name += "."
    return name


def run(exe, cases, states=16):
    out = subprocess.run([exe, str(states)], check=True, capture_output=True, text=True).stdout.splitlines()
    deferred, bad = {}, []
    for line in out:
        k, kind, *rest = line.split(" ", 2)
        name, w = cases[int(k)]
        name = key(name, w)
        if kind == "deferred":
            deferred.setdefault(name, set()).add(rest[0])
        elif kind != "ok":
            bad.append("%s %08X: %s %s" % (name, w, kind, " ".join(rest)))
    assert len(out) == len(cases)
    return deferred, bad


@pytest.mark.skipif(not shutil.which("cc") or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_integer_interpreter_matches_lifted_code(tmp_path):
    rng = random.Random(284)
    cases = place_branches(rng, samples(rng))
    exe, _ = build(tmp_path, cases)
    deferred, bad = run(exe, cases)
    assert not bad, bad[:30]
    want = DEFERRED | FP_REC
    assert set(deferred) == want, sorted(set(deferred) ^ want)
    # the deferred words the interpreter must run (r = 0 ran, -1 trapped / failed on purpose, 1 unknown)
    unknown = sorted(n for n, rs in deferred.items() if "1" in rs and n not in NOT_INTERPRETED)
    assert not unknown, unknown


# OE vectors worked out by hand (not by the formulas the lifter and the interpreter use): (mnemonic, rA, rB, CA in,
# SO in) -> (low word of rD, OV, SO, CA out or None when the form leaves it). Words: mnemonico. r3, r4, r5.
OE_XO = {"add": 266, "addc": 10, "adde": 138, "addze": 202, "addme": 234, "subf": 40, "subfc": 8, "subfe": 136,
         "subfze": 200, "subfme": 232, "neg": 104, "mullw": 235, "divw": 491, "divwu": 459, "mulld": 233,
         "divd": 489, "divdu": 457}
OE_VECTORS = [
    ("add", 0x7FFFFFFF, 1, 0, 0, 0x80000000, 1, 1, None),
    ("add", 0x00000001, 1, 0, 0, 0x00000002, 0, 0, None),
    ("add", 0x00000001, 1, 0, 1, 0x00000002, 0, 1, None),          # SO sticks
    ("add", 0x80000000, 0x80000000, 0, 0, 0x00000000, 1, 1, None),
    ("add", 0xFFFFFFFF7FFFFFFF, 0x0000000100000001, 0, 0, 0x80000000, 1, 1, None),  # the low word decides
    ("add", 0x000000007FFFFFFF, 0xFFFFFFFF00000000, 0, 0, 0x7FFFFFFF, 0, 0, None),
    ("addc", 0xFFFFFFFF, 1, 0, 0, 0x00000000, 0, 0, 1),
    ("addc", 0x80000000, 0xFFFFFFFF, 0, 0, 0x7FFFFFFF, 1, 1, 1),
    ("adde", 0x7FFFFFFF, 0, 1, 0, 0x80000000, 1, 1, 0),
    ("adde", 0x7FFFFFFE, 0, 1, 0, 0x7FFFFFFF, 0, 0, 0),
    ("addze", 0x7FFFFFFF, 0, 1, 0, 0x80000000, 1, 1, 0),
    ("addze", 0xFFFFFFFF, 0, 1, 0, 0x00000000, 0, 0, 1),
    ("addme", 0x80000000, 0, 0, 0, 0x7FFFFFFF, 1, 1, 1),
    ("addme", 0x80000000, 0, 1, 0, 0x80000000, 0, 0, 1),
    ("subf", 1, 0x80000000, 0, 0, 0x7FFFFFFF, 1, 1, None),           # rB - rA
    ("subf", 0xFFFFFFFF, 0x7FFFFFFF, 0, 0, 0x80000000, 1, 1, None),
    ("subf", 1, 2, 0, 0, 0x00000001, 0, 0, None),
    ("subfc", 0x80000000, 0, 0, 0, 0x80000000, 1, 1, 0),
    ("subfc", 0, 0, 0, 0, 0x00000000, 0, 0, 1),
    ("subfe", 0, 0x80000000, 0, 0, 0x7FFFFFFF, 1, 1, 1),            # ~0 + 0x80000000 + 0
    ("subfe", 0, 0x80000000, 1, 0, 0x80000000, 0, 0, 1),
    ("subfze", 0x80000000, 0, 1, 0, 0x80000000, 1, 1, 0),           # ~0x80000000 + 1
    ("subfze", 0x80000000, 0, 0, 0, 0x7FFFFFFF, 0, 0, 0),
    ("subfme", 0x7FFFFFFF, 0, 0, 0, 0x7FFFFFFF, 1, 1, 1),           # 0x80000000 - 1
    ("subfme", 0x7FFFFFFF, 0, 1, 0, 0x80000000, 0, 0, 1),
    ("neg", 0x80000000, 0, 0, 0, 0x80000000, 1, 1, None),
    ("neg", 0xFFFFFFFF00000001, 0, 0, 0, 0xFFFFFFFF, 0, 0, None),
    ("neg", 0x0000000180000000, 0, 0, 0, 0x80000000, 1, 1, None),
    ("mullw", 0x10000, 0x8000, 0, 0, 0x80000000, 1, 1, None),
    ("mullw", 0x10000, 0x7FFF, 0, 0, 0x7FFF0000, 0, 0, None),
    ("mullw", 0xFFFFFFFF, 0x80000000, 0, 0, 0x80000000, 1, 1, None),  # -1 * INT_MIN
    ("mullw", 0xFFFFFFFF, 0x7FFFFFFF, 0, 0, 0x80000001, 0, 0, None),
    ("mullw", 0x100000002, 0x300000003, 0, 0, 0x00000006, 0, 0, None),  # the low words only
    ("divw", 7, 0, 0, 0, None, 1, 1, None),
    ("divw", 0x80000000, 0xFFFFFFFF, 0, 0, None, 1, 1, None),
    ("divw", 0x80000000, 0xFFFFFFFE, 0, 0, 0x40000000, 0, 0, None),
    ("divw", 0xFFFFFFFF80000000, 0x00000000FFFFFFFF, 0, 0, None, 1, 1, None),
    ("divwu", 7, 0, 0, 0, None, 1, 1, None),
    ("divwu", 0x80000000, 0xFFFFFFFF, 0, 0, 0x00000000, 0, 0, None),
    ("divwu", 7, 0x100000000, 0, 0, None, 1, 1, None),                # low word of rB is zero
    ("mulld", 0x100000000, 0x100000000, 0, 0, 0x00000000, 1, 1, None),
    ("mulld", 0x100000000, 0x7FFFFFFF, 0, 0, 0x00000000, 0, 0, None),
    ("mulld", 0xFFFFFFFFFFFFFFFF, 0x8000000000000000, 0, 0, 0x00000000, 1, 1, None),
    ("mulld", 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0, 0, 0x00000001, 0, 0, None),
    ("divd", 7, 0, 0, 0, None, 1, 1, None),
    ("divd", 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0, 0, None, 1, 1, None),
    ("divd", 0x80000000, 0xFFFFFFFFFFFFFFFF, 0, 0, 0x80000000, 0, 0, None),  # only 64-bit INT_MIN overflows
    ("divdu", 7, 0, 0, 0, None, 1, 1, None),
    ("divdu", 7, 0x100000000, 0, 0, 0x00000000, 0, 0, None),
]


@pytest.mark.skipif(not shutil.which("cc") or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_oe_forms_set_ov_and_so(tmp_path):
    names = sorted(OE_XO)
    cases = [(n, (31 << 26) | (3 << 21) | (4 << 16) | (5 << 11) | (1 << 10) | (OE_XO[n] << 1) | 1) for n in names]
    exe, s = build(tmp_path, cases)
    assert s["interp_fallback_insns"] == 0, s["unlifted"]
    rows = "".join("%d %x %x %d %d\n" % (names.index(v[0]), v[1], v[2], v[3], v[4]) for v in OE_VECTORS)
    out = subprocess.run([exe, "0"], input=rows, check=True, capture_output=True, text=True).stdout.splitlines()
    assert len(out) == len(OE_VECTORS)
    bad = []
    for v, line in zip(OE_VECTORS, out):
        f = line.split()
        lifted, interp = f[:6], f[6:]
        name, a, b, ca, so, rd, ov, so2, ca2 = v
        if lifted != interp:
            bad.append("%s %x %x: lifted %s, interpreted %s" % (name, a, b, lifted, interp))
            continue
        r, got, gov, gso, gca, cr0 = int(f[0]), int(f[1], 16), int(f[2]), int(f[3]), int(f[4]), int(f[5], 16)
        want_lo = got & 0xFFFFFFFF if rd is None else rd  # a divide by zero leaves rD undefined
        want_cr0 = (8 if want_lo & 0x80000000 else 4 if want_lo and want_lo < 0x80000000 else 2) | so2
        if r or (got & 0xFFFFFFFF) != want_lo or gov != ov or gso != so2 or (ca2 is not None and gca != ca2) or \
                (rd is not None and cr0 != want_cr0):
            bad.append("%s %x %x: got %s" % (name, a, b, " ".join(lifted)))
    assert not bad, bad


@pytest.mark.skipif(not shutil.which("cc") or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_fused_family_interpreter_matches_lifted_code(tmp_path):
    """XBA-292: many states per word, so that frA, frB and frC are NaNs together (0xFE85127A: the word that showed it)."""
    rng = random.Random(292)
    dec = XenonDecoder()
    words = [0xFE85127A]
    for op, xo in ((63, 28), (63, 29), (63, 30), (63, 31), (59, 28), (59, 29), (59, 30), (59, 31)):
        words += [(op << 26) | (rng.getrandbits(20) << 6) | (xo << 1) for _ in range(12)]
    cases = [(dec.decode(w, TEXT).name, w) for w in words]
    exe, s = build(tmp_path, cases)
    assert s["interp_fallback_insns"] == 0, s["unlifted"]
    _, bad = run(exe, cases, states=512)
    assert not bad, bad[:30]


# XBA-292: the fused family's results, worked out here from the architecture's rule and run through ppc_ops.h: with a
# NaN operand, frA's, else frB's, else frC's, quieted, sign kept whatever the form negates.
FMA_A, FMA_B, FMA_C, FMA_ONE = 0x7FF8000000000A0A, 0xFFF80000000000B0, 0x7FF4000000000C0C, 0x3FF8000000000000
QUIET = 0x0008000000000000
FMA_FORMS = ("ppc_fmadd", "ppc_fmsub", "ppc_fnmadd", "ppc_fnmsub")
TWO, THREE, MINUS3 = 0x4000000000000000, 0x4008000000000000, 0xC008000000000000
# exact zeros: the sign of the fused result, then its negation as a sign flip (AArch64 fmsub / fnmadd folding lost it)
FMA_ZEROS = [("ppc_fmadd", TWO, FMA_ONE, MINUS3, 0), ("ppc_fmsub", TWO, FMA_ONE, THREE, 0),
             ("ppc_fnmadd", TWO, FMA_ONE, MINUS3, 1 << 63), ("ppc_fnmsub", TWO, FMA_ONE, THREE, 1 << 63)]


def fma_vectors():
    out = list(FMA_ZEROS)
    for form in FMA_FORMS:
        for mask in range(1, 8):
            a, b, c = (FMA_A if mask & 1 else FMA_ONE), (FMA_B if mask & 2 else FMA_ONE), (FMA_C if mask & 4 else FMA_ONE)
            first = a if mask & 1 else b if mask & 2 else c
            out.append((form, a, c, b, first | QUIET))
    return out


@pytest.mark.skipif(not shutil.which("cc") or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_fused_family_architected_results(tmp_path, opt):
    vectors = fma_vectors()
    rows = "".join("    {%s, 0x%016XULL, 0x%016XULL, 0x%016XULL, 0x%016XULL},\n" % v for v in vectors)
    src = tmp_path / "fma.c"
    src.write_text("""#include <stdio.h>
#include "ppc_ops.h"
static const struct { double (*f)(double, double, double); uint64_t a, c, b, want; } v[] = {
%s};
int main(void)
{
    int bad = 0;
    for (unsigned k = 0; k < sizeof v / sizeof v[0]; k++) {
        uint64_t got = ppc_f2u(v[k].f(ppc_u2f(v[k].a), ppc_u2f(v[k].c), ppc_u2f(v[k].b)));
        if (got != v[k].want)
            printf("%%u: %%016llx, want %%016llx\\n", k, (unsigned long long)got, (unsigned long long)v[k].want), bad = 1;
    }
    return bad;
}
""" % rows)
    exe = str(tmp_path / "fma")
    subprocess.run(["cc", opt, "-std=gnu11", "-w"] + ["-I" + d for d in RUNTIME_DIRS] + [str(src), "-o", exe, "-lm"],
                   check=True)
    r = subprocess.run([exe], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout
