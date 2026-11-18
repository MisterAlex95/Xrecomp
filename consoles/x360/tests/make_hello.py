"""make_hello.py OUT.xex [--compression none|basic|normal] [--key-file KEY] -- the synthetic Xbox 360 test title
(XBA-270): written here in PowerPC assembly (x360_tools.asm), in the shapes MSVC gives a title (a CRT-like entry
point calling main, prologues and epilogues through __savegprlr / __restgprlr, a jump table), packed by
tools/xex/synth.py with imports from xboxkrnl.exe (and one from xam.xex) by their real ordinals. Run recompiled
under x360_run it must print hello.txt through DbgPrint: memory allocation, a critical section, TLS, a second
thread with an event, a wait on the thread, a delay, a semaphore, a switch. No console data, no key (a key file,
if given, is any 16 bytes the test makes up)."""

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
KRNL = ["DbgPrint", "NtAllocateVirtualMemory", "NtFreeVirtualMemory", "RtlInitializeCriticalSection",
        "RtlEnterCriticalSection", "RtlLeaveCriticalSection", "NtCreateEvent", "NtSetEvent", "NtWaitForSingleObjectEx",
        "ExCreateThread", "NtClose", "KeTlsAlloc", "KeTlsSetValue", "KeTlsGetValue", "KeDelayExecutionThread",
        "NtCreateSemaphore", "NtReleaseSemaphore", "NtOpenFile", "NtReadFile", "NtQueryInformationFile",
        "NtQueryDirectoryFile"]
XAM = ["XamLoaderTerminateTitle"]  # imported, never called: an xam.xex import in the table
VARS = ["XboxHardwareInfo"]

STRINGS = [
    ("s_hello", "Xbox 360 title recompiled: hello from main()\n"),
    ("s_hw", "hardware threads: %d\n"),
    ("s_mem", "alloc status %08X, sum %d, free status %08X\n"),
    ("s_cs", "critical section entered %d times, left\n"),
    ("s_tls", "tls slot %d = %X\n"),
    ("s_worker", "hello from a second thread (%s)\n"),
    ("s_arg", "context ok"),
    ("s_said", "worker said %d\n"),
    ("s_exit", "worker exited (wait status %d)\n"),
    ("s_slept", "slept, delay status %d\n"),
    ("s_sem", "semaphore: %d signals counted\n"),
    ("s_sw", "switch: %d %d %d\n"),
    ("s_file", "file: open %X, read %X, %d bytes: %s\n"),
    ("s_size", "file size %d, info status %X\n"),
    ("s_path", "game:\\data\\hello.txt"),
    ("s_root", "\\Device\\Cdrom0\\"),
    ("s_entry", "entry: %.*s%s\n"),
    ("s_isdir", " (directory)"),
    ("s_isfile", ""),
    ("s_dirend", "listing ends with %X\n"),
    ("s_end", "main returns %d\n"),
]


def ha(t):
    return ((t + 0x8000) >> 16) & 0xFFFF


def lo(t):
    v = t & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def cmplwi(crf, ra, uimm):
    return (10 << 26) | ((crf << 2) << 21) | (ra << 16) | (uimm & 0xFFFF)


def sth(rs, d, ra):
    return (44 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)


def stwu(rs, d, ra):
    return (37 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)


