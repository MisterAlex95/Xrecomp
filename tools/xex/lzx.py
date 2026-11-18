"""LZX decompression (XBA-270): the "normal" compression of XEX2 images, written from the format's description.

    out = decompress(stream, window_bits, out_size)                       # XEX / CAB / CHM bitstream
    out = decompress(stream, 15, out_size, flavour="wim")                 # a WIM resource chunk (wimlib)
    stream = xex_deblock(payload, first_block_size, first_block_hash)     # XEX "normal": the block chain -> stream

Sources: Microsoft's [MS-PATCH] « LZX DELTA Compression and Decompression » open specification (the bitstream: 16-bit
little-endian words read most-significant bit first, block types, pretree-coded tree deltas, repeated offsets, aligned
offsets, E8 translation) and the public descriptions of the XEX2 container (Free60, Xenia's xex_module.cc, BSD: the
block chain and 2-byte chunk sizes). No code was copied from libmspack (LGPL) or wimlib (LGPL / GPL); both were only
used as tools to make and check the test vectors (tools/xex/lzx_vectors/README.md).

Flavours (the same LZX core; they differ in their framing):
- "cab" (XEX, CAB, CHM): at the start and after each reset, one bit says whether E8 translation is on (then a 32-bit
  translation size); block sizes are 24 bits; output comes in 32 KB frames, after each one the input is realigned to
  16 bits and E8 translation (when on) is undone frame by frame; a token may not cross a frame; `reset_interval`
  frames (0: never, as XEX uses) reset the state.
- "wim": no E8 header bit (translation always on, size 12000000, over the whole chunk), block sizes are one bit
  (set: 32768) or 16 bits (24 from a 64 KB window), no frames.

A malformed stream raises LzxError (never loops: every token makes output, and reading past the end of the input
raises), so callers may try it on data decrypted with a wrong key.
"""

import hashlib
import struct

FRAME = 32768
MIN_MATCH = 2
NUM_CHARS = 256
LEN_SYMS = 249
PRETREE_SYMS = 20
ALIGNED_SYMS = 8
VERBATIM, ALIGNED, UNCOMPRESSED = 1, 2, 3
BLOCK_NAMES = {VERBATIM: "verbatim", ALIGNED: "aligned", UNCOMPRESSED: "uncompressed"}
WIM_E8_SIZE = 12000000

