"""Allegrex decoder (XBA-224): ps1_tools' MIPS I decoder (isa.Ins), extended with what the PSP's CPU adds and with
the encodings it gives another meaning. Pure Python, our own table (encodings from the MIPS32 manuals and the
Allegrex's documented additions).

Unchanged from the R3000A: the MIPS I integer set (ALU, shifts, HI / LO, loads and stores with LWL / LWR / SWL /
SWR, branches, J / JAL / JR / JALR, SYSCALL / BREAK). Added (classes as in ps1_tools.isa, plus):

  alu      MOVZ / MOVN (read rd too), CLZ / CLO, MAX / MIN, ROTR / ROTRV, SPECIAL3 EXT / INS (INS reads rt),
           SEB / SEH / WSBH / WSBW / BITREV
  muldiv   MADD / MADDU / MSUB / MSUBU (HI / LO accumulate)
  branch   the likely branches (BEQL BNEL BLEZL BGTZL BLTZL BGEZL BLTZALL BGEZALL: the delay slot runs only when the
           branch is taken; `likely`), BC1F / BC1T / BC1FL / BC1TL (FPU condition)
  load     LL (a plain LW here: one CPU, no other bus master that matters), no load delay on the Allegrex
  store    SC (stores, then rt = 1)
  fpu      the single-precision FPU: ADD.S SUB.S MUL.S DIV.S SQRT.S ABS.S MOV.S NEG.S, ROUND / TRUNC / CEIL / FLOOR
           .W.S, CVT.S.W, CVT.W.S, C.cond.S, MFC1 / MTC1, CFC1 / CTC1 (FCR31 only), LWC1 / SWC1 (`fls`)
  nop      SYNC, CACHE (no caches modelled)
  interp   the VFPU (COP2 transfers, LV / SV, every VFPU op), HALT, MFIC / MTIC, COP0, reserved encodings

The R3000A meaning that breaks on the Allegrex: opcode 0x12 is the VFPU, not the GTE; 0x32 / 0x3A are LV.S / SV.S,
not LWC2 / SWC2; 0x14-0x17 and REGIMM 2 / 3 / 0x12 / 0x13 are likely branches, not reserved; there is no load delay
slot (conflicts() never applies).
"""

from ps1_tools import isa as _ps1
from ps1_tools.isa import RA, REG_NAMES, Ins

FMT_S, FMT_W = 16, 20
FPU_S = {0x00: "add.s", 0x01: "sub.s", 0x02: "mul.s", 0x03: "div.s", 0x04: "sqrt.s", 0x05: "abs.s", 0x06: "mov.s",
         0x07: "neg.s", 0x0C: "round.w.s", 0x0D: "trunc.w.s", 0x0E: "ceil.w.s", 0x0F: "floor.w.s",
         0x24: "cvt.w.s"}
FPU_COND = ["f", "un", "eq", "ueq", "olt", "ult", "ole", "ule", "sf", "ngle", "seq", "ngl", "lt", "nge", "le", "ngt"]
SPECIAL_EXTRA = {0x0A: "movz", 0x0B: "movn", 0x0F: "sync", 0x16: "clz", 0x17: "clo", 0x1C: "madd", 0x1D: "maddu",
                 0x2C: "max", 0x2D: "min", 0x2E: "msub", 0x2F: "msubu"}
BSHFL = {0x02: "wsbh", 0x03: "wsbw", 0x10: "seb", 0x14: "bitrev", 0x18: "seh"}
LIKELY = {0x14: "beql", 0x15: "bnel", 0x16: "blezl", 0x17: "bgtzl"}
REGIMM = {0x00: "bltz", 0x01: "bgez", 0x02: "bltzl", 0x03: "bgezl", 0x10: "bltzal", 0x11: "bgezal",
          0x12: "bltzall", 0x13: "bgezall"}
VFPU_OPS = (0x12, 0x18, 0x19, 0x1B, 0x32, 0x34, 0x35, 0x36, 0x37, 0x3A, 0x3C, 0x3D, 0x3E, 0x3F)