def program(addr):
    """addr: label -> address for the data and the imports (thunks, variable records)."""
    p = Prog(TEXT)

    def la(r, name):
        t = addr[name]
        p(A.lis(r, ha(t)), A.addi(r, r, lo(t)))

    def call(name):
        if name in addr:
            p(A.bl(p.here, addr[name]))
        else:
            p.branch(A.bl, name)

    # the MSVC helpers (GPR + LR only)
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

    # entry point: a frame, main(), its value back to the caller (the thread's end)
    p.func("start")
    p(A.mflr(12))
    p.branch(A.bl, "__savegprlr_29")
    p(stwu(1, -0x70, 1))
    p.branch(A.bl, "main")
    p(A.addi(1, 1, 0x70))
    p.branch(A.b, "__restgprlr_29")
    p.end()

    p.func("main")
    p(A.mflr(12))
    p.branch(A.bl, "__savegprlr_24")
    p(stwu(1, -0xC0, 1))
    la(3, "s_hello")
    call("DbgPrint")
    # a variable import: XboxHardwareInfo's address is in its record; the thread count is the byte at +4
    rec = addr["rec_XboxHardwareInfo"]
    p(A.lis(11, ha(rec)), A.lwz(11, lo(rec), 11), A.lbz(4, 4, 11))
    la(3, "s_hw")
    call("DbgPrint")
    # 64 KB of virtual memory, filled and summed, freed
    p(A.li(0, 0), A.stw(0, 0x50, 1), A.lis(0, 1), A.stw(0, 0x54, 1),
      A.addi(3, 1, 0x50), A.addi(4, 1, 0x54), A.li(5, 0x3000), A.li(6, 4))
    call("NtAllocateVirtualMemory")
    p(A.mr(31, 3), A.lwz(30, 0x50, 1), A.li(4, 0), A.li(5, 256), A.mtctr(5), A.mr(6, 30))
    p.label("fill")
    p(A.stw(4, 0, 6), A.addi(4, 4, 3), A.addi(6, 6, 4))
    p.branch(A.bdnz, "fill")
    p(A.li(5, 256), A.mtctr(5), A.mr(6, 30), A.li(29, 0))
    p.label("sum")
    p(A.lwz(4, 0, 6), A.add(29, 29, 4), A.addi(6, 6, 4))
    p.branch(A.bdnz, "sum")
    p(A.addi(3, 1, 0x50), A.addi(4, 1, 0x54), A.li(5, 0), A.ori(5, 5, 0x8000))
    call("NtFreeVirtualMemory")
    p(A.mr(6, 3), A.mr(4, 31), A.mr(5, 29))
    la(3, "s_mem")
    call("DbgPrint")
    # a critical section, entered twice (recursion), left twice
    la(3, "cs")
    call("RtlInitializeCriticalSection")
    la(3, "cs")
    call("RtlEnterCriticalSection")
    la(3, "cs")
    call("RtlEnterCriticalSection")
    la(11, "cs")
    p(A.lwz(28, 0x14, 11))     # recursion count
    la(3, "cs")
    call("RtlLeaveCriticalSection")
    la(3, "cs")
    call("RtlLeaveCriticalSection")
    p(A.mr(4, 28))
    la(3, "s_cs")
    call("DbgPrint")
    # TLS
    call("KeTlsAlloc")
    p(A.mr(28, 3), A.li(4, 0x1234))
    call("KeTlsSetValue")
    p(A.mr(3, 28))
    call("KeTlsGetValue")
    p(A.mr(5, 3), A.mr(4, 28))
    la(3, "s_tls")
    call("DbgPrint")
    # an event the worker sets; ctx = shared block {value, event handle}
    la(3, "shared")
    p(A.addi(3, 3, 4), A.li(4, 0), A.li(5, 1), A.li(6, 0))
    call("NtCreateEvent")
    p(A.addi(3, 1, 0x58), A.li(4, 0), A.addi(5, 1, 0x5C), A.li(6, 0))
    p.branch(lambda at, t: A.lis(7, ha(t)), "worker")
    p.branch(lambda at, t: A.addi(7, 7, lo(t)), "worker")
    la(8, "shared")
    p(A.li(9, 0))
    call("ExCreateThread")
    la(11, "shared")
    p(A.lwz(3, 4, 11), A.li(4, 1), A.li(5, 0), A.li(6, 0))
    call("NtWaitForSingleObjectEx")
    la(11, "shared")
    p(A.lwz(4, 0, 11))
    la(3, "s_said")
    call("DbgPrint")
    p(A.lwz(3, 0x58, 1), A.li(4, 1), A.li(5, 0), A.li(6, 0))
    call("NtWaitForSingleObjectEx")
    p(A.mr(4, 3))
    la(3, "s_exit")
    call("DbgPrint")
    p(A.lwz(3, 0x58, 1))
    call("NtClose")
    # a 1 ms delay (relative: negative 100 ns units)
    p(A.li(0, -1), A.stw(0, 0x60, 1), A.li(0, -10000), A.stw(0, 0x64, 1), A.li(3, 1), A.li(4, 0), A.addi(5, 1, 0x60))
    call("KeDelayExecutionThread")
    p(A.mr(4, 3))
    la(3, "s_slept")
    call("DbgPrint")
    # a semaphore released three times, then taken three times with a zero timeout, a fourth time times out
    p(A.addi(3, 1, 0x68), A.li(4, 0), A.li(5, 0), A.li(6, 10))
    call("NtCreateSemaphore")
    p(A.lwz(3, 0x68, 1), A.li(4, 3), A.li(5, 0))
    call("NtReleaseSemaphore")
    p(A.li(0, 0), A.stw(0, 0x70, 1), A.stw(0, 0x74, 1), A.li(27, 0), A.li(26, 4))
    p.label("semloop")
    p(A.lwz(3, 0x68, 1), A.li(4, 1), A.li(5, 0), A.addi(6, 1, 0x70))
    call("NtWaitForSingleObjectEx")
    p(A.cmpwi(0, 3, 0))
    p.branch(lambda at, t: A.bc(at, 4, 2, t), "semnext")  # bne: timed out
    p(A.addi(27, 27, 1))
    p.label("semnext")
    p(A.addi(26, 26, -1), A.cmpwi(0, 26, 0))
    p.branch(lambda at, t: A.bc(at, 4, 2, t), "semloop")
    p(A.mr(4, 27))
    la(3, "s_sem")
    call("DbgPrint")
    # files on the disc: OBJECT_ATTRIBUTES at r1+0x80 {root, ANSI_STRING*, attributes}, the string at r1+0x90
    def attrs(name, length):
        la(9, name)
        p(A.li(0, 0), A.stw(0, 0x80, 1), A.stw(0, 0x88, 1), A.addi(10, 1, 0x90), A.stw(10, 0x84, 1),
          A.li(0, length), sth(0, 0x90, 1), sth(0, 0x92, 1), A.stw(9, 0x94, 1))

    attrs("s_path", 20)
    p(A.addi(3, 1, 0x78), A.li(4, 1), A.addi(5, 1, 0x80), A.addi(6, 1, 0x98), A.li(7, 0x40))
    call("NtOpenFile")
    p(A.mr(23, 3), A.lwz(3, 0x78, 1), A.li(4, 0), A.li(5, 0), A.li(6, 0), A.addi(7, 1, 0x98))
    la(8, "filebuf")
    p(A.li(9, 63), A.li(10, 0))
    call("NtReadFile")
    p(A.mr(5, 3), A.mr(4, 23), A.lwz(6, 0x9C, 1))
    la(7, "filebuf")
    la(3, "s_file")
    call("DbgPrint")
    p(A.lwz(3, 0x78, 1), A.addi(4, 1, 0x98), A.addi(5, 1, 0xA0), A.li(6, 24), A.li(7, 5))
    call("NtQueryInformationFile")
    p(A.mr(5, 3), A.lwz(4, 0xAC, 1))  # EndOfFile's low word
    la(3, "s_size")
    call("DbgPrint")
    p(A.lwz(3, 0x78, 1))
    call("NtClose")
    # the disc's root directory, one entry per call (the ninth argument, restart scan, in its stack slot)
    attrs("s_root", 15)
    p(A.addi(3, 1, 0x78), A.li(4, 1), A.addi(5, 1, 0x80), A.addi(6, 1, 0x98), A.li(7, 1))
    call("NtOpenFile")
    p.label("dirloop")
    p(A.li(0, 0), A.stw(0, 0x54, 1), A.lwz(3, 0x78, 1), A.li(4, 0), A.li(5, 0), A.li(6, 0), A.addi(7, 1, 0x98))
    la(8, "dirbuf")
    p(A.li(9, 0x100), A.li(10, 0))
    call("NtQueryDirectoryFile")
    p(A.cmpwi(0, 3, 0))
    p.branch(lambda at, t: A.bc(at, 4, 2, t), "dirdone")
    la(11, "dirbuf")
    p(A.lwz(4, 60, 11), A.addi(5, 11, 0x40), A.lwz(0, 56, 11), A.rlwinm(0, 0, 0, 27, 27), A.cmpwi(0, 0, 0))
    la(6, "s_isfile")
    p.branch(lambda at, t: A.bc(at, 12, 2, t), "notdir")  # beq: a file
    la(6, "s_isdir")
    p.label("notdir")
    la(3, "s_entry")
    call("DbgPrint")
    p.branch(A.b, "dirloop")
    p.label("dirdone")
    p(A.mr(4, 3))
    la(3, "s_dirend")
    call("DbgPrint")
    p(A.lwz(3, 0x78, 1))
    call("NtClose")
    # a switch through a jump table
    p(A.li(3, 0))
    p.branch(A.bl, "sw")
    p(A.mr(25, 3), A.li(3, 2))
    p.branch(A.bl, "sw")
    p(A.mr(24, 3), A.li(3, 9))
    p.branch(A.bl, "sw")
    p(A.mr(6, 3), A.mr(4, 25), A.mr(5, 24))
    la(3, "s_sw")
    call("DbgPrint")
    p(A.li(4, 0))
    la(3, "s_end")
    call("DbgPrint")
    p(A.li(3, 0), A.addi(1, 1, 0xC0))
    p.branch(A.b, "__restgprlr_24")
    p.end()

    # the second thread: r3 = the shared block
    p.func("worker")
    p(A.mflr(12))
    p.branch(A.bl, "__savegprlr_30")
    p(stwu(1, -0x60, 1), A.mr(31, 3))
    la(4, "s_arg")
    la(3, "s_worker")
    call("DbgPrint")
    p(A.li(4, 4321), A.stw(4, 0, 31), A.lwz(3, 4, 31), A.li(4, 0))
    call("NtSetEvent")
    p(A.li(3, 7), A.addi(1, 1, 0x60))
    p.branch(A.b, "__restgprlr_30")
    p.end()

    # sw(i): a jump table, MSVC's absolute form
    p.func("sw")
    p(cmplwi(6, 3, 3))
    p.branch(lambda at, t: A.bc(at, 12, 25, t), "sw_def")
    p.branch(lambda at, t: A.lis(12, ha(t)), "sw_tbl")
    p.branch(lambda at, t: A.addi(12, 12, lo(t)), "sw_tbl")
    p(A.rlwinm(0, 3, 2, 0, 29), A.lwzx(0, 12, 0), A.mtctr(0), A.bctr())
    p.label("sw_tbl")
    for k in range(4):
        p.branch(lambda at, t: t, "sw_%d" % k)
    for k, v in enumerate((10, 20, 30, 40)):
        p.label("sw_%d" % k)
        p(A.li(3, v), A.blr())
    p.label("sw_def")
    p(A.li(3, -1), A.blr())
    p.end()
    words = p.link()
    funcs = [(a, b) for _, a, b in p.funcs]
    return words, funcs, p.labels


