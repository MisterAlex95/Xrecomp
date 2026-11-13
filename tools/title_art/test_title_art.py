"""tools.title_art on synthetic XBEs (tools/conformance/mkxbe.py plus a $$XTIMAGE section we build): no game data."""

import json
import os
import struct
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.conformance import mkxbe                 # noqa: E402
from tools.title_art import art, icons              # noqa: E402
from tools.title_art.__main__ import main           # noqa: E402

RED, BLUE = (0xF8 >> 3) << 11, 0x1F                 # RGB565


def xpr(fmt, log_w, log_h, data):
    """An XPR0 file holding one texture: header (12), one resource (20), the end marker, data at 2048."""
    hdr_size = 2048
    b = bytearray(hdr_size)
    b[0:4] = b"XPR0"
    struct.pack_into("<II", b, 4, hdr_size + len(data), hdr_size)
    struct.pack_into("<IIIII", b, 12, 0x00040001, 0, 0, fmt << 8 | log_w << 20 | log_h << 24, 0)
    struct.pack_into("<I", b, 32, 0xFFFFFFFF)
    return bytes(b) + data


def dxt1_solid(c0, c1, idx=0, blocks=1):
    return struct.pack("<HHI", c0, c1, idx) * blocks


def xbe_with(section=b"", name="$$XTIMAGE"):
    """mkxbe's synthetic XBE (title 12345678 « SYNTH ») with a second section called name holding section."""
    data = bytearray(mkxbe.build())
    if not section:
        return bytes(data)
    base = mkxbe.BASE
    secs = struct.unpack_from("<I", data, 0x120)[0] - base
    struct.pack_into("<I", data, 0x11C, 2)
    s = secs + 56
    name_va = base + 0x640
    raw = len(data)
    struct.pack_into("<IIIIII", data, s, 0, 0x00100000, len(section), raw, len(section), name_va)
    n = name_va - base
    data[n:n + len(name) + 1] = name.encode() + b"\0"
    return bytes(data) + section


def test_dxt1_title_image():
    # 8 x 8: four blocks, colour 0 red everywhere.
    img = art.title_image(xbe_with(xpr(0x0C, 3, 3, dxt1_solid(RED, BLUE, 0, 4))))
    assert (img.w, img.h) == (8, 8)
    assert set(img.px) == {(255, 0, 0, 255)}


def test_dxt1_transparent_index():
    # c0 <= c1: index 3 is transparent black.
    img = art.decode_xpr(xpr(0x0C, 2, 2, dxt1_solid(BLUE, RED, 0xFFFFFFFF)))
    assert set(img.px) == {(0, 0, 0, 0)}


def test_swizzled_argb_matches_morton_order():
    w = h = 4
    xm, ym = art.swizzle_masks(w, h)
    texels = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            i = art.deposit(x, xm) | art.deposit(y, ym)
            texels[4 * i:4 * i + 4] = bytes((x * 10, y * 10, 7, 200))      # B G R A
    img = art.decode_xpr(xpr(0x06, 2, 2, bytes(texels)))
    assert img.get(3, 1) == (7, 10, 30, 200)
    assert img.get(0, 2) == (7, 20, 0, 200)


def test_save_image_is_the_fallback():
    img = art.title_image(xbe_with(xpr(0x0C, 2, 2, dxt1_solid(RED, BLUE)), name="$$XSIMAGE"))
    assert img is not None and img.get(0, 0) == (255, 0, 0, 255)


def test_no_image_and_garbage():
    assert art.title_image(xbe_with()) is None
    assert art.title_image(b"not an xbe" * 100) is None
    assert art.decode_xpr(b"XPR0" + b"\xff" * 40) is None
    assert art.title_image(xbe_with(xpr(0x0C, 3, 3, b"\0" * 8))) is None      # data cut short


def test_title_hue_matches_the_launchers():
    # Values from the JVM: "12345678".hashCode() = -1861353340, TitleArt.titleHue("12345678") = 284.
    assert art._java_hash("12345678") == -1861353340 & 0xFFFFFFFF
    assert art.title_hue("12345678") == 284
    assert art.title_hue("4D530004") == 145
    assert art.title_hue("abcdef01") == art.title_hue("ABCDEF01")


