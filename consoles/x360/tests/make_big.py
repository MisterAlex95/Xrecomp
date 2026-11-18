"""make_big.py OUT.xex [--mb N] [--seed S] -- a large synthetic title made of our own generated code (XBA-283), to
measure what Prepare costs on a whole retail-sized XEX (tools/prepare/bench_x360.py). Never run: only lifted.

N MB of .text (default 12) in functions of 40..400 instructions, every one listed in .pdata as MSVC titles do. Each
has a prologue and an epilogue, and a body drawn from what lifted Xenon code is made of: integer and 64-bit
arithmetic, rotates, loads and stores on the stack frame, compares and short forward branches (several blocks per
function), calls to other functions, scalar FP and VMX arithmetic, vector loads / stores. Deterministic (--seed)."""

import argparse
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [os.path.normpath(os.path.join(HERE, "..", "tools")), os.path.normpath(os.path.join(HERE, "..", "..",
                                                                                                    ".."))]

from tools.xex.synth import BASE, PAGE, build_xex  # noqa: E402
from x360_tools import asm as A  # noqa: E402

TEXT = BASE + PAGE
FRAME = 0x100


def body_insn(r, words, here, funcs):
    """One instruction (or a short group) of a function body."""
    k = r.random()
    g = lambda: r.randrange(3, 12)  # noqa: E731  (volatile GPRs)
    f = lambda: r.randrange(0, 14)  # noqa: E731
    v = lambda: r.randrange(0, 32)  # noqa: E731
    off = lambda: 0x10 + 8 * r.randrange(0, 24)  # noqa: E731
    if k < 0.14:
        words.append(A.addi(g(), g(), r.randrange(-500, 500)))
    elif k < 0.24:
        words.append(A.add(g(), g(), g()))
    elif k < 0.30:
        words.append(A.rlwinm(g(), g(), r.randrange(32), r.randrange(16), r.randrange(16, 32)))
    elif k < 0.34:
        words.append(A.mullw(g(), g(), g()))
    elif k < 0.38:
        words.append(A.rldicl(g(), g(), r.randrange(64), r.randrange(64)))
    elif k < 0.42:
        words.append(A.xor(g(), g(), g()))
    elif k < 0.54:
        words.append(A.lwz(g(), off(), 1))
    elif k < 0.64:
        words.append(A.stw(g(), off(), 1))
    elif k < 0.67:
        words.append(A.ld(g(), off(), 1))
    elif k < 0.70:
        words.append(A.std(g(), off(), 1))
    elif k < 0.74:
        words.append(A.fadd(f(), f(), f()))
    elif k < 0.77:
        words.append(A.fmul(f(), f(), f()))
    elif k < 0.79:
        words.append(A.lfd(f(), off(), 1))
    elif k < 0.82:
        words.append(A.vaddfp(v(), v(), v()))
    elif k < 0.85:
        words.append(A.vmaddfp(v(), v(), v(), v()))
    elif k < 0.87:
        words.extend([A.addi(11, 1, 0x40), A.lvx(v(), 0, 11)])
    elif k < 0.88:
        words.extend([A.addi(11, 1, 0x40), A.stvx(v(), 0, 11)])
    elif k < 0.95:
        # a compare and a short forward branch: a new block
        words.append(A.cmpwi(r.randrange(8), g(), r.randrange(-100, 100)))
        skip = r.randrange(2, 6)
        at = here + 4 * len(words)
        words.append(A.bc(at, 12, r.randrange(32), at + 4 * skip))
        for _ in range(skip - 1):
            words.append(A.addi(g(), g(), r.randrange(-50, 50)))
    else:
        # a call
        at = here + 4 * len(words)
        words.append(A.bl(at, r.choice(funcs)))


def build(mb, seed):
    r = random.Random(seed)
    total = mb << 20
    sizes = []
    n = 0
    while n < total:
        s = r.randrange(40, 400)
        sizes.append(s)
        n += 4 * (s + 10)
    # function addresses first (calls go anywhere), then the code
    addrs, a = [], TEXT
    for s in sizes:
        addrs.append(a)
        a += 4 * (s + 10)
    words, functions = [], []
    for fi, s in enumerate(sizes):
        start = len(words)
        here = TEXT + 4 * start
        assert here == addrs[fi]
        fn = [A.mflr(12), A.stw(12, -8, 1), (37 << 26) | (1 << 21) | (1 << 16) | (-FRAME & 0xFFFF)]  # stwu
        body = []
        while len(body) < s:
            body_insn(r, body, here + 4 * len(fn), addrs)
        fn += body[:s] if len(body) == s else body
        fn += [A.addi(1, 1, FRAME), A.lwz(12, -8, 1), A.mtlr(12), A.blr()]
        # pad to the planned size so the next function starts where the calls expect it
        want = s + 10
        while len(fn) < want:
            fn.append(A.ori(0, 0, 0))
        if len(fn) > want:  # a group ran past the plan: cut the body, keep the epilogue
            fn = fn[:want - 4] + [A.addi(1, 1, FRAME), A.lwz(12, -8, 1), A.mtlr(12), A.blr()]
        words += fn
        functions.append((4 * start, 4 * len(fn)))
    xex, _ = build_xex(words, functions=functions, compression="basic", name="big.exe")
    return xex, len(sizes), len(words)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--mb", type=int, default=12)
    ap.add_argument("--seed", type=int, default=283)
    a = ap.parse_args(argv)
    xex, nf, nw = build(a.mb, a.seed)
    with open(a.out, "wb") as f:
        f.write(xex)
    print("%s: %d functions, %d instructions (%.1f MB of code), XEX %.1f MB" % (a.out, nf, nw, 4 * nw / 2 ** 20,
                                                                               len(xex) / 2 ** 20))
    return 0


if __name__ == "__main__":
    sys.exit(main())
