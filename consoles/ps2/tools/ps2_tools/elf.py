"""EE executable loading (XBA-226), as consoles/ps2/runtime/ps2_load.c does it, byte for byte (lifted code is
checked against the loaded bytes): a little-endian ELF32 for MIPS, statically linked (PS2SDK links at 0x00100000,
no relocations). PT_LOAD segments are placed at their virtual addresses (file bytes, then zeros to the memory
size); the code is the span of the SHF_EXECINSTR sections, or of the executable segments without a section table.
"""

import hashlib
import struct


class Ps2Error(Exception):
    pass


class Executable:
    def __init__(self, path, data):
        self.path = path
        self.sha256 = hashlib.sha256(data).hexdigest()
        if len(data) < 52 or data[:4] != b"\x7fELF":
            raise Ps2Error("not an ELF file")
        if data[4] != 1 or data[5] != 1 or struct.unpack_from("<H", data, 18)[0] != 8:
            raise Ps2Error("not a little-endian 32-bit MIPS ELF")
        if struct.unpack_from("<H", data, 16)[0] != 2:
            raise Ps2Error("not an executable ELF (IRX / relocatable modules are not loaded)")
        self.entry, phoff, shoff = struct.unpack_from("<III", data, 24)
        phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", data, 42)
        segs = []
        xlo, xhi = 0xFFFFFFFF, 0
        for k in range(phnum):
            o = phoff + k * phentsize
            if o + 32 > len(data):
                break
            ptype, off, vaddr, _paddr, filesz, memsz, flags = struct.unpack_from("<7I", data, o)
            if ptype != 1:
                continue
            if filesz > memsz or off + filesz > len(data):
                raise Ps2Error("segment %d: bad sizes" % k)
            segs.append((vaddr, data[off:off + filesz] + bytes(memsz - filesz)))
            if flags & 1 and memsz:
                xlo, xhi = min(xlo, vaddr), max(xhi, vaddr + memsz)
        if not segs:
            raise Ps2Error("no PT_LOAD segment")
        slo, shi = 0xFFFFFFFF, 0
        for k in range(shnum if shoff else 0):
            o = shoff + k * shentsize
            if o + 40 > len(data):
                break
            _name, _type, flags, addr, _off, size = struct.unpack_from("<6I", data, o)
            if flags & 0x4 and flags & 0x2 and size:
                slo, shi = min(slo, addr), max(shi, addr + size)
        if shi > slo:
            xlo, xhi = slo, shi
        if xhi <= xlo:
            raise Ps2Error("no code (no executable section or segment)")
        self.code_lo, self.code_hi = xlo, xhi
        lo = min(v for v, _ in segs)
        hi = max(v + len(b) for v, b in segs)
        img = bytearray(hi - lo)
        for v, b in segs:
            img[v - lo:v - lo + len(b)] = b
        self.base = lo
        self.image = bytes(img)

    def code(self):
        """The code's bytes, at code_lo."""
        return self.image[self.code_lo - self.base:self.code_hi - self.base]

    def words(self):
        """(address, word) of every aligned word of the loaded image (code pointers live in data)."""
        n = len(self.image) // 4
        return zip(range(self.base, self.base + 4 * n, 4), struct.unpack_from("<%dI" % n, self.image, 0))

    def as_dict(self):
        return {"entry": "0x%08X" % self.entry, "code": ["0x%08X" % self.code_lo, "0x%08X" % self.code_hi],
                "image": ["0x%08X" % self.base, "0x%08X" % (self.base + len(self.image))]}


def load_file(path):
    with open(path, "rb") as f:
        return Executable(path, f.read())
