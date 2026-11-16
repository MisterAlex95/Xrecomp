"""Tests of the GameCube disc images (XBA-261): the test-image writer (gc_tools.disc) and, built from source with the
host cc, the runtime's readers (gc_disc_info over ISO / CISO / GCZ / RVZ / WIA) and the apploader boot (gc_run).
Every image is generated here from our own programs: no game data."""

import hashlib
import os
import shutil
import struct
import subprocess
import sys

import pytest

from . import CONSOLE  # noqa: F401
from . import cbuild, disc
from .dol import Image

CC = os.environ.get("GC_TEST_CC", "cc")
CFLAGS = ["-O2", "-ffp-contract=off", "-Wall", "-Werror"]


def test_padding_generator_positions():
    seed = disc.block_seed(b"GXRE", 3)
    whole = disc.lfg_bytes(seed, 0, 0x8000)
    assert disc.lfg_bytes(seed, 0x1234, 0x100) == whole[0x1234:0x1334]
    assert disc.lfg_bytes(seed, 2084 * 3 + 5, 77) == whole[2084 * 3 + 5:2084 * 3 + 82]
    # pinned: the generator must not drift (RVZ images store seeds, not bytes)
    assert hashlib.sha256(whole).hexdigest()[:16] == "d40a2d2c9e4e8387"
    b = list(struct.unpack(">17I", seed)) + [0] * 504
    for i in range(17, 521):
        b[i] = ((b[i - 17] << 23) & 0xFFFFFFFF) ^ (b[i - 16] >> 9) ^ b[i - 1]
    for _ in range(4):
        disc._forward(b)
    w = b[0]
    assert whole[:4] == bytes(((w >> 24) & 255, (w >> 18) & 255, (w >> 8) & 255, w & 255))


def test_zstd_frame_blocks():
    f = disc.zstd_frame(bytes(1000) + b"abc")
    assert struct.unpack("<I", f[:4])[0] == 0xFD2FB528 and f[4] == 0xA0
    assert struct.unpack("<I", f[5:9])[0] == 1003
    hdr = int.from_bytes(f[9:12], "little")
    assert hdr & 1 == 1 and (hdr >> 1) & 3 == 0 and hdr >> 3 == 1003  # one raw block, last
    rle = disc.zstd_frame(bytes(500))
    hdr = int.from_bytes(rle[9:12], "little")
    assert (hdr >> 1) & 3 == 1 and hdr >> 3 == 500 and len(rle) == 13


def test_gcm_layout():
    dol = disc.di_program(0x18000, 0x3010)
    gcm = disc.build_gcm(dol, disc.test_files())
    d = gcm.data
    assert struct.unpack(">I", d[0x1C:0x20])[0] == disc.MAGIC
    assert d[:6] == b"GXRE01" and d[6] == 0 and d[7] == 0
    dol_off, fst_off, fst_size = struct.unpack(">III", d[0x420:0x42C])
    assert d[dol_off:dol_off + len(dol)] == dol
    assert Image(d[dol_off:dol_off + len(dol)]).entry == 0x80003100
    entry, size, trailer = struct.unpack(">III", d[0x2450:0x245C])
    assert entry == disc.APPLOADER_BASE and size > 0 and trailer == 0
    assert struct.unpack(">I", d[fst_off + 8:fst_off + 12])[0] == 7  # root, 4 files, 2 directories
    for path, off, n, data in gcm.files:
        assert off % disc.BLOCK == 0 and d[off:off + n] == data


@pytest.mark.skipif(not shutil.which(CC) or not sys.platform.startswith("linux") or os.environ.get("GC_TEST_EMULATOR"),
                    reason="no C compiler / not Linux / cross test")
def test_formats_and_boot(tmp_path):
    build = str(tmp_path / "build")
    run = cbuild.gc_run(build, CC, CFLAGS)
    info = cbuild.gc_disc_info(build, CC, CFLAGS)
    script = os.path.join(cbuild.REPO, "consoles", "gc", "tests", "run_disc.sh")
    r = subprocess.run(["sh", script, "formats", run, info, str(tmp_path / "disc")], capture_output=True, text=True,
                       timeout=600)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "ok   DVD results" in r.stdout and "ok   disc change" in r.stdout
    assert r.stdout.count("ok   test") == len(disc.FORMATS) + 1
