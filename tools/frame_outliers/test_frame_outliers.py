"""tools.frame_outliers on synthetic data (no game data): BMP sequences with a
flash, a two-frame glitch, a black spike, a camera pan and a cut; batch
records with a texture that flickers; [GPUSIG] tables."""

import os
import struct
import sys

import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.frame_outliers import images, records             # noqa: E402
from tools.frame_outliers.__main__ import main               # noqa: E402

W, H = 32, 24


def write_bmp(path, pixels, w=W, h=H, bpp=24, top_down=False):
    """pixels(x, y) -> (r, g, b)."""
    step = bpp // 8
    stride = (w * step + 3) & ~3
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b = pixels(x, y)
            row += bytes((b, g, r)) + (b"\xff" if step == 4 else b"")
        rows.append(bytes(row) + b"\0" * (stride - len(row)))
    if not top_down:
        rows.reverse()
    body = b"".join(rows)
    hdr = b"BM" + struct.pack("<IHHI", 54 + len(body), 0, 0, 54)
    hdr += struct.pack("<IiiHHIIiiII", 40, w, -h if top_down else h, 1, bpp, 0, len(body), 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr + body)


def scene(t, glitch=None):
    """A gradient that pans one pixel a frame, a grey box; glitch 'black'
    paints the box black, 'white' paints a band white."""
    def px(x, y):
        if 8 <= x < 20 and 6 <= y < 18:
            if glitch == "black":
                return (0, 0, 0)
            return (128, 128, 128)
        if glitch == "white" and 2 <= y < 5:
            return (255, 255, 255)
        v = ((x + t) * 3) % 256
        return (v, 255 - v, 60)
    return px


def make_seq(tmp_path, n, glitches=None, cut_at=None, start=0):
    glitches = glitches or {}
    for i in range(n):
        t = i if cut_at is None or i < cut_at else i + 17
        write_bmp(str(tmp_path / ("f%05d.bmp" % (start + i))), scene(t, glitches.get(i)))
    return images.list_frames(str(tmp_path))


@pytest.mark.parametrize("bpp,top_down", [(24, False), (32, False), (24, True)])
def test_read_bmp_layouts(tmp_path, bpp, top_down):
    p = str(tmp_path / "a.bmp")
    write_bmp(p, lambda x, y: (x * 7 % 256, y * 9 % 256, 3), w=5, h=3, bpp=bpp, top_down=top_down)
    for use_np in (False, True):
        fr = images.read_bmp(p, use_numpy=use_np)
        assert (fr.width, fr.height) == (5, 3)
        assert fr.rgb[0:3] == bytes((0, 0, 3))                     # top-left
        o = (2 * 5 + 4) * 3
        assert fr.rgb[o:o + 3] == bytes((28, 18, 3))               # x 4, y 2


def test_clean_pan_and_cut_have_no_outlier(tmp_path):
    fr = make_seq(tmp_path, 24, cut_at=12)
    res = images.analyse_frames(fr, floor=0.01)
    assert res["frames"] == 24
    assert res["count"] == 0, images.format_report(res)


def test_one_frame_flash_and_black_spike(tmp_path):
    fr = make_seq(tmp_path, 30, glitches={10: "black", 20: "white"})
    res = images.analyse_frames(fr, floor=0.01, black_excess=0.05)
    assert res["outliers"] == [10, 20], images.format_report(res)
    assert [b[0] for b in res["black"]] == [10]
    assert sorted(n for r in res["flash"] for n in r[0]) == [10, 20]


def test_two_frame_glitch(tmp_path):
    fr = make_seq(tmp_path, 20, glitches={8: "white", 9: "white"})
    res = images.analyse_frames(fr, floor=0.01, detectors=("flash",))
    assert res["outliers"] == [8, 9]
    assert res["flash"][0][0] == [8, 9]
    res1 = images.analyse_frames(fr, floor=0.01, detectors=("flash",), max_run=1)
    assert res1["outliers"] == []