def test_png_round_trip(tmp_path):
    img = art.Image(3, 2, [(1, 2, 3, 255), (4, 5, 6, 128), (0, 0, 0, 0), (9, 9, 9, 9), (255, 0, 0, 255), (7, 7, 7, 7)])
    p = str(tmp_path / "a.png")
    icons.write_png(p, img)
    back = icons.read_png(p)
    assert (back.w, back.h, back.px) == (img.w, img.h, img.px)


def test_scale_and_border():
    img = art.Image(2, 2, [(200, 0, 0, 255)] * 4)
    big = icons.scale(img, 5, 7)
    assert (big.w, big.h) == (5, 7) and set(big.px) == {(200, 0, 0, 255)}
    assert icons.border_color(img) == (200, 0, 0)
    assert icons.border_color(art.Image(2, 2, [(0, 0, 0, 0)] * 4)) is None


def test_centred_keeps_aspect():
    img = art.Image(4, 2, [(0, 255, 0, 255)] * 8)
    c = icons.centred(img, 40, 0.5)
    opaque = [(i % 40, i // 40) for i, p in enumerate(c.px) if p[3]]
    xs, ys = [p[0] for p in opaque], [p[1] for p in opaque]
    assert (max(xs) - min(xs) + 1, max(ys) - min(ys) + 1) == (20, 10)
    assert (min(xs), min(ys)) == (10, 15)


def test_android_res_with_image(tmp_path):
    img = art.Image(8, 8, [(10, 20, 30, 255)] * 64)
    out = str(tmp_path / "res")
    assert icons.android_res(out, img, "12345678") == "image"
    fg = icons.read_png(os.path.join(out, "drawable-nodpi", "xba_title_icon_foreground.png"))
    assert (fg.w, fg.h) == (icons.LAYER_PX, icons.LAYER_PX)
    assert fg.get(0, 0)[3] == 0 and fg.get(216, 216) == (10, 20, 30, 255)
    xml = open(os.path.join(out, "mipmap-anydpi-v26", "ic_launcher.xml")).read()
    assert "@drawable/xba_title_icon_foreground" in xml
    assert "#FF0A141E" in open(os.path.join(out, "values", "xba_title_icon.xml")).read()


def test_android_res_without_image(tmp_path):
    out = str(tmp_path / "res")
    assert icons.android_res(out, None, "12345678") == "glyph"
    xml = open(os.path.join(out, "mipmap-anydpi-v26", "ic_launcher.xml")).read()
    assert "@drawable/ic_launcher_foreground" in xml
    assert not os.path.exists(os.path.join(out, "drawable-nodpi", "xba_title_icon_foreground.png"))
    col = icons.hex_color(art.monogram_color("12345678"))
    assert col in open(os.path.join(out, "values", "xba_title_icon.xml")).read()
    with pytest.raises(ValueError):
        icons.android_res(out, None, "12345678", 3.0)


def test_cli(tmp_path, capsys):
    xbe = tmp_path / "default.xbe"
    xbe.write_bytes(xbe_with(xpr(0x0C, 3, 3, dxt1_solid(RED, BLUE, 0, 4))))
    png, res = str(tmp_path / "t.png"), str(tmp_path / "res")
    assert main([str(tmp_path), "--png", png, "--android-res", res, "--json"]) == 0
    out = json.loads(capsys.readouterr().out)
    assert out["title_id"] == "12345678" and out["name"] == "SYNTH" and out["image"] == [8, 8]
    assert out["icon"] == "image" and icons.read_png(png).w == 8
    bare = tmp_path / "bare.xbe"
    bare.write_bytes(xbe_with())
    assert main([str(bare), "--png", png]) == 2
    assert main([str(tmp_path / "missing")]) == 1


def test_module_runs():
    r = subprocess.run([sys.executable, "-m", "tools.title_art", "--help"], cwd=ROOT, capture_output=True, text=True)
    assert r.returncode == 0 and "android-res" in r.stdout
