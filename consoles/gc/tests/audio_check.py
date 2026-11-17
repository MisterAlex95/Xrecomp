#!/usr/bin/env python3
"""Compares a GameCube sound test's WAV with its reference (XBA-263).

    audio_check.py OUT.wav SPEC [--ref REF.wav]

SPEC names a scene list below (asnd / aesnd: the libasnd / libaesnd homebrew; ax: gc_audio_test's AX voices; dtk: its
disc stream; zelda: gc_zelda_test's Zelda ucode voices).
The reference is synthesised from the list (ideal sines at the expected levels, written to --ref for listening) and
both files are measured the same way in each scene (its first and last 0.1 s left out): per channel, the level of
each expected tone (Goertzel), the RMS, and what is left once the tones are taken out (noise, distortion, a wrong
pitch). The test passes when every measure of OUT is within tolerance of the reference's. Time 0 is where OUT gets
loud first (the onset) unless the spec gives the scene times from the start of the file. Pure Python, no numpy."""
import math
import struct
import sys
import wave

RATE = 48000

# scene: (start s, length s, left tones, right tones); a tone is (Hz, amplitude)
SPECS = {
    # homebrew/asnd: amplitude 16000 at volume 255 -> 16000 * 255 / 256; 128 -> half
    "asnd": {
        "onset": True,
        "scenes": [
            (0.0, 1.0, [(1000, 15937.5)], []),
            (1.0, 1.0, [], [(500, 15937.5)]),
            (2.0, 1.0, [(1000, 15937.5)], [(1500, 15937.5)]),
            (3.0, 1.0, [(1000, 8000), (500, 8000)], [(1000, 8000), (500, 8000)]),
            (4.0, 1.0, [], []),
        ],
    },
    # gc_audio_test's AX run (times from the file's start): an ADPCM 1000 Hz sine (looped, sample-rate converted
    # from 16 kHz, amplitude 16000) on the left at volume 0.5, a PCM16 1500 Hz sine on the right ramping up to
    # 24000 / 32768 by 0.375 s (measured after it); from 1 s a PCM8 500 Hz sine (62 << 8) on both at 0.375; from
    # 1.5 s the voices stopped
    "ax": {
        "onset": False,
        "scenes": [
            (0.6, 0.4, [(1000, 8000)], [(1500, 16000 * 24000 / 32768)]),
            (1.0, 0.5, [(500, 62 * 256 * 0.375)], [(500, 62 * 256 * 0.375)]),
            (1.5, 0.5, [], []),
        ],
    },
    # gc_zelda_test's Zelda ucode run (XBA-266; times from the file's start): PCM16 1000 Hz left and AFC 500 Hz
    # right (16 -> 32 kHz), amplitude 16000 at a destination volume of 0x7FFF (the ucode mixes at half scale); then a
    # PCM8 750 Hz (100 << 8, 8 -> 32 kHz) on both; then a positional voice front left (1000 Hz, quadrant volume
    # 0x3FFF x 0x7FFF >> 16 on The Wind Waker's version: 1 / 8) and a main-memory PCM16 1250 Hz right; then silence
    "zelda": {
        "onset": False,
        "scenes": [
            (0.0, 0.5, [(1000, 8000)], [(500, 8000)]),
            (0.5, 0.5, [(750, 12800)], [(750, 12800)]),
            (1.0, 0.5, [(1000, 16000 * 8191 / 65536)], [(1250, 8000)]),
            (1.5, 0.5, [], []),
        ],
    },
    # gc_audio_test's disc stream: 750 Hz left, 1250 Hz right at amplitude 12000, AI volume 255 / 128
    "dtk": {
        "onset": False,
        "scenes": [
            (0.2, 0.6, [(750, 12000)], [(1250, 12000 * 128 / 255)]),
        ],
    },
}


# homebrew/aesnd plays asnd's five scenes through libaesnd (XBA-266)
SPECS["aesnd"] = SPECS["asnd"]


