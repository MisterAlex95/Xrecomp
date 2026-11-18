"""make_missed.py OUT.xex -- a title with a function discovery cannot see (XBA-270): only a pointer in .data reaches
it (bctrl), and .pdata does not list it. The first recompiled run must stop on it and name it (x360_run --missed);
recompiled again with that address (x360_tools.recomp --discovered), the title ends with exit code 42."""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [os.path.normpath(os.path.join(HERE, "..", "tools")), os.path.normpath(os.path.join(HERE, "..", "..",
                                                                                                    ".."))]

from tools.xex.synth import BASE, PAGE, build_xex  # noqa: E402
from x360_tools import asm as A  # noqa: E402
from x360_tools.prog import Prog  # noqa: E402

TEXT = BASE + PAGE


def ha(t):
    return ((t + 0x8000) >> 16) & 0xFFFF


def lo(t):
    v = t & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def program(fptr):
    p = Prog(TEXT)
    p.func("main")
    p((37 << 26) | (1 << 21) | (1 << 16) | (-0x20 & 0xFFFF))  # stwu r1, -0x20(r1)
    p(A.mflr(0), A.stw(0, 0x24, 1), A.lis(11, ha(fptr)), A.lwz(11, lo(fptr), 11), A.mtctr(11), A.bctrl(),
      A.lwz(0, 0x24, 1), A.mtlr(0), A.addi(1, 1, 0x20), A.blr())
    p.end()
    p.label("hidden")  # not in .pdata, no direct call
    p(A.li(3, 42), A.blr())
    return p.link(), p.funcs, p.labels


def build():
    words, _, _ = program(0)
    data_rva = PAGE + ((4 * len(words) + PAGE - 1) & ~(PAGE - 1)) + PAGE  # synth.py: .rdata (empty) then .data
    fptr = BASE + data_rva
    words, funcs, labels = program(fptr)
    data = labels["hidden"].to_bytes(4, "big")
    xex, lay = build_xex(words, data=data, functions=[(0, 4 * funcs[0][2])], compression="basic", name="missed.exe")
    assert lay.data == fptr
    return xex, labels["hidden"]


if __name__ == "__main__":
    xex, hidden = build()
    with open(sys.argv[1], "wb") as f:
        f.write(xex)
    print("%08X" % hidden)
