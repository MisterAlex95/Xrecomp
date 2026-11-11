"""Golden vectors for GamePaletteTest (android/app/src/test/resources/theme/golden.txt), from palette.py.
Usage: python3 golden.py > ../../../android/app/src/test/resources/theme/golden.txt

Each line: <case> | hues <accent>/<backdrop>/<second or -> | NAME=RRGGBB ... (or « none » when no theme).
Cases: « fallback <hue> » (an image without colour, the title-ID hue), « solid <r>,<g>,<b> » (a one-colour image),
« image <name> » (a synthetic picture made by the same formula in GamePaletteTest; no game art)."""
import math
import palette as P

KEYS = ('BG_TOP', 'BG_BOTTOM', 'PANEL', 'PANEL_HI', 'LINE', 'TEXT_DIM', 'TEXT_FAINT', 'TRACK', 'ACCENT', 'ACCENT_DEEP',
        'ACCENT_FACE', 'GLOW', 'GLOW2', 'GLYPH_INK', 'TEXT', 'OK', 'WARN', 'ERROR')

def line(case, t):
    if t is None: return '%s | none' % case
    ha, hb, h2 = t['_hues']
    hues = '%.2f/%.2f/%s' % (ha, hb, '-' if h2 is None else '%.2f' % h2)
    return '%s | hues %s | %s' % (case, hues, ' '.join('%s=%s' % (k, P.hexs(t[k])[1:]) for k in KEYS))

def split():
    """64 x 64: 70 % blue (40,80,220), 30 % magenta (220,60,150) on the right, a dark grey top band."""
    px = []
    for y in range(64):
        for x in range(64):
            px.append((30, 30, 32, 255) if y < 8 else (40, 80, 220, 255) if x < 45 else (220, 60, 150, 255))
    return px

def flame():
    """64 x 64: an orange-to-yellow streak on near-black (integer maths only, so Java makes the same pixels)."""
    px = []
    for y in range(64):
        for x in range(64):
            d = abs(2 * y - 80 + (x - 32)) * 100 // 40         # 0 on the streak, 100 at 20 px
            k = max(0, 100 - d)                                # 0..100
            px.append((8 + 247 * k // 100, 8 + 150 * k * k // 10000, 10 + 30 * k * k * k // 1000000, 255))
    return px

def stripes():
    """64 x 64: vertical stripes, teal (20,170,160) wide, gold (230,180,40) narrow, every 8th column transparent."""
    px = []
    for y in range(64):
        for x in range(64):
            a = 0 if x % 8 == 7 else 255
            px.append((230, 180, 40, a) if x % 16 < 4 else (20, 170, 160, a))
    return px

if __name__ == '__main__':
    for h in range(0, 360, 15):
        print(line('fallback %d' % h, P.build(dict(chromatic=0, peaks=[], n=1), fallback_hue=h)))
    for rgb in ((40, 80, 220), (240, 130, 20), (230, 30, 30), (128, 128, 128), (255, 255, 255), (8, 10, 40),
                (20, 200, 120), (250, 200, 30), (150, 60, 220)):
        print(line('solid %d,%d,%d' % rgb, P.build(P.analyse([rgb + (255,)] * 64))))
    for name, f in (('split', split), ('flame', flame), ('stripes', stripes)):
        print(line('image %s' % name, P.build(P.analyse(f()))))
