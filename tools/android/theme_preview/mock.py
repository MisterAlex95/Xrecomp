"""Mock-ups of the themed launcher (PIL), following LauncherActivity.buildHome / Widgets / GameMenu / LauncherPanel geometry
on the 1920x1080 (and 1240x1080) design grids. Not the real views: a preview of palettes and composition (the real
launcher in a game's theme: ThemePreviewTest, docs/android/app-shell.md « Game theme »). macOS fonts (Helvetica Neue).
Usage: mock.py <art dir> <out dir> [last frame png]; <art dir> holds default-XTIMAGE.png and saveimage.png extracted
from your own game with xart.py (never commit them)."""
import math, sys, os
from PIL import Image, ImageDraw, ImageFont, ImageFilter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import palette as P

SS = 2  # supersampling
HN = '/System/Library/Fonts/HelveticaNeue.ttc'
def font(size, face='regular'):
    return ImageFont.truetype(HN, int(size * SS), index={'regular': 0, 'medium': 10, 'light': 7}[face])
def A(rgb, a): return tuple(rgb[:3]) + (max(0, min(255, round(255 * a))),)

class Cv:
    def __init__(self, w, h, bg=(0, 0, 0)):
        self.w, self.h = w, h
        self.im = Image.new('RGBA', (w * SS, h * SS), tuple(bg) + (255,))
    def layer(self): return Image.new('RGBA', self.im.size, (0, 0, 0, 0))
    def put(self, layer): self.im.alpha_composite(layer)
    def rrect(self, box, rad, fill=None, outline=None, width=1):
        l = self.layer(); d = ImageDraw.Draw(l)
        d.rounded_rectangle([v * SS for v in box], rad * SS, fill=fill, outline=outline, width=max(1, round(width * SS)))
        self.put(l)
    def circle(self, cx, cy, r, fill=None, outline=None, width=1):
        l = self.layer(); d = ImageDraw.Draw(l)
        d.ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS], fill=fill, outline=outline, width=max(1, round(width * SS)))
        self.put(l)
    def poly(self, pts, fill): 
        l = self.layer(); ImageDraw.Draw(l).polygon([(x * SS, y * SS) for x, y in pts], fill=fill); self.put(l)
    def line(self, pts, fill, width):
        l = self.layer(); ImageDraw.Draw(l).line([(x * SS, y * SS) for x, y in pts], fill=fill, width=max(1, round(width * SS)), joint='curve'); self.put(l)
    def glow_rrect(self, box, rad, color, blur, alpha, width=2.5):
        l = self.layer(); d = ImageDraw.Draw(l)
        d.rounded_rectangle([v * SS for v in box], rad * SS, outline=A(color, alpha), width=round((width + 4) * SS))
        self.put(l.filter(ImageFilter.GaussianBlur(blur * SS / 2)))
    def text(self, x, y, s, size, face, fill, anchor='ls', spacing=0.0):
        """y is the baseline ('ls') unless another anchor is given; spacing in em (letter spacing)."""
        l = self.layer(); d = ImageDraw.Draw(l); f = font(size, face)
        if spacing:
            cx = x * SS
            if anchor[0] == 'm': cx -= self.tw(s, size, face, spacing) * SS / 2
            if anchor[0] == 'r': cx -= self.tw(s, size, face, spacing) * SS
            for ch in s:
                d.text((cx, y * SS), ch, font=f, fill=fill, anchor='l' + anchor[1]); cx += f.getlength(ch) + spacing * size * SS
        else:
            d.text((x * SS, y * SS), s, font=f, fill=fill, anchor=anchor)
        self.put(l)
    def tw(self, s, size, face, spacing=0.0): return font(size, face).getlength(s) / SS + spacing * size * len(s)
    def hgrad(self, box, c0, c1, rad=0):
        x0, y0, x1, y1 = [round(v * SS) for v in box]; w = x1 - x0
        g = Image.new('RGBA', (w, 1)); px = g.load()
        for i in range(w):
            t = i / max(1, w - 1); px[i, 0] = tuple(round(c0[k] * (1 - t) + c1[k] * t) for k in range(4))
        g = g.resize((w, y1 - y0)); m = Image.new('L', g.size, 0)
        ImageDraw.Draw(m).rounded_rectangle([0, 0, g.size[0] - 1, g.size[1] - 1], rad * SS, fill=255)
        l = self.layer(); l.paste(g, (x0, y0), Image.composite(g.getchannel('A'), Image.new('L', g.size, 0), m)); self.put(l)
    def radial(self, cx, cy, r, color, alpha):
        n = 256; g = Image.new('L', (n, n)); px = g.load()
        for y in range(n):
            for x in range(n):
                d = math.hypot(x - n / 2, y - n / 2) / (n / 2); px[x, y] = round(255 * alpha * max(0, 1 - d))
        g = g.resize((round(2 * r * SS), round(2 * r * SS)), Image.BILINEAR)
        l = self.layer(); l.paste(Image.new('RGBA', g.size, tuple(color[:3]) + (255,)), (round((cx - r) * SS), round((cy - r) * SS)), g); self.put(l)
    def image(self, im, box, rad=0):
        x0, y0, x1, y1 = [round(v * SS) for v in box]
        im = im.convert('RGBA').resize((x1 - x0, y1 - y0), Image.BILINEAR)
        m = Image.new('L', im.size, 0); ImageDraw.Draw(m).rounded_rectangle([0, 0, im.size[0] - 1, im.size[1] - 1], rad * SS, fill=255)
        l = self.layer(); l.paste(im, (x0, y0), m); self.put(l)
    def out(self, path):
        self.im.convert('RGB').resize((self.w, self.h), Image.LANCZOS).save(path); print('wrote', path)

