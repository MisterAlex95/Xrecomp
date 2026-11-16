#!/usr/bin/env python3
"""Compares two 8-bit RGB / RGBA PNGs pixel for pixel: `png_compare.py GOT EXPECTED [--update]` (XBA-223; a copy
of consoles/nds/tests/png_compare.py, so the PSP spike's test project stays self-contained, XBA-224).

Exit 0 when the pictures are equal, 1 with the count and bounding box of the differing pixels, 2 on a bad file.
--update writes GOT's pixels to EXPECTED, deflate-compressed (psp_run writes stored blocks: big, not for the repo).
Standard library only.
"""

import struct
import sys
import zlib


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos, idat, w = 8, b"", None
    while pos < len(data):
        n, kind = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _c, _f, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError("%s: only 8-bit RGB / RGBA, not interlaced" % path)
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if w is None:
        raise ValueError("%s: no IHDR" % path)
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([bytes(line[j:j + 3]) for j in range(0, stride, bpp)])
        prev = line
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\0" + b"".join(r) for r in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        return 2
    got, exp = argv[0], argv[1]
    try:
        gw, gh, grows = read_png(got)
        if "--update" in argv[2:]:
            write_png(exp, gw, gh, grows)
            print("wrote %s" % exp)
            return 0
        ew, eh, erows = read_png(exp)
    except (OSError, ValueError, zlib.error) as e:
        print("png_compare: %s" % e, file=sys.stderr)
        return 2
    if (gw, gh) != (ew, eh):
        print("png_compare: size %dx%d, expected %dx%d" % (gw, gh, ew, eh))
        return 1
    bad = [(x, y) for y in range(gh) for x in range(gw) if grows[y][x] != erows[y][x]]
    if bad:
        xs, ys = [p[0] for p in bad], [p[1] for p in bad]
        print("png_compare: %d pixels differ, in x %d..%d y %d..%d" % (len(bad), min(xs), max(xs), min(ys), max(ys)))
        return 1
    print("png_compare: %s matches %s (%dx%d)" % (got, exp, gw, gh))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
