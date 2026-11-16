"""The 3DSX format (3dbrew « 3DSX Format ») and the code image the recompiler lifts (XBA-228).

A .3dsx is what devkitARM's 3dsxtool makes of a libctru ELF and what the Homebrew Launcher loads: a header, three
relocation headers, the code / rodata / data segments (data without its .bss), then per segment its absolute and
relative relocations as runs of {u16 skip, u16 patch} words. The loader places the segments at 0x00100000, each
page-aligned after the previous one, and patches every listed word: it holds an address in the file's own space (code
at 0, rodata and data after it, page-aligned), which becomes 0x00100000 + that address, or that minus the word's own
address for a relative one (kind 0: 32-bit, kind 1: 31-bit). consoles/3ds/runtime/ctr_loader.c does the same at run
time; the lifted image must be byte-identical to what it loads (the core checks a hash).

The relocations are a gift to discovery: they name every absolute pointer of the program. Those that point into the
code segment are function pointers or jump targets (C function tables, init arrays, callbacks handed to threads),
which a symbol-less recursive descent would otherwise only meet at run time.
"""

import hashlib
import struct

BASE = 0x00100000
PAGE = 0x1000


class ThreeDsxError(ValueError):
    pass


def page(v):
    return (v + PAGE - 1) & ~(PAGE - 1)


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
    """The program as loaded and relocated. Duck-types gba_tools.rom.Rom / nds_tools.nds.Image for Discovery and the
    lifter (contains / u8 / u16 / u32 / segments / seeds / sha256): `segments` is the code segment only (what may be
    lifted and where literals fold from: it is read-only at run time); rodata and data are `data_segments`."""

    def __init__(self, raw: bytes):
        if len(raw) < 0x20 or raw[:4] != b"3DSX":
            raise ThreeDsxError("not a 3DSX (magic)")
        hdr_size, rel_size, self.version, self.flags = struct.unpack_from("<HHII", raw, 4)
        code, ro, data, bss = struct.unpack_from("<IIII", raw, 0x10)
        if hdr_size < 0x20 or rel_size < 8 or bss > data or code == 0:
            raise ThreeDsxError("bad 3DSX header (header %d, relocation header %d, bss %d > data %d)"
                                % (hdr_size, rel_size, bss, data))
        self.sha256 = hashlib.sha256(raw).hexdigest()
        self.sizes = {"code": code, "rodata": ro, "data": data, "bss": bss}
        at = hdr_size
        nrel = []
        for _ in range(3):
            if at + rel_size > len(raw):
                raise ThreeDsxError("truncated in the relocation headers")
            nrel.append(struct.unpack_from("<II", raw, at))
            at += rel_size
        file_size = [code, ro, data - bss]
        mem_size = [page(code), page(ro), page(data)]
        addrs = [BASE, BASE + mem_size[0], BASE + mem_size[0] + mem_size[1]]
        mem = [bytearray(n) for n in mem_size]
        for s in range(3):
            if at + file_size[s] > len(raw):
                raise ThreeDsxError("truncated in segment %d" % s)
            mem[s][:file_size[s]] = raw[at:at + file_size[s]]
            at += file_size[s]
        total = sum(mem_size)
        self.relocations = []  # (address of the word, value after relocation, absolute?)
        for s in range(3):
            for t in range(2):
                pos = 0
                for _ in range(nrel[s][t]):
                    if at + 4 > len(raw):
                        raise ThreeDsxError("truncated in the relocations of segment %d" % s)
                    skip, patch = struct.unpack_from("<HH", raw, at)
                    at += 4
                    pos += skip * 4
                    for _ in range(patch):
                        if pos + 4 > mem_size[s]:
                            break
                        orig = struct.unpack_from("<I", mem[s], pos)[0]
                        kind, a = orig >> 28, orig & 0x0FFFFFFF
                        if a >= total:
                            raise ThreeDsxError("relocation at 0x%08X names 0x%X, outside the image"
                                                % (addrs[s] + pos, a))
                        addr = BASE + a
                        where = addrs[s] + pos
                        if t == 0:
                            if kind:
                                raise ThreeDsxError("absolute relocation of kind %d at 0x%08X" % (kind, where))
                            v = addr
                        else:
                            if kind > 1:
                                raise ThreeDsxError("relative relocation of kind %d at 0x%08X" % (kind, where))
                            v = (addr - where) & 0xFFFFFFFF
                            if kind:
                                v &= 0x7FFFFFFF
                        struct.pack_into("<I", mem[s], pos, v)
                        self.relocations.append((where, v, t == 0))
                        pos += 4
        self.segments = [Segment(addrs[0], mem[0][:code], "code")]
        self.data_segments = [Segment(addrs[1], mem[1][:ro], "rodata"), Segment(addrs[2], mem[2], "data")]
        self.entry = BASE
        self.seeds = [(BASE, False)]

    @classmethod
    def load(cls, path: str) -> "Image":
        with open(path, "rb") as f:
            return cls(f.read())

    # ---- the code image ---------------------------------------------------------------------------------------

    @property
    def code(self):
        return self.segments[0]

    def segment_of(self, addr):
        s = self.code
        return s if s.vma <= addr < s.end else None

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
        return struct.unpack_from("<H", self.code.data, a - self.code.vma)[0]

    def u32(self, addr: int):
        a = addr & ~3
        if not self.contains(a, 4):
            return None
        return struct.unpack_from("<I", self.code.data, a - self.code.vma)[0]

    def code_pointers(self):
        """Absolute relocated words that point into the code segment: (where, target, thumb)."""
        out = []
        for (where, v, absolute) in self.relocations:
            if absolute and self.contains(v & ~1, 2):
                out.append((where, v & ~1, bool(v & 1)))
        return out

    def image_hash(self):
        """(lo, hi, FNV-1a) of the code segment as loaded: what ctr_attach_recomp checks."""
        return self.code.vma, self.code.end, fnv1a(self.code.data)

    def as_dict(self) -> dict:
        segs = [self.code] + self.data_segments
        return {"loader": "3dsx", "sizes": self.sizes, "relocations": len(self.relocations),
                "segments": [{"vma": "0x%08X" % s.vma, "size": len(s.data), "what": s.what} for s in segs],
                "seeds": ["0x%08X%s" % (a, "t" if t else "a") for (a, t) in self.seeds]}
