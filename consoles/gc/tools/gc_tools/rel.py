"""GameCube REL modules (XBA-264): parse, relocate at a load address, build (tests).

A REL is a relocatable module the game loads at run time (the SDK's OSLink): the file is read into a buffer, its
sections stay where they are in that buffer (base + section offset; the bss section is allocated apart), then the
relocations are applied: the module's own references (to itself), its references to the main DOL (module 0, absolute
addresses), and its references to other modules already loaded; OSLink also patches the modules already loaded with
their references to the new one. Every loaded module is on a list in low memory (0x800030C8 head, 0x800030CC tail),
each entry the module's header itself, i.e. the buffer's address.

How XRecomp lifts them (« per load address », consoles/README.md « GameCube Prepare »): a module's load address is
chosen by the game's heap at run time, so it cannot be known from the disc. The runtime (gc_rel.c) watches that list
and writes one record per module and load address to discovered.txt:

    REL <id> <base> <bss> <fnv1a of the executable sections> [<id>@<base> ...]

(the other modules loaded at that moment follow). Prepare reads the module from the disc's FST (by its id), applies the
relocations here for that base / bss / co-loaded set, and lifts the result only when its executable sections hash to
what the runtime saw: same bytes, same native code. The lifted functions are keyed by the address and checked like
DOL text (the span's FNV-1a over MEM1 before first use and after each icbi), so a module loaded somewhere else next
time runs in the interpreter (and is recorded at its new address for the next Prepare). Modules loaded in turn at one
place (stage modules) are each lifted; the table holds several entries per address and the span check picks the one
whose bytes are in memory.

Header (big-endian; facts from the public format descriptions):
  0x00 id  0x04 next  0x08 prev  0x0C numSections  0x10 sectionInfoOffset  0x14 nameOffset  0x18 nameSize
  0x1C version  0x20 bssSize  0x24 relOffset  0x28 impOffset  0x2C impSize
  0x30 prologSection 0x31 epilogSection 0x32 unresolvedSection 0x33 bssSection  0x34 prolog  0x38 epilog
  0x3C unresolved  (v2+) 0x40 align 0x44 bssAlign  (v3) 0x48 fixSize
Section table: {offset (bit 0: executable), size}; offset 0 with a size is the bss section.
Imports: {module id, offset of its relocation list}; a relocation: {u16 offset from the previous one, u8 type,
u8 section, u32 addend}.
"""

import struct

LIST_HEAD = 0x800030C8

R_PPC_NONE = 0
R_PPC_ADDR32 = 1
R_PPC_ADDR24 = 2
R_PPC_ADDR16 = 3
R_PPC_ADDR16_LO = 4
R_PPC_ADDR16_HI = 5
R_PPC_ADDR16_HA = 6
R_PPC_ADDR14 = 7
R_PPC_ADDR14_BRTAKEN = 8
R_PPC_ADDR14_BRNTAKEN = 9
R_PPC_REL24 = 10
R_PPC_REL14 = 11
R_PPC_REL14_BRTAKEN = 12
R_PPC_REL14_BRNTAKEN = 13
R_DOLPHIN_NOP = 201
R_DOLPHIN_SECTION = 202
R_DOLPHIN_END = 203
R_DOLPHIN_MRKREF = 204


class RelError(Exception):
    pass


def fnv1a(data, h=0x811C9DC5):
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


