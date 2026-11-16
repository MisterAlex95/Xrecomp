"""The Xenon layer on the shared PowerPC frontend (XBA-230): 64-bit integer forms, VMX (AltiVec) and VMX128.

tools.ppc decodes and lifts the 750's 32-bit instruction set; the Xbox 360's Xenon (PowerPC 2.02, 64-bit, big-endian)
adds, on opcodes the base leaves open or does not know:

- 64-bit integer: ld / ldu / lwa (58), std / stdu (62), the rld* rotates (30), tdi (2), and on opcode 31 ldx / ldux /
  stdx / stdux / lwax / lwaux / ldbrx / stdbrx / ldarx / stdcx., mulld / mulhd / mulhdu / divd / divdu, sld / srd /
  srad / sradi, cntlzd, extsw, td, mtmsrd; cmp / cmpl / cmpi / cmpli with L = 1 (cmpd...); fctid / fctidz / fcfid
  (63); dcbz128 (dcbz with bit 21 set).
- VMX (AltiVec, 32 registers): opcode 4 VX / VC / VA forms, the loads / stores on opcode 31 (lvx, stvx, lvsl, lvsr,
  lve*x, stve*x, and Xenon's lvlx / lvrx / stvlx / stvrx and their l forms).
- VMX128 (128 registers, register numbers split over several fields): loads / stores on opcode 4 (lvx128...), opcode
  5 (arithmetic, logic, packs, vperm128) and opcode 6 (compares, converts, splats, shifts, merges, vpermwi128,
  vrlimi128, vpkd3d128 / vupkd3d128).

Encodings and field layouts follow Xenia's PowerPC tables (ppc_opcode_lookup_gen.cc / ppc_decode_data.h, BSD), checked
against the opcode constants in the tests. 64-bit semantics: R(n) is 64-bit; word instructions follow 64-bit mode
rules (CR0 compares the 64-bit result, rlw* replicate the low word, sraw / extsh sign-extend to 64 bits); effective
addresses are 32-bit (titles run with 32-bit pointers in 64-bit registers: the memory macros take uint32_t).

The lifter emits the C vocabulary of tools/ppc/lift.py plus V(n) (a 128-bit vector register, union xe_v128 with
PPC element order: element 0 is the first in memory) and the xv_* helpers of consoles/x360/runtime/xenon_ops.h.
Instructions with no op_ method here (most saturating / pack / unpack integer VMX ops, the D3D pack ops vpkd3d128 /
vupkd3d128, vexptefp / vlogefp, traps, mtmsrd) are handed to the runtime with RC_INTERP: the spike has no
interpreter, so the recompiler's stats count them (`unlifted`).
"""

from tools.ppc.isa import TRAP, Decoder, sext
from tools.ppc.lift import RA0, FnLifter, R, hx, mask

M64 = (1 << 64) - 1

# ---- tables (keys: the instruction word under each form's mask) ----

VX4 = {0x000: "vaddubm", 0x002: "vmaxub", 0x004: "vrlb", 0x008: "vmuloub", 0x00A: "vaddfp", 0x00C: "vmrghb",
       0x00E: "vpkuhum", 0x040: "vadduhm", 0x042: "vmaxuh", 0x044: "vrlh", 0x048: "vmulouh", 0x04A: "vsubfp",
       0x04C: "vmrghh", 0x04E: "vpkuwum", 0x080: "vadduwm", 0x082: "vmaxuw", 0x084: "vrlw", 0x08C: "vmrghw",
       0x08E: "vpkuhus", 0x0CE: "vpkuwus", 0x102: "vmaxsb", 0x104: "vslb", 0x108: "vmulosb", 0x10A: "vrefp",
       0x10C: "vmrglb", 0x10E: "vpkshus", 0x142: "vmaxsh", 0x144: "vslh", 0x148: "vmulosh", 0x14A: "vrsqrtefp",
       0x14C: "vmrglh", 0x14E: "vpkswus", 0x180: "vaddcuw", 0x182: "vmaxsw", 0x184: "vslw", 0x18A: "vexptefp",
       0x18C: "vmrglw", 0x18E: "vpkshss", 0x1C4: "vsl", 0x1CA: "vlogefp", 0x1CE: "vpkswss", 0x200: "vaddubs",
       0x202: "vminub", 0x204: "vsrb", 0x208: "vmuleub", 0x20A: "vrfin", 0x20C: "vspltb", 0x20E: "vupkhsb",
       0x240: "vadduhs", 0x242: "vminuh", 0x244: "vsrh", 0x248: "vmuleuh", 0x24A: "vrfiz", 0x24C: "vsplth",
       0x24E: "vupkhsh", 0x280: "vadduws", 0x282: "vminuw", 0x284: "vsrw", 0x28A: "vrfip", 0x28C: "vspltw",
       0x28E: "vupklsb", 0x2C4: "vsr", 0x2CA: "vrfim", 0x2CE: "vupklsh", 0x300: "vaddsbs", 0x302: "vminsb",
       0x304: "vsrab", 0x308: "vmulesb", 0x30A: "vcfux", 0x30C: "vspltisb", 0x30E: "vpkpx", 0x340: "vaddshs",
       0x342: "vminsh", 0x344: "vsrah", 0x348: "vmulesh", 0x34A: "vcfsx", 0x34C: "vspltish", 0x34E: "vupkhpx",
       0x380: "vaddsws", 0x382: "vminsw", 0x384: "vsraw", 0x38A: "vctuxs", 0x38C: "vspltisw", 0x3CA: "vctsxs",
       0x3CE: "vupklpx", 0x400: "vsububm", 0x402: "vavgub", 0x404: "vand", 0x40A: "vmaxfp", 0x40C: "vslo",
       0x440: "vsubuhm", 0x442: "vavguh", 0x444: "vandc", 0x44A: "vminfp", 0x44C: "vsro", 0x480: "vsubuwm",
       0x482: "vavguw", 0x484: "vor", 0x4C4: "vxor", 0x502: "vavgsb", 0x504: "vnor", 0x542: "vavgsh",
       0x580: "vsubcuw", 0x582: "vavgsw", 0x600: "vsububs", 0x604: "mfvscr", 0x608: "vsum4ubs", 0x640: "vsubuhs",
       0x644: "mtvscr", 0x648: "vsum4shs", 0x680: "vsubuws", 0x688: "vsum2sws", 0x700: "vsubsbs", 0x708: "vsum4sbs",
       0x740: "vsubshs", 0x780: "vsubsws", 0x788: "vsumsws"}
