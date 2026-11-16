"""
PE front end of the x86 pipeline (XBA-233, epic XBA-231: Windows PC games).

A 32-bit Windows executable (PE32, i386) read into what the XBE parser gives the
rest of the pipeline: the image base, the entry point, the sections with their
virtual and file ranges, and the imports. On top of that, what only a PE has:

  * the import table by DLL, each import by name (with its hint) or by ordinal,
    with the address of its IAT slot (the `call [slot]` the code makes);
  * the base relocations (the addresses of the 32-bit words the loader rebases),
    so a loader that maps the image elsewhere can fix it up;
  * the TLS directory, if any (callbacks the loader runs before the entry point).

`open_image` in tools/xbe_parser dispatches here on the `MZ` magic, so
tools.disasm, tools.func_id, tools.abi_analysis and tools.recomp take a PE the
way they take an XBE: `PEImage.as_xbe()` is the duck-typed view they read
(header.entry_point, header.base_address, sections with XBE flag bits).

    python3 -m tools.pe_parser hello.exe --json hello_analysis.json
"""

import argparse
import json
import os
import sys
from dataclasses import dataclass, field
from types import SimpleNamespace
from typing import List, Optional

PE_MAGIC = b"MZ"

IMAGE_FILE_MACHINE_I386 = 0x014C
IMAGE_NT_OPTIONAL_HDR32_MAGIC = 0x10B

IMAGE_SCN_CNT_CODE = 0x00000020
IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080
IMAGE_SCN_MEM_DISCARDABLE = 0x02000000
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_READ = 0x40000000
IMAGE_SCN_MEM_WRITE = 0x80000000

IMAGE_REL_BASED_ABSOLUTE = 0
IMAGE_REL_BASED_HIGHLOW = 3

# The XBE section flag bits (tools/xbe_parser), so the tools that test them read a PE section the same way.
XBE_WRITABLE, XBE_PRELOAD, XBE_EXECUTABLE = 0x1, 0x2, 0x4


@dataclass
class PESection:
    name: str
    virtual_addr: int       # absolute VA (image base + RVA), as an XBE section's
    virtual_size: int       # what the loader maps (VirtualSize, or the raw size when that is 0)
    raw_addr: int           # file offset
    raw_size: int           # file bytes the loader copies (at most virtual_size); the rest is zero fill
    characteristics: int

    @property
    def executable(self) -> bool:
        return bool(self.characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE))

    @property
    def writable(self) -> bool:
        return bool(self.characteristics & IMAGE_SCN_MEM_WRITE)

    @property
    def flags(self) -> int:
        """The XBE flag bits: every PE section is in memory at load (PRELOAD)."""
        return (XBE_PRELOAD | (XBE_EXECUTABLE if self.executable else 0)
                | (XBE_WRITABLE if self.writable else 0))


@dataclass
class PEImport:
    dll: str                # as the import descriptor names it, e.g. "KERNEL32.dll"
    name: Optional[str]     # None for an import by ordinal
    ordinal: Optional[int]  # set for an import by ordinal
    hint: int
    thunk_addr: int         # VA of its IAT slot

    @property
    def label(self) -> str:
        """`WriteFile`, or `WS2_32_ord115` for an import by ordinal (a C identifier either way: tools.disasm
        makes labels of them)."""
        if self.name:
            return self.name
        stem = "".join(c if c.isalnum() else "_" for c in self.dll.rsplit(".", 1)[0])
        return f"{stem}_ord{self.ordinal}"


@dataclass
class PETls:
    start: int              # raw data template, VAs
    end: int
    index_addr: int
    callbacks: List[int]
    zero_fill_size: int


