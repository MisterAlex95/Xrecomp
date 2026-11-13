#!/usr/bin/env python3
"""Fixtures for tests/kernel_disc (XBA-126), run by CMake at configure time:

    python3 make_fixtures.py OUT_DIR

OUT_DIR/game/ is a synthetic game folder (tools/conformance/mkxbe.py's XBE, every other file a pseudo-random
stream, as tests/xiso/make_fixtures.py makes them); the same tree as disc images: game.iso (bare), game_redump.iso
(the game partition at 0x18300000, sparse), game_v2.cso (the Xbox tools' LZ4 CSO), game_v1.cso (deflate),
game_split.1.iso .. (1 MB parts) and game_cs.1.cso .. (a split CSO). Nothing from a real disc.
"""
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "xiso"))
sys.path.insert(0, os.path.join(ROOT, "tools", "conformance"))
import mkcso   # noqa: E402
import mkxbe   # noqa: E402
import mkxiso  # noqa: E402


def stream(path, size):
    x = 0x811C9DC5
    for b in path.encode("latin-1"):
        x = ((x ^ b) * 0x01000193) & 0xFFFFFFFF
    x = x or 1
    words = []
    for _ in range((size + 3) // 4):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        words.append(x)
    return struct.pack("<%dI" % len(words), *words)[:size]


TREE = [("default.xbe", None), ("media/Intro.bik", 300 * 1024 + 5), ("media/outro.bik", 4096),
        ("media/sub/leaf.txt", 3), ("media/empty.bin", 0), ("Data/big.pak", 3 * 1024 * 1024 + 777),
        ("Data/levels/l1.lvl", 9000), ("Data/levels/l2.lvl", 9001), ("game2.xbe", None), ("emptydir/", None)]


def main():
    out = os.path.abspath(sys.argv[1])
    game = os.path.join(out, "game")
    shutil.rmtree(game, ignore_errors=True)
    os.makedirs(game)
    for rel, size in TREE:
        p = os.path.join(game, *rel.strip("/").split("/"))
        if rel.endswith("/"):
            os.makedirs(p, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(p), exist_ok=True)
        data = mkxbe.build() if rel.endswith(".xbe") else stream(rel, size)
        with open(p, "wb") as f:
            f.write(data)
    img = lambda n: os.path.join(out, n)                                   # noqa: E731
    mkxiso.build(game, img("game.iso"))
    mkxiso.build(game, img("game_redump.iso"), base=0x18300000)
    with open(img("game.iso"), "rb") as f:
        data = f.read()
    cso2 = mkcso.compress(data, "cso2")
    with open(img("game_v2.cso"), "wb") as f:
        f.write(cso2)
    with open(img("game_v1.cso"), "wb") as f:
        f.write(mkcso.compress(data, "cso1"))
    mkcso.write_split(data, img("game_split.iso"), 1024 * 1024, "dot")
    mkcso.write_split(cso2, img("game_cs.cso"), 1500 * 1024, "dot")


if __name__ == "__main__":
    main()
