"""
Synthetic XBEs for tests (XBA-124): our own writer, no game data.

    build_xbe([Sec(".text", code), Sec("DATA", b"...", flags=W | PRELOAD)], debug=True)

The header page holds, in order: the fixed header fields, the certificate,
the section headers, the section names, the shared-page reference counts,
the library table, the kernel thunk table and the TLS directory. Sections
follow in the file (16-byte aligned) and, unless given a VA, in memory right
after the header, page aligned. Every odd case the parser and the loader must
survive is a keyword: a debug key pair, a header bigger than a page, no
certificate or a short one, no library table, sections at the same VA.

tools/xbe_ident/test_xbe_ident.make_xbe stays the minimal one-section
writer the older tests use.
"""

import struct
from dataclasses import dataclass
from typing import Optional

from tools.xbe_parser.xbe_parser import (ENTRY_DEBUG_XOR, ENTRY_RETAIL_XOR, SECTION_EXECUTABLE,
                                         SECTION_PRELOAD, SECTION_WRITABLE, THUNK_DEBUG_XOR,
                                         THUNK_RETAIL_XOR)

BASE = 0x00010000
W, PRELOAD, X = SECTION_WRITABLE, SECTION_PRELOAD, SECTION_EXECUTABLE

CERT_OFF = 0x180
SECT_OFF = 0x380


@dataclass
class Sec:
    name: str
    data: bytes = b""
    flags: int = PRELOAD | X
    vsize: Optional[int] = None     # default: len(data)
    va: Optional[int] = None        # default: packed after the previous one, page aligned


def _page(va):
    return va & ~0xFFF


def build_xbe(sections=None, *, debug=False, header_size=0x1000, certificate=True, cert_size=0x1D0,
              libs=(("XAPILIB", 5849, 0),), xapi_index=0, title_id=0x58424199, title="Synth",
              version=0, kernel_imports=(), tls=False, entry=None, library_table_at=None):
    """The XBE's bytes. kernel_imports: ordinals. library_table_at: an offset
    in the header for the library table (to place it past the first page of
    a bigger header); None after the other header structures."""
    sections = list(sections) if sections is not None else [Sec(".text", b"\xc3" + b"\x90" * 15)]
    if header_size & 0xFFF or header_size < 0x1000:
        raise ValueError("header_size: a whole number of pages")

    # Section VAs and sizes.
    va = BASE + header_size
    placed = []
    for s in sections:
        vsize = s.vsize if s.vsize is not None else len(s.data)
        sva = s.va if s.va is not None else va
        placed.append((s, sva, vsize))
        if s.va is None:
            va = (sva + max(vsize, 1) + 0xFFF) & ~0xFFF
    image_end = max([BASE + header_size] + [sva + vsize for _, sva, vsize in placed])

    hdr = bytearray(header_size)
    hdr[0:4] = b"XBEH"
    cursor = SECT_OFF + 56 * len(sections)

    def alloc(n, align=4):
        nonlocal cursor
        cursor = (cursor + align - 1) & ~(align - 1)
        at = cursor
        cursor += n
        if cursor > header_size:
            raise ValueError("header_size 0x%X too small for the header structures" % header_size)
        return at

    # Names.
    name_va = []
    for s, _, _ in placed:
        o = alloc(len(s.name) + 1)
        hdr[o:o + len(s.name)] = s.name.encode("ascii")
        name_va.append(BASE + o)
    # Shared-page reference counts: one u16 per page a section starts or ends
    # in, shared by the sections that start or end in it.
    page_ref = {}
    for _, sva, vsize in placed:
        for p in (_page(sva), _page(sva + max(vsize, 1) - 1)):
            if p not in page_ref:
                page_ref[p] = BASE + alloc(2, 2)
    # Library table.
    lib_va = 0
    if libs:
        o = library_table_at if library_table_at is not None else alloc(16 * len(libs))
        lib_va = BASE + o
        for i, (name, build, flags) in enumerate(libs):
            e = o + 16 * i
            hdr[e:e + 8] = name.encode("ascii")[:8].ljust(8, b"\0")
            struct.pack_into("<4H", hdr, e + 8, 1, 0, build, flags)
    # Kernel thunk table (ordinals | 0x80000000, 0-terminated).
    thunk_o = alloc(4 * (len(kernel_imports) + 1))
    for i, ordinal in enumerate(kernel_imports):
        struct.pack_into("<I", hdr, thunk_o + 4 * i, 0x80000000 | ordinal)
    # TLS directory: no template data, 16 bytes of zero fill, an index slot.
    tls_va = 0
    if tls:
        o = alloc(24 + 4)
        tls_va = BASE + o
        struct.pack_into("<6I", hdr, o, 0, 0, BASE + o + 24, 0, 16, 0)

    # Certificate.
    if certificate:
        struct.pack_into("<III", hdr, CERT_OFF, cert_size, 0, title_id)
        t = title.encode("utf-16-le")[:80]
        hdr[CERT_OFF + 12:CERT_OFF + 12 + len(t)] = t
        struct.pack_into("<I", hdr, CERT_OFF + 156, 0x80000007)        # allowed media: HDD, DVD, ...
        struct.pack_into("<I", hdr, CERT_OFF + 160, 0x7)                # every region
        struct.pack_into("<I", hdr, CERT_OFF + 172, version)

    # Raw data and section headers.
    raw = bytearray()
    raw_base = header_size
    for i, (s, sva, vsize) in enumerate(placed):
        roff = raw_base + len(raw)
        raw += s.data
        raw += b"\0" * (-len(raw) % 16)
        o = SECT_OFF + 56 * i
        struct.pack_into("<9I", hdr, o, s.flags, sva, vsize, roff if s.data else 0, len(s.data),
                         name_va[i], 0, page_ref[_page(sva)], page_ref[_page(sva + max(vsize, 1) - 1)])

    if entry is None:
        entry = next((sva for s, sva, _ in placed if s.flags & X), BASE + header_size)
    ek, tk = (ENTRY_DEBUG_XOR, THUNK_DEBUG_XOR) if debug else (ENTRY_RETAIL_XOR, THUNK_RETAIL_XOR)
    struct.pack_into("<5I", hdr, 0x104, BASE, header_size, image_end - BASE, 0x178, 0)
    struct.pack_into("<3I", hdr, 0x118, BASE + CERT_OFF if certificate else 0, len(sections), BASE + SECT_OFF)
    struct.pack_into("<3I", hdr, 0x124, 0, (entry ^ ek) & 0xFFFFFFFF, tls_va)
    struct.pack_into("<I", hdr, 0x158, ((BASE + thunk_o) ^ tk) & 0xFFFFFFFF)
    struct.pack_into("<II", hdr, 0x160, len(libs) if libs else 0, lib_va)
    if libs and xapi_index is not None:
        struct.pack_into("<I", hdr, 0x16C, lib_va + 16 * xapi_index)
    return bytes(hdr + raw)


def write_xbe(path, *args, **kw):
    data = build_xbe(*args, **kw)
    with open(path, "wb") as f:
        f.write(data)
    return str(path)
