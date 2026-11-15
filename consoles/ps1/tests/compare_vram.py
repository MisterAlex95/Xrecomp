#!/usr/bin/env python3
"""ctest helper (XBA-200): compare_vram.py REF.png OURS.ppm [--region X0,Y0,X1,Y1] [--ignore X0,Y0,X1,Y1]...
                                            [--tolerance N]

Compares a 1024x512 VRAM picture that ps1_run --vram wrote (binary PPM) with one of JaCzekanski/ps1-tests' reference
VRAM dumps (PNG: grey, RGB, RGBA or palette, 1 to 8 bits, no interlace), channel by channel at 5 bits (what VRAM
holds; the PNGs scale 5-bit values their own way). --region limits the comparison to a rectangle (X1 / Y1 exclusive),
--ignore leaves rectangles out (several allowed), --tolerance accepts channels up to N 5-bit units apart (the
MDEC pictures, whose last unit of rounding is not the hardware's yet). Prints the number of pixels that differ and the first few; exits 0
when none does, 1 otherwise, 2 on a bad file. Pure Python (zlib), no imaging library."""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat, plte = 8, b'', b''
    width = height = depth = ctype = None
    while pos < len(data):
        n, = struct.unpack('>I', data[pos:pos + 4])
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b'IHDR':
            width, height, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if interlace:
                raise ValueError('interlaced PNG')
        elif kind == b'PLTE':
            plte = body
        elif kind == b'IDAT':
            idat += body
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, channels * depth // 8)
    stride = (width * channels * depth + 7) // 8
    raw = zlib.decompress(idat)
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(height):
        f, line = raw[p], bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        row = []
        for x in range(width):
            if depth == 8:
                s = line[x * channels:(x + 1) * channels]
                v = s[0]
            else:
                per = 8 // depth
                v = (line[x // per] >> (8 - depth * (x % per + 1))) & ((1 << depth) - 1)
                s = None
            if ctype == 3:
                row.append(tuple(plte[v * 3:v * 3 + 3]))
            elif channels < 3:
                g = v if depth == 8 else v * 255 // ((1 << depth) - 1)
                row.append((g, g, g))
            else:
                row.append(tuple(s[:3]))
        rows.append(row)
    return width, height, rows


def read_ppm(path):
    data = open(path, 'rb').read()
    magic, size, maxval, px = data.split(b'\n', 3)
    if magic != b'P6' or maxval != b'255':
        raise ValueError('not a binary 8-bit PPM')
    width, height = map(int, size.split())
    return width, height, px


def rect(text):
    x0, y0, x1, y1 = (int(v) for v in text.split(','))
    return x0, y0, x1, y1


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    region, ignore, tol, i = None, [], 0, 3
    while i < len(argv):
        if argv[i] == '--region' and i + 1 < len(argv):
            region = rect(argv[i + 1])
        elif argv[i] == '--ignore' and i + 1 < len(argv):
            ignore.append(rect(argv[i + 1]))
        elif argv[i] == '--tolerance' and i + 1 < len(argv):
            tol = int(argv[i + 1])
        else:
            print('compare_vram: bad argument', argv[i])
            return 2
        i += 2
    try:
        rw, rh, ref = read_png(argv[1])
        ow, oh, ours = read_ppm(argv[2])
    except (OSError, ValueError, KeyError, zlib.error) as e:
        print('compare_vram:', e)
        return 2
    if (rw, rh) != (ow, oh):
        print('compare_vram: sizes differ: %dx%d and %dx%d' % (rw, rh, ow, oh))
        return 2
    x0, y0, x1, y1 = region or (0, 0, rw, rh)
    bad = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if any(a <= x < c and b <= y < d for a, b, c, d in ignore):
                continue
            o = (y * ow + x) * 3
            want = tuple(v >> 3 for v in ref[y][x])
            got = tuple(v >> 3 for v in ours[o:o + 3])
            if any(abs(a - b) > tol for a, b in zip(want, got)):
                bad += 1
                if bad <= 10:
                    print('  (%d, %d): reference %s, ours %s (5-bit R, G, B)' % (x, y, want, got))
    print('compare_vram: %d pixel(s) differ%s in %d,%d..%d,%d%s' %
          (bad, ' by more than %d' % tol if tol else '', x0, y0, x1, y1,
           ' (%d rectangle(s) ignored)' % len(ignore) if ignore else ''))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
