"""
XMV writer and test-fixture maker (XBA-70).

Builds XMV files from our own material, for the runtime's XMV decoder tests
(src/video/xmv, tests/xmv_decode). Never used on game data.

- `mux_xmv()` packs WMV2 frames and audio blocks into the XMV container the
  Xbox's movie player reads (layout: docs/runtime/xmv.md, the inverse of
  xmv_demux.py / src/video/xmv/xmv_demux.c).
- `encode_xbox_adpcm()` is an Xbox ADPCM (IMA, 36-byte blocks per channel)
  encoder that also returns what a decoder must produce.
- `wmv2_from_avi()` reads the WMV2 frames and sequence header out of an AVI
  that FFmpeg's wmv2 encoder wrote.
- `make-fixture` (CLI) does the lot: FFmpeg encodes a synthetic test pattern
  to WMV2, the frames are muxed with a synthetic tone, and FFmpeg's own XMV
  demuxer + WMV2 decoder decode the result as the reference: one CRC-32 per
  decoded frame, written next to the file. FFmpeg is only needed to make
  fixtures, not to run the tests.

Usage:
    python -m tools.xmv.xmv_mux make-test-set tests/xmv_decode/fixtures
    python -m tools.xmv.xmv_mux make-fallback-set tests/xmv_hle/fixtures
    python -m tools.xmv.xmv_mux make-fixture OUT.xmv --size 96x64 --frames 12 \
        [--gop 6] [--q 4] [--channels 2] [--rate 22050] [--loop-filter] [--slices N]
"""

import argparse
import math
import os
import struct
import subprocess
import tempfile
import zlib

# IMA ADPCM tables (IMA Recommended Practices, 1992).
STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
INDEX_ADJUST = [-1, -1, -1, -1, 2, 4, 6, 8]

ADPCM_BLOCK = 36          # bytes per channel
ADPCM_SAMPLES = 65        # sample frames per block


def _adpcm_step(pred, index, nib):
    step = STEP_TABLE[index]
    diff = step >> 3
    if nib & 4:
        diff += step
    if nib & 2:
        diff += step >> 1
    if nib & 1:
        diff += step >> 2
    if nib & 8:
        diff = -diff
    pred = max(-32768, min(32767, pred + diff))
    index = max(0, min(88, index + INDEX_ADJUST[nib & 7]))
    return pred, index


def encode_xbox_adpcm(samples, channels):
    """Interleaved int16 samples -> (ADPCM bytes, decoded interleaved samples).

    The input is padded with zeros to whole blocks of 65 frames. The decoded
    samples are what the Xbox (and src/video/xmv/xbox_adpcm.c) decode."""
    frames = len(samples) // channels
    nblocks = (frames + ADPCM_SAMPLES - 1) // ADPCM_SAMPLES
    samples = list(samples) + [0] * (nblocks * ADPCM_SAMPLES * channels - len(samples))
    out = bytearray()
    decoded = [0] * len(samples)
    index = [0] * channels
    for b in range(nblocks):
        base = b * ADPCM_SAMPLES
        pred = [0] * channels
        codes = [[] for _ in range(channels)]
        for ch in range(channels):
            first = samples[base * channels + ch]
            pred[ch] = first
            decoded[base * channels + ch] = first
            out += struct.pack("<hBB", first, index[ch], 0)
        for ch in range(channels):
            p, ix = pred[ch], index[ch]
            for k in range(1, ADPCM_SAMPLES):
                target = samples[(base + k) * channels + ch]
                # Greedy: the code whose decode lands nearest the target.
                best = None
                for nib in range(16):
                    np_, nix = _adpcm_step(p, ix, nib)
                    err = abs(np_ - target)
                    if best is None or err < best[0]:
                        best = (err, nib, np_, nix)
                _, nib, p, ix = best
                codes[ch].append(nib)
                decoded[(base + k) * channels + ch] = p
            pred[ch], index[ch] = p, ix
        for chunk in range(8):
            for ch in range(channels):
                c = codes[ch][chunk * 8:chunk * 8 + 8]
                out += bytes(c[2 * i] | (c[2 * i + 1] << 4) for i in range(4))
    return bytes(out), decoded


