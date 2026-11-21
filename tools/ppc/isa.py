"""PowerPC instruction decoder (XBA-225): the 32-bit user and supervisor instruction set of the 750 family.

Shared by the GameCube (Gekko) and, later, the Xbox 360 (Xenon) frontends. This module knows the base architecture
only; a console layer subclasses `Decoder` and claims the opcodes the base leaves open through `decode_ext`:

- primary opcode 4: Gekko paired singles on the GameCube, VMX / VMX128 on the Xbox 360;
- primary opcodes 56, 57, 60, 61: psq_l / psq_lu / psq_st / psq_stu on the Gekko; lq, the 64-bit FP pair loads and
  stq / the DS-form 64-bit stores on Xenon;
- 64-bit forms (opcodes 2, 30, 58, 62, the doubleword ops of 31) are not decoded here: a 64-bit layer would add them.

`Insn.name` is the base mnemonic (no simplified forms: `bc` not `beq`, `rlwinm` not `slwi`), `Insn.kind` what the
discovery needs to know about control flow, `Insn.text` a readable disassembly for comments.
"""


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v & ((1 << bits) - 1) ^ m) - m


# kind: control-flow class
NORMAL, BRANCH, BCOND, BCLR, BCCTR, RFI, SC, TRAP, INVALID = (
    "normal", "b", "bc", "bclr", "bcctr", "rfi", "sc", "trap", "invalid")

D_FORM = {
    7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.", 14: "addi", 15: "addis",
    20: "rlwimi", 21: "rlwinm", 23: "rlwnm", 24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.",
    29: "andis.", 32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
    40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw", 48: "lfs",
    49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu", 3: "twi",
}

# opcode 31, 10-bit extended opcode
X31 = {
    0: "cmp", 4: "tw", 19: "mfcr", 20: "lwarx", 23: "lwzx", 24: "slw", 26: "cntlzw", 28: "and", 32: "cmpl",
    54: "dcbst", 55: "lwzux", 60: "andc", 83: "mfmsr", 86: "dcbf", 87: "lbzx", 119: "lbzux", 124: "nor",
    144: "mtcrf", 146: "mtmsr", 150: "stwcx.", 151: "stwx", 183: "stwux", 210: "mtsr", 215: "stbx", 242: "mtsrin",
    246: "dcbtst", 247: "stbux", 278: "dcbt", 279: "lhzx", 284: "eqv", 306: "tlbie", 310: "eciwx", 311: "lhzux",
    316: "xor", 339: "mfspr", 343: "lhax", 370: "tlbia", 371: "mftb", 375: "lhaux", 407: "sthx", 412: "orc",
    438: "ecowx", 439: "sthux", 444: "or", 467: "mtspr", 470: "dcbi", 476: "nand", 512: "mcrxr", 533: "lswx",
    534: "lwbrx", 535: "lfsx", 536: "srw", 566: "tlbsync", 567: "lfsux", 595: "mfsr", 597: "lswi", 598: "sync",
    599: "lfdx", 631: "lfdux", 659: "mfsrin", 661: "stswx", 662: "stwbrx", 663: "stfsx", 695: "stfsux",
    725: "stswi", 727: "stfdx", 759: "stfdux", 790: "lhbrx", 792: "sraw", 824: "srawi", 854: "eieio",
    918: "sthbrx", 922: "extsh", 954: "extsb", 982: "icbi", 983: "stfiwx", 1014: "dcbz",
}
# opcode 31 XO-form arithmetic (9-bit extended opcode, OE in bit 10)
XO31 = {
    8: "subfc", 10: "addc", 11: "mulhwu", 40: "subf", 75: "mulhw", 104: "neg", 136: "subfe", 138: "adde",
    200: "subfze", 202: "addze", 232: "subfme", 234: "addme", 235: "mullw", 266: "add", 459: "divwu", 491: "divw",
}
X19 = {
    0: "mcrf", 16: "bclr", 33: "crnor", 50: "rfi", 129: "crandc", 150: "isync", 193: "crxor", 225: "crnand",
    257: "crand", 289: "creqv", 417: "crorc", 449: "cror", 528: "bcctr",
}
A59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls", 28: "fmsubs", 29: "fmadds",
       30: "fnmsubs", 31: "fnmadds"}
A63 = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul", 26: "frsqrte", 28: "fmsub",
       29: "fmadd", 30: "fnmsub", 31: "fnmadd"}
X63 = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 38: "mtfsb1", 40: "fneg", 64: "mcrfs",
       70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs", 264: "fabs", 583: "mffs", 711: "mtfsf"}

SPR_NAMES = {1: "xer", 8: "lr", 9: "ctr", 22: "dec", 26: "srr0", 27: "srr1", 268: "tbl", 269: "tbu",
             284: "tbl", 285: "tbu", 1008: "hid0", 1009: "hid1", 920: "hid2"}


class Insn:
    __slots__ = ("addr", "word", "op", "name", "kind", "rd", "ra", "rb", "rc", "mb", "me", "simm", "uimm",
                 "rec", "oe", "lk", "aa", "spr", "target", "ext", "text")

    def __init__(self, addr, word):
        w = word
        self.addr = addr
        self.word = w
        self.op = w >> 26
        self.name = None
        self.kind = NORMAL
        self.rd = (w >> 21) & 31   # rD / rS / frD / frS / BO / crbD / TO
        self.ra = (w >> 16) & 31   # rA / frA / BI / crbA
        self.rb = (w >> 11) & 31   # rB / frB / SH / NB / crbB
        self.rc = (w >> 6) & 31    # frC / MB
        self.mb = (w >> 6) & 31
        self.me = (w >> 1) & 31
        self.simm = ((w & 0xFFFF) ^ 0x8000) - 0x8000   # sext(w & 0xFFFF, 16) without the call (millions a lift)
        self.uimm = w & 0xFFFF
        self.rec = w & 1           # Rc
        self.oe = (w >> 10) & 1
        self.lk = w & 1
        self.aa = (w >> 1) & 1
        self.spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
        self.target = None         # static branch target
        self.ext = None            # console layer's data
        self.text = None

    @property
    def crfd(self):
        return self.rd >> 2

    @property
    def crfs(self):
        return self.ra >> 2

    def __repr__(self):
        return "<%08X %08X %s>" % (self.addr, self.word, self.text or self.name)