def test_numbering_gap_is_a_boundary(tmp_path):
    # frames 0-9, then 40-49: frame 9 vs 40 is not a neighbour pair
    make_seq(tmp_path, 10)
    for i in range(10):
        write_bmp(str(tmp_path / ("f%05d.bmp" % (40 + i))), scene(100 + i, "white" if i == 0 else None))
    res = images.analyse_frames(images.list_frames(str(tmp_path)), floor=0.01)
    assert res["count"] == 0


def test_crop_ignores_outside(tmp_path):
    fr = make_seq(tmp_path, 16, glitches={7: "white"})
    assert images.analyse_frames(fr, floor=0.01, crop=(0, 6, W, H))["count"] == 0
    assert images.analyse_frames(fr, floor=0.01)["outliers"] == [7]


def test_pure_python_matches_numpy(tmp_path):
    pytest.importorskip("numpy")
    fr = make_seq(tmp_path, 14, glitches={5: "black"})
    a = images.analyse_frames(fr, floor=0.01, black_excess=0.05, use_numpy=True)
    b = images.analyse_frames(fr, floor=0.01, black_excess=0.05, use_numpy=False)
    assert a["flash"] == b["flash"] and a["black"] == b["black"]


def test_cli_frames_exit_code(tmp_path, capsys):
    make_seq(tmp_path, 20, glitches={6: "black"})
    assert main(["frames", str(tmp_path), "--floor", "0.01", "--black-excess", "0.05"]) == 1
    out = capsys.readouterr().out
    assert "outlier frames: 6" in out and "verdict: FAIL" in out
    assert main(["frames", str(tmp_path), "--floor", "0.01", "--black-excess", "0.05",
                 "--max-outliers", "1", "--json", str(tmp_path / "r.json")]) == 0
    assert (tmp_path / "r.json").exists()


def test_scale_dumps_are_ignored(tmp_path):
    make_seq(tmp_path, 3)
    write_bmp(str(tmp_path / "fS00001.bmp"), scene(0))
    assert [n for n, _ in images.list_frames(str(tmp_path), "f")] == [0, 1, 2]


# ---- batch records ---------------------------------------------------------

SHADOW_TEX, OWN_TEX = 0x80CE6080, 0x801C2380
FMT_32 = 0x0C | (31 << 8) | (31 << 20)
FMT_128 = 0x0C | (127 << 8) | (255 << 20)


def frame_records(f, wrong=False, drop_last=False, swap=False):
    """A frame: a shadow pass (3 batches, placeholder texture) and a main pass
    drawing the same vertex buffers with their own texture; `wrong`: the
    second main-pass batch binds the shadow pass's texture. `swap`: double
    buffering, odd frames draw into the other buffer, and a last batch
    samples the buffer of the frame before."""
    out = []
    n = 0
    back, front = (0x809E8000, 0x80A7E000) if not (swap and f & 1) else (0x80A7E000, 0x809E8000)
    for vb in (0x82B4DA60, 0x82B63000, 0x82B70B00):
        out.append(records.pack_record(frame=f, index=n, sig=0x1111, look=0x1111, zeta=0x83078000,
                                       vb=vb, count=1296, prog=7, idx=vb ^ 5, tex0=SHADOW_TEX,
                                       fmt0=FMT_32, vtx=f))       # vertices change every frame
        n += 1
    for i, vb in enumerate((0x82B4DA60, 0x82B63000, 0x82B70B00)):
        if drop_last and i == 2:
            break
        bad = wrong and i == 1
        out.append(records.pack_record(frame=f, index=n, sig=(0x1111 if bad else 0x2222) ^ back,
                                       look=0x1111 if bad else 0x2222,
                                       target=back, zeta=0x80800000, vb=vb, count=1296,
                                       prog=9, idx=vb ^ 5, tex0=SHADOW_TEX if bad else OWN_TEX,
                                       fmt0=FMT_32 if bad else FMT_128, const=f))
        n += 1
    out.append(records.pack_record(frame=f, index=n, sig=0x3333 ^ back, look=0x3333, target=back,
                                   vb=0x82000000, count=6, prog=11, tex0=front, fmt0=0x12))
    return b"".join(out)


