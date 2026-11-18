"""Xenon encoders for tests (XBA-230): 64-bit integer forms, VMX and VMX128, on top of tools/ppc/asm.py.

Each function returns one instruction word. VMX128 register numbers go up to 127 and are split over the fields the
way the hardware wants (see xenon.py); the tests check the decoder gives them back.
"""

from tools.ppc.asm import *  # noqa: F401,F403  (the base encoders: li, addi, lwz, bdnz...)
from tools.ppc.asm import _d, _x


# ---- 64-bit integer ----
def ld(rd, ds, ra): return (58 << 26) | (rd << 21) | (ra << 16) | (ds & 0xFFFC)
def ldu(rd, ds, ra): return ld(rd, ds, ra) | 1
def lwa(rd, ds, ra): return ld(rd, ds, ra) | 2
def std(rs, ds, ra): return (62 << 26) | (rs << 21) | (ra << 16) | (ds & 0xFFFC)
def stdu(rs, ds, ra): return std(rs, ds, ra) | 1
def ldx(rd, ra, rb): return _x(31, rd, ra, rb, 21)
def stdx(rs, ra, rb): return _x(31, rs, ra, rb, 149)
def lbzu(rd, d, ra): return _d(35, rd, ra, d)
def lwzu(rd, d, ra): return _d(33, rd, ra, d)
def xor(ra, rs, rb, rc=0): return _x(31, rs, ra, rb, 316, rc)
def or_(ra, rs, rb): return _x(31, rs, ra, rb, 444)
def mr(ra, rs): return or_(ra, rs, rs)
def oris(ra, rs, uimm): return _d(25, rs, ra, uimm)
def mulld(rd, ra, rb): return _x(31, rd, ra, rb, 233)
def mulhdu(rd, ra, rb): return _x(31, rd, ra, rb, 9)
def mulhd(rd, ra, rb): return _x(31, rd, ra, rb, 73)
def divd(rd, ra, rb): return _x(31, rd, ra, rb, 489)
def divdu(rd, ra, rb): return _x(31, rd, ra, rb, 457)
def subf(rd, ra, rb): return _x(31, rd, ra, rb, 40)
def neg(rd, ra): return _x(31, rd, ra, 0, 104)
def sld(ra, rs, rb): return _x(31, rs, ra, rb, 27)
def srd(ra, rs, rb): return _x(31, rs, ra, rb, 539)
def srad(ra, rs, rb): return _x(31, rs, ra, rb, 794)
def cntlzd(ra, rs): return _x(31, rs, ra, 0, 58)
def extsw(ra, rs, rc=0): return _x(31, rs, ra, 0, 986, rc)
def extsh(ra, rs): return _x(31, rs, ra, 0, 922)
def cmpd(crf, ra, rb): return _x(31, (crf << 2) | 1, ra, rb, 0)
def cmpld(crf, ra, rb): return _x(31, (crf << 2) | 1, ra, rb, 32)
def cmpdi(crf, ra, simm): return _d(11, (crf << 2) | 1, ra, simm)
def cmpwi(crf, ra, simm): return _d(11, crf << 2, ra, simm)
def addc(rd, ra, rb): return _x(31, rd, ra, rb, 10)
def adde(rd, ra, rb): return _x(31, rd, ra, rb, 138)


