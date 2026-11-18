"""make_msr.py OUT.xex [--no-msr] -- two threads increment a shared word 50,000 times each with a load / store that
the scheduler could split (a branch between them starts a new block), inside mfmsr / mtmsrd sections that clear
MSR[EE] (XBA-270). The title's exit code is the final count minus 100,000: 0 when no update was lost, which needs
the runtime not to preempt a thread while EE is clear. --no-msr leaves the mtmsrd out (nops): updates are lost
then, which shows the test can fail."""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [os.path.normpath(os.path.join(HERE, "..", "tools")), os.path.normpath(os.path.join(HERE, "..", "..",
                                                                                                    ".."))]

from tools.xex.synth import BASE, PAGE, build_xex  # noqa: E402
from x360_tools import asm as A  # noqa: E402
from x360_tools.exports import ordinal  # noqa: E402
from x360_tools.prog import Prog  # noqa: E402

TEXT = BASE + PAGE
IMPORTS = ["ExCreateThread", "NtWaitForSingleObjectEx"]
NOP = 0x60000000


def ha(t):
    return ((t + 0x8000) >> 16) & 0xFFFF


def lo(t):
    v = t & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def mtmsrd(rs, l=1):
    return (31 << 26) | (rs << 21) | (l << 16) | (178 << 1)


def stwu(rs, d, ra):
    return (37 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)


def program(addr, msr=True):
    p = Prog(TEXT)

    def call(name):
        p(A.bl(p.here, addr[name]))

    p.func("main")
    p(stwu(1, -0x60, 1), A.mflr(0), A.stw(0, 0x64, 1),
      A.addi(3, 1, 0x50), A.li(4, 0), A.li(5, 0), A.li(6, 0))
    p.branch(lambda at, t: A.lis(7, ha(t)), "worker")
    p.branch(lambda at, t: A.addi(7, 7, lo(t)), "worker")
    p(A.li(8, 0), A.li(9, 0))
    call("ExCreateThread")
    p.branch(A.bl, "incloop")
    p(A.lwz(3, 0x50, 1), A.li(4, 1), A.li(5, 0), A.li(6, 0))
    call("NtWaitForSingleObjectEx")
    c = addr["counter"]
    p(A.lis(11, ha(c)), A.lwz(3, lo(c), 11), A.lis(4, ha(100000)), A.addi(4, 4, lo(100000)), A.subf(3, 4, 3),
      A.lwz(0, 0x64, 1), A.mtlr(0), A.addi(1, 1, 0x60), A.blr())
    p.end()
    p.func("worker")
    p(A.mflr(0), stwu(1, -0x20, 1), A.stw(0, 0x24, 1))
    p.branch(A.bl, "incloop")
    p(A.li(3, 0), A.lwz(0, 0x24, 1), A.mtlr(0), A.addi(1, 1, 0x20), A.blr())
    p.end()
    p.func("incloop")
    p(A.lis(11, ha(c)), A.addi(11, 11, lo(c)), A.lis(9, 0), A.ori(9, 9, 50000), A.mtctr(9))
    p.label("loop")
    p(A.mfmsr(12), A.rlwinm(10, 12, 0, 17, 15), mtmsrd(10) if msr else NOP,
      A.lwz(5, 0, 11), A.cmpwi(0, 5, -1))
    p.branch(lambda at, t: A.bc(at, 12, 2, t), "next")  # never taken: a block boundary between load and store
    p.label("next")
    p(A.addi(5, 5, 1), A.stw(5, 0, 11), mtmsrd(12) if msr else NOP)
    p.branch(A.bdnz, "loop")
    p(A.blr())
    p.end()
    return p.link(), p.funcs


def build(msr=True):
    words, _ = program({"ExCreateThread": TEXT, "NtWaitForSingleObjectEx": TEXT, "counter": 0}, msr)
    n = len(words)
    addr = {name: TEXT + 4 * n + 16 * k for k, name in enumerate(IMPORTS)}
    text_size = 4 * n + 16 * len(IMPORTS)
    rdata_rva = PAGE + ((text_size + PAGE - 1) & ~(PAGE - 1))
    data_rva = rdata_rva + PAGE  # .rdata holds only the two import records
    addr["counter"] = BASE + data_rva
    words, funcs = program(addr, msr)
    imports = {"xboxkrnl.exe": [(ordinal("xboxkrnl", name), "thunk") for name in IMPORTS]}
    xex, lay = build_xex(words, data=bytes(16), imports=imports, functions=[(4 * a, 4 * (b - a)) for _, a, b in funcs],
                         compression="basic", name="msr.exe")
    assert lay.data == addr["counter"]
    for name in IMPORTS:
        assert lay.thunks[("xboxkrnl.exe", ordinal("xboxkrnl", name))] == addr[name]
    return xex


if __name__ == "__main__":
    with open(sys.argv[1], "wb") as f:
        f.write(build("--no-msr" not in sys.argv))
