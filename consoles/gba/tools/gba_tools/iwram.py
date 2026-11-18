"""Code a game copies from the cartridge to IWRAM at run time, lifted (XBA-279).

GBA games run their hottest code from IWRAM (32-bit bus, no wait state): the IRQ handler (libtonc's isr_master,
Nintendo's intr_main), sound mixers, rasterisers. crt0 or the game copies it there from the ROM, so its bytes are in
the image, but not at the address it runs from. The core records where execution enters IWRAM code the pack does not
hold, and for each such address where the cartridge holds the same bytes, as a delta record in the discovered list
(gba_recomp.h « Discovery of IWRAM code »: 0xE0000000 | (ROM address - IWRAM address - DELTA_MIN) / 4). Here, each
IWRAM address is explored and lifted from the ROM at the distance recorded (the first one under which its code
decodes cleanly), with gba_tools' discovery and lifter unchanged, through IwramImage (the ROM seen at IWRAM addresses)
and two overrides:

- the open-bus latch of an instruction is read from IWRAM at run time (RC_IW_AOB / RC_IW_TOB: IWRAM's own prefetch
  pattern), not folded from the cartridge;
- the function enters with RC_ENTER_IW(T, LO, HI), its span: the core calls it only while IWRAM holds the bytes it was
  lifted from (compared at each call), and a store of its own into them stops it after that instruction.

PC-relative literals in IWRAM are not folded (gba_tools folds cartridge literals only): they are loaded at run time,
as the interpreter does. The ROM's functions, their files and their names are the same with or without IWRAM code:
the IWRAM functions go to files of their own (gba_rc_iw_*.c), and the table lists them with their spans (ABI 5).
"""

from .discover import Discovery
from .lift import FnLifter, hx

IWRAM_BASE = 0x03000000
IWRAM_SIZE = 0x8000
DELTA_TAG = 0xE0000000
DELTA_MIN = 0x04FF8000  # 0x08000000 - 0x03008000


def is_iwram(addr):
    return IWRAM_BASE <= (addr & ~1) < IWRAM_BASE + IWRAM_SIZE


def decode_delta(v):
    """The ROM - IWRAM distance of a delta record, else None."""
    if (v & 0xFF000000) != DELTA_TAG:
        return None
    return DELTA_MIN + ((v & 0x00FFFFFF) << 2)


def encode_delta(d):
    return DELTA_TAG | ((d - DELTA_MIN) >> 2)


def split_discovered(addrs):
    """(IWRAM keys, deltas) of a discovered list (bit 0 of a key = Thumb), each sorted."""
    keys, deltas = set(), set()
    for a in addrs:
        d = decode_delta(a)
        if d is not None:
            deltas.add(d)
        elif is_iwram(a):
            keys.add(a)
    return sorted(keys), sorted(deltas)


class IwramImage:
    """The ROM's bytes seen at IWRAM addresses, at a distance delta: IWRAM address a holds ROM address a + delta."""

    def __init__(self, rom, delta):
        self.rom = rom
        self.delta = delta
        self.sha256 = rom.sha256
        self.header = rom.header
        self.data = rom.data  # FnLifter.fold_literal reads it for cartridge addresses only: none are in this image
        self.size = rom.size

    def contains(self, addr, size=1):
        return IWRAM_BASE <= addr and addr + size <= IWRAM_BASE + IWRAM_SIZE and self.rom.contains(addr + self.delta,
                                                                                                    size)

    def u8(self, addr):
        return self.rom.u8(addr + self.delta) if self.contains(addr) else None

    def u16(self, addr):
        return self.rom.u16(addr + self.delta) if self.contains(addr & ~1, 2) else None

    def u32(self, addr):
        return self.rom.u32(addr + self.delta) if self.contains(addr & ~3, 4) else None

    def span(self, lo, hi):
        o = lo + self.delta - 0x08000000
        return self.rom.data[o:o + (hi - lo)]


class IwramDiscovery(Discovery):
    """gba_tools' discovery on an IwramImage, from the recorded IWRAM addresses (the cartridge entry is not here)."""

    def add_entry(self, addr, thumb, why):
        if not self.in_rom(addr, 2 if thumb else 4):
            return False
        return super().add_entry(addr, thumb, why)


def span_of(f):
    size = 2 if f.thumb else 4
    return min(f.addrs), max(f.addrs) + size


class IwramLifter(FnLifter):
    """gba_tools' lifter for an IWRAM function: the open bus from IWRAM at run time, RC_ENTER_IW with its span."""

    def aob(self, a):
        return "RC_IW_AOB(%s)" % hx(a)

    def tob(self, a):
        return "RC_IW_TOB(%s)" % hx(a)

    def function(self, name, region, comments=True):
        text = super().function(name, region, comments)
        lo, hi = min(self.addrs), max(self.addrs) + (2 if self.thumb else 4)
        old = "RC_ENTER(%d, %d);" % (int(self.thumb), region)
        assert old in text
        return text.replace(old, "RC_ENTER_IW(%d, %s, %s);" % (int(self.thumb), hx(lo), hx(hi)), 1)


def discover(rom, discovered):
    """[(function, image)] of the IWRAM code the discovered list names, functions sorted by entry (one per entry:
    the first distance under which an address decodes cleanly wins it)."""
    keys, deltas = split_discovered(discovered)
    if not keys or not deltas:
        return []
    owner = {}
    for key in keys:
        addr, thumb = key & ~1, bool(key & 1)
        if addr & (1 if thumb else 3):
            continue
        for d in deltas:
            img = IwramImage(rom, d)
            if not img.contains(addr, 2 if thumb else 4):
                continue
            g = IwramDiscovery(img).explore(addr, thumb, strict=True)
            if not g.bad and g.addrs:
                owner[key] = d
                break
    out, seen = [], set()
    for d in deltas:
        seeds = [k for k in sorted(owner) if owner[k] == d]
        if not seeds:
            continue
        img = IwramImage(rom, d)
        for f in IwramDiscovery(img, seeds).run():
            if f.key in seen:
                continue
            seen.add(f.key)
            out.append((f, img))
    out.sort(key=lambda t: t[0].key)
    return out
