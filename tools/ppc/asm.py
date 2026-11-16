"""A few PowerPC encoders for tests (XBA-225): enough to write synthetic code without a toolchain.

Each function returns one instruction word. Branch helpers take the instruction's own address and the target.
"""


def _d(op, a, b, imm):
    return (op << 26) | ((a & 31) << 21) | ((b & 31) << 16) | (imm & 0xFFFF)


def _x(op, d, a, b, xo, rc=0):
    return (op << 26) | ((d & 31) << 21) | ((a & 31) << 16) | ((b & 31) << 11) | ((xo & 0x3FF) << 1) | rc


def addi(rd, ra, simm): return _d(14, rd, ra, simm)
def li(rd, simm): return addi(rd, 0, simm)
def addis(rd, ra, imm): return _d(15, rd, ra, imm)
def lis(rd, imm): return addis(rd, 0, imm)
def ori(ra, rs, uimm): return _d(24, rs, ra, uimm)
def mulli(rd, ra, simm): return _d(7, rd, ra, simm)
def lwz(rd, d, ra): return _d(32, rd, ra, d)
def stw(rs, d, ra): return _d(36, rs, ra, d)
def lfs(frd, d, ra): return _d(48, frd, ra, d)
def stfs(frs, d, ra): return _d(52, frs, ra, d)
def lfd(frd, d, ra): return _d(50, frd, ra, d)
def stfd(frs, d, ra): return _d(54, frs, ra, d)
def add(rd, ra, rb, rc=0): return _x(31, rd, ra, rb, 266, rc)
def addze(rd, ra): return _x(31, rd, ra, 0, 202)
def mullw(rd, ra, rb): return _x(31, rd, ra, rb, 235)
def divw(rd, ra, rb): return _x(31, rd, ra, rb, 491)
def srawi(ra, rs, sh): return _x(31, rs, ra, sh, 824)
def cmpw(crf, ra, rb): return _x(31, crf << 2, ra, rb, 0)
def cmplw(crf, ra, rb): return _x(31, crf << 2, ra, rb, 32)
def mfcr(rd): return _x(31, rd, 0, 0, 19)
def stfiwx(frs, ra, rb): return _x(31, frs, ra, rb, 983)
def crxor(d, a, b): return _x(19, d, a, b, 193)
def blr(): return 0x4E800020
def bctr(): return 0x4E800420


def rlwinm(ra, rs, sh, mb, me, rc=0):
    return (21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc


def mtspr(spr, rs):
    return (31 << 26) | (rs << 21) | ((spr & 31) << 16) | ((spr >> 5) << 11) | (467 << 1)


def mfspr(rd, spr):
    return (31 << 26) | (rd << 21) | ((spr & 31) << 16) | ((spr >> 5) << 11) | (339 << 1)


def mtctr(rs): return mtspr(9, rs)
def mtlr(rs): return mtspr(8, rs)
def mflr(rd): return mfspr(rd, 8)


def b(at, target, lk=0):
    return (18 << 26) | ((target - at) & 0x03FFFFFC) | lk


def bl(at, target):
    return b(at, target, 1)


def bc(at, bo, bi, target, lk=0):
    return (16 << 26) | (bo << 21) | (bi << 16) | ((target - at) & 0xFFFC) | lk


def bdnz(at, target):
    return bc(at, 16, 0, target)


def fa(op, xo, d, a, b_, c=0, rc=0):
    """A-form FP (opcode 59 / 63), and the Gekko's paired-single arithmetic (opcode 4)."""
    return (op << 26) | (d << 21) | (a << 16) | (b_ << 11) | (c << 6) | (xo << 1) | rc


def fadds(d, a, b_): return fa(59, 21, d, a, b_)
def fmadd(d, a, c, b_): return fa(63, 29, d, a, b_, c)
def fnmsub(d, a, c, b_): return fa(63, 30, d, a, b_, c)
def fnmadd(d, a, c, b_): return fa(63, 31, d, a, b_, c)
def fnmsubs(d, a, c, b_): return fa(59, 30, d, a, b_, c)
def fnmadds(d, a, c, b_): return fa(59, 31, d, a, b_, c)
def fctiwz(d, b_): return _x(63, d, 0, b_, 15)


# Gekko
def psq_l(frd, d, ra, w, i): return (56 << 26) | (frd << 21) | (ra << 16) | (w << 15) | (i << 12) | (d & 0xFFF)
def psq_st(frs, d, ra, w, i): return (60 << 26) | (frs << 21) | (ra << 16) | (w << 15) | (i << 12) | (d & 0xFFF)
def ps_madd(d, a, c, b_): return fa(4, 29, d, a, b_, c)
def ps_nmsub(d, a, c, b_): return fa(4, 30, d, a, b_, c)
def ps_nmadd(d, a, c, b_): return fa(4, 31, d, a, b_, c)
def ps_add(d, a, b_): return fa(4, 21, d, a, b_)
def ps_merge10(d, a, b_): return _x(4, d, a, b_, 592)
def ps_sum0(d, a, c, b_): return fa(4, 10, d, a, b_, c)
