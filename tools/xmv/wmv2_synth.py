"""
Synthetic WMV2 bitstreams for the features FFmpeg's wmv2 encoder never uses
(XBA-70): mspel motion compensation, the adaptive block transform (8x4 /
4x8), the MPEG / row / column skip maps, per-macroblock run/level tables, the
top-left MV predictor flag, several slices, intra macroblocks in P frames.

The streams are random but valid syntax: every macroblock, vector and
coefficient is drawn at random and written with the format's own code tables
(read from src/video/xmv/wmv2_tables.c). The pictures are noise; what matters
is that two decoders agree on them. tests/xmv_decode compares the runtime's
decoder with FFmpeg's decode of the same file (bit-exact mode), as for the
encoder-made fixtures (xmv_mux.py).

Motion vectors are kept inside the picture (with the filter taps' margin):
outside it FFmpeg's decoder reads one chroma column past the coded width in
some cases, which is its artefact, not the format's.
"""

import os
import random
import re

_TABLES = None


def tables():
    global _TABLES
    if _TABLES is None:
        here = os.path.dirname(os.path.abspath(__file__))
        path = os.path.join(here, "..", "..", "src", "video", "xmv", "wmv2_tables.c")
        text = re.sub(r"/\*.*?\*/", "", open(path).read(), flags=re.S)
        _TABLES = {}
        for m in re.finditer(r"const \w+ (\w+)\[\d+\] = \{(.*?)\};", text, flags=re.S):
            _TABLES[m.group(1)] = [int(v, 0) for v in re.findall(r"-?\d+", m.group(2))]
    return _TABLES


# (n, last) of the six run/level tables
RL_SHAPE = [(132, 85), (185, 119), (102, 67), (148, 81), (168, 99), (102, 58)]


class BitWriter:
    def __init__(self):
        self.bits = []

    def put(self, value, n):
        for k in range(n - 1, -1, -1):
            self.bits.append((value >> k) & 1)

    def put012(self, v):
        if v == 0:
            self.put(0, 1)
        else:
            self.put(1, 1)
            self.put(v - 1, 1)

    def bytes(self):
        b = self.bits + [0] * (-len(self.bits) % 32)
        return bytes(int("".join(map(str, b[i:i + 8])), 2) for i in range(0, len(b), 8))


def _mid(a, b, c):
    return sorted((a, b, c))[1]


