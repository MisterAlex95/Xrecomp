""".nds header (GBATEK « DS Cartridge Header ») and the ARM9 code image the recompiler lifts (XBA-223).

A GBA cartridge is code at fixed addresses, read-only. A DS program is not: the header names the ARM9 binary's ROM
offset, RAM address and entry; the boot loads it into main RAM, and the program's own start-up code then moves its
sections to where they run. With devkitPro's calico (libnds 2), the ARM9 entry is a position-independent boot stub
(`__ds9_bootstub`, magic "MOD9" at entry + 4) that copies the crt0 section to its address, and crt0 then walks a
load list: (start, end, bss end) triplets whose bytes follow each other in the binary from a cursor, copied over the
binary itself (.main runs below where it was loaded) and to ITCM (the exception vectors at 0, the kernel at
0x01FF8000). So the code the program runs is the image *after* those copies: `Image.load` applies them statically
and keeps only the run-time segments (the boot stub itself, overwritten by .main, stays interpreted).

A binary without that stub is taken as it is loaded (one segment at its RAM address).
"""

import hashlib
import struct

HEADER_SIZE = 0x200
MOD9 = 0x39444F4D           # "MOD9": calico's ARM9 boot stub
LIST_MAX = 64               # load list entries at most (calico writes a handful)


class NdsError(ValueError):
    pass


class Binary:
    """One CPU's binary as the header describes it."""

    def __init__(self, data: bytes, base: int):
        self.rom_offset, self.entry, self.ram, self.size = struct.unpack_from("<IIII", data, base)

    def as_dict(self) -> dict:
        return {"rom_offset": "0x%X" % self.rom_offset, "entry": "0x%08X" % self.entry,
                "ram": "0x%08X" % self.ram, "size": self.size}


class Header:
    def __init__(self, data: bytes):
        if len(data) < HEADER_SIZE:
            raise NdsError("file shorter than the 512-byte .nds header")
        self.title = data[0:12].split(b"\0")[0].decode("ascii", "replace")
        self.game_code = data[12:16].decode("ascii", "replace")
        self.maker = data[16:18].decode("ascii", "replace")
        self.unit = data[0x12]
        self.arm9 = Binary(data, 0x20)
        self.arm7 = Binary(data, 0x30)
        self.header_crc = struct.unpack_from("<H", data, 0x15E)[0]
        self.computed_crc = crc16(data[:0x15E])
        for name, b in (("ARM9", self.arm9), ("ARM7", self.arm7)):
            if b.rom_offset + b.size > len(data) or b.size == 0:
                raise NdsError("%s binary (offset 0x%X, size 0x%X) outside the file" % (name, b.rom_offset, b.size))

    def as_dict(self) -> dict:
        return {"title": self.title, "game_code": self.game_code, "maker": self.maker, "unit": self.unit,
                "header_crc_ok": self.header_crc == self.computed_crc,
                "arm9": self.arm9.as_dict(), "arm7": self.arm7.as_dict()}


