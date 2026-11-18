"""The uncompressed data of the LZX test vectors (XBA-270): made here, deterministic, nothing borrowed.

data(name) -> bytes. The vectors in this directory are these bytes compressed by other people's encoders (gen.sh).
Run as a script: python3 vector_data.py OUTDIR writes every NAME.raw."""

import os
import random
import struct
import sys

NAMES = ("small", "mid", "mixed", "x86", "long")
WORDS = ("the of and to in is that for it as with was on be by this are or from at which an have not but they "
         "recompiled static guest host thread memory kernel import export vector register branch block window "
         "frame offset literal match aligned verbatim xenon xbox console title image section header").split()


def _text(rng, n):
    out = []
    size = 0
    while size < n:
        w = rng.choice(WORDS)
        if rng.random() < 0.08:
            w = w.capitalize() + "."
        out.append(w)
        size += len(w) + 1
    return " ".join(out).encode()[:n]


def _code(rng, n):
    """Big-endian PowerPC-shaped words: a few opcodes with varying registers and displacements."""
    pats = [0x38600000, 0x7C0802A6, 0x9421FF90, 0x4E800020, 0x81610000, 0x48000001, 0x2C030000, 0x41820000,
            0xF8210000, 0xE8610000, 0x7C631A14, 0x60000000]
    out = bytearray()
    while len(out) < n:
        w = rng.choice(pats)
        if w not in (0x4E800020, 0x60000000, 0x7C0802A6):
            w |= rng.randrange(0, 1 << 10) << rng.choice((0, 2, 11, 16))
        out += struct.pack(">I", w & 0xFFFFFFFF)
    return bytes(out[:n])


def _x86(rng, n):
    """E8 (call) bytes with plausible displacements, for E8 translation."""
    out = bytearray()
    while len(out) < n:
        k = rng.random()
        if k < 0.15:
            out += b"\xE8" + struct.pack("<i", rng.randrange(-4000, 4000))
        elif k < 0.3:
            out += b"\x55\x8B\xEC\x83\xEC" + bytes([rng.randrange(256)])
        else:
            out += bytes([rng.choice((0x89, 0x8B, 0x50, 0x51, 0xC3, 0x90, 0x33, 0xC0))])
    return bytes(out[:n])


def _noise(rng, n):
    return bytes(rng.randrange(256) for _ in range(n))


def data(name):
    rng = random.Random("xba-270-lzx-" + name)
    if name == "small":       # one frame: text and code
        return _text(rng, 9000) + _code(rng, 6000)
    if name == "mid":         # two frames in one CHM reset interval: also checked as a CAB by cabextract
        return _text(rng, 20000) + _code(rng, 15000) + _x86(rng, 6000) + _noise(rng, 1500) + _text(rng, 7000)
    if name == "mixed":       # several frames, every kind of content, noise in the middle
        parts = [_text(rng, 40000), _code(rng, 30000), _noise(rng, 9000), _x86(rng, 20000), b"\0" * 7001,
                 _text(rng, 25000), _noise(rng, 3001), _code(rng, 41000)]
        return b"".join(parts)
    if name == "x86":         # E8 translation
        return _x86(rng, 30000)
    if name == "long":        # long matches and far offsets in a big window
        base = _text(rng, 70000) + _code(rng, 50000)
        return base + _noise(rng, 2000) + base[1000:61000] + base[:30000]
    raise KeyError(name)


if __name__ == "__main__":
    os.makedirs(sys.argv[1], exist_ok=True)
    for n in NAMES:
        with open(os.path.join(sys.argv[1], n + ".raw"), "wb") as f:
            f.write(data(n))
