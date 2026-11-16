"""A Dreamcast program as the lifter sees it (XBA-227): an SH ELF or a raw, unscrambled 1ST_READ.BIN.

ELF: PT_LOAD segments, the entry point, code ranges from the executable sections (.text, .init, .fini) and
function starts from the symbol table (FUNC symbols and global labels in code). Raw binary: loaded at 0x8C010000,
one code range, entry at its start, no symbols (the lifter falls back on a scan for code pointers in it).
Addresses are P1 (0x8C......), as the program runs. No commercial disc data is read or stored by these tools:
they take the file they are given.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field

BIN_BASE = 0x8C010000


@dataclass
class Segment:
    vaddr: int
    data: bytes
    executable: bool


@dataclass
class Image:
    segments: list[Segment]
    entry: int
    code_ranges: list[tuple[int, int]]
    data_ranges: list[tuple[int, int]]
    symbols: dict[int, str] = field(default_factory=dict)
    sha256: str = ""
    kind: str = "elf"

    def _find(self, addr: int, size: int) -> bytes | None:
        for s in self.segments:
            if s.vaddr <= addr and addr + size <= s.vaddr + len(s.data):
                o = addr - s.vaddr
                return s.data[o : o + size]
        return None

    def read16(self, addr: int) -> int | None:
        b = self._find(addr, 2)
        return None if b is None else struct.unpack_from("<H", b)[0]

    def read32(self, addr: int) -> int | None:
        b = self._find(addr, 4)
        return None if b is None else struct.unpack_from("<I", b)[0]

    def read8(self, addr: int) -> int | None:
        b = self._find(addr, 1)
        return None if b is None else b[0]

    def is_code(self, addr: int) -> bool:
        return any(lo <= addr < hi for lo, hi in self.code_ranges)

    def spans(self) -> list[tuple[int, int]]:
        """The bytes the lifted code depends on: the executable segments as loaded."""
        return [(s.vaddr, s.vaddr + len(s.data)) for s in self.segments if s.executable and s.data]

    def span_bytes(self, lo: int, hi: int) -> bytes:
        b = self._find(lo, hi - lo)
        assert b is not None
        return b


def _p1(addr: int) -> int:
    return (addr & 0x1FFFFFFF) | 0x80000000


def load_elf(data: bytes) -> Image:
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("not a 32-bit little-endian ELF")
    (e_type, e_machine, _ver, e_entry, e_phoff, e_shoff, _flags, _ehsize, e_phentsize, e_phnum, e_shentsize,
     e_shnum, e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    if e_machine != 42:
        raise ValueError("not an SH ELF (e_machine %d)" % e_machine)
    segs: list[Segment] = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, p_flags, _align = struct.unpack_from(
            "<IIIIIIII", data, e_phoff + i * e_phentsize)
        if p_type != 1 or p_memsz == 0:
            continue
        body = data[p_offset : p_offset + p_filesz] + b"\0" * (p_memsz - p_filesz)
        segs.append(Segment(_p1(p_vaddr), body, bool(p_flags & 1)))
    sections = []
    for i in range(e_shnum):
        sh = struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize)
        sections.append(sh)

    def name_at(off: int, base: int) -> str:
        end = data.index(b"\0", base + off)
        return data[base + off : end].decode("latin-1")

    code: list[tuple[int, int]] = []
    dat: list[tuple[int, int]] = []
    symtab = None
    for sh in sections:
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link = sh[:7]
        if sh_type == 2:  # SHT_SYMTAB
            symtab = sh
        if not (sh_flags & 2) or sh_size == 0 or not (0x0C000000 <= (sh_addr & 0x1FFFFFFF) < 0x10000000):
            continue
        rng = (_p1(sh_addr), _p1(sh_addr) + sh_size)
        if sh_flags & 4:  # SHF_EXECINSTR
            code.append(rng)
        elif sh_type == 1:  # PROGBITS data: scanned for code pointers
            dat.append(rng)
    syms: dict[int, str] = {}
    if symtab is not None:
        _n, _t, _f, _a, off, size, link, _info, _al, entsize = symtab
        str_base = sections[link][4]
        for k in range(size // (entsize or 16)):
            st_name, st_value, _st_size, st_info, _other, st_shndx = struct.unpack_from(
                "<IIIBBH", data, off + k * (entsize or 16))
            stype, bind = st_info & 15, st_info >> 4
            if st_shndx == 0 or st_shndx >= 0xFF00 or st_value & 1:
                continue
            addr = _p1(st_value)
            if not any(lo <= addr < hi for lo, hi in code):
                continue
            # FUNC symbols, and global labels of assembly files (no type): function starts.
            if stype == 2 or (stype == 0 and bind == 1):
                syms.setdefault(addr, name_at(st_name, str_base))
    if not code:
        code = [(s.vaddr, s.vaddr + len(s.data)) for s in segs if s.executable]
    return Image(segs, _p1(e_entry), sorted(code), sorted(dat), syms, hashlib.sha256(data).hexdigest(), "elf")


def load_bin(data: bytes, base: int = BIN_BASE) -> Image:
    seg = Segment(base, bytes(data), True)
    rng = (base, base + len(data))
    return Image([seg], base, [rng], [rng], {}, hashlib.sha256(data).hexdigest(), "bin")


def load(path: str) -> Image:
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == b"\x7fELF":
        return load_elf(data)
    return load_bin(data)
