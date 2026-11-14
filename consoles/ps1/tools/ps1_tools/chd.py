"""MAME CHD v5 CD images (XBA-159 / XBA-168): the data sectors a Prepare needs (ISO 9660, SYSTEM.CNF, the boot EXE)
read from a .chd, in pure Python (the app's embedded CPython 3.13 runs it; consoles/ps1/runtime/ps1_disc.c reads the
same images through libchdr when a game plays).

Layout (MAME's chd.cpp / libchdr's chd.c, which consoles/ps1/runtime uses): a 124-byte big-endian header (version 5,
four codec tags, logical size, map and metadata offsets, hunk and unit sizes), a hunk map, CHT2 / CHTR metadata
entries (a linked list) describing the tracks, and hunks of `hunkbytes / 2448` CD frames (2352 bytes of sector + 96
of subcode each), every track padded to a multiple of 4 frames.

Map: uncompressed CHDs have a 4-byte hunk number per hunk (offset = n * hunkbytes, 0 = a hunk of zeros). Compressed
ones have the 16-byte map header then a bit stream: the compression type of every hunk, Huffman coded (16 codes of at
most 8 bits, an RLE-coded code-length table) with run lengths, then per hunk its length / CRC-16 (codec 0..3, none) or
the hunk it repeats (self); the CRC-16 of the decoded map is checked. A hunk read through the parent CHD is refused.

Codecs of CD images: `cdzl` (raw deflate) and `cdlz` (raw LZMA, lc 3 lp 0 pb 2) are in every CPython (zlib, lzma);
`cdzs` (zstd) needs CPython 3.14's compression.zstd; `cdfl` (FLAC) is not decoded: chdman picks it for audio hunks,
which a Prepare never reads (a data hunk compressed with it, or with zstd on an older Python, is reported). A CD
codec's hunk holds a bitmap of the frames whose sync and ECC were stripped (restored here: sync only, the ECC is
not needed for user data), the compressed length of the sector data (2 bytes, 3 for hunks of 64 KB or more), the
sector data (frames x 2352) and then the subcode, which is never decompressed here.
"""

import lzma
import os
import struct
import zlib

FRAME = 2352
SUBCODE = 96
FRAME_STORED = FRAME + SUBCODE
TRACK_PADDING = 4
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"

# Map entry types (libchdr's COMPRESSION_*).
TYPE_0, TYPE_1, TYPE_2, TYPE_3, NONE, SELF, PARENT, RLE_SMALL, RLE_LARGE, SELF_0, SELF_1, PARENT_SELF, PARENT_0, \
    PARENT_1 = range(14)


def _tag(s):
    return struct.unpack(">I", s.encode("ascii"))[0]


CODEC_NAMES = {_tag(n): n for n in ("cdzl", "cdlz", "cdzs", "cdfl", "zlib", "lzma", "zstd", "flac", "huff", "avhu")}


class ChdError(ValueError):
    """A CHD this reader cannot read (with why)."""


def _crc16_table():
    t = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
        t.append(c)
    return t


_CRC16 = _crc16_table()


def crc16(data):
    """CRC-16/CCITT-FALSE, the CHD map and hunk check."""
    crc, t = 0xFFFF, _CRC16
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ t[(crc >> 8) ^ b]
    return crc


class _Bits:
    """MSB-first bit reader (libchdr's bitstream): reads past the end give zeros, `overflow` says it happened."""

    def __init__(self, data):
        self.v = int.from_bytes(data, "big") if data else 0
        self.n = len(data) * 8
        self.pos = 0

    def peek(self, bits):
        if bits == 0:
            return 0
        end = self.pos + bits
        if end <= self.n:
            return (self.v >> (self.n - end)) & ((1 << bits) - 1)
        avail = max(0, self.n - self.pos)
        head = self.v & ((1 << avail) - 1) if avail else 0
        return head << (bits - avail)

    def read(self, bits):
        r = self.peek(bits)
        self.pos += bits
        return r

    def overflow(self):
        return self.pos > self.n


