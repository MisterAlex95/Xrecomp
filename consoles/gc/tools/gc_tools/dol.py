"""The DOL executable format (XBA-225): a 256-byte header, 7 text and 11 data sections, BSS, entry point.

Header, all big-endian 32-bit: file offsets of the 18 sections at 0x00, their load addresses at 0x48, their sizes
at 0x90, BSS address 0xD8 and size 0xDC, entry 0xE0. elf2dol (devkitPPC) writes it from an ELF; the IPL and
apploaders load it as is. `Image` is the program as loaded, with the interface tools.ppc.discover needs.
"""

import hashlib
import struct


class DolError(Exception):
    pass


class Section:
    __slots__ = ("index", "kind", "offset", "addr", "data")

    def __init__(self, index, kind, offset, addr, data):
        self.index, self.kind, self.offset, self.addr, self.data = index, kind, offset, addr, data

    @property
    def end(self):
        return self.addr + len(self.data)

    def as_dict(self):
        return {"kind": self.kind, "index": self.index, "addr": "0x%08X" % self.addr, "size": len(self.data)}


class Image:
    def __init__(self, raw):
        if len(raw) < 0x100:
            raise DolError("too small for a DOL header (%d bytes)" % len(raw))
        words = struct.unpack(">64I", raw[:0x100])
        self.raw = raw
        self.sha256 = hashlib.sha256(raw).hexdigest()
        self.sections = []
        for k in range(18):
            off, addr, size = words[k], words[18 + k], words[36 + k]
            if not size:
                continue
            if off + size > len(raw):
                raise DolError("section %d (0x%08X+0x%X) runs past the file" % (k, addr, size))
            if addr & 3 or (addr & 0x3FFFFFFF) + size > 0x01800000:
                raise DolError("section %d at 0x%08X+0x%X is not in MEM1" % (k, addr, size))
            kind = "text" if k < 7 else "data"
            self.sections.append(Section(k if k < 7 else k - 7, kind, off, addr, raw[off:off + size]))
        self.bss_addr, self.bss_size, self.entry = words[54], words[55], words[56]
        self.text = [s for s in self.sections if s.kind == "text"]
        if not self.text:
            raise DolError("no text section")
        if not self.in_code(self.entry):
            raise DolError("entry 0x%08X is not in a text section" % self.entry)

    @classmethod
    def load(cls, path):
        with open(path, "rb") as f:
            return cls(f.read())

    def section_at(self, addr):
        for s in self.sections:
            if s.addr <= addr < s.end:
                return s
        return None

    def in_code(self, addr):
        return any(s.addr <= addr and addr + 4 <= s.end for s in self.text)

    def u32(self, addr):
        s = self.section_at(addr)
        if s is None or addr + 4 > s.end:
            raise DolError("0x%08X is not loaded" % addr)
        return struct.unpack_from(">I", s.data, addr - s.addr)[0]

    def data_words(self):
        """(addr, word) of every aligned word of the data sections."""
        for s in self.sections:
            if s.kind != "data":
                continue
            for o in range(0, len(s.data) - 3, 4):
                yield s.addr + o, struct.unpack_from(">I", s.data, o)[0]

    def span_hash(self, lo, hi):
        """FNV-1a 32 of the loaded bytes lo..hi-1 (what the core checks before running lifted code)."""
        h = 0x811C9DC5
        s = self.section_at(lo)
        for b in s.data[lo - s.addr:hi - s.addr]:
            h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
        return h

    def as_dict(self):
        return {"entry": "0x%08X" % self.entry, "bss": "0x%08X+%d" % (self.bss_addr, self.bss_size),
                "sections": [s.as_dict() for s in self.sections]}


def build(texts, datas=(), entry=None, bss=(0, 0)):
    """A DOL from [(addr, bytes)] text and data sections (tests)."""
    texts, datas = list(texts), list(datas)
    if len(texts) > 7 or len(datas) > 11:
        raise DolError("at most 7 text and 11 data sections")
    offs, addrs, sizes = [0] * 18, [0] * 18, [0] * 18
    body = b""
    pos = 0x100
    for k, sec in enumerate(texts + [None] * (7 - len(texts)) + datas):
        if sec is None:
            continue
        addr, data = sec
        offs[k], addrs[k], sizes[k] = pos, addr, len(data)
        body += data
        pos += len(data)
    if entry is None:
        entry = texts[0][0]
    head = struct.pack(">18I18I18I3I", *offs, *addrs, *sizes, bss[0], bss[1], entry)
    return head + b"\0" * (0x100 - len(head)) + body
