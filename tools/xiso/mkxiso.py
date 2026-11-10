#!/usr/bin/env python3
"""Build a synthetic Xbox disc image (XDVDFS) from a directory tree. Standard library only.

    python3 tools/xiso/mkxiso.py SRC_DIR OUT.iso [--base OFFSET] [--rename OLD=NEW]...
                                 [--corrupt KIND] [--truncate BYTES]

For tests of the XISO readers (src/xiso, tools/xiso/xdvdfs.py): it never reads a real disc and the images it writes
hold only what SRC_DIR holds. Layout: descriptor at sector 32 of the game partition, directory tables from sector 33
(root first), then the file data, each file on a sector boundary. Each directory's entries form a balanced binary
tree sorted by upper-cased name (what the Xbox kernel's lookup expects), laid out root first, never crossing a
sector, padding 0xFF. An empty directory is one sector of 0xFF; an empty file has sector 0, size 0.

    --base OFFSET     game partition offset: 0 (bare image, extract-xiso style, the default) or a redump-style base
                      such as 0x18300000 (video partition first; written sparse, the space before it stays a hole)
    --rename OLD=NEW  store the entry named OLD (any directory) as NEW, which may be a name the file system cannot
                      hold: "..", "a/b", or a duplicate of a sibling (corrupt images)
    --corrupt KIND    bad-magic      the descriptor magic is wrong
                      sector-range   the first file of the root points past the end of the image
                      self-loop      a tree node's left link points at itself
                      dir-cycle      the first subdirectory of the root points back at the root table
    --truncate BYTES  cut the image to BYTES (relative to the end when negative)
"""
import argparse
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
ATTR_DIR = 0x10
ATTR_FILE = 0x80   # "normal", what mastering tools write for files


class Node:
    def __init__(self, name, path, is_dir):
        self.name = name          # bytes, as stored
        self.path = path
        self.is_dir = is_dir
        self.children = []        # Node, directories only
        self.size = 0
        self.sector = 0
        self.table = b""          # directory table, directories only


def scan(path, name, renames):
    node = Node(renames.get(name, name).encode("latin-1"), path, os.path.isdir(path))
    if node.is_dir:
        for n in sorted(os.listdir(path)):
            node.children.append(scan(os.path.join(path, n), n, renames))
        node.children.sort(key=lambda c: c.name.upper())
    else:
        node.size = os.path.getsize(path)
    return node


def bst_preorder(items):
    """Balanced BST over the sorted items, as a pre-order list of (item, left_index, right_index)."""
    out = []

    def build(lo, hi):
        if lo >= hi:
            return None
        mid = (lo + hi) // 2
        idx = len(out)
        out.append([items[mid], None, None])
        out[idx][1] = build(lo, mid)
        out[idx][2] = build(mid + 1, hi)
        return idx
    build(0, len(items))
    return out


def entry_len(child):
    return (14 + len(child.name) + 3) & ~3


def layout_table(d):
    """Offsets of d's entries (pre-order), padded so that no entry crosses a sector. Returns (nodes, offsets, size)."""
    nodes = bst_preorder(d.children)
    offsets, off = [], 0
    for child, _, _ in nodes:
        n = entry_len(child)
        if off // SECTOR != (off + n - 1) // SECTOR:
            off = (off + SECTOR - 1) // SECTOR * SECTOR
        offsets.append(off)
        off += n
    size = max(SECTOR, (off + SECTOR - 1) // SECTOR * SECTOR)
    return nodes, offsets, size


def fill_table(d):
    nodes, offsets, size = layout_table(d)
    t = bytearray(b"\xff" * size)
    for i, (child, l, r) in enumerate(nodes):
        lo = offsets[l] // 4 if l is not None else 0
        ro = offsets[r] // 4 if r is not None else 0
        e = struct.pack("<HHIIBB", lo, ro, child.sector, child.size,
                        ATTR_DIR if child.is_dir else ATTR_FILE, len(child.name)) + child.name
        e += b"\x00" * ((-len(e)) % 4)
        t[offsets[i]:offsets[i] + len(e)] = e
    d.table = bytes(t)
    return nodes, offsets


def walk(node):
    yield node
    for c in node.children:
        yield from walk(c)


def build(src, out, base=0, renames=None, corrupt=None, truncate=None):
    root = scan(src, "", renames or {})
    if not root.is_dir:
        raise SystemExit("mkxiso: %s is not a directory" % src)
    dirs = [n for n in walk(root) if n.is_dir]
    files = [n for n in walk(root) if not n.is_dir]

    sec = 33
    for d in dirs:                                   # tables first, root at 33
        d.sector = sec
        d.size = layout_table(d)[2]
        sec += d.size // SECTOR
    for f in files:
        if f.size:
            f.sector = sec
            sec += (f.size + SECTOR - 1) // SECTOR
    end_sector = sec

    if corrupt == "sector-range":
        victim = next(c for c in root.children if not c.is_dir and c.size)
        victim.sector = end_sector + 16
    if corrupt == "dir-cycle":
        next(c for c in root.children if c.is_dir).sector = root.sector
    for d in dirs:
        nodes, offsets = fill_table(d)
        if corrupt == "self-loop" and d is root:
            i = next(i for i, (_, l, _r) in enumerate(nodes) if l is not None)   # a node with a left child
            j = nodes[i][1]
            t = bytearray(d.table)
            struct.pack_into("<H", t, offsets[j], offsets[j] // 4)              # its left child points at itself
            d.table = bytes(t)

    desc = bytearray(SECTOR)
    desc[0:20] = MAGIC if corrupt != "bad-magic" else b"MICROSOFT*XBOX*MEDIX"
    struct.pack_into("<II", desc, 0x14, root.sector, root.size)
    desc[0x7EC:0x7EC + 20] = MAGIC

    with open(out, "wb") as f:
        def put(sector, data):
            f.seek(base + sector * SECTOR)
            f.write(data)
        f.truncate(0)
        put(32, desc)
        for d in dirs:
            if corrupt == "dir-cycle" and d.sector == root.sector and d is not root:
                continue
            put(d.sector, d.table)
        for n in files:
            if n.size:
                with open(n.path, "rb") as src_f:
                    put(n.sector, src_f.read())
        total = base + end_sector * SECTOR
        f.truncate(total)
        if truncate is not None:
            f.truncate(truncate if truncate >= 0 else total + truncate)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--rename", action="append", default=[], metavar="OLD=NEW")
    ap.add_argument("--corrupt", choices=("bad-magic", "sector-range", "self-loop", "dir-cycle"))
    ap.add_argument("--truncate", type=int)
    a = ap.parse_args(argv)
    renames = dict(r.split("=", 1) for r in a.rename)
    build(a.src, a.out, a.base, renames, a.corrupt, a.truncate)
    return 0


if __name__ == "__main__":
    sys.exit(main())