VC4 = {0x006: "vcmpequb", 0x046: "vcmpequh", 0x086: "vcmpequw", 0x0C6: "vcmpeqfp", 0x1C6: "vcmpgefp",
       0x206: "vcmpgtub", 0x246: "vcmpgtuh", 0x286: "vcmpgtuw", 0x2C6: "vcmpgtfp", 0x306: "vcmpgtsb",
       0x346: "vcmpgtsh", 0x386: "vcmpgtsw", 0x3C6: "vcmpbfp"}
VA4 = {0x020: "vmhaddshs", 0x021: "vmhraddshs", 0x022: "vmladduhm", 0x024: "vmsumubm", 0x025: "vmsummbm",
       0x026: "vmsumuhm", 0x027: "vmsumuhs", 0x028: "vmsumshm", 0x029: "vmsumshs", 0x02A: "vsel", 0x02B: "vperm",
       0x02C: "vsldoi", 0x02E: "vmaddfp", 0x02F: "vnmsubfp"}
VX128_1 = {0x003: "lvsl128", 0x043: "lvsr128", 0x083: "lvewx128", 0x0C3: "lvx128", 0x183: "stvewx128",
           0x1C3: "stvx128", 0x2C3: "lvxl128", 0x3C3: "stvxl128", 0x403: "lvlx128", 0x443: "lvrx128",
           0x503: "stvlx128", 0x543: "stvrx128", 0x603: "lvlxl128", 0x643: "lvrxl128", 0x703: "stvlxl128",
           0x743: "stvrxl128"}
OP5 = {0x010: "vaddfp128", 0x050: "vsubfp128", 0x090: "vmulfp128", 0x0D0: "vmaddfp128", 0x110: "vmaddcfp128",
       0x150: "vnmsubfp128", 0x190: "vmsum3fp128", 0x1D0: "vmsum4fp128", 0x200: "vpkshss128", 0x210: "vand128",
       0x240: "vpkshus128", 0x250: "vandc128", 0x280: "vpkswss128", 0x290: "vnor128", 0x2C0: "vpkswus128",
       0x2D0: "vor128", 0x300: "vpkuhum128", 0x310: "vxor128", 0x340: "vpkuhus128", 0x350: "vsel128",
       0x380: "vpkuwum128", 0x390: "vslo128", 0x3C0: "vpkuwus128", 0x3D0: "vsro128"}
OP6_3 = {0x230: "vcfpsxws128", 0x270: "vcfpuxws128", 0x2B0: "vcsxwfp128", 0x2F0: "vcuxwfp128", 0x330: "vrfim128",
         0x370: "vrfin128", 0x3B0: "vrfip128", 0x3F0: "vrfiz128", 0x630: "vrefp128", 0x670: "vrsqrtefp128",
         0x6B0: "vexptefp128", 0x6F0: "vlogefp128", 0x730: "vspltw128", 0x770: "vspltisw128", 0x7F0: "vupkd3d128"}
OP6_R = {0x000: "vcmpeqfp128", 0x080: "vcmpgefp128", 0x100: "vcmpgtfp128", 0x180: "vcmpbfp128",
         0x200: "vcmpequw128"}
OP6 = {0x050: "vrlw128", 0x0D0: "vslw128", 0x150: "vsraw128", 0x1D0: "vsrw128", 0x280: "vmaxfp128",
       0x2C0: "vminfp128", 0x300: "vmrghw128", 0x340: "vmrglw128", 0x380: "vupkhsb128", 0x3C0: "vupklsb128"}
# opcode 31, VMX loads / stores (X-form, 32 registers)
X31_VMX = {6: "lvsl", 7: "lvebx", 38: "lvsr", 39: "lvehx", 71: "lvewx", 103: "lvx", 135: "stvebx", 167: "stvehx",
           199: "stvewx", 231: "stvx", 359: "lvxl", 487: "stvxl", 519: "lvlx", 551: "lvrx", 647: "stvlx",
           679: "stvrx", 775: "lvlxl", 807: "lvrxl", 903: "stvlxl", 935: "stvrxl"}
# opcode 31, 64-bit integer (10-bit extended opcode)
X31_64 = {21: "ldx", 27: "sld", 53: "ldux", 58: "cntlzd", 68: "td", 84: "ldarx", 149: "stdx", 178: "mtmsrd",
          181: "stdux", 214: "stdcx.", 341: "lwax", 373: "lwaux", 532: "ldbrx", 539: "srd", 660: "stdbrx",
          794: "srad", 986: "extsw"}
# opcode 31, 64-bit XO-form (9-bit extended opcode)
XO31_64 = {9: "mulhdu", 73: "mulhd", 233: "mulld", 457: "divdu", 489: "divd"}
X63_64 = {814: "fctid", 815: "fctidz", 846: "fcfid"}
MD30 = {0: "rldicl", 1: "rldicr", 2: "rldic", 3: "rldimi"}
MDS30 = {8: "rldcl", 9: "rldcr"}

VEC_LOADSTORE = set(X31_VMX.values()) | set(VX128_1.values())


class Ext:
    """Fields of a Xenon instruction (vector registers up to 127, 6-bit shifts / masks, DS displacements)."""
    __slots__ = ("vd", "va", "vb", "vc", "imm", "z", "rc", "sh", "mb", "ds", "form", "text")

    def __init__(self, form):
        self.form = form
        self.vd = self.va = self.vb = self.vc = self.imm = self.z = self.rc = self.sh = self.mb = self.ds = 0
        self.text = None


def _vx128_regs(w, e):
    e.vd = ((w >> 21) & 31) | (((w >> 2) & 3) << 5)
    e.va = ((w >> 16) & 31) | (((w >> 5) & 1) << 5) | (((w >> 10) & 1) << 6)
    e.vb = ((w >> 11) & 31) | ((w & 3) << 5)