# ---- The hero backdrop bake (GameTheme.bakeHero in the design): small, blurred, luminance-capped ----
LUM_CAP = 0.012      # with the (reduced) glows on top, TEXT_FAINT keeps 3:1 over any pixel of the backdrop
def bake_hero(src, side=48, blur=3.0, cap=LUM_CAP):
    im = src.convert('RGB'); w, h = im.size; s = side / max(w, h)
    im = im.resize((max(1, round(w * s)), max(1, round(h * s))), Image.BOX).filter(ImageFilter.GaussianBlur(blur))
    px = im.load(); lums = [P.lum(px[x, y]) for y in range(im.size[1]) for x in range(im.size[0])]
    hi = sorted(lums)[int(len(lums) * 0.95)] or 1e-6
    gain = min(1.0, cap / hi)                              # the 95th percentile lands on the cap ...
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            lin = [P._lin(v) * gain for v in px[x, y]]; L = 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]
            if L > cap: lin = [v * cap / L for v in lin]   # ... and nothing is above it
            px[x, y] = tuple(round(255 * P._gam(v)) for v in lin)
    return im

def backdrop(cv, t, hero=None, hero_box=None):
    w, h = cv.w, cv.h
    g = Image.new('RGBA', (1, h * SS)); px = g.load()
    for y in range(h * SS):
        k = y / (h * SS - 1); px[0, y] = tuple(round(t['BG_TOP'][i] * (1 - k) + t['BG_BOTTOM'][i] * k) for i in range(3)) + (255,)
    cv.im.paste(g.resize((w * SS, h * SS)))
    if hero is not None:
        hb = hero_box or (0, 0, w, h)
        bw, bh = hb[2] - hb[0], hb[3] - hb[1]
        s = max(bw / hero.size[0], bh / hero.size[1]); hw, hh = hero.size[0] * s, hero.size[1] * s
        big = hero.resize((round(hw * SS), round(hh * SS)), Image.BICUBIC)
        l = cv.layer(); l.paste(big, (round((hb[0] + (bw - hw) / 2) * SS), round((hb[1] + (bh - hh) / 2) * SS)))
        # fade: full on the right, gone at the left edge and at the bottom (the tiles and hints stay on the plain gradient)
        m = Image.new('L', (256, 256)); mp = m.load()
        for y in range(256):
            for x in range(256):
                fx = min(1.0, max(0.0, (x / 255 - 0.05) / 0.45)); fy = min(1.0, max(0.0, (0.98 - y / 255) / 0.35))
                mp[x, y] = round(255 * fx * fy)
        m = m.resize(cv.im.size, Image.BILINEAR)
        l.putalpha(m)
        cv.put(l)
    own = t.get('GLOW') is not None
    cv.radial(w * 0.86, h * 0.08, w * 0.55, t.get('GLOW', t['ACCENT']), (0.07 if hero is not None else 0.16) if own else 0.11)
    cv.radial(w * 0.06, h * 1.02, w * 0.42, t.get('GLOW2', t['ACCENT']), (0.05 if hero is not None else 0.10) if own else 0.05)
    l = cv.layer(); d = ImageDraw.Draw(l)
    for y in range(48, h, 48):
        for x in range(48, w, 48): d.ellipse([(x - 1.5) * SS, (y - 1.5) * SS, (x + 1.5) * SS, (y + 1.5) * SS], fill=A(t['TEXT'], 0.045))
    cv.put(l)
    cv.rrect((18, 18, w - 18, h - 18), 34, outline=A(t['LINE'], 0.9), width=2)