@dataclass
class PEImage:
    path: str
    machine: int
    subsystem: int
    base_address: int
    image_size: int
    headers_size: int
    entry_point: int        # VA
    sections: List[PESection]
    imports: List[PEImport]
    relocations: List[int]  # VAs of the 32-bit words a rebase fixes up (HIGHLOW)
    iat_addr: int           # the IAT directory (VA), 0 when the image has none
    iat_size: int
    tls: Optional[PETls]
    stack_reserve: int
    stack_commit: int
    heap_reserve: int
    raw_data: bytes
    warnings: List[str] = field(default_factory=list)

    def section_at(self, va: int) -> Optional[PESection]:
        for s in self.sections:
            if s.virtual_addr <= va < s.virtual_addr + s.virtual_size:
                return s
        return None

    def read(self, va: int, size: int) -> Optional[bytes]:
        """Bytes at a VA as the loader maps them (the headers, then the sections with their zero fill)."""
        if self.base_address <= va and va + size <= self.base_address + self.headers_size:
            off = va - self.base_address
            return self.raw_data[off:off + size]
        s = self.section_at(va)
        if s is None or va + size > s.virtual_addr + s.virtual_size:
            return None
        off = va - s.virtual_addr
        data = self.raw_data[s.raw_addr + off:s.raw_addr + min(off + size, s.raw_size)] if off < s.raw_size else b""
        return data + bytes(size - len(data))

    def as_xbe(self):
        """The XBEFile-shaped view the pipeline's configure_from_xbe and loaders read."""
        header = SimpleNamespace(
            magic=PE_MAGIC, base_address=self.base_address, headers_size=self.headers_size,
            image_size=self.image_size, entry_point=self.entry_point,
            kernel_thunk_addr=self.iat_addr or (self.imports[0].thunk_addr if self.imports else 0),
            is_debug=False, entry_key="pe", timestamp=0)
        sections = [SimpleNamespace(name=s.name, flags=s.flags, virtual_addr=s.virtual_addr,
                                    virtual_size=s.virtual_size, raw_addr=s.raw_addr, raw_size=s.raw_size)
                    for s in self.sections]
        imports = [SimpleNamespace(ordinal=i.ordinal or 0, name=i.label, thunk_addr=i.thunk_addr)
                   for i in self.imports]
        return SimpleNamespace(header=header, sections=sections, kernel_imports=imports, libraries=[],
                               certificate=None, tls=None, raw_data=self.raw_data,
                               warnings=list(self.warnings), pe=self)


def is_pe(data: bytes) -> bool:
    return data[:2] == PE_MAGIC


def _section_name(raw: bytes) -> str:
    return raw.rstrip(b"\0").decode("latin-1")


def parse_pe(source, path: str = "image.exe") -> PEImage:
    """Parse a PE32 i386 image from a path or its bytes. Raises ValueError on anything else."""
    try:
        import pefile
    except ImportError as e:  # pragma: no cover - CI and the deps images have it
        raise RuntimeError("the PE front end needs pefile (pip install pefile)") from e

    if isinstance(source, (bytes, bytearray)):
        data = bytes(source)
    else:
        path = source
        with open(source, "rb") as f:
            data = f.read()
    if not is_pe(data):
        raise ValueError(f"{path}: not a PE (no MZ magic)")
    try:
        pe = pefile.PE(data=data, fast_load=True)
    except pefile.PEFormatError as e:
        raise ValueError(f"{path}: {e}") from e
    if pe.FILE_HEADER.Machine != IMAGE_FILE_MACHINE_I386:
        raise ValueError(f"{path}: machine 0x{pe.FILE_HEADER.Machine:04X}, not i386 (only 32-bit x86 PEs lift)")
    opt = pe.OPTIONAL_HEADER
    if opt.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC:
        raise ValueError(f"{path}: not a PE32 optional header (0x{opt.Magic:X})")
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_TLS"],
    ])

    base = opt.ImageBase
    warnings = []
    sections = []
    for s in pe.sections:
        vsize = s.Misc_VirtualSize or s.SizeOfRawData
        raw = 0 if s.Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA else min(s.SizeOfRawData, vsize)
        if s.PointerToRawData + raw > len(data):
            warnings.append(f"section {_section_name(s.Name)}: raw data past the end of the file, cut")
            raw = max(0, len(data) - s.PointerToRawData)
        sections.append(PESection(_section_name(s.Name), base + s.VirtualAddress, vsize,
                                  s.PointerToRawData if raw else 0, raw, s.Characteristics))

    imports = []
    for desc in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        dll = desc.dll.decode("latin-1")
        for imp in desc.imports:
            by_ordinal = imp.import_by_ordinal
            imports.append(PEImport(dll, None if by_ordinal else imp.name.decode("latin-1"),
                                    imp.ordinal if by_ordinal else None, imp.hint or 0, imp.address))

    relocations = []
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for e in block.entries:
            if e.type == IMAGE_REL_BASED_HIGHLOW:
                relocations.append(base + e.rva)
            elif e.type != IMAGE_REL_BASED_ABSOLUTE:
                warnings.append(f"base relocation type {e.type} at 0x{base + e.rva:08X} not handled")

    tls = None
    if hasattr(pe, "DIRECTORY_ENTRY_TLS"):
        t = pe.DIRECTORY_ENTRY_TLS.struct
        callbacks = []
        cb = t.AddressOfCallBacks
        while cb:
            try:
                fn = pe.get_dword_at_rva(cb - base)
            except Exception:
                fn = None
            if not fn:
                break
            callbacks.append(fn)
            cb += 4
        tls = PETls(t.StartAddressOfRawData, t.EndAddressOfRawData, t.AddressOfIndex, callbacks,
                    t.SizeOfZeroFill)

    iat = opt.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IAT"]]
    return PEImage(
        path=path, machine=pe.FILE_HEADER.Machine, subsystem=opt.Subsystem, base_address=base,
        image_size=opt.SizeOfImage, headers_size=opt.SizeOfHeaders,
        entry_point=base + opt.AddressOfEntryPoint, sections=sections, imports=imports,
        relocations=sorted(relocations), iat_addr=base + iat.VirtualAddress if iat.VirtualAddress else 0,
        iat_size=iat.Size, tls=tls, stack_reserve=opt.SizeOfStackReserve, stack_commit=opt.SizeOfStackCommit,
        heap_reserve=opt.SizeOfHeapReserve, raw_data=data, warnings=warnings)