class XenonDecoder(Decoder):
    EXT_OPS = (2, 4, 5, 6, 30, 56, 57, 58, 60, 61, 62)

    def decode(self, word, addr):
        w = word & 0xFFFFFFFF
        op = w >> 26
        if op == 31:
            xo = (w >> 1) & 0x3FF
            name = X31_VMX.get(xo)
            form = "x_vmx"
            if name is None:
                name, form = X31_64.get(xo), "x64"
            if name is None and xo & 0x1FF in XO31_64:
                name, form = XO31_64[xo & 0x1FF], "xo64"
            if name is None and (xo >> 1) == 413:
                name, form = "sradi", "xs"
            if name is None and xo == 1014 and (w >> 21) & 31 == 1:
                name, form = "dcbz128", "x64"
            if name is not None:
                return self._finish(w, addr, name, form)
        elif op == 63:
            xo = (w >> 1) & 0x3FF
            if xo in X63_64:
                return self._finish(w, addr, X63_64[xo], "x63")
        return super().decode(w, addr)

    def _finish(self, w, addr, name, form):
        from tools.ppc.isa import Insn
        i = Insn(addr, w)
        i.name = name
        e = Ext(form)
        if form == "x_vmx":
            e.vd, e.va, e.vb = i.rd, i.ra, i.rb
        elif form == "xs":
            e.sh = i.rb | (((w >> 1) & 1) << 5)
        i.ext = e
        if name in ("td",):
            i.kind = TRAP
        e.text = self._text(i)
        i.text = e.text
        return i

    def decode_ext(self, i):
        w, op = i.word, i.op
        if op == 2:
            i.name, i.kind = "tdi", TRAP
            return
        if op == 58 or op == 62:
            names = ("ld", "ldu", "lwa", None) if op == 58 else ("std", "stdu", None, None)
            i.name = names[w & 3]
            if i.name:
                e = Ext("ds")
                e.ds = sext(w & 0xFFFC, 16)
                i.ext = e
                e.text = self._text(i)
            return
        if op == 30:
            e = Ext("md")
            e.sh = ((w >> 11) & 31) | (((w >> 1) & 1) << 5)
            e.mb = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
            i.name = MD30.get((w >> 2) & 7) or MDS30.get((w >> 1) & 15)
            if i.name:
                i.ext = e
                e.text = self._text(i)
            return
        if op == 4:
            self._op4(i)
        elif op == 5:
            self._op5(i)
        elif op == 6:
            self._op6(i)
        if i.name and i.ext is not None:
            i.ext.text = self._text(i)

    # ---- VMX / VMX128 ----
    def _op4(self, i):
        w = i.word
        if (w & 0x7F3) in VX128_1:
            e = Ext("vx128_1")
            e.vd = ((w >> 21) & 31) | (((w >> 2) & 3) << 5)
            i.name, i.ext = VX128_1[w & 0x7F3], e
            return
        e = Ext("va")
        e.vd, e.va, e.vb, e.vc = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        if (w & 0x7FF) in VX4:
            i.name, e.form = VX4[w & 0x7FF], "vx"
        elif (w & 0x3FF) in VC4:
            i.name, e.form, e.rc = VC4[w & 0x3FF], "vc", (w >> 10) & 1
        elif (w & 0x3F) in VA4:
            i.name = VA4[w & 0x3F]
        elif w & 0x10:
            e = Ext("vx128_5")
            _vx128_regs(w, e)
            e.sh = (w >> 6) & 15
            i.name = "vsldoi128"
        if i.name:
            i.ext = e

    def _op5(self, i):
        w = i.word
        e = Ext("vx128")
        _vx128_regs(w, e)
        if (w & 0x210) == 0:
            e.form, e.vc = "vx128_2", (w >> 6) & 7
            i.name = "vperm128"
        else:
            i.name = OP5.get(w & 0x3D0)
        if i.name:
            i.ext = e

    def _op6(self, i):
        w = i.word
        e = Ext("vx128")
        _vx128_regs(w, e)
        e.imm = (w >> 16) & 31
        if (w & 0x630) == 0x210:
            e.form, e.imm = "vx128_p", ((w >> 16) & 31) | (((w >> 6) & 7) << 5)
            i.name = "vpermwi128"
        elif (w & 0x730) in (0x610, 0x710):
            e.form, e.z = "vx128_4", (w >> 6) & 3
            i.name = "vpkd3d128" if (w & 0x730) == 0x610 else "vrlimi128"
        elif (w & 0x7F0) in OP6_3:
            e.form = "vx128_3"
            i.name = OP6_3[w & 0x7F0]
        elif (w & 0x390) in OP6_R:
            e.form, e.rc = "vx128_r", (w >> 6) & 1
            i.name = OP6_R[w & 0x390]
        else:
            i.name = OP6.get(w & 0x3D0)
        if i.name:
            i.ext = e

    # ---- disassembly ----
    def _text(self, i):
        n, e = i.name, i.ext
        f = e.form
        if f == "ds":
            reg = "r%d" % i.rd
            return "%s %s, %d(r%d)" % (n, reg, e.ds, i.ra)
        if f == "md":
            if n in MDS30.values():
                return "%s%s r%d, r%d, r%d, %d" % (n, "." if i.rec else "", i.ra, i.rd, i.rb, e.mb)
            return "%s%s r%d, r%d, %d, %d" % (n, "." if i.rec else "", i.ra, i.rd, e.sh, e.mb)
        if f == "xs":
            return "sradi%s r%d, r%d, %d" % ("." if i.rec else "", i.ra, i.rd, e.sh)
        if f == "x_vmx":
            return "%s v%d, r%d, r%d" % (n, e.vd, i.ra, i.rb)
        if f == "vx128_1":
            return "%s v%d, r%d, r%d" % (n, e.vd, i.ra, i.rb)
        if f in ("x64", "xo64"):
            if n in ("ldx", "ldux", "stdx", "stdux", "lwax", "lwaux", "ldbrx", "stdbrx", "ldarx", "stdcx.", "td"):
                return "%s r%d, r%d, r%d" % (n, i.rd, i.ra, i.rb)
            if n in ("cntlzd", "extsw"):
                return "%s%s r%d, r%d" % (n, "." if i.rec else "", i.ra, i.rd)
            if n in ("sld", "srd", "srad"):
                return "%s%s r%d, r%d, r%d" % (n, "." if i.rec else "", i.ra, i.rd, i.rb)
            if n == "dcbz128":
                return "dcbz128 r%d, r%d" % (i.ra, i.rb)
            return "%s%s%s r%d, r%d, r%d" % (n, "o" if i.oe else "", "." if i.rec else "", i.rd, i.ra, i.rb)
        if f == "x63":
            return "%s%s f%d, f%d" % (n, "." if i.rec else "", i.rd, i.rb)
        dot = "." if e.rc else ""
        if f == "va":
            if n == "vsldoi":
                return "vsldoi v%d, v%d, v%d, %d" % (e.vd, e.va, e.vb, e.vc & 15)
            return "%s v%d, v%d, v%d, v%d" % (n, e.vd, e.va, e.vb, e.vc)
        if f in ("vx", "vc"):
            if n in ("vspltb", "vsplth", "vspltw", "vcfux", "vcfsx", "vctuxs", "vctsxs"):
                return "%s v%d, v%d, %d" % (n, e.vd, e.vb, e.va)
            if n in ("vspltisb", "vspltish", "vspltisw"):
                return "%s v%d, %d" % (n, e.vd, sext(e.va, 5))
            if n == "mfvscr":
                return "mfvscr v%d" % e.vd
            if n == "mtvscr":
                return "mtvscr v%d" % e.vb
            return "%s%s v%d, v%d, v%d" % (n, dot, e.vd, e.va, e.vb)
        if f == "vx128_2":
            return "%s v%d, v%d, v%d, v%d" % (n, e.vd, e.va, e.vb, e.vc)
        if f == "vx128_5":
            return "%s v%d, v%d, v%d, %d" % (n, e.vd, e.va, e.vb, e.sh)
        if f == "vx128_3":
            if n == "vspltisw128":
                return "%s v%d, %d" % (n, e.vd, sext(e.imm, 5))
            if n in ("vspltw128", "vcfpsxws128", "vcfpuxws128", "vcsxwfp128", "vcuxwfp128"):
                return "%s v%d, v%d, %d" % (n, e.vd, e.vb, e.imm)
            return "%s v%d, v%d" % (n, e.vd, e.vb)
        if f == "vx128_4":
            return "%s v%d, v%d, %d, %d" % (n, e.vd, e.vb, e.imm, e.z)
        if f == "vx128_p":
            return "%s v%d, v%d, %d" % (n, e.vd, e.vb, e.imm)
        return "%s%s v%d, v%d, v%d" % (n, dot, e.vd, e.va, e.vb)


