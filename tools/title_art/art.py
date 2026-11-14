"""
The title image of an XBE: its « $$XTIMAGE » section, an XPR0 texture, decoded
to RGBA. The Python twin of the launchers' decoders
(android/app/src/main/java/fr/adserver/xrecomp/TitleArt.java,
macos/Sources/XRecompCore/TitleArt.swift): same formats, same limits, same
fallbacks. The format notes are in TitleArt.java and docs/formats/xbe.md.

An Image is (width, height, pixels) with pixels a list of (r, g, b, a) tuples,
row by row. Pure Python: no Pillow, so it runs in the CI and on the device.
"""

import struct

SECTION = "$$XTIMAGE"
SAVE_SECTION = "$$XSIMAGE"
# The largest section or image taken (title images are 128 x 128 DXT1, 8 KB).
MAX_BYTES = 4 << 20
MAX_SIDE = 1024


class Image(object):
    __slots__ = ("w", "h", "px")

    def __init__(self, w, h, px):
        self.w, self.h, self.px = w, h, px

    def get(self, x, y):
        return self.px[y * self.w + x]


def le32(b, o):
    return struct.unpack_from("<I", b, o)[0]


# ---- XBE ------------------------------------------------------------------------------------------------------

def xbe_section(data, name):
    """The raw bytes of the XBE section called name, or None (no such section, not an XBE, out of bounds)."""
    if len(data) < 0x178 or data[:4] != b"XBEH":
        return None
    base, headers = le32(data, 0x104), le32(data, 0x108)
    n = le32(data, 0x11C)
    secs = le32(data, 0x120) - base
    head_len = min(len(data), headers)
    if n <= 0 or n > 512 or secs < 0 or headers > MAX_BYTES or secs + 56 * n > head_len:
        return None
    want = name.encode("ascii") + b"\0"
    for i in range(n):
        sh = secs + 56 * i
        name_off = le32(data, sh + 0x14) - base
        if name_off < 0 or name_off + len(want) > head_len or data[name_off:name_off + len(want)] != want:
            continue
        raw, size = le32(data, sh + 0x0C), le32(data, sh + 0x10)
        if size == 0 or size > MAX_BYTES or raw + size > len(data):
            return None
        return bytes(data[raw:raw + size])
    return None


def title_image(data):
    """The title image of XBE bytes (else the save image, which some titles fill instead), or None."""
    for name in (SECTION, SAVE_SECTION):
        sec = xbe_section(data, name)
        img = decode_xpr(sec) if sec else None
        if img is not None:
            return img
    return None


# ---- XPR0 -----------------------------------------------------------------------------------------------------

def bytes_for(fmt, w, h):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    if fmt == 0x0C:
        return bw * bh * 8
    if fmt in (0x0E, 0x0F):
        return bw * bh * 16
    if fmt in (0x06, 0x07, 0x12, 0x1E):
        return w * h * 4
    if fmt in (0x05, 0x11):
        return w * h * 2
    return -1


def decode_xpr(b):
    """The first texture of an XPR0 file as an Image, or None."""
    if len(b) < 32 or b[:4] != b"XPR0":
        return None
    hdr = le32(b, 8)
    if hdr < 12 or hdr > len(b):
        return None
    o = 12
    while o + 20 <= hdr:
        common = le32(b, o)
        if common == 0xFFFFFFFF:
            break
        if (common >> 16 & 7) != 4:         # not a texture
            o += 20
            continue
        data, _lock, fmtw, size = struct.unpack_from("<IIII", b, o + 4)
        fmt = fmtw >> 8 & 0xFF
        w, h = 1 << (fmtw >> 20 & 0xF), 1 << (fmtw >> 24 & 0xF)
        if size:
            w, h = (size & 0xFFF) + 1, (size >> 12 & 0xFFF) + 1
        if w > MAX_SIDE or h > MAX_SIDE:
            return None
        need = bytes_for(fmt, w, h)
        if need <= 0:
            return None
        at = hdr + data
        if at + need > len(b) and data >= hdr:
            at = data       # an absolute offset past the header, as some writers put it
        if at + need > len(b):
            return None
        px = decode(b, at, fmt, w, h)
        return Image(w, h, px) if px is not None else None
    return None


def rgb565(v):
    r, g, bl = v >> 11 & 31, v >> 5 & 63, v & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, bl << 3 | bl >> 2, 255)


def swizzle_masks(w, h):
    """The NV2A swizzle masks (xemu's generate_swizzle_masks): x and y bits interleaved, x first."""
    x = y = 0
    bit, i = 1, 1
    while i < w or i < h:
        if i < w:
            x |= bit
            bit <<= 1
        if i < h:
            y |= bit
            bit <<= 1
        i <<= 1
    return x, y