# extra bits and base of each position slot
EXTRA = [0 if s < 4 else min((s - 2) // 2, 17) for s in range(51)]
BASE = [0]
for _s in range(50):
    BASE.append(BASE[-1] + (1 << EXTRA[_s]))
del _s
SLOTS = {15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50}


class LzxError(ValueError):
    pass


def window_bits_of(size):
    """The window size of an XEX header (a power of two, 32 KB .. 2 MB) -> bits."""
    b = size.bit_length() - 1
    if size <= 0 or size != 1 << b or b not in SLOTS:
        raise LzxError("LZX window size 0x%X is not a power of two from 32 KB to 2 MB" % size)
    return b


def _table(lens, nsyms):
    """A 16-bit lookup table of the canonical code with these lengths: entry (symbol << 5) | length, -1 unused.
    None when every length is 0 (an empty tree, valid as long as it is never used)."""
    if not any(lens):
        return None
    table = [-1] * 65536
    code = 0
    for length in range(1, 17):
        for sym in range(nsyms):
            if lens[sym] == length:
                span = 1 << (16 - length)
                start = code << (16 - length)
                if start + span > 65536:
                    raise LzxError("over-subscribed Huffman code")
                table[start:start + span] = [(sym << 5) | length] * span
                code += 1
        code <<= 1
    return table


class _Bits:
    __slots__ = ("w", "pos", "buf", "n", "limit")

    def __init__(self, data, byte_pos=0):
        if len(data) & 1:
            data = bytes(data) + b"\0"
        self.w = struct.unpack("<%dH" % (len(data) // 2), data)
        self.limit = len(self.w) + 2  # libmspack-style tolerance: a couple of zero words past the end
        self.seek_byte(byte_pos)

    def seek_byte(self, b):
        self.pos, self.buf, self.n = b // 2, 0, 0

    def bitpos(self):
        return self.pos * 16 - self.n

    def ensure(self, k):
        while self.n < k:
            p = self.pos
            if p >= len(self.w):
                if p >= self.limit:
                    raise LzxError("LZX stream ends before the output is complete")
                v = 0
            else:
                v = self.w[p]
            self.buf = (self.buf << 16) | v
            self.pos = p + 1
            self.n += 16

    def read(self, k):
        if k == 0:
            return 0
        self.ensure(k)
        self.n -= k
        v = self.buf >> self.n
        self.buf &= (1 << self.n) - 1
        return v

    def sym(self, table):
        if table is None:
            raise LzxError("symbol read from an empty Huffman tree")
        self.ensure(16)
        e = table[(self.buf >> (self.n - 16)) & 0xFFFF]
        if e < 0:
            raise LzxError("invalid Huffman code")
        self.n -= e & 31
        self.buf &= (1 << self.n) - 1
        return e >> 5

    def align16(self):
        """Drop bits to the next 16-bit boundary (none when already there)."""
        p = self.bitpos()
        p = (p + 15) & ~15
        self.pos, self.buf, self.n = p // 16, 0, 0


def _read_lengths(bits, lens, first, last):
    """Tree lengths lens[first:last] as deltas through a pretree."""
    pre = [bits.read(4) for _ in range(PRETREE_SYMS)]
    pt = _table(pre, PRETREE_SYMS)
    x = first
    while x < last:
        z = bits.sym(pt)
        if z == 17:
            n = bits.read(4) + 4
            if x + n > last:
                raise LzxError("pretree run past the end of the tree")
            lens[x:x + n] = [0] * n
            x += n
        elif z == 18:
            n = bits.read(5) + 20
            if x + n > last:
                raise LzxError("pretree run past the end of the tree")
            lens[x:x + n] = [0] * n
            x += n
        elif z == 19:
            n = bits.read(1) + 4
            if x + n > last:
                raise LzxError("pretree run past the end of the tree")
            v = (lens[x] - bits.sym(pt)) % 17
            lens[x:x + n] = [v] * n
            x += n
        else:
            lens[x] = (lens[x] - z) % 17
            x += 1


def decompress(data, window_bits, out_size, flavour="cab", reset_interval=0, stats=None):
    """The decompressed bytes (exactly out_size). stats: an optional dict filled with block counts per type, frames,
    matches by kind (literal / repeat / new), whether E8 translation was on and (cab) where each frame ends in the
    input (byte offsets: where a CAB would cut its data blocks)."""
    if window_bits not in SLOTS:
        raise LzxError("LZX window of 2^%d bytes not supported (15..21)" % window_bits)
    if flavour not in ("cab", "wim"):
        raise ValueError("flavour: cab or wim")
    cab = flavour == "cab"
    nslots = SLOTS[window_bits]
    nmain = NUM_CHARS + 8 * nslots
    wsize = 1 << window_bits
    bits = _Bits(data)
    out = bytearray(out_size + 257)  # room for a last match running past the end (cut below)
    st = {"blocks": {}, "frames": 0, "literals": 0, "repeat_matches": 0, "matches": 0, "e8": False,
          "aligned_offsets": 0, "frame_ends": []}
    main_lens = [0] * nmain
    len_lens = [0] * LEN_SYMS
    r0 = r1 = r2 = 1
    e8_size = WIM_E8_SIZE if not cab else 0
    e8_on = not cab
    e8_seen = False
    e8_frames = []  # (start, end, size) of the frames to translate
    block_type = 0
    block_left = 0
    block_len = 0
    ub = 0
    main_t = len_t = al_t = None
    pos = 0
    frame = 0
    frame_size = FRAME if cab else max(out_size, 1)
    while pos < out_size:
        if frame == 0 or (cab and reset_interval and frame % reset_interval == 0):
            if block_left:
                raise LzxError("reset inside a block")
            r0 = r1 = r2 = 1
            main_lens = [0] * nmain
            len_lens = [0] * LEN_SYMS
            block_type = 0
            if cab:
                e8_on = bool(bits.read(1))
                if e8_on:
                    hi = bits.read(16)
                    e8_size = (hi << 16) | bits.read(16)
        frame_start = pos
        frame_end = min(pos + frame_size, out_size)
        while pos < frame_end:
            if block_left == 0:
                block_type = bits.read(3)
                if cab:
                    hi = bits.read(16)
                    block_len = (hi << 8) | bits.read(8)
                elif bits.read(1):
                    block_len = FRAME
                else:
                    block_len = bits.read(16)
                    if window_bits >= 16:
                        block_len = (block_len << 8) | bits.read(8)
                if block_len == 0:
                    raise LzxError("empty LZX block")
                block_left = block_len
                st["blocks"][BLOCK_NAMES.get(block_type, "?")] = \
                    st["blocks"].get(BLOCK_NAMES.get(block_type, "?"), 0) + 1
                if block_type == ALIGNED:
                    al_t = _table([bits.read(3) for _ in range(ALIGNED_SYMS)], ALIGNED_SYMS)
                if block_type in (VERBATIM, ALIGNED):
                    _read_lengths(bits, main_lens, 0, NUM_CHARS)
                    _read_lengths(bits, main_lens, NUM_CHARS, nmain)
                    main_t = _table(main_lens, nmain)
                    if main_t is None:
                        raise LzxError("empty main tree")
                    if main_lens[0xE8]:
                        e8_seen = True
                    _read_lengths(bits, len_lens, 0, LEN_SYMS)
                    len_t = _table(len_lens, LEN_SYMS)
                elif block_type == UNCOMPRESSED:
                    e8_seen = True
                    # 1 to 16 bits of padding to the next 16-bit boundary, then R0 R1 R2 (little-endian)
                    p = bits.bitpos()
                    p = p + 16 if p % 16 == 0 else (p + 15) & ~15
                    b = p // 8
                    if b + 12 > len(data):
                        raise LzxError("uncompressed block header past the end of the input")
                    r0, r1, r2 = struct.unpack_from("<III", data, b)
                    if not (r0 and r1 and r2):
                        raise LzxError("uncompressed block with a zero repeated offset")
                    ub = b + 12
                else:
                    raise LzxError("invalid LZX block type %d" % block_type)
            run = min(block_left, frame_end - pos)
            if block_type == UNCOMPRESSED:
                if ub + run > len(data):
                    raise LzxError("uncompressed block past the end of the input")
                out[pos:pos + run] = data[ub:ub + run]
                pos += run
                ub += run
                block_left -= run
                if block_left == 0:
                    ub += block_len & 1  # an odd-sized block is padded to 16 bits
                    bits.seek_byte(ub)
                continue
            end = pos + run
            aligned = block_type == ALIGNED
            while pos < end:
                s = bits.sym(main_t)
                if s < NUM_CHARS:
                    out[pos] = s
                    pos += 1
                    st["literals"] += 1
                    continue
                s -= NUM_CHARS
                length = s & 7
                if length == 7:
                    length += bits.sym(len_t)
                length += MIN_MATCH
                slot = s >> 3
                if slot < 3:
                    st["repeat_matches"] += 1
                    if slot == 0:
                        off = r0
                    elif slot == 1:
                        off = r1
                        r1 = r0
                        r0 = off
                    else:
                        off = r2
                        r2 = r0
                        r0 = off
                else:
                    st["matches"] += 1
                    extra = EXTRA[slot]
                    off = BASE[slot] - 2
                    if aligned and extra >= 3:
                        off += (bits.read(extra - 3) << 3) + bits.sym(al_t)
                        st["aligned_offsets"] += 1
                    else:
                        off += bits.read(extra)
                    r2 = r1
                    r1 = r0
                    r0 = off
                if pos + length > end and (end < out_size or length > block_left - (pos - end + run)):
                    # a match may only run past the end of the output (some encoders pad the last frame, and
                    # decoders stop at the size they were given), never past a frame or its block
                    raise LzxError("match runs past the end of its block or frame")
                if off > pos or off > wsize:
                    raise LzxError("match offset %d before the start of the output" % off)
                src = pos - off
                if off >= length:
                    out[pos:pos + length] = out[src:src + length]
                else:
                    for k in range(length):
                        out[pos + k] = out[src + k]
                pos += length
            block_left -= run
        if cab:
            if e8_on and frame < 32768:
                e8_frames.append((frame_start, pos, e8_size))
            if block_type != UNCOMPRESSED:
                bits.align16()  # (an uncompressed block keeps its byte position across frames)
            st["frame_ends"].append(ub if block_type == UNCOMPRESSED and block_left else bits.bitpos() // 8)
        frame += 1
    st["frames"] = frame
    del out[out_size:]
    pos = min(pos, out_size)
    raw = bytes(out)
    if not cab:
        if e8_seen and out_size > 10:
            out = _e8(out, raw, 0, out_size, WIM_E8_SIZE)
            st["e8"] = True
    elif e8_seen and e8_frames:
        st["e8"] = True
        for a, b, size in e8_frames:
            if b - a > 10:
                out = _e8(out, raw, a, b, size)
    if stats is not None:
        stats.update(st)
    return bytes(out)


def _e8(out, raw, start, end, size):
    """Undo E8 translation over raw[start:end] (the last 10 bytes left alone), writing into out."""
    i = start
    stop = end - 10
    while i < stop:
        i = raw.find(b"\xE8", i, stop)
        if i < 0:
            break
        a = struct.unpack_from("<i", raw, i + 1)[0]
        if -i <= a < size:
            rel = a - i if a >= 0 else a + size
            struct.pack_into("<i", out, i + 1, rel - (1 << 32) if rel >= 1 << 31 else rel)
        i += 5
    return out


def xex_deblock(payload, block_size, block_hash=None, check=True):
    """The LZX stream of an XEX2 "normal" image: a chain of blocks, each starting with the next block's size (32-bit
    big-endian) and SHA-1 (20 bytes), then chunks (16-bit big-endian size, data) up to a zero size; the first
    block's size and SHA-1 are in the file format header. check: compare each block's SHA-1 when one is known."""
    out = bytearray()
    p = 0
    size, digest = block_size, block_hash
    n = 0
    while size:
        if p + size > len(payload) or size < 24:
            raise LzxError("XEX compressed block %d runs past the data (size 0x%X at 0x%X)" % (n, size, p))
        blk = payload[p:p + size]
        if check and digest is not None and any(digest) and hashlib.sha1(blk).digest() != bytes(digest):
            raise LzxError("XEX compressed block %d: SHA-1 mismatch" % n)
        nsize = struct.unpack_from(">I", blk, 0)[0]
        ndigest = blk[4:24]
        q = 24
        while True:
            if q + 2 > size:
                raise LzxError("XEX compressed block %d: chunk list runs past the block" % n)
            c = (blk[q] << 8) | blk[q + 1]
            q += 2
            if c == 0:
                break
            if q + c > size:
                raise LzxError("XEX compressed block %d: chunk runs past the block" % n)
            out += blk[q:q + c]
            q += c
        p += size
        size, digest = nsize, ndigest
        n += 1
    return bytes(out)


def xex_reblock(stream_chunks, block_limit=0x8000):
    """The inverse of xex_deblock, for tests: chunks of an LZX stream -> (payload, first size, first SHA-1). Each
    block holds as many chunks as fit in block_limit bytes."""
    blocks = []
    cur = []
    for c in stream_chunks:
        if len(c) > 0xFFFF:
            raise ValueError("chunk too large")
        if cur and 24 + sum(2 + len(x) for x in cur) + 2 + len(c) + 2 > block_limit:
            blocks.append(cur)
            cur = []
        cur.append(c)
    if cur:
        blocks.append(cur)
    bodies = [b"".join(struct.pack(">H", len(c)) + c for c in b) + b"\0\0" for b in blocks]
    out = []
    nsize, ndigest = 0, bytes(20)
    for body in reversed(bodies):
        blk = struct.pack(">I", nsize) + ndigest + body
        out.append(blk)
        nsize, ndigest = len(blk), hashlib.sha1(blk).digest()
    return b"".join(reversed(out)), nsize, ndigest


def store(data, block=FRAME):
    """An LZX stream (cab flavour, E8 off) holding `data` in uncompressed blocks of `block` bytes: what synth.py
    puts in a "normal" XEX2. (Our own writer; gen.sh checks its output with cabextract.)"""
    words = []
    acc, n = 0, 0

    def put(v, k):
        nonlocal acc, n
        acc = (acc << k) | v
        n += k
        while n >= 16:
            n -= 16
            words.append((acc >> n) & 0xFFFF)
        acc &= (1 << n) - 1

    out = bytearray()
    put(0, 1)  # no E8 translation
    for o in range(0, max(len(data), 1), block):
        chunk = data[o:o + block]
        put(UNCOMPRESSED, 3)
        put(len(chunk) >> 8, 16)
        put(len(chunk) & 0xFF, 8)
        if n == 0:
            put(0, 16)  # 1 to 16 bits of padding
        else:
            put(0, 16 - n)
        out += struct.pack("<%dH" % len(words), *words)
        words.clear()
        out += struct.pack("<III", 1, 1, 1) + chunk + bytes(len(chunk) & 1)
    return bytes(out)