class _Huffman:
    """libchdr's huffman_decoder for the map: an RLE code-length table, canonical codes (longest first), a full lookup
    table of 2^maxbits entries."""

    def __init__(self, bits, numcodes=16, maxbits=8):
        numbits = 5 if maxbits >= 16 else 4 if maxbits >= 8 else 3
        lengths = []
        while len(lengths) < numcodes:
            nb = bits.read(numbits)
            if nb != 1:
                lengths.append(nb)
                continue
            nb = bits.read(numbits)
            if nb == 1:
                lengths.append(1)
                continue
            rep = bits.read(numbits) + 3
            if len(lengths) + rep > numcodes:
                raise ChdError("chd: bad map (Huffman table)")
            lengths += [nb] * rep
        histo = [0] * 33
        for n in lengths:
            if n > maxbits:
                raise ChdError("chd: bad map (Huffman code length)")
            histo[n] += 1
        start = 0
        for n in range(32, 0, -1):
            nxt = (start + histo[n]) >> 1
            if n != 1 and nxt * 2 != start + histo[n]:
                raise ChdError("chd: bad map (Huffman tree)")
            histo[n] = start
            start = nxt
        self.maxbits = maxbits
        self.lookup = [(0, 0)] * (1 << maxbits)
        for value, n in enumerate(lengths):
            if n <= 0:
                continue
            code = histo[n]
            histo[n] += 1
            shift = maxbits - n
            for i in range(code << shift, (code + 1) << shift):
                self.lookup[i] = (value, n)

    def decode(self, bits):
        value, n = self.lookup[bits.peek(self.maxbits)]
        bits.pos += n
        return value


class Track:
    def __init__(self, num, ttype, frames, pregap, pgtype, postgap):
        self.num, self.type, self.frames, self.pregap, self.pgtype, self.postgap = \
            num, ttype, frames, pregap, pgtype, postgap
        self.audio = ttype == "AUDIO"
        self.begin = self.start = self.data_lba = self.end = 0
        self.frame = 0          # CHD frame number of data_lba


