"""Reading a GameCube disc image for Prepare (XBA-264): ISO / GCM (also the sparse image the app extracts), CISO and
GCZ; RVZ / WIA are read by the runtime's own reader (gc_run --extract writes a sparse GCM of what Prepare needs).

What Prepare takes from a disc: the header (title key: game code + maker + "-" + revision, "-D<n>" from the second
disc, as gc_disc_key_from_header), the main DOL (its SHA-256 is the pack's source identity) and the REL modules of the
FST (*.rel), read by their module id.
"""

import struct
import zlib

MAGIC = 0xC2339F3D


class DiscError(Exception):
    pass


class _Plain:
    def __init__(self, f):
        self.f = f

    def read(self, off, n):
        self.f.seek(off)
        d = self.f.read(n)
        if len(d) != n:
            raise DiscError("read past the end of the image (0x%X+%d)" % (off, n))
        return d


class _Ciso:
    HEADER = 0x8000

    def __init__(self, f):
        self.f = f
        f.seek(4)
        self.block = struct.unpack("<I", f.read(4))[0]
        if self.block < 0x800 or self.block & (self.block - 1):
            raise DiscError("CISO: bad block size")
        mp = f.read(self.HEADER - 8)
        self.index, k = [], 0
        for b in mp:
            self.index.append(k if b else -1)
            k += 1 if b else 0

    def read(self, off, n):
        out = bytearray()
        while n:
            blk, inb = divmod(off, self.block)
            take = min(n, self.block - inb)
            i = self.index[blk] if blk < len(self.index) else -1
            if i < 0:
                out += b"\0" * take
            else:
                self.f.seek(self.HEADER + i * self.block + inb)
                d = self.f.read(take)
                if len(d) != take:
                    raise DiscError("CISO: truncated")
                out += d
            off += take
            n -= take
        return bytes(out)


class _Gcz:
    def __init__(self, f):
        self.f = f
        f.seek(0)
        magic, _t, self.csize, self.dsize, self.block, self.n = struct.unpack("<IIQQII", f.read(32))
        if magic != 0xB10BC001:
            raise DiscError("GCZ: bad magic")
        self.ptrs = struct.unpack("<%dQ" % self.n, f.read(8 * self.n))
        self.data_off = 32 + 12 * self.n
        self._cache = {}

    def _block(self, i):
        b = self._cache.get(i)
        if b is not None:
            return b
        p = self.ptrs[i]
        stored = bool(p >> 63)
        p &= (1 << 63) - 1
        end = (self.ptrs[i + 1] & ((1 << 63) - 1)) if i + 1 < self.n else self.csize
        self.f.seek(self.data_off + p)
        raw = self.f.read(end - p)
        b = raw if stored else zlib.decompress(raw)
        if len(self._cache) > 64:
            self._cache.clear()
        self._cache[i] = b
        return b

    def read(self, off, n):
        out = bytearray()
        while n:
            blk, inb = divmod(off, self.block)
            take = min(n, self.block - inb)
            out += self._block(blk)[inb:inb + take]
            off += take
            n -= take
        return bytes(out)


def _code_char(b):
    return chr(b) if 0x41 <= b <= 0x5A or 0x30 <= b <= 0x39 else "_"


def key_from_header(h):
    """gc_disc_key_from_header: « GALE01-0 », « GXXE01-1-D2 »."""
    if struct.unpack_from(">I", h, 0x1C)[0] != MAGIC:
        raise DiscError("no GameCube magic")
    ident = "".join(_code_char(b) for b in h[0:6])
    if h[6]:
        return "%s-%d-D%d" % (ident, h[7], h[6] + 1)
    return "%s-%d" % (ident, h[7])


def is_disc(path):
    """A GameCube disc image this module or the runtime reads, by content."""
    try:
        with open(path, "rb") as f:
            head = f.read(0x20)
            if len(head) >= 0x20 and struct.unpack_from(">I", head, 0x1C)[0] == MAGIC:
                return True
            if head[:4] == b"CISO":
                return True
            if head[:4] in (b"\x01\xC0\x0B\xB1", b"WIA\x01", b"RVZ\x01"):
                return True
    except OSError:
        pass
    return False


def needs_native(path):
    """RVZ / WIA: read through the runtime's extractor."""
    with open(path, "rb") as f:
        return f.read(4) in (b"WIA\x01", b"RVZ\x01")


def dol_size(head):
    """A DOL's size from its 0x100-byte header (the end of its furthest section)."""
    words = struct.unpack(">64I", head[:0x100])
    end = 0x100
    for k in range(18):
        off, size = words[k], words[36 + k]
        if size:
            end = max(end, off + size)
    return end


class Disc:
    """An open disc image: header, key, FST, files."""

    def __init__(self, path):
        self.f = open(path, "rb")
        try:
            head = self.f.read(4)
            if head == b"CISO":
                self.r = _Ciso(self.f)
                self.format = "ciso"
            elif head == b"\x01\xC0\x0B\xB1":
                self.r = _Gcz(self.f)
                self.format = "gcz"
            elif head in (b"WIA\x01", b"RVZ\x01"):
                raise DiscError("an RVZ / WIA image: extract it first (gc_run --extract OUT.gcm IMAGE)")
            else:
                self.r = _Plain(self.f)
                self.format = "iso"
            self.header = self.r.read(0, 0x440)
            self.key = key_from_header(self.header)
            self.name = self.header[0x20:0x60].split(b"\0")[0].decode("latin-1").strip()
            self.dol_off, self.fst_off, self.fst_size = struct.unpack_from(">III", self.header, 0x420)
        except Exception:
            self.f.close()
            raise

    def close(self):
        self.f.close()

    def read(self, off, n):
        return self.r.read(off, n)

    def dol(self):
        head = self.read(self.dol_off, 0x100)
        n = dol_size(head)
        if n > 16 << 20:
            raise DiscError("main DOL of %d bytes" % n)
        return self.read(self.dol_off, n)

    def fst(self):
        """[(path, offset, size)] of every file."""
        if not self.fst_size or self.fst_size > 16 << 20:
            raise DiscError("FST of %d bytes" % self.fst_size)
        raw = self.read(self.fst_off, self.fst_size)
        n = struct.unpack_from(">I", raw, 8)[0]
        if n == 0 or 12 * n > len(raw):
            raise DiscError("FST entry count %d" % n)
        names = raw[12 * n:]
        out = []
        ends = [(n, "")]

        def name(o):
            e = names.find(b"\0", o)
            return names[o:e if e >= 0 else len(names)].decode("latin-1")

        for i in range(1, n):
            while ends and i >= ends[-1][0]:
                ends.pop()
            w0, a, b = struct.unpack_from(">III", raw, 12 * i)
            nm = name(w0 & 0xFFFFFF)
            prefix = ends[-1][1] if ends else ""
            if w0 >> 24:
                if b <= i or b > n:
                    raise DiscError("FST directory %s ends at %d" % (nm, b))
                ends.append((b, prefix + nm + "/"))
            else:
                out.append((prefix + nm, a, b))
        return out

    def rels(self):
        """[(path, bytes)] of the FST's REL modules."""
        out = []
        for path, off, size in self.fst():
            if path.lower().endswith(".rel") and 0x40 <= size <= 32 << 20:
                out.append((path, self.read(off, size)))
        return out
