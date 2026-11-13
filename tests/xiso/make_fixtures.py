#!/usr/bin/env python3
"""Synthetic disc images for tests/xiso (XBA-40). Run by CMake at configure time:

    python3 make_fixtures.py OUT_DIR

Writes two source trees (tree/, small/), their manifests (tree.manifest, small.manifest) and the images built from
them with tools/xiso/mkxiso.py. Nothing comes from a real disc: default.xbe is tools/conformance/mkxbe.py's
synthetic XBE (title "SYNTH"), every other file is a pseudo-random stream the C test regenerates from the path:

    seed = FNV-1a 32 of the relative path ('/' separators), or 1 when that is 0
    words: xorshift32 (x ^= x << 13; x ^= x >> 17; x ^= x << 5), one per 4 bytes, little-endian

Manifest lines: "d PATH" (a directory) or "f SIZE PATH" (a file; "x SIZE PATH" for default.xbe, whose content is
not the stream but default.xbe.ref). The device bundle has no python: CMake passes every image and manifest to the test on its command
line, so tools/android/bundle_tests.py copies them.
"""
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "xiso"))
sys.path.insert(0, os.path.join(ROOT, "tools", "conformance"))
import mkxiso  # noqa: E402
import mkxbe   # noqa: E402
import mkcso   # noqa: E402

BIG = 4 * 1024 * 1024 + 300 * 1024 + 7      # > the 2 MB copy buffer, crosses the 4 MB progress step


def fnv1a(s):
    h = 0x811C9DC5
    for b in s.encode("latin-1"):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h or 1


def stream(path, size):
    x = fnv1a(path)
    words = []
    for _ in range((size + 3) // 4):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        words.append(x)
    return struct.pack("<%dI" % len(words), *words)[:size]


def make_tree(top, spec):
    """spec: list of (relpath, size) for files, (relpath, None) for directories. Returns the manifest text."""
    shutil.rmtree(top, ignore_errors=True)
    os.makedirs(top)
    lines = []
    for rel, size in spec:
        p = os.path.join(top, *rel.split("/"))
        if size is None:
            os.makedirs(p, exist_ok=True)
            lines.append("d %s" % rel)
            continue
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if rel == "default.xbe":
            data = mkxbe.build()
            lines.append("x %d %s" % (len(data), rel))
        else:
            data = stream(rel, size)
            lines.append("f %d %s" % (size, rel))
        with open(p, "wb") as f:
            f.write(data)
    return "\n".join(lines) + "\n"


def main():
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)

    tree = [("default.xbe", 0), ("empty.bin", 0), ("big.bin", BIG), ("ReadMe.TXT", 2048),
            ("media", None), ("media/intro.bik", 5000), ("media/sub", None), ("media/sub/deeper", None),
            ("media/sub/deeper/leaf.txt", 3), ("emptydir", None), ("many", None)]
    # Enough entries for a tree several levels deep whose table spans many sectors.
    tree += [("many/file_%03d_%s.dat" % (i, "x" * (i % 40)), (i * 37) % 3000) for i in range(300)]
    small = [("default.xbe", 0), ("a.txt", 100), ("b.txt", 2049), ("c.txt", 1), ("dotdot", 10), ("slash", 10),
             ("sub", None), ("sub/inner.bin", 4096), ("sub/d2", None), ("sub/d2/z.txt", 77), ("void", None)]

    with open(os.path.join(out, "tree.manifest"), "w") as f:
        f.write(make_tree(os.path.join(out, "tree"), tree))
    with open(os.path.join(out, "small.manifest"), "w") as f:
        f.write(make_tree(os.path.join(out, "small"), small))

    t, s = os.path.join(out, "tree"), os.path.join(out, "small")
    img = lambda n: os.path.join(out, n)                                   # noqa: E731
    mkxiso.build(t, img("tree.iso"))
    mkxiso.build(s, img("small.iso"))
    mkxiso.build(s, img("small_base30600.iso"), base=0x30600)
    mkxiso.build(s, img("bad_magic.iso"), corrupt="bad-magic")
    mkxiso.build(s, img("sector_range.iso"), corrupt="sector-range")
    mkxiso.build(s, img("self_loop.iso"), corrupt="self-loop")
    mkxiso.build(s, img("dir_cycle.iso"), corrupt="dir-cycle")
    mkxiso.build(s, img("dotdot.iso"), renames={"dotdot": ".."})
    mkxiso.build(s, img("dot.iso"), renames={"dotdot": "."})
    mkxiso.build(s, img("slash.iso"), renames={"slash": "a/b"})
    mkxiso.build(s, img("backslash.iso"), renames={"slash": "a\\b"})
    mkxiso.build(s, img("duplicate.iso"), renames={"dotdot": "a.txt"})
    mkxiso.build(s, img("truncated.iso"), truncate=-3000)                  # inside the last file's data
    mkxiso.build(s, img("truncated_dir.iso"), truncate=33 * 2048 + 100)    # inside the root table
    with open(img("default.xbe.ref"), "wb") as f:                          # the test compares default.xbe to it
        f.write(mkxbe.build())

    # XBA-126: split and compressed images of the same trees (tools/xiso/mkcso.py).
    with open(img("small.iso"), "rb") as f:
        small_img = f.read()
    with open(img("tree.iso"), "rb") as f:
        tree_img = f.read()
    for fmt, name in (("cso1", "small_v1.cso"), ("cso2", "small_v2.cso"), ("cso2-framed", "small_v2f.cso"),
                      ("cso2-maxcso", "small_v2m.cso"), ("zso", "small.zso"), ("cci", "small.cci")):
        with open(img(name), "wb") as f:
            f.write(mkcso.compress(small_img, fmt))
    with open(img("small_b16k.cso"), "wb") as f:                           # a bigger block, deflate
        f.write(mkcso.compress(small_img, "cso1", block=16384, align=0))
    tree_cso = mkcso.compress(tree_img, "cso1")                              # stored blocks, many batches
    with open(img("tree_v1.cso"), "wb") as f:
        f.write(tree_cso)
    mkcso.write_split(tree_img, img("tree_split.iso"), 2 * 1024 * 1024 + 4096, "dot")   # .1/.2/.3/.4.iso
    mkcso.write_split(small_img, img("small_x.iso"), 20000, "ext3")         # .iso.001 ...
    mkcso.write_split(small_img, img("small_p.iso"), 30000, "part")         # .part1.iso ...
    mkcso.write_split(mkcso.compress(small_img, "cso2"), img("small_cs.cso"), 7000, "dot")   # a split .cso
    bad = bytearray(mkcso.compress(small_img, "zso"))                      # a block that does not decode
    import struct as _st
    first = (_st.unpack_from("<I", bad, 24)[0] & 0x7FFFFFFF) << bad[21]
    for i in range(first, first + 64):
        bad[i] = 0xFF                                                       # literal lengths that never end
    with open(img("bad_block.zso"), "wb") as f:
        f.write(bad)
    short = mkcso.compress(small_img, "cso2")                              # the index points past the end
    with open(img("short.cso"), "wb") as f:
        f.write(short[:len(short) - 3000])
    shutil.rmtree(t)                                                       # the test regenerates the contents
    shutil.rmtree(s)


if __name__ == "__main__":
    main()