def brand(cv, t, x, y, s):
    cx, cy = x + s / 2, y + s / 2
    cv.rrect((cx - s * .42, cy - s * .42, cx + s * .42, cy + s * .42), s * .16, outline=A(t['ACCENT'], 1), width=3.5)
    d = s * .2; cv.poly([(cx, cy - d), (cx + d, cy), (cx, cy + d), (cx - d, cy)], A(t['ACCENT'], 1))

def chip(cv, t, x, y, label, color):
    w = cv.tw(label, 25, 'medium') + 78
    cv.rrect((x, y, x + w, y + 52), 26, fill=A(color, 0.09), outline=A(color, 0.5), width=1.5)
    cv.circle(x + 28, y + 26, 6, fill=A(color, 1)); cv.text(x + 48, y + 26, label, 25, 'medium', A(t['TEXT'], 1), 'lm'); return w

def icon(cv, kind, cx, cy, s, col, w=3.5):
    if kind == 'play': cv.poly([(cx - s * .32, cy - s * .45), (cx + s * .5, cy), (cx - s * .32, cy + s * .45)], col)
    elif kind == 'sliders':
        for i, k in enumerate((-.4, 0, .4)):
            cv.line([(cx - s * .55, cy + s * k), (cx + s * .55, cy + s * k)], col, w); cv.circle(cx + s * (.25, -.2, .1)[i], cy + s * k, s * .14, fill=col)
    elif kind == 'pad':
        cv.rrect((cx - s * .65, cy - s * .38, cx + s * .65, cy + s * .38), s * .3, outline=col, width=w); cv.circle(cx + s * .3, cy, s * .09, fill=col)
        cv.line([(cx - s * .42, cy), (cx - s * .18, cy)], col, w); cv.line([(cx - s * .3, cy - s * .12), (cx - s * .3, cy + s * .12)], col, w)
    elif kind == 'disc': cv.circle(cx, cy, s * .55, outline=col, width=w); cv.circle(cx, cy, s * .14, outline=col, width=w)
    elif kind == 'info': cv.line([(cx, cy - s * .08), (cx, cy + s * .45)], col, w * 1.3); cv.circle(cx, cy - s * .38, s * .09, fill=col)
    elif kind == 'camera': cv.rrect((cx - s * .6, cy - s * .35, cx + s * .6, cy + s * .42), s * .12, outline=col, width=w); cv.circle(cx, cy + s * .04, s * .2, outline=col, width=w)
    elif kind == 'share': cv.line([(cx + s * .35, cy - s * .4), (cx - s * .35, cy), (cx + s * .35, cy + s * .4)], col, w)
    elif kind == 'grid':
        for dx in (-.3, .3):
            for dy in (-.3, .3): cv.rrect((cx + s * (dx - .2), cy + s * (dy - .2), cx + s * (dx + .2), cy + s * (dy + .2)), 2, outline=col, width=w)
    elif kind == 'power': cv.circle(cx, cy + s * .05, s * .45, outline=col, width=w); cv.line([(cx, cy - s * .6), (cx, cy - s * .05)], col, w * 1.2)

