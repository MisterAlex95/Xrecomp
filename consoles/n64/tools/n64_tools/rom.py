"""N64 ROM parsing (XBA-169): byte orders, the header, the CIC (from the IPL3, as n64_rom.c), and the code the boot
loads into RDRAM -- from the ROM alone, no symbol or ELF file.

Two boots exist:
- classic (libultra games): IPL3 copies the first megabyte after the header area (ROM 0x1000..0x101000) to the
  header's boot address (minus 0x100000 for a 6103 CIC, 0x200000 for a 6106) and jumps there;
- libdragon's open IPL3: the program is an ELF stored in the ROM after IPL3 (uncompressed: N64_ROM_ELFCOMPRESS=0);
  IPL3 finds it, copies its PT_LOAD segments to their virtual addresses and jumps to the ELF entry. Only the ELF and
  program headers are read here -- what IPL3 itself reads at run time -- never section headers or symbols (libdragon
  strips them from the ROM's copy anyway).
Either way the result is a list of Segments (virtual address, bytes, executable flag) and the boot PC.
"""

import hashlib
import struct
import zlib

Z64, V64, N64 = "z64", "v64", "n64"

# IPL3 CRC32 over 0x40..0xFFF -> CIC (n64_rom.c detect_cic, first table)
CIC_BY_CRC = {0x6170A4A1: "6101", 0x90BB6CB5: "6102", 0x0B050EE0: "6103", 0x98BC2C86: "6105",
              0xACC8580A: "6106", 0x009E9EA3: "7102"}
CIC_BY_CRC_9C0 = {0x1DEB51A9: "6101", 0xC08E5BD6: "6102", 0x03B8376A: "6103", 0xCF7F41DC: "6105",
                  0xD1059C6A: "6106"}
BOOT_OFFSET = {"6103": 0x100000, "6106": 0x200000}  # the boot address IPL3 uses is the header's minus this


def detect_order(data):
    if len(data) < 4:
        return None
    b = bytes(data[:4])
    if b == b"\x80\x37\x12\x40":
        return Z64
    if b == b"\x37\x80\x40\x12":
        return V64
    if b == b"\x40\x12\x37\x80":
        return N64
    return None


def normalize(data):
    """The ROM in the console's big-endian order (z64)."""
    order = detect_order(data)
    if order is None:
        raise ValueError("not an N64 ROM (unknown byte order)")
    data = bytes(data)
    if order == V64:
        n = len(data) & ~1
        out = bytearray(data)
        out[0:n:2], out[1:n:2] = data[1:n:2], data[0:n:2]
        return bytes(out)
    if order == N64:
        n = len(data) & ~3
        out = bytearray(data)
        out[0:n:4], out[1:n:4], out[2:n:4], out[3:n:4] = data[3:n:4], data[2:n:4], data[1:n:4], data[0:n:4]
        return bytes(out)
    return data


class Segment:
    """Bytes the boot (or a DMA) puts at a virtual address."""

    def __init__(self, vaddr, data, executable, origin, rom_offset=None):
        self.vaddr = vaddr & 0xFFFFFFFF
        self.data = bytes(data[: len(data) & ~3])
        self.executable = executable
        self.origin = origin  # "boot", "elf", "dma"
        self.rom_offset = rom_offset

    @property
    def end(self):
        return self.vaddr + len(self.data)

    def as_dict(self):
        return {"vaddr": "0x%08X" % self.vaddr, "size": len(self.data), "executable": self.executable,
                "origin": self.origin, "rom_offset": None if self.rom_offset is None else "0x%X" % self.rom_offset}


class Rom:
    def __init__(self, raw, name="rom"):
        self.name = name
        self.order = detect_order(raw)
        self.data = normalize(raw)
        if len(self.data) < 0x1000:
            raise ValueError("ROM too small (at least 4 KB)")
        d = self.data
        self.sha256 = hashlib.sha256(d).hexdigest()
        self.boot_address = struct.unpack_from(">I", d, 8)[0]
        self.crc1, self.crc2 = struct.unpack_from(">II", d, 16)
        self.title = d[0x20:0x34].decode("ascii", "replace").rstrip(" \0")
        self.game_code = d[0x3B:0x3F].decode("ascii", "replace")
        self.ipl3_crc = zlib.crc32(d[0x40:0x1000]) & 0xFFFFFFFF
        self.cic = CIC_BY_CRC.get(self.ipl3_crc) or CIC_BY_CRC_9C0.get(zlib.crc32(d[0x40:0xA00]) & 0xFFFFFFFF)
        if self.cic is None and b" Libdragon IPL3 " in d[0x40:0x1000]:
            self.cic = "libdragon"
        self.elf = find_elf(d)
        if self.elf is not None:
            self.boot = "libdragon-elf"
            self.entry = self.elf["entry"]
            self.segments = self.elf["segments"]
        else:
            self.boot = "classic"
            base = (self.boot_address - BOOT_OFFSET.get(self.cic, 0)) & 0xFFFFFFFF
            self.entry = base
            self.segments = [Segment(base, d[0x1000:0x101000], True, "boot", 0x1000)]

    def title_key(self):
        return "%s-%08X" % (self.game_code, self.crc1)

    def as_dict(self):
        return {"name": self.name, "sha256": self.sha256, "order": self.order, "title": self.title,
                "game_code": self.game_code, "cic": self.cic or "unknown", "boot": self.boot,
                "entry": "0x%08X" % self.entry, "boot_address": "0x%08X" % self.boot_address,
                "segments": [s.as_dict() for s in self.segments]}


def find_elf(d):
    """libdragon's ELF in the ROM (big-endian ELF32 MIPS, executable), as IPL3 finds it: the first one after IPL3 at
    a 256-byte boundary. Returns {entry, segments} from the program headers, or None."""
    for off in range(0x1000, min(len(d), 0x100000) - 52, 0x100):
        if d[off:off + 4] != b"\x7fELF":
            continue
        if d[off + 4] != 1 or d[off + 5] != 2:  # ELF32, big-endian
            continue
        e_type, e_machine = struct.unpack_from(">HH", d, off + 16)
        if e_machine != 8 or e_type != 2:  # MIPS, executable
            continue
        entry, phoff = struct.unpack_from(">II", d, off + 24)
        phentsize, phnum = struct.unpack_from(">HH", d, off + 42)
        if phentsize != 32 or not 0 < phnum < 64:
            continue
        segs = []
        for k in range(phnum):
            p = off + phoff + 32 * k
            if p + 32 > len(d):
                break
            p_type, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, p_flags, _ = struct.unpack_from(">8I", d, p)
            if p_type != 1 or not p_filesz:  # PT_LOAD with bytes
                continue
            start = off + p_offset
            data = d[start:start + p_filesz]
            segs.append(Segment(p_vaddr, data, bool(p_flags & 1), "elf", start))
        if segs:
            return {"entry": entry, "segments": segs, "rom_offset": off}
    return None


def load_rom(path):
    with open(path, "rb") as f:
        raw = f.read()
    import os
    return Rom(raw, os.path.basename(path))
