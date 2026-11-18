"""Tests of the LZX decoder (XBA-270) on streams made by encoders that are not ours (tools/xex/lzx_vectors/gen.sh:
wimlib, Free Pascal's chmcmd; the CHM ones also checked by Free Pascal's reader and, without a reset, by cabextract),
from data made in lzx_vectors/vector_data.py. No console data."""

import hashlib
import json
import os
import sys

import pytest

from tools.xex import lzx

HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(HERE, "lzx_vectors")
sys.path.insert(0, VEC)
from vector_data import data  # noqa: E402

with open(os.path.join(VEC, "vectors.json")) as _f:
    VECTORS = json.load(_f)
STREAMS = sorted(k for k in VECTORS if not k.endswith(".stored"))


def _stream(name):
    with open(os.path.join(VEC, name + ".lzx"), "rb") as f:
        return f.read()


def _decode(name, stats=None):
    v = VECTORS[name]
    raw = data(v["data"])
    s = _stream(name)
    if v["flavour"] == "wim":
        return lzx.decompress(s, v["window_bits"], len(raw), "wim", stats=stats), raw
    c = v["chm"]
    out = lzx.decompress(s, c["window_bits"], c["out_size"], "cab", c["reset_interval"], stats=stats)
    return out[c["file_offset"]:c["file_offset"] + c["file_length"]], raw


@pytest.mark.parametrize("name", STREAMS)
def test_vector(name):
    st = {}
    out, raw = _decode(name, st)
    assert out == raw
    assert st["literals"] and st["matches"] and st["repeat_matches"]


def test_vectors_cover_the_format():
    """What the vectors exercise, all together: both block kinds the encoders make, aligned offsets, E8 translation,
    several frames with and without a reset, windows of 2^15 to 2^19."""
    blocks, e8, frames, bits = set(), False, 0, set()
    for name in STREAMS:
        st = {}
        _decode(name, st)
        blocks |= set(st["blocks"])
        e8 |= st["e8"]
        frames = max(frames, st["frames"])
        v = VECTORS[name]
        bits.add(v.get("window_bits") or v["chm"]["window_bits"])
        if st["aligned_offsets"]:
            blocks.add("aligned offsets")
    assert {"verbatim", "aligned", "aligned offsets"} <= blocks
    assert e8 and frames >= 6 and bits >= {15, 16, 18, 19}


@pytest.mark.parametrize("name", ["small.stored", "mixed.stored"])
def test_store_matches_the_checked_stream(name):
    """lzx.store (what synth.py's "normal" XEX uses) gives the stream cabextract decoded when the vectors were made."""
    v = VECTORS[name]
    s = lzx.store(data(v["data"]))
    assert hashlib.sha256(s).hexdigest() == v["sha256"]
    st = {}
    assert lzx.decompress(s, 15, len(data(v["data"])), stats=st) == data(v["data"])
    assert set(st["blocks"]) == {"uncompressed"}


def test_xex_block_chain_round_trip():
    """An external stream cut into XEX blocks and chunks, then walked back (SHA-1 of every block checked)."""
    s = _stream("mid.chm")
    chunks = [s[o:o + 5000] for o in range(0, len(s), 5000)]
    payload, size, digest = lzx.xex_reblock(chunks, block_limit=0x3000)
    assert lzx.xex_deblock(payload, size, digest) == s
    bad = bytearray(payload)
    bad[size + 40] ^= 1  # inside the second block
    with pytest.raises(lzx.LzxError, match="SHA-1"):
        lzx.xex_deblock(bytes(bad), size, digest)
    with pytest.raises(lzx.LzxError):
        lzx.xex_deblock(payload[:size + 10], size, digest)


def test_garbage_raises_and_never_hangs():
    raw = data("small")
    s = _stream("small-100.wim")
    for k in range(0, 200, 7):
        bad = bytearray(s)
        bad[k] ^= 0x5A
        try:
            out = lzx.decompress(bytes(bad), 15, len(raw), "wim")
        except lzx.LzxError:
            continue
        assert len(out) == len(raw)  # a corruption may still decode: then to the right size
    with pytest.raises(lzx.LzxError):
        lzx.decompress(s[:100], 15, len(raw), "wim")
    for seed in range(20):
        junk = hashlib.sha256(bytes([seed])).digest() * 64
        try:
            lzx.decompress(junk, 16, 50000)
        except lzx.LzxError:
            pass
    with pytest.raises(lzx.LzxError):
        lzx.window_bits_of(0x9000)
    assert lzx.window_bits_of(0x20000) == 17