def glide(cv, t, box):
    x0, y0, x1, y1 = box; r = (x0 + 12, y0 + 12, x1 - 12, y1 - 12)
    cv.hgrad(r, A(t['ACCENT'], 0.13), A(t['ACCENT'], 0.02), 18)
    cv.glow_rrect(r, 18, t['ACCENT'], 18, 0.6)
    cv.rrect(r, 18, outline=A(t['ACCENT'], 1), width=2.5)
    bh = max(20, (r[3] - r[1]) - 36 - 8); cy = (r[1] + r[3]) / 2
    cv.rrect((r[0] + 9, cy - bh / 2, r[0] + 15, cy + bh / 2), 3, fill=A(t['ACCENT'], 1))

def blend(a, b, k): return tuple(round(a[i] * (1 - k) + b[i] * k) for i in range(3))

def menurow(cv, t, box, ic, title, sub, focused=False, primary=False, tsize=38, ssize=25, value=None):
    x0, y0, x1, y1 = box; h = y1 - y0; pad = 12; r = (x0 + pad, y0 + pad, x1 - pad, y1 - pad)
    if primary:
        cv.hgrad(r, A(t['ACCENT'], 0.16), A(t['ACCENT'], 0.03), 18)
        if not focused: cv.rrect(r, 18, outline=A(t['ACCENT'], 0.45), width=2)
    if focused: glide(cv, t, box)
    x = x0 + pad + 30; fg = t['TEXT_BRIGHT'] if focused else blend(t['TEXT'], t['TEXT_DIM'], 0.25)
    rad = min(h * 0.29, 46 if primary else 40); cx, cy = x + rad, y0 + h / 2
    if primary:
        cv.circle(cx, cy, rad, fill=A(t['ACCENT'] if focused else t['ACCENT_DEEP'], 1)); icon(cv, ic, cx, cy, rad * .95, A(t['GLYPH_INK'], 1))
    else:
        cv.circle(cx, cy, rad, outline=A(t['ACCENT'] if focused else t['LINE'], 1), width=2.5)
        icon(cv, ic, cx, cy, rad * .8, A(t['ACCENT'] if focused else t['TEXT_DIM'], 1))
    x = cx + rad + 30
    if value:
        cv.text(x1 - pad - 34, cy, value, 31, 'medium', A(t['ACCENT'] if focused else t['TEXT'], 1), 'rm')
    if sub:
        cv.text(x, cy - 8, title, tsize, 'medium', A(fg, 1)); cv.text(x, cy + 8 + ssize, sub, ssize, 'regular', A(t['TEXT_DIM'], 1))
    else: cv.text(x, cy, title, tsize, 'medium', A(fg, 1), 'lm')

def hint(cv, t, xr, cy, glyph, label):
    lw = cv.tw(label, 29, 'regular'); gw = 46 if len(glyph) <= 1 else cv.tw(glyph, 24, 'medium') + 30
    x = xr - (gw + 16 + lw + 8)
    cv.rrect((x, cy - 23, x + gw, cy + 23), 23, fill=A(t['GLYPH_FACE'], 1))
    cv.text(x + gw / 2, cy, glyph, 24, 'medium', A(t['GLYPH_INK'], 1), 'mm'); cv.text(x + gw + 16, cy, label, 29, 'regular', A(t['TEXT_DIM'], 1), 'lm')
    return x - 44

