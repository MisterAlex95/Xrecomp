"""Dead CR stores (xenon.py XenonLifter._plan_cr): the guest state must be the same, byte for byte, wherever it can be
seen. Random functions of compares, record forms (OE ones too), CR readers (mfcr, CR logic), XER writes, fcmpu, vector
record compares, traps and conditional branches (forward, and counted loops back) are lifted with and without the
fusion and run under the runtime's RC_BLOCK (XE_RUNTIME) with a time slice of 1, 2, 3, 5 blocks and an unlimited one,
so that a slice ends at every block boundary, on the dead edges' own RC_BLOCK_X too. The state after every return to
the dispatcher (all GPRs, CR, XER, CTR, LR, PC, the instruction count) must be the same in both builds."""

import random
import shutil
import subprocess
import sys

import pytest

from . import CONSOLE  # noqa: F401
from tools.xex.synth import build_xex
from tools.xex.xex import parse_xex

from . import asm as A
from .recomp import RUNTIME_DIRS, recompile
from .xenon import XenonDecoder, XenonLifter

TEXT = 0x82001000

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xenon_rt.h"
typedef void (*xe_rc_fn)(xe_machine *);
extern const xe_rc_entry xe_rc_resume[];
extern const unsigned xe_rc_resume_count;
static const uint32_t entries[] = {%(entries)s};
static xe_machine M;
static xe_rc_fn find(uint32_t pc)
{
    for (unsigned k = 0; k < xe_rc_resume_count; k++)
        if (xe_rc_resume[k].entry == pc)
            return xe_rc_resume[k].fn;
    return NULL;
}
static uint64_t rng;
static uint64_t rnd(void)
{
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return rng;
}
static uint64_t edge(void)
{
    static const uint64_t e[] = {0, 1, 2, 5, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFF80000000ull,
                                 0xFFFFFFFFFFFFFFFFull, 0x100000000ull};
    return (rnd() & 3) ? e[rnd() %% 10] : rnd();
}
int main(void)
{
    static const long slices[] = {1, 2, 3, 5, 1L << 40};
    M.cur = &M.cpu;
    for (unsigned f = 0; f < sizeof entries / sizeof entries[0]; f++)
        for (unsigned s = 0; s < 6; s++)
            for (unsigned k = 0; k < sizeof slices / sizeof slices[0]; k++) {
                xe_cpu *c = &M.cpu;
                memset(c, 0, sizeof *c);
                rng = 0x9E3779B97F4A7C15ull * (f * 8 + s + 1);
                for (int r = 0; r < 32; r++)
                    c->gpr[r] = edge();
                c->gpr[20] = rnd() %% 4 + 1; /* the loop counters */
                c->gpr[21] = rnd() %% 3 + 1;
                c->cr = (uint32_t)rnd();
                c->xer_so = rnd() & 1;
                c->xer_ca = rnd() & 1;
                c->msr = 0x8000u; /* EE set: RC_BLOCK ends the slice */
                c->lr = 0;
                c->pc = entries[f];
                M.rc_native = 0;
                for (int n = 0; n < 100000 && c->pc != 0; n++) {
                    xe_rc_fn fn = find(c->pc);
                    if (!fn) {
                        printf("%%u %%u %%u nofn %%08x\n", f, s, k, c->pc);
                        break;
                    }
                    M.slice = slices[k];
                    M.rc_exit = 0;
                    fn(&M);
                    if (M.rc_exit) {
                        printf("%%u %%u %%u exit %%d\n", f, s, k, M.rc_exit);
                        break;
                    }
                    uint64_t h = 1469598103934665603ull;
#define H(v) (h = (h ^ (uint64_t)(v)) * 1099511628211ull)
                    for (int r = 0; r < 32; r++)
                        H(c->gpr[r]);
                    H(c->cr), H(c->xer_so), H(c->xer_ov), H(c->xer_ca), H(c->ctr), H(c->lr), H(c->pc), H(M.rc_native);
                    printf("%%u %%u %%u %%08x %%08x %%016llx\n", f, s, k, c->pc, c->cr, (unsigned long long)h);
                }
            }
    return 0;
}
"""

REGS = list(range(3, 13))
SKIP = 0  # placeholder word, patched into a forward branch


def random_function(rng, n=40):
    """[(word or ("bc", bo, bi, label)), ...] with labels: a random body of CR producers, readers, branches."""
    body = []
    for _ in range(n):
        k = rng.random()
        rd, ra, rb = rng.choice(REGS), rng.choice(REGS), rng.choice(REGS)
        crf = rng.choice((0, 0, 6, 6, 7, 1, 3))
        if k < 0.30:  # compare, often right before its branch
            cmp = rng.choice((A.cmpw(crf, ra, rb), A.cmplw(crf, ra, rb), A.cmpwi(crf, ra, rng.randint(-3, 3)),
                              A.cmpd(crf, ra, rb), A.cmpdi(crf, ra, 0)))
            body.append(cmp)
            if rng.random() < 0.4:
                body.append(A.addi(ra, ra, 1))  # between the two, the compared register changes
            body.append(("bc", rng.choice((12, 4)), crf * 4 + rng.randint(0, 3)))
        elif k < 0.45:  # record forms (CR0)
            body.append(rng.choice((A.add(rd, ra, rb, 1), A.rlwinm(rd, ra, rng.randint(0, 31), 0, 31, 1),
                                    A.xor(rd, ra, rb, 1), A.extsw(rd, ra, 1))))
            if rng.random() < 0.6:
                body.append(("bc", rng.choice((12, 4)), rng.randint(0, 3)))
        elif k < 0.49:
            body.append(A.mfcr(rd))
        elif k < 0.52:  # SO into CR0: an OE record form, XER written; other fields' setters; a trap (never taken)
            body.append(rng.choice((A.add(rd, ra, rb, 1) | 1 << 10, A.mtspr(1, ra),
                                    (63 << 26) | (crf << 23) | (rng.randint(0, 3) << 16) | (rng.randint(0, 3) << 11),
                                    (4 << 26) | (1 << 21) | (2 << 16) | (3 << 11) | 0x400 | 0x086,
                                    A.twi(4, ra, -12345))))
        elif k < 0.56:
            body.append(A.crxor(rng.randint(0, 31), rng.randint(0, 31), rng.randint(0, 31)))
        elif k < 0.62:  # a counted loop back to here: r20 / r21 count down
            r = rng.choice((20, 21))
            body.append(("loop", r))
        else:
            body.append(rng.choice((A.addi(rd, ra, rng.randint(-4, 4)), A.add(rd, ra, rb), A.srawi(rd, ra, 3),
                                    A.addc(rd, ra, rb), A.adde(rd, ra, rb))))
    return body


def assemble(rng, body, base):
    """Words at base: forward branches to a random later instruction (or the final blr), loops back to their own
    start (`addi r, r, -1; cmpwi cr7, r, 0; bgt cr7, start`), then mfcr r3 and blr."""
    words, fix, loops = [], [], []
    for item in body:
        if isinstance(item, tuple) and item[0] == "loop":
            r = item[1]
            start = len(words)
            words += [A.addi(r, r, -1), A.cmpwi(7, r, 0)]
            words.append(A.bc(base + 4 * len(words), 12, 29, base + 4 * start))
            continue
        if isinstance(item, tuple):
            fix.append((len(words), item[1], item[2]))
            words.append(SKIP)
            continue
        words.append(item)
    words += [A.mfcr(3), A.blr()]
    for at, bo, bi in fix:
        t = rng.randint(at + 1, len(words) - 1)
        words[at] = A.bc(base + 4 * at, bo, bi, base + 4 * t)
    return words


def build(tmp_path, tag, words, funcs, fuse, monkeypatch):
    data, _ = build_xex(words, functions=funcs, compression="none")
    src = tmp_path / ("src_" + tag)
    monkeypatch.setattr(XenonLifter, "FUSE_CR", fuse)
    recompile(parse_xex(data), str(src))
    entries = ", ".join("0x%08Xu" % (TEXT + off) for off, _ in funcs)
    (tmp_path / "h.c").write_text(HARNESS % {"entries": entries})
    inc = []
    for d in RUNTIME_DIRS:
        inc += ["-I", d]
    exe = str(tmp_path / ("t_" + tag))
    subprocess.run(["cc", "-std=gnu11", "-O1", "-w", "-DXE_RUNTIME=1"] + inc + ["-o", exe, str(tmp_path / "h.c")] +
                   sorted(str(p) for p in src.glob("x360_rc_*.c")), check=True)
    return exe, "".join(p.read_text() for p in sorted(src.glob("x360_rc_*.c")))


@pytest.mark.skipif(not shutil.which("cc") or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_dead_cr_stores_leave_the_same_state(tmp_path, monkeypatch):
    rng = random.Random(0xC0DE)
    words, funcs = [], []
    for _ in range(120):
        off = 4 * len(words)
        fw = assemble(rng, random_function(rng), TEXT + off)
        funcs.append((off, 4 * len(fw)))
        words += fw
    exe0, c0 = build(tmp_path, "off", words, funcs, False, monkeypatch)
    exe1, c1 = build(tmp_path, "on", words, funcs, True, monkeypatch)
    assert "RC_BLOCK_X" not in c0 and "cf_" not in c0
    # the fusion happens, dead edges too (loops, compare chains)
    assert c1.count("cf_ = ") > 200 and c1.count("RC_BLOCK_X(") > 50, (c1.count("cf_ = "), c1.count("RC_BLOCK_X("))
    out0 = subprocess.run([exe0], check=True, capture_output=True, text=True).stdout
    out1 = subprocess.run([exe1], check=True, capture_output=True, text=True).stdout
    assert "nofn" not in out0 and "exit" not in out0
    assert len(out0.splitlines()) > 10000
    if out0 != out1:
        a, b = out0.splitlines(), out1.splitlines()
        k = next(n for n in range(min(len(a), len(b))) if a[n] != b[n])
        pytest.fail("first difference, line %d: %s / %s" % (k, a[k], b[k]))


def test_fusion_off_is_the_old_c(monkeypatch):
    """FUSE_CR = False lifts what the lifter lifted before, and the fusion only adds cf_ and RC_BLOCK_X."""
    class F:
        entry = TEXT
        addrs = set(range(TEXT, TEXT + 4 * 4, 4))

    words = [A.cmpwi(6, 3, 0), A.bc(TEXT + 4, 12, 26, TEXT), A.mfcr(3), A.blr()]
    dec = XenonDecoder()
    decode = lambda a: dec.decode(words[(a - TEXT) // 4], a)  # noqa: E731
    monkeypatch.setattr(XenonLifter, "FUSE_CR", False)
    off = XenonLifter(None, decode, F, "f", comments=False).function()
    monkeypatch.setattr(XenonLifter, "FUSE_CR", True)
    on = XenonLifter(None, decode, F, "f", comments=False).function()
    assert "cf_" not in off and "c->cr = ppc_cr_set(c->cr, 6, ppc_cmps(" in off
    # the loop's back edge stores nothing on its way (the block sets cr6 again first); the way out stores it
    assert "cf_ = xe_cmps(" in on and "if (((int32_t)R(3) == 0)) {" in on
    assert "RC_BLOCK_X(0x82001000u, 2, c->cr = ppc_cr_set(c->cr, 6, cf_)); goto L_82001000_b;" in on
    assert "} c->cr = ppc_cr_set(c->cr, 6, cf_);" in on
