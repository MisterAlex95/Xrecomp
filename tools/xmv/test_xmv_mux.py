"""The XMV writer, the Xbox ADPCM encoder and the synthetic WMV2 streams
(tools/xmv, XBA-70), and the fixtures tests/xmv_decode decodes. No FFmpeg
needed: remaking the fixtures' references is not tested here."""

import os
import random
import struct

from tools.xmv.xmv_demux import XMVFile
from tools.xmv.xmv_mux import (ADPCM_BLOCK, ADPCM_SAMPLES, STEP_TABLE, INDEX_ADJUST, encode_xbox_adpcm, le_words,
                               mux_xmv, tone, xmv_header_word, SYNTH_SET, TEST_SET)
from tools.xmv.wmv2_synth import synth_movie

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "..", "..", "tests", "xmv_decode", "fixtures")


def decode_adpcm(data, channels):
    """A straightforward Xbox ADPCM decoder (the APU's arithmetic)."""
    out = []
    blk = ADPCM_BLOCK * channels
    for b in range(0, len(data) - blk + 1, blk):
        block = data[b:b + blk]
        pred, index, frames = [], [], [[0] * channels for _ in range(ADPCM_SAMPLES)]
        for ch in range(channels):
            p, ix, _ = struct.unpack_from("<hBB", block, 4 * ch)
            pred.append(p)
            index.append(max(0, min(88, ix if ix < 128 else ix - 256)))
            frames[0][ch] = p
        for chunk in range(8):
            for ch in range(channels):
                word = block[4 * channels + (chunk * channels + ch) * 4:][:4]
                for k, byte in enumerate(word):
                    for half, nib in enumerate((byte & 15, byte >> 4)):
                        step = STEP_TABLE[index[ch]]
                        diff = step >> 3
                        if nib & 4:
                            diff += step
                        if nib & 2:
                            diff += step >> 1
                        if nib & 1:
                            diff += step >> 2
                        if nib & 8:
                            diff = -diff
                        pred[ch] = max(-32768, min(32767, pred[ch] + diff))
                        index[ch] = max(0, min(88, index[ch] + INDEX_ADJUST[nib & 7]))
                        frames[1 + chunk * 8 + 2 * k + half][ch] = pred[ch]
        for f in frames:
            out.extend(f)
    return out


def test_adpcm_encoder_reconstructs_what_a_decoder_decodes():
    for channels in (1, 2):
        pcm = tone(22050, channels, 1000)
        data, decoded = encode_xbox_adpcm(pcm, channels)
        nblocks = (1000 + ADPCM_SAMPLES - 1) // ADPCM_SAMPLES
        assert len(data) == nblocks * ADPCM_BLOCK * channels
        assert decode_adpcm(data, channels) == decoded
        # And it follows the input once the step size has adapted (the first
        # block starts at the smallest step).
        tail = list(zip(pcm, decoded))[200 * channels:]
        err = sum(abs(a - b) for a, b in tail) / len(tail)
        assert err < 1000, err


def test_header_word_packing():
    # FFmpeg's wmv2 encoder header: 30 fps, mspel, abt, j_type allowed, per-MB RL, one slice.
    assert xmv_header_word(bytes.fromhex("f0c3b480")) == (1 | 4 | 8 | 32 | (1 << 6))


def test_le_words():
    assert le_words(b"\x01\x02\x03\x04\x05") == b"\x04\x03\x02\x01\x00\x00\x00\x05"


def test_mux_demux_round_trip(tmp_path):
    r = random.Random(7)
    frames = []
    for k in range(9):
        body = bytes(r.randrange(256) for _ in range(r.randrange(4, 60)))
        # First bit 0 for the I frames (k % 4 == 0), 1 for the others.
        first = (body[0] & 0x7F) | (0 if k % 4 == 0 else 0x80)
        frames.append(bytes([first]) + body[1:])
    adpcm, _ = encode_xbox_adpcm(tone(11025, 1, 3000), 1)
    blob = mux_xmv(64, 48, frames, 0x45, fps=30,
                   audio=[(0x0069, 1, 11025, 4, 0, adpcm)], frames_per_packet=4)
    path = tmp_path / "round_trip.xmv"
    path.write_bytes(blob)
    x = XMVFile(path)
    assert (x.width, x.height, x.audio_track_count, x.version) == (64, 48, 1, 4)
    assert x.audio_tracks[0].compression == 0x0069 and x.audio_tracks[0].sample_rate == 11025
    got = [f for p in x.packets for f in p.frames]
    assert len(x.packets) == 3 and len(got) == 9
    for k, f in enumerate(got):
        assert f.data == le_words(frames[k])
        assert f.is_keyframe == (k % 4 == 0)
        assert f.timestamp == round(k * 1000 / 30)
    assert x.packets[0].extradata is not None
    assert b"".join(x.data[p.audio_offsets[0]:p.audio_offsets[0] + p.audio_data_sizes[0]] for p in x.packets) == adpcm


def test_synth_streams_are_deterministic_and_whole_words():
    for name, (w, h), n, gop, seed, kw in SYNTH_SET:
        a = synth_movie(w, h, n, gop, seed, **kw)
        b = synth_movie(w, h, n, gop, seed, **kw)
        assert a == b, name
        word, frames = a
        assert len(frames) == n and all(len(f) % 4 == 0 and f for f in frames)
        assert all((f[0] & 0x80) == (0 if k % gop == 0 else 0x80) for k, f in enumerate(frames))


def test_fixtures_are_there_and_listed():
    names = [t[0] for t in TEST_SET] + [t[0] for t in SYNTH_SET]
    expected = open(os.path.join(FIXTURES, "expected.h")).read()
    for name in names:
        x = XMVFile(os.path.join(FIXTURES, name + ".xmv"))
        assert sum(len(p.frames) for p in x.packets) > 0
        assert '"%s"' % name in expected


def test_fallback_fixtures_are_what_make_fallback_set_writes(tmp_path):
    """tests/xmv_hle's hand-over fixtures (XBA-199): deterministic, and the X8 frame where it is said to be."""
    from tools.xmv.xmv_mux import FALLBACK_SET, make_fallback_set
    make_fallback_set(str(tmp_path))
    here = os.path.join(HERE, "..", "..", "tests", "xmv_hle", "fixtures")
    for name, *_ in FALLBACK_SET:
        made = (tmp_path / (name + ".xmv")).read_bytes()
        assert made == open(os.path.join(here, name + ".xmv"), "rb").read(), name
    _, frames = synth_movie(96, 64, 12, 4, 11, x8_at=(4,))
    w = struct.unpack(">I", frames[4][:4])[0]
    assert (w >> 31) == 0 and (w >> 18) & 1 and (w >> 19) & 31     # I, j_type 1, a quantiser
    assert all(not ((struct.unpack(">I", frames[k][:4])[0] >> 18) & 1) for k in (0, 8))
