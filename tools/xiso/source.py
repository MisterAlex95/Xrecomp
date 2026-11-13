"""
Disc image sources (XBA-126): one file, FAT32 split parts, compressed images.

The Python twin of src/xiso/xiso_source.c, for the pipeline (tools.prepare, tools.triage, `python3 -m tools.xiso`):
`Source(path)` reads the image's bytes whatever holds them. Same rules as the C reader:

- split parts by name: ``Game.1.iso`` / ``Game.2.iso``, ``Game.part1.iso``, ``Game.iso.1`` / ``Game.iso.001``, with
  .iso .xiso .cso .ciso .zso .cci; any part may be named, the lowest number found is the first;
- compressed by magic (never by name): CSO ("CISO": v0/v1 deflate, v2 deflate or LZ4, the Xbox tools' layout with
  LZ4 flagged blocks), ZSO ("ZISO", LZ4), CCI ("CCIM", LZ4). The format notes are at the top of xiso_source.c.

Standard library only (zlib for deflate, a small LZ4 block decoder here). Not read: CHD, 7z, zip.
"""

import os
import re
import struct
import zlib

EXTS = ("iso", "xiso", "cso", "ciso", "zso", "cci")
MAX_PARTS = 16
_EXT = "|".join(EXTS)
_PATTERNS = (
    re.compile(r"^(?P<pre>.*\.(?:%s)\.)(?P<n>\d{1,4})(?P<post>)$" % _EXT, re.I),       # Game.iso.1, .001
    re.compile(r"^(?P<pre>.+\.part)(?P<n>\d{1,4})(?P<post>\.(?:%s))$" % _EXT, re.I),   # Game.part1.iso
    re.compile(r"^(?P<pre>.+\.)(?P<n>\d{1,4})(?P<post>\.(?:%s))$" % _EXT, re.I),       # Game.1.iso
)


class SourceError(Exception):
    pass


def split_parts(path):
    """The split parts path belongs to, in order; [path] when it is not one (or alone)."""
    d, name = os.path.split(path)
    for pat in _PATTERNS:
        m = pat.match(name)
        if not m:
            continue
        digits = m.group("n")
        width = len(digits) if digits.startswith("0") and len(digits) > 1 else 1
        first = 0 if int(digits) == 0 else 1
        parts = []
        for k in range(first, first + MAX_PARTS):
            cand = os.path.join(d, "%s%0*d%s" % (m.group("pre"), width, k, m.group("post")))
            if not os.path.isfile(cand):
                break
            parts.append(cand)
        if len(parts) >= 2 and any(os.path.samefile(p, path) for p in parts):
            return parts
        return [path]
    return [path]


def lz4_block(src, want):
    """Decodes one LZ4 block into exactly `want` bytes (input after it is ignored). Raises ValueError."""
    out = bytearray()
    i, n = 0, len(src)
    if want == 0:
        return b""
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                if i >= n:
                    raise ValueError("lz4: truncated")
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        if lit > n - i or lit > want - len(out):
            raise ValueError("lz4: literals overrun")
        out += src[i:i + lit]
        i += lit
        if len(out) == want:
            return bytes(out)
        if n - i < 2:
            raise ValueError("lz4: truncated")
        off = src[i] | src[i + 1] << 8
        i += 2
        if off == 0 or off > len(out):
            raise ValueError("lz4: bad offset")
        ml = token & 15
        if ml == 15:
            while True:
                if i >= n:
                    raise ValueError("lz4: truncated")
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        if ml > want - len(out):
            raise ValueError("lz4: match overrun")
        start = len(out) - off
        if off >= ml:
            out += out[start:start + ml]
        else:
            for k in range(ml):
                out.append(out[start + k])
        if len(out) == want:
            return bytes(out)
    raise ValueError("lz4: short block")


def _inflate(data, want):
    d = zlib.decompressobj(-15)
    out = d.decompress(data, want + 1)
    if len(out) != want or not d.eof:
        raise ValueError("deflate: bad block")
    return out


def _lz4_framed(d, want, align):
    if len(d) >= 8:
        v = struct.unpack_from("<I", d)[0]
        k = v & 0x7FFFFFFF
        if k <= len(d) - 4 and len(d) - 4 - k <= (1 << align) + 3:
            try:
                if v >> 31:
                    if k >= want:
                        return bytes(d[4:4 + want])
                else:
                    return lz4_block(d[4:4 + k], want)
            except ValueError:
                pass
    return lz4_block(d, want)


