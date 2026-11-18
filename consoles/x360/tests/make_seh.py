"""make_seh.py OUT.xex -- the synthetic Xbox 360 exception test title (XBA-282): our own PowerPC assembly in MSVC's
shapes for structured exceptions, packed by tools/xex/synth.py. main has two __try / __except scopes handled by the
kernel's __C_specific_handler (its .pdata entry has the exception flag; the handler and the scope table are the two
words before it):

1. a call to middle, whose own __try / __finally scope wraps a call to thrower; thrower raises 0xE0001234
   (RtlRaiseException). main's filter sees the code and returns EXCEPTION_EXECUTE_HANDLER; the unwind runs middle's
   __finally (abnormal termination: 1), and main's __except block runs with r3 = the code and its non-volatile
   registers back (middle and thrower overwrote r28-r31 after saving them in their prologs);
2. a call to resumer, which raises 0xE0005678; the filter returns EXCEPTION_CONTINUE_EXECUTION, so RtlRaiseException
   returns and resumer goes on.

Every line goes out through DbgPrint (seh.txt holds what run_seh.sh expects). No console data."""

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [os.path.normpath(os.path.join(HERE, "..", "tools")), os.path.normpath(os.path.join(HERE, "..", "..",
                                                                                                    ".."))]

from tools.xex.synth import BASE, PAGE, build_xex  # noqa: E402
from x360_tools import asm as A  # noqa: E402
from x360_tools.exports import ordinal  # noqa: E402
from x360_tools.prog import Prog  # noqa: E402

TEXT = BASE + PAGE
KRNL = ["DbgPrint", "RtlRaiseException", "__C_specific_handler", "RtlUnwind"]
STRINGS = [
    ("s_start", "seh: start\n"),
    ("s_filter", "filter sees %08X\n"),
    ("s_finally", "finally ran (abnormal %d)\n"),
    ("s_caught", "caught %08X, r31 %X, r28 %X\n"),
    ("s_noexc", "no exception (wrong)\n"),
    ("s_filter2", "filter2 sees %08X\n"),
    ("s_cont", "continued after %08X, r30 %X\n"),
    ("s_end", "seh: end\n"),
]
FLAG = 0x80000000          # the .pdata exception flag (x360_seh.c)


def ha(t):
    return ((t + 0x8000) >> 16) & 0xFFFF


def lo(t):
    v = t & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def stwu(rs, d, ra):
    return (37 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)


def program(addr):
    p = Prog(TEXT)
    flags = {}

    def la(r, name):
        t = addr[name]
        p(A.lis(r, ha(t)), A.addi(r, r, lo(t)))

    def call(name):
        if name in addr:
            p(A.bl(p.here, addr[name]))
        else:
            p.branch(A.bl, name)

    def prolog(first, size):
        p(A.mflr(12))
        p.branch(A.bl, "__savegprlr_%d" % first)
        p(stwu(1, -size, 1))

    def epilog(first, size):
        p(A.addi(1, 1, size))
        p.branch(A.b, "__restgprlr_%d" % first)

    def handler_words(scope):
        """The handler and its data, just before a function with the exception flag."""
        p(addr["__C_specific_handler"], addr.get(scope, 0))

    p.func("__savegprlr")
    for r in range(14, 32):
        p.label("__savegprlr_%d" % r)
        p(A.std(r, -0x98 + 8 * (r - 14), 1))
    p(A.stw(12, -8, 1), A.blr())
    p.end()
    p.func("__restgprlr")
    for r in range(14, 32):
        p.label("__restgprlr_%d" % r)
        p(A.ld(r, -0x98 + 8 * (r - 14), 1))
    p(A.lwz(12, -8, 1), A.mtlr(12), A.blr())
    p.end()

    p.func("start")
    prolog(29, 0x70)
    p.branch(A.bl, "main")
    epilog(29, 0x70)
    p.end()

    handler_words("scope_main")
    p.func("main")
    flags["main"] = FLAG
    prolog(28, 0x80)
    p(A.li(31, 0x1234), A.li(28, 0x77))
    la(3, "s_start")
    call("DbgPrint")
    p.label("try1_begin")
    p.branch(A.bl, "middle")
    p.label("try1_end")
    la(3, "s_noexc")
    call("DbgPrint")
    p.branch(A.b, "after1")
    p.label("except1")                      # r3 = the exception code (RtlUnwind's return value)
    p(A.mr(4, 3), A.mr(5, 31), A.mr(6, 28))
    la(3, "s_caught")
    call("DbgPrint")
    p.label("after1")
    p.label("try2_begin")
    p.branch(A.bl, "resumer")
    p.label("try2_end")
    la(3, "s_end")
    call("DbgPrint")
    p(A.li(3, 0))
    epilog(28, 0x80)
    p.label("except2")                      # never reached: the filter continues execution
    p(A.li(3, 9))
    epilog(28, 0x80)
    p.end()

    # main's filters: (EXCEPTION_POINTERS *, establisher frame)
    p.func("filter1")
    prolog(31, 0x60)
    p(A.lwz(5, 0, 3), A.lwz(4, 0, 5))
    la(3, "s_filter")
    call("DbgPrint")
    p(A.li(3, 1))                           # EXCEPTION_EXECUTE_HANDLER
    epilog(31, 0x60)
    p.end()

    p.func("filter2")
    prolog(31, 0x60)
    p(A.lwz(5, 0, 3), A.lwz(4, 0, 5))
    la(3, "s_filter2")
    call("DbgPrint")
    p(A.li(3, -1))                          # EXCEPTION_CONTINUE_EXECUTION
    epilog(31, 0x60)
    p.end()

    handler_words("scope_middle")
    p.func("middle")
    flags["middle"] = FLAG
    prolog(29, 0x70)
    p(A.li(29, 0x5555), A.li(31, 0xDEAD))
    p.label("tryf_begin")
    p.branch(A.bl, "thrower")
    p.label("tryf_end")
    p(A.li(3, 0))
    epilog(29, 0x70)
    p.end()

    # middle's __finally: (abnormal termination, establisher frame)
    p.func("finally_mid")
    prolog(31, 0x60)
    p(A.mr(4, 3))
    la(3, "s_finally")
    call("DbgPrint")
    epilog(31, 0x60)
    p.end()

    def raise_code(code, flags_=0):
        p(A.lis(0, code >> 16), A.ori(0, 0, code & 0xFFFF), A.stw(0, 0x50, 1), A.li(0, flags_), A.stw(0, 0x54, 1),
          A.li(0, 0), A.stw(0, 0x58, 1), A.stw(0, 0x5C, 1), A.stw(0, 0x60, 1), A.addi(3, 1, 0x50))
        call("RtlRaiseException")

    p.func("thrower")
    prolog(28, 0x90)
    p(A.li(28, 0x6666), A.li(30, 0x6666), A.li(31, 0x7777))
    raise_code(0xE0001234, 1)               # EXCEPTION_NONCONTINUABLE
    epilog(28, 0x90)
    p.end()

    p.func("resumer")
    prolog(30, 0x90)
    p(A.li(30, 0x4242))
    raise_code(0xE0005678)
    p(A.lwz(4, 0x50, 1), A.mr(5, 30))
    la(3, "s_cont")
    call("DbgPrint")
    epilog(30, 0x90)
    p.end()

    words = p.link()
    funcs = []
    for name, a, b in p.funcs:
        funcs.append((4 * a, 4 * (b - a), 0 if name.startswith(("__save", "__rest")) else 3,
                      flags.get(name, 0) | 0x40000000))    # bit 30: 32-bit code
    return words, funcs, p.labels