def layout(n_words):
    """Addresses of the strings, the data and the imports once .text is n_words long (synth.py's layout)."""
    imports = {"xboxkrnl.exe": [(ordinal("xboxkrnl", n), "thunk") for n in KRNL] +
               [(ordinal("xboxkrnl", n), "variable") for n in VARS],
               "xam.xex": [(ordinal("xam", n), "thunk") for n in XAM]}
    addr = {}
    thunk = TEXT + 4 * n_words
    for lib, imps in imports.items():
        for o, kind in imps:
            if kind == "thunk":
                addr[(lib, o)] = thunk
                thunk += 16
    text_size = thunk - TEXT
    rdata_rva = PAGE + ((text_size + PAGE - 1) & ~(PAGE - 1))
    rdata = b""
    for name, s in STRINGS:
        addr[name] = BASE + rdata_rva + len(rdata)
        rdata += s.encode() + b"\0"
        rdata += bytes(-len(rdata) % 4)
    rec = BASE + rdata_rva + len(rdata)
    for lib, imps in imports.items():
        for o, kind in imps:
            addr[("rec", lib, o)] = rec
            rec += 4
    n_records = sum(len(v) for v in imports.values())
    data_rva = rdata_rva + ((len(rdata) + 4 * n_records + PAGE - 1) & ~(PAGE - 1))
    addr["cs"] = BASE + data_rva
    addr["shared"] = BASE + data_rva + 0x40
    addr["filebuf"] = BASE + data_rva + 0x80
    addr["dirbuf"] = BASE + data_rva + 0x100
    data = bytes(0x200)
    for n in KRNL:
        addr[n] = addr[("xboxkrnl.exe", ordinal("xboxkrnl", n))]
    for n in VARS:
        addr["rec_" + n] = addr[("rec", "xboxkrnl.exe", ordinal("xboxkrnl", n))]
    return imports, rdata, data, addr