class Decoder:
    """Base 32-bit PowerPC. Subclasses override decode_ext(i) for opcodes 4, 56, 57, 60, 61."""

    EXT_OPS = (4, 56, 57, 60, 61)

    def decode(self, word, addr):
        i = Insn(addr, word & 0xFFFFFFFF)
        op = i.op
        if op in self.EXT_OPS:
            self.decode_ext(i)
        elif op == 18:
            i.name, i.kind = "b", BRANCH
            li = sext(word & 0x03FFFFFC, 26)
            i.target = (li if i.aa else addr + li) & 0xFFFFFFFF
        elif op == 16:
            i.name, i.kind = "bc", BCOND
            bd = sext(word & 0xFFFC, 16)
            i.target = (bd if i.aa else addr + bd) & 0xFFFFFFFF
        elif op == 17:
            if word & 2:
                i.name, i.kind = "sc", SC
        elif op == 19:
            i.name = X19.get((word >> 1) & 0x3FF)
            if i.name == "bclr":
                i.kind = BCLR
            elif i.name == "bcctr":
                i.kind = BCCTR
            elif i.name == "rfi":
                i.kind = RFI
        elif op == 31:
            xo = (word >> 1) & 0x3FF
            i.name = XO31.get(xo & 0x1FF) or X31.get(xo)
            if i.name == "tw":
                i.kind = TRAP
        elif op == 59:
            i.name = A59.get((word >> 1) & 31)
        elif op == 63:
            xo5 = (word >> 1) & 31
            i.name = A63.get(xo5) if xo5 & 16 else X63.get((word >> 1) & 0x3FF)
        else:
            i.name = D_FORM.get(op)
            if i.name == "twi":
                i.kind = TRAP
        if i.name is None:
            i.kind = INVALID
        i.text = self.disasm(i)
        return i

    def decode_ext(self, i):
        """Opcodes the base leaves to the console layer: invalid here."""
        i.name = None

    # ---- disassembly (comments only) ----
    def disasm(self, i):
        n = i.name
        if n is None:
            return ".long 0x%08X" % i.word
        if i.ext is not None and hasattr(i.ext, "text"):
            return i.ext.text
        if i.kind == BRANCH:
            return "%s%s 0x%08X" % (n, "l" if i.lk else "", i.target)
        if i.kind == BCOND:
            return "bc%s %d, %d, 0x%08X" % ("l" if i.lk else "", i.rd, i.ra, i.target)
        if i.kind in (BCLR, BCCTR):
            if (i.rd & 0x14) == 0x14:
                return ("blrl" if i.lk else "blr") if i.kind == BCLR else ("bctrl" if i.lk else "bctr")
            return "%s%s %d, %d" % (n, "l" if i.lk else "", i.rd, i.ra)
        if n in ("lwz", "lwzu", "lbz", "lbzu", "stw", "stwu", "stb", "stbu", "lhz", "lhzu", "lha", "lhau", "sth",
                 "sthu", "lmw", "stmw"):
            return "%s r%d, %d(r%d)" % (n, i.rd, i.simm, i.ra)
        if n in ("lfs", "lfsu", "lfd", "lfdu", "stfs", "stfsu", "stfd", "stfdu"):
            return "%s f%d, %d(r%d)" % (n, i.rd, i.simm, i.ra)
        if n in ("addi", "addis", "addic", "addic.", "subfic", "mulli"):
            return "%s r%d, r%d, %d" % (n, i.rd, i.ra, i.simm)
        if n in ("ori", "oris", "xori", "xoris", "andi.", "andis."):
            return "%s r%d, r%d, 0x%X" % (n, i.ra, i.rd, i.uimm)
        if n in ("cmpi", "cmpli"):
            return "%s cr%d, r%d, %d" % (n, i.crfd, i.ra, i.simm if n == "cmpi" else i.uimm)
        if n in ("rlwinm", "rlwimi"):
            return "%s%s r%d, r%d, %d, %d, %d" % (n, "." if i.rec else "", i.ra, i.rd, i.rb, i.mb, i.me)
        if n == "rlwnm":
            return "rlwnm%s r%d, r%d, r%d, %d, %d" % ("." if i.rec else "", i.ra, i.rd, i.rb, i.mb, i.me)
        if n in ("mfspr", "mftb"):
            return "%s r%d, %s" % (n, i.rd, SPR_NAMES.get(i.spr, str(i.spr)))
        if n == "mtspr":
            return "mtspr %s, r%d" % (SPR_NAMES.get(i.spr, str(i.spr)), i.rd)
        if i.op == 31 and n in XO31.values():
            return "%s%s%s r%d, r%d, r%d" % (n, "o" if i.oe else "", "." if i.rec else "", i.rd, i.ra, i.rb)
        if i.op == 31:
            return "%s%s r%d, r%d, r%d" % (n, "." if i.rec and not n.endswith(".") else "", i.rd, i.ra, i.rb)
        if i.op in (59, 63):
            return "%s%s f%d, f%d, f%d, f%d" % (n, "." if i.rec else "", i.rd, i.ra, i.rc, i.rb)
        return "%s %d, %d, %d" % (n, i.rd, i.ra, i.rb)
