"""Synthetic XEX2 files for tests (XBA-230): our own writer, no console data, no real key.

    data, layout = build_xex(text, imports={"xboxkrnl.exe": [(0x5A, "thunk"), (0x1B, "variable")]},
                             compression="basic", key=TEST_KEY, functions=[(0, 12), (12, 8)])

The mapped PE image (what the XEX carries) has a fixed shape, page aligned (4 KB pages, flagged in the security
info), so tests can name every address:

    base + 0x0000   PE headers (MZ, PE, PowerPC big-endian machine 0x1F2), little-endian like any PE
    base + 0x1000   .text   the caller's code (big-endian words), then one 16-byte thunk per imported function
    next page       .rdata  the caller's read-only data, then the import records (one word per import)
    next page       .data   the caller's data
    next page       .pdata  one IMAGE_CE_RUNTIME_FUNCTION_ENTRY per function in `functions`

`layout` gives the addresses: text, rdata, data, pdata, thunks {(lib, ordinal): addr}, records {(lib, ordinal):
addr}, image_size. Encryption uses `key` (a 16-byte key the test makes up: TEST_KEY is not a console key) and a
session key derived from `seed`. Compression: "none", "basic" (pages split into data then zero runs) or "normal"
(LZX, XBA-270: the image in uncompressed LZX blocks, or `lzx_stream`, in the XEX block chain with SHA-1s).
"""

import hashlib
import struct
from types import SimpleNamespace

from . import aes, lzx
from .xex import (COMP_BASIC, COMP_NONE, COMP_NORMAL, ENC_NONE, ENC_NORMAL, H_DEFAULT_STACK_SIZE, H_ENTRY_POINT, H_EXECUTION_INFO,
                  H_FILE_FORMAT_INFO, H_IMAGE_BASE_ADDRESS, H_IMPORT_LIBRARIES, H_ORIGINAL_PE_NAME, H_SYSTEM_FLAGS,
                  H_TLS_INFO, IMAGE_PAGE_SIZE_4KB, PE_MACHINE_POWERPCBE)

BASE = 0x82000000
PAGE = 0x1000
TEST_KEY = bytes(range(0xA0, 0xB0))  # made up for tests: not any console's key

NOP = 0x60000000


def _align(n, a=PAGE):
    return (n + a - 1) & ~(a - 1)


def _version(major, minor, build, qfe):
    return (major << 28) | (minor << 24) | (build << 8) | qfe


