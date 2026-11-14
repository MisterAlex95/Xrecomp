"""ps1_tools.chd (XBA-159 / XBA-168): CHD v5 CD images written here in the shapes chdman writes (uncompressed, cdzl,
cdlz, cdzs on Python 3.14+; Huffman-coded maps, raw and repeated hunks, stripped syncs, an audio track) read back
frame for frame; an ISO 9660 disc read through ps1_tools.exe from a .chd; the errors; and, when chdman is on the PATH,
images chdman itself made."""

import os
import shutil
import struct
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from ps1_tools import chd, exe  # noqa: E402

SYNC = b"\x00" + b"\xff" * 10 + b"\x00"
HAVE_ZSTD = sys.version_info >= (3, 14)


def data_frame(lba, fill=None):
    """A raw mode 2 form 1 sector: sync, header (MSF of lba), subheader, 2048 user bytes."""
    a = lba + 150
    s = bytearray(2352)
    s[0:12] = SYNC
    bcd = lambda v: (v // 10) << 4 | v % 10  # noqa: E731
    s[12:16] = bytes([bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75), 2])
    s[16:24] = bytes([0, 0, 8, 0, 0, 0, 8, 0])
    user = bytes(((lba * 7 + i) & 0xFF) for i in range(2048)) if fill is None else bytes([fill]) * 2048
    s[24:24 + 2048] = user
    return bytes(s)


def audio_frame(n):
    return bytes(((n * 13 + i * 5) & 0xFF) for i in range(2352))


def frames_of(chd_file, n):
    c = chd.Chd(chd_file)
    try:
        return [c.frame(lba) for lba in range(n)]
    finally:
        c.close()


@pytest.mark.parametrize("codec", ["none", "cdzl", "cdlz"] + (["cdzs"] if HAVE_ZSTD else []))
@pytest.mark.parametrize("strip", [False, True])
def test_round_trip(tmp_path, codec, strip):
    # 37 data frames (hunks 2 and 3 the same: a repeated hunk), then a 13-frame audio track
    data = [data_frame(16, fill=0x55) if 16 <= i < 32 else data_frame(i) for i in range(37)]
    audio = [audio_frame(i) for i in range(13)]
    p = str(tmp_path / ("d_%s.chd" % codec))
    chd.write_cd(p, [("MODE2_RAW", data), ("AUDIO", audio)], codec=codec, strip_sync=strip)
    c = chd.Chd(p)
    try:
        assert [(t.num, t.type, t.begin, t.start, t.end) for t in c.tracks] == \
            [(1, "MODE2_RAW", 0, 0, 37), (2, "AUDIO", 37, 37, 50)]
        for lba in range(37):
            assert c.frame(lba) == data[lba], lba
            assert c.user_data(lba) == data[lba][24:24 + 2048]
        for i in range(13):
            # stored big-endian: the frame read is the stored one, swapped back by the player (ps1_disc.c)
            f = c.frame(37 + i)
            assert f[0::2] == audio[i][1::2] and f[1::2] == audio[i][0::2]
        with pytest.raises(ValueError, match="audio"):
            c.user_data(40)
        with pytest.raises(ValueError, match="past the end"):
            c.frame(50)
        if codec != "none":
            assert any(e[0] == chd.SELF for e in c.map)       # hunk 3 repeats hunk 2
    finally:
        c.close()


def test_raw_hunks_and_runs(tmp_path):
    # 40 hunks: 0..2 compressed, 3..24 stored raw (type NONE: a run of 22, two run-length codes), the rest compressed
    # (a run of 15).
    rnd = os.urandom(2352 * 320)
    data = [rnd[i * 2352:(i + 1) * 2352] for i in range(320)]
    p = str(tmp_path / "r.chd")
    chd.write_cd(p, [("MODE2_RAW", data)], codec="cdlz", raw_hunks=range(3, 25))
    c = chd.Chd(p)
    try:
        assert [e[0] for e in c.map] == [chd.TYPE_0] * 3 + [chd.NONE] * 22 + [chd.TYPE_0] * 15
        assert [c.frame(i) for i in range(320)] == data
    finally:
        c.close()