class Source:
    """read_at(offset, length) over the image's bytes; size, kind ("iso", "cso", "zso", "cci"), parts."""

    def __init__(self, path):
        self.path = path
        self.parts = split_parts(path)
        self._files = [open(p, "rb") for p in self.parts]
        self._starts = [0]
        for f in self._files:
            f.seek(0, os.SEEK_END)
            self._starts.append(self._starts[-1] + f.tell())
        self.stored_size = self._starts[-1]
        self.kind = "iso"
        self.size = self.stored_size
        self._cache = {}
        try:
            self._open_compressed()
        except Exception:
            self.close()
            raise

    def close(self):
        for f in self._files:
            f.close()
        self._files = []

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def _raw(self, off, n):
        out = bytearray()
        i = 0
        while n > 0:
            while i < len(self._files) and off >= self._starts[i + 1]:
                i += 1
            if i >= len(self._files):
                break
            f = self._files[i]
            f.seek(off - self._starts[i])
            chunk = f.read(min(n, self._starts[i + 1] - off))
            if not chunk:
                break
            out += chunk
            off += len(chunk)
            n -= len(chunk)
        return bytes(out)

    def _open_compressed(self):
        h = self._raw(0, 32)
        if h[:4] in (b"CISO", b"ZISO") and len(h) >= 24:
            hs, self.size, self.block, self.version, self.align = struct.unpack_from("<IQIBB", h, 4)
            self.kind = "cso" if h[:4] == b"CISO" else "zso"
            index_off = hs if 24 <= hs <= 4096 else 24
        elif h[:4] == b"CCIM" and len(h) >= 32:
            _, self.size, index_off, self.block, self.version, self.align = struct.unpack_from("<IQQIBB", h, 4)
            self.kind = "cci"
        else:
            return
        if not (512 <= self.block <= 1 << 20) or self.size == 0 or self.align > 16:
            raise SourceError("%s: bad %s header" % (os.path.basename(self.path), self.kind))
        self.nblocks = (self.size + self.block - 1) // self.block
        raw = self._raw(index_off, (self.nblocks + 1) * 4)
        if len(raw) != (self.nblocks + 1) * 4:
            raise SourceError("%s: truncated %s index" % (os.path.basename(self.path), self.kind))
        self.index = struct.unpack("<%dI" % (self.nblocks + 1), raw)

    def _pos(self, b):
        return (self.index[b] & 0x7FFFFFFF) << self.align

    def _block(self, b):
        got = self._cache.get(b)
        if got is not None:
            return got
        want = self.block if b + 1 < self.nblocks else self.size - b * self.block
        p, q = self._pos(b), self._pos(b + 1)
        if q < p or q - p > 2 * self.block + 64:
            raise SourceError("%s: bad index at block %d" % (os.path.basename(self.path), b))
        d = self._raw(p, q - p)
        flag = self.index[b] >> 31
        try:
            if self.kind == "cso" and self.version < 2:
                out = d[:want] if flag else _inflate(d, want)
            elif self.kind == "cso":
                if len(d) >= want:
                    out = d[:want]
                elif flag:
                    out = _lz4_framed(d, want, self.align)
                else:
                    try:
                        out = _inflate(d, want)
                    except (ValueError, zlib.error):
                        out = _lz4_framed(d, want, self.align)
            elif self.kind == "zso":
                out = d[:want] if flag else lz4_block(d, want)
            else:                                   # cci
                if not flag:
                    out = d[:want]
                else:
                    out = None
                    if d and d[0] <= (1 << self.align) and len(d) - 1 > d[0]:
                        try:
                            out = lz4_block(d[1:len(d) - d[0]], want)
                        except ValueError:
                            out = None
                    if out is None:
                        out = lz4_block(d, want)
        except (ValueError, zlib.error) as e:
            raise SourceError("%s: block %d does not decode (%s)" % (os.path.basename(self.path), b, e))
        if len(out) != want:
            raise SourceError("%s: short block %d" % (os.path.basename(self.path), b))
        if len(self._cache) > 512:
            self._cache.clear()
        self._cache[b] = out
        return out

    def read_at(self, off, n):
        """Up to n bytes at off (fewer only at the end of the image)."""
        if self.kind == "iso":
            return self._raw(off, n)
        if off >= self.size:
            return b""
        n = min(n, self.size - off)
        out = bytearray()
        while n > 0:
            b, inb = divmod(off, self.block)
            chunk = self._block(b)[inb:inb + n]
            out += chunk
            off += len(chunk)
            n -= len(chunk)
        return bytes(out)


def is_image_name(path):
    """A name the pipeline takes for a disc image (or one of its split parts)."""
    low = os.path.basename(path).lower()
    if any(low.endswith("." + e) for e in EXTS):
        return True
    return any(p.match(os.path.basename(path)) for p in _PATTERNS[:1])
