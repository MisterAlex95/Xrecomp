"""
App icons from a title image: PNG writing, scaling, the background colour, and
the Android adaptive icon of a title's own APK (docs/android/app-shell.md
« A title's own APK »).

Android (minSdk 30) only needs the adaptive icon: two 108 dp layers, of which a
launcher shows the middle 72 dp (66 dp in a circle mask). The title image is the
foreground, centred at --scale of the layer (default 72/108, the visible part,
so a squircle or rounded square keeps the whole picture); the background is the
average colour of the image's border, so the picture seems to extend to the
icon's edges. A title without an image keeps XRecomp's glyph on a background of
the title's colour (TitleArt.monogramColor, the launchers' monogram colour).
"""

import os
import struct
import zlib

from tools.title_art import art

# 108 dp at xxxhdpi (4x): the layer's pixels; a launcher scales it down.
LAYER_PX = 432
DEFAULT_SCALE = 72.0 / 108.0


def write_png(path, img):
    """An RGBA PNG of img, written atomically."""
    raw = bytearray()
    for y in range(img.h):
        raw.append(0)                       # filter: none
        for x in range(img.w):
            raw.extend(img.px[y * img.w + x])

    def chunk(kind, data):
        c = kind + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    data = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", img.w, img.h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))
    _write_atomic(path, data)


def read_png(path):
    """An Image from an 8-bit RGBA or RGB PNG without interlacing (what write_png and most tools write)."""
    with open(path, "rb") as f:
        b = f.read()
    if b[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    o, idat, w = 8, bytearray(), None
    while o < len(b):
        n, kind = struct.unpack_from(">I4s", b, o)
        data = b[o + 8:o + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", data)
            if depth != 8 or ctype not in (2, 6) or inter:
                raise ValueError("%s: only 8-bit RGB/RGBA PNGs without interlacing" % path)
        elif kind == b"IDAT":
            idat += data
        o += 12 + n
    if w is None:
        raise ValueError("%s: no IHDR" % path)
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(bytes(idat))
    stride = w * bpp
    prev = bytearray(stride)
    px = []
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            up = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 0xFF
            elif f == 2:
                line[i] = (line[i] + up) & 0xFF
            elif f == 3:
                line[i] = (line[i] + ((a + up) >> 1)) & 0xFF
            elif f == 4:
                p = a + up - c
                pa, pb, pc = abs(p - a), abs(p - up), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else up if pb <= pc else c)) & 0xFF
        for x in range(w):
            q = line[x * bpp:(x + 1) * bpp]
            px.append(tuple(q) if bpp == 4 else tuple(q) + (255,))
        prev = line
    return art.Image(w, h, px)


def _write_atomic(path, data):
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def scale(img, w, h):
    """img resampled to w x h: bilinear on premultiplied colour (no dark fringes at transparent edges)."""
    src = [(r * a, g * a, b * a, a) for r, g, b, a in img.px]
    sw, sh = img.w, img.h
    out = []
    for y in range(h):
        fy = max(0.0, min(sh - 1.0, (y + 0.5) * sh / h - 0.5))
        y0 = int(fy)
        y1 = min(y0 + 1, sh - 1)
        ty = fy - y0
        for x in range(w):
            fx = max(0.0, min(sw - 1.0, (x + 0.5) * sw / w - 0.5))
            x0 = int(fx)
            x1 = min(x0 + 1, sw - 1)
            tx = fx - x0
            p00, p10 = src[y0 * sw + x0], src[y0 * sw + x1]
            p01, p11 = src[y1 * sw + x0], src[y1 * sw + x1]
            c = [(p00[k] * (1 - tx) + p10[k] * tx) * (1 - ty) + (p01[k] * (1 - tx) + p11[k] * tx) * ty
                 for k in range(4)]
            a = c[3]
            if a < 0.5:
                out.append((0, 0, 0, 0))
            else:
                out.append((int(c[0] / a + 0.5), int(c[1] / a + 0.5), int(c[2] / a + 0.5), int(a + 0.5)))
    return art.Image(w, h, out)


def border_color(img):
    """The alpha-weighted average colour of img's outer ring of pixels, or None when that ring is transparent."""
    ring = set()
    for x in range(img.w):
        ring.add((x, 0))
        ring.add((x, img.h - 1))
    for y in range(img.h):
        ring.add((0, y))
        ring.add((img.w - 1, y))
    tot = [0, 0, 0]
    weight = 0
    for x, y in ring:
        r, g, b, a = img.get(x, y)
        tot[0] += r * a
        tot[1] += g * a
        tot[2] += b * a
        weight += a
    if weight < 255 * len(ring) // 4:       # a mostly transparent border: a logo on nothing
        return None
    return tuple(int(t / weight + 0.5) for t in tot)


def centred(img, side, fraction):
    """A side x side transparent canvas with img scaled (aspect kept) to fraction of it, centred."""
    box = max(1, int(round(side * fraction)))
    k = min(box / float(img.w), box / float(img.h))
    w, h = max(1, int(round(img.w * k))), max(1, int(round(img.h * k)))
    small = scale(img, w, h)
    px = [(0, 0, 0, 0)] * (side * side)
    ox, oy = (side - w) // 2, (side - h) // 2
    for y in range(h):
        row = (oy + y) * side + ox
        px[row:row + w] = small.px[y * w:(y + 1) * w]
    return art.Image(side, side, px)


def hex_color(rgb):
    return "#FF%02X%02X%02X" % tuple(rgb)


ADAPTIVE_IMAGE = """<?xml version="1.0" encoding="utf-8"?>
<!-- Generated by tools/title_art from the title's own default.xbe: never commit it. -->
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@color/xba_title_icon_background" />
    <foreground android:drawable="@drawable/xba_title_icon_foreground" />
</adaptive-icon>
"""

ADAPTIVE_GLYPH = """<?xml version="1.0" encoding="utf-8"?>
<!-- Generated by tools/title_art: the title has no image, XRecomp's glyph on the title's colour. -->
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@color/xba_title_icon_background" />
    <foreground android:drawable="@drawable/ic_launcher_foreground" />
    <monochrome android:drawable="@drawable/ic_launcher_monochrome" />
</adaptive-icon>
"""

COLOR = """<?xml version="1.0" encoding="utf-8"?>
<resources>
    <color name="xba_title_icon_background">%s</color>
</resources>
"""


def android_res(out_dir, img, title_id, fraction=DEFAULT_SCALE):
    """
    Write the res/ tree of a title's adaptive icon to out_dir (gradle -PxbaIconRes=out_dir), which overrides the
    app's mipmap/ic_launcher. img None: the glyph variant. Returns what was written ("image" or "glyph").
    """
    if not 0.2 <= fraction <= 1.0:
        raise ValueError("scale %.3f: expected 0.2 .. 1.0" % fraction)
    bg = (border_color(img) if img is not None else None) or art.monogram_color(title_id)
    _write_atomic(os.path.join(out_dir, "values", "xba_title_icon.xml"), (COLOR % hex_color(bg)).encode())
    fg = os.path.join(out_dir, "drawable-nodpi", "xba_title_icon_foreground.png")
    if img is not None:
        write_png(fg, centred(img, LAYER_PX, fraction))
        xml, kind = ADAPTIVE_IMAGE, "image"
    else:
        if os.path.exists(fg):
            os.remove(fg)
        xml, kind = ADAPTIVE_GLYPH, "glyph"
    _write_atomic(os.path.join(out_dir, "mipmap-anydpi-v26", "ic_launcher.xml"), xml.encode())
    return kind
