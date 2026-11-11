"""XBE section + XPR0 decode (mirror of TitleArt.java) for Mac previews. Usage: xart.py <xbe-or-xbx> <outdir>
Reads your own game's files; keep the PNGs it writes out of the repo."""
import struct, sys, os
from PIL import Image

def le32(b, o): return struct.unpack_from('<I', b, o)[0]

def xbe_sections(b):
    if b[:4] != b'XBEH': return []
    base = le32(b, 0x104); n = le32(b, 0x11c); secs = le32(b, 0x120) - base
    out = []
    for i in range(n):
        sh = secs + 56 * i
        vaddr, vsize, raw, rsize, nameaddr = struct.unpack_from('<IIIII', b, sh + 4)
        no = nameaddr - base
        name = b[no:b.index(b'\0', no)].decode('latin1')
        out.append((name, raw, rsize, vaddr, vsize))
    return out

def xbe_cert(b):
    base = le32(b, 0x104); c = le32(b, 0x118) - base
    tid = le32(b, c + 8)
    name = b[c + 0xc:c + 0xc + 80].decode('utf-16le').split('\0')[0]
    return tid, name

def rgb565(v):
    r, g, bl = v >> 11 & 31, v >> 5 & 63, v & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, bl << 3 | bl >> 2, 255)

def dxt(b, at, fmt, w, h):
    img = Image.new('RGBA', (w, h)); px = img.load()
    bw, bh = (w + 3) // 4, (h + 3) // 4; bs = 8 if fmt == 0x0C else 16
    for by in range(bh):
        for bx in range(bw):
            o = at + (by * bw + bx) * bs
            alpha = [255] * 16
            if fmt == 0x0E:
                for i in range(16): alpha[i] = ((b[o + i // 2] >> ((i & 1) * 4)) & 0xf) * 17
            elif fmt == 0x0F:
                a0, a1 = b[o], b[o + 1]; bits = int.from_bytes(b[o + 2:o + 8], 'little')
                for i in range(16):
                    c = bits >> (3 * i) & 7
                    if c == 0: a = a0
                    elif c == 1: a = a1
                    elif a0 > a1: a = ((8 - c) * a0 + (c - 1) * a1) // 7
                    elif c == 6: a = 0
                    elif c == 7: a = 255
                    else: a = ((6 - c) * a0 + (c - 1) * a1) // 5
                    alpha[i] = a
            c = o if fmt == 0x0C else o + 8
            c0, c1 = struct.unpack_from('<HH', b, c)
            p0, p1 = rgb565(c0), rgb565(c1)
            mix = lambda a, bb, wa, wb, d: tuple((a[k] * wa + bb[k] * wb) // d for k in range(3)) + (255,)
            if c0 > c1 or fmt != 0x0C: pal = [p0, p1, mix(p0, p1, 2, 1, 3), mix(p0, p1, 1, 2, 3)]
            else: pal = [p0, p1, mix(p0, p1, 1, 1, 2), (0, 0, 0, 0)]
            idx = le32(b, c + 4)
            for i in range(16):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x >= w or y >= h: continue
                col = pal[idx >> (2 * i) & 3]
                if fmt != 0x0C: col = col[:3] + (alpha[i],)
                px[x, y] = col
    return img

def xpr(b):
    if b[:4] != b'XPR0': return None, 'not XPR0 (%r)' % b[:4]
    hdr = le32(b, 8); o = 12
    while o + 20 <= hdr:
        common = le32(b, o)
        if common == 0xFFFFFFFF: break
        if (common >> 16 & 7) == 4:
            data, _, fmtw, size = struct.unpack_from('<IIII', b, o + 4)
            fmt = fmtw >> 8 & 0xff; w = 1 << (fmtw >> 20 & 0xf); h = 1 << (fmtw >> 24 & 0xf)
            if size: w = (size & 0xfff) + 1; h = (size >> 12 & 0xfff) + 1
            info = 'fmt 0x%02X %dx%d data@%d hdr %d total %d' % (fmt, w, h, data, hdr, len(b))
            if fmt in (0x0C, 0x0E, 0x0F): return dxt(b, hdr + data, fmt, w, h), info
            return None, info + ' (not decoded here)'
        o += 20
    return None, 'no texture'

if __name__ == '__main__':
    src, out = sys.argv[1], sys.argv[2]; os.makedirs(out, exist_ok=True)
    b = open(src, 'rb').read()
    stem = os.path.splitext(os.path.basename(src))[0]
    if b[:4] == b'XBEH':
        tid, name = xbe_cert(b); print('title id %08X name %r' % (tid, name))
        for name, raw, rsize, va, vs in xbe_sections(b):
            if name.startswith('$$') or 'IMAGE' in name.upper(): print('section', name, 'raw', hex(raw), rsize)
            if name in ('$$XTIMAGE', '$$XSIMAGE'):
                img, info = xpr(b[raw:raw + rsize]); print('  ', info)
                if img: img.save(os.path.join(out, stem + '-' + name.strip('$') + '.png'))
        print('all sections:', ' '.join(s[0] for s in xbe_sections(b)))
    else:
        img, info = xpr(b); print(info)
        if img: img.save(os.path.join(out, stem + '.png'))