def build(compression="basic", key=None):
    words, _, _ = program(layout(0)[3] | {"rec_XboxHardwareInfo": 0})  # pass 1: the size of .text
    imports, rdata, data, addr = layout(len(words))
    words, funcs, labels = program(addr)
    assert len(words) == len(program(layout(len(words))[3])[0])
    pdata = [(TEXT + 4 * a - TEXT, 4 * (b - a)) for a, b in funcs]
    xex, lay = build_xex(words, rdata=rdata, data=data, imports=imports, entry=4 * labels_index(labels, "start"),
                         functions=pdata, compression=compression, key=key, name="hello.exe")
    for n in KRNL:
        assert lay.thunks[("xboxkrnl.exe", ordinal("xboxkrnl", n))] == addr[n], n
    assert lay.records[("xboxkrnl.exe", ordinal("xboxkrnl", "XboxHardwareInfo"))] == addr["rec_XboxHardwareInfo"]
    assert lay.data == addr["cs"]
    return xex


def labels_index(labels, name):
    return (labels[name] - TEXT) // 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--compression", default="basic", choices=("none", "basic", "normal"))
    ap.add_argument("--key-file")
    a = ap.parse_args()
    key = None
    if a.key_file:
        with open(a.key_file, "rb") as f:
            key = f.read()[:16]
    with open(a.out, "wb") as f:
        f.write(build(a.compression, key))


if __name__ == "__main__":
    main()