def write_log(tmp_path, frames):
    p = tmp_path / "b.blg"
    p.write_bytes(b"".join(frames))
    return str(p)


def test_records_roundtrip(tmp_path):
    p = write_log(tmp_path, [frame_records(5)])
    recs = list(records.read_records(p))
    assert len(recs) == 7 and recs[0]["frame"] == 5 and recs[3]["tex0"] == OWN_TEX
    # a truncated tail record is dropped, a bad magic is an error
    with open(p, "ab") as f:
        f.write(records.pack_record(frame=6)[:40])
    assert len(list(records.read_records(p))) == 7
    with open(p, "ab") as f:
        f.write(b"\0" * 200)
    with pytest.raises(ValueError):
        list(records.read_records(p))


def test_records_texture_flicker(tmp_path):
    frames = [frame_records(f, wrong=f in (103, 110, 111)) for f in range(100, 120)]
    res = records.analyse_records(records.read_records(write_log(tmp_path, frames)))
    assert res["outliers"] == [103, 110, 111], records.format_records_report(res)
    text = records.format_records_report(res)
    assert "t0 80CE6080(32x32 f0C) instead of 801C2380(128x256 f0C)" in text
    assert "vb 82B63000" in text
    assert res["structure"] == []


@pytest.mark.parametrize("swap", [False, True])
def test_records_double_buffering(tmp_path, swap):
    frames = [frame_records(f, wrong=f == 7, swap=swap) for f in range(16)]
    res = records.analyse_records(records.read_records(write_log(tmp_path, frames)))
    assert res["outliers"] == [7], records.format_records_report(res)
    assert res["structure"] == []


def test_records_constants_and_vertices_do_not_count(tmp_path):
    frames = [frame_records(f) for f in range(10)]
    res = records.analyse_records(records.read_records(write_log(tmp_path, frames)))
    assert res["count"] == 0
    # with constants: they change every frame, which is not a flicker either
    res = records.analyse_records(records.read_records(write_log(tmp_path, frames)), with_constants=True)
    assert res["count"] == 0


def test_records_missing_batch_and_gap(tmp_path):
    frames = [frame_records(f, drop_last=f == 4) for f in range(10)]
    frames += [frame_records(f, wrong=f == 20) for f in range(20, 25)]   # 20 has no frame 19
    res = records.analyse_records(records.read_records(write_log(tmp_path, frames)))
    assert [s[0] for s in res["structure"]] == [4]
    assert res["count"] == 0


def test_cli_batches(tmp_path, capsys):
    frames = [frame_records(f, wrong=f == 3) for f in range(8)]
    assert main(["batches", write_log(tmp_path, frames)]) == 1
    assert "verdict: FAIL (1 outlier frames" in capsys.readouterr().out


# ---- signature tables ------------------------------------------------------

def sig_log(frames, swap=False):
    """Tables of two kinds of batch; with swap, odd frames draw into the other
    buffer, so every signature differs (it includes the target address)."""
    lines = []
    for f, table in frames:
        rt = 0x80A7E000 if swap and f & 1 else 0x809E8000
        lines.append("[GPUSIG] frame %d: %d signatures, 2 target runs" % (f, len(table)))
        lines.append("[GPUSEQ] %08X(640x480)x3" % rt)
        for sig, (n, tris) in table.items():
            fmt = {"0000AAAA": "0C", "0000BBBB": "30"}[sig]
            lines.append("[GPUSIG] %08X n %d tris %d : rt %08X 640x480 sp 00001 | t0[m1 f%s k1 32x32 @%08X]"
                         % (int(sig, 16) ^ (rt & 0xFFFF0000), n, tris, rt, fmt, 0x80000000 + f * 64))
        lines.append("some other line")
    return lines