# ---- the lifter ----

def k64(v):
    return "0x%016XULL" % (v & M64)


def mask64(mb, me):
    a = M64 >> mb
    b = M64 if me == 63 else ~(M64 >> (me + 1)) & M64
    return (a & b) if mb <= me else (a | b)


def V(n):
    return "V(%d)" % n


XENON_INTERP = {"td", "tdi", "mtmsrd"}
OE_64 = {"mulld", "mulhd", "mulhdu", "divd", "divdu"}


class XenonLifter(FnLifter):
    def needs_interp(self, i):
        n = i.name
        if n in XENON_INTERP:
            return True
        if n in OE_64 and i.oe:
            return True
        return super().needs_interp(i)

    def is_fp(self, i):
        if i.ext is not None and isinstance(i.ext, Ext) and i.ext.form not in ("x63",):
            return False
        return super().is_fp(i)

    # ---- 64-bit versions of the base's word instructions ----
    def op_addi(self, i):
        return "%s = %s + %s;" % (R(i.rd), RA0(i.ra), k64(i.simm))

    def op_addis(self, i):
        return "%s = %s + %s;" % (R(i.rd), RA0(i.ra), k64(i.simm << 16))

    def op_addic(self, i):
        return "{ uint64_t a_ = %s; %s = a_ + %s; c->xer_ca = ppc_carry64(a_, %s, 0); }" % (
            R(i.ra), R(i.rd), k64(i.simm), k64(i.simm))

    def op_addic_rec(self, i):
        return self.op_addic(i) + " RC_CR0(%s);" % R(i.rd)

    def op_subfic(self, i):
        return "{ uint64_t a_ = %s; %s = ~a_ + %s + 1u; c->xer_ca = ppc_carry64(~a_, %s, 1); }" % (
            R(i.ra), R(i.rd), k64(i.simm), k64(i.simm))

    def op_mulli(self, i):
        return "%s = (uint64_t)((int64_t)%s * %dLL);" % (R(i.rd), R(i.ra), i.simm)

    def _l(self, i):
        return i.rd & 1

    def op_cmpi(self, i):
        if self._l(i):
            return "c->cr = ppc_cr_set(c->cr, %d, ppc_cmps64((int64_t)%s, %dLL, c->xer_so));" % (
                i.crfd, R(i.ra), i.simm)
        return super().op_cmpi(i)

    def op_cmpli(self, i):
        if self._l(i):
            return "c->cr = ppc_cr_set(c->cr, %d, ppc_cmpu64(%s, %du, c->xer_so));" % (i.crfd, R(i.ra), i.uimm)
        return super().op_cmpli(i)

    def op_cmp(self, i):
        if self._l(i):
            return "c->cr = ppc_cr_set(c->cr, %d, ppc_cmps64((int64_t)%s, (int64_t)%s, c->xer_so));" % (
                i.crfd, R(i.ra), R(i.rb))
        return super().op_cmp(i)

    def op_cmpl(self, i):
        if self._l(i):
            return "c->cr = ppc_cr_set(c->cr, %d, ppc_cmpu64(%s, %s, c->xer_so));" % (i.crfd, R(i.ra), R(i.rb))
        return super().op_cmpl(i)

    def _rlw(self, i, sh):
        """rlwinm / rlwnm in 64-bit mode: the low word rotated and replicated in both halves, under
        MASK(mb + 32, me + 32): with mb > me the mask also covers the whole high word."""
        m = mask(i.mb, i.me)
        rot = "ppc_rotl((uint32_t)%s, %s)" % (R(i.rd), sh)
        if i.mb <= i.me:
            body = "%s = (uint64_t)(%s & %s);" % (R(i.ra), rot, hx(m))
        else:
            body = "{ uint32_t r_ = %s; %s = ((uint64_t)r_ << 32) | (r_ & %s); }" % (rot, R(i.ra), hx(m))
        return body + self.rec(i, i.ra)

    def op_rlwinm(self, i):
        return self._rlw(i, str(i.rb))

    def op_rlwnm(self, i):
        return self._rlw(i, "(unsigned)(%s & 31)" % R(i.rb))

    def op_rlwimi(self, i):
        m = mask(i.mb, i.me)
        m64 = m if i.mb <= i.me else (0xFFFFFFFF00000000 | m)
        return ("{ uint32_t r_ = ppc_rotl((uint32_t)%s, %d); uint64_t v_ = ((uint64_t)r_ << 32) | r_; "
                "%s = (v_ & %s) | (%s & %s); }%s" % (R(i.rd), i.rb, R(i.ra), k64(m64), R(i.ra), k64(~m64),
                                                     self.rec(i, i.ra)))

    def _xo(self, i, body):
        return "{ uint64_t a_ = %s, b_ = %s; (void)b_; %s }%s" % (R(i.ra), R(i.rb), body, self.rec(i, i.rd))

    def op_addc(self, i): return self._xo(i, "%s = a_ + b_; c->xer_ca = ppc_carry64(a_, b_, 0);" % R(i.rd))

    def op_subfc(self, i):
        return self._xo(i, "%s = ~a_ + b_ + 1u; c->xer_ca = ppc_carry64(~a_, b_, 1);" % R(i.rd))

    def op_adde(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = a_ + b_ + ci_; c->xer_ca = ppc_carry64(a_, b_, ci_);"
                        % R(i.rd))

    def op_subfe(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = ~a_ + b_ + ci_; c->xer_ca = ppc_carry64(~a_, b_, ci_);"
                        % R(i.rd))

    def op_addze(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = a_ + ci_; c->xer_ca = ppc_carry64(a_, 0, ci_);"
                        % R(i.rd))

    def op_subfze(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = ~a_ + ci_; c->xer_ca = ppc_carry64(~a_, 0, ci_);"
                        % R(i.rd))

    def op_addme(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = a_ - 1u + ci_; c->xer_ca = ppc_carry64(a_, ~0ULL, ci_);"
                        % R(i.rd))

    def op_subfme(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = ~a_ - 1u + ci_; c->xer_ca = ppc_carry64(~a_, ~0ULL, ci_);"
                        % R(i.rd))

    def op_mullw(self, i):
        return self._xo(i, "%s = (uint64_t)((int64_t)(int32_t)a_ * (int64_t)(int32_t)b_);" % R(i.rd))

    def op_mulhw(self, i): return self._xo(i, "%s = (uint32_t)ppc_mulhw((uint32_t)a_, (uint32_t)b_);" % R(i.rd))
    def op_mulhwu(self, i): return self._xo(i, "%s = (uint32_t)ppc_mulhwu((uint32_t)a_, (uint32_t)b_);" % R(i.rd))
    def op_divw(self, i): return self._xo(i, "%s = (uint32_t)ppc_divw((uint32_t)a_, (uint32_t)b_);" % R(i.rd))
    def op_divwu(self, i): return self._xo(i, "%s = (uint32_t)ppc_divwu((uint32_t)a_, (uint32_t)b_);" % R(i.rd))
    def op_mulld(self, i): return self._xo(i, "%s = a_ * b_;" % R(i.rd))
    def op_mulhd(self, i): return self._xo(i, "%s = ppc_mulhd(a_, b_);" % R(i.rd))
    def op_mulhdu(self, i): return self._xo(i, "%s = ppc_mulhdu(a_, b_);" % R(i.rd))
    def op_divd(self, i): return self._xo(i, "%s = ppc_divd(a_, b_);" % R(i.rd))
    def op_divdu(self, i): return self._xo(i, "%s = ppc_divdu(a_, b_);" % R(i.rd))

    def op_sraw(self, i): return self._x(i, "(uint64_t)(int64_t)(int32_t)ppc_sraw((uint32_t)%(s)s, (uint32_t)%(b)s, "
                                            "&c->xer_ca)")

    def op_srawi(self, i):
        return self._x(i, "(uint64_t)(int64_t)(int32_t)ppc_srawi((uint32_t)%%(s)s, %d, &c->xer_ca)" % i.rb)

    def op_slw(self, i): return self._x(i, "(uint64_t)ppc_slw((uint32_t)%(s)s, (uint32_t)%(b)s)")
    def op_srw(self, i): return self._x(i, "(uint64_t)ppc_srw((uint32_t)%(s)s, (uint32_t)%(b)s)")
    def op_cntlzw(self, i): return self._x(i, "(uint64_t)ppc_cntlzw((uint32_t)%(s)s)")
    def op_extsh(self, i): return self._x(i, "(uint64_t)(int64_t)(int16_t)%(s)s")
    def op_extsb(self, i): return self._x(i, "(uint64_t)(int64_t)(int8_t)%(s)s")
    def op_extsw(self, i): return self._x(i, "(uint64_t)(int64_t)(int32_t)%(s)s")
    def op_cntlzd(self, i): return self._x(i, "ppc_cntlzd(%(s)s)")
    def op_sld(self, i): return self._x(i, "ppc_sld(%(s)s, %(b)s)")
    def op_srd(self, i): return self._x(i, "ppc_srd(%(s)s, %(b)s)")
    def op_srad(self, i): return self._x(i, "ppc_srad(%(s)s, %(b)s, &c->xer_ca)")
    def op_sradi(self, i): return self._x(i, "ppc_sradi(%%(s)s, %d, &c->xer_ca)" % i.ext.sh)

    # rotates (MD / MDS forms)
    def _rot(self, i, sh):
        return "ppc_rotl64(%s, %s)" % (R(i.rd), sh)

    def op_rldicl(self, i):
        e = i.ext
        return "%s = %s & %s;%s" % (R(i.ra), self._rot(i, e.sh), k64(mask64(e.mb, 63)), self.rec(i, i.ra))

    def op_rldicr(self, i):
        e = i.ext  # the 6-bit field is ME here
        return "%s = %s & %s;%s" % (R(i.ra), self._rot(i, e.sh), k64(mask64(0, e.mb)), self.rec(i, i.ra))

    def op_rldic(self, i):
        e = i.ext
        return "%s = %s & %s;%s" % (R(i.ra), self._rot(i, e.sh), k64(mask64(e.mb, 63 - e.sh)), self.rec(i, i.ra))

    def op_rldimi(self, i):
        e = i.ext
        m = mask64(e.mb, 63 - e.sh)
        return "%s = (%s & %s) | (%s & %s);%s" % (R(i.ra), self._rot(i, e.sh), k64(m), R(i.ra), k64(~m),
                                                  self.rec(i, i.ra))

    def op_rldcl(self, i):
        e = i.ext
        return "%s = %s & %s;%s" % (R(i.ra), self._rot(i, "(unsigned)(%s & 63)" % R(i.rb)), k64(mask64(e.mb, 63)),
                                    self.rec(i, i.ra))

    def op_rldcr(self, i):
        e = i.ext
        return "%s = %s & %s;%s" % (R(i.ra), self._rot(i, "(unsigned)(%s & 63)" % R(i.rb)), k64(mask64(0, e.mb)),
                                    self.rec(i, i.ra))

    # loads and stores: 32-bit effective addresses, 64-bit registers
    def _load(self, i, kind, ea, update):
        fn, conv = {"lwz": ("RC_LWZ", "(uint64_t)"), "lbz": ("RC_LBZ", "(uint64_t)"), "lhz": ("RC_LHZ", "(uint64_t)"),
                    "lha": ("RC_LHZ", "(uint64_t)(int64_t)(int16_t)"), "ld": ("RC_LD64", ""),
                    "lwa": ("RC_LWZ", "(uint64_t)(int64_t)(int32_t)")}[kind]
        if update:
            return "{ uint32_t ea_ = (uint32_t)(%s); %s = %s%s(ea_); %s = ea_; }" % (ea, R(i.rd), conv, fn, R(i.ra))
        return "%s = %s%s((uint32_t)(%s));" % (R(i.rd), conv, fn, ea)

    def _store(self, i, kind, ea, update):
        fn = {"stw": "RC_STW", "stb": "RC_STB", "sth": "RC_STH", "std": "RC_ST64"}[kind]
        cast = "" if kind == "std" else "(uint32_t)"
        if update:
            return "{ uint32_t ea_ = (uint32_t)(%s); %s(ea_, %s%s); %s = ea_; }" % (ea, fn, cast, R(i.rd), R(i.ra))
        return "%s((uint32_t)(%s), %s%s);" % (fn, ea, cast, R(i.rd))

    def _ea_ds(self, i):
        d = i.ext.ds
        return "%s + %s" % (RA0(i.ra), hx(d)) if d else RA0(i.ra)

    def op_ld(self, i): return self._load(i, "ld", self._ea_ds(i), False)
    def op_ldu(self, i): return self._load(i, "ld", "%s + %s" % (R(i.ra), hx(i.ext.ds)), True)
    def op_lwa(self, i): return self._load(i, "lwa", self._ea_ds(i), False)
    def op_std(self, i): return self._store(i, "std", self._ea_ds(i), False)
    def op_stdu(self, i): return self._store(i, "std", "%s + %s" % (R(i.ra), hx(i.ext.ds)), True)

    def _x64mem(self, i):
        n = i.name
        upd = n.endswith("ux")
        ea = "%s + %s" % (R(i.ra) if upd else RA0(i.ra), R(i.rb))
        kind = {"ldx": "ld", "ldux": "ld", "lwax": "lwa", "lwaux": "lwa", "stdx": "std", "stdux": "std"}[n]
        if kind == "std":
            return self._store(i, kind, ea, upd)
        return self._load(i, kind, ea, upd)

    op_ldx = op_ldux = op_lwax = op_lwaux = op_stdx = op_stdux = _x64mem

    def op_ldbrx(self, i): return "%s = __builtin_bswap64(RC_LD64((uint32_t)(%s)));" % (R(i.rd), self._eax(i))
    def op_stdbrx(self, i): return "RC_ST64((uint32_t)(%s), __builtin_bswap64(%s));" % (self._eax(i), R(i.rd))
    def op_lwbrx(self, i): return "%s = __builtin_bswap32(RC_LWZ((uint32_t)(%s)));" % (R(i.rd), self._eax(i))
    def op_lhbrx(self, i): return "%s = __builtin_bswap16((uint16_t)RC_LHZ((uint32_t)(%s)));" % (R(i.rd), self._eax(i))
    def op_stwbrx(self, i): return "RC_STW((uint32_t)(%s), __builtin_bswap32((uint32_t)%s));" % (self._eax(i), R(i.rd))

    def op_sthbrx(self, i):
        return "RC_STH((uint32_t)(%s), __builtin_bswap16((uint16_t)%s));" % (self._eax(i), R(i.rd))

    def op_ldarx(self, i):
        return "%s = RC_LD64((uint32_t)(%s)); c->reserve = 1;" % (R(i.rd), self._eax(i))

    def op_stdcx_rec(self, i):
        return ("if (c->reserve) { RC_ST64((uint32_t)(%s), %s); c->reserve = 0; "
                "c->cr = ppc_cr_set(c->cr, 0, PPC_CR_EQ | c->xer_so); } "
                "else c->cr = ppc_cr_set(c->cr, 0, c->xer_so);" % (self._eax(i), R(i.rd)))

    def op_dcbz128(self, i): return "RC_DCBZ128((uint32_t)(%s));" % self._eax(i)

    def op_dcbz(self, i): return "RC_DCBZ((uint32_t)(%s));" % self._eax(i)

    # FP conversions to / from 64-bit integers
    def op_fctid(self, i): return "F0(%d) = ppc_u2f((uint64_t)ppc_fctid(F0(%d), 0));" % (i.rd, i.rb)
    def op_fctidz(self, i): return "F0(%d) = ppc_u2f((uint64_t)ppc_fctid(F0(%d), 1));" % (i.rd, i.rb)
    def op_fcfid(self, i): return "F0(%d) = (double)(int64_t)ppc_f2u(F0(%d));" % (i.rd, i.rb)

    # ---- VMX / VMX128 ----
    def _vea(self, i):
        return "(uint32_t)(%s + %s)" % (RA0(i.ra), R(i.rb))

    def _vmem(self, i):
        n = i.name.replace("128", "")
        e = i.ext
        ea = self._vea(i)
        if n in ("lvx", "lvxl"):
            return "%s = xv_load(m_, %s);" % (V(e.vd), ea)
        if n in ("stvx", "stvxl"):
            return "xv_store(m_, %s, %s);" % (ea, V(e.vd))
        if n in ("lvlx", "lvlxl"):
            return "%s = xv_lvlx(m_, %s);" % (V(e.vd), ea)
        if n in ("lvrx", "lvrxl"):
            return "%s = xv_lvrx(m_, %s);" % (V(e.vd), ea)
        if n in ("stvlx", "stvlxl"):
            return "xv_stvlx(m_, %s, %s);" % (ea, V(e.vd))
        if n in ("stvrx", "stvrxl"):
            return "xv_stvrx(m_, %s, %s);" % (ea, V(e.vd))
        if n == "lvsl":
            return "%s = xv_lvsl(%s);" % (V(e.vd), ea)
        if n == "lvsr":
            return "%s = xv_lvsr(%s);" % (V(e.vd), ea)
        if n in ("lvebx", "lvehx", "lvewx"):
            return "xv_lve(m_, &%s, %s, %d);" % (V(e.vd), ea, {"lvebx": 1, "lvehx": 2, "lvewx": 4}[n])
        if n in ("stvebx", "stvehx", "stvewx"):
            return "xv_stve(m_, %s, %s, %d);" % (ea, V(e.vd), {"stvebx": 1, "stvehx": 2, "stvewx": 4}[n])
        raise AssertionError(n)

    for _n in VEC_LOADSTORE:
        locals()["op_" + _n] = _vmem
    del _n

    def _v2(self, fn, i, a=None, b=None):
        e = i.ext
        return "%s = %s(%s, %s);" % (V(e.vd), fn, V(e.va if a is None else a), V(e.vb if b is None else b))

    def _v1(self, fn, i, extra=None):
        e = i.ext
        if extra is None:
            return "%s = %s(%s);" % (V(e.vd), fn, V(e.vb))
        return "%s = %s(%s, %s);" % (V(e.vd), fn, V(e.vb), extra)

    # float arithmetic
    def op_vaddfp(self, i): return self._v2("xv_addfp", i)
    def op_vsubfp(self, i): return self._v2("xv_subfp", i)
    def op_vmaxfp(self, i): return self._v2("xv_maxfp", i)
    def op_vminfp(self, i): return self._v2("xv_minfp", i)
    op_vaddfp128, op_vsubfp128, op_vmaxfp128, op_vminfp128 = op_vaddfp, op_vsubfp, op_vmaxfp, op_vminfp
    def op_vmulfp128(self, i): return self._v2("xv_mulfp", i)
    def op_vmsum3fp128(self, i): return self._v2("xv_msum3fp", i)
    def op_vmsum4fp128(self, i): return self._v2("xv_msum4fp", i)

    def _v3(self, fn, i, a, c, b):
        return "%s = %s(%s, %s, %s);" % (V(i.ext.vd), fn, V(a), V(c), V(b))

    def op_vmaddfp(self, i): return self._v3("xv_maddfp", i, i.ext.va, i.ext.vc, i.ext.vb)       # a*c + b
    def op_vnmsubfp(self, i): return self._v3("xv_nmsubfp", i, i.ext.va, i.ext.vc, i.ext.vb)     # -(a*c - b)
    def op_vmaddfp128(self, i): return self._v3("xv_maddfp", i, i.ext.va, i.ext.vb, i.ext.vd)    # a*b + d
    def op_vmaddcfp128(self, i): return self._v3("xv_maddfp", i, i.ext.va, i.ext.vd, i.ext.vb)   # a*d + b
    def op_vnmsubfp128(self, i): return self._v3("xv_nmsubfp", i, i.ext.va, i.ext.vb, i.ext.vd)  # -(a*b - d)

    def op_vrefp(self, i): return self._v1("xv_refp", i)
    def op_vrsqrtefp(self, i): return self._v1("xv_rsqrtefp", i)
    op_vrefp128, op_vrsqrtefp128 = op_vrefp, op_vrsqrtefp

    def op_vrfin(self, i): return self._v1("xv_rfi", i, "0")
    def op_vrfiz(self, i): return self._v1("xv_rfi", i, "1")
    def op_vrfip(self, i): return self._v1("xv_rfi", i, "2")
    def op_vrfim(self, i): return self._v1("xv_rfi", i, "3")
    op_vrfin128, op_vrfiz128, op_vrfip128, op_vrfim128 = op_vrfin, op_vrfiz, op_vrfip, op_vrfim

    # conversions: AltiVec puts the immediate in vA, VMX128 in its IMM field
    def _imm(self, i):
        e = i.ext
        return e.va if e.form in ("vx", "vc") else e.imm

    def op_vcfsx(self, i): return self._v1("xv_cfsx", i, str(self._imm(i)))
    def op_vcfux(self, i): return self._v1("xv_cfux", i, str(self._imm(i)))
    def op_vctsxs(self, i): return "%s = xv_ctsxs(%s, %d, &c->vscr);" % (V(i.ext.vd), V(i.ext.vb), self._imm(i))
    def op_vctuxs(self, i): return "%s = xv_ctuxs(%s, %d, &c->vscr);" % (V(i.ext.vd), V(i.ext.vb), self._imm(i))
    op_vcsxwfp128, op_vcuxwfp128, op_vcfpsxws128, op_vcfpuxws128 = op_vcfsx, op_vcfux, op_vctsxs, op_vctuxs

    # compares (record forms set CR6: all true -> 0b1000, none true -> 0b0010)
    def _vcmp(self, fn, i):
        e = i.ext
        s = "%s = %s(%s, %s);" % (V(e.vd), fn, V(e.va), V(e.vb))
        if e.rc:
            s += " c->cr = ppc_cr_set(c->cr, 6, xv_cr6(%s));" % V(e.vd)
        return s

    def op_vcmpeqfp(self, i): return self._vcmp("xv_cmpeqfp", i)
    def op_vcmpgefp(self, i): return self._vcmp("xv_cmpgefp", i)
    def op_vcmpgtfp(self, i): return self._vcmp("xv_cmpgtfp", i)
    def op_vcmpbfp(self, i): return self._vcmp("xv_cmpbfp", i)
    def op_vcmpequw(self, i): return self._vcmp("xv_cmpequw", i)
    def op_vcmpgtuw(self, i): return self._vcmp("xv_cmpgtuw", i)
    def op_vcmpgtsw(self, i): return self._vcmp("xv_cmpgtsw", i)
    op_vcmpeqfp128, op_vcmpgefp128, op_vcmpgtfp128 = op_vcmpeqfp, op_vcmpgefp, op_vcmpgtfp
    op_vcmpbfp128, op_vcmpequw128 = op_vcmpbfp, op_vcmpequw

    # logic
    def op_vand(self, i): return self._v2("xv_and", i)
    def op_vandc(self, i): return self._v2("xv_andc", i)
    def op_vor(self, i): return self._v2("xv_or", i)
    def op_vnor(self, i): return self._v2("xv_nor", i)
    def op_vxor(self, i): return self._v2("xv_xor", i)
    op_vand128, op_vandc128, op_vor128, op_vnor128, op_vxor128 = op_vand, op_vandc, op_vor, op_vnor, op_vxor

    def op_vsel(self, i): return self._v3("xv_sel", i, i.ext.va, i.ext.vb, i.ext.vc)
    def op_vsel128(self, i): return self._v3("xv_sel", i, i.ext.va, i.ext.vb, i.ext.vd)

    # integer word arithmetic and shifts
    def op_vadduwm(self, i): return self._v2("xv_adduwm", i)
    def op_vsubuwm(self, i): return self._v2("xv_subuwm", i)
    def op_vslw(self, i): return self._v2("xv_slw", i)
    def op_vsrw(self, i): return self._v2("xv_srw", i)
    def op_vsraw(self, i): return self._v2("xv_sraw", i)
    def op_vrlw(self, i): return self._v2("xv_rlw", i)
    op_vslw128, op_vsrw128, op_vsraw128, op_vrlw128 = op_vslw, op_vsrw, op_vsraw, op_vrlw
    def op_vmaxsw(self, i): return self._v2("xv_maxsw", i)
    def op_vminsw(self, i): return self._v2("xv_minsw", i)

    # permutes, merges, splats
    def op_vperm(self, i): return self._v3("xv_perm", i, i.ext.va, i.ext.vb, i.ext.vc)
    def op_vperm128(self, i): return self._v3("xv_perm", i, i.ext.va, i.ext.vb, i.ext.vc)

    def op_vsldoi(self, i):
        e = i.ext
        return "%s = xv_sldoi(%s, %s, %d);" % (V(e.vd), V(e.va), V(e.vb), e.vc & 15)

    def op_vsldoi128(self, i):
        e = i.ext
        return "%s = xv_sldoi(%s, %s, %d);" % (V(e.vd), V(e.va), V(e.vb), e.sh)

    def op_vslo(self, i): return self._v2("xv_slo", i)
    def op_vsro(self, i): return self._v2("xv_sro", i)
    op_vslo128, op_vsro128 = op_vslo, op_vsro
    def op_vmrghw(self, i): return self._v2("xv_mrghw", i)
    def op_vmrglw(self, i): return self._v2("xv_mrglw", i)
    op_vmrghw128, op_vmrglw128 = op_vmrghw, op_vmrglw

    def op_vspltw(self, i): return self._v1("xv_spltw", i, str(self._imm(i) & 3))
    op_vspltw128 = op_vspltw
    def op_vsplth(self, i): return self._v1("xv_splth", i, str(i.ext.va & 7))
    def op_vspltb(self, i): return self._v1("xv_spltb", i, str(i.ext.va & 15))

    def op_vspltisw(self, i):
        return "%s = xv_splat32(0x%08Xu);" % (V(i.ext.vd), sext(self._imm(i), 5) & 0xFFFFFFFF)

    op_vspltisw128 = op_vspltisw

    def op_vspltish(self, i):
        v = sext(i.ext.va, 5) & 0xFFFF
        return "%s = xv_splat32(0x%08Xu);" % (V(i.ext.vd), (v << 16) | v)

    def op_vspltisb(self, i):
        v = sext(i.ext.va, 5) & 0xFF
        return "%s = xv_splat32(0x%08Xu);" % (V(i.ext.vd), v * 0x01010101)

    def op_vpermwi128(self, i):
        e = i.ext
        p = e.imm
        return "%s = xv_permwi(%s, %d, %d, %d, %d);" % (V(e.vd), V(e.vb), (p >> 6) & 3, (p >> 4) & 3, (p >> 2) & 3,
                                                       p & 3)

    def op_vrlimi128(self, i):
        e = i.ext
        return "%s = xv_rlimi(%s, %s, %d, %d);" % (V(e.vd), V(e.vd), V(e.vb), e.imm, e.z)

    def op_mfvscr(self, i): return "%s = xv_words(0, 0, 0, c->vscr);" % V(i.ext.vd)
    def op_mtvscr(self, i): return "c->vscr = %s.u32[3];" % V(i.ext.vb)