class Rel:
    """A REL file's header, sections and imports."""

    def __init__(self, raw):
        if len(raw) < 0x40:
            raise RelError("too small for a REL header (%d bytes)" % len(raw))
        self.raw = bytes(raw)
        (self.id, _n, _p, self.nsec, self.secinfo, self.name_off, self.name_size, self.version, self.bss_size,
         self.rel_off, self.imp_off, self.imp_size) = struct.unpack_from(">12I", raw, 0)
        (self.prolog_sec, self.epilog_sec, self.unres_sec, self.bss_sec) = struct.unpack_from(">4B", raw, 0x30)
        self.prolog, self.epilog, self.unresolved = struct.unpack_from(">3I", raw, 0x34)
        if self.version > 3 or self.version == 0:
            raise RelError("version %d" % self.version)
        if not 0 < self.nsec <= 64 or self.secinfo + 8 * self.nsec > len(raw):
            raise RelError("section table out of the file")
        self.sections = []  # (file offset or 0, size, exec)
        for k in range(self.nsec):
            off, size = struct.unpack_from(">II", raw, self.secinfo + 8 * k)
            ex = bool(off & 1)
            off &= ~1
            if off and off + size > len(raw):
                raise RelError("section %d runs past the file" % k)
            self.sections.append((off, size, ex))
        if self.imp_off + self.imp_size > len(raw) or self.imp_size % 8:
            raise RelError("import table out of the file")
        self.imports = [struct.unpack_from(">II", raw, self.imp_off + 8 * k) for k in range(self.imp_size // 8)]

    def section_addrs(self, base, bss):
        """Where each section is once loaded at base with its bss at bss (0 for an empty section)."""
        out = []
        for k, (off, size, _ex) in enumerate(self.sections):
            if off:
                out.append(base + off)
            elif size:
                out.append(bss)
            else:
                out.append(0)
        return out

    def exec_sections(self):
        return [k for k, (off, size, ex) in enumerate(self.sections) if ex and off and size]

    def relocations(self, imp_offset):
        """The relocation list at a file offset: (offset, type, section, addend) up to R_DOLPHIN_END."""
        p = imp_offset
        while p + 8 <= len(self.raw):
            off, typ, sec, add = struct.unpack_from(">HBBI", self.raw, p)
            yield off, typ, sec, add
            if typ == R_DOLPHIN_END:
                return
            p += 8
        raise RelError("relocation list without its end")


class Loaded:
    """A module as the game has it in memory: its bytes from base, its section addresses (relocated by link())."""

    def __init__(self, rel, base, bss):
        self.rel, self.base, self.bss = rel, base, bss
        self.mem = bytearray(rel.raw)
        self.addrs = rel.section_addrs(base, bss)
        self.skipped = []  # module ids whose references were left as in the file (not loaded)

    def _put(self, at, size, value):
        o = at - self.base
        if o < 0 or o + size > len(self.mem):
            raise RelError("relocation at 0x%08X outside the module" % at)
        if size == 4:
            struct.pack_into(">I", self.mem, o, value & 0xFFFFFFFF)
        else:
            struct.pack_into(">H", self.mem, o, value & 0xFFFF)

    def _get(self, at):
        return struct.unpack_from(">I", self.mem, at - self.base)[0]

    def link(self, others=()):
        """Applies this module's relocations against module 0 (the DOL), itself and others ([Loaded]); references to
        any other module stay as the file has them (OSLink leaves them until that module loads)."""
        by_id = {o.rel.id: o for o in others}
        by_id[self.rel.id] = self
        for mid, off in self.rel.imports:
            if mid == 0:
                target = None
            elif mid in by_id:
                target = by_id[mid].addrs
            else:
                self.skipped.append(mid)
                continue
            p = 0
            for roff, typ, sec, add in self.rel.relocations(off):
                p += roff
                s = 0 if target is None else target[sec] if sec < len(target) else 0
                x = (s + add) & 0xFFFFFFFF
                if typ == R_DOLPHIN_SECTION:
                    p = self.addrs[sec]
                elif typ in (R_PPC_NONE, R_DOLPHIN_NOP, R_DOLPHIN_MRKREF):
                    pass
                elif typ == R_DOLPHIN_END:
                    break
                elif typ == R_PPC_ADDR32:
                    self._put(p, 4, x)
                elif typ == R_PPC_ADDR24:
                    self._put(p, 4, (self._get(p) & ~0x03FFFFFC) | (x & 0x03FFFFFC))
                elif typ == R_PPC_ADDR16:
                    self._put(p, 2, x)
                elif typ == R_PPC_ADDR16_LO:
                    self._put(p, 2, x)
                elif typ == R_PPC_ADDR16_HI:
                    self._put(p, 2, x >> 16)
                elif typ == R_PPC_ADDR16_HA:
                    self._put(p, 2, (x >> 16) + (1 if x & 0x8000 else 0))
                elif typ in (R_PPC_ADDR14, R_PPC_ADDR14_BRTAKEN, R_PPC_ADDR14_BRNTAKEN):
                    self._put(p, 4, (self._get(p) & ~0xFFFC) | (x & 0xFFFC))
                elif typ == R_PPC_REL24:
                    self._put(p, 4, (self._get(p) & ~0x03FFFFFC) | ((x - p) & 0x03FFFFFC))
                elif typ in (R_PPC_REL14, R_PPC_REL14_BRTAKEN, R_PPC_REL14_BRNTAKEN):
                    self._put(p, 4, (self._get(p) & ~0xFFFC) | ((x - p) & 0xFFFC))
                else:
                    raise RelError("relocation type %d" % typ)
        return self

    def section_bytes(self, k):
        off, size, _ex = self.rel.sections[k]
        return bytes(self.mem[off:off + size])

    def exec_hash(self):
        """FNV-1a over the executable sections in order (gc_rel.c's record hash)."""
        h = 0x811C9DC5
        for k in self.rel.exec_sections():
            h = fnv1a(self.section_bytes(k), h)
        return h

    def os_image(self):
        """The module's bytes as OSLink leaves them in memory (tests): the relocated sections, and the header's
        section table, prolog / epilog / unresolved turned into addresses."""
        out = bytearray(self.mem)
        r = self.rel
        struct.pack_into(">I", out, 0x10, self.base + r.secinfo)
        for k, (off, size, ex) in enumerate(r.sections):
            if off or size:
                struct.pack_into(">I", out, r.secinfo + 8 * k, self.addrs[k] | (1 if ex and off else 0))
        for o, sec, val in ((0x34, r.prolog_sec, r.prolog), (0x38, r.epilog_sec, r.epilog),
                            (0x3C, r.unres_sec, r.unresolved)):
            if sec:
                struct.pack_into(">I", out, o, self.addrs[sec] + val)
        return bytes(out)

    def entry_points(self):
        """prolog / epilog / unresolved as addresses (0 when absent)."""
        out = []
        r = self.rel
        for sec, off in ((r.prolog_sec, r.prolog), (r.epilog_sec, r.epilog), (r.unres_sec, r.unresolved)):
            if sec and sec < len(self.addrs) and self.addrs[sec]:
                out.append(self.addrs[sec] + off)
        return out


# ---- discovered.txt records ----

def parse_record(line):
    """« REL id base bss hash [id@base ...] » -> (id, base, bss, hash, ((id, base), ...)), else None."""
    s = line.split("#", 1)[0].split()
    if len(s) < 5 or s[0] != "REL":
        return None
    try:
        mid, base, bss, h = int(s[1], 16), int(s[2], 16), int(s[3], 16), int(s[4], 16)
        co = []
        for t in s[5:]:
            a, b = t.split("@")
            co.append((int(a, 16), int(b, 16)))
    except ValueError:
        return None
    return mid, base, bss, h, tuple(sorted(co))


def format_record(mid, base, bss, h, co=()):
    return "REL %X %08X %08X %08X" % (mid, base, bss, h) + "".join(" %X@%08X" % c for c in sorted(co))


# ---- building (tests) ----

def build(rel_id, sections, imports, prolog=None, epilog=None, unresolved=None, bss_size=0, version=3):
    """A REL file: sections [(bytes or None for bss, exec)], section 0 left empty as the SDK does; imports
    {module id: [(section, offset, type, target section, addend)]} with offsets inside that section; prolog / epilog /
    unresolved (section, offset)."""
    hdr = {1: 0x40, 2: 0x48, 3: 0x4C}[version]
    secs = [(None, False, 0)] + [(d, ex, 0) for d, ex in sections]
    body = bytearray(hdr + 8 * len(secs))
    table = []
    bss_index = 0
    for k, (data, ex, _z) in enumerate(secs):
        if k == 0:
            table.append((0, 0))
            continue
        if data is None:
            table.append((0, bss_size))
            bss_index = k
            continue
        while len(body) % 32:
            body.append(0)
        table.append((len(body) | (1 if ex else 0), len(data)))
        body += data
    while len(body) % 4:
        body.append(0)
    imp_off = len(body)
    mods = sorted(imports, key=lambda m: (m == 0, m))  # module 0 last, as makerel does
    body += b"\0" * (8 * len(mods))
    rel_off = len(body)
    lists = []
    for mid in mods:
        start = len(body)
        cur_sec, pos = None, 0
        for sec, off, typ, tsec, add in sorted(imports[mid]):
            if sec != cur_sec:
                body += struct.pack(">HBBI", 0, R_DOLPHIN_SECTION, sec, 0)
                cur_sec, pos = sec, 0
            delta = off - pos
            while delta > 0xFFFF:
                body += struct.pack(">HBBI", 0xFFFF, R_DOLPHIN_NOP, 0, 0)
                delta -= 0xFFFF
            body += struct.pack(">HBBI", delta, typ, tsec, add)
            pos = off
        body += struct.pack(">HBBI", 0, R_DOLPHIN_END, 0, 0)
        lists.append((mid, start))
    for k, (mid, start) in enumerate(lists):
        struct.pack_into(">II", body, imp_off + 8 * k, mid, start)
    for k, (off, size) in enumerate(table):
        struct.pack_into(">II", body, hdr + 8 * k, off, size)
    p, e, u = prolog or (0, 0), epilog or (0, 0), unresolved or (0, 0)
    struct.pack_into(">12I", body, 0, rel_id, 0, 0, len(secs), hdr, 0, 0, version, bss_size, rel_off, imp_off,
                     8 * len(mods))
    struct.pack_into(">4B3I", body, 0x30, p[0], e[0], u[0], bss_index, p[1], e[1], u[1])
    if version >= 2:
        struct.pack_into(">II", body, 0x40, 32, 32)
    if version >= 3:
        struct.pack_into(">I", body, 0x48, rel_off)
    return bytes(body)
