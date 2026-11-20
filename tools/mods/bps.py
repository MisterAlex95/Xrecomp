"""
BPS patches: apply one, and make one (docs/technical/mods.md « Patches »).

Our own implementation of the format as byuu / Near published it (beat's « BPS » specification): the header
"BPS1", the source, target and metadata sizes as variable-length numbers, then actions until the last 12 bytes,
which are the CRC32 of the source, of the target and of the patch before them. Actions: the length minus one
shifted left by 2, plus the command: 0 SourceRead (the source's bytes at the output offset), 1 TargetRead (bytes
in the patch), 2 SourceCopy and 3 TargetCopy (a signed relative offset into the source / the output so far).

apply() checks every CRC and size: a patch made for another binary is refused with BpsError("source") before
anything is written, and a damaged one with "patch" or "target".
"""

import zlib

MAGIC = b"BPS1"


class BpsError(ValueError):
    """kind: "format" (not a BPS patch, damaged structure), "patch" (its own CRC), "source" (made for another
    binary: size or CRC), "target" (the result does not match the CRC the patch carries)."""

    def __init__(self, msg, kind):
        ValueError.__init__(self, msg)
        self.kind = kind


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def _num(data, pos, end):
    v, shift = 0, 1
    while True:
        if pos >= end:
            raise BpsError("BPS: a number runs past the end of the patch", "format")
        x = data[pos]
        pos += 1
        v += (x & 0x7F) * shift
        if x & 0x80:
            return v, pos
        shift <<= 7
        v += shift


def enc_num(n):
    out = bytearray()
    while True:
        x = n & 0x7F
        n >>= 7
        if n == 0:
            out.append(0x80 | x)
            return bytes(out)
        out.append(x)
        n -= 1


def info(patch):
    """{"source_size", "target_size", "metadata", "source_crc", "target_crc"} of a BPS patch (BpsError if not one)."""
    patch = bytes(patch)
    if len(patch) < len(MAGIC) + 3 + 12 or patch[:4] != MAGIC:
        raise BpsError("not a BPS patch (no BPS1 header)", "format")
    end = len(patch) - 12
    pos = 4
    ss, pos = _num(patch, pos, end)
    ts, pos = _num(patch, pos, end)
    ms, pos = _num(patch, pos, end)
    if pos + ms > end:
        raise BpsError("BPS: metadata runs past the end of the patch", "format")
    return {"source_size": ss, "target_size": ts, "metadata": patch[pos:pos + ms], "body": pos + ms,
            "source_crc": int.from_bytes(patch[end:end + 4], "little"),
            "target_crc": int.from_bytes(patch[end + 4:end + 8], "little")}


def apply(source, patch):
    """The target bytes. BpsError when the patch is damaged or made for another source."""
    source, patch = bytes(source), bytes(patch)
    h = info(patch)
    end = len(patch) - 12
    if crc32(patch[:-4]) != int.from_bytes(patch[-4:], "little"):
        raise BpsError("BPS: the patch is damaged (its CRC does not match)", "patch")
    if len(source) != h["source_size"] or crc32(source) != h["source_crc"]:
        raise BpsError("BPS: made for another file (%d bytes, CRC32 %08X; this one: %d bytes, CRC32 %08X)"
                       % (h["source_size"], h["source_crc"], len(source), crc32(source)), "source")
    ts = h["target_size"]
    out = bytearray()
    pos = h["body"]
    src_rel = tgt_rel = 0
    while pos < end:
        d, pos = _num(patch, pos, end)
        cmd, length = d & 3, (d >> 2) + 1
        if len(out) + length > ts:
            raise BpsError("BPS: writes past the target size", "format")
        if cmd == 0:                                    # SourceRead
            at = len(out)
            if at + length > len(source):
                raise BpsError("BPS: SourceRead past the end of the source", "format")
            out += source[at:at + length]
        elif cmd == 1:                                  # TargetRead
            if pos + length > end:
                raise BpsError("BPS: TargetRead past the end of the patch", "format")
            out += patch[pos:pos + length]
            pos += length
        else:
            o, pos = _num(patch, pos, end)
            o = -(o >> 1) if o & 1 else o >> 1
            if cmd == 2:                                # SourceCopy
                src_rel += o
                if src_rel < 0 or src_rel + length > len(source):
                    raise BpsError("BPS: SourceCopy outside the source", "format")
                out += source[src_rel:src_rel + length]
                src_rel += length
            else:                                       # TargetCopy (may overlap what it writes: byte by byte)
                tgt_rel += o
                if tgt_rel < 0 or tgt_rel >= len(out):
                    raise BpsError("BPS: TargetCopy outside the output", "format")
                for _ in range(length):
                    out.append(out[tgt_rel])
                    tgt_rel += 1
    if len(out) != ts:
        raise BpsError("BPS: the result is %d bytes, the patch says %d" % (len(out), ts), "format")
    if crc32(out) != h["target_crc"]:
        raise BpsError("BPS: the result does not match the patch's target CRC", "target")
    return bytes(out)


def make(source, target, metadata=b""):
    """A BPS patch from source to target: runs of bytes equal at the same offset become SourceRead, the others
    TargetRead (and the bytes past the source's end). Small for the patches mods make of a binary (a few changed
    instructions or values, same size); not a general delta encoder."""
    source, target = bytes(source), bytes(target)
    out = bytearray(MAGIC)
    out += enc_num(len(source)) + enc_num(len(target)) + enc_num(len(metadata)) + bytes(metadata)
    i, n = 0, len(target)
    while i < n:
        same = i < len(source) and source[i] == target[i]
        j = i
        if same:
            while j < n and j < len(source) and source[j] == target[j]:
                j += 1
            out += enc_num(((j - i - 1) << 2) | 0)
        else:
            # Keep a short equal run inside a TargetRead: it costs less than a SourceRead and a new TargetRead.
            while j < n and not (j + 4 <= min(n, len(source)) and source[j:j + 4] == target[j:j + 4]):
                j += 1
            out += enc_num(((j - i - 1) << 2) | 1) + target[i:j]
        i = j
    out += crc32(source).to_bytes(4, "little") + crc32(target).to_bytes(4, "little")
    out += crc32(bytes(out)).to_bytes(4, "little")
    return bytes(out)