def read_wav(path):
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 2 or w.getsampwidth() != 2 or w.getframerate() != RATE:
            raise SystemExit("%s: not 48 kHz 16-bit stereo" % path)
        n = w.getnframes()
        d = struct.unpack("<%dh" % (2 * n), w.readframes(n))
    return list(d[0::2]), list(d[1::2])


def synth(spec, n, t0):
    left, right = [0.0] * n, [0.0] * n
    for start, length, lt, rt in spec["scenes"]:
        a, b = int((t0 + start) * RATE), min(n, int((t0 + start + length) * RATE))
        for tones, out in ((lt, left), (rt, right)):
            for f, amp in tones:
                w = 2 * math.pi * f / RATE
                for i in range(max(a, 0), b):
                    out[i] += amp * math.sin(w * (i - a))
    clip = lambda v: max(-32768, min(32767, int(round(v))))
    return [clip(v) for v in left], [clip(v) for v in right]


def write_wav(path, left, right):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(struct.pack("<%dh" % (2 * len(left)), *[v for p in zip(left, right) for v in p]))


def tone_amp(x, f):
    """Amplitude of the f Hz component of x (Goertzel, exact for a whole number of periods)."""
    k = 2 * math.cos(2 * math.pi * f / RATE)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + k * s1 - s2
        s2, s1 = s1, s0
    p = s1 * s1 + s2 * s2 - k * s1 * s2
    return 2 * math.sqrt(max(p, 0.0)) / len(x)


def measure(x, tones):
    rms = math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0
    amps = [tone_amp(x, f) for f, _ in tones]
    rest2 = rms * rms - sum(a * a / 2 for a in amps)
    return rms, amps, math.sqrt(max(rest2, 0.0))


def onset(left, right, level=1000):
    for i, (l, r) in enumerate(zip(left, right)):
        if abs(l) > level or abs(r) > level:
            return i / RATE
    return None


def main():
    args = sys.argv[1:]
    ref_out = None
    if "--ref" in args:
        i = args.index("--ref")
        ref_out = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2 or args[1] not in SPECS:
        raise SystemExit("usage: audio_check.py OUT.wav {%s} [--ref REF.wav]" % "|".join(SPECS))
    path, name = args
    spec = SPECS[name]
    left, right = read_wav(path)
    t0 = 0.0
    if spec["onset"]:
        t0 = onset(left, right)
        if t0 is None:
            print("FAIL %s: silent" % name)
            return 1
        print("onset at %.3f s" % t0)
    rl, rr = synth(spec, len(left), t0)
    if ref_out:
        write_wav(ref_out, rl, rr)
    bad = 0
    for k, (start, length, lt, rt) in enumerate(spec["scenes"]):
        a, b = int((t0 + start + 0.1) * RATE), int((t0 + start + length - 0.1) * RATE)
        if b > len(left):
            print("FAIL scene %d: the WAV ends at %.2f s" % (k + 1, len(left) / RATE))
            bad += 1
            continue
        for ch, (got, ref, tones) in enumerate(((left, rl, lt), (right, rr, rt))):
            g, r = measure(got[a:b], tones), measure(ref[a:b], tones)
            side = "LR"[ch]
            ok = abs(g[0] - r[0]) <= 0.05 * r[0] + 150  # RMS within 5 %
            for (f, _), ga, ra in zip(tones, g[1], r[1]):
                ok &= abs(ga - ra) <= 0.05 * ra + 150  # each tone's level within 5 %
            ok &= g[2] <= 0.08 * r[0] + 150  # what is not the tones: under 8 % of the level
            tones_txt = " ".join("%d Hz %.0f/%.0f" % (f, ga, ra) for (f, _), ga, ra in zip(tones, g[1], r[1]))
            print("%s scene %d %s: rms %.0f/%.0f, rest %.0f%s%s" % ("ok  " if ok else "FAIL", k + 1, side, g[0], r[0],
                                                                    g[2], ", " if tones else "", tones_txt))
            bad += not ok
    print("%s: %s" % (name, "ok" if not bad else "%d measure(s) off" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