class AllegrexIns(Ins):
    __slots__ = ("likely", "fs", "ft", "fd")

    def __init__(self, addr, word):
        self.likely = False
        self.fs = self.ft = self.fd = 0
        super().__init__(addr, word)  # the R3000A decode first (ps1_tools.isa._decode) ...
        _decode_allegrex(self)        # ... then what the Allegrex decodes differently

    def reads(self):
        n = self.name
        if n in ("movz", "movn"):
            return {self.rs, self.rt, self.rd}
        if n in ("clz", "clo"):
            return {self.rs}
        if n in ("rotr",) or n in BSHFL.values():
            return {self.rt}
        if n == "rotrv":
            return {self.rs, self.rt}
        if n == "ext":
            return {self.rs}
        if n == "ins":
            return {self.rs, self.rt}
        if n in ("madd", "maddu", "msub", "msubu", "max", "min"):
            return {self.rs, self.rt}
        if self.likely and self.cls == "branch":
            return {self.rs, self.rt} if n in ("beql", "bnel") else {self.rs}
        if n in ("ll",):
            return {self.rs}
        if n == "sc":
            return {self.rs, self.rt}
        if n in ("mtc1", "ctc1"):
            return {self.rt}
        if n in ("lwc1", "swc1"):
            return {self.rs}
        if self.cls in ("fpu", "fls", "nop") or n.startswith("bc1"):
            return set()
        return super().reads()

    def dest(self):
        n = self.name
        if n in ("ext", "ins"):
            return self.rt or None
        if n in BSHFL.values() or n in ("movz", "movn", "clz", "clo", "max", "min", "rotr", "rotrv"):
            return self.rd or None
        if n in ("mfc1", "cfc1", "sc"):
            return self.rt or None
        if self.cls in ("fpu", "fls", "nop") or n in ("madd", "maddu", "msub", "msubu"):
            return None
        return super().dest()

    def ldest(self):
        if self.name == "ll":
            return self.rt or None
        if self.cls in ("fpu", "fls", "nop"):
            return None
        return super().ldest()

    def is_unconditional(self):
        if self.name == "beql" and self.rs == self.rt:
            return True
        if self.name in ("bgezl", "bgezall") and self.rs == 0:
            return True
        return super().is_unconditional()

    def text(self):
        return disasm(self)


def _branch_target(i):
    return (i.addr + 4 + (i.simm << 2)) & 0xFFFFFFFF


def _decode_allegrex(i):
    w = i.word
    op = w >> 26
    if op == 0:
        f = w & 63
        if f == 0x02 and i.rs == 1:
            i.name, i.cls = "rotr", "alu"
        elif f == 0x06 and i.sa == 1:
            i.name, i.cls = "rotrv", "alu"
        elif f in SPECIAL_EXTRA:
            n = SPECIAL_EXTRA[f]
            i.name = n
            i.cls = "nop" if n == "sync" else "muldiv" if n in ("madd", "maddu", "msub", "msubu") else "alu"
        return
    if op == 1:
        n = REGIMM.get(i.rt)
        i.name, i.cls = (n, "branch") if n else ("reserved", "interp")
        i.target = _branch_target(i) if n else None
        i.link = RA if n and "al" in n[3:] else None
        i.likely = bool(n) and n.endswith("l") and n != "bltzal" and n != "bgezal"
        return
    if op in LIKELY:
        i.name, i.cls, i.likely = LIKELY[op], "branch", True
        i.target = _branch_target(i)
        return
    if op == 0x1F:  # SPECIAL3
        f = w & 63
        i.cls = "alu"
        if f == 0x00:
            i.name = "ext"
        elif f == 0x04:
            i.name = "ins"
        elif f == 0x20 and i.sa in BSHFL:
            i.name = BSHFL[i.sa]
        else:
            i.name, i.cls = "reserved", "interp"
        return
    if op == 0x1C:  # SPECIAL2: HALT, MFIC, MTIC
        i.name = {0x00: "halt", 0x24: "mfic", 0x26: "mtic"}.get(w & 63, "reserved")
        i.cls = "interp"
        return
    if op == 0x11:
        _decode_cop1(i)
        return
    if op in VFPU_OPS:
        i.name, i.cls, i.target, i.link = "vfpu", "interp", None, None
        return
    if op == 0x13:
        i.name, i.cls = "reserved", "interp"
        return
    if op == 0x2F:
        i.name, i.cls = "cache", "nop"
    elif op == 0x30:
        i.name, i.cls = "ll", "load"
    elif op == 0x38:
        i.name, i.cls = "sc", "store"
    elif op == 0x31:
        i.name, i.cls = "lwc1", "fls"
        i.ft = i.rt
    elif op == 0x39:
        i.name, i.cls = "swc1", "fls"
        i.ft = i.rt