def sradi(ra, rs, sh, rc=0):
    return (31 << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | (413 << 2) | (((sh >> 5) & 1) << 1) | rc


def _md(xo, ra, rs, sh, mb, rc=0):
    """MD form: 6-bit sh (sh5 in bit 1) and mb / me (mb5 in bit 5)."""
    return ((30 << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | ((mb & 31) << 6) | (((mb >> 5) & 1) << 5)
            | (xo << 2) | (((sh >> 5) & 1) << 1) | rc)


def rldicl(ra, rs, sh, mb, rc=0): return _md(0, ra, rs, sh, mb, rc)
def rldicr(ra, rs, sh, me, rc=0): return _md(1, ra, rs, sh, me, rc)
def rldic(ra, rs, sh, mb, rc=0): return _md(2, ra, rs, sh, mb, rc)
def rldimi(ra, rs, sh, mb, rc=0): return _md(3, ra, rs, sh, mb, rc)
def sldi(ra, rs, n): return rldicr(ra, rs, n, 63 - n)
def srdi(ra, rs, n): return rldicl(ra, rs, 64 - n, n)
def rotldi(ra, rs, n): return rldicl(ra, rs, n, 0)
def clrldi(ra, rs, n): return rldicl(ra, rs, 0, n)


def rldcl(ra, rs, rb, mb, rc=0):
    return ((30 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | ((mb & 31) << 6) | (((mb >> 5) & 1) << 5) | (8 << 1)
            | rc)


def fctidz(d, b_): return _x(63, d, 0, b_, 815)
def fcfid(d, b_): return _x(63, d, 0, b_, 846)
def fmr(d, b_): return _x(63, d, 0, b_, 72)
def fabs_(d, b_): return _x(63, d, 0, b_, 264)
def fsel(d, a, c, b_): return fa(63, 23, d, a, b_, c)  # noqa: F405
def fmul(d, a, c): return fa(63, 25, d, a, 0, c)  # noqa: F405
def fadd(d, a, b_): return fa(63, 21, d, a, b_)  # noqa: F405
def fsub(d, a, b_): return fa(63, 20, d, a, b_)  # noqa: F405
def lfdu(frd, d, ra): return _d(51, frd, ra, d)
def stfd_(frs, d, ra): return _d(54, frs, ra, d)


def li64(rd, v):
    """The 5-instruction sequence compilers use for a 64-bit constant (lis / ori / sldi 32 / oris / ori)."""
    v &= (1 << 64) - 1
    return [lis(rd, (v >> 48) & 0xFFFF), ori(rd, rd, (v >> 32) & 0xFFFF), sldi(rd, rd, 32),  # noqa: F405
            oris(rd, rd, (v >> 16) & 0xFFFF), ori(rd, rd, v & 0xFFFF)]  # noqa: F405


# ---- VMX (AltiVec, v0..v31) ----
def _vx(xo, vd, va, vb): return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | xo
def _va(xo, vd, va, vb, vc): return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | (vc << 6) | xo
def vaddfp(vd, va, vb): return _vx(0x00A, vd, va, vb)
def vsubfp(vd, va, vb): return _vx(0x04A, vd, va, vb)
def vadduwm(vd, va, vb): return _vx(0x080, vd, va, vb)
def vmrghw(vd, va, vb): return _vx(0x08C, vd, va, vb)
def vmrglw(vd, va, vb): return _vx(0x18C, vd, va, vb)
def vspltw(vd, vb, uimm): return _vx(0x28C, vd, uimm, vb)
def vspltisw(vd, simm): return _vx(0x38C, vd, simm & 31, 0)
def vxor(vd, va, vb): return _vx(0x4C4, vd, va, vb)
def vor(vd, va, vb): return _vx(0x484, vd, va, vb)
def vmaxfp(vd, va, vb): return _vx(0x40A, vd, va, vb)
def vcfsx(vd, vb, uimm): return _vx(0x34A, vd, uimm, vb)
def vctsxs(vd, vb, uimm): return _vx(0x3CA, vd, uimm, vb)
def vcmpgtfp(vd, va, vb, rc=0): return _vx(0x2C6 | (rc << 10), vd, va, vb)
def vmaddfp(vd, va, vc, vb): return _va(0x2E, vd, va, vb, vc)
def vnmsubfp(vd, va, vc, vb): return _va(0x2F, vd, va, vb, vc)
def vperm(vd, va, vb, vc): return _va(0x2B, vd, va, vb, vc)
def vsel(vd, va, vb, vc): return _va(0x2A, vd, va, vb, vc)
def vsldoi(vd, va, vb, sh): return _va(0x2C, vd, va, vb, sh & 15)
def lvx(vd, ra, rb): return _x(31, vd, ra, rb, 103)
def stvx(vs, ra, rb): return _x(31, vs, ra, rb, 231)
def lvsl(vd, ra, rb): return _x(31, vd, ra, rb, 6)
def lvsr(vd, ra, rb): return _x(31, vd, ra, rb, 38)
def stvewx(vs, ra, rb): return _x(31, vs, ra, rb, 199)
def lvlx(vd, ra, rb): return _x(31, vd, ra, rb, 519)
def lvrx(vd, ra, rb): return _x(31, vd, ra, rb, 551)


# ---- VMX128 (v0..v127) ----
def _r128(vd, va, vb):
    """The split VMX128 register fields: VD128 = VDl | VDh << 5, VA128 = VAl | VAh << 5 | VAH << 6,
    VB128 = VBl | VBh << 5."""
    return (((vd & 31) << 21) | (((vd >> 5) & 3) << 2) | ((va & 31) << 16) | (((va >> 5) & 1) << 5)
            | (((va >> 6) & 1) << 10) | ((vb & 31) << 11) | ((vb >> 5) & 3))


def _vx128_1(xo, vd, ra, rb):
    return (4 << 26) | ((vd & 31) << 21) | (ra << 16) | (rb << 11) | (((vd >> 5) & 3) << 2) | xo


def lvx128(vd, ra, rb): return _vx128_1(0x0C3, vd, ra, rb)
def stvx128(vs, ra, rb): return _vx128_1(0x1C3, vs, ra, rb)
def lvlx128(vd, ra, rb): return _vx128_1(0x403, vd, ra, rb)
def lvrx128(vd, ra, rb): return _vx128_1(0x443, vd, ra, rb)
def stvlx128(vs, ra, rb): return _vx128_1(0x503, vs, ra, rb)
def stvrx128(vs, ra, rb): return _vx128_1(0x543, vs, ra, rb)
def lvsl128(vd, ra, rb): return _vx128_1(0x003, vd, ra, rb)


def _op5(xo, vd, va, vb): return (5 << 26) | _r128(vd, va, vb) | xo
def _op6(xo, vd, va, vb): return (6 << 26) | _r128(vd, va, vb) | xo
def vaddfp128(vd, va, vb): return _op5(0x010, vd, va, vb)
def vsubfp128(vd, va, vb): return _op5(0x050, vd, va, vb)
def vmulfp128(vd, va, vb): return _op5(0x090, vd, va, vb)
def vmaddfp128(vd, va, vb): return _op5(0x0D0, vd, va, vb)     # vd = va * vb + vd
def vmaddcfp128(vd, va, vb): return _op5(0x110, vd, va, vb)    # vd = va * vd + vb
def vnmsubfp128(vd, va, vb): return _op5(0x150, vd, va, vb)    # vd = -(va * vb - vd)
def vmsum3fp128(vd, va, vb): return _op5(0x190, vd, va, vb)
def vmsum4fp128(vd, va, vb): return _op5(0x1D0, vd, va, vb)
def vand128(vd, va, vb): return _op5(0x210, vd, va, vb)
def vandc128(vd, va, vb): return _op5(0x250, vd, va, vb)
def vor128(vd, va, vb): return _op5(0x2D0, vd, va, vb)
def vxor128(vd, va, vb): return _op5(0x310, vd, va, vb)
def vsel128(vd, va, vb): return _op5(0x350, vd, va, vb)        # control is vd
def vperm128(vd, va, vb, vc): return (5 << 26) | _r128(vd, va, vb) | ((vc & 7) << 6)
def vslw128(vd, va, vb): return _op6(0x0D0, vd, va, vb)
def vsrw128(vd, va, vb): return _op6(0x1D0, vd, va, vb)
def vmaxfp128(vd, va, vb): return _op6(0x280, vd, va, vb)
def vminfp128(vd, va, vb): return _op6(0x2C0, vd, va, vb)
def vmrghw128(vd, va, vb): return _op6(0x300, vd, va, vb)
def vmrglw128(vd, va, vb): return _op6(0x340, vd, va, vb)
def vcmpgtfp128(vd, va, vb, rc=0): return _op6(0x100 | (rc << 6), vd, va, vb)
def vcmpeqfp128(vd, va, vb, rc=0): return _op6(0x000 | (rc << 6), vd, va, vb)


def _vx128_3(xo, vd, vb, imm): return (6 << 26) | _r128(vd, 0, vb) | ((imm & 31) << 16) | xo
def vspltw128(vd, vb, uimm): return _vx128_3(0x730, vd, vb, uimm)
def vspltisw128(vd, simm): return _vx128_3(0x770, vd, 0, simm)
def vcsxwfp128(vd, vb, uimm): return _vx128_3(0x2B0, vd, vb, uimm)
def vcfpsxws128(vd, vb, uimm): return _vx128_3(0x230, vd, vb, uimm)
def vrfin128(vd, vb): return _vx128_3(0x370, vd, vb, 0)
def vrefp128(vd, vb): return _vx128_3(0x630, vd, vb, 0)
def vupkd3d128(vd, vb, imm): return _vx128_3(0x7F0, vd, vb, imm)


def vpermwi128(vd, vb, perm):
    return (6 << 26) | _r128(vd, 0, vb) | ((perm & 31) << 16) | (((perm >> 5) & 7) << 6) | 0x210


def vrlimi128(vd, vb, imm, z):
    return (6 << 26) | _r128(vd, 0, vb) | ((imm & 31) << 16) | ((z & 3) << 6) | 0x710


def vpkd3d128(vd, vb, typ, pack, shift):
    """IMM = type << 2 | pack, z = shift (VX128_4 form)."""
    return (6 << 26) | _r128(vd, 0, vb) | ((((typ & 7) << 2) | (pack & 3)) << 16) | ((shift & 3) << 6) | 0x610


def vsldoi128(vd, va, vb, sh):
    return (4 << 26) | _r128(vd, va, vb) | ((sh & 15) << 6) | 0x10


def mfcr(rd): return _x(31, rd, 0, 0, 19)