@pytest.mark.parametrize("swap", [False, True])
def test_sig_tables(tmp_path, capsys, swap):
    good = {"0000AAAA": (62, 9000), "0000BBBB": (6, 2813)}
    bad = {"0000AAAA": (63, 9400), "0000BBBB": (5, 2413)}
    lines = sig_log([(f, bad if f == 12 else good) for f in range(10, 16)], swap)
    tables = records.read_sig_tables(lines)
    assert sorted(tables) == list(range(10, 16))
    res = records.analyse_sig_tables(tables)
    assert res["outliers"] == [12]
    assert len(res["details"][0][1]) == 2
    p = tmp_path / "run.log"
    p.write_text("\n".join(lines) + "\n")
    assert main(["sigs", str(p)]) == 1
    assert "n 6 tris 2813 -> n 5 tris 2413" in capsys.readouterr().out


def test_black_step_is_not_a_spike(tmp_path):
    # a fade to black over two frames, and a cut into a dark scene: steps, not spikes
    def dark(t):
        return lambda x, y: (0, 0, 0) if y < 12 else scene(t)(x, y)
    for i in range(20):
        write_bmp(str(tmp_path / ("f%05d.bmp" % i)), dark(i) if i >= 10 else scene(i))
    res = images.analyse_frames(images.list_frames(str(tmp_path)), floor=0.01, detectors=("black",))
    assert res["black"] == []


def test_records_periodic_change_is_not_counted(tmp_path):
    # every 6th frame the batch is drawn with another texture (the title
    # re-renders something every 6 frames); frame 23 is a real glitch
    frames = [frame_records(f, wrong=f == 23) for f in range(40)]
    out = []
    for f, blob in enumerate(frames):
        if f % 6 == 0:
            blob += records.pack_record(frame=f, index=99, look=0x4444, target=0x809E8000, vb=0x82D00000,
                                        count=36, prog=13, tex0=0x83078000, fmt0=0x30)
        else:
            blob += records.pack_record(frame=f, index=99, look=0x4444, target=0x809E8000, vb=0x82D00000,
                                        count=36, prog=13, tex0=0x8038DC80, fmt0=0x0C)
        out.append(blob)
    res = records.analyse_records(records.read_records(write_log(tmp_path, out)))
    assert res["outliers"] == [23], records.format_records_report(res)
    assert res["periodic"] and res["periodic"][0][0] == 6
    assert "every 6 frames" in records.format_records_report(res)


def test_records_reordered_billboards_do_not_count(tmp_path):
    # one quad drawn for three billboards with three textures, in another
    # order every frame: the same picture
    texs = [0x80403580, 0x832C7680, 0x83341780]
    out = []
    for f in range(12):
        order = texs[f % 3:] + texs[:f % 3]
        out.append(b"".join(records.pack_record(frame=f, index=i, look=0x5555, target=0x809E8000,
                                                vb=0x80D7A010, count=36, prog=13, tex0=t, fmt0=0x0C)
                            for i, t in enumerate(order)))
    res = records.analyse_records(records.read_records(write_log(tmp_path, out)))
    assert res["count"] == 0 and res["structure"] == []


def test_black_in_a_transition_is_not_counted(tmp_path):
    # every frame another picture (a loading sequence): a dark one among them
    # is no glitch; the same dark frame in a steady scene is
    def noise(t, dark=False):
        return lambda x, y: (0, 0, 0) if dark and y < 12 else ((x * 37 + y * 11 + t * 101) % 256, (t * 53) % 256, 90)
    for i in range(16):
        write_bmp(str(tmp_path / ("f%05d.bmp" % i)), noise(i, dark=i == 8))
    res = images.analyse_frames(images.list_frames(str(tmp_path)), floor=0.01)
    assert res["count"] == 0, images.format_report(res)
    res = images.analyse_frames(images.list_frames(str(tmp_path)), floor=0.01, detectors=("black",))
    assert [b[0] for b in res["black"]] == [8]      # without the flash pass: no steadiness test
