"""List the RT_RCDATA resources of Lossless.dll: IDs, sizes and kinds (DXBC / SPIR-V / other). Metadata only.

    python3 -m tools.lsfg.inspect_dll path/to/Lossless.dll [--json] [--all]

Prints, for the LSFG 3.1 and 3.1 performance shader sets (the resource IDs framegen needs, src/framegen/lsfg_spv.c),
how many are present as DXBC at the base ID, as embedded FP16 SPIR-V at base + 49 and as FP32 SPIR-V at base + 98.
Never prints or writes resource contents: the DLL is the user's own file and stays out of the repository, APKs, CI
and logs (docs/technical/framegen.md).
"""

import argparse
import json
import struct
import sys

RT_RCDATA = 10
FP16_OFFSET = 49
FP32_OFFSET = 98

# Base resource IDs per variant (framegen shader names -> IDs, the table lsfg-vk 1.0 used; lsfg_spv.c).
FULL_IDS = [255, 256] + list(range(257, 280))
PERF_IDS = [255, 256] + list(range(280, 303))


class PeError(Exception):
    pass


def _u16(b, o):
    if o + 2 > len(b):
        raise PeError("truncated at 0x%x" % o)
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    if o + 4 > len(b):
        raise PeError("truncated at 0x%x" % o)
    return struct.unpack_from("<I", b, o)[0]


def rcdata(buf):
    """[(id, size, first bytes)] of the RT_RCDATA resources with numeric IDs."""
    try:
        return _rcdata(buf)
    except struct.error as e:
        raise PeError("truncated (%s)" % e)


def _rcdata(buf):
    if buf[:2] != b"MZ":
        raise PeError("no MZ header")
    pe = _u32(buf, 0x3C)
    if buf[pe:pe + 4] != b"PE\0\0":
        raise PeError("no PE signature")
    nsec = _u16(buf, pe + 6)
    optsz = _u16(buf, pe + 20)
    opt = pe + 24
    magic = _u16(buf, opt)
    dirs = {0x20B: 112, 0x10B: 96}.get(magic)
    if dirs is None:
        raise PeError("optional header magic 0x%x" % magic)
    rva = _u32(buf, opt + dirs + 16)
    secs = []
    for i in range(nsec):
        s = opt + optsz + 40 * i
        vsize, va, rawsz, rawptr = struct.unpack_from("<IIII", buf, s + 8)
        secs.append((va, max(vsize, rawsz), rawsz, rawptr))

    def off(r):
        for va, span, rawsz, rawptr in secs:
            if va <= r < va + span and r - va < rawsz:
                return rawptr + (r - va)
        raise PeError("rva 0x%x outside the sections" % r)

    root = off(rva)

    def entries(d):
        named, ids = _u16(buf, root + d + 12), _u16(buf, root + d + 14)
        if named + ids > 4096:
            raise PeError("oversized resource directory")
        for i in range(named, named + ids):
            e = root + d + 16 + 8 * i
            yield _u32(buf, e), _u32(buf, e + 4)

    out = []
    for typ, tgt in entries(0):
        if typ != RT_RCDATA or not tgt & 0x80000000:
            continue
        for rid, ntgt in entries(tgt & 0x7FFFFFFF):
            if not ntgt & 0x80000000:
                raise PeError("bad resource tree")
            langs = list(entries(ntgt & 0x7FFFFFFF))
            if not langs or langs[0][1] & 0x80000000:
                raise PeError("bad resource tree")
            de = root + langs[0][1]
            drva, size = _u32(buf, de), _u32(buf, de + 4)
            o = off(drva)
            if o + size > len(buf):
                raise PeError("resource %d runs past the end" % rid)
            out.append((rid & 0x7FFFFFFF, size, buf[o:o + min(size, 8)]))
    return out


def kind(head):
    if head[:4] == b"DXBC":
        return "dxbc"
    if len(head) >= 4 and struct.unpack_from("<I", head)[0] == 0x07230203:
        return "spirv"
    return "other"


def summarize(res):
    by_id = {rid: (size, kind(head)) for rid, size, head in res}
    out = {"rcdata": len(res), "kinds": {}}
    for _, (_, k) in by_id.items():
        out["kinds"][k] = out["kinds"].get(k, 0) + 1
    for name, ids in (("lsfg_3_1", FULL_IDS), ("lsfg_3_1p", PERF_IDS)):
        v = {"needed": len(ids)}
        v["dxbc"] = sum(1 for i in ids if by_id.get(i, (0, ""))[1] == "dxbc")
        v["spirv_fp16"] = sum(1 for i in ids if by_id.get(i + FP16_OFFSET, (0, ""))[1] == "spirv")
        v["spirv_fp32"] = sum(1 for i in ids if by_id.get(i + FP32_OFFSET, (0, ""))[1] == "spirv")
        out[name] = v
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dll")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--all", action="store_true", help="list every RCDATA resource, not only the LSFG IDs")
    a = ap.parse_args(argv)
    with open(a.dll, "rb") as f:
        buf = f.read()
    try:
        res = rcdata(buf)
    except PeError as e:
        print("%s: %s" % (a.dll, e), file=sys.stderr)
        return 1
    s = summarize(res)
    lsfg = set(FULL_IDS + PERF_IDS)
    lsfg |= {i + FP16_OFFSET for i in lsfg} | {i + FP32_OFFSET for i in lsfg}
    rows = [(rid, size, kind(head)) for rid, size, head in res if a.all or rid in lsfg]
    if a.json:
        s["resources"] = [{"id": r, "size": n, "kind": k} for r, n, k in rows]
        print(json.dumps(s, indent=1))
        return 0
    print("RT_RCDATA resources: %d (%s)" % (s["rcdata"], ", ".join("%s %d" % kv for kv in sorted(s["kinds"].items()))))
    for name in ("lsfg_3_1", "lsfg_3_1p"):
        v = s[name]
        print("%-10s %2d/%d DXBC, %2d/%d SPIR-V FP16 (+%d), %2d/%d SPIR-V FP32 (+%d)" % (
            name, v["dxbc"], v["needed"], v["spirv_fp16"], v["needed"], FP16_OFFSET,
            v["spirv_fp32"], v["needed"], FP32_OFFSET))
    print("%6s %9s  %s" % ("id", "bytes", "kind"))
    for r, n, k in rows:
        print("%6d %9d  %s" % (r, n, k))
    return 0


if __name__ == "__main__":
    sys.exit(main())
