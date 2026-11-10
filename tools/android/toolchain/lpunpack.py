"""Minimal lpunpack: copies logical partitions out of an Android "super" partition (liblp metadata format).

    python3 -I lpunpack.py DISK_IMAGE SUPER_START_SECTOR OUT_DIR [NAME ...]

Used by android_root.sh on the SDK's emulator system image (a GPT disk whose "super" partition holds system,
vendor, ...). Writes OUT_DIR/<name>.img for each NAME (all when none is given) and prints every partition.
"""
import os
import struct
import sys

LP_GEOMETRY_MAGIC = 0x616C4467
LP_HEADER_MAGIC = 0x414C5030
LP_RESERVED = 4096          # bytes before the primary geometry
LP_GEOMETRY_SIZE = 4096     # primary + backup geometry precede the metadata


def main(argv):
    img, start, out = argv[0], int(argv[1]) * 512, argv[2]
    want = set(argv[3:])
    with open(img, "rb") as f:
        f.seek(start + LP_RESERVED)
        if struct.unpack("<I", f.read(4))[0] != LP_GEOMETRY_MAGIC:
            raise SystemExit("no LP geometry at sector %s" % argv[1])
        meta = start + LP_RESERVED + 2 * LP_GEOMETRY_SIZE
        f.seek(meta)
        h = f.read(256)
        magic, _major, _minor, hsize = struct.unpack("<IHHI", h[:12])
        if magic != LP_HEADER_MAGIC:
            raise SystemExit("bad LP header magic %#x" % magic)
        tables_size = struct.unpack("<I", h[44:48])[0]
        # partitions, extents, groups, block devices: (offset, count, entry size) each
        desc = [struct.unpack("<III", h[80 + 12 * i:92 + 12 * i]) for i in range(4)]
        f.seek(meta + hsize)
        tables = f.read(tables_size)
        po, pn, pe = desc[0]
        eo, en, ee = desc[1]
        exts = [struct.unpack("<QIQI", tables[eo + i * ee:eo + i * ee + 24]) for i in range(en)]
        os.makedirs(out, exist_ok=True)
        for i in range(pn):
            e = tables[po + i * pe:po + (i + 1) * pe]
            name = e[:36].rstrip(b"\0").decode()
            _attrs, first, num, _group = struct.unpack("<IIII", e[36:52])
            print(name, sum(exts[first + k][0] for k in range(num)) * 512)
            if want and name not in want:
                continue
            with open(os.path.join(out, name + ".img"), "wb") as o:
                for k in range(num):
                    sectors, target_type, target_data, _source = exts[first + k]
                    if target_type != 0:            # LP_TARGET_TYPE_ZERO
                        o.write(b"\0" * sectors * 512)
                        continue
                    f.seek(start + target_data * 512)
                    left = sectors * 512
                    while left:
                        b = f.read(min(left, 1 << 22))
                        if not b:
                            raise SystemExit("%s: short read" % name)
                        o.write(b)
                        left -= len(b)


if __name__ == "__main__":
    main(sys.argv[1:])
