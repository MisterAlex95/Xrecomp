"""
IPS patches: apply one, and make one (docs/technical/mods.md « Patches »).

Our own implementation of the classic format: "PATCH", then records until "EOF": a 3-byte big-endian offset, a
2-byte size and that many bytes; a size of 0 is a run (2-byte count, 1 byte repeated). An optional 3-byte size
after "EOF" truncates the file (the common extension). IPS carries no checksum: the mod's "binary" SHA-256 is what
tells a patch made for another version (tools.mods.patch checks it before applying).
"""

MAGIC = b"PATCH"
EOF = b"EOF"
LIMIT = 1 << 24             # offsets are 3 bytes


class IpsError(ValueError):
    pass


def apply(source, patch):
    patch = bytes(patch)
    if patch[:5] != MAGIC:
        raise IpsError("not an IPS patch (no PATCH header)")
    out = bytearray(source)
    pos = 5
    while True:
        if pos + 3 > len(patch):
            raise IpsError("IPS: no EOF marker")
        if patch[pos:pos + 3] == EOF:
            pos += 3
            break
        if pos + 5 > len(patch):
            raise IpsError("IPS: a record runs past the end of the patch")
        off = int.from_bytes(patch[pos:pos + 3], "big")
        size = int.from_bytes(patch[pos + 3:pos + 5], "big")
        pos += 5
        if size:
            data = patch[pos:pos + size]
            if len(data) != size:
                raise IpsError("IPS: a record's data runs past the end of the patch")
            pos += size
        else:
            if pos + 3 > len(patch):
                raise IpsError("IPS: a run record runs past the end of the patch")
            count = int.from_bytes(patch[pos:pos + 2], "big")
            data = patch[pos + 2:pos + 3] * count
            pos += 3
        if off > len(out):
            out += b"\0" * (off - len(out))
        out[off:off + len(data)] = data
    if pos + 3 == len(patch):
        del out[int.from_bytes(patch[pos:pos + 3], "big"):]
    elif pos != len(patch):
        raise IpsError("IPS: bytes after the EOF marker")
    return bytes(out)


def make(source, target):
    """An IPS patch from source to target (target no larger than 16 MB). Records of the differing runs; a record
    that would start at offset 0x454F46 ("EOF") starts one byte earlier."""
    source, target = bytes(source), bytes(target)
    if len(target) > LIMIT:
        raise IpsError("IPS: files over 16 MB need BPS")
    out = bytearray(MAGIC)
    i, n = 0, len(target)
    while i < n:
        if i < len(source) and source[i] == target[i]:
            i += 1
            continue
        j = i
        while j < n and j - i < 0xFFFF and not (j < len(source) and source[j:j + 6] == target[j:j + 6]):
            j += 1
        start = i - 1 if i == 0x454F46 else i
        out += start.to_bytes(3, "big") + (j - start).to_bytes(2, "big") + target[start:j]
        i = j
    out += EOF
    if len(target) < len(source):
        out += len(target).to_bytes(3, "big")
    return bytes(out)