def art_tile(cv, t, x, y, s, art, glow, initials=None):
    r = (x + 26, y + 26, x + s - 26, y + s - 26)
    l = cv.layer(); ImageDraw.Draw(l).rounded_rectangle([v * SS for v in (r[0], r[1] + 8, r[2], r[3] + 8)], 34 * SS, fill=A(glow, 0.55))
    cv.put(l.filter(ImageFilter.GaussianBlur(15 * SS)))
    if art is not None: cv.rrect(r, 34, fill=(0, 0, 0, 255)); cv.image(art, r, 34)
    else:
        L, C, hue = P.rgb_to_oklch(glow)
        cv.hgrad(r, P.oklch(0.52, 0.13, hue) + (255,), P.oklch(0.22, 0.07, hue + 34) + (255,), 34)
        cv.text((r[0] + r[2]) / 2, (r[1] + r[3]) / 2, initials or '?', 150, 'medium', A(t['TEXT_BRIGHT'], 0.94), 'mm')
    cv.rrect(r, 34, outline=A(t['TEXT'], 0.14), width=2)

def home(path, t, art, title, tid, hero=None, facts=None, status=('Ready', 'OK'), initials=None, label=None, lead=None, note=None, primary=('Play', 'Start the game')):
    cv = Cv(1920, 1080); backdrop(cv, t, hero)
    brand(cv, t, 72, 60, 64); cv.text(162, 92, 'Home', 46, 'medium', A(t['TEXT'], 1), 'lm')
    cv.rrect((72, 148, 1848, 150), 0, fill=A(t['LINE'], 1))
    art_tile(cv, t, 72, 264, 400, art, t.get('GLOW', t['ACCENT']), initials)
    x = 524; y = 222
    cv.text(x, y + 20, ('TITLE ID ' + tid).upper(), 22, 'medium', A(t['TEXT_FAINT'], 1), spacing=0.22); y += 22 + 14
    cv.text(x, y + 62, title, 76, 'medium', A(t['TEXT'], 1)); y += 80 + 22
    chip(cv, t, x, y, status[0], t[status[1]]); y += 52 + 22
    cv.text(x, y + 26, lead or 'Ready to play. Your saves are kept on this device.', 29, 'regular', A(t['TEXT_DIM'], 1)); y += 35 + 26
    fx = x
    for k, v in facts or (('LAST PLAYED', 'Yesterday'), ('PLAY TIME', '12 h 40 min'), ('PREPARED', '10 Nov 2026'), ('PACK SIZE', '412 MB')):
        cv.text(fx, y + 20, k, 22, 'medium', A(t['TEXT_FAINT'], 1), spacing=0.22); cv.text(fx, y + 22 + 10 + 28, v, 32, 'medium', A(t['TEXT'], 1))
        fx += max(cv.tw(k, 22, 'medium', 0.22), cv.tw(v, 32, 'medium')) + 64
    y += 66 + 30
    menurow(cv, t, (x - 12, y, x - 12 + 660, y + 128), 'play', primary[0], primary[1], focused=True, primary=True, tsize=46, ssize=27)
    tw = (1848 + 12 - (72 - 12)) / 4
    for i, (ic, a, b) in enumerate((('sliders', 'Settings', 'GPU · 2× · 16:9'), ('pad', 'Controls', 'What each button does'),
                                    ('disc', 'Game data', '4.1 GB · Imported copy'), ('info', 'About', 'Version 0.9'))):
        menurow(cv, t, (60 + i * tw + 6, 766, 60 + (i + 1) * tw - 6, 894), ic, a, b, tsize=34)
    if note: cv.circle(87, 926, 7, fill=A(t[note[1]], 1)); cv.text(114, 926, note[0], 29, 'regular', A(t[note[1]], 1), 'lm')
    xr = 1848
    for g, lab in (('START', 'Play'), ('B', 'Quit'), ('A', 'Select')): xr = hint(cv, t, xr, 1004, g, lab)
    if label: cv.text(72, 1066, label, 18, 'regular', A(t['TEXT_FAINT'], 1), 'lm')
    cv.out(path)

