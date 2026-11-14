"""Title and save images of your own game as PNGs, for the Mac previews (mock.py, palette.py).
Usage: xart.py <default.xbe or a .xbx> <outdir>  ->  <stem>-XTIMAGE.png, <stem>-XSIMAGE.png (or <stem>.png for a .xbx).
The decoding is tools/title_art's (the launchers' TitleArt formats). Keep the PNGs it writes out of the repo."""
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..")))
from tools.title_art import art, icons      # noqa: E402

if __name__ == '__main__':
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    b = open(src, 'rb').read()
    stem = os.path.splitext(os.path.basename(src))[0]
    if b[:4] == b'XBEH':
        for name in (art.SECTION, art.SAVE_SECTION):
            sec = art.xbe_section(b, name)
            img = art.decode_xpr(sec) if sec else None
            print(name, '%dx%d' % (img.w, img.h) if img else 'none or not decoded')
            if img:
                icons.write_png(os.path.join(out, stem + '-' + name.strip('$') + '.png'), img)
    else:
        img = art.decode_xpr(b)
        print('%dx%d' % (img.w, img.h) if img else 'not an XPR0 texture this decoder reads')
        if img:
            icons.write_png(os.path.join(out, stem + '.png'), img)
