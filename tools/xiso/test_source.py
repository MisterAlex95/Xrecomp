"""tools/xiso/source.py (XBA-126): split and compressed images read back byte for byte, through Source and Xiso,
from images mkxiso.py and mkcso.py build (ours, synthetic)."""
import os
import random
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..")))
sys.path.insert(0, HERE)

import mkcso                                           # noqa: E402
import mkxiso                                          # noqa: E402
from tools.xiso.source import Source, SourceError, is_image_name, lz4_block, split_parts   # noqa: E402
from tools.xiso.xdvdfs import Xiso                     # noqa: E402


@pytest.fixture(scope="module")
def disc(tmp_path_factory):
    t = tmp_path_factory.mktemp("disc")
    src = t / "src"
    (src / "media" / "sub").mkdir(parents=True)
    rnd = random.Random(126)
    files = {"default.xbe": b"XBEH" + bytes(rnd.getrandbits(8) for _ in range(5000)),
             "media/zeros.bin": b"\0" * 70000,
             "media/text.txt": b"the quick brown fox " * 900,
             "media/sub/rand.bin": bytes(rnd.getrandbits(8) for _ in range(40000)),
             "empty.bin": b""}
    for rel, data in files.items():
        (src / rel).write_bytes(data)
    iso = t / "Game.iso"
    mkxiso.build(str(src), str(iso))
    return t, iso, files


def _tree(path):
    with Xiso(str(path)) as iso:
        return {(d + "/" + e.name).lstrip("/"): iso.read(e) for d, e in iso.walk()}


@pytest.mark.parametrize("fmt", ["cso1", "cso2", "cso2-framed", "cso2-maxcso", "zso", "cci"])
def test_compressed_formats_read_back(disc, fmt):
    t, iso, files = disc
    out = t / ("g_%s.img" % fmt)
    out.write_bytes(mkcso.compress(iso.read_bytes(), fmt))
    with Source(str(out)) as s:
        assert s.kind == {"zso": "zso", "cci": "cci"}.get(fmt, "cso")
        assert s.size == iso.stat().st_size
        plain = iso.read_bytes()
        rnd = random.Random(fmt)
        for _ in range(200):
            off = rnd.randrange(0, len(plain) + 50)
            n = rnd.randrange(0, 9000)
            assert s.read_at(off, n) == plain[off:off + n]
    assert _tree(out) == files


@pytest.mark.parametrize("naming,first", [("dot", "G.1.iso"), ("part", "G.part1.iso"), ("ext", "G.iso.1"),
                                          ("ext3", "G.iso.001")])
def test_split_names(disc, naming, first):
    t, iso, files = disc
    d = t / ("split_" + naming)
    d.mkdir()
    parts = mkcso.write_split(iso.read_bytes(), str(d / "G.iso"), 30000, naming)
    assert os.path.basename(parts[0]) == first and len(parts) > 3
    for p in (parts[0], parts[2], parts[-1]):           # any part names the whole image
        assert split_parts(p) == parts
        assert _tree(p) == files
        assert is_image_name(p)
    with Source(parts[1]) as s:
        assert s.read_at(29990, 30) == iso.read_bytes()[29990:30020]       # across a cut


def test_split_cso(disc):
    t, iso, files = disc
    parts = mkcso.write_split(mkcso.compress(iso.read_bytes(), "cso2"), str(t / "S.cso"), 3000, "dot")
    with Source(parts[0]) as s:
        assert s.kind == "cso" and len(s.parts) == len(parts) > 1
    assert _tree(parts[-1]) == files


def test_not_splits(tmp_path):
    lone = tmp_path / "Halo.2.iso"                       # no .1: one image named with a number
    lone.write_bytes(b"x")
    assert split_parts(str(lone)) == [str(lone)]
    one = tmp_path / "x.1.iso"
    one.write_bytes(b"x")
    assert split_parts(str(one)) == [str(one)]
    assert not is_image_name(str(tmp_path / "notes.txt"))
    assert is_image_name("a/Game.ISO") and is_image_name("Game.cci") and is_image_name("Game.iso.003")


def test_broken_compressed(disc):
    t, iso, _ = disc
    good = bytearray(mkcso.compress(iso.read_bytes(), "zso"))
    short = t / "short.zso"
    short.write_bytes(bytes(good[:len(good) - 2000]))
    with pytest.raises(SourceError):
        Source(str(short)).read_at(iso.stat().st_size - 100, 100)
    bad = t / "bad.zso"
    first = (int.from_bytes(good[24:28], "little") & 0x7FFFFFFF) << good[21]
    good[first:first + 64] = b"\xff" * 64
    bad.write_bytes(bytes(good))
    with Source(str(bad)) as s:
        with pytest.raises(SourceError):
            s.read_at(0, 100)
        assert s.read_at(32 * 2048, 20) == b"MICROSOFT*XBOX*MEDIA"   # the other blocks still read


def test_lz4_roundtrip():
    rnd = random.Random(7)
    for data in (b"", b"a", b"abcd" * 1000, bytes(rnd.getrandbits(8) for _ in range(3000)), b"\0" * 2048):
        c = mkcso.lz4_compress(data)
        assert lz4_block(c + b"\0\0\0", len(data)) == data          # padding after the block is ignored
