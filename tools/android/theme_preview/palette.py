"""Reference implementation of the per-game launcher palette, ported 1:1 to android/.../GamePalette.java (change both
together; golden.py regenerates the Java test's golden vectors). Pure Python; PIL only for image I/O.
Usage: palette.py <image>...  (prints the analysis, the tokens and the contrast checks)"""
import math, sys

# ---- sRGB <-> OKLab / OKLCH (Björn Ottosson, public domain) ----
def _lin(c): c /= 255.0; return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
def _gam(c): return 12.92 * c if c <= 0.0031308 else 1.055 * (c ** (1 / 2.4)) - 0.055
def rgb_to_oklab(rgb):
    r, g, b = (_lin(v) for v in rgb[:3])
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    l, m, s = l ** (1 / 3), m ** (1 / 3), s ** (1 / 3)
    return (0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s)
def oklab_to_lrgb(L, a, b):
    l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3
    m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3
    s = (L - 0.0894841775 * a - 1.2914855480 * b) ** 3
    return (4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s)
def rgb_to_oklch(rgb):
    L, a, b = rgb_to_oklab(rgb); return L, math.hypot(a, b), math.degrees(math.atan2(b, a)) % 360
def in_gamut(L, C, h):
    r, g, b = oklab_to_lrgb(L, C * math.cos(math.radians(h)), C * math.sin(math.radians(h)))
    e = 1e-4; return all(-e <= v <= 1 + e for v in (r, g, b))
def oklch(L, C, h):
    """sRGB (r,g,b) for OKLCH, chroma reduced (bisection) until in gamut."""
    if not in_gamut(L, C, h):
        lo, hi = 0.0, C
        for _ in range(16):
            mid = (lo + hi) / 2
            if in_gamut(L, mid, h): lo = mid
            else: hi = mid
        C = lo
    r, g, b = oklab_to_lrgb(L, C * math.cos(math.radians(h)), C * math.sin(math.radians(h)))
    return tuple(max(0, min(255, round(255 * _gam(max(0.0, min(1.0, v)))))) for v in (r, g, b))

# ---- WCAG 2.x ----
def lum(rgb): r, g, b = (_lin(v) for v in rgb[:3]); return 0.2126 * r + 0.7152 * g + 0.0722 * b
def contrast(a, b): la, lb = lum(a), lum(b); return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)
def over(fg, a, bg): return tuple(round(fg[i] * a + bg[i] * (1 - a)) for i in range(3))
def hexs(rgb): return '#%02X%02X%02X' % tuple(rgb[:3])
def unhex(s): s = s.lstrip('#'); return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))

# ---- The stock theme (Theme.java, GamePalette.STOCK) ----
STOCK = dict(BG_TOP='0C110F', BG_BOTTOM='060807', PANEL='111814', PANEL_HI='18211C', LINE='27332D', TEXT='F3EEE4',
             TEXT_BRIGHT='FFFFFF', TEXT_DIM='A9AFA6', TEXT_FAINT='6C746D', ACCENT='4FE38C', ACCENT_DEEP='1F7A48',
             WARN='F2BA52', ERROR='FF6F61', OK='4FE38C', GLYPH_FACE='EDE6D8', GLYPH_INK='0C110F',
             ACCENT_FACE='143A26', TRACK='1A241F')
STOCK = {k: unhex(v) for k, v in STOCK.items()}
NEUTRALS = ('BG_TOP', 'BG_BOTTOM', 'PANEL', 'PANEL_HI', 'LINE', 'TEXT_DIM', 'TEXT_FAINT', 'TRACK')

BINS = 36
def hue_dist(a, b): d = abs(a - b) % 360; return min(d, 360 - d)

def analyse(pixels):
    """pixels: iterable of (r,g,b,a). Returns dict(chromatic, peaks=[(hue, vivid_share, area_share, meanC)])."""
    vivid = [0.0] * BINS; area = [0.0] * BINS; sx = [0.0] * BINS; sy = [0.0] * BINS; sc = [0.0] * BINS
    n = chrom = 0
    for p in pixels:
        if len(p) > 3 and p[3] < 128: continue
        n += 1
        L, C, h = rgb_to_oklch(p)
        if C < 0.04 or L < 0.18 or L > 0.96: continue        # greys, near-black, near-white carry no hue
        chrom += 1
        w = C * C * (0.4 + 0.6 * min(1.0, L / 0.6))           # vivid weight: chroma squared, dark pixels count less
        i = int(h / (360 / BINS)) % BINS
        vivid[i] += w; area[i] += 1; sc[i] += C
        sx[i] += math.cos(math.radians(h)) * w; sy[i] += math.sin(math.radians(h)) * w
    if n == 0: return dict(chromatic=0.0, peaks=[], n=0)
    sm = lambda v: [(v[i - 1] + 2 * v[i] + v[(i + 1) % BINS]) / 4 for i in range(BINS)]   # circular 1-2-1
    vs, as_ = sm(vivid), sm(area)
    vt, at = sum(vs) or 1, sum(as_) or 1
    peaks = []
    for i in range(BINS):
        if vs[i] > 0 and vs[i] >= vs[i - 1] and vs[i] > vs[(i + 1) % BINS]:
            # the peak owns its bin and both neighbours
            ks = [(i - 1) % BINS, i, (i + 1) % BINS]
            x = sum(sx[k] for k in ks); y = sum(sy[k] for k in ks)
            hue = math.degrees(math.atan2(y, x)) % 360 if (x or y) else (i + 0.5) * 360 / BINS
            cnt = sum(area[k] for k in ks) or 1
            peaks.append((hue, sum(vivid[k] for k in ks) / (sum(vivid) or 1), cnt / n, sum(sc[k] for k in ks) / cnt))
    peaks.sort(key=lambda p: -p[1])
    return dict(chromatic=chrom / n, peaks=peaks, n=n)

