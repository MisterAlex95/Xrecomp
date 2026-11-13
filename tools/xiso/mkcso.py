#!/usr/bin/env python3
"""Compress or split a disc image the way the usual tools do, for the tests of the readers (XBA-126).

    python3 tools/xiso/mkcso.py IN.iso OUT --format cso1|cso2|cso2-framed|zso|cci [--block 2048] [--align 2]
    python3 tools/xiso/mkcso.py IN.iso OUT.iso --split BYTES [--naming dot|part|ext|ext3]

It only ever reads the image it is given (tools/xiso/mkxiso.py's synthetic ones in the tests). Formats:

    cso1         CSO v1 (PSP tools): raw deflate, bit 31 = stored
    cso2         CSO v2 as the Xbox tools write it: LZ4, bit 31 = LZ4, stored blocks unflagged
    cso2-framed  the same, each LZ4 block keeping the 4-byte block header of an LZ4 frame
    cso2-maxcso  CSO v2 as maxcso writes it: deflate unflagged, bit 31 = LZ4 (or stored when a full block)
    zso          ZSO: LZ4, bit 31 = stored
    cci          CCI: LZ4 with a leading padding-count byte, bit 31 = LZ4

--split writes OUT's parts of BYTES each (the last one shorter): with --naming dot, OUT=Game.iso gives Game.1.iso,
Game.2.iso...; part: Game.part1.iso; ext: Game.iso.1; ext3: Game.iso.001. A compressed output can be split too
(--split with --format): the parts hold the compressed stream, as the Xbox tools' split .cso does.
Standard library only (zlib; the LZ4 compressor below is a plain greedy one).
"""
import argparse
import os
import struct
import sys
import zlib


def lz4_compress(data):
    """One LZ4 block (the format's rules: the last 5 bytes are literals, no match starts in the last 12)."""
    n = len(data)
    out = bytearray()
    table = {}
    anchor = i = 0
    limit = n - 12

    def length(v):
        while v >= 255:
            out.append(255)
            v -= 255
        out.append(v)

    while i < limit:
        key = data[i:i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is not None and i - cand <= 0xFFFF:
            m = 4
            maxm = n - 5 - i
            while m < maxm and data[cand + m] == data[i + m]:
                m += 1
            lit = i - anchor
            out.append((min(lit, 15) << 4) | min(m - 4, 15))
            if lit >= 15:
                length(lit - 15)
            out += data[anchor:i]
            out += struct.pack("<H", i - cand)
            if m - 4 >= 15:
                length(m - 4 - 15)
            i += m
            anchor = i
        else:
            i += 1
    lit = n - anchor
    out.append(min(lit, 15) << 4)
    if lit >= 15:
        length(lit - 15)
    out += data[anchor:]
    return bytes(out)


def deflate_raw(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def compress(img, fmt, block=2048, align=2):
    """The compressed file's bytes."""
    size = len(img)
    nblocks = (size + block - 1) // block
    magic = {"zso": b"ZISO", "cci": b"CCIM"}.get(fmt, b"CISO")
    version = {"cso1": 1, "cci": 1}.get(fmt, 2)
    if fmt == "cci":
        header_size = 32
        index_size = (nblocks + 1) * 4
        pos = header_size                       # data first, the index at the end
    else:
        header_size = 24
        pos = header_size + (nblocks + 1) * 4
    body = bytearray()
    index = []
    unit = 1 << align
    for b in range(nblocks):
        if pos % unit:
            pad = unit - pos % unit
            body += b"\0" * pad
            pos += pad
        raw = img[b * block:(b + 1) * block]
        entry = pos >> align
        if fmt == "cso1":
            z = deflate_raw(raw)
            data, flag = (raw, 1) if len(z) >= len(raw) else (z, 0)
        elif fmt == "cso2-maxcso":
            z = deflate_raw(raw)
            l4 = lz4_compress(raw)
            if min(len(z), len(l4)) >= len(raw):
                data, flag = raw, 1
            elif len(l4) < len(z) and b % 2:    # both kinds of blocks in one file
                data, flag = l4, 1
            else:
                data, flag = z, 0
        else:
            l4 = lz4_compress(raw)
            if fmt == "cso2-framed":
                l4 = struct.pack("<I", len(l4)) + l4
            if fmt == "cci":
                padn = (-(len(l4) + 1)) % unit
                l4 = bytes([padn]) + l4 + b"\0" * padn
            compressed = len(l4) + 12 < len(raw)
            if fmt == "zso":
                data, flag = (l4, 0) if compressed else (raw, 1)
            else:
                data, flag = (l4, 1) if compressed else (raw, 0)
        index.append(entry | (0x80000000 if flag else 0))
        body += data
        pos += len(data)
    if pos % unit:
        pad = unit - pos % unit
        body += b"\0" * pad
        pos += pad
    index.append(pos >> align)
    idx = struct.pack("<%dI" % len(index), *index)
    if fmt == "cci":
        head = magic + struct.pack("<IQQIBBxx", header_size, size, pos, block, version, align)
        return head + bytes(body) + idx
    head = magic + struct.pack("<IQIBBxx", header_size, size, block, version, align)
    return head + idx + bytes(body)


def part_name(out, k, naming):
    d, name = os.path.split(out)
    stem, ext = os.path.splitext(name)
    if naming == "dot":
        return os.path.join(d, "%s.%d%s" % (stem, k, ext))
    if naming == "part":
        return os.path.join(d, "%s.part%d%s" % (stem, k, ext))
    if naming == "ext":
        return os.path.join(d, "%s.%d" % (name, k))
    return os.path.join(d, "%s.%03d" % (name, k))


def write_split(data, out, size, naming="dot"):
    names = []
    for k, off in enumerate(range(0, len(data), size), 1):
        p = part_name(out, k, naming)
        with open(p, "wb") as f:
            f.write(data[off:off + size])
        names.append(p)
    return names


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("out")
    ap.add_argument("--format", choices=("cso1", "cso2", "cso2-framed", "cso2-maxcso", "zso", "cci"))
    ap.add_argument("--block", type=int, default=2048)
    ap.add_argument("--align", type=int, default=2)
    ap.add_argument("--split", type=lambda s: int(s, 0))
    ap.add_argument("--naming", choices=("dot", "part", "ext", "ext3"), default="dot")
    a = ap.parse_args(argv)
    with open(a.image, "rb") as f:
        data = f.read()
    if a.format:
        data = compress(data, a.format, a.block, a.align)
    if a.split:
        for p in write_split(data, a.out, a.split, a.naming):
            print(p)
    else:
        with open(a.out, "wb") as f:
            f.write(data)
        print(a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
