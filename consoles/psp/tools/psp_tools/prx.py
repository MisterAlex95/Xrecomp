"""PSP executables (XBA-224): EBOOT.PBP, ELF and PRX (relocatable ELF, e_type 0xFFA0) loaded as the firmware would,
statically: segments at the load address, relocations applied, module info read, every NID import stub patched to
`jr ra ; syscall N` (N = the import's index, the HLE's number for it). consoles/psp/runtime/psp_load.c does the same
in C, byte for byte: lifted code names its functions by content (psp_recomp.h), so both must agree.

Layout:
- EBOOT.PBP: "\\0PBP", version, eight offsets; DATA.PSP (the module) is between the 7th and the 8th. Encrypted
  modules (~PSP) are refused.
- PRX: one or more PT_LOAD segments at vaddr 0; loaded at PSP_LOAD_BASE (where the firmware puts the first user
  module). Relocations: sections of type 0x700000A0 (SHT_PRXRELOC; plain REL entries whose r_info carries the
  offset base segment in bits 8-15 and the address base segment in bits 16-23). R_MIPS_32, R_MIPS_26, R_MIPS_HI16
  (paired with the next R_MIPS_LO16 for the carry), R_MIPS_LO16; GPREL16 needs nothing (the module moves as a
  whole). PRXes without section headers (relocations in program headers, type 0x700000A1 compressed) are not
  handled: PSPSDK's psp-prxgen keeps the sections.
- ELF (ET_EXEC): segments at their vaddr, nothing to relocate.
- Module info (SceModuleInfo, 52 bytes): the first segment's p_paddr is its file offset in a PRX (else the
  .rodata.sceModuleInfo section): attributes, name, gp, the export and import tables' bounds.
- Imports (.lib.stub): {libname, version, attr, entry length in words, var count, func count, NID table, stubs};
  stub k of a library is 8 bytes at stubs + 8k, its NID at nids + 4k.
- Exports (.lib.ent): module_start (NID 0xD632ACDB) is the entry when exported, else e_entry;
  module_start_thread_parameter (0x0F7C276C) gives the start thread's priority, stack and attributes.
"""

import hashlib
import struct

PSP_LOAD_BASE = 0x08804000
SHT_PRXRELOC = 0x700000A0
ET_EXEC, ET_PRX = 2, 0xFFA0
NID_MODULE_START = 0xD632ACDB
NID_MODULE_START_THREAD_PARAMETER = 0x0F7C276C
JR_RA = 0x03E00008
SYSCALL_IMPORT_BASE = 0  # syscall code of import k (psp_hle.c uses the same numbering)

R_MIPS_NONE, R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16, R_MIPS_GPREL16 = 0, 2, 4, 5, 6, 7


class PspError(ValueError):
    pass


def syscall_word(code):
    return ((code & 0xFFFFF) << 6) | 0x0C


class Import:
    __slots__ = ("index", "lib", "nid", "stub")

    def __init__(self, index, lib, nid, stub):
        self.index, self.lib, self.nid, self.stub = index, lib, nid, stub


class Module:
    """A loaded module: mem (bytes at base), the code range, entry, gp, imports, code pointers in data."""

    def __init__(self):
        self.name = ""
        self.base = 0
        self.mem = bytearray()
        self.entry = 0
        self.gp = 0
        self.code_lo = self.code_hi = 0
        self.imports = []
        self.exports = {}  # NID -> address (functions)
        self.data_ptrs = set()  # R_MIPS_32 targets (relocated words) inside the code range
        self.start_prio, self.start_stack, self.start_attr = 0x20, 0x40000, 0
        self.sha256 = ""
        self.path = ""
        self.kind = ""

    def word(self, addr):
        return struct.unpack_from("<I", self.mem, addr - self.base)[0]

    def put(self, addr, v):
        struct.pack_into("<I", self.mem, addr - self.base, v & 0xFFFFFFFF)

    def cstr(self, addr, limit=64):
        o = addr - self.base
        if not 0 <= o < len(self.mem):
            return ""
        raw = bytes(self.mem[o:o + limit]).split(b"\0", 1)[0]
        return raw.decode("latin-1")

    def code(self):
        return bytes(self.mem[self.code_lo - self.base:self.code_hi - self.base])

    def as_dict(self):
        return {"name": self.name, "kind": self.kind, "base": "0x%08X" % self.base, "size": len(self.mem),
                "entry": "0x%08X" % self.entry, "gp": "0x%08X" % self.gp,
                "code": ["0x%08X" % self.code_lo, "0x%08X" % self.code_hi], "imports": len(self.imports),
                "sha256": self.sha256}


