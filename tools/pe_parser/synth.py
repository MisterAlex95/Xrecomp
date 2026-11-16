"""
Synthetic Windows PEs for tests (XBA-233): our own writer, no third-party binary.

    build_pe(code_fn, imports={"KERNEL32.dll": ["ExitProcess", ...], "WS2_32.dll": [115]})

The layout is fixed, one page per section, so the code can name every address
before it is assembled:

    RVA 0x1000 .text   code (code_fn's bytes)                     R X
    RVA 0x2000 .rdata  read-only data (`rdata`)                    R
    RVA 0x3000 .data   initialized data, then zero fill (`bss`)    R W
    RVA 0x4000 .idata  import descriptors, ILTs, IATs, hint/names  R
    RVA 0x5000 .reloc  base relocations (HIGHLOW)                  R

code_fn(layout) returns (code bytes, offsets in .text of the 32-bit words a
rebase fixes up); `layout` has .base, .text, .rdata, .data (VAs) and .iat
({import name or ordinal: VA of its IAT slot}).

hello_pe() is a console hello world in this format: GetStdHandle, WriteFile
of a message in .rdata (bytes written into .data), ExitProcess(0), or 3 when
the write came up short. tools/win32's end-to-end test lifts and runs it.
"""

import struct
from types import SimpleNamespace

BASE = 0x00400000
TEXT, RDATA, DATA, IDATA, RELOC = 0x1000, 0x2000, 0x3000, 0x4000, 0x5000
FILE_ALIGN, HEADERS = 0x200, 0x400

SCN_CODE, SCN_IDATA, SCN_UDATA = 0x20, 0x40, 0x80
SCN_EXEC, SCN_READ, SCN_WRITE, SCN_DISCARD = 0x20000000, 0x40000000, 0x80000000, 0x02000000

HELLO_MESSAGE = b"Hello, world! (a synthetic Win32 PE recompiled by XRecomp)\n"


def _align(n, a):
    return (n + a - 1) & ~(a - 1)


def _idata(imports, rva):
    """The .idata bytes for {dll: [name or ordinal]}, and {name or ordinal: IAT slot RVA}, and the IAT range."""
    dlls = list(imports.items())
    desc_size = 20 * (len(dlls) + 1)
    off = desc_size
    thunks = []                         # (dll index, ilt off, iat off)
    for _, names in dlls:
        n = 4 * (len(names) + 1)
        thunks.append((off, off + n))
        off += 2 * n
    iat_lo, iat_hi = thunks[0][1] if thunks else 0, off
    hint_names = {}
    blob = bytearray()
    for _, names in dlls:
        for name in names:
            if isinstance(name, str):
                hint_names[name] = off + len(blob)
                entry = struct.pack("<H", len(hint_names)) + name.encode() + b"\0"
                blob += entry + (b"\0" if len(entry) & 1 else b"")
    dll_names = []
    for dll, _ in dlls:
        dll_names.append(off + len(blob))
        blob += dll.encode() + b"\0"
    data = bytearray(off) + blob
    slots = {}
    for i, ((dll, names), (ilt, iat)) in enumerate(zip(dlls, thunks)):
        struct.pack_into("<IIIII", data, 20 * i, rva + ilt, 0, 0, rva + dll_names[i], rva + iat)
        for j, name in enumerate(names):
            word = (0x80000000 | name) if isinstance(name, int) else rva + hint_names[name]
            struct.pack_into("<I", data, ilt + 4 * j, word)
            struct.pack_into("<I", data, iat + 4 * j, word)
            slots[name] = rva + iat + 4 * j
    return bytes(data), slots, (rva + iat_lo, iat_hi - iat_lo)


def _reloc(rvas):
    """The .reloc bytes: one block per page, HIGHLOW entries, padded to 4 bytes."""
    out = bytearray()
    pages = {}
    for r in sorted(rvas):
        pages.setdefault(r & ~0xFFF, []).append(r & 0xFFF)
    for page, offs in sorted(pages.items()):
        entries = [(3 << 12) | o for o in offs]
        if len(entries) & 1:
            entries.append(0)           # IMAGE_REL_BASED_ABSOLUTE padding
        out += struct.pack("<II", page, 8 + 2 * len(entries)) + struct.pack("<%dH" % len(entries), *entries)
    return bytes(out)