def layout(n_words):
    imports = {"xboxkrnl.exe": [(ordinal("xboxkrnl", n), "thunk") for n in KRNL]}
    addr = {}
    thunk = TEXT + 4 * n_words
    for o, _ in imports["xboxkrnl.exe"]:
        addr[("xboxkrnl.exe", o)] = thunk
        thunk += 16
    rdata_rva = PAGE + ((thunk - TEXT + PAGE - 1) & ~(PAGE - 1))
    rdata = b""
    for name, s in STRINGS:
        addr[name] = BASE + rdata_rva + len(rdata)
        rdata += s.encode() + b"\0"
        rdata += bytes(-len(rdata) % 4)
    n_records = len(imports["xboxkrnl.exe"])
    data_rva = rdata_rva + ((len(rdata) + 4 * n_records + PAGE - 1) & ~(PAGE - 1))
    addr["scope_main"] = BASE + data_rva
    addr["scope_middle"] = BASE + data_rva + 0x40
    for n in KRNL:
        addr[n] = addr[("xboxkrnl.exe", ordinal("xboxkrnl", n))]
    return imports, rdata, addr


def scope_tables(labels):
    """{Count, {Begin, End, Handler (filter / __finally), JumpTarget (0: __finally)}...} for main and middle."""
    main = [(labels["try1_begin"], labels["try1_end"], labels["filter1"], labels["except1"]),
            (labels["try2_begin"], labels["try2_end"], labels["filter2"], labels["except2"])]
    mid = [(labels["tryf_begin"], labels["tryf_end"], labels["finally_mid"], 0)]
    data = bytearray(0x80)
    for off, recs in ((0, main), (0x40, mid)):
        struct.pack_into(">I", data, off, len(recs))
        for k, r in enumerate(recs):
            struct.pack_into(">IIII", data, off + 4 + 16 * k, *r)
    return bytes(data)


def build():
    words, _, _ = program(layout(0)[2])
    imports, rdata, addr = layout(len(words))
    words, funcs, labels = program(addr)
    data = scope_tables(labels)
    xex, lay = build_xex(words, rdata=rdata, data=data, imports=imports, entry=labels["start"] - TEXT,
                         functions=funcs, compression="basic", name="seh.exe")
    assert lay.data == addr["scope_main"]
    for n in KRNL:
        assert lay.thunks[("xboxkrnl.exe", ordinal("xboxkrnl", n))] == addr[n], n
    return xex


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    a = ap.parse_args()
    with open(a.out, "wb") as f:
        f.write(build())
    return 0


if __name__ == "__main__":
    sys.exit(main())