def analysis(img: PEImage) -> dict:
    """The analysis JSON: the keys tools.xbe_parser --json writes (what tools.disasm loads), plus the PE's own."""
    title = os.path.splitext(os.path.basename(img.path))[0]
    return {
        "format": "pe",
        "title": title,
        "title_id": "0x00000000",
        "type": "pe",
        "key": "pe",
        "xdk_version": 0,
        "build_path": "",
        "base_address": f"0x{img.base_address:08X}",
        "image_size": img.image_size,
        "entry_point": f"0x{img.entry_point:08X}",
        "kernel_thunk_addr": f"0x{img.as_xbe().header.kernel_thunk_addr:08X}",
        "subsystem": img.subsystem,
        "sections": [
            {
                "name": s.name,
                "virtual_addr": f"0x{s.virtual_addr:08X}",
                "virtual_size": s.virtual_size,
                "raw_addr": f"0x{s.raw_addr:08X}",
                "raw_size": s.raw_size,
                "flags": ", ".join(f for f, on in (("W", s.writable), ("PRE", True), ("X", s.executable)) if on),
                "writable": s.writable,
                "executable": s.executable,
                "preload": True,
                "characteristics": f"0x{s.characteristics:08X}",
            }
            for s in img.sections
        ],
        "overlaps": [],
        "warnings": list(img.warnings),
        "libraries": [],
        # Named kernel_imports so tools.disasm labels the IAT slots and knows the calls that never return.
        "kernel_imports": [
            {"ordinal": i.ordinal or 0, "name": i.label, "dll": i.dll, "thunk_addr": f"0x{i.thunk_addr:08X}"}
            for i in img.imports
        ],
        "imports": [
            {"dll": i.dll, "name": i.name, "ordinal": i.ordinal, "hint": i.hint,
             "thunk_addr": f"0x{i.thunk_addr:08X}"}
            for i in img.imports
        ],
        "relocations": len(img.relocations),
        "tls": {
            "index_addr": f"0x{img.tls.index_addr:08X}",
            "zero_fill_size": img.tls.zero_fill_size,
            "callbacks": [f"0x{c:08X}" for c in img.tls.callbacks],
        } if img.tls else None,
    }


def print_info(img: PEImage) -> None:
    print(f"PE32 i386 image: {img.path}")
    print(f"  Image base:   0x{img.base_address:08X}  size 0x{img.image_size:X}")
    print(f"  Entry point:  0x{img.entry_point:08X}")
    print(f"  Subsystem:    {img.subsystem}")
    print(f"  Sections ({len(img.sections)}):")
    for s in img.sections:
        print(f"    {s.name:<10} VA 0x{s.virtual_addr:08X} vsize 0x{s.virtual_size:06X} "
              f"raw 0x{s.raw_addr:06X}+0x{s.raw_size:06X} "
              f"{'X' if s.executable else '-'}{'W' if s.writable else '-'}")
    print(f"  Imports ({len(img.imports)}):")
    for i in img.imports:
        print(f"    0x{i.thunk_addr:08X}  {i.dll}!{i.name or '#%d' % i.ordinal}")
    print(f"  Base relocations: {len(img.relocations)}")
    if img.tls:
        print(f"  TLS: {len(img.tls.callbacks)} callback(s)")
    for w in img.warnings:
        print(f"  warning: {w}")


def main(argv=None):
    ap = argparse.ArgumentParser(description="PE32 (i386) front end of the x86 pipeline")
    ap.add_argument("pe_path")
    ap.add_argument("--json", metavar="FILE", help="write the analysis JSON tools.disasm reads")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    if not os.path.exists(args.pe_path):
        print(f"Error: File not found: {args.pe_path}", file=sys.stderr)
        return 1
    img = parse_pe(args.pe_path)
    if not args.quiet:
        print_info(img)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(analysis(img), f, indent=2)
        print(f"JSON exported to: {args.json}")
    return 0