class Chd:
    """A CHD v5 CD image: tracks, raw frames, and the 2048 user bytes of a data sector by absolute LBA (LBA 0 = MSF
    00:02:00, as ps1_disc.c)."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        try:
            self._open()
        except BaseException:
            self.f.close()
            raise

    def close(self):
        self.f.close()

    def _read_at(self, off, n):
        self.f.seek(off)
        data = self.f.read(n)
        if len(data) != n:
            raise ChdError("chd: truncated file (%d bytes at %d)" % (n, off))
        return data

    def _open(self):
        self.size = os.fstat(self.f.fileno()).st_size
        head = self.f.read(124)
        if len(head) < 16 or head[:8] != b"MComprHD":
            raise ChdError("not a CHD (no MComprHD magic)")
        length, version = struct.unpack_from(">II", head, 8)
        if version != 5:
            raise ChdError("chd: version %d (only CHD v5, what chdman has written since 2012, is read; "
                           "`chdman copy` converts an older one)" % version)
        if length < 124 or len(head) < 124:
            raise ChdError("chd: truncated header")
        self.codecs = list(struct.unpack_from(">4I", head, 16))
        self.logical, self.map_off, self.meta_off = struct.unpack_from(">QQQ", head, 32)
        self.hunkbytes, self.unitbytes = struct.unpack_from(">II", head, 56)
        if any(head[104:124]):
            raise ChdError("chd: a CHD with a parent (a diff image) cannot be read alone")
        if self.hunkbytes < FRAME_STORED or self.hunkbytes % FRAME_STORED:
            raise ChdError("chd: not a CD image (hunk of %d bytes)" % self.hunkbytes)
        self.frames_per_hunk = self.hunkbytes // FRAME_STORED
        self.hunks = (self.logical + self.hunkbytes - 1) // self.hunkbytes
        if self.hunks <= 0 or self.hunks > 1 << 24:
            raise ChdError("chd: bad hunk count")
        self._read_map()
        self._read_tracks()
        self._cache = (None, None)

    # ---- map ----
    def _read_map(self):
        n = self.hunks
        if self.codecs[0] == 0:
            raw = self._read_at(self.map_off, 4 * n)
            self.map = [(NONE if e else -1, e * self.hunkbytes, self.hunkbytes, None)
                        for e in struct.unpack(">%dI" % n, raw)]
            return
        mh = self._read_at(self.map_off, 16)
        mapbytes = struct.unpack_from(">I", mh, 0)[0]
        firstoffs = int.from_bytes(mh[4:10], "big")
        mapcrc = struct.unpack_from(">H", mh, 10)[0]
        lengthbits, selfbits, parentbits = mh[12], mh[13], mh[14]
        if lengthbits > 32 or selfbits > 32 or parentbits > 32 or self.map_off + 16 + mapbytes > self.size:
            raise ChdError("chd: bad map header")
        bits = _Bits(self._read_at(self.map_off + 16, mapbytes))
        huff = _Huffman(bits)
        types = []
        last, rep = 0, 0
        for _ in range(n):
            if rep:
                types.append(last)
                rep -= 1
                continue
            if bits.overflow():
                raise ChdError("chd: bad map (truncated)")
            v = huff.decode(bits)
            if v == RLE_SMALL:      # this hunk and 2 + n more repeat the last type
                rep = 2 + huff.decode(bits)
                types.append(last)
            elif v == RLE_LARGE:
                rep = 2 + 16 + (huff.decode(bits) << 4)
                rep += huff.decode(bits)
                types.append(last)
            else:
                last = v
                types.append(v)
        cur = firstoffs
        last_self, last_parent = 0, 0
        entries, rawmap = [], bytearray()
        for h, t in enumerate(types):
            offset, length, crc = cur, 0, 0
            if t in (TYPE_0, TYPE_1, TYPE_2, TYPE_3):
                length = bits.read(lengthbits)
                cur += length
                crc = bits.read(16)
            elif t == NONE:
                length = self.hunkbytes
                cur += length
                crc = bits.read(16)
            elif t == SELF:
                last_self = offset = bits.read(selfbits)
            elif t == PARENT:
                last_parent = offset = bits.read(parentbits)
            elif t in (SELF_0, SELF_1):
                if t == SELF_1:
                    last_self += 1
                t, offset = SELF, last_self
            elif t == PARENT_SELF:
                t = PARENT
                last_parent = offset = h * self.hunkbytes // self.unitbytes
            elif t in (PARENT_0, PARENT_1):
                if t == PARENT_1:
                    last_parent += self.hunkbytes // self.unitbytes
                t, offset = PARENT, last_parent
            else:
                raise ChdError("chd: bad map (hunk type %d)" % t)
            entries.append((t, offset, length, crc))
            rawmap += bytes([t]) + length.to_bytes(3, "big") + (offset & ((1 << 48) - 1)).to_bytes(6, "big") + \
                crc.to_bytes(2, "big")
        if crc16(rawmap) != mapcrc:
            raise ChdError("chd: bad map (CRC)")
        self.map = entries

    # ---- metadata ----
    def _read_tracks(self):
        off, seen, metas = self.meta_off, 0, []
        while off and seen < 1000:
            h = self._read_at(off, 16)
            tag = h[:4]
            length = int.from_bytes(h[5:8], "big")
            nxt = struct.unpack_from(">Q", h, 8)[0]
            if tag in (b"CHT2", b"CHTR"):
                metas.append((tag, self._read_at(off + 16, length).split(b"\0")[0].decode("ascii", "replace")))
            off, seen = nxt, seen + 1
        if not metas:
            raise ChdError("chd: no CD track metadata (not a CD image)")
        self.tracks = []
        for tag, text in metas:
            kv = dict(p.split(":", 1) for p in text.split() if ":" in p)
            try:
                t = Track(int(kv["TRACK"]), kv["TYPE"], int(kv["FRAMES"]), int(kv.get("PREGAP", 0)),
                          kv.get("PGTYPE", ""), int(kv.get("POSTGAP", 0)))
            except (KeyError, ValueError):
                raise ChdError("chd: bad track metadata %r" % text)
            self.tracks.append(t)
        self.tracks.sort(key=lambda t: t.num)
        lba, frame = 0, 0
        for t in self.tracks:
            t.begin = lba
            t.start = lba + t.pregap
            t.data_lba = t.begin if t.pgtype.startswith("V") else t.start
            t.frame = frame
            t.end = t.data_lba + t.frames
            lba = t.end + t.postgap
            frame += (t.frames + TRACK_PADDING - 1) // TRACK_PADDING * TRACK_PADDING

    # ---- hunks ----
    def _codec_name(self, t):
        return CODEC_NAMES.get(self.codecs[t], "0x%08X" % self.codecs[t])

    def hunk(self, h):
        """The decompressed hunk h: frames x (2352 sector bytes) only (the subcode is dropped)."""
        if self._cache[0] == h:
            return self._cache[1]
        if not 0 <= h < self.hunks:
            raise ChdError("chd: hunk %d past the end" % h)
        t, offset, length, _crc = self.map[h]
        nf = self.frames_per_hunk
        if t == -1:
            data = bytes(nf * FRAME)
        elif t == NONE:
            raw = self._read_at(offset, self.hunkbytes)
            data = b"".join(raw[i * FRAME_STORED:i * FRAME_STORED + FRAME] for i in range(nf))
        elif t == SELF:
            if offset >= h:         # only an earlier hunk (libchdr's rule: a crafted map cannot loop)
                raise ChdError("chd: bad map (hunk %d repeats itself)" % h)
            data = self.hunk(offset)
        elif t == PARENT:
            raise ChdError("chd: hunk %d is in the parent CHD (a diff image cannot be read alone)" % h)
        else:
            data = self._decompress(t, self._read_at(offset, length), h)
        self._cache = (h, data)
        return data

    def _decompress(self, t, src, h):
        name = self._codec_name(t)
        nf = self.frames_per_hunk
        if name not in ("cdzl", "cdlz", "cdzs"):
            if name == "cdfl":
                raise ChdError("chd: hunk %d holds a data sector compressed as FLAC (cdfl), which this reader does "
                               "not decode (chdman uses it for audio)" % h)
            raise ChdError("chd: codec %s is not a CD codec" % name)
        ecc_bytes = (nf + 7) // 8
        clen_bytes = 2 if self.hunkbytes < 65536 else 3
        if len(src) < ecc_bytes + clen_bytes:
            raise ChdError("chd: hunk %d truncated" % h)
        base_len = int.from_bytes(src[ecc_bytes:ecc_bytes + clen_bytes], "big")
        start = ecc_bytes + clen_bytes
        if start + base_len > len(src):
            raise ChdError("chd: hunk %d truncated" % h)
        comp = src[start:start + base_len]
        want = nf * FRAME
        try:
            if name == "cdzl":
                d = zlib.decompressobj(-15)
                data = d.decompress(comp, want)
            elif name == "cdlz":
                dict_size = max(1 << 12, 1 << (self.hunkbytes - 1).bit_length())
                d = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[
                    {"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": 3, "lp": 0, "pb": 2}])
                data = d.decompress(comp, want)
            else:
                try:
                    from compression import zstd   # CPython 3.14+
                except ImportError:
                    raise ChdError("chd: hunk %d is zstd-compressed (cdzs), which needs Python 3.14 "
                                   "(compression.zstd); this is %s" % (h, _pyver()))
                try:
                    data = zstd.ZstdDecompressor().decompress(comp, want)
                except zstd.ZstdError as e:
                    raise ChdError("chd: hunk %d does not decompress (cdzs: %s)" % (h, e))
        except (zlib.error, lzma.LZMAError) as e:
            raise ChdError("chd: hunk %d does not decompress (%s: %s)" % (h, name, e))
        if len(data) != want:
            raise ChdError("chd: hunk %d decompresses to %d bytes, not %d (%s)" % (h, len(data), want, name))
        if any(src[:ecc_bytes]):
            out = bytearray(data)
            for i in range(nf):
                if src[i // 8] & (1 << (i % 8)):
                    out[i * FRAME:i * FRAME + 12] = SYNC
            data = bytes(out)
        return data

    # ---- sectors ----
    def track_of(self, lba):
        for t in self.tracks:
            if t.begin <= lba < t.end:
                return t
        return None

    def frame(self, lba):
        """The 2352 stored bytes of absolute LBA lba (None in a pregap that is not stored)."""
        t = self.track_of(lba)
        if t is None:
            raise ChdError("sector %d past the end of the disc" % lba)
        if lba < t.data_lba:
            return None
        f = t.frame + (lba - t.data_lba)
        hk, i = divmod(f, self.frames_per_hunk)
        return self.hunk(hk)[i * FRAME:(i + 1) * FRAME]

    def user_data(self, lba):
        """The 2048 user bytes of data sector lba (mode 1 / mode 2 form 1, raw or cooked tracks)."""
        t = self.track_of(lba)
        if t is None:
            raise ChdError("sector %d past the end of the disc" % lba)
        if t.audio:
            raise ChdError("sector %d is on an audio track" % lba)
        s = self.frame(lba)
        if s is None:
            return bytes(2048)
        if t.type in ("MODE1_RAW", "MODE2_RAW"):
            off = 24 if s[15] == 2 else 16
            return s[off:off + 2048]
        if t.type in ("MODE2", "MODE2_FORM_MIX"):
            return s[8:8 + 2048]
        return s[:2048]       # MODE1, MODE2_FORM1: 2048 bytes stored


def _pyver():
    import sys
    return "Python %d.%d" % sys.version_info[:2]


def is_chd(path):
    try:
        with open(path, "rb") as f:
            return f.read(8) == b"MComprHD"
    except OSError:
        return False


# ---- writing (tests only): the shapes chdman writes ------------------------------------------------------------

class _BitWriter:
    def __init__(self):
        self.v, self.n = 0, 0

    def write(self, value, bits):
        self.v = (self.v << bits) | (value & ((1 << bits) - 1))
        self.n += bits

    def data(self):
        pad = (-self.n) % 8
        return (self.v << pad).to_bytes((self.n + pad) // 8, "big") if self.n else b""


def write_cd(path, tracks, codec="cdlz", hunk_frames=8, self_ref=True, strip_sync=False, raw_hunks=()):
    """Writes a CHD v5 CD image (tests): tracks = [(type, [2352-byte frames]), ...] ('MODE2_RAW', 'MODE1_RAW' or
    'AUDIO'; audio frames given little-endian, stored big-endian as chdman does). codec: 'none' (uncompressed map),
    or a CD codec among 'cdzl', 'cdlz', 'cdzs' (zstd, Python 3.14+), with a Huffman-coded map (every type coded on 4
    bits, run lengths for repeats), hunks that do not shrink stored raw (type NONE) and repeated hunks as SELF
    references when self_ref. strip_sync: the sync of every frame that has one is dropped and flagged in the hunk's
    ECC bitmap (libchdr then regenerates the ECC too, so only for sectors whose ECC is right, or for this reader).
    raw_hunks: hunk numbers stored raw whatever their compression gives."""
    hunkbytes = hunk_frames * FRAME_STORED
    frames, metas = [], []
    for i, (ttype, fr) in enumerate(tracks):
        metas.append("TRACK:%d TYPE:%s SUBTYPE:NONE FRAMES:%d PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:0"
                     % (i + 1, ttype, len(fr)))
        for s in fr:
            assert len(s) == FRAME
            if ttype == "AUDIO":
                b = bytearray(s)
                b[0::2], b[1::2] = s[1::2], s[0::2]
                s = bytes(b)
            frames.append(s)
        while len(frames) % TRACK_PADDING:
            frames.append(bytes(FRAME))
    while len(frames) % hunk_frames:
        frames.append(bytes(FRAME))
    nh = len(frames) // hunk_frames
    hunks = []
    for h in range(nh):
        sect = b"".join(frames[h * hunk_frames:(h + 1) * hunk_frames])
        hunks.append(sect)
    # metadata
    meta = bytearray()
    meta_entries = []
    for m in metas:
        meta_entries.append(m.encode() + b"\0")
    head_len = 124
    stored = []          # (type, payload)
    if codec == "none":
        map_bytes = 4 * nh
        meta_off = head_len + map_bytes
        off = meta_off
        for i, e in enumerate(meta_entries):
            nxt = off + 16 + len(e) if i + 1 < len(meta_entries) else 0
            meta += b"CHT2" + bytes([1]) + len(e).to_bytes(3, "big") + struct.pack(">Q", nxt) + e
            off += 16 + len(e)
        data_off = (meta_off + len(meta) + hunkbytes - 1) // hunkbytes * hunkbytes
        body = bytearray()
        mp = bytearray()
        for h, sect in enumerate(hunks):
            mp += struct.pack(">I", data_off // hunkbytes + h)
            raw = b"".join(sect[i * FRAME:(i + 1) * FRAME] + bytes(SUBCODE) for i in range(hunk_frames))
            body += raw
        header = _header(hunkbytes, nh, [0, 0, 0, 0], head_len, meta_off)
        out = header + mp + meta
        out += bytes(data_off - len(out)) + body
        with open(path, "wb") as f:
            f.write(out)
        return
    codec_tag = _tag(codec)
    seen = {}
    for h, sect in enumerate(hunks):
        if self_ref and sect in seen:
            stored.append((SELF, seen[sect], 0))
            continue
        seen.setdefault(sect, h)
        nf = hunk_frames
        clen_bytes = 2 if hunkbytes < 65536 else 3
        # strip the sync of frames that have one (the ECC flag): the reader restores it
        ecc = bytearray((nf + 7) // 8)
        body = bytearray(sect)
        for i in range(nf):
            if strip_sync and body[i * FRAME:i * FRAME + 12] == SYNC:
                ecc[i // 8] |= 1 << (i % 8)
                body[i * FRAME:i * FRAME + 12] = bytes(12)
        if codec == "cdzl":
            c = zlib.compressobj(9, zlib.DEFLATED, -15)
            base = c.compress(bytes(body)) + c.flush()
            sub = zlib.compressobj(9, zlib.DEFLATED, -15)
            subc = sub.compress(bytes(nf * SUBCODE)) + sub.flush()
        elif codec == "cdlz":
            dict_size = max(1 << 12, 1 << (hunkbytes - 1).bit_length())
            base = lzma.compress(bytes(body), format=lzma.FORMAT_RAW, filters=[
                {"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": 3, "lp": 0, "pb": 2}])
            sub = zlib.compressobj(9, zlib.DEFLATED, -15)
            subc = sub.compress(bytes(nf * SUBCODE)) + sub.flush()
        elif codec == "cdzs":
            from compression import zstd
            base = zstd.compress(bytes(body))
            subc = zstd.compress(bytes(nf * SUBCODE))
        else:
            raise ValueError(codec)
        payload = bytes(ecc) + len(base).to_bytes(clen_bytes, "big") + base + subc
        raw = b"".join(sect[i * FRAME:(i + 1) * FRAME] + bytes(SUBCODE) for i in range(hunk_frames))
        if len(payload) >= hunkbytes or h in raw_hunks:
            stored.append((NONE, raw, crc16(raw)))
        else:
            stored.append((TYPE_0, payload, crc16(raw)))
    # layout: header, metadata, hunk data, map at the end (as chdman)
    meta_off = head_len
    off = meta_off
    for i, e in enumerate(meta_entries):
        nxt = off + 16 + len(e) if i + 1 < len(meta_entries) else 0
        meta += b"CHT2" + bytes([1]) + len(e).to_bytes(3, "big") + struct.pack(">Q", nxt) + e
        off += 16 + len(e)
    data_off = head_len + len(meta)
    body = bytearray()
    entries = []
    for t, p, crc in stored:
        if t == SELF:
            entries.append((SELF, p, 0, 0))
        else:
            entries.append((t, data_off + len(body), len(p), crc))
            body += p
    map_off = data_off + len(body)
    firstoffs = data_off
    maxlen = max([e[2] for e in entries if e[0] == TYPE_0] or [0])
    lengthbits = max(1, maxlen.bit_length())
    selfbits = max(1, (nh - 1).bit_length())
    bw = _BitWriter()
    for _ in range(16):          # code-length table: every type 4 bits long (numbits 4: 4 = « 4 »)
        bw.write(4, 4)
    # types, runs of 3..18 hunks of the last type as RLE_SMALL (this hunk and 2 + n more)
    types = [e[0] for e in entries]
    i, last = 0, 0
    while i < len(types):
        run = 0
        while i + run < len(types) and types[i + run] == last and run < 18:
            run += 1
        if run >= 3:
            bw.write(RLE_SMALL, 4)
            bw.write(run - 3, 4)
            i += run
            continue
        bw.write(types[i], 4)
        last = types[i]
        i += 1
    rawmap = bytearray()
    for t, offset, length, crc in entries:
        if t == TYPE_0:
            bw.write(length, lengthbits)
            bw.write(crc, 16)
        elif t == NONE:
            bw.write(crc, 16)
        else:
            bw.write(offset, selfbits)
        rawmap += bytes([t]) + length.to_bytes(3, "big") + offset.to_bytes(6, "big") + crc.to_bytes(2, "big")
    mapdata = bw.data()
    maphead = struct.pack(">I", len(mapdata)) + firstoffs.to_bytes(6, "big") + struct.pack(">H", crc16(rawmap)) + \
        bytes([lengthbits, selfbits, 0, 0])
    header = _header(hunkbytes, nh, [codec_tag, _tag("cdzl"), 0, 0], map_off, meta_off)
    out = header + meta + body + maphead + mapdata + bytes(16)
    with open(path, "wb") as f:
        f.write(out)


def _header(hunkbytes, nh, codecs, map_off, meta_off):
    h = bytearray(124)
    h[0:8] = b"MComprHD"
    struct.pack_into(">II", h, 8, 124, 5)
    struct.pack_into(">4I", h, 16, *codecs)
    struct.pack_into(">QQQ", h, 32, nh * hunkbytes, map_off, meta_off)
    struct.pack_into(">II", h, 56, hunkbytes, FRAME_STORED)
    return bytes(h)


def main(argv=None):
    """`python3 -m ps1_tools.chd info X.chd` (tracks, codecs); `python3 -m ps1_tools.chd write [--codec C]
    [--strip-sync] OUT.chd TRACK.bin [audio.bin ...]` (tests: a raw MODE2 data track, then audio tracks)."""
    import argparse
    ap = argparse.ArgumentParser(prog="ps1_tools.chd")
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("info")
    i.add_argument("chd")
    w = sub.add_parser("write")
    w.add_argument("--codec", default="cdlz", choices=("none", "cdzl", "cdlz", "cdzs"))
    w.add_argument("--strip-sync", action="store_true")
    w.add_argument("out")
    w.add_argument("data")
    w.add_argument("audio", nargs="*")
    a = ap.parse_args(argv)
    if a.cmd == "info":
        c = Chd(a.chd)
        try:
            print("hunks %d x %d bytes, codecs %s" % (c.hunks, c.hunkbytes,
                  " ".join(CODEC_NAMES.get(x, "%08X" % x) for x in c.codecs if x)))
            for t in c.tracks:
                print("track %d %s lba %d..%d (index 01 at %d)" % (t.num, t.type, t.begin, t.end, t.start))
        finally:
            c.close()
        return 0

    def frames_of(path):
        with open(path, "rb") as f:
            data = f.read()
        if len(data) % FRAME:
            data += bytes(FRAME - len(data) % FRAME)
        return [data[i:i + FRAME] for i in range(0, len(data), FRAME)]
    tracks = [("MODE2_RAW", frames_of(a.data))] + [("AUDIO", frames_of(p)) for p in a.audio]
    write_cd(a.out, tracks, codec=a.codec, strip_sync=a.strip_sync)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