def crc16(data: bytes) -> int:
    """The header's CRC-16 (MODBUS polynomial 0xA001, initial 0xFFFF)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def fnv1a(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


class Segment:
    def __init__(self, vma: int, data: bytes, what: str):
        self.vma = vma
        self.data = bytes(data)
        self.what = what

    @property
    def end(self):
        return self.vma + len(self.data)


class Image:
    """The ARM9's code as it runs: segments at their run addresses. Duck-types gba_tools.rom.Rom for Discovery and
    the lifter (contains / u8 / u16 / u32 / sha256)."""

    def __init__(self, nds: bytes):
        self.header = Header(nds)
        self.sha256 = hashlib.sha256(nds).hexdigest()
        a9 = self.header.arm9
        self.binary = nds[a9.rom_offset:a9.rom_offset + a9.size]
        self.segments = []
        self.seeds = []         # (addr, thumb) entries the boot hands over to
        self.loader = "plain"
        if not self._calico():
            self.segments = [Segment(a9.ram, self.binary, "arm9")]
            self.seeds = [(a9.entry & ~1, bool(a9.entry & 1))]
        self.segments.sort(key=lambda s: s.vma)

    @classmethod
    def load(cls, path: str) -> "Image":
        with open(path, "rb") as f:
            return cls(f.read())

    # ---- the binary as loaded (before the boot stub runs) -------------------------------------------------------

    def _bin32(self, addr):
        off = addr - self.header.arm9.ram
        if off < 0 or off + 4 > len(self.binary):
            return None
        return struct.unpack_from("<I", self.binary, off)[0]

    def _bin(self, addr, n):
        off = addr - self.header.arm9.ram
        if off < 0 or off + n > len(self.binary):
            raise NdsError("range 0x%08X+0x%X outside the ARM9 binary" % (addr, n))
        return self.binary[off:off + n]

    def _calico(self) -> bool:
        """calico's MOD9 boot stub: its crt0 copy and load list applied, the run-time segments kept."""
        stub = self.header.arm9.entry & ~1
        if self._bin32(stub + 4) != MOD9:
            return False
        words = [self._bin32(stub + 4 * i) for i in range(9)]
        if None in words:
            raise NdsError("truncated MOD9 header")
        cursor, lst, lst_end = words[3], words[4], words[5]
        # The crt0 copy: `add ip, pc, #imm ; ldm ip, {r0, r1, r2}` names (source, destination, destination end).
        crt0 = None
        for i in range(9, 0x80):
            op, nxt = self._bin32(stub + 4 * i), self._bin32(stub + 4 * i + 4)
            if op is None or nxt is None:
                break
            if (op & 0xFFFFF000) == 0xE28FC000 and nxt == 0xE89C0007:
                at = stub + 4 * i + 8 + (op & 0xFF)
                crt0 = (self._bin32(at), self._bin32(at + 4), self._bin32(at + 8))
                break
        if crt0 is None or None in crt0 or crt0[2] < crt0[1]:
            raise NdsError("MOD9 boot stub without its crt0 copy")
        src, dst, end = crt0
        self.segments.append(Segment(dst, self._bin(src, end - dst), "crt0"))
        if (lst_end - lst) % 12 or (lst_end - lst) // 12 > LIST_MAX:
            raise NdsError("odd MOD9 load list 0x%08X..0x%08X" % (lst, lst_end))
        for k in range((lst_end - lst) // 12):
            start, stop, _bss = struct.unpack("<III", self._bin(lst + 12 * k, 12))
            if stop > start:
                self.segments.append(Segment(start, self._bin(cursor, stop - start), "load list %d" % k))
            cursor += stop - start
        # Where the stub hands over: the constants it jumps to (`ldr pc, =x`) or returns to (`ldr lr, =x`).
        for i in range(9, 0x80):
            op = self._bin32(stub + 4 * i)
            if op is None:
                break
            if (op & 0xFFFFE000) == 0xE59FE000 and ((op >> 12) & 15) in (14, 15):
                v = self._bin32(stub + 4 * i + 8 + (op & 0xFFF))
                if v is not None and self.contains(v & ~1):
                    self.seeds.append((v & ~1, bool(v & 1)))
        # The exception vectors, when a segment lands at 0 (ITCM).
        for s in self.segments:
            if s.vma == 0:
                self.seeds += [(4 * i, False) for i in range(min(8, len(s.data) // 4))]
        self.loader = "calico"
        return True

    # ---- the run-time image ------------------------------------------------------------------------------------

    def segment_of(self, addr):
        for s in self.segments:
            if s.vma <= addr < s.end:
                return s
        return None

    def contains(self, addr: int, size: int = 1) -> bool:
        s = self.segment_of(addr)
        return s is not None and addr + size <= s.end

    def u8(self, addr: int):
        s = self.segment_of(addr)
        return None if s is None else s.data[addr - s.vma]

    def u16(self, addr: int):
        a = addr & ~1
        if not self.contains(a, 2):
            return None
        s = self.segment_of(a)
        return struct.unpack_from("<H", s.data, a - s.vma)[0]

    def u32(self, addr: int):
        a = addr & ~3
        if not self.contains(a, 4):
            return None
        s = self.segment_of(a)
        return struct.unpack_from("<I", s.data, a - s.vma)[0]

    def span_hash(self, lo: int, hi: int) -> int:
        s = self.segment_of(lo)
        return fnv1a(s.data[lo - s.vma:hi - s.vma])

    def as_dict(self) -> dict:
        return {"loader": self.loader,
                "segments": [{"vma": "0x%08X" % s.vma, "size": len(s.data), "what": s.what} for s in self.segments],
                "seeds": ["0x%08X%s" % (a, "t" if t else "a") for (a, t) in self.seeds]}
