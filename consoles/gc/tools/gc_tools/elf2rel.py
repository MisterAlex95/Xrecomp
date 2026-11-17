"""`python3 -m gc_tools.elf2rel MODULE.o PROG.elf OUT.rel --id N`: a REL module from a relocatable PowerPC ELF object
(XBA-264, for our homebrew tests: consoles/gc/tests/homebrew/relload).

The REL's sections are the object's, by index (as the SDK's makerel keeps them: section k of the REL is ELF section
k; non-allocated ones are left empty), executable ones flagged, the one NOBITS section the bss. Every relocation of an
allocated section becomes an import: against the module itself when its symbol is defined in the object (target
section = the symbol's, addend = its value + the relocation's addend), else against the DOL (module 0) with the
address of the symbol of that name in PROG.elf's symbol table. _prolog / _epilog / _unresolved, when defined, are the
header's entry points. Relocation types: what GCC emits for -G0 -msdata=none code (ADDR32, ADDR16 / LO / HI / HA,
ADDR24, ADDR14, REL24, REL14). Facts from the ELF and PowerPC EABI specifications.
"""

import argparse
import struct
import sys

from . import rel

SHT_SYMTAB, SHT_NOBITS, SHT_RELA = 2, 8, 4
SHF_ALLOC, SHF_EXECINSTR = 2, 4
SHN_UNDEF, SHN_ABS = 0, 0xFFF1
TYPES = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}


class ElfError(Exception):
    pass


class Elf:
    """A 32-bit big-endian ELF file's sections and symbols."""

    def __init__(self, raw):
        if raw[:4] != b"\x7fELF" or raw[4] != 1 or raw[5] != 2:
            raise ElfError("not a 32-bit big-endian ELF")
        self.raw = raw
        (self.type, _m, _v, self.entry, _ph, shoff, _f, _eh, _phes, _phn, shes, shn, shstr) = \
            struct.unpack_from(">HHIIIIIHHHHHH", raw, 16)
        self.sections = []
        for k in range(shn):
            name, typ, flags, addr, off, size, link, info, align, entsize = \
                struct.unpack_from(">10I", raw, shoff + k * shes)
            self.sections.append({"name_off": name, "type": typ, "flags": flags, "addr": addr, "offset": off,
                                  "size": size, "link": link, "info": info, "align": align, "entsize": entsize})
        strtab = self.sections[shstr]
        for s in self.sections:
            s["name"] = self._str(strtab, s["name_off"])
        self.symbols = []
        for s in self.sections:
            if s["type"] == SHT_SYMTAB:
                names = self.sections[s["link"]]
                for i in range(s["size"] // 16):
                    nm, value, size, info, other, shndx = struct.unpack_from(">IIIBBH", raw, s["offset"] + 16 * i)
                    self.symbols.append({"name": self._str(names, nm), "value": value, "size": size, "info": info,
                                         "shndx": shndx})

    def _str(self, sec, off):
        b = self.raw[sec["offset"] + off:]
        return b[:b.index(b"\0")].decode("latin-1")

    def data(self, k):
        s = self.sections[k]
        return b"" if s["type"] == SHT_NOBITS else self.raw[s["offset"]:s["offset"] + s["size"]]

    def symbol_addrs(self):
        """{name: value} of the defined global and local symbols (functions and objects)."""
        return {s["name"]: s["value"] for s in self.symbols if s["name"] and s["shndx"] != SHN_UNDEF}


def elf2rel(obj, dol_symbols, rel_id, version=3):
    """REL bytes of a relocatable ELF object (bytes) against the DOL's symbols ({name: address})."""
    e = Elf(obj)
    if e.type != 1:
        raise ElfError("not a relocatable object (ET_REL)")
    nsec = len(e.sections)
    sections = []
    bss_size = 0
    for k in range(1, nsec):
        s = e.sections[k]
        if not s["flags"] & SHF_ALLOC or not s["size"]:
            sections.append((b"", False))
            continue
        if s["type"] == SHT_NOBITS:
            if bss_size:
                raise ElfError("more than one NOBITS section (%s)" % s["name"])
            bss_size = s["size"]
            sections.append((None, False))
            continue
        sections.append((e.data(k), bool(s["flags"] & SHF_EXECINSTR)))
    imports = {}
    for s in e.sections:
        if s["type"] != SHT_RELA:
            continue
        target = s["info"]
        if not e.sections[target]["flags"] & SHF_ALLOC or not e.sections[target]["size"]:
            continue
        for i in range(s["size"] // 12):
            off, info, add = struct.unpack_from(">IIi", obj, s["offset"] + 12 * i)
            typ, symi = info & 0xFF, info >> 8
            if typ not in TYPES:
                raise ElfError("relocation type %d in %s" % (typ, s["name"]))
            sym = e.symbols[symi]
            if sym["shndx"] not in (SHN_UNDEF, SHN_ABS):
                imports.setdefault(rel_id, []).append((target, off, typ, sym["shndx"],
                                                       (sym["value"] + add) & 0xFFFFFFFF))
            else:
                if sym["shndx"] == SHN_ABS:
                    addr = sym["value"]
                elif sym["name"] in dol_symbols:
                    addr = dol_symbols[sym["name"]]
                else:
                    raise ElfError("%s: not in the DOL's symbols" % sym["name"])
                imports.setdefault(0, []).append((target, off, typ, 0, (addr + add) & 0xFFFFFFFF))
    entry = {}
    for nm in ("_prolog", "_epilog", "_unresolved"):
        for sym in e.symbols:
            if sym["name"] == nm and sym["shndx"] not in (SHN_UNDEF, SHN_ABS):
                entry[nm] = (sym["shndx"], sym["value"])
    # sections given to rel.build start at 1 (its section 0 is the empty one), as the ELF's do
    out = rel.build(rel_id, sections, imports, prolog=entry.get("_prolog"),
                    epilog=entry.get("_epilog"), unresolved=entry.get("_unresolved"), bss_size=bss_size,
                    version=version)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(prog="gc_tools.elf2rel", description=__doc__.splitlines()[0])
    ap.add_argument("obj")
    ap.add_argument("dol_elf")
    ap.add_argument("out")
    ap.add_argument("--id", type=int, required=True)
    a = ap.parse_args(argv)
    with open(a.obj, "rb") as f:
        obj = f.read()
    with open(a.dol_elf, "rb") as f:
        syms = Elf(f.read()).symbol_addrs()
    with open(a.out, "wb") as f:
        f.write(elf2rel(obj, syms, a.id))
    return 0


if __name__ == "__main__":
    sys.exit(main())