def _chunks(data, off, end):
    while off + 8 <= end:
        cid = data[off:off + 4]
        size = struct.unpack_from("<I", data, off + 4)[0]
        yield cid, off + 8, size
        off += 8 + size + (size & 1)


def wmv2_from_avi(path):
    """(4-byte WMV2 sequence header, [frame bytes]) from an AVI with one
    WMV2 video stream (FFmpeg's wmv2 encoder output)."""
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"AVI ":
        raise ValueError("not an AVI file")
    ext = None
    frames = []

    def walk(off, end):
        nonlocal ext
        for cid, body, size in _chunks(data, off, end):
            if cid == b"LIST":
                walk(body + 4, body + size)
            elif cid == b"strf" and size >= 44 and data[body + 16:body + 20] == b"WMV2":
                ext = data[body + 40:body + 44]
            elif cid[2:4] in (b"dc", b"db") and cid[:2] == b"00":
                frames.append(data[body:body + size])
    walk(12, len(data))
    if ext is None:
        raise ValueError("no WMV2 stream header in the AVI")
    return ext, frames


def xmv_header_word(ext):
    """WMV2 extradata (4 bytes, big-endian bit fields) -> XMV's packing."""
    w = struct.unpack(">I", ext)[0]
    return (((w >> 15) & 1) | (((w >> 14) & 1) << 1) | (((w >> 13) & 1) << 2) |
            (((w >> 12) & 1) << 3) | (((w >> 11) & 1) << 4) | (((w >> 10) & 1) << 5) |
            (((w >> 7) & 7) << 6))


def le_words(frame):
    """A big-endian WMV2 frame -> XMV's little-endian 32-bit words, padded."""
    frame = bytes(frame) + b"\0" * (-len(frame) % 4)
    if not frame:
        frame = b"\0" * 4
    return b"".join(frame[i:i + 4][::-1] for i in range(0, len(frame), 4))


