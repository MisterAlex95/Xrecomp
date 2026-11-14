"""PS-X EXE and PlayStation disc input (XBA-160): the program a title boots, from an EXE file or from a disc image.

A disc (.cue / .bin / .iso / .chd) is read as ISO 9660: SYSTEM.CNF's BOOT line names the EXE (cdrom:\\SLUS_005.94;1); without
SYSTEM.CNF, PSX.EXE. Raw images (2352-byte sectors, mode 1 or mode 2 form 1) and cooked ones (2048) are recognised by
their sync pattern; a .chd (MAME compressed hunks, told by its MComprHD magic) through chd.py. Pure Python, no native
extension (the app's embedded CPython runs it).
"""

import hashlib
import os
import re
import struct

SECTOR_RAW = 2352
SECTOR = 2048
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


class Exe:
    """A PS-X EXE: text (code + data) loaded at text_addr, entry pc0, gp0, the stack, BSS."""

    def __init__(self, data, name="exe"):
        if len(data) < 0x800 or data[:8] != b"PS-X EXE":
            raise ValueError("not a PS-X EXE")
        (self.pc0, self.gp0, self.text_addr, self.text_size, _d_addr, _d_size, self.bss_addr, self.bss_size,
         self.sp_addr, self.sp_size) = struct.unpack_from("<10I", data, 0x10)
        self.text = bytes(data[0x800:0x800 + self.text_size])
        if len(self.text) < self.text_size:
            self.text += bytes(self.text_size - len(self.text))
        if self.text_addr & 3 or (self.text_addr & 0x1FFFFFFF) + self.text_size > 0x00800000:
            raise ValueError("PS-X EXE text outside main RAM")
        self.sha256 = hashlib.sha256(bytes(data)).hexdigest()
        self.name = name

    def as_dict(self):
        return {"name": self.name, "pc0": "0x%08X" % self.pc0, "gp0": "0x%08X" % self.gp0,
                "text_addr": "0x%08X" % self.text_addr, "text_size": self.text_size, "sha256": self.sha256}


class Disc:
    """Reads 2048-byte user data sectors from a .cue / .bin / .iso image (first data track) or a .chd."""

    def __init__(self, path):
        self.path = path
        self.chd = None
        with open(path, "rb") as f:
            if f.read(8) == b"MComprHD":
                from ps1_tools import chd
                self.chd = chd.Chd(path)
                if not self.chd.tracks or self.chd.tracks[0].audio:
                    self.chd.close()
                    raise ValueError("chd: the first track is not a data track")
                return
        bin_path, mode = path, None
        if path.lower().endswith(".cue"):
            bin_path, mode = self._parse_cue(path)
        self.f = open(bin_path, "rb")
        size = os.path.getsize(bin_path)
        head = self.f.read(16 * SECTOR_RAW + 16)
        if mode in ("MODE1/2048", "MODE2/2048") or (mode is None and head[:12] != SYNC):
            self.raw, self.size = False, SECTOR
        else:
            self.raw, self.size = True, SECTOR_RAW
        if not self.raw and size % SECTOR and size % SECTOR_RAW == 0 and head[:12] == SYNC:
            self.raw, self.size = True, SECTOR_RAW

    @staticmethod
    def _parse_cue(path):
        base = os.path.dirname(os.path.abspath(path))
        cur_file, mode = None, None
        with open(path, encoding="latin-1") as f:
            for line in f:
                m = re.match(r'\s*FILE\s+"?(.*?)"?\s+\w+\s*$', line, re.I)
                if m:
                    cur_file = os.path.join(base, m.group(1))
                    continue
                m = re.match(r"\s*TRACK\s+\d+\s+(\S+)", line, re.I)
                if m and cur_file:
                    mode = m.group(1).upper()
                    if mode != "AUDIO":
                        return cur_file, mode
        if not cur_file:
            raise ValueError("no FILE in the cue sheet")
        return cur_file, mode

    def close(self):
        if self.chd:
            self.chd.close()
        else:
            self.f.close()

    def sector(self, lba):
        if self.chd:
            return self.chd.user_data(lba)
        self.f.seek(lba * self.size)
        raw = self.f.read(self.size)
        if len(raw) < self.size:
            raise ValueError("sector %d past the end of the image" % lba)
        if not self.raw:
            return raw
        off = 24 if raw[15] == 2 else 16  # mode 2 form 1: subheader; mode 1: none
        return raw[off:off + SECTOR]

    def read(self, lba, length):
        out = bytearray()
        while len(out) < length:
            out += self.sector(lba)
            lba += 1
        return bytes(out[:length])

    # ---- ISO 9660 ----
    def _dir_entries(self, lba, length):
        data = self.read(lba, length)
        pos = 0
        while pos < len(data):
            n = data[pos]
            if n == 0:
                pos = (pos // SECTOR + 1) * SECTOR  # records never cross a sector
                continue
            ext, size = struct.unpack_from("<I", data, pos + 2)[0], struct.unpack_from("<I", data, pos + 10)[0]
            flags, nlen = data[pos + 25], data[pos + 32]
            name = data[pos + 33:pos + 33 + nlen].decode("latin-1")
            if name not in ("\x00", "\x01"):
                yield name.split(";")[0].upper(), ext, size, bool(flags & 2)
            pos += n

    def pvd(self):
        d = self.sector(16)
        if d[1:6] != b"CD001":
            raise ValueError("no ISO 9660 volume")
        return d

    def find(self, path):
        """(lba, size) of a file by path ('\\DIR\\NAME.EXT;1', case-insensitive), or None."""
        d = self.pvd()
        lba, size = struct.unpack_from("<I", d, 156 + 2)[0], struct.unpack_from("<I", d, 156 + 10)[0]
        parts = [p for p in re.split(r"[\\/]+", path) if p]
        for i, part in enumerate(parts):
            want = part.split(";")[0].upper()
            for name, ext, sz, is_dir in self._dir_entries(lba, size):
                if name == want and is_dir == (i < len(parts) - 1):
                    lba, size = ext, sz
                    break
            else:
                return None
        return lba, size

    def read_file(self, path):
        hit = self.find(path)
        if not hit:
            raise ValueError("%s not on the disc" % path)
        return self.read(hit[0], hit[1])

    def boot_path(self):
        """The EXE SYSTEM.CNF boots (BOOT = cdrom:\\X;1), else PSX.EXE."""
        if self.find("SYSTEM.CNF"):
            cnf = self.read_file("SYSTEM.CNF").decode("latin-1")
            m = re.search(r"BOOT\s*=\s*(\S+)", cnf, re.I)
            if m:
                p = m.group(1)
                p = re.sub(r"^cdrom:?", "", p, flags=re.I)
                return p
        return "PSX.EXE"


def load_input(path):
    """An Exe from a PS-X EXE file or a disc image (.cue / .bin / .iso / .img / .chd)."""
    with open(path, "rb") as f:
        head = f.read(8)
    if head == b"PS-X EXE":
        with open(path, "rb") as f:
            return Exe(f.read(), os.path.basename(path))
    disc = Disc(path)
    try:
        boot = disc.boot_path()
        return Exe(disc.read_file(boot), boot.split(";")[0].strip("\\/"))
    finally:
        disc.close()
