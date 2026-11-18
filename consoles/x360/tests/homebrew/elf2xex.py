"""elf2xex.py PROGRAM.elf OUT.xex -- a test title linked by x360.ld into a synthetic XEX2 (XBA-270).

The ELF (big-endian, 32-bit: devkitPPC) holds one blob from 0x82001000 (.text, .rodata, .data with its bss) and the
import names placed on thunks after it (`__thunks` + 16 k, in KERNEL's order). tools/xex/synth.py takes the blob as
its .text, appends the thunks at the same addresses (checked), gives them the real ordinals, and writes .pdata from
the ELF's function symbols (what MSVC's .pdata gives discovery on a real title). Our own reader of ELF headers."""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [os.path.normpath(os.path.join(HERE, "..", "..", "tools")),
                os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))]

from tools.xex.synth import BASE, PAGE, build_xex  # noqa: E402
from x360_tools.exports import ordinal  # noqa: E402

# the order of x360.ld
KERNEL = ["DbgPrint", "NtAllocateVirtualMemory", "NtFreeVirtualMemory", "RtlInitializeCriticalSection",
          "RtlEnterCriticalSection", "RtlLeaveCriticalSection", "NtCreateEvent", "NtSetEvent", "NtWaitForSingleObjectEx",
          "NtWaitForMultipleObjectsEx", "ExCreateThread", "NtClose", "KeDelayExecutionThread", "NtCreateSemaphore",
          "NtReleaseSemaphore", "MmAllocatePhysicalMemoryEx", "KeTlsAlloc", "KeTlsSetValue", "KeTlsGetValue", "sprintf"]
TEXT = BASE + PAGE


def read_elf(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 2:
        raise ValueError("not a 32-bit big-endian ELF")
    entry, _phoff, shoff = struct.unpack_from(">III", data, 24)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", data, 46)
    secs = []
    for k in range(shnum):
        secs.append(struct.unpack_from(">IIIIIIIIII", data, shoff + k * shentsize))
    names = secs[shstrndx]

    def name(off, strtab):
        e = data.index(b"\0", strtab[4] + off)
        return data[strtab[4] + off:e].decode()

    sections = {name(s[0], names): s for s in secs}
    symtab = sections[".symtab"]
    strtab = secs[symtab[6]]
    syms = {}
    funcs = []
    for o in range(symtab[4], symtab[4] + symtab[5], 16):
        st_name, value, size, info, _other, shndx = struct.unpack_from(">IIIBBH", data, o)
        n = name(st_name, strtab)
        if n:
            syms[n] = value
        if info & 15 == 2 and size:  # STT_FUNC
            funcs.append((value, size, n))
    return entry, sections, syms, sorted(funcs)


def convert(elf):
    entry, sections, syms, funcs = read_elf(elf)
    end = syms["__thunks"]
    blob = bytearray(end - TEXT)
    for sname in (".text", ".rodata", ".data"):
        s = sections.get(sname)
        if not s:
            continue
        sh_type, addr, off, size = s[1], s[3], s[4], s[5]
        if sh_type == 8:  # NOBITS
            continue
        blob[addr - TEXT:addr - TEXT + size] = elf[off:off + size]
    for k, n in enumerate(KERNEL):
        if syms.get(n) != end + 16 * k:
            raise ValueError("%s is not on thunk %d: x360.ld and KERNEL disagree" % (n, k))
    imports = {"xboxkrnl.exe": [(ordinal("xboxkrnl", n), "thunk") for n in KERNEL]}
    pdata = [(a - TEXT, (size + 3) & ~3) for a, size, _ in funcs]
    xex, lay = build_xex(bytes(blob), imports=imports, entry=entry - TEXT, functions=pdata, compression="basic",
                         name=os.path.basename("hello_c.exe"))
    for k, n in enumerate(KERNEL):
        assert lay.thunks[("xboxkrnl.exe", ordinal("xboxkrnl", n))] == end + 16 * k, n
    return xex


def main():
    with open(sys.argv[1], "rb") as f:
        xex = convert(f.read())
    with open(sys.argv[2], "wb") as f:
        f.write(xex)


if __name__ == "__main__":
    main()