def deposit(v, mask):
    r, bit = 0, 1
    while mask:
        low = mask & -mask
        if v & bit:
            r |= low
        mask &= mask - 1
        bit <<= 1
    return r


def _texel(b, at, fmt, i):
    if fmt in (0x05, 0x11):
        return rgb565(b[at + 2 * i] | b[at + 2 * i + 1] << 8)
    bl, g, r, a = b[at + 4 * i:at + 4 * i + 4]        # A8R8G8B8 little-endian: B G R A
    return (r, g, bl, 255 if fmt in (0x07, 0x1E) else a)


def decode(b, at, fmt, w, h):
    if fmt in (0x0C, 0x0E, 0x0F):
        return decode_dxt(b, at, fmt, w, h)
    if fmt in (0x06, 0x07, 0x05):
        if w & (w - 1) or h & (h - 1):
            return None
        xm, ym = swizzle_masks(w, h)
        xs = [deposit(x, xm) for x in range(w)]
        return [_texel(b, at, fmt, xs[x] | deposit(y, ym)) for y in range(h) for x in range(w)]
    if fmt in (0x12, 0x1E, 0x11):
        return [_texel(b, at, fmt, i) for i in range(w * h)]
    return None


def _mix(a, c, wa, wc, d):
    return tuple((a[k] * wa + c[k] * wc) // d for k in range(3)) + (255,)


def decode_dxt(b, at, fmt, w, h):
    """DXT1 / DXT3 / DXT5 (BC1 / BC2 / BC3) blocks, row by row."""
    px = [(0, 0, 0, 0)] * (w * h)
    bw, bh = (w + 3) // 4, (h + 3) // 4
    bs = 8 if fmt == 0x0C else 16
    for by in range(bh):
        for bx in range(bw):
            o = at + (by * bw + bx) * bs
            alpha = None
            if fmt == 0x0E:
                alpha = [((b[o + i // 2] >> ((i & 1) * 4)) & 0xF) * 17 for i in range(16)]
            elif fmt == 0x0F:
                a0, a1 = b[o], b[o + 1]
                bits = int.from_bytes(b[o + 2:o + 8], "little")
                alpha = []
                for i in range(16):
                    code = bits >> (3 * i) & 7
                    if code == 0:
                        a = a0
                    elif code == 1:
                        a = a1
                    elif a0 > a1:
                        a = ((8 - code) * a0 + (code - 1) * a1) // 7
                    elif code == 6:
                        a = 0
                    elif code == 7:
                        a = 255
                    else:
                        a = ((6 - code) * a0 + (code - 1) * a1) // 5
                    alpha.append(a)
            c = o if fmt == 0x0C else o + 8
            c0, c1 = struct.unpack_from("<HH", b, c)
            p0, p1 = rgb565(c0), rgb565(c1)
            if c0 > c1 or fmt != 0x0C:
                pal = (p0, p1, _mix(p0, p1, 2, 1, 3), _mix(p0, p1, 1, 2, 3))
            else:
                pal = (p0, p1, _mix(p0, p1, 1, 1, 2), (0, 0, 0, 0))   # transparent black
            idx = le32(b, c + 4)
            for i in range(16):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x >= w or y >= h:
                    continue
                col = pal[idx >> (2 * i) & 3]
                if alpha is not None:
                    col = col[:3] + (alpha[i],)
                px[y * w + x] = col
    return px


# ---- The title's colour (the monogram's, TitleArt.titleHue) -----------------------------------------------------

def _java_hash(s):
    """String.hashCode over UTF-16 code units, as an unsigned 32-bit value."""
    b = s.encode("utf-16-be")
    h = 0
    for i in range(0, len(b), 2):
        h = (31 * h + (b[i] << 8 | b[i + 1])) & 0xFFFFFFFF
    return h


def title_hue(title_id):
    """TitleArt.titleHue: a hue (0..359) from the title ID, stable for a title, the same as the launchers'."""
    h = _java_hash((title_id or "").upper()) if title_id else 0
    h ^= h >> 16
    h = (h * 0x45D9F3B) & 0xFFFFFFFF
    h ^= h >> 16
    return (h & 0x7FFFFFFF) % 360


def hsv(hue, s, v):
    """(r, g, b) for hue in degrees, s and v in 0..1 (Theme.hsv / android.graphics.Color.HSVToColor)."""
    c = v * s
    hp = (hue % 360) / 60.0
    x = c * (1 - abs(hp % 2 - 1))
    r, g, b = [(c, x, 0), (x, c, 0), (0, c, x), (0, x, c), (x, 0, c), (c, 0, x)][int(hp) % 6]
    m = v - c
    return tuple(int(round((k + m) * 255)) for k in (r, g, b))


def monogram_color(title_id):
    return hsv(title_hue(title_id), 0.62, 0.9)