ERROR_HUE = rgb_to_oklch(STOCK['ERROR'])[2]
WARN_HUE = rgb_to_oklch(STOCK['WARN'])[2]
ERROR_AVOID_DEG = 18
WARN_AVOID_DEG = 0      # off as designed; 15 pushes a warm accent out of +-15 degrees around WARN's amber

def avoid(h, centre, deg):
    if deg <= 0: return h
    d = (h - centre + 540) % 360 - 180
    return (centre + (deg if d >= 0 else -deg)) % 360 if abs(d) < deg else h

def build(an, fallback_hue=None, min_chromatic=0.06):
    """The theme tokens from an analysis; None when the image has no usable colour (caller falls back)."""
    peaks = [p for p in an['peaks'] if p[2] >= 0.02]          # a hue on under 2 % of the image is noise
    if an['chromatic'] < min_chromatic or not peaks:
        if fallback_hue is None: return None
        ha = hb = fallback_hue; h2 = None
    else:
        ha = peaks[0][0]                                      # accent: the most vivid hue
        hb = max(peaks, key=lambda p: p[2])[0]                # backdrop: the hue covering the most area
        h2 = next((p[0] for p in peaks[1:] if hue_dist(p[0], ha) >= 40 and p[1] >= 0.08), None)
    # a pure red accent would read as an error everywhere: keep 18 degrees away from ERROR's hue
    ha = avoid(ha, WARN_HUE, WARN_AVOID_DEG)
    ha = avoid(ha, ERROR_HUE, ERROR_AVOID_DEG)
    t = dict(STOCK)
    for k in NEUTRALS:                                        # same lightness as the stock token, the game's hue
        L, C, _ = rgb_to_oklch(STOCK[k])
        t[k] = oklch(L, min(C * 1.6, 0.035), hb)
    t['GLYPH_INK'] = t['BG_TOP']
    panel_hi = t['PANEL_HI']
    La, Ca, _ = rgb_to_oklch(STOCK['ACCENT'])
    L = La
    acc = oklch(L, Ca, ha)
    while contrast(acc, panel_hi) < 7.0 and L < 0.93:          # AAA on the brightest surface it is text on
        L += 0.01; acc = oklch(L, Ca, ha)
    t['ACCENT'] = acc
    Ld, Cd, _ = rgb_to_oklch(STOCK['ACCENT_DEEP'])
    t['ACCENT_DEEP'] = oklch(Ld, Cd, ha)
    Lf, Cf, _ = rgb_to_oklch(STOCK['ACCENT_FACE'])
    t['ACCENT_FACE'] = oklch(Lf, Cf, ha)
    t['GLOW'] = oklch(0.80, 0.16, ha)
    t['GLOW2'] = oklch(0.74, 0.15, h2 if h2 is not None else hb)
    t['_hues'] = (ha, hb, h2)
    return t

def checks(t):
    """(name, ratio, minimum) for every pair the UI relies on."""
    bg, hi = t['BG_TOP'], t['PANEL_HI']
    acc_fill = over(t['ACCENT'], 0.16, hi)                    # the primary row's tinted fill
    return [('TEXT on PANEL_HI', contrast(t['TEXT'], hi), 7.0), ('TEXT_DIM on PANEL_HI', contrast(t['TEXT_DIM'], hi), 4.5),
            ('TEXT_FAINT on BG_TOP', contrast(t['TEXT_FAINT'], bg), 3.0), ('ACCENT on PANEL_HI', contrast(t['ACCENT'], hi), 7.0),
            ('ACCENT on BG_TOP', contrast(t['ACCENT'], bg), 7.0), ('GLYPH_INK on ACCENT', contrast(t['GLYPH_INK'], t['ACCENT']), 4.5),
            ('GLYPH_INK on ACCENT_DEEP', contrast(t['GLYPH_INK'], t['ACCENT_DEEP']), 3.0),
            ('TEXT_BRIGHT on accent fill', contrast(t['TEXT_BRIGHT'], acc_fill), 7.0),
            ('ACCENT outline on BG_TOP (non-text)', contrast(t['ACCENT'], bg), 3.0),
            ('LINE on BG_TOP (decorative)', contrast(t['LINE'], bg), 1.0)]

def from_image(path, fallback_hue=None):
    from PIL import Image
    im = Image.open(path).convert('RGBA'); im.thumbnail((64, 64), Image.BOX)
    an = analyse(im.getdata()); return an, build(an, fallback_hue)

if __name__ == '__main__':
    for path in sys.argv[1:]:
        an, t = from_image(path)
        print('==', path, 'chromatic %.2f' % an['chromatic'])
        for p in an['peaks'][:5]: print('   peak hue %5.1f vivid %.2f area %.2f meanC %.3f' % p)
        if not t: print('   no usable colour -> fallback'); continue
        print('   hues accent/backdrop/second:', t['_hues'])
        for k in ('ACCENT', 'ACCENT_DEEP', 'ACCENT_FACE', 'GLOW', 'GLOW2') + NEUTRALS: print('   %-12s %s' % (k, hexs(t[k])))
        for n, r, m in checks(t): print('   %-36s %5.2f (min %.1f) %s' % (n, r, m, 'ok' if r >= m else 'FAIL'))
    print('== stock'); 
    for k in STOCK: print('   %-12s %s  oklch %.3f %.3f %5.1f' % ((k, hexs(STOCK[k])) + rgb_to_oklch(STOCK[k])))
    for n, r, m in checks(dict(STOCK, GLOW=STOCK['ACCENT'], GLOW2=STOCK['ACCENT'])): print('   %-36s %5.2f (min %.1f) %s' % (n, r, m, 'ok' if r >= m else 'FAIL'))
