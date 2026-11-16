"""GameCube disc images for tests (XBA-261): a GCM built from a DOL and files, with an apploader of ours, and its
conversions to CISO, GCZ, RVZ and WIA. No game data: every byte comes from our own programs and generators.

The readers under test are consoles/gc/runtime/gc_disc*.c; this is the writing side, written independently from the
same public format descriptions (Dolphin's docs/WiaAndRvz.md for WIA / RVZ; the GCZ and CISO layouts):

  GCM   header (game code, maker, disc number, revision, magic 0xC2339F3D at 0x1C, name, DOL / FST offsets), bi2 at
        0x440, the apploader at 0x2440 (date, entry, size, trailer, code for 0x81200000), the DOL, the FST, files at
        32 KiB boundaries; the gaps hold padding from the discs' lagged Fibonacci generator, reseeded every 32 KiB,
        as RVZ packing expects
  CISO  "CISO", block size, map; blocks of zeros left out
  GCZ   zlib blocks, a block stored when zlib does not shrink it
  RVZ   Zstandard (raw / RLE blocks: Python has no zstd encoder), LZMA, LZMA2, bzip2 or none; groups that are all
        zeros stored as size 0, groups that do not shrink stored as is, padding RVZ-packed as seeds
  WIA   none, purge, bzip2, LZMA, LZMA2 (no packing)

The apploader (built with tools.ppc.asm): entry fills init / main / close; init reports through the IPL's callback;
main walks a table of (destination, length, offset) reads computed here (the DOL's sections, then the FST under the
top of memory); close sets the FST pointer at 0x80000038 / 0x8000003C and returns the DOL's entry.

CLI (tests): python3 -m gc_tools.disc testset OUTDIR [--dol HOMEBREW.dol] writes test.gcm, its conversions and
expected.txt (the listing gc_disc_info must print for every one of them).
"""

import argparse
import bz2
import hashlib
import lzma
import os
import struct
import sys
import zlib

from . import CONSOLE  # noqa: F401
from tools.ppc import asm

from .dol import Image, build as build_dol

MAGIC = 0xC2339F3D
APPLOADER_BASE = 0x81200000
TOP = 0x81800000
BLOCK = 0x8000

# ---- the padding generator (lagged Fibonacci, XOR, j = 32, k = 521) ----


def _forward(b):
    for i in range(32):
        b[i] ^= b[i + 489]
    for i in range(32, 521):
        b[i] ^= b[i - 32]


def lfg_bytes(seed, skip, n):
    """n bytes of the generator seeded with seed (68 bytes), after skipping skip bytes."""
    b = list(struct.unpack(">17I", seed)) + [0] * 504
    for i in range(17, 521):
        b[i] = ((b[i - 17] << 23) & 0xFFFFFFFF) ^ (b[i - 16] >> 9) ^ b[i - 1]
    for _ in range(4):
        _forward(b)
    while skip >= 2084:
        _forward(b)
        skip -= 2084
    out = bytearray()
    pos = skip
    while len(out) < n + pos:
        for w in b:
            out += bytes(((w >> 24) & 255, (w >> 18) & 255, (w >> 8) & 255, w & 255))
        _forward(b)
    # out holds the stream from the current buffer's start
    return bytes(out[pos:pos + n])


def block_seed(game_id, blk):
    """Our padding's seed for 32 KiB block blk (any 68 bytes do: RVZ stores them)."""
    h = b""
    k = 0
    while len(h) < 68:
        h += hashlib.sha256(b"%s/%d/%d" % (game_id, blk, k)).digest()
        k += 1
    return h[:68]


# ---- the apploader ----


def _cmpwi(crf, ra, simm):
    return (11 << 26) | (crf << 23) | ((ra & 31) << 16) | (simm & 0xFFFF)


def _stwu(rs, d, ra):
    return (37 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)


def _andi_(ra, rs, uimm):
    return (28 << 26) | (rs << 21) | (ra << 16) | (uimm & 0xFFFF)


def _beq(at, target):
    return asm.bc(at, 12, 2, target)


def _bne(at, target):
    return asm.bc(at, 4, 2, target)


BCTRL = 0x4E800421