def game_menu(path, t, frame, title):
    cv = Cv(1920, 1080, (0, 0, 0))
    cv.image(frame, (240, 0, 1680, 1080))                               # the 4:3 game frame
    cv.hgrad((0, 0, 1920, 1080), (0, 0, 0, 0xD9), (0, 0, 0, 0x73))      # the scrim
    cv.rrect((-40, 0, 680, 1080), 22, fill=A(t['BG_BOTTOM'], 0.7)); cv.rrect((-40, 1, 680, 1079), 22, fill=A(t['PANEL'], 0.9), outline=A(t['LINE'], 1), width=1.5)
    cv.line([(642, 1068), (668, 1068)], A(t['ACCENT'], 0.7), 3); cv.line([(668, 1068), (668, 1042)], A(t['ACCENT'], 0.7), 3)
    brand(cv, t, 40, 46, 44); cv.text(102, 68, 'GAME MENU', 22, 'medium', A(t['ACCENT'], 1), 'lm', spacing=0.22)
    w = cv.tw('Paused', 25, 'medium') + 78; chip(cv, t, 652 - w, 42, 'Paused', t['WARN'])
    cv.text(40, 152, title, 46, 'medium', A(t['TEXT'], 1)); cv.text(40, 194, 'Playing for 0:42:10', 25, 'regular', A(t['TEXT_DIM'], 1))
    cv.rrect((40, 222, 652, 224), 0, fill=A(t['LINE'], 1))
    y = 250
    for i, (ic, a) in enumerate((('play', 'Resume'), ('sliders', 'Quick settings'), ('pad', 'Controls'), ('camera', 'Screenshot'),
                                 ('share', 'Share log'), ('grid', 'Back to launcher'), ('power', 'Quit game'))):
        menurow(cv, t, (28, y, 664, y + 100), ic, a, None, focused=(i == 0), tsize=36); y += 100
    xr = 652
    for g, lab in (('B', 'Back'), ('A', 'Select')): xr = hint(cv, t, xr, 1020, g, lab)
    cv.text(1880, 40, '59.9 fps · 16.7 ms', 26, 'medium', A(t['TEXT'], 0.9), 'rm')
    cv.out(path)

def companion(path, t, art, title, status=('Ready', 'OK')):
    cv = Cv(1240, 1080, (0, 0, 0))                                      # pure black: AMOLED
    cv.circle(51, 92, 7, fill=A(t['TEXT_DIM'], 0)); cv.text(44, 92, 'Battery 82 %', 30, 'medium', A(t['TEXT_DIM'], 1), 'lm')
    cv.circle(1080, 92, 7, fill=A(t['OK'], 1)); cv.text(1100, 92, 'Cool', 30, 'medium', A(t['OK'], 1), 'lm')
    cv.rrect((36, 140, 1204, 470), 28, fill=A(t['PANEL'], 1), outline=A(t['LINE'], 1), width=2)
    tx = 84
    if art is not None:
        cv.image(art, (72, 176, 330, 434), 24); cv.rrect((72, 176, 330, 434), 24, outline=A(t['TEXT'], 0.14), width=2); tx = 372
    cv.text(tx, 212, 'XRECOMP', 22, 'medium', A(t['ACCENT'], 1), spacing=0.22)
    f = 76
    while cv.tw(title, f, 'light') > 1204 - 48 - tx: f -= 2
    cv.text(tx, 318, title, f, 'light', A(t['TEXT'], 1))
    cv.circle(tx + 8, 400, 8, fill=A(t[status[1]], 1)); cv.text(tx + 30, 400, status[0], 30, 'medium', A(t['TEXT_DIM'], 1), 'lm')
    for (x0, x1, lab, ic, acc) in ((36, 520, 'Play', 'play', True), (548, 872, 'Settings', 'sliders', False), (900, 1204, 'Change game', 'disc', False)):
        cv.rrect((x0, 510, x1, 860), 30, fill=A(t['ACCENT_FACE'] if acc else t['PANEL_HI'], 1), outline=A(t['ACCENT_DEEP'] if acc else t['LINE'], 1), width=3)
        ink = A(t['ACCENT'] if acc else t['TEXT'], 1); cx = (x0 + x1) / 2
        icon(cv, ic, cx, 645, 84, ink, 6); cv.text(cx, 747, lab, 32 if len(lab) > 9 else 38, 'medium', ink, 'ms')
    cv.out(path)