def mux_xmv(width, height, frames, header_word, fps=30, audio=(), frames_per_packet=4,
            version=4):
    """XMV bytes.

    frames: big-endian WMV2 frames, one every 1/fps second.
    header_word: the WMV2 sequence header in XMV's packing (sent in the first
    packet). audio: [(compression, channels, rate, bits, flags, data bytes)];
    each track's data is cut into whole blocks spread over the packets."""
    ntracks = len(audio)
    nframes = len(frames)
    npackets = max(1, (nframes + frames_per_packet - 1) // frames_per_packet)
    duration_ms = int(round(nframes * 1000.0 / fps))
    hdr = bytearray(struct.pack("<III4sIIIIHH", 0, 0, 0, b"xobX", version, width, height,
                                duration_ms, ntracks, 0))
    for comp, ch, rate, bits, flags, _ in audio:
        hdr += struct.pack("<HHIHH", comp, ch, rate, bits, flags)

    # Audio blocks per packet, spread evenly.
    blocks = []
    for comp, ch, rate, bits, flags, data in audio:
        align = ADPCM_BLOCK * ch if comp == 0x0069 else 2 * ch
        nb = len(data) // align
        cuts = [(nb * k) // npackets for k in range(npackets + 1)]
        blocks.append([data[cuts[k] * align:cuts[k + 1] * align] for k in range(npackets)])

    packets = []
    t_prev = 0
    for k in range(npackets):
        fr = frames[k * frames_per_packet:(k + 1) * frames_per_packet]
        video = bytearray()
        if k == 0:
            video += struct.pack("<I", header_word)
        for i, f in enumerate(fr):
            n = k * frames_per_packet + i
            t = int(round(n * 1000.0 / fps))
            delta = t - t_prev
            t_prev = t
            body = le_words(f)
            video += struct.pack("<I", ((len(body) - 4) // 4) | (delta << 17)) + body
        vword = (len(video) + 4 * ntracks) | (len(fr) << 23) | ((1 << 31) if k == 0 else 0)
        pkt = bytearray(struct.pack("<III", 0, vword, 0))
        for t in range(ntracks):
            pkt += struct.pack("<I", len(blocks[t][k]))
        pkt += video
        for t in range(ntracks):
            pkt += blocks[t][k]
        packets.append(pkt)
    # Each packet starts with the size of the one after it (0: the end).
    for k in range(npackets):
        nxt = len(packets[k + 1]) if k + 1 < npackets else 0
        struct.pack_into("<I", packets[k], 0, nxt)
    max_pkt = max(len(p) for p in packets)
    struct.pack_into("<III", hdr, 0, len(packets[0]), len(hdr) + len(packets[0]), max_pkt)
    return bytes(hdr) + b"".join(packets)


def tone(rate, channels, frames):
    """A synthetic two-tone signal, interleaved int16."""
    out = []
    for n in range(frames):
        for ch in range(channels):
            f = 440.0 * (ch + 1)
            v = 0.45 * math.sin(2 * math.pi * f * n / rate) + 0.2 * math.sin(2 * math.pi * 97 * n / rate)
            out.append(int(round(v * 32767)))
    return out


def ffmpeg_reference(xmv_path, width, height, ffmpeg="ffmpeg"):
    """CRC-32 of each frame FFmpeg decodes from the XMV (raw yuv420p)."""
    # bitexact: FFmpeg's SIMD no-rounding averages are approximate otherwise.
    raw = subprocess.run([ffmpeg, "-v", "error", "-flags", "+bitexact", "-i", xmv_path, "-map", "0:v", "-f", "rawvideo",
                          "-pix_fmt", "yuv420p", "-fps_mode", "passthrough", "-"],
                         check=True, capture_output=True).stdout
    fsize = width * height + 2 * ((width + 1) // 2) * ((height + 1) // 2)
    return [zlib.crc32(raw[i:i + fsize]) for i in range(0, len(raw) - fsize + 1, fsize)]


def make_fixture(out, width, height, nframes, gop, q, channels, rate, loop_filter, slices,
                 ffmpeg="ffmpeg", pattern="testsrc2"):
    with tempfile.TemporaryDirectory() as tmp:
        avi = os.path.join(tmp, "v.avi")
        src = f"{pattern}=size={width}x{height}:rate=30"
        subprocess.run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", src, "-frames:v", str(nframes),
                        "-c:v", "wmv2", "-g", str(gop), "-q:v", str(q), "-f", "avi", avi],
                       check=True)
        ext, frames = wmv2_from_avi(avi)
    word = xmv_header_word(ext)
    # Loop filter and slice count change how the picture is reconstructed,
    # not the syntax: set here, FFmpeg's decode of the file is the reference.
    if loop_filter:
        word |= 2
    if slices:
        word = (word & ~(7 << 6)) | ((slices & 7) << 6)
    audio = []
    pcm = []
    if channels:
        nsamp = int(nframes * rate / 30)
        data, pcm = encode_xbox_adpcm(tone(rate, channels, nsamp), channels)
        audio.append((0x0069, channels, rate, 4, 0, data))
    blob = mux_xmv(width, height, frames, word, 30, audio)
    with open(out, "wb") as f:
        f.write(blob)
    crcs = ffmpeg_reference(out, width, height, ffmpeg)
    pcm_bytes = struct.pack("<%dh" % len(pcm), *pcm) if pcm else b""
    return {"frames": len(frames), "crcs": crcs, "pcm_frames": len(pcm) // max(channels, 1),
            "pcm_crc": zlib.crc32(pcm_bytes), "header_word": word}


# The fixtures tests/xmv_decode decodes: (name, size, frames, gop, q, channels,
# rate, loop filter, slices, pattern). Together they cover I and P frames,
# intra MBs in P frames, the three cbp tables (q <= 10, <= 20, > 20), both
# escape-3 length codings (q < 8 and q >= 8), the loop filter, several slices,
# a picture that is not a whole number of macroblocks, mono and stereo audio.
TEST_SET = [
    ("basic", (96, 64), 12, 6, 4, 2, 22050, False, 0, "testsrc2"),
    ("loopfilter", (96, 64), 10, 5, 9, 1, 11025, True, 0, "testsrc2"),
    ("slices", (112, 80), 16, 16, 14, 0, 0, False, 3, "testsrc2"),
    ("odd_size", (104, 72), 10, 4, 24, 1, 22050, False, 0, "mandelbrot"),
    ("fine_q", (64, 48), 6, 3, 2, 0, 0, False, 0, "testsrc2"),
    ("coarse_q", (128, 96), 14, 7, 31, 0, 0, True, 2, "testsrc"),
]


# Synthetic streams (wmv2_synth.py) for what FFmpeg's encoder never writes:
# (name, size, frames, gop, seed, Synth options).
SYNTH_SET = [
    ("synth_mspel_abt", (80, 64), 12, 6, 1, dict(mspel_bit=1, abt_flag=1, tlmv=0, per_mb_rl_bit=1, slices=1)),
    ("synth_tlmv_slices", (96, 80), 12, 12, 2, dict(mspel_bit=0, abt_flag=1, tlmv=1, per_mb_rl_bit=0, slices=2,
                                                     loop_filter=1)),
    ("synth_all", (112, 64), 16, 8, 3, dict(mspel_bit=1, abt_flag=1, tlmv=1, per_mb_rl_bit=1, slices=4,
                                             loop_filter=1)),
]


def make_synth_fixture(out, width, height, nframes, gop, seed, ffmpeg="ffmpeg", **kw):
    from .wmv2_synth import synth_movie
    word, frames = synth_movie(width, height, nframes, gop, seed, **kw)
    with open(out, "wb") as f:
        f.write(mux_xmv(width, height, frames, word, 30, ()))
    return {"frames": len(frames), "crcs": ffmpeg_reference(out, width, height, ffmpeg),
            "pcm_frames": 0, "pcm_crc": 0}


def make_test_set(directory, ffmpeg="ffmpeg"):
    """Writes the TEST_SET and SYNTH_SET fixtures and expected.h (the
    reference CRCs)."""
    os.makedirs(directory, exist_ok=True)
    lines = ["/* Generated by `python -m tools.xmv.xmv_mux make-test-set`: the CRC-32 of",
             " * each frame FFmpeg decodes from each fixture (Y, U, V planes, cropped), and",
             " * of the PCM the ADPCM encoder reconstructed. Do not edit. */",
             "typedef struct {", "    const char *name; int width, height, channels, rate;",
             "    int nframes; unsigned crc[32]; unsigned pcm_frames, pcm_crc;", "} XmvFixture;",
             "static const XmvFixture xmv_fixtures[] = {"]
    for name, (w, h), n, gop, q, ch, rate, lf, sl, pat in TEST_SET:
        r = make_fixture(os.path.join(directory, name + ".xmv"), w, h, n, gop, q, ch, rate, lf, sl,
                         ffmpeg, pat)
        assert len(r["crcs"]) == n <= 32, (name, len(r["crcs"]))
        crcs = ", ".join("0x%08Xu" % c for c in r["crcs"])
        lines.append('    { "%s", %d, %d, %d, %d, %d, { %s }, %du, 0x%08Xu },'
                     % (name, w, h, ch, rate, n, crcs, r["pcm_frames"], r["pcm_crc"]))
    for name, (w, h), n, gop, seed, kw in SYNTH_SET:
        r = make_synth_fixture(os.path.join(directory, name + ".xmv"), w, h, n, gop, seed, ffmpeg, **kw)
        assert len(r["crcs"]) == n <= 32, (name, len(r["crcs"]))
        crcs = ", ".join("0x%08Xu" % c for c in r["crcs"])
        lines.append('    { "%s", %d, %d, 0, 0, %d, { %s }, 0u, 0u },' % (name, w, h, n, crcs))
    lines.append("};")
    with open(os.path.join(directory, "expected.h"), "w") as f:
        f.write("\n".join(lines) + "\n")


# The fixtures of tests/xmv_hle's fallback (XBA-199): movies using what the runtime's decoder does not decode
# exactly, which it hands to the title's own decoder, and one that only allows X8 without using it. Made from
# wmv2_synth.py streams; no FFmpeg (nothing here is a decoding reference). (name, frames, gop, seed, X8 frames,
# audio kind, damaged frames, X8 allowed by the sequence header).
FALLBACK_SET = [
    ("x8", 12, 4, 11, (4,), "adpcm", (), 1),
    ("x8_unused", 12, 4, 12, (), "adpcm", (), 1),
    ("wma", 12, 6, 13, (), "wma", (), 0),
    ("surround", 12, 6, 14, (), "5.1", (), 0),
    ("damaged", 12, 6, 15, (), "adpcm", (2, 3, 4, 5), 0),
]
FALLBACK_SIZE = (96, 64)


def make_fallback_fixture(out, nframes, gop, seed, x8_at, audio_kind, damaged, x8_allowed):
    from .wmv2_synth import synth_movie
    import random
    w, h = FALLBACK_SIZE
    word, frames = synth_movie(w, h, nframes, gop, seed, x8_at=x8_at, mspel_bit=1, abt_flag=1, tlmv=0,
                               per_mb_rl_bit=1, slices=1, x8_allowed=x8_allowed)
    for k in damaged:
        frames[k] = b"\0\0\0\0"            # an I frame header with quantiser 0: never valid
    rate = 22050
    nsamp = int(nframes * rate / 30)
    audio = []
    if audio_kind == "adpcm":
        audio.append((0x0069, 2, rate, 4, 0, encode_xbox_adpcm(tone(rate, 2, nsamp), 2)[0]))
    elif audio_kind == "5.1":
        for flags in (1, 2, 4):              # front, centre + LFE, rear: three stereo tracks
            audio.append((0x0069, 2, rate, 4, flags, encode_xbox_adpcm(tone(rate, 2, nsamp), 2)[0]))
    elif audio_kind == "wma":
        r = random.Random(seed)
        audio.append((0x0161, 2, 44100, 16, 0, bytes(r.randrange(256) for _ in range(4 * 1024))))
    blob = mux_xmv(w, h, frames, word, 30, audio)
    with open(out, "wb") as f:
        f.write(blob)
    return blob


def make_fallback_set(directory):
    os.makedirs(directory, exist_ok=True)
    for name, *spec in FALLBACK_SET:
        make_fallback_fixture(os.path.join(directory, name + ".xmv"), *spec)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.xmv.xmv_mux", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    mf = sub.add_parser("make-fixture", help="encode, mux and reference-decode a test XMV")
    mf.add_argument("out")
    mf.add_argument("--size", default="96x64")
    mf.add_argument("--frames", type=int, default=12)
    mf.add_argument("--gop", type=int, default=6)
    mf.add_argument("--q", type=int, default=4)
    mf.add_argument("--channels", type=int, default=2)
    mf.add_argument("--rate", type=int, default=22050)
    mf.add_argument("--loop-filter", action="store_true")
    mf.add_argument("--slices", type=int, default=0)
    mf.add_argument("--pattern", default="testsrc2")
    mf.add_argument("--ffmpeg", default="ffmpeg")
    ts = sub.add_parser("make-test-set", help="write tests/xmv_decode's fixtures and expected.h")
    ts.add_argument("dir")
    ts.add_argument("--ffmpeg", default="ffmpeg")
    fs = sub.add_parser("make-fallback-set", help="write tests/xmv_hle's fallback fixtures (no FFmpeg)")
    fs.add_argument("dir")
    a = ap.parse_args(argv)
    if a.cmd == "make-fallback-set":
        make_fallback_set(a.dir)
        return None
    if a.cmd == "make-test-set":
        make_test_set(a.dir, a.ffmpeg)
        return None
    w, h = (int(v) for v in a.size.split("x"))
    r = make_fixture(a.out, w, h, a.frames, a.gop, a.q, a.channels, a.rate, a.loop_filter,
                     a.slices, a.ffmpeg, a.pattern)
    print(f"{a.out}: {r['frames']} frames, {len(r['crcs'])} decoded by FFmpeg, "
          f"{r['pcm_frames']} audio frames")
    print("crcs:", " ".join("0x%08X" % c for c in r["crcs"]))
    print("pcm_crc: 0x%08X" % r["pcm_crc"])
    return r


if __name__ == "__main__":
    main()