def _la(r, v):
    return [asm.lis(r, (v >> 16) & 0xFFFF), asm.ori(r, r, v & 0xFFFF)]


def apploader(reads, entry, fst_addr, fst_max):
    """The apploader body (for 0x81200000) and its entry address.

    Layout: entry, init, main, close, the message, then the state block {read index (bytes), DOL entry, FST address,
    FST max size} and the read table {destination, length, disc offset} ending with a zero length."""
    msg = b"apploader of XRecomp's tests: %d reads\0"
    msg += b"\0" * (-len(msg) % 4)
    entry_at = APPLOADER_BASE
    init_at = entry_at + 4 * 10
    main_at = init_at + 4 * 12
    done_at = main_at + 4 * 16
    close_at = done_at + 4 * 2
    msg_at = close_at + 4 * 9
    state = msg_at + len(msg)
    w = []
    for slot, fn in ((3, init_at), (4, main_at), (5, close_at)):  # entry(&init, &main, &close)
        w += _la(6, fn) + [asm.stw(6, 0, slot)]
    w += [asm.blr()]
    assert APPLOADER_BASE + 4 * len(w) == init_at
    w += [_stwu(1, -16, 1), asm.mflr(0), asm.stw(0, 20, 1), asm.mtctr(3)] + _la(3, msg_at)  # init(report)
    w += [asm.li(4, len(reads)), BCTRL, asm.lwz(0, 20, 1), asm.mtlr(0), asm.addi(1, 1, 16), asm.blr()]
    assert APPLOADER_BASE + 4 * len(w) == main_at
    w += _la(6, state) + [asm.lwz(7, 0, 6), asm.add(8, 6, 7), asm.lwz(9, 20, 8), _cmpwi(0, 9, 0)]  # main(...)
    w += [_beq(APPLOADER_BASE + 4 * len(w), done_at)]
    w += [asm.lwz(10, 16, 8), asm.stw(10, 0, 3), asm.stw(9, 0, 4), asm.lwz(10, 24, 8), asm.stw(10, 0, 5),
          asm.addi(7, 7, 12), asm.stw(7, 0, 6), asm.li(3, 1), asm.blr()]
    assert APPLOADER_BASE + 4 * len(w) == done_at
    w += [asm.li(3, 0), asm.blr()]
    w += _la(6, state) + [asm.lwz(7, 8, 6), asm.lwz(8, 12, 6), asm.lis(9, 0x8000), asm.stw(7, 0x38, 9),  # close()
                          asm.stw(8, 0x3C, 9), asm.lwz(3, 4, 6), asm.blr()]
    assert APPLOADER_BASE + 4 * len(w) == msg_at
    w += list(struct.unpack(">%dI" % (len(msg) // 4), msg))
    w += [0, entry, fst_addr, fst_max]
    for dst, length, off in reads:
        w += [dst, length, off]
    w += [0, 0, 0]
    return struct.pack(">%dI" % len(w), *w), entry_at


# ---- GCM ----


def build_fst(files):
    """files: [(path, offset, size)], paths with '/' (directories implied, in order). The FST bytes."""
    entries = [None]  # root
    names = bytearray()
    stack = []  # (dir path, entry index)

    def name_off(n):
        o = len(names)
        names.extend(n.encode() + b"\0")
        return o

    tree = {}
    order = []
    for path, off, size in files:
        parts = path.split("/")
        node = tree
        for d in parts[:-1]:
            node = node.setdefault(d, {})
        node[parts[-1]] = (off, size)
        order.append(path)

    def emit(node, parent):
        for nm, v in node.items():
            if isinstance(v, dict):
                idx = len(entries)
                entries.append([1, name_off(nm), parent, 0])
                emit(v, idx)
                entries[idx][3] = len(entries)
            else:
                entries.append([0, name_off(nm), v[0], v[1]])

    emit(tree, 0)
    entries[0] = [1, 0, 0, len(entries)]
    out = bytearray()
    for kind, no, a, b in entries:
        out += struct.pack(">III", (kind << 24) | no, a, b)
    del stack
    return bytes(out + names)


class Gcm:
    """A built image: bytes, and where things are."""

    def __init__(self, data, files, dol_off, fst_off, fst_size, fst_addr, junk):
        self.data, self.files = data, files
        self.dol_off, self.fst_off, self.fst_size, self.fst_addr = dol_off, fst_off, fst_size, fst_addr
        self.junk = junk  # [(start, end)] padding ranges (each within one 32 KiB block)


def build_gcm(dol, files, game_id=b"GXRE", maker=b"01", disc=0, revision=0, name=b"XRecomp disc test",
              size=0x200000, padding=True):
    """A GCM image: dol (bytes), files [(path, bytes)]."""
    image = Image(dol)
    head = bytearray(0x440)
    head[0:4], head[4:6], head[6], head[7] = game_id, maker, disc, revision
    struct.pack_into(">I", head, 0x1C, MAGIC)
    head[0x20:0x20 + len(name)] = name
    bi2 = bytearray(0x2000)
    struct.pack_into(">I", bi2, 0x04, 0x01800000)  # simulated memory size
    dol_off = 0x10000
    # files at 32 KiB boundaries after the DOL, the FST after the files
    pos = (dol_off + len(dol) + BLOCK - 1) & ~(BLOCK - 1)
    placed = []
    for path, data in files:
        placed.append((path, pos, len(data), data))
        pos = (pos + len(data) + BLOCK - 1) & ~(BLOCK - 1)
    fst = build_fst([(p, o, n) for p, o, n, _ in placed])
    fst_off = pos
    fst_max = (len(fst) + 31) & ~31
    fst_addr = (TOP - fst_max) & ~31
    end = fst_off + len(fst)
    if end > size:
        raise ValueError("files do not fit in 0x%X bytes" % size)
    reads = [(s.addr, len(s.data), dol_off + s.offset) for s in image.sections]
    reads.append((fst_addr, len(fst), fst_off))
    body, entry = apploader(reads, image.entry, fst_addr, fst_max)
    app = bytearray(0x20)
    app[0:10] = b"2026/11/16"
    struct.pack_into(">III", app, 0x10, entry, len(body), 0)
    struct.pack_into(">III", head, 0x420, dol_off, fst_off, len(fst))
    struct.pack_into(">I", head, 0x42C, fst_max)
    used = [(0, 0x440, bytes(head)), (0x440, 0x2440, bytes(bi2)), (0x2440, 0x2460, bytes(app)),
            (0x2460, 0x2460 + len(body), body), (dol_off, dol_off + len(dol), dol), (fst_off, end, fst)]
    used += [(o, o + n, d) for _, o, n, d in placed]
    used.sort()
    data = bytearray(size)
    junk = []
    last = 0
    for s, e, d in used:
        if s < last:
            raise ValueError("overlap at 0x%X" % s)
        if padding:
            junk += _gaps(last, s)
        data[s:e] = d
        last = e
    if padding:
        junk += _gaps(last, size)
    for s, e in junk:
        data[s:e] = lfg_bytes(block_seed(game_id, s // BLOCK), s % BLOCK, e - s)
    return Gcm(bytes(data), placed, dol_off, fst_off, len(fst), fst_addr, junk)


def _gaps(s, e):
    """[s, e) cut at 32 KiB boundaries."""
    out = []
    while s < e:
        n = min(e, (s // BLOCK + 1) * BLOCK)
        out.append((s, n))
        s = n
    return out


# ---- conversions ----


def to_ciso(iso, block=0x8000):
    n = (len(iso) + block - 1) // block
    if n > 0x8000 - 8:
        raise ValueError("too many blocks for a CISO map")
    mp = bytearray(0x8000 - 8)
    body = bytearray()
    for i in range(n):
        b = iso[i * block:(i + 1) * block].ljust(block, b"\0")
        if any(b):
            mp[i] = 1
            body += b
    return b"CISO" + struct.pack("<I", block) + bytes(mp) + bytes(body)


def to_gcz(iso, block=0x4000):
    n = (len(iso) + block - 1) // block
    ptrs, hashes, body = [], [], bytearray()
    for i in range(n):
        raw = iso[i * block:(i + 1) * block].ljust(block, b"\0")
        z = zlib.compress(raw, 9)
        stored = len(z) >= len(raw)
        d = raw if stored else z
        ptrs.append(len(body) | (1 << 63 if stored else 0))
        hashes.append(zlib.adler32(d))
        body += d
    head = struct.pack("<IIQQII", 0xB10BC001, 0, len(body), len(iso), block, n)
    return head + struct.pack("<%dQ" % n, *ptrs) + struct.pack("<%dI" % n, *hashes) + bytes(body)


METHODS = {"none": 0, "purge": 1, "bzip2": 2, "lzma": 3, "lzma2": 4, "zstd": 5}


def zstd_frame(data):
    """A Zstandard frame of raw and RLE blocks (valid zstd; no entropy coding)."""
    out = bytearray(struct.pack("<IB", 0xFD2FB528, 0xA0) + struct.pack("<I", len(data)))
    blocks = [data[i:i + 0x20000] for i in range(0, len(data), 0x20000)] or [b""]
    for k, b in enumerate(blocks):
        last = 1 if k == len(blocks) - 1 else 0
        if b and b.count(b[0:1]) == len(b):
            out += struct.pack("<I", last | (1 << 1) | (len(b) << 3))[:3] + b[0:1]
        else:
            out += struct.pack("<I", last | (len(b) << 3))[:3] + b
    return bytes(out)


LZMA_DICT = 1 << 20
LZMA2_PROP = 16  # (2 | 0) << (16 / 2 + 11) = 1 MiB


def _compress(method, data):
    if method == "none":
        return data
    if method == "purge":
        segs = bytearray()
        i = 0
        while i < len(data):
            if data[i] == 0:
                i += 1
                continue
            j = i
            while j < len(data) and (data[j] or any(data[j:j + 16])):
                j += 1
            segs += struct.pack(">II", i, j - i) + data[i:j]
            i = j
        return bytes(segs) + hashlib.sha1(segs).digest()
    if method == "bzip2":
        return bz2.compress(data, 9)
    if method == "lzma":
        return lzma.compress(data, format=lzma.FORMAT_RAW,
                             filters=[{"id": lzma.FILTER_LZMA1, "dict_size": LZMA_DICT, "lc": 3, "lp": 0, "pb": 2}])
    if method == "lzma2":
        return lzma.compress(data, format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA2, "dict_size": LZMA_DICT}])
    if method == "zstd":
        return zstd_frame(data)
    raise ValueError(method)


def _compr_data(method):
    if method == "lzma":
        return bytes([(2 * 5 + 0) * 9 + 3]) + struct.pack("<I", LZMA_DICT)
    if method == "lzma2":
        return bytes([LZMA2_PROP])
    return b""


def _rvz_pack(iso, start, end, junk, game_id):
    """The RVZ packing of iso[start:end]: padding ranges as seeds, the rest as bytes. None when there is none."""
    runs = [(s, e) for s, e in junk if s < end and e > start]
    if not runs:
        return None
    out = bytearray()
    pos = start
    for s, e in runs:
        s, e = max(s, start), min(e, end)
        if s > pos:
            out += struct.pack(">I", s - pos) + iso[pos:s]
        out += struct.pack(">I", 0x80000000 | (e - s)) + block_seed(game_id, s // BLOCK)
        pos = e
    if pos < end:
        out += struct.pack(">I", end - pos) + iso[pos:end]
    return bytes(out)


def to_wia(gcm, method="zstd", rvz=True, chunk=0x20000):
    """An RVZ (rvz=True) or WIA image of a Gcm."""
    iso = gcm.data
    game_id = iso[0:4]
    m = METHODS[method]
    if rvz and method == "purge" or not rvz and method == "zstd":
        raise ValueError("%s is not a %s method" % (method, "RVZ" if rvz else "WIA"))
    groups = []
    data = bytearray()
    for g0 in range(0, len(iso), chunk):
        g1 = min(len(iso), g0 + chunk)
        part = iso[g0:g1]
        if not any(part):
            groups.append((0, 0, 0))
            continue
        packed = _rvz_pack(iso, g0, g1, gcm.junk, game_id) if rvz else None
        src = packed if packed is not None else part
        comp = _compress(method, src)
        flag = 1
        if rvz and len(comp) >= len(src):
            comp, flag = src, 0  # stored
        while len(data) % 4:
            data.append(0)
        groups.append((len(data), len(comp) | (flag << 31 if rvz else 0), len(packed) if packed else 0))
        data += comp
    # layout: header, disc struct, group data, then the two tables (any order is allowed)
    head_len, disc_len = 0x48, 0xDC
    data_off = head_len + disc_len
    gl = [((data_off + off) // 4 if s & 0x7FFFFFFF else 0, s, p) for off, s, p in groups]
    raw = struct.pack(">QQII", 0x80, len(iso) - 0x80, 0, len(groups))
    if rvz:
        gt = b"".join(struct.pack(">III", *g) for g in gl)
    else:
        gt = b"".join(struct.pack(">II", g[0], g[1]) for g in gl)
    raw_c, gt_c = _compress(method, raw), _compress(method, gt)
    raw_off = data_off + len(data)
    gt_off = raw_off + len(raw_c)
    cd = _compr_data(method)
    disc = struct.pack(">IIII", 1, m, 9, chunk) + iso[:0x80]
    disc += struct.pack(">IIQ", 0, 0x30, 0) + bytes(20)
    disc += struct.pack(">IQI", 1, raw_off, len(raw_c))
    disc += struct.pack(">IQI", len(groups), gt_off, len(gt_c))
    disc += bytes([len(cd)]) + cd.ljust(7, b"\0")
    assert len(disc) == disc_len
    body = disc + bytes(data) + raw_c + gt_c
    total = head_len + len(body)
    head = (b"RVZ\x01" if rvz else b"WIA\x01") + struct.pack(">III", 0x01000000, 0x00030000 if rvz else 0x00090000,
                                                            disc_len)
    head += hashlib.sha1(disc).digest() + struct.pack(">QQ", len(iso), total)
    head += hashlib.sha1(head).digest()
    assert len(head) == head_len
    return head + bytes(body)


# ---- the test set ----

DI = 0xCC006000
OUT = 0x80006000
DMA = 0x00020000


def di_program(data_off, data_len):
    """A DOL that drives the DVD interface (no libogc): a read, the disc ID, an inquiry, an audio status, a bad
    command and its error, the cover; results at OUT, then idles."""
    text = 0x80003100
    w = []

    def at():
        return text + 4 * len(w)

    w += _la(20, DI) + _la(21, OUT)
    n32 = (data_len + 31) & ~31

    def command(cmd0, cmd1, cmd2, mar, length, cr):
        nonlocal w
        w += [asm.li(0, 0x54), asm.stw(0, 0x00, 20)]  # clear the interrupt bits
        for reg, v in ((0x08, cmd0), (0x0C, cmd1), (0x10, cmd2), (0x14, mar), (0x18, length)):
            w += _la(0, v) + [asm.stw(0, reg, 20)]
        w += [asm.li(0, cr), asm.stw(0, 0x1C, 20)]
        loop = at()
        w += [asm.lwz(0, 0x1C, 20), _andi_(0, 0, 1)]
        w += [_bne(at(), loop)]

    command(0xA8000000, data_off >> 2, n32, DMA, n32, 3)
    w += [asm.lwz(0, 0x00, 20), asm.stw(0, 0, 21), asm.lwz(0, 0x14, 20), asm.stw(0, 4, 21),
          asm.lwz(0, 0x18, 20), asm.stw(0, 8, 21)]
    command(0xA8000040, 0, 0x20, DMA + 0x1000, 0x20, 3)
    w += [asm.lwz(0, 0x00, 20), asm.stw(0, 12, 21)]
    command(0x12000000, 0, 0x20, DMA + 0x1100, 0x20, 3)
    command(0xE2010000, 0, 0, 0, 0, 1)
    w += [asm.lwz(0, 0x20, 20), asm.stw(0, 16, 21)]
    command(0x77000000, 0, 0, 0, 0, 1)
    w += [asm.lwz(0, 0x00, 20), asm.stw(0, 20, 21)]
    command(0xE0000000, 0, 0, 0, 0, 1)
    w += [asm.lwz(0, 0x20, 20), asm.stw(0, 24, 21), asm.lwz(0, 0x04, 20), asm.stw(0, 28, 21)]
    w += [asm.lis(22, 0x8000)]
    for k, a in enumerate((0x38, 0x34, 0x20, 0x00)):
        w += [asm.lwz(0, a, 22), asm.stw(0, 32 + 4 * k, 21)]
    w += _la(0, 0xC0DEC0DE) + [asm.stw(0, 48, 21)]
    idle = at()
    w += [asm.b(idle, idle)]
    return build_dol([(text, struct.pack(">%dI" % len(w), *w))])


def payload(n, salt):
    out = bytearray()
    k = 0
    while len(out) < n:
        out += hashlib.sha256(b"%s%d" % (salt, k)).digest()
        k += 1
    return bytes(out[:n])


def test_files():
    return [("data.bin", payload(0x3010, b"data")), ("dir/sub/hello.txt", b"hello from XRecomp's disc test\n"),
            ("dir/zeros.bin", bytes(0x40000)), ("last.bin", payload(0x777, b"last"))]


def listing(gcm, dol):
    """What gc_disc_info prints for the image (key, DOL, FST)."""
    import binascii
    hdr = gcm.data
    gid = "".join(chr(c) if 65 <= c <= 90 or 48 <= c <= 57 else "_" for c in hdr[0:6])
    key = "%s-%d" % (gid, hdr[7]) + ("-D%d" % (hdr[6] + 1) if hdr[6] else "")
    lines = ["key %s" % key, "disc %X %08x" % (len(hdr), binascii.crc32(hdr)),
             "dol %d %08x" % (len(dol), binascii.crc32(dol))]
    names = []
    for path, off, size, data in gcm.files:
        names.append((path, off, size, binascii.crc32(data)))
    for path, off, size, crc in names:
        lines.append("file %s %X %X %08x" % (path, off, size, crc))
    return "\n".join(lines) + "\n"


FORMATS = [("test.iso", None), ("test.ciso", ("ciso",)), ("test.gcz", ("gcz",)),
           ("test-zstd.rvz", ("rvz", "zstd")), ("test-lzma.rvz", ("rvz", "lzma")), ("test-lzma2.rvz", ("rvz", "lzma2")),
           ("test-bzip2.rvz", ("rvz", "bzip2")), ("test-none.rvz", ("rvz", "none")),
           ("test-purge.wia", ("wia", "purge")), ("test-bzip2.wia", ("wia", "bzip2")),
           ("test-lzma2.wia", ("wia", "lzma2"))]


def convert(gcm, spec):
    if spec is None:
        return gcm.data
    if spec[0] == "ciso":
        return to_ciso(gcm.data)
    if spec[0] == "gcz":
        return to_gcz(gcm.data)
    return to_wia(gcm, spec[1], rvz=spec[0] == "rvz")


def testset(outdir, dol=None, disc=0):
    """test.gcm (dol, or di_program()) and its conversions in outdir, expected.txt; returns the Gcm."""
    os.makedirs(outdir, exist_ok=True)
    files = test_files()
    if dol is None:
        # the DI program needs data.bin's offset: build once to place it, then again with the program
        probe = build_gcm(di_program(0, 0x3010), files, disc=disc, padding=False)
        off = [o for p, o, n, d in probe.files if p == "data.bin"][0]
        dol = di_program(off, 0x3010)
    gcm = build_gcm(dol, files, disc=disc)
    with open(os.path.join(outdir, "test.gcm"), "wb") as f:
        f.write(gcm.data)
    for name, spec in FORMATS:
        with open(os.path.join(outdir, name), "wb") as f:
            f.write(convert(gcm, spec))
    with open(os.path.join(outdir, "expected.txt"), "w") as f:
        f.write(listing(gcm, dol))
    return gcm


def main(argv=None):
    ap = argparse.ArgumentParser(prog="gc_tools.disc", description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("testset", help="test images and expected.txt")
    t.add_argument("outdir")
    t.add_argument("--dol", help="a DOL to pack instead of the DVD-interface test program")
    t.add_argument("--disc", type=int, default=0, help="disc number (0: first)")
    a = ap.parse_args(argv)
    if a.cmd == "testset":
        dol = open(a.dol, "rb").read() if a.dol else None
        testset(a.outdir, dol, a.disc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