class Synth:
    def __init__(self, width, height, seed, mspel_bit=1, abt_flag=1, tlmv=0, per_mb_rl_bit=1,
                 slices=1, loop_filter=0, x8_allowed=0):
        self.t = tables()
        self.rng = random.Random(seed)
        self.w, self.h = width, height
        self.mbw, self.mbh = (width + 15) // 16, (height + 15) // 16
        self.cw, self.ch = self.mbw * 16, self.mbh * 16
        self.mspel_bit, self.abt_flag, self.tlmv = mspel_bit, abt_flag, tlmv
        self.per_mb_rl_bit, self.slices, self.loop_filter = per_mb_rl_bit, slices, loop_filter
        self.x8_allowed = x8_allowed
        self.slice_height = self.mbh // slices if slices else self.mbh
        self.w8 = 2 * self.mbw + 1
        self.mv = {}            # b8 index -> (x, y); the extra column and the top border read as 0
        self.coded = {}         # luma coded-block flags, bordered grid
        self.abt_type = 0
        self.per_mb_abt = 0
        self.no_rounding = 1

    def header_word(self):
        """The sequence header in XMV's packing."""
        return (self.mspel_bit | (self.loop_filter << 1) | (self.abt_flag << 2) | (self.x8_allowed << 3) |
                (self.tlmv << 4) | (self.per_mb_rl_bit << 5) | ((self.slices & 7) << 6))

    # ── code tables ──
    def _vlc(self, bw, pairs, sym):
        code, ln = pairs[2 * sym], pairs[2 * sym + 1]
        assert ln > 0, sym
        bw.put(code, ln)

    def _rl(self, k):
        n, last = RL_SHAPE[k]
        return (self.t["wmv2_rl%d_vlc" % k], self.t["wmv2_rl%d_run" % k], self.t["wmv2_rl%d_level" % k], n, last)

    def _coeffs(self, bw, k, start, limit):
        """A run of coefficients from position start+1 on, ending at or before limit."""
        vlc, run, level, n, last = self._rl(k)
        r = self.rng
        i = start
        count = r.randint(1, 5)
        for c in range(count):
            final = c == count - 1
            s = None
            for _ in range(200):
                q = r.randrange(last, n) if final else r.randrange(0, last)
                # Leave room for a closing code after a non-final one.
                if i + run[q] + 1 <= (limit if final else limit - 1):
                    s = q
                    break
            if s is None:
                # Nothing short enough: end with the shortest last code
                # (every table has a last code of run 0).
                s = min(range(last, n), key=lambda q: run[q])
                final = True
            self._vlc(bw, vlc, s)
            bw.put(r.randint(0, 1), 1)          # sign
            i += run[s] + 1
            if final:
                break

    def _intra_block(self, bw, n, dc_table, rl_luma, rl_chroma, coded):
        r = self.rng
        chroma = n >= 4
        pairs = self.t["wmv2_dc_vlc"][(dc_table * 2 + chroma) * 240:(dc_table * 2 + chroma + 1) * 240]
        diff = r.randint(-3, 3)
        self._vlc(bw, pairs, abs(diff))
        if diff:
            bw.put(1 if diff < 0 else 0, 1)
        if coded:
            self._coeffs(bw, (3 + rl_chroma) if chroma else rl_luma, 0, 62)

    def _set_mv(self, mbx, mby, v):
        i = 2 * mby * self.w8 + 2 * mbx
        for o in (0, 1, self.w8, self.w8 + 1):
            self.mv[i + o] = v

    def _getmv(self, i):
        return self.mv.get(i, (0, 0))

    def _mv_ok(self, mbx, mby, mx, my, mspel, hshift_possible):
        """The prediction reads only inside the picture (with filter margins)."""
        sx, sy = mbx * 16 + (mx >> 1), mby * 16 + (my >> 1)
        if not (2 <= sx and sx + 16 + 3 <= self.cw and 2 <= sy and sy + 16 + 3 <= self.ch):
            return False
        if mspel:
            cx, cy = mbx * 8 + (mx >> 2), mby * 8 + (my >> 2)
        else:
            cx, cy = sx >> 1, sy >> 1
        return 1 <= cx and cx + 9 + 1 <= self.cw // 2 and 1 <= cy and cy + 9 + 1 <= self.ch // 2

    # ── frames ──
    def i_frame(self, q):
        r = self.rng
        bw = BitWriter()
        bw.put(0, 1)                     # I
        bw.put(0, 7)
        bw.put(q, 5)
        if self.x8_allowed:
            bw.put(0, 1)                 # j_type: the usual intra coding
        per_mb_rl = r.randint(0, 1) if self.per_mb_rl_bit else 0
        if self.per_mb_rl_bit:
            bw.put(per_mb_rl, 1)
        rl_chroma = rl_luma = 0
        if not per_mb_rl:
            rl_chroma, rl_luma = r.randint(0, 2), r.randint(0, 2)
            bw.put012(rl_chroma)
            bw.put012(rl_luma)
        dc_table = r.randint(0, 1)
        bw.put(dc_table, 1)
        self.no_rounding = 1
        W = self.w8
        for mby in range(self.mbh):
            for mbx in range(self.mbw):
                code = r.randrange(64)
                self._vlc(bw, self.t["wmv2_mb_intra"], code)
                cbp = 0
                for n in range(6):
                    val = (code >> (5 - n)) & 1
                    if n < 4:
                        bi = (2 * mby + (n >> 1) + 1) * W + 2 * mbx + (n & 1) + 1
                        a, b, c = self.coded.get(bi - 1, 0), self.coded.get(bi - 1 - W, 0), self.coded.get(bi - W, 0)
                        val ^= a if b == c else c
                        self.coded[bi] = val
                    cbp |= val << (5 - n)
                bw.put(r.randint(0, 1), 1)                       # ac_pred
                if per_mb_rl and cbp:
                    rl_luma = rl_chroma = r.randint(0, 2)
                    bw.put012(rl_luma)
                for n in range(6):
                    self._intra_block(bw, n, dc_table, rl_luma, rl_chroma, (cbp >> (5 - n)) & 1)
                self._set_mv(mbx, mby, (0, 0))
        return bw.bytes()

    def x8_i_frame(self, q, nbytes=96):
        """An I frame coded with the X8 intra coder (j_type 1): the picture header, then random bytes
        standing for the X8 data. Not a decodable picture: it is what tests the runtime's detection of X8
        frames (its fallback to the title's own decoder, XBA-199), never a reference for decoding."""
        assert self.x8_allowed
        bw = BitWriter()
        bw.put(0, 1)                     # I
        bw.put(0, 7)
        bw.put(q, 5)
        bw.put(1, 1)                     # j_type: X8
        for _ in range(nbytes):
            bw.put(self.rng.randrange(256), 8)
        return bw.bytes()

    def p_frame(self, q, intra_share=0.15):
        r = self.rng
        bw = BitWriter()
        bw.put(1, 1)                     # P
        bw.put(q, 5)
        n_mb = self.mbw * self.mbh
        skip_type = r.randint(0, 3)
        while True:
            skip = [r.random() < 0.3 for _ in range(n_mb)] if skip_type else [False] * n_mb
            if skip_type == 2:
                for y in range(self.mbh):
                    if r.random() < 0.25:
                        for x in range(self.mbw):
                            skip[y * self.mbw + x] = True
            if skip_type == 3:
                for x in range(self.mbw):
                    if r.random() < 0.25:
                        for y in range(self.mbh):
                            skip[y * self.mbw + x] = True
            if not all(skip):
                break
        bw.put(skip_type, 2)
        if skip_type == 1:
            for s in skip:
                bw.put(int(s), 1)
        elif skip_type == 2:
            for y in range(self.mbh):
                row = skip[y * self.mbw:(y + 1) * self.mbw]
                bw.put(int(all(row)), 1)
                if not all(row):
                    for s in row:
                        bw.put(int(s), 1)
        elif skip_type == 3:
            for x in range(self.mbw):
                col = [skip[y * self.mbw + x] for y in range(self.mbh)]
                bw.put(int(all(col)), 1)
                if not all(col):
                    for s in col:
                        bw.put(int(s), 1)
        cbp_index = r.randint(0, 2)
        bw.put012(cbp_index)
        cbp_table = [[0, 2, 1], [1, 0, 2], [2, 1, 0]][(q > 10) + (q > 20)][cbp_index]
        mspel = r.randint(0, 1) if self.mspel_bit else 0
        if self.mspel_bit:
            bw.put(mspel, 1)
        if self.abt_flag:
            self.per_mb_abt = r.randint(0, 1)
            bw.put(self.per_mb_abt ^ 1, 1)
            if not self.per_mb_abt:
                self.abt_type = r.randint(0, 2)
                bw.put012(self.abt_type)
        per_mb_rl = r.randint(0, 1) if self.per_mb_rl_bit else 0
        if self.per_mb_rl_bit:
            bw.put(per_mb_rl, 1)
        rl = 0
        if not per_mb_rl:
            rl = r.randint(0, 2)
            bw.put012(rl)
        dc_table, mv_table = r.randint(0, 1), r.randint(0, 1)
        bw.put(dc_table, 1)
        bw.put(mv_table, 1)
        self.no_rounding ^= 1
        mvx, mvy = self.t["wmv2_mv%d_x" % mv_table], self.t["wmv2_mv%d_y" % mv_table]
        mvcode, mvbits = self.t["wmv2_mv%d_code" % mv_table], self.t["wmv2_mv%d_bits" % mv_table]
        inter_pairs = self.t["wmv2_mb_inter%d" % cbp_table]
        per_block_abt = 0
        W = self.w8
        for mby in range(self.mbh):
            slice_top = (mby // self.slice_height) * self.slice_height
            first_line = mby == slice_top
            for mbx in range(self.mbw):
                if skip[mby * self.mbw + mbx]:
                    self._set_mv(mbx, mby, (0, 0))
                    continue
                intra = r.random() < intra_share
                while True:
                    code = r.randrange(128)
                    if bool(~code & 0x40) == intra:
                        break
                self._vlc(bw, inter_pairs, code)
                cbp = code & 0x3F
                if intra:
                    bw.put(r.randint(0, 1), 1)                   # ac_pred
                    if per_mb_rl and cbp:
                        rl = r.randint(0, 2)
                        bw.put012(rl)
                    for n in range(6):
                        self._intra_block(bw, n, dc_table, rl, rl, (cbp >> (5 - n)) & 1)
                    self._set_mv(mbx, mby, (0, 0))
                    continue
                # Prediction (mirrors the decoder's).
                i = 2 * mby * W + 2 * mbx
                A, B, C = self._getmv(i - 1), self._getmv(i - W), self._getmv(i + 2 - W)
                diff = 0
                if mbx and not first_line and not mspel and self.tlmv:
                    diff = max(abs(A[0] - B[0]), abs(A[1] - B[1]))
                typ = 2
                if diff >= 8:
                    typ = r.randint(0, 1)
                    bw.put(typ, 1)
                if typ == 0 or (typ == 2 and first_line):
                    px, py = A
                elif typ == 1:
                    px, py = B
                else:
                    px, py = _mid(A[0], B[0], C[0]), _mid(A[1], B[1], C[1])
                if cbp:
                    if per_mb_rl:
                        rl = r.randint(0, 2)
                        bw.put012(rl)
                    if self.abt_flag and self.per_mb_abt:
                        per_block_abt = r.randint(0, 1)
                        bw.put(per_block_abt, 1)
                        if not per_block_abt:
                            self.abt_type = r.randint(0, 2)
                            bw.put012(self.abt_type)
                    else:
                        per_block_abt = 0

                def wrap(v):
                    return v + 64 if v <= -64 else v - 64 if v >= 64 else v
                chosen = None
                for _ in range(60):
                    s = r.randrange(1099)
                    mx, my = wrap(mvx[s] + px - 32), wrap(mvy[s] + py - 32)
                    if self._mv_ok(mbx, mby, mx, my, mspel, True):
                        chosen = (s, mx, my)
                        break
                if chosen is None:
                    # The escape: any vector within 32 of the prediction.
                    cands = [(ex, ey) for ex in range(64) for ey in range(64)
                             if self._mv_ok(mbx, mby, wrap(ex + px - 32), wrap(ey + py - 32), mspel, True)]
                    ex, ey = r.choice(cands)
                    chosen = (1099, wrap(ex + px - 32), wrap(ey + py - 32))
                    bw.put(mvcode[1099], mvbits[1099])
                    bw.put(ex, 6)
                    bw.put(ey, 6)
                else:
                    bw.put(mvcode[chosen[0]], mvbits[chosen[0]])
                _, mx, my = chosen
                if mspel and ((mx | my) & 1):
                    bw.put(r.randint(0, 1), 1)                   # hshift
                for n in range(6):
                    if not (cbp >> (5 - n)) & 1:
                        continue
                    if per_block_abt:
                        self.abt_type = r.randint(0, 2)
                        bw.put012(self.abt_type)
                    if self.abt_type:
                        sub_index = r.randint(0, 2)
                        bw.put012(sub_index)
                        sub = (2, 3, 1)[sub_index]
                        if sub & 1:
                            self._coeffs(bw, 3 + rl, -1, 31)
                        if sub & 2:
                            self._coeffs(bw, 3 + rl, -1, 31)
                    else:
                        self._coeffs(bw, 3 + rl, -1, 62)
                self._set_mv(mbx, mby, (mx, my))
        return bw.bytes()


def synth_movie(width, height, nframes, gop, seed, x8_at=(), **kw):
    """(header word, [big-endian frames]): an I frame every gop frames. The I frames whose index is in x8_at
    are X8 ones (x8_i_frame; x8_allowed is then set)."""
    if x8_at:
        kw["x8_allowed"] = 1
    s = Synth(width, height, seed, **kw)
    r = random.Random(seed ^ 0x5A5A)
    frames = []
    for k in range(nframes):
        q = r.randint(1, 31)
        if k % gop == 0:
            frames.append(s.x8_i_frame(q) if k in x8_at else s.i_frame(q))
        else:
            frames.append(s.p_frame(q))
    return s.header_word(), frames
