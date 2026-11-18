"""make_draw.py DIR (XBA-270): the shaders of xenos_draw_test, assembled with x360_tools.xenos_asm (no title data):
DIR/vs.bin and DIR/ps.bin (big-endian microcode words) and their GLSL ES in DIR/cache/vs_<crc>.vert /
ps_<crc>.frag, translated by x360_tools.xenos_glsl, the way bringup fills a title's shader cache.

Vertex shader: two vertex fetches from fetch constant 0 (position at offset 0, colour at offset 4, stride 8 dwords,
32_32_32_32_FLOAT), position exported to oPos, colour to interpolator 0. Pixel shader: oC0 = interpolator 0 * c0."""

import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

from x360_tools.xenos_asm import alu_words, export, fetch_words, program  # noqa: E402
from x360_tools.xenos_glsl import translate  # noqa: E402

XYZW = 0 | 1 << 3 | 2 << 6 | 3 << 9


def shaders():
    vs = program([(2, [("fetch", fetch_words(0, 0, 1, XYZW, 0, sel=0, format=38, stride=8, offset=0)),
                       ("fetch", fetch_words(0, 0, 2, XYZW, 0, sel=0, format=38, stride=8, offset=4)),
                       ("alu", alu_words(**export(62, 1))), ("alu", alu_words(**export(0, 2)))], {})])
    ps = program([(2, [("alu", alu_words(vop=1, vdest=0, vmask=0xF, export=1,
                                         srcs=((0, 1, 0, 0), (0, 0, 0, 0), (0, 0, 0, 0))))], {})])
    return vs, ps


def main(out):
    vs, ps = shaders()
    os.makedirs(os.path.join(out, "cache"), exist_ok=True)
    for name, words, stage, ext in (("vs", vs, "vertex", "vert"), ("ps", ps, "pixel", "frag")):
        raw = struct.pack(">%dI" % len(words), *words)
        open(os.path.join(out, name + ".bin"), "wb").write(raw)
        src, _ = translate(words, stage, interpolators=1)
        with open(os.path.join(out, "cache", "%s_%08X.%s" % (name, zlib.crc32(raw), ext)), "w") as f:
            f.write(src)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