def build_pe(code_fn, imports=None, rdata=b"", data=b"\0" * 4, bss=0x100, base=BASE, entry_offset=0,
             subsystem=3):
    """The PE's bytes (see the module docstring)."""
    imports = imports if imports is not None else {"KERNEL32.dll": ["ExitProcess"]}
    idata, slots, (iat_rva, iat_size) = _idata(imports, IDATA)
    layout = SimpleNamespace(base=base, text=base + TEXT, rdata=base + RDATA, data=base + DATA,
                             iat={k: base + v for k, v in slots.items()})
    code, fixups = code_fn(layout)
    reloc = _reloc([TEXT + o for o in fixups])
    for blob, what in ((code, ".text"), (rdata, ".rdata"), (data, ".data"), (idata, ".idata"), (reloc, ".reloc")):
        if len(blob) > 0x1000:
            raise ValueError(f"{what}: {len(blob)} bytes, one page at most")

    # (name, rva, vsize, bytes, characteristics)
    secs = [
        (".text", TEXT, len(code), code, SCN_CODE | SCN_EXEC | SCN_READ),
        (".rdata", RDATA, max(len(rdata), 1), rdata, SCN_IDATA | SCN_READ),
        (".data", DATA, len(data) + bss, data, SCN_IDATA | SCN_READ | SCN_WRITE),
        (".idata", IDATA, len(idata), idata, SCN_IDATA | SCN_READ),
        (".reloc", RELOC, max(len(reloc), 1), reloc, SCN_IDATA | SCN_READ | SCN_DISCARD),
    ]
    image_size = RELOC + 0x1000

    out = bytearray(HEADERS)
    out[0:2] = b"MZ"
    struct.pack_into("<I", out, 0x3C, 0x40)
    out[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", out, 0x44, 0x014C, len(secs), 0, 0, 0, 0xE0, 0x0102)
    dirs = [(0, 0)] * 16
    dirs[1] = (IDATA, 20 * (len(imports) + 1))
    dirs[5] = (RELOC, len(reloc)) if reloc else (0, 0)
    dirs[12] = (iat_rva, iat_size)
    file_off = HEADERS
    headers = []
    for name, rva, vsize, blob, chars in secs:
        raw = _align(len(blob), FILE_ALIGN)
        headers.append(struct.pack("<8sIIIIIIHHI", name.encode(), vsize, rva, raw, file_off if raw else 0,
                                   0, 0, 0, 0, chars))
        file_off += raw
    opt = struct.pack("<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII",
                      0x10B, 2, 30, _align(len(code), FILE_ALIGN), 0x2000, 0, TEXT + entry_offset, TEXT, RDATA,
                      base, 0x1000, FILE_ALIGN, 4, 0, 0, 0, 4, 0, 0, image_size, HEADERS, 0, subsystem, 0x0140,
                      0x200000, 0x1000, 0x100000, 0x1000, 0, 16)
    opt += b"".join(struct.pack("<II", *d) for d in dirs)
    out[0x58:0x58 + len(opt)] = opt
    sh = 0x58 + len(opt)
    out[sh:sh + 40 * len(secs)] = b"".join(headers)
    for _, _, _, blob, _ in secs:
        out += blob + bytes(_align(len(blob), FILE_ALIGN) - len(blob))
    return bytes(out)


class Asm:
    """Just enough of an x86 assembler for hello_pe: bytes, 32-bit absolute words (relocated) and short jumps."""

    def __init__(self):
        self.code = bytearray()
        self.fixups = []
        self.labels = {}
        self.jumps = []                 # (offset of the rel8, label)

    def emit(self, *parts):
        for p in parts:
            self.code += bytes(p) if not isinstance(p, int) else bytes([p])
        return self

    def abs32(self, va):
        self.fixups.append(len(self.code))
        self.code += struct.pack("<I", va)
        return self

    def imm32(self, v):
        self.code += struct.pack("<I", v & 0xFFFFFFFF)
        return self

    def jcc8(self, opcode, label):
        self.code += bytes([opcode, 0])
        self.jumps.append((len(self.code) - 1, label))
        return self

    def label(self, name):
        self.labels[name] = len(self.code)
        return self

    def done(self):
        for at, name in self.jumps:
            rel = self.labels[name] - (at + 1)
            if not -128 <= rel < 128:
                raise ValueError(f"jump to {name} out of range")
            self.code[at] = rel & 0xFF
        return bytes(self.code), self.fixups


def hello_pe(message=HELLO_MESSAGE, extra_imports=None):
    """A console hello world: kernel32 only, by name. extra_imports: more DLLs, never called."""
    imports = {"KERNEL32.dll": ["ExitProcess", "GetStdHandle", "WriteFile"]}
    imports.update(extra_imports or {})

    def code(l):
        written = l.data            # DWORD in .data
        a = Asm()
        a.emit(0x6A, 0xF5)                                  # push -11 (STD_OUTPUT_HANDLE)
        a.emit(0xFF, 0x15).abs32(l.iat["GetStdHandle"])     # call [GetStdHandle]
        a.emit(0x6A, 0x00)                                  # push 0 (lpOverlapped)
        a.emit(0x68).abs32(written)                         # push &written
        a.emit(0x68).imm32(len(message))                    # push len
        a.emit(0x68).abs32(l.rdata)                         # push message
        a.emit(0x50)                                        # push eax (the handle)
        a.emit(0xFF, 0x15).abs32(l.iat["WriteFile"])        # call [WriteFile]
        a.emit(0x85, 0xC0)                                  # test eax, eax
        a.jcc8(0x74, "fail")                                # jz fail
        a.emit(0x81, 0x3D).abs32(written).imm32(len(message))   # cmp dword [written], len
        a.jcc8(0x75, "fail")                                # jne fail
        a.emit(0x6A, 0x00)                                  # push 0
        a.jcc8(0xEB, "exit")                                # jmp exit
        a.label("fail").emit(0x6A, 0x03)                    # push 3
        a.label("exit").emit(0xFF, 0x15).abs32(l.iat["ExitProcess"])   # call [ExitProcess]
        a.emit(0xCC)                                        # int3: never reached
        return a.done()

    return build_pe(code, imports=imports, rdata=message)
