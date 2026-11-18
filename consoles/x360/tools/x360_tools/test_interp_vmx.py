"""Differential test of the generated VMX / VMX128 interpreter (XBA-283, x360_interp_vmx.inc) against lifted code:
random instruction words of every vector mnemonic (register fields, immediates, record bits drawn at random; the
VMX128 register numbers split over their fields), each lifted into its own function, and the same word run by
vmx_exec on the same state. Vector registers, CR, VSCR and memory must come out bit for bit the same: this checks the
dispatch and the field extraction the generator writes by hand (the bodies are the lifter's)."""

import os
import random
import shutil
import subprocess
import sys

import pytest

from . import CONSOLE  # noqa: F401
from tools.xex.synth import build_xex
from tools.xex.xex import parse_xex

from .recomp import RUNTIME_DIRS, recompile
from .xenon import OP5, OP6, OP6_3, OP6_R, VA4, VC4, VX4, VX128_1, X31_VMX, VEC_LOADSTORE, XenonDecoder

TEXT = 0x82001000
BLR = 0x4E800020
HERE = os.path.dirname(os.path.abspath(__file__))
INC = os.path.normpath(os.path.join(HERE, "..", "..", "runtime", "x360_interp_vmx.inc"))

HARNESS = r"""
#include <stdio.h>
#include "xenon_rt.h"
typedef struct x360_machine { xe_machine m; } x360_machine;
#include "%(inc)s"
extern const xe_rc_entry xe_rc_resume[];
extern const unsigned xe_rc_resume_count;
static uint8_t mem[0x400];
static x360_machine X;
static uint32_t rnd = 2832;
static uint32_t next(void) { rnd = rnd * 1103515245u + 12345u; return rnd ^ (rnd >> 15); }
static void init(unsigned seed, unsigned r4)
{
    rnd = seed * 2654435761u + 1;
    memset(&X.m.cpu, 0, sizeof X.m.cpu);
    for (int k = 0; k < 128; k++)
        for (int j = 0; j < 4; j++) {
            uint32_t v = next();
            if ((v & 7) == 0)
                v = (v & 0x80000000u) | 0x3F800000u; /* some ordinary floats */
            X.m.cpu.vr[k].u32[j] = v;
        }
    for (int k = 0; k < (int)sizeof mem; k++)
        mem[k] = (uint8_t)next();
    X.m.cpu.cr = next();
    X.m.cpu.vscr = (next() & 1) ? 0x00010000u : 0;
    X.m.cpu.gpr[3] = 0x40000100u;
    X.m.cpu.gpr[4] = r4;
}
static void dump(void)
{
    const uint8_t *p = (const uint8_t *)X.m.cpu.vr;
    for (unsigned k = 0; k < sizeof X.m.cpu.vr; k++)
        printf("%%02x", p[k]);
    printf(" %%08x %%08x ", (unsigned)X.m.cpu.cr, (unsigned)X.m.cpu.vscr);
    for (unsigned k = 0; k < sizeof mem; k++)
        printf("%%02x", mem[k]);
    printf("\n");
}
static int lifted(uint32_t entry)
{
    X.m.cpu.pc = entry;
    X.m.cpu.lr = 0xDEAD0000u;
    for (int n = 0; n < 16; n++) {
        if (X.m.cpu.pc == 0xDEAD0000u)
            return 0;
        xe_rc_fn f = NULL;
        for (unsigned k = 0; k < xe_rc_resume_count; k++)
            if (xe_rc_resume[k].entry == X.m.cpu.pc)
                f = xe_rc_resume[k].fn;
        if (!f)
            return 2;
        X.m.rc_exit = 0;
        f(&X.m);
        if (X.m.rc_exit)
            return 10 + X.m.rc_exit;
    }
    return 3;
}
static void c(uint32_t entry, uint32_t w, unsigned seed, unsigned r4)
{
    init(seed, r4);
    printf("%%d ", lifted(entry));
    dump();
    init(seed, r4);
    printf("%%d ", vmx_exec(&X, &X.m.cpu, w));
    dump();
}
int main(void)
{
    X.m.mem = mem;
    X.m.mem_base = 0x40000000u;
    X.m.mem_size = sizeof mem;
%(calls)s
    return 0;
}
"""


def samples(rng, per_name=4):
    """[(name, word)]: random words that decode to each vector mnemonic."""
    dec = XenonDecoder()
    forms = [(4, VX128_1, 0x7F3), (4, VX4, 0x7FF), (4, VC4, 0x3FF), (4, VA4, 0x3F), (5, OP5, 0x3D0),
             (6, OP6_3, 0x7F0), (6, OP6_R, 0x390), (6, OP6, 0x3D0)]
    keys = []
    for op, table, mask in forms:
        keys += [(op, key, mask, name) for key, name in table.items()]
    keys += [(31, xo << 1, 0x7FE, name) for xo, name in X31_VMX.items()]
    keys += [(4, 0x10, 0x10, "vsldoi128"), (5, 0, 0x210, "vperm128"), (6, 0x210, 0x630, "vpermwi128"),
             (6, 0x610, 0x730, "vpkd3d128"), (6, 0x710, 0x730, "vrlimi128")]
    out = []
    for op, key, mask, name in keys:
        got = 0
        for _ in range(400):
            w = (op << 26) | key | (rng.getrandbits(26) & ~mask & 0x03FFFFFF)
            if name in VEC_LOADSTORE:
                w = (w & ~(0x3FF << 11)) | (3 << 16) | (4 << 11)  # rA = r3, rB = r4
            if op == 31:
                w &= ~1
            i = dec.decode(w, TEXT)
            if i.name == name:
                out.append((name, w))
                got += 1
                if got == per_name:
                    break
        assert got, name
    return out


@pytest.mark.skipif(not shutil.which("cc") or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_vmx_interpreter_matches_lifted_code(tmp_path):
    rng = random.Random(283)
    cases = samples(rng)
    words, funcs = [], []
    for _, w in cases:
        funcs.append((4 * len(words), 8))
        words += [w, BLR]
    data, _ = build_xex(words, functions=funcs, compression="none")
    src = tmp_path / "src"
    s = recompile(parse_xex(data), str(src))
    assert s["interp_fallback_insns"] == 0, s["unlifted"]
    calls = []
    for k, ((off, _), (_, w)) in enumerate(zip(funcs, cases)):
        calls.append("    c(0x%08Xu, 0x%08Xu, %du, %du);" % (TEXT + off, w, k, rng.randrange(0, 0x100)))
    (tmp_path / "h.c").write_text(HARNESS % {"inc": INC, "calls": "\n".join(calls)})
    inc = []
    for d in RUNTIME_DIRS:
        inc += ["-I", d]
    exe = str(tmp_path / "t")
    subprocess.run(["cc", "-std=gnu11", "-O1", "-w", "-ffp-contract=off"] + inc + ["-o", exe, str(tmp_path / "h.c")] +
                   sorted(str(p) for p in src.glob("x360_rc_*.c")) + ["-lm"], check=True)
    out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout.splitlines()
    assert len(out) == 2 * len(cases)
    bad = []
    for k, (name, w) in enumerate(cases):
        a, b = out[2 * k].split(" ", 1), out[2 * k + 1].split(" ", 1)
        assert a[0] == "0", (name, "%08X" % w, "lifted run", a[0])
        if b[0] != "0" or a[1] != b[1]:
            bad.append("%s %08X%s" % (name, w, " (vmx_exec %s)" % b[0] if b[0] != "0" else ""))
    assert not bad, bad[:20]