def unwrap_pbp(data):
    """The module inside an EBOOT.PBP (DATA.PSP), or data itself when it is no PBP."""
    if data[:4] != b"\0PBP":
        return data
    if len(data) < 40:
        raise PspError("truncated PBP header")
    offs = struct.unpack_from("<8I", data, 8)
    psp = data[offs[6]:offs[7]] if offs[7] >= offs[6] else data[offs[6]:]
    if not psp:
        raise PspError("PBP has no DATA.PSP")
    return psp


def load(data, base=PSP_LOAD_BASE):
    """A Module from the bytes of an EBOOT.PBP, a PRX or an ELF."""
    raw = data
    data = unwrap_pbp(data)
    if data[:4] == b"~PSP":
        raise PspError("encrypted module (~PSP): decrypt it first")
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise PspError("not a little-endian ELF32")
    (e_type, e_machine, _v, e_entry, e_phoff, e_shoff, _fl, _eh, e_phentsize, e_phnum, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    if e_machine != 8:
        raise PspError("not a MIPS ELF (machine %d)" % e_machine)
    if e_type not in (ET_EXEC, ET_PRX):
        raise PspError("ELF type 0x%X: neither an executable nor a PRX" % e_type)
    m = Module()
    m.kind = "prx" if e_type == ET_PRX else "elf"
    m.sha256 = hashlib.sha256(raw).hexdigest()
    phs = [struct.unpack_from("<8I", data, e_phoff + k * e_phentsize) for k in range(e_phnum)]
    loads = [p for p in phs if p[0] == 1]
    if not loads:
        raise PspError("no PT_LOAD segment")
    reloc = base if m.kind == "prx" else 0
    lo = min(p[2] for p in loads) + reloc
    hi = max(p[2] + p[5] for p in loads) + reloc
    m.base = lo
    m.mem = bytearray(hi - lo)
    for p in loads:
        _t, off, vaddr, _pa, filesz, _memsz, _f, _al = p
        m.mem[vaddr + reloc - lo:vaddr + reloc - lo + filesz] = data[off:off + filesz]
    segs = [p[2] for p in loads]
    shs = [struct.unpack_from("<10I", data, e_shoff + k * e_shentsize) for k in range(e_shnum)] if e_shoff else []
    names = {}
    if shs and e_shstrndx < len(shs):
        stro = shs[e_shstrndx][4]
        for k, sh in enumerate(shs):
            names[k] = data[stro + sh[0]:].split(b"\0", 1)[0].decode("latin-1")
    # the code: the executable sections, else the first segment
    ex = [(sh[3], sh[3] + sh[5]) for sh in shs if sh[2] & 4 and sh[5] and sh[1] == 1]
    if ex:
        m.code_lo, m.code_hi = min(a for a, _ in ex) + reloc, max(b for _, b in ex) + reloc
    else:
        m.code_lo, m.code_hi = loads[0][2] + reloc, loads[0][2] + loads[0][4] + reloc
    if m.kind == "prx":
        for k, sh in enumerate(shs):
            if sh[1] == SHT_PRXRELOC:
                _relocate(m, data[sh[4]:sh[4] + sh[5]], segs, reloc)
    # module info
    mi = None
    if m.kind == "prx":
        p0 = loads[0]
        mi = p0[3] - p0[1] + p0[2] + reloc if p0[1] <= p0[3] < p0[1] + p0[4] else None
    if mi is None:
        for k, sh in enumerate(shs):
            if names.get(k) == ".rodata.sceModuleInfo":
                mi = sh[3] + reloc
    if mi is None:
        raise PspError("no module info")
    m.name = m.cstr(mi + 4, 28)
    m.gp = m.word(mi + 32)
    ent_top, ent_end, stub_top, stub_end = (m.word(mi + 36 + 4 * k) for k in range(4))
    m.entry = (e_entry + reloc) & 0xFFFFFFFF
    _exports(m, ent_top, ent_end)
    _imports(m, stub_top, stub_end)
    if NID_MODULE_START in m.exports:
        m.entry = m.exports[NID_MODULE_START]
    return m


def _relocate(m, rel, segs, reloc):
    n = len(rel) // 8
    ents = [struct.unpack_from("<II", rel, 8 * k) for k in range(n)]
    for k, (off, info) in enumerate(ents):
        typ, ofs_seg, adr_seg = info & 0xFF, (info >> 8) & 0xFF, (info >> 16) & 0xFF
        if ofs_seg >= len(segs) or adr_seg >= len(segs):
            raise PspError("relocation %d: segment out of range" % k)
        at = segs[ofs_seg] + off + reloc
        delta = segs[adr_seg] + reloc
        if not m.base <= at < m.base + len(m.mem) - 3:
            raise PspError("relocation %d at 0x%08X outside the module" % (k, at))
        w = m.word(at)
        if typ == R_MIPS_32:
            v = (w + delta) & 0xFFFFFFFF
            m.put(at, v)
            if m.code_lo <= v < m.code_hi and not v & 3:
                m.data_ptrs.add(v)
        elif typ == R_MIPS_26:
            m.put(at, (w & 0xFC000000) | (((w & 0x03FFFFFF) + (delta >> 2)) & 0x03FFFFFF))
        elif typ == R_MIPS_HI16:
            lo = 0
            for off2, info2 in ents[k + 1:]:
                if info2 & 0xFF == R_MIPS_LO16:
                    lo = m.word(segs[(info2 >> 8) & 0xFF] + off2 + reloc) & 0xFFFF
                    lo = lo - 0x10000 if lo & 0x8000 else lo
                    break
            full = (((w & 0xFFFF) << 16) + lo + delta) & 0xFFFFFFFF
            m.put(at, (w & 0xFFFF0000) | (((full + 0x8000) >> 16) & 0xFFFF))
        elif typ == R_MIPS_LO16:
            m.put(at, (w & 0xFFFF0000) | ((w + delta) & 0xFFFF))
        elif typ in (R_MIPS_NONE, R_MIPS_GPREL16):
            pass
        else:
            raise PspError("relocation %d: type %d not handled" % (k, typ))


def _exports(m, top, end):
    a = top
    while top and a + 16 <= end:
        _lib, _ver, _attr, length, vcount, fcount, table = struct.unpack_from("<IHHBBHI", m.mem, a - m.base)
        if length < 4:
            break
        for k in range(fcount + vcount):
            nid = m.word(table + 4 * k)
            val = m.word(table + 4 * (fcount + vcount + k))
            if k < fcount:
                m.exports[nid] = val
            elif nid == NID_MODULE_START_THREAD_PARAMETER:
                cnt = m.word(val)
                if cnt >= 3:
                    m.start_prio, m.start_stack, m.start_attr = (m.word(val + 4 * j) for j in (1, 2, 3))
        a += length * 4


def _imports(m, top, end):
    a = top
    while top and a + 20 <= end:
        libp, _ver, _attr, length, _vcount, fcount, nids, stubs = struct.unpack_from("<IHHBBHII", m.mem,
                                                                                    a - m.base)
        if length < 5:
            break
        lib = m.cstr(libp)
        for k in range(fcount):
            imp = Import(len(m.imports), lib, m.word(nids + 4 * k), stubs + 8 * k)
            m.imports.append(imp)
            m.put(imp.stub, JR_RA)
            m.put(imp.stub + 4, syscall_word(SYSCALL_IMPORT_BASE + imp.index))
        a += length * 4


def load_file(path, base=PSP_LOAD_BASE):
    with open(path, "rb") as f:
        m = load(f.read(), base)
    m.path = path
    return m