def _pe_headers(entry_rva, sections, image_size):
    """DOS + PE headers for the mapped image; sections: [(name, rva, size, flags)]."""
    h = bytearray(PAGE)
    h[0:2] = b"MZ"
    struct.pack_into("<I", h, 0x3C, 0x80)
    h[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", h, 0x84, PE_MACHINE_POWERPCBE, len(sections), 0, 0, 0, 0xE0, 0x0102)
    o = 0x98
    code = sum(s[2] for s in sections if s[3] & 0x20)
    struct.pack_into("<HBBIIIIII", h, o, 0x10B, 8, 0, code, 0, 0, entry_rva, 0x1000, 0)
    struct.pack_into("<III", h, o + 28, BASE, PAGE, PAGE)
    struct.pack_into("<HHHHHH", h, o + 40, 4, 0, 0, 0, 4, 0)
    struct.pack_into("<IIII", h, o + 52, 0, image_size, PAGE, 0)
    struct.pack_into("<HH", h, o + 68, 14, 0)  # subsystem: Xbox
    struct.pack_into("<IIIIII", h, o + 72, 0x40000, 0x1000, 0x100000, 0x1000, 0, 16)
    for name, rva, size, flags in sections:
        if name == ".pdata":
            struct.pack_into("<II", h, o + 96 + 8 * 3, rva, size)  # exception directory
    so = o + 0xE0
    for k, (name, rva, size, flags) in enumerate(sections):
        struct.pack_into("<8sIIIIIIHHI", h, so + 40 * k, name.encode(), size, rva, _align(size), rva, 0, 0, 0, 0,
                         flags)
    return h


def _import_blob(libs):
    """xex2_opt_import_libraries for [(name, [record addrs])]."""
    names = b""
    for name, _ in libs:
        names += name.encode() + b"\0"
    names += bytes(-len(names) % 4)
    body = b""
    for k, (name, addrs) in enumerate(libs):
        ver = _version(2, 0, 1888 + k, 0)
        rec = struct.pack(">I20sIIIHH", 0x28 + 4 * len(addrs), bytes(20), 0x10000 * (k + 1), ver,
                          _version(2, 0, 1888, 0), k, len(addrs))
        rec += struct.pack(">%dI" % len(addrs), *addrs)
        body += rec
    return struct.pack(">III", 12 + len(names) + len(body), len(names), len(libs)) + names + body


def _basic_blocks(image):
    blocks, stored = [], bytearray()
    for o in range(0, len(image), PAGE):
        page = image[o:o + PAGE]
        n = len(page.rstrip(b"\0"))
        blocks.append((n, len(page) - n))
        stored += page[:n]
    return blocks, bytes(stored)


def build_xex(text, rdata=b"", data=b"", imports=None, entry=0, functions=(), compression="none", key=None,
              seed=b"xba-230", tls=None, title_id=0x58425A30, stack_size=0x40000, name="synth.exe",
              lzx_window=0x8000, lzx_stream=None):
    """text: bytes or a list of big-endian words. entry: offset in .text. functions: [(offset in .text, length in
    bytes)]. imports: {library: [(ordinal, "thunk" | "variable")]}. tls: (slots, data bytes) or None."""
    if isinstance(text, (list, tuple)):
        text = struct.pack(">%dI" % len(text), *text)
    imports = imports or {}
    libs = list(imports.items())
    lay = SimpleNamespace(base=BASE, thunks={}, records={})
    lay.text = BASE + PAGE
    n_thunks = sum(1 for _, imps in libs for _, kind in imps if kind == "thunk")
    text_size = len(text) + 16 * n_thunks
    rdata_rva = PAGE + _align(max(text_size, 4))
    n_records = sum(len(imps) for _, imps in libs)
    tls_data = tls[1] if tls else b""
    rdata_size = len(rdata) + 4 * n_records + len(tls_data)
    data_rva = rdata_rva + _align(max(rdata_size, 4))
    pdata_rva = data_rva + _align(max(len(data), 4))
    image_size = pdata_rva + _align(max(8 * len(functions), 8))
    lay.rdata, lay.data, lay.pdata, lay.image_size = BASE + rdata_rva, BASE + data_rva, BASE + pdata_rva, image_size

    img = bytearray(image_size)
    img[PAGE:PAGE + len(text)] = text
    img[rdata_rva:rdata_rva + len(rdata)] = rdata
    img[data_rva:data_rva + len(data)] = data
    thunk_at = PAGE + len(text)
    rec_at = rdata_rva + len(rdata)
    lib_records = []
    for li, (lib, imps) in enumerate(libs):
        addrs = []
        for ordinal, kind in imps:
            struct.pack_into(">I", img, rec_at, (li << 16) | ordinal)
            lay.records[(lib, ordinal)] = BASE + rec_at
            addrs.append(BASE + rec_at)
            rec_at += 4
            if kind == "thunk":
                struct.pack_into(">IIII", img, thunk_at, (1 << 24) | (li << 16) | ordinal, NOP, NOP, NOP)
                lay.thunks[(lib, ordinal)] = BASE + thunk_at
                addrs.append(BASE + thunk_at)
                thunk_at += 16
        lib_records.append((lib, addrs))
    tls_addr = BASE + rec_at
    img[rec_at:rec_at + len(tls_data)] = tls_data
    for k, fn in enumerate(functions):
        # (offset, length): the old fixed info (prolog 2 words, bit 31); XBA-282: (offset, length, prolog words, high
        # bits) to give an exception flag and a real prolog length
        off, length = fn[0], fn[1]
        prolog, high = (fn[2], fn[3]) if len(fn) > 2 else (2, 0x80000000)
        struct.pack_into(">II", img, pdata_rva + 8 * k, lay.text + off, ((length // 4) << 8) | high | (prolog & 0xFF))
    sections = [(".text", PAGE, text_size, 0x60000020), (".rdata", rdata_rva, rdata_size, 0x40000040),
                (".data", data_rva, len(data), 0xC0000040), (".pdata", pdata_rva, 8 * len(functions), 0x40000040)]
    img[0:PAGE] = _pe_headers(PAGE + entry, sections, image_size)
    image = bytes(img)
    lay.image = image

    # the stored data: compressed, then encrypted
    if compression == "basic":
        blocks, stored = _basic_blocks(image)
        ff = struct.pack(">IHH", 8 + 8 * len(blocks), ENC_NORMAL if key else ENC_NONE, COMP_BASIC)
        ff += b"".join(struct.pack(">II", a, b) for a, b in blocks)
    elif compression == "normal":
        # LZX in uncompressed blocks (lzx.store, checked with cabextract by lzx_vectors/gen.sh), or a stream given
        stream = lzx.store(image) if lzx_stream is None else lzx_stream
        payload_, first_size, first_hash = lzx.xex_reblock([stream[o:o + 0x7FF0]
                                                            for o in range(0, len(stream), 0x7FF0)])
        stored = payload_
        ff = struct.pack(">IHHII", 36, ENC_NORMAL if key else ENC_NONE, COMP_NORMAL, lzx_window, first_size)
        ff += first_hash
    elif compression == "none":
        stored = image
        ff = struct.pack(">IHH", 8, ENC_NORMAL if key else ENC_NONE, COMP_NONE)
    else:
        raise ValueError("compression: none, basic or normal")
    session = hashlib.sha256(seed).digest()[:16]
    session_enc = bytes(16)
    if key:
        stored += bytes(-len(stored) % 16)
        stored = aes.cbc_encrypt(session, stored)
        session_enc = aes.ecb_encrypt(key, session)

    # optional headers, sorted by key like the real ones
    blobs = {H_FILE_FORMAT_INFO: ff, H_IMPORT_LIBRARIES: _import_blob(lib_records) if libs else None,
             H_EXECUTION_INFO: struct.pack(">IIIIBBBBI", 0x12345678, _version(1, 0, 3, 0), 0, title_id, 0, 0, 1, 1, 0),
             H_ORIGINAL_PE_NAME: None}
    pe_name = name.encode() + b"\0"
    pe_name += bytes(-len(pe_name) % 4)
    blobs[H_ORIGINAL_PE_NAME] = struct.pack(">I", 4 + len(pe_name)) + pe_name
    if tls:
        blobs[H_TLS_INFO] = struct.pack(">IIII", tls[0], tls_addr, len(tls_data), len(tls_data))
    inline = {H_ENTRY_POINT: lay.text + entry, H_IMAGE_BASE_ADDRESS: BASE, H_DEFAULT_STACK_SIZE: stack_size,
              H_SYSTEM_FLAGS: 0x400}
    keys = sorted([k for k, v in blobs.items() if v is not None] + list(inline))
    dir_end = 0x18 + 8 * len(keys)
    out = bytearray(dir_end)
    entries = []
    for k in keys:
        if k in inline:
            entries.append((k, inline[k]))
        else:
            out += bytes(-len(out) % 4)
            entries.append((k, len(out)))
            out += blobs[k]
    out += bytes(-len(out) % 16)
    sec_off = len(out)
    npages = image_size // PAGE
    sec = bytearray(0x184 + 24 * npages)
    struct.pack_into(">II", sec, 0, len(sec), image_size)
    struct.pack_into(">III", sec, 0x108, 0x174, IMAGE_PAGE_SIZE_4KB, BASE)
    sec[0x150:0x160] = session_enc
    struct.pack_into(">I", sec, 0x180, npages)
    for p in range(npages):
        info = 1 if PAGE <= p * PAGE < rdata_rva else 2
        struct.pack_into(">I", sec, 0x184 + 24 * p, (1 << 4) | info)
    out += sec
    out += bytes(-len(out) % PAGE)
    pe_off = len(out)
    struct.pack_into(">4sIIIII", out, 0, b"XEX2", 1, pe_off, 0, sec_off, len(keys))
    for k, (key_, value) in enumerate(entries):
        struct.pack_into(">II", out, 0x18 + 8 * k, key_, value)
    return bytes(out) + stored, lay
