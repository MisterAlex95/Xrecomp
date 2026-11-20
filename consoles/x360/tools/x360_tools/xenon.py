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
against the opcode constants in the tests. Semantics: R(n) is 64-bit and 64-bit instructions compute 64 bits, but
titles run in 32-bit mode (MSR[SF] = 0, XBA-270; as Xenia's and XenonRecomp's code generators assume): record forms
set CR0 from the low word, XER[CA] is the carry out of the low word, CTR-decrementing branches test the low word of
CTR, effective addresses are 32-bit (the memory macros take uint32_t); rlw* replicate the low word, sraw / extsh
sign-extend to 64 bits.

The lifter emits the C vocabulary of tools/ppc/lift.py plus V(n) (a 128-bit vector register, union xe_v128 with
PPC element order: element 0 is the first in memory) and the xv_* helpers of consoles/x360/runtime/xenon_ops.h.
Every VMX / VMX128 instruction the decoder knows is lifted (XBA-270: the integer element ops, saturating packs and
sums, the D3D pack ops vpkd3d128 / vupkd3d128, the estimates; float ops honour VSCR[NJ], non-Java mode). Instructions
with no op_ method (traps, mtmsrd, the base's rare system ones) are handed to the runtime with RC_INTERP, and the
recompiler's stats count them (`unlifted`).
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

    def __init__(self, text=True):
        self.with_text = text  # False: no disassembly (Insn.text is None), for lifts without comments

    def disasm(self, i):
        return super().disasm(i) if self.with_text else None

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
        if not self.with_text:
            return None
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


# Traps: MSVC puts `twi 6, rD, 0` / `tdi 6, rD, 0` (trap if zero) before each division. Lifted as the
# test; the word goes to the interpreter only when it traps (x360_interp.c reports the trap and stops the title)
TRAPS = ("tw", "twi", "td", "tdi")

# OE forms (XBA-284): XER[OV] = the overflow, XER[SO] |= OV, before the record form's CR0 reads SO. 32-bit mode: the add
# / subtract family overflows on the low word (the sign of the two addends against the result's, carry in included);
# mullwo when the 64-bit product of the low words does not fit 32 bits; divwo / divwuo on a zero divisor (and
# INT_MIN / -1); the doubleword forms on 64 bits. mulhw / mulhwu / mulhd / mulhdu have no OE form: the bit is ignored.
OE_FORMS = {
    "add": ("a_", "b_"), "addc": ("a_", "b_"), "adde": ("a_", "b_"), "addze": ("a_", "0"), "addme": ("a_", "~0ULL"),
    "subf": ("~a_", "b_"), "subfc": ("~a_", "b_"), "subfe": ("~a_", "b_"), "subfze": ("~a_", "0"),
    "subfme": ("~a_", "~0ULL"), "neg": ("~a_", "0"),
}
OE_OTHER = {
    "mullw": "xe_ov_mullw(a_, b_)", "divw": "xe_ov_divw(a_, b_)", "divwu": "((uint32_t)b_ == 0)",
    "mulld": "xe_ov_mulld(a_, b_)", "divd": "xe_ov_divd(a_, b_)", "divdu": "(b_ == 0)",
}


class XenonLifter(FnLifter):
    @staticmethod
    def cond(i):
        """32-bit mode: bdnz and the other CTR-decrementing branches test the low word of CTR."""
        pre, c = FnLifter.cond(i)
        return pre, c.replace("c->ctr == 0", "(uint32_t)c->ctr == 0").replace("c->ctr != 0", "(uint32_t)c->ctr != 0")

    def op_or(self, i):
        # `or 31,31,31` / `or 1,1,1`: the SMT thread-priority hints (very low / low) MSVC's spin-wait loops run while
        # they poll; they change no register. XE_SPIN_HINT (xenon_rt.h) charges virtual time for them, so a busy
        # wait reaches its event in little host time. `or 2,2,2` (back to normal priority) ends the wait: a plain or.
        if i.rd == i.ra == i.rb and i.rd in (1, 31) and not i.rec:
            return "XE_SPIN_HINT();"
        return FnLifter.op_or(self, i)

    def _trap(self, i, b, wide):
        """`if (TO's conditions on rA against b) RC_INTERP(pc);`: the trapping case goes to the interpreter, which
        reports it; TO = 0 never traps, TO = 31 always does."""
        to = i.rd
        if to == 0:
            return "/* trap never taken */"
        if to & 0x1F == 0x1F:
            return "RC_INTERP(%s);" % hx(i.addr)
        s, u = ("int64_t", "uint64_t") if wide else ("int32_t", "uint32_t")
        a = R(i.ra)
        terms = []
        for bit, t, op in ((16, s, "<"), (8, s, ">"), (4, u, "=="), (2, u, "<"), (1, u, ">")):
            if to & bit:
                terms.append("(%s)%s %s (%s)%s" % (t, a, op, t, b))
        return "if (%s) RC_INTERP(%s);" % (" || ".join(terms), hx(i.addr))

    def op_twi(self, i):
        return self._trap(i, "(int64_t)%d" % i.simm, False)

    def op_tdi(self, i):
        return self._trap(i, "(int64_t)%d" % i.simm, True)

    def op_tw(self, i):
        return self._trap(i, R(i.rb), False)

    def op_td(self, i):
        return self._trap(i, R(i.rb), True)

    def op_mtmsrd(self, i):
        # titles use mtmsrd rS, 1 around short critical sections (MSR[EE], external interrupts): the L = 1 form
        # changes only EE and RI (the architecture's definition); xenon_rt.h does not preempt a thread while EE is
        # clear
        return "c->msr = ((uint32_t)%s & 0x8002u) | (c->msr & ~0x8002u);" % R(i.rd)

    def op_mtmsr(self, i):
        return self.op_mtmsrd(i)

    def needs_interp(self, i):
        n = i.name
        if n in TRAPS:
            return False  # op_tw / op_twi / op_td / op_tdi: the test, the interpreter when it traps
        if i.op == 31 and i.oe and (n in OE_FORMS or n in OE_OTHER):
            return False  # lifted with OV / SO (_xo)
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
        return "{ uint64_t a_ = %s; %s = a_ + %s; c->xer_ca = xe_carry(a_, %s, 0); }" % (
            R(i.ra), R(i.rd), k64(i.simm), k64(i.simm))

    def op_addic_rec(self, i):
        return self.op_addic(i) + " RC_CR0(%s);" % R(i.rd)

    def op_subfic(self, i):
        return "{ uint64_t a_ = %s; %s = ~a_ + %s + 1u; c->xer_ca = xe_carry(~a_, %s, 1); }" % (
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
        if i.oe and i.name in OE_FORMS:
            x, y = OE_FORMS[i.name]
            body += " RC_OE(xe_ov_add(%s, %s, %s));" % (x, y, R(i.rd))
        elif i.oe and i.name in OE_OTHER:
            body += " RC_OE(%s);" % OE_OTHER[i.name]
        return "{ uint64_t a_ = %s, b_ = %s; (void)b_; %s }%s" % (R(i.ra), R(i.rb), body, self.rec(i, i.rd))

    def op_addc(self, i): return self._xo(i, "%s = a_ + b_; c->xer_ca = xe_carry(a_, b_, 0);" % R(i.rd))

    def op_subfc(self, i):
        return self._xo(i, "%s = ~a_ + b_ + 1u; c->xer_ca = xe_carry(~a_, b_, 1);" % R(i.rd))

    def op_adde(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = a_ + b_ + ci_; c->xer_ca = xe_carry(a_, b_, ci_);"
                        % R(i.rd))

    def op_subfe(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = ~a_ + b_ + ci_; c->xer_ca = xe_carry(~a_, b_, ci_);"
                        % R(i.rd))

    def op_addze(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = a_ + ci_; c->xer_ca = xe_carry(a_, 0, ci_);"
                        % R(i.rd))

    def op_subfze(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = ~a_ + ci_; c->xer_ca = xe_carry(~a_, 0, ci_);"
                        % R(i.rd))

    def op_addme(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = a_ - 1u + ci_; c->xer_ca = xe_carry(a_, ~0ULL, ci_);"
                        % R(i.rd))

    def op_subfme(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = ~a_ - 1u + ci_; c->xer_ca = xe_carry(~a_, ~0ULL, ci_);"
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

    # reservations (XBA-283): the runtime's xenon_rt.h implements them; stwcx. / stdcx. store only when the word
    # still holds what lwarx / ldarx read (a compare-and-swap), so guest threads on host threads stay atomic
    def op_lwarx(self, i):
        return "%s = (uint64_t)RC_LWARX((uint32_t)(%s));" % (R(i.rd), self._eax(i))

    def op_stwcx_rec(self, i):
        return "RC_STWCX((uint32_t)(%s), (uint32_t)%s);" % (self._eax(i), R(i.rd))

    def op_ldarx(self, i):
        return "%s = RC_LDARX((uint32_t)(%s));" % (R(i.rd), self._eax(i))

    # sync / lwsync / eieio: a host fence (guest threads on host threads: arm64 reorders as the Xenon does)
    def op_sync(self, i):
        return "RC_SYNC();"

    op_eieio = op_sync

    def op_stdcx_rec(self, i):
        return "RC_STDCX((uint32_t)(%s), %s);" % (self._eax(i), R(i.rd))

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

    # float arithmetic. Non-Java mode (XBA-270): every input and the result go through xv_daz, which flushes
    # denormals to zero while VSCR[NJ] is set (xenon_vmx.h).
    @staticmethod
    def _daz(n):
        return "xv_daz(c->vscr, %s)" % V(n)

    def _fv(self, fn, i, regs, extra=""):
        args = ", ".join(self._daz(n) for n in regs)
        return "%s = xv_daz(c->vscr, %s(%s%s));" % (V(i.ext.vd), fn, args, extra)

    def _f2(self, fn, i): return self._fv(fn, i, (i.ext.va, i.ext.vb))
    def _f1(self, fn, i, extra=""): return self._fv(fn, i, (i.ext.vb,), extra)

    def op_vaddfp(self, i): return self._f2("xv_addfp", i)
    def op_vsubfp(self, i): return self._f2("xv_subfp", i)
    def op_vmaxfp(self, i): return self._f2("xv_maxfp", i)
    def op_vminfp(self, i): return self._f2("xv_minfp", i)
    op_vaddfp128, op_vsubfp128, op_vmaxfp128, op_vminfp128 = op_vaddfp, op_vsubfp, op_vmaxfp, op_vminfp
    def op_vmulfp128(self, i): return self._f2("xv_mulfp", i)
    def op_vmsum3fp128(self, i): return self._f2("xv_msum3fp", i)
    def op_vmsum4fp128(self, i): return self._f2("xv_msum4fp", i)

    def _v3(self, fn, i, a, c, b):
        return "%s = %s(%s, %s, %s);" % (V(i.ext.vd), fn, V(a), V(c), V(b))

    def op_vmaddfp(self, i): return self._fv("xv_maddfp", i, (i.ext.va, i.ext.vc, i.ext.vb))      # a*c + b
    def op_vnmsubfp(self, i): return self._fv("xv_nmsubfp", i, (i.ext.va, i.ext.vc, i.ext.vb))    # -(a*c - b)
    def op_vmaddfp128(self, i): return self._fv("xv_maddfp", i, (i.ext.va, i.ext.vb, i.ext.vd))   # a*b + d
    def op_vmaddcfp128(self, i): return self._fv("xv_maddfp", i, (i.ext.va, i.ext.vd, i.ext.vb))  # a*d + b
    def op_vnmsubfp128(self, i): return self._fv("xv_nmsubfp", i, (i.ext.va, i.ext.vb, i.ext.vd))  # -(a*b - d)

    # estimates: exact here (the hardware's tables are not reproduced, xenon_vmx.h)
    def op_vrefp(self, i): return self._f1("xv_refp", i)
    def op_vrsqrtefp(self, i): return self._f1("xv_rsqrtefp", i)
    def op_vexptefp(self, i): return self._f1("xv_exptefp", i)
    def op_vlogefp(self, i): return self._f1("xv_logefp", i)
    op_vrefp128, op_vrsqrtefp128, op_vexptefp128, op_vlogefp128 = op_vrefp, op_vrsqrtefp, op_vexptefp, op_vlogefp

    def op_vrfin(self, i): return self._f1("xv_rfi", i, ", 0")
    def op_vrfiz(self, i): return self._f1("xv_rfi", i, ", 1")
    def op_vrfip(self, i): return self._f1("xv_rfi", i, ", 2")
    def op_vrfim(self, i): return self._f1("xv_rfi", i, ", 3")
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
        if fn.endswith("fp"):  # non-Java mode: denormal operands compare as zero
            s = "%s = %s(%s, %s);" % (V(e.vd), fn, self._daz(e.va), self._daz(e.vb))
        else:
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

    # ---- integer element ops, packs, unpacks, sums (XBA-270; helpers in xenon_vmx.h) ----
    def _vs(self, fn, i):
        """A saturating two-operand op (sets VSCR[SAT])."""
        e = i.ext
        return "%s = %s(%s, %s, &c->vscr);" % (V(e.vd), fn, V(e.va), V(e.vb))

    def op_vcmpequb(self, i): return self._vcmp("xv_cmpequb", i)
    def op_vcmpequh(self, i): return self._vcmp("xv_cmpequh", i)
    def op_vcmpgtub(self, i): return self._vcmp("xv_cmpgtub", i)
    def op_vcmpgtuh(self, i): return self._vcmp("xv_cmpgtuh", i)
    def op_vcmpgtsb(self, i): return self._vcmp("xv_cmpgtsb", i)
    def op_vcmpgtsh(self, i): return self._vcmp("xv_cmpgtsh", i)

    def op_vsl(self, i): return self._v2("xv_sl", i)
    def op_vsr(self, i): return self._v2("xv_sr", i)

    def op_vmrghb(self, i): return "%s = xv_mrgb(%s, %s, 0);" % (V(i.ext.vd), V(i.ext.va), V(i.ext.vb))
    def op_vmrglb(self, i): return "%s = xv_mrgb(%s, %s, 1);" % (V(i.ext.vd), V(i.ext.va), V(i.ext.vb))
    def op_vmrghh(self, i): return "%s = xv_mrgh(%s, %s, 0);" % (V(i.ext.vd), V(i.ext.va), V(i.ext.vb))
    def op_vmrglh(self, i): return "%s = xv_mrgh(%s, %s, 1);" % (V(i.ext.vd), V(i.ext.va), V(i.ext.vb))

    def _mul(self, fn, i, odd, sgn):
        return "%s = %s(%s, %s, %d, %d);" % (V(i.ext.vd), fn, V(i.ext.va), V(i.ext.vb), odd, sgn)

    def op_vmuleub(self, i): return self._mul("xv_mulb", i, 0, 0)
    def op_vmuloub(self, i): return self._mul("xv_mulb", i, 1, 0)
    def op_vmulesb(self, i): return self._mul("xv_mulb", i, 0, 1)
    def op_vmulosb(self, i): return self._mul("xv_mulb", i, 1, 1)
    def op_vmuleuh(self, i): return self._mul("xv_mulh", i, 0, 0)
    def op_vmulouh(self, i): return self._mul("xv_mulh", i, 1, 0)
    def op_vmulesh(self, i): return self._mul("xv_mulh", i, 0, 1)
    def op_vmulosh(self, i): return self._mul("xv_mulh", i, 1, 1)

    def _pk(self, fn, i, mode):
        return "%s = %s(%s, %s, %d, &c->vscr);" % (V(i.ext.vd), fn, V(i.ext.va), V(i.ext.vb), mode)

    def op_vpkuhum(self, i): return self._pk("xv_pkh", i, 0)
    def op_vpkuhus(self, i): return self._pk("xv_pkh", i, 1)
    def op_vpkshus(self, i): return self._pk("xv_pkh", i, 2)
    def op_vpkshss(self, i): return self._pk("xv_pkh", i, 3)
    def op_vpkuwum(self, i): return self._pk("xv_pkw", i, 0)
    def op_vpkuwus(self, i): return self._pk("xv_pkw", i, 1)
    def op_vpkswus(self, i): return self._pk("xv_pkw", i, 2)
    def op_vpkswss(self, i): return self._pk("xv_pkw", i, 3)
    op_vpkuhum128, op_vpkuhus128, op_vpkshus128, op_vpkshss128 = op_vpkuhum, op_vpkuhus, op_vpkshus, op_vpkshss
    op_vpkuwum128, op_vpkuwus128, op_vpkswus128, op_vpkswss128 = op_vpkuwum, op_vpkuwus, op_vpkswus, op_vpkswss
    def op_vpkpx(self, i): return self._v2("xv_pkpx", i)

    def _upk(self, fn, i, lo):
        return "%s = %s(%s, %d);" % (V(i.ext.vd), fn, V(i.ext.vb), lo)

    def op_vupkhsb(self, i): return self._upk("xv_upkb", i, 0)
    def op_vupklsb(self, i): return self._upk("xv_upkb", i, 1)
    def op_vupkhsh(self, i): return self._upk("xv_upkh", i, 0)
    def op_vupklsh(self, i): return self._upk("xv_upkh", i, 1)
    def op_vupkhpx(self, i): return self._upk("xv_upkpx", i, 0)
    def op_vupklpx(self, i): return self._upk("xv_upkpx", i, 1)
    # vupkhsb128 / vupklsb128 with VA = 0x60 are the halfword unpacks (vupkhsh128 / vupklsh128: Xenia and
    # XenonRecomp both decode them that way)
    def op_vupkhsb128(self, i): return self._upk("xv_upkh" if i.ext.va == 0x60 else "xv_upkb", i, 0)
    def op_vupklsb128(self, i): return self._upk("xv_upkh" if i.ext.va == 0x60 else "xv_upkb", i, 1)

    def op_vsum4ubs(self, i): return self._vs("xv_sum4ubs", i)
    def op_vsum4sbs(self, i): return self._vs("xv_sum4sbs", i)
    def op_vsum4shs(self, i): return self._vs("xv_sum4shs", i)
    def op_vsum2sws(self, i): return self._vs("xv_sum2sws", i)
    def op_vsumsws(self, i): return self._vs("xv_sumsws", i)

    def _va3(self, i, call):
        e = i.ext
        return "%s = %s;" % (V(e.vd), call % (V(e.va), V(e.vb), V(e.vc)))

    def op_vmhaddshs(self, i): return self._va3(i, "xv_mhaddshs(%s, %s, %s, 0, &c->vscr)")
    def op_vmhraddshs(self, i): return self._va3(i, "xv_mhaddshs(%s, %s, %s, 1, &c->vscr)")
    def op_vmladduhm(self, i): return self._va3(i, "xv_mladduhm(%s, %s, %s)")
    def op_vmsumubm(self, i): return self._va3(i, "xv_msumb(%s, %s, %s, 0)")
    def op_vmsummbm(self, i): return self._va3(i, "xv_msumb(%s, %s, %s, 1)")
    def op_vmsumuhm(self, i): return self._va3(i, "xv_msumh(%s, %s, %s, 0, 0, &c->vscr)")
    def op_vmsumuhs(self, i): return self._va3(i, "xv_msumh(%s, %s, %s, 0, 1, &c->vscr)")
    def op_vmsumshm(self, i): return self._va3(i, "xv_msumh(%s, %s, %s, 1, 0, &c->vscr)")
    def op_vmsumshs(self, i): return self._va3(i, "xv_msumh(%s, %s, %s, 1, 1, &c->vscr)")

    # D3D pack / unpack (VMX128): IMM = type << 2 | pack, z = shift (Xenia's description, xenon_vmx.h)
    def op_vpkd3d128(self, i):
        e = i.ext
        return "%s = xv_pkd3d(%s, %s, %d, %d, %d);" % (V(e.vd), V(e.vd), V(e.vb), e.imm >> 2, e.imm & 3, e.z)

    def op_vupkd3d128(self, i):
        e = i.ext
        return "%s = xv_upkd3d(%s, %d);" % (V(e.vd), V(e.vb), e.imm >> 2)

    def op_vaddubm(self, i): return self._v2("xv_addubm", i)
    def op_vadduhm(self, i): return self._v2("xv_adduhm", i)
    def op_vsububm(self, i): return self._v2("xv_sububm", i)
    def op_vsubuhm(self, i): return self._v2("xv_subuhm", i)
    def op_vaddcuw(self, i): return self._v2("xv_addcuw", i)
    def op_vsubcuw(self, i): return self._v2("xv_subcuw", i)
    def op_vmaxub(self, i): return self._v2("xv_maxub", i)
    def op_vmaxuh(self, i): return self._v2("xv_maxuh", i)
    def op_vmaxuw(self, i): return self._v2("xv_maxuw", i)
    def op_vmaxsb(self, i): return self._v2("xv_maxsb", i)
    def op_vmaxsh(self, i): return self._v2("xv_maxsh", i)
    def op_vminub(self, i): return self._v2("xv_minub", i)
    def op_vminuh(self, i): return self._v2("xv_minuh", i)
    def op_vminuw(self, i): return self._v2("xv_minuw", i)
    def op_vminsb(self, i): return self._v2("xv_minsb", i)
    def op_vminsh(self, i): return self._v2("xv_minsh", i)
    def op_vavgub(self, i): return self._v2("xv_avgub", i)
    def op_vavguh(self, i): return self._v2("xv_avguh", i)
    def op_vavguw(self, i): return self._v2("xv_avguw", i)
    def op_vavgsb(self, i): return self._v2("xv_avgsb", i)
    def op_vavgsh(self, i): return self._v2("xv_avgsh", i)
    def op_vavgsw(self, i): return self._v2("xv_avgsw", i)
    def op_vrlb(self, i): return self._v2("xv_rlb", i)
    def op_vrlh(self, i): return self._v2("xv_rlh", i)
    def op_vslb(self, i): return self._v2("xv_slb", i)
    def op_vslh(self, i): return self._v2("xv_slh", i)
    def op_vsrb(self, i): return self._v2("xv_srb", i)
    def op_vsrh(self, i): return self._v2("xv_srh", i)
    def op_vsrab(self, i): return self._v2("xv_srab", i)
    def op_vsrah(self, i): return self._v2("xv_srah", i)
    def op_vaddubs(self, i): return self._vs("xv_addubs", i)
    def op_vadduhs(self, i): return self._vs("xv_adduhs", i)
    def op_vadduws(self, i): return self._vs("xv_adduws", i)
    def op_vaddsbs(self, i): return self._vs("xv_addsbs", i)
    def op_vaddshs(self, i): return self._vs("xv_addshs", i)
    def op_vaddsws(self, i): return self._vs("xv_addsws", i)
    def op_vsububs(self, i): return self._vs("xv_sububs", i)
    def op_vsubuhs(self, i): return self._vs("xv_subuhs", i)
    def op_vsubuws(self, i): return self._vs("xv_subuws", i)
    def op_vsubsbs(self, i): return self._vs("xv_subsbs", i)
    def op_vsubshs(self, i): return self._vs("xv_subshs", i)
    def op_vsubsws(self, i): return self._vs("xv_subsws", i)

    def op_mfvscr(self, i): return "%s = xv_words(0, 0, 0, c->vscr);" % V(i.ext.vd)
    def op_mtvscr(self, i): return "c->vscr = %s.u32[3];" % V(i.ext.vb)