def palette_sheet(path, entries):
    """entries: [(name, image or None, tokens)]"""
    rows = len(entries); cv = Cv(1920, 60 + rows * 330, (16, 16, 16))
    keys = ('ACCENT', 'ACCENT_DEEP', 'ACCENT_FACE', 'GLOW', 'GLOW2', 'BG_TOP', 'PANEL', 'PANEL_HI', 'LINE', 'TEXT_FAINT', 'TEXT_DIM', 'TEXT')
    for i, (name, im, t) in enumerate(entries):
        y = 40 + i * 330
        cv.text(40, y + 24, name, 28, 'medium', (240, 240, 240, 255))
        if im is not None: cv.image(im.resize((256, 256), Image.NEAREST if max(im.size) <= 128 else Image.BILINEAR), (40, y + 44, 296, y + 300), 8)
        for j, k in enumerate(keys):
            x = 330 + j * 130; c = t.get(k, t['ACCENT'])
            cv.rrect((x, y + 44, x + 118, y + 150), 10, fill=A(c, 1), outline=(70, 70, 70, 255))
            cv.text(x, y + 176, k, 16, 'medium', (220, 220, 220, 255)); cv.text(x, y + 198, P.hexs(c), 18, 'regular', (170, 170, 170, 255))
        cx = 330
        for n, r, m in P.checks(t)[:8]:
            s = '%s %.1f:1' % (n, r); cv.text(cx, y + 240 + (0 if cx < 1700 else 0), s, 17, 'regular', (150, 220, 150, 255) if r >= m else (255, 120, 110, 255))
            cx += cv.tw(s, 17, 'regular') + 28
            if cx > 1500: cx = 330; y += 26
    cv.out(path)

