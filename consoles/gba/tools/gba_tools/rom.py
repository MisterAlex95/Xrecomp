"""GBA cartridge header (GBATEK « GBA Cartridge Header ») and ROM access for the recompiler (XBA-150)."""

import hashlib
import struct

ROM_BASE = 0x08000000
ROM_MAX = 0x02000000  # 32 MB


class RomError(ValueError):
    pass


class Header:
    """The 192-byte header: entry branch, title, game code, maker, version, complement check."""

    def __init__(self, data: bytes):
        if len(data) < 0xC0:
            raise RomError("ROM shorter than its 192-byte header")
        self.entry_word = struct.unpack_from("<I", data, 0)[0]
        self.title = data[0xA0:0xAC].split(b"\0")[0].decode("ascii", "replace")
        self.game_code = data[0xAC:0xB0].decode("ascii", "replace")
        self.maker = data[0xB0:0xB2].decode("ascii", "replace")
        self.fixed = data[0xB2]
        self.unit = data[0xB3]
        self.version = data[0xBC]
        self.complement = data[0xBD]
        self.computed_complement = (-(sum(data[0xA0:0xBD]) + 0x19)) & 0xFF

    @property
    def checksum_ok(self) -> bool:
        return self.complement == self.computed_complement

    @property
    def entry(self):
        """Target of the ARM branch at 0x08000000, or None when the first word is not a B."""
        w = self.entry_word
        if (w & 0x0F000000) != 0x0A000000:
            return None
        off = w & 0xFFFFFF
        if off & 0x800000:
            off -= 0x1000000
        return (ROM_BASE + 8 + off * 4) & 0xFFFFFFFF

    def as_dict(self) -> dict:
        return {
            "title": self.title,
            "game_code": self.game_code,
            "maker": self.maker,
            "version": self.version,
            "checksum_ok": self.checksum_ok,
            "entry": None if self.entry is None else "0x%08X" % self.entry,
        }


class Rom:
    """The cartridge image mapped at 0x08000000 (and its mirrors up to 0x0DFFFFFF, as the bus reads them)."""

    def __init__(self, data: bytes):
        if not data or len(data) > ROM_MAX:
            raise RomError("ROM size %d out of range (1 B .. 32 MB)" % len(data))
        self.data = bytes(data)
        self.size = len(data)
        self.header = Header(self.data) if len(data) >= 0xC0 else None
        self.sha256 = hashlib.sha256(self.data).hexdigest()

    @classmethod
    def load(cls, path: str) -> "Rom":
        with open(path, "rb") as f:
            return cls(f.read())

    def contains(self, addr: int, size: int = 1) -> bool:
        """addr..addr+size-1 inside the image (at 0x08000000, first mirror only: code lives there)."""
        return ROM_BASE <= addr and addr + size <= ROM_BASE + self.size

    def u16(self, addr: int):
        if not self.contains(addr & ~1, 2):
            return None
        return struct.unpack_from("<H", self.data, (addr & ~1) - ROM_BASE)[0]

    def u32(self, addr: int):
        if not self.contains(addr & ~3, 4):
            return None
        return struct.unpack_from("<I", self.data, (addr & ~3) - ROM_BASE)[0]

    def u8(self, addr: int):
        if not self.contains(addr):
            return None
        return self.data[addr - ROM_BASE]

    def bus_read32(self, addr: int) -> int:
        """What gba_read32 returns for a cartridge address (region 0x08-0x0D), the past-the-end pattern included."""
        a = addr & 0x1FFFFFC
        if a + 4 <= self.size:
            return struct.unpack_from("<I", self.data, a)[0]
        if a < self.size:  # the last, partial word: the core pads the image with zeros
            tail = self.data[a:] + b"\0" * 4
            return struct.unpack_from("<I", tail, 0)[0]
        return ((a >> 1) & 0xFFFF) | ((((a >> 1) + 1) & 0xFFFF) << 16)