def make_iso_frames(exe_bytes, cnf=b"BOOT = cdrom:\\SLUS_005.94;1\r\n"):
    """A PS1 data track as raw frames: PVD at 16, root directory at 18, SYSTEM.CNF at 20, the EXE at 21."""
    def rec(name, lba, size, is_dir):
        n = name.encode() if isinstance(name, str) else name
        ln = 33 + len(n) + ((33 + len(n)) & 1)
        r = bytearray(ln)
        r[0] = ln
        struct.pack_into("<I", r, 2, lba)
        struct.pack_into("<I", r, 10, size)
        r[25] = 2 if is_dir else 0
        r[32] = len(n)
        r[33:33 + len(n)] = n
        return bytes(r)
    users = {}
    pvd = bytearray(2048)
    pvd[0], pvd[1:6], pvd[8:19] = 1, b"CD001", b"PLAYSTATION"
    pvd[156:156 + 34] = rec(b"\x00", 18, 2048, True)
    users[16] = bytes(pvd)
    root = rec(b"\x00", 18, 2048, True) + rec(b"\x01", 18, 2048, True) + rec("SYSTEM.CNF;1", 20, len(cnf), False) + \
        rec("SLUS_005.94;1", 21, len(exe_bytes), False)
    users[18] = root.ljust(2048, b"\0")
    users[20] = cnf.ljust(2048, b"\0")
    for i in range(0, len(exe_bytes), 2048):
        users[21 + i // 2048] = exe_bytes[i:i + 2048].ljust(2048, b"\0")
    out = []
    for lba in range(max(users) + 1):
        f = bytearray(data_frame(lba))
        f[24:24 + 2048] = users.get(lba, bytes(2048))
        out.append(bytes(f))
    return out


def test_disc_through_exe_reader(tmp_path):
    head = bytearray(0x800)
    head[:8] = b"PS-X EXE"
    text = bytes((i * 3) & 0xFF for i in range(0x3000))
    struct.pack_into("<10I", head, 0x10, 0x80010000, 0, 0x80010000, len(text), 0, 0, 0, 0, 0x801FFFF0, 0)
    boot = bytes(head) + text
    for codec in ("none", "cdzl", "cdlz"):
        p = str(tmp_path / ("g_%s.chd" % codec))
        chd.write_cd(p, [("MODE2_RAW", make_iso_frames(boot)), ("AUDIO", [audio_frame(1)] * 5)], codec=codec)
        d = exe.Disc(p)
        try:
            assert d.pvd()[8:19] == b"PLAYSTATION"
            assert d.boot_path() == "\\SLUS_005.94;1"
        finally:
            d.close()
        e = exe.load_input(p)
        assert e.pc0 == 0x80010000 and e.text == text and e.name == "SLUS_005.94"


def corrupt(path, off, data):
    with open(path, "r+b") as f:
        f.seek(off)
        f.write(data)


def test_errors(tmp_path):
    data = [data_frame(i) for i in range(24)]
    good = str(tmp_path / "g.chd")
    chd.write_cd(good, [("MODE2_RAW", data)], codec="cdlz")
    blob = open(good, "rb").read()

    def variant(name, mutate):
        p = str(tmp_path / name)
        b = bytearray(blob)
        mutate(b)
        open(p, "wb").write(bytes(b))
        return p

    with pytest.raises(ValueError, match="magic"):
        chd.Chd(variant("m.chd", lambda b: b.__setitem__(slice(0, 8), b"MComprHE")))
    with pytest.raises(ValueError, match="version 4"):
        chd.Chd(variant("v4.chd", lambda b: struct.pack_into(">I", b, 12, 4)))
    with pytest.raises(ValueError, match="parent"):
        chd.Chd(variant("p.chd", lambda b: b.__setitem__(104, 1)))
    with pytest.raises(ValueError, match="not a CD image"):
        chd.Chd(variant("h.chd", lambda b: struct.pack_into(">I", b, 56, 4096)))
    map_off = struct.unpack_from(">Q", blob, 40)[0]
    with pytest.raises(ValueError, match="CRC"):
        chd.Chd(variant("crc.chd", lambda b: struct.pack_into(">H", b, map_off + 10,
                                                              struct.unpack_from(">H", b, map_off + 10)[0] ^ 1)))
    with pytest.raises(ValueError, match="truncated"):
        chd.Chd(variant("t.chd", lambda b: b.__delitem__(slice(map_off + 4, None))))
    # a data hunk whose codec is FLAC (codec 0 relabelled cdfl): reported, not decoded
    p = variant("fl.chd", lambda b: b.__setitem__(slice(16, 20), b"cdfl"))
    c = chd.Chd(p)
    with pytest.raises(ValueError, match="FLAC"):
        c.frame(0)
    c.close()
    p = variant("zs.chd", lambda b: b.__setitem__(slice(16, 20), b"cdzs"))
    c = chd.Chd(p)
    with pytest.raises(ValueError, match="zstd" if not HAVE_ZSTD else "decompress"):
        c.frame(0)
    c.close()
    # no track metadata
    with pytest.raises(ValueError, match="metadata"):
        chd.Chd(variant("nm.chd", lambda b: struct.pack_into(">Q", b, 48, 0)))
    # an audio first track: no data to read
    p = str(tmp_path / "a.chd")
    chd.write_cd(p, [("AUDIO", [audio_frame(0)] * 8)], codec="cdzl")
    with pytest.raises(ValueError, match="data track"):
        exe.Disc(p)


def test_cli(tmp_path):
    bin_path = tmp_path / "t.bin"
    bin_path.write_bytes(b"".join(data_frame(i) for i in range(10)))
    out = str(tmp_path / "t.chd")
    assert chd.main(["write", "--codec", "cdzl", out, str(bin_path)]) == 0
    assert frames_of(out, 10) == [data_frame(i) for i in range(10)]
    assert chd.main(["info", out]) == 0


CHDMAN = shutil.which("chdman")


@pytest.mark.skipif(not CHDMAN, reason="no chdman (MAME tools) on the PATH")
def test_chdman_images(tmp_path):
    """Images chdman made from a .cue: a data track, an audio track with a stored pregap; every codec chdman offers
    for CDs (cdfl only where the hunk is audio; cdzs only on Python 3.14+)."""
    import math
    data = make_iso_frames(bytes(0x800 * 3).replace(b"\0" * 8, b"PS-X EXE", 1))
    (tmp_path / "data.bin").write_bytes(b"".join(data))
    pcm = bytearray()
    for i in range(80 * 588):
        v = int(9000 * math.sin(i * 0.03))
        pcm += struct.pack("<hh", v, -v)
    (tmp_path / "audio.bin").write_bytes(bytes(pcm))
    (tmp_path / "d.cue").write_text('FILE "data.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
                                    'FILE "audio.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n'
                                    '    INDEX 01 00:00:30\n')
    for codecs in (None, "cdzl", "cdlz") + (("cdzs",) if HAVE_ZSTD else ()):
        out = str(tmp_path / ("%s.chd" % (codecs or "default")))
        cmd = [CHDMAN, "createcd", "-f", "-i", str(tmp_path / "d.cue"), "-o", out] + (["-c", codecs] if codecs else [])
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
        c = chd.Chd(out)
        try:
            assert [(t.type, t.begin, t.start, t.end) for t in c.tracks] == \
                [("MODE2_RAW", 0, 0, len(data)), ("AUDIO", len(data), len(data) + 30, len(data) + 80)]
            for lba in range(len(data)):
                assert c.user_data(lba) == data[lba][24:24 + 2048]
        finally:
            c.close()