def synthetic_racer(n=128):
    """A made-up 128x128 'arcade racer' title image (no game art): an orange-to-yellow flame streak on near-black."""
    im = Image.new('RGB', (n, n), (8, 8, 10)); px = im.load()
    for y in range(n):
        for x in range(n):
            d = abs((y - n * 0.62) + (x - n / 2) * 0.35) / (n * 0.30); k = max(0.0, 1 - d)
            fl = k ** 1.5 * (0.55 + 0.45 * math.sin(x * 0.21 + y * 0.13) ** 2)
            r, g, b = 8 + 247 * min(1, fl * 1.5), 8 + 190 * min(1, fl ** 1.6 * 1.3), 10 + 40 * fl ** 4
            if y < n * 0.2 and (x // 8) % 2 == 0: r, g, b = 200, 205, 215   # a strip of chrome "lettering"
            px[x, y] = (int(r), int(g), int(b))
    return im

if __name__ == '__main__':
    art_dir, out = sys.argv[1], sys.argv[2]; os.makedirs(out, exist_ok=True)
    stock = dict(P.STOCK)
    ti = Image.open(os.path.join(art_dir, 'default-XTIMAGE.png')).convert('RGB')
    frame = Image.open(sys.argv[3]).convert('RGB') if len(sys.argv) > 3 else None
    an, tf = P.from_image(os.path.join(art_dir, 'default-XTIMAGE.png'))
    T, TID = 'Sample Game', '58420001'   # your title's name and ID go here
    home(os.path.join(out, 'game-home-0-stock.png'), stock, ti, T, TID, label='MOCK · stock theme (today)')
    home(os.path.join(out, 'game-home-1-themed.png'), tf, ti, T, TID, hero=bake_hero(ti), label='MOCK · game theme from $$XTIMAGE: palette + blurred hero backdrop')
    home(os.path.join(out, 'game-home-2-themed-flat.png'), tf, ti, T, TID, label='MOCK · game theme, palette only (hero backdrop off)')
    if frame is not None:
        home(os.path.join(out, 'game-home-3-themed-lastframe.png'), tf, ti, T, TID, hero=bake_hero(frame, side=160, blur=1.6), label='MOCK · game theme + "where you left off" frame as hero backdrop (phase 2)')
        game_menu(os.path.join(out, 'game-gamemenu-themed.png'), tf, frame, T)
        game_menu(os.path.join(out, 'game-gamemenu-stock.png'), stock, frame, T)
    companion(os.path.join(out, 'game-companion-themed.png'), tf, ti, T)
    companion(os.path.join(out, 'game-companion-stock.png'), stock, None, T)
    home(os.path.join(out, 'game-home-4-needs-preparing.png'), tf, ti, T, TID, hero=bake_hero(ti), status=('Needs preparing', 'WARN'),
         lead='This game has to be prepared once before it can be played.', facts=(('TAKES ABOUT', '25 min'), ('INTERNAL FREE', '38 GB'), ('MEMORY', '12 GB')), primary=('Prepare', 'Once, about 25 min, on this device'),
         note=(T + ' is ready', 'OK'), label='MOCK · semantic colours stay: WARN chip amber, OK note green, accent periwinkle')
    syn = synthetic_racer(); syn.save(os.path.join(out, 'synthetic-racer-title.png'))
    an2 = P.analyse(syn.resize((64, 64), Image.BOX).convert('RGBA').getdata()); tb = P.build(an2)
    home(os.path.join(out, 'racer-home-expected-SYNTHETIC.png'), tb, syn, 'Sample Racer', '5842XXXX', hero=bake_hero(syn),
         label='MOCK · SYNTHETIC placeholder art (not the game\'s image): what a warm, flame-coloured title image would give')
    home(os.path.join(out, 'racer-home-expected-SYNTHETIC-warn.png'), tb, syn, 'Sample Racer', '5842XXXX', hero=bake_hero(syn), status=('Needs preparing', 'WARN'),
         lead='This game has to be prepared once before it can be played.', facts=(('TAKES ABOUT', '25 min'), ('INTERNAL FREE', '38 GB'), ('MEMORY', '12 GB')), primary=('Prepare', 'Once, about 25 min, on this device'),
         label='MOCK · SYNTHETIC placeholder art: an orange accent next to the amber WARN chip (the clash to judge)')
    grey = Image.new('RGB', (128, 128), (90, 90, 92))
    hh = 212.0
    tg = P.build(P.analyse(grey.convert('RGBA').getdata()), fallback_hue=hh); tg['GLOW'] = P.oklch(0.80, 0.16, hh)
    home(os.path.join(out, 'fallback-home-monogram.png'), tg, None, 'Sample Game', '58420002', initials='SG', label='MOCK · no usable title image: monogram, hue from the title ID')
    palette_sheet(os.path.join(out, 'palettes.png'), [('Stock (Theme.java today)', None, stock), (T + ' · $$XTIMAGE 128x128 DXT1', ti, tf),
        (T + ' · Data/Misc/saveimage.xbx 64x64 (alternative source)', Image.open(os.path.join(art_dir, 'saveimage.png')).convert('RGB'), P.from_image(os.path.join(art_dir, 'saveimage.png'))[1]),
        ('SYNTHETIC warm placeholder (stand-in for a flame-coloured title image)', syn, tb), ('Grey image -> fallback (hue from title ID)', grey, tg)])
    for n, t in (('game', tf), ('synthetic', tb)):
        print(n, {k: P.hexs(v) for k, v in t.items() if not k.startswith('_')}, t['_hues'])
        for c in P.checks(t): print('   %-36s %5.2f (min %.1f) %s' % (c[0], c[1], c[2], 'ok' if c[1] >= c[2] else 'FAIL'))