def _decode_cop1(i):
    w = i.word
    fmt = i.rs
    i.ft, i.fs, i.fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    i.cls = "fpu"
    if fmt in (0, 2, 4, 6):
        i.name = {0: "mfc1", 2: "cfc1", 4: "mtc1", 6: "ctc1"}[fmt]
        if fmt in (2, 6) and i.fs not in (0, 31):
            i.name, i.cls = "reserved", "interp"
        return
    if fmt == 8:
        n = {0: "bc1f", 1: "bc1t", 2: "bc1fl", 3: "bc1tl"}.get(i.rt & 3)
        i.name, i.cls, i.likely = n, "branch", n.endswith("l")
        i.target = _branch_target(i)
        return
    f = w & 63
    if fmt == FMT_S:
        if f in FPU_S:
            i.name = FPU_S[f]
            return
        if f >= 0x30:
            i.name = "c.%s.s" % FPU_COND[f & 15]
            return
    elif fmt == FMT_W and f == 0x20:
        i.name = "cvt.s.w"
        return
    i.name, i.cls = "reserved", "interp"


def decode(word, addr=0):
    return AllegrexIns(addr, word)


def disasm(i):
    R = REG_NAMES
    n = i.name
    if n in ("movz", "movn", "max", "min"):
        return "%s %s, %s, %s" % (n, R[i.rd], R[i.rs], R[i.rt])
    if n in ("clz", "clo"):
        return "%s %s, %s" % (n, R[i.rd], R[i.rs])
    if n == "rotr":
        return "rotr %s, %s, %d" % (R[i.rd], R[i.rt], i.sa)
    if n == "rotrv":
        return "rotrv %s, %s, %s" % (R[i.rd], R[i.rt], R[i.rs])
    if n in BSHFL.values():
        return "%s %s, %s" % (n, R[i.rd], R[i.rt])
    if n == "ext":
        return "ext %s, %s, %d, %d" % (R[i.rt], R[i.rs], i.sa, i.rd + 1)
    if n == "ins":
        return "ins %s, %s, %d, %d" % (R[i.rt], R[i.rs], i.sa, i.rd + 1 - i.sa)
    if n in ("madd", "maddu", "msub", "msubu"):
        return "%s %s, %s" % (n, R[i.rs], R[i.rt])
    if i.likely and i.cls == "branch" and not n.startswith("bc1"):
        if n in ("beql", "bnel"):
            return "%s %s, %s, 0x%08X" % (n, R[i.rs], R[i.rt], i.target)
        return "%s %s, 0x%08X" % (n, R[i.rs], i.target)
    if n.startswith("bc1"):
        return "%s 0x%08X" % (n, i.target)
    if n in ("ll", "sc"):
        return "%s %s, %d(%s)" % (n, R[i.rt], i.simm, R[i.rs])
    if n in ("lwc1", "swc1"):
        return "%s $f%d, %d(%s)" % (n, i.ft, i.simm, R[i.rs])
    if n in ("mfc1", "mtc1"):
        return "%s %s, $f%d" % (n, R[i.rt], i.fs)
    if n in ("cfc1", "ctc1"):
        return "%s %s, $fcr%d" % (n, R[i.rt], i.fs)
    if n == "cvt.s.w" or n in FPU_S.values():
        if n in ("add.s", "sub.s", "mul.s", "div.s"):
            return "%s $f%d, $f%d, $f%d" % (n, i.fd, i.fs, i.ft)
        return "%s $f%d, $f%d" % (n, i.fd, i.fs)
    if n.startswith("c."):
        return "%s $f%d, $f%d" % (n, i.fs, i.ft)
    if n in ("sync", "cache", "halt", "vfpu", "mfic", "mtic"):
        return "%s (0x%08X)" % (n, i.word)
    return _ps1.disasm(i)
