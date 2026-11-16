"""R5900 (EE) decoder (XBA-226): ps1_tools' Ins (fields, the discovery's view of an instruction), with a decode of its
own: most MIPS I encodings keep their meaning on the R5900, but enough change (or exist only here) that patching the
R3000A decode, as psp_tools does for the Allegrex, would leave silent mis-decodes. Pure Python, our own table
(encodings from the R5900 / TX79 core manuals).

Classes (ps1_tools.isa's, plus):
  alu      plain GPR write of the low 64 bits (rd or rt): the 32-bit ops sign-extend their result, the D* ops are
           64-bit; MOVZ / MOVN (read rd too), MFHI / MFLO / MFHI1 / MFLO1, PLZCW
  trap     ADD / ADDI / SUB / DADD / DADDI / DSUB (an overflow raises) and the conditional traps (TEQ, TNE, TGE ...,
           TEQI ...: `tcond`)
  muldiv   MULT / MULTU (three-operand: rd = LO too) / DIV / DIVU, the pipeline-1 forms MULT1 / MULTU1 / DIV1 /
           DIVU1 (HI1 / LO1), MADD / MADDU / MADD1 / MADDU1, MTHI / MTLO / MTHI1 / MTLO1
  load     LB LH LW LBU LHU LWU LD LQ (128-bit, address & ~15), LWL LWR LDL LDR; no load delay slot on the R5900
  store    SB SH SW SD SQ SWL SWR SDL SDR
  branch   BEQ BNE BLEZ BGTZ, REGIMM, the likely forms (`likely`: the slot runs only when taken), BC1F / BC1T(L)
  fpu      COP1, single precision only, the R5900's set (ADDA / MULA / MADD / MSUB ..., MAX / MIN, RSQRT, the
           accumulator ACC); `fls`: LWC1 / SWC1
  mmi      the 128-bit multimedia ops the lifter knows (MMI_OPS: `rd = op(rs, rt)`, MMI_SHIFTS: `rd = op(rt, sa)`)
           and PMFHI / PMFLO / PMTHI / PMTLO / PMFHL.LW / PMFHL.UW / PMTHL.LW
  nop      SYNC, SYNC.P, CACHE, PREF
  interp   SYSCALL, BREAK, COP0 (MFC0 MTC0 EI DI ERET TLB*), COP2 (VU0 macro mode, LQC2 / SQC2), the SA register
           (MFSA MTSA MTSAB MTSAH, QFSRV), the MMI ops not in the tables (parallel multiplies / divides, PEXT5 ...),
           reserved encodings

What the R3000A decode gets wrong here (ps1_tools.isa would say): 0x1C is MMI, 0x1E / 0x1F are LQ / SQ, 0x14-0x17
and REGIMM 2 / 3 / 0x12 / 0x13 are likely branches, 0x18-0x1B / 0x27 / 0x2C / 0x2D / 0x37 / 0x3F are the 64-bit
loads, stores and immediates, SPECIAL 0x14-0x17 / 0x2C-0x2F / 0x38-0x3F the 64-bit ALU, MULT / MULTU write rd.
"""

from ps1_tools.isa import RA, REG_NAMES, Ins

# ---- tables -------------------------------------------------------------------------------------------------------

SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav", 0x08: "jr", 0x09: "jalr",
    0x0A: "movz", 0x0B: "movn", 0x0C: "syscall", 0x0D: "break", 0x0F: "sync", 0x10: "mfhi", 0x11: "mthi",
    0x12: "mflo", 0x13: "mtlo", 0x14: "dsllv", 0x16: "dsrlv", 0x17: "dsrav", 0x18: "mult", 0x19: "multu",
    0x1A: "div", 0x1B: "divu", 0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu", 0x24: "and", 0x25: "or",
    0x26: "xor", 0x27: "nor", 0x28: "mfsa", 0x29: "mtsa", 0x2A: "slt", 0x2B: "sltu", 0x2C: "dadd", 0x2D: "daddu",
    0x2E: "dsub", 0x2F: "dsubu", 0x30: "tge", 0x31: "tgeu", 0x32: "tlt", 0x33: "tltu", 0x34: "teq", 0x36: "tne",
    0x38: "dsll", 0x3A: "dsrl", 0x3B: "dsra", 0x3C: "dsll32", 0x3E: "dsrl32", 0x3F: "dsra32",
}
REGIMM = {
    0x00: "bltz", 0x01: "bgez", 0x02: "bltzl", 0x03: "bgezl", 0x08: "tgei", 0x09: "tgeiu", 0x0A: "tlti",
    0x0B: "tltiu", 0x0C: "teqi", 0x0E: "tnei", 0x10: "bltzal", 0x11: "bgezal", 0x12: "bltzall", 0x13: "bgezall",
    0x18: "mtsab", 0x19: "mtsah",
}
PRIMARY = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz", 0x08: "addi", 0x09: "addiu",
    0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori", 0x0E: "xori", 0x0F: "lui", 0x14: "beql", 0x15: "bnel",
    0x16: "blezl", 0x17: "bgtzl", 0x18: "daddi", 0x19: "daddiu", 0x1A: "ldl", 0x1B: "ldr", 0x1E: "lq", 0x1F: "sq",
    0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr", 0x27: "lwu", 0x28: "sb",
    0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2C: "sdl", 0x2D: "sdr", 0x2E: "swr", 0x2F: "cache", 0x31: "lwc1",
    0x33: "pref", 0x36: "lqc2", 0x37: "ld", 0x39: "swc1", 0x3E: "sqc2", 0x3F: "sd",
}
LOADS = {"lb", "lh", "lw", "lbu", "lhu", "lwu", "ld", "lq", "lwl", "lwr", "ldl", "ldr"}
STORES = {"sb", "sh", "sw", "sd", "sq", "swl", "swr", "sdl", "sdr"}
TRAPS = {"tge", "tgeu", "tlt", "tltu", "teq", "tne", "tgei", "tgeiu", "tlti", "tltiu", "teqi", "tnei"}

# MMI (opcode 0x1C) by funct; MMI0-3 by the sa field. `rd = op(rs, rt)` (one-operand ops ignore rs). Mirrored by
# PS2_MMI_LIST in consoles/ps2/runtime/ps2_alu.h (test_ps2_tools checks both agree).
MMI_GROUPS = {0x08: "mmi0", 0x28: "mmi1", 0x09: "mmi2", 0x29: "mmi3"}
MMI_OPS = {
    0x08: {0x00: "paddw", 0x01: "psubw", 0x02: "pcgtw", 0x03: "pmaxw", 0x04: "paddh", 0x05: "psubh", 0x06: "pcgth",
           0x07: "pmaxh", 0x08: "paddb", 0x09: "psubb", 0x0A: "pcgtb", 0x10: "paddsw", 0x11: "psubsw",
           0x12: "pextlw", 0x13: "ppacw", 0x14: "paddsh", 0x15: "psubsh", 0x16: "pextlh", 0x17: "ppach",
           0x18: "paddsb", 0x19: "psubsb", 0x1A: "pextlb", 0x1B: "ppacb"},
    0x28: {0x01: "pabsw", 0x02: "pceqw", 0x03: "pminw", 0x05: "pabsh", 0x06: "pceqh", 0x07: "pminh", 0x0A: "pceqb",
           0x10: "padduw", 0x11: "psubuw", 0x12: "pextuw", 0x14: "padduh", 0x15: "psubuh", 0x16: "pextuh",
           0x18: "paddub", 0x19: "psubub", 0x1A: "pextub"},
    0x09: {0x0A: "pinth", 0x0E: "pcpyld", 0x12: "pand", 0x13: "pxor", 0x1A: "pexeh", 0x1B: "prevh", 0x1E: "pexew",
           0x1F: "prot3w"},
    0x29: {0x0A: "pinteh", 0x0E: "pcpyud", 0x12: "por", 0x13: "pnor", 0x1A: "pexch", 0x1B: "pcpyh",
           0x1E: "pexcw"},
}
MMI_ONE = {"pabsw", "pabsh", "pexeh", "prevh", "pexew", "prot3w", "pexch", "pcpyh", "pexcw"}  # rd = op(rt)
MMI_HILO = {(0x09, 0x08): "pmfhi", (0x09, 0x09): "pmflo", (0x29, 0x08): "pmthi", (0x29, 0x09): "pmtlo"}
MMI_SHIFTS = {0x34: "psllh", 0x36: "psrlh", 0x37: "psrah", 0x3C: "psllw", 0x3E: "psrlw", 0x3F: "psraw"}
MMI_MULDIV = {0x00: "madd", 0x01: "maddu", 0x10: "mfhi1", 0x11: "mthi1", 0x12: "mflo1", 0x13: "mtlo1",
              0x18: "mult1", 0x19: "multu1", 0x1A: "div1", 0x1B: "divu1", 0x20: "madd1", 0x21: "maddu1"}
PMFHL = {0: "pmfhl.lw", 1: "pmfhl.uw"}

FMT_S, FMT_W = 16, 20
FPU_S = {0x00: "add.s", 0x01: "sub.s", 0x02: "mul.s", 0x03: "div.s", 0x04: "sqrt.s", 0x05: "abs.s", 0x06: "mov.s",
         0x07: "neg.s", 0x16: "rsqrt.s", 0x18: "adda.s", 0x19: "suba.s", 0x1A: "mula.s", 0x1C: "madd.s",
         0x1D: "msub.s", 0x1E: "madda.s", 0x1F: "msuba.s", 0x24: "cvt.w.s", 0x28: "max.s", 0x29: "min.s",
         0x30: "c.f.s", 0x32: "c.eq.s", 0x34: "c.lt.s", 0x36: "c.le.s"}


def _branch_target(i):
    return (i.addr + 4 + (i.simm << 2)) & 0xFFFFFFFF


class R5900Ins(Ins):
    __slots__ = ("likely", "fs", "ft", "fd")

    def __init__(self, addr, word):
        self.likely = False
        self.fs = (word >> 11) & 31
        self.ft = (word >> 16) & 31
        self.fd = (word >> 6) & 31
        super().__init__(addr, word)  # fields, and the R3000A decode, which _decode replaces entirely
        self.target = None
        self.link = None
        _decode(self)

    def reads(self):
        """GPRs read (the low 64 bits; MMI ops read all 128). Only for analyses: the lifter does not use it."""
        n, c = self.name, self.cls
        if n in ("sll", "srl", "sra", "dsll", "dsrl", "dsra", "dsll32", "dsrl32", "dsra32") or n in MMI_ONE \
                or n in MMI_SHIFTS.values():
            return {self.rt}
        if n in ("movz", "movn"):
            return {self.rs, self.rt, self.rd}
        if n in ("jr", "jalr", "mthi", "mtlo", "mthi1", "mtlo1", "pmthi", "pmtlo", "pmthl.lw", "plzcw"):
            return {self.rs}
        if n in ("mtc1", "ctc1"):
            return {self.rt}
        if c in ("load", "fls") or n in ("lui", "j", "jal") or c in ("fpu", "nop", "interp"):
            return {self.rs} if c in ("load", "fls") else set()
        if c == "branch" and n.startswith("bc1"):
            return set()
        if c == "branch" and n not in ("beq", "bne", "beql", "bnel"):
            return {self.rs}
        if c in ("alu", "trap") and (self.word >> 26) not in (0, 0x1C):
            return {self.rs}
        return {self.rs, self.rt}

    def dest(self):
        """The GPR written (its low 64 bits at least), or None (r0 counts as none). Loads count: no load delay."""
        n, c = self.name, self.cls
        d = None
        if c in ("alu", "trap") and n not in TRAPS:
            d = self.rd if (self.word >> 26) in (0, 0x1C) else self.rt
        elif n in ("mult", "multu", "mult1", "multu1", "madd", "maddu", "madd1", "maddu1"):
            d = self.rd
        elif c == "mmi" and n not in ("pmthi", "pmtlo", "pmthl.lw"):
            d = self.rd
        elif c == "load" or n in ("mfc1", "cfc1"):
            d = self.rt
        elif self.link is not None:
            d = self.link
        return d or None

    def ldest(self):
        return None  # no delayed load on the R5900

    def is_unconditional(self):
        if self.name in ("beq", "beql") and self.rs == self.rt:
            return True
        if self.name in ("bgez", "bgezal", "bgezl") and self.rs == 0:  # BGEZALL zero is a call, like BAL
            return True
        return self.cls in ("jump", "jr")

    def text(self):
        return disasm(self)


def _decode(i):
    w = i.word
    op = w >> 26
    i.cls = "interp"
    i.name = "reserved"
    if op == 0:
        n = SPECIAL.get(w & 63)
        if n is None:
            return
        i.name = n
        if n in ("syscall", "break", "mfsa", "mtsa"):
            i.cls = "interp"
        elif n == "sync":
            i.cls = "nop"
        elif n in ("jr", "jalr"):
            i.cls = "jr"
            if n == "jalr":
                i.link = i.rd
        elif n in ("mult", "multu", "div", "divu", "mthi", "mtlo"):
            i.cls = "muldiv"
        elif n in ("add", "sub", "dadd", "dsub") or n in TRAPS:
            i.cls = "trap"
        else:
            i.cls = "alu"
        return
    if op == 1:
        n = REGIMM.get(i.rt)
        if n is None:
            return
        i.name = n
        if n in TRAPS:
            i.cls = "trap"
        elif n in ("mtsab", "mtsah"):
            i.cls = "interp"
        else:
            i.cls = "branch"
            i.target = _branch_target(i)
            i.likely = n.endswith("l") and n not in ("bltzal", "bgezal")
            if "al" in n[3:]:
                i.link = RA
        return
    if op == 0x10:
        _decode_cop0(i)
        return
    if op == 0x11:
        _decode_cop1(i)
        return
    if op == 0x12:
        i.name = "cop2"
        return
    if op == 0x1C:
        _decode_mmi(i)
        return
    n = PRIMARY.get(op)
    if n is None:
        return
    i.name = n
    if n in ("j", "jal"):
        i.cls = "jump"
        i.target = ((i.addr + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        if n == "jal":
            i.link = RA
    elif n in ("beq", "bne", "blez", "bgtz", "beql", "bnel", "blezl", "bgtzl"):
        i.cls = "branch"
        i.target = _branch_target(i)
        i.likely = n.endswith("l")
    elif n in ("addi", "daddi"):
        i.cls = "trap"
    elif n in LOADS:
        i.cls = "load"
    elif n in STORES:
        i.cls = "store"
    elif n in ("lwc1", "swc1"):
        i.cls = "fls"
    elif n in ("cache", "pref"):
        i.cls = "nop"
    elif n in ("lqc2", "sqc2"):
        i.cls = "interp"
    else:
        i.cls = "alu"


def _decode_cop0(i):
    w = i.word
    if i.rs == 0:
        i.name = "mfc0"
    elif i.rs == 4:
        i.name = "mtc0"
    elif i.rs == 0x10:
        i.name = {0x01: "tlbr", 0x02: "tlbwi", 0x06: "tlbwr", 0x08: "tlbp", 0x18: "eret", 0x38: "ei",
                  0x39: "di"}.get(w & 63, "cop0")
    else:
        i.name = "cop0"


def _decode_cop1(i):
    w = i.word
    fmt = i.rs
    i.cls = "fpu"
    if fmt in (0, 2, 4, 6):
        i.name = {0: "mfc1", 2: "cfc1", 4: "mtc1", 6: "ctc1"}[fmt]
        return
    if fmt == 8:
        n = {0: "bc1f", 1: "bc1t", 2: "bc1fl", 3: "bc1tl"}[i.rt & 3]
        i.name, i.cls, i.likely = n, "branch", n.endswith("l")
        i.target = _branch_target(i)
        return
    f = w & 63
    if fmt == FMT_S and f in FPU_S:
        i.name = FPU_S[f]
        return
    if fmt == FMT_W and f == 0x20:
        i.name = "cvt.s.w"
        return
    i.name, i.cls = "cop1", "interp"


def _decode_mmi(i):
    w = i.word
    f = w & 63
    i.cls = "mmi"
    if f in MMI_GROUPS:
        if (f, i.sa) in MMI_HILO:
            i.name = MMI_HILO[(f, i.sa)]
            return
        n = MMI_OPS[f].get(i.sa)
        if n:
            i.name = n
            return
        i.name, i.cls = "%s.%02x" % (MMI_GROUPS[f], i.sa), "interp"
        return
    if f in MMI_SHIFTS:
        i.name = MMI_SHIFTS[f]
        return
    if f in MMI_MULDIV:
        n = i.name = MMI_MULDIV[f]
        i.cls = "alu" if n in ("mfhi1", "mflo1") else "muldiv"
        return
    if f == 0x04:
        i.name, i.cls = "plzcw", "alu"
        return
    if f == 0x30 and i.sa in PMFHL:
        i.name = PMFHL[i.sa]
        return
    if f == 0x31 and i.sa == 0:
        i.name = "pmthl.lw"
        return
    i.name, i.cls = "mmi.%02x" % f, "interp"


def decode(word, addr=0):
    return R5900Ins(addr, word)


def disasm(i):
    R = REG_NAMES
    n, c = i.name, i.cls
    op = i.word >> 26
    if i.word == 0:
        return "nop"
    if n in ("sll", "srl", "sra", "dsll", "dsrl", "dsra", "dsll32", "dsrl32", "dsra32") or n in MMI_SHIFTS.values():
        return "%s %s, %s, %d" % (n, R[i.rd], R[i.rt], i.sa)
    if n in ("sllv", "srlv", "srav", "dsllv", "dsrlv", "dsrav"):
        return "%s %s, %s, %s" % (n, R[i.rd], R[i.rt], R[i.rs])
    if n in ("jr", "mthi", "mtlo", "mthi1", "mtlo1", "pmthi", "pmtlo", "pmthl.lw"):
        return "%s %s" % (n, R[i.rs])
    if n == "jalr":
        return "jalr %s, %s" % (R[i.rd], R[i.rs])
    if n in ("mfhi", "mflo", "mfhi1", "mflo1", "pmfhi", "pmflo", "pmfhl.lw", "pmfhl.uw"):
        return "%s %s" % (n, R[i.rd])
    if n in MMI_ONE:
        return "%s %s, %s" % (n, R[i.rd], R[i.rt])
    if n == "plzcw":
        return "plzcw %s, %s" % (R[i.rd], R[i.rs])
    if n in TRAPS and op == 0:
        return "%s %s, %s" % (n, R[i.rs], R[i.rt])
    if n in TRAPS:
        return "%s %s, %d" % (n, R[i.rs], i.simm)
    if (c in ("alu", "trap", "muldiv", "mmi")) and op in (0, 0x1C):
        return "%s %s, %s, %s" % (n, R[i.rd], R[i.rs], R[i.rt])
    if n == "lui":
        return "lui %s, 0x%04X" % (R[i.rt], i.imm)
    if n in ("andi", "ori", "xori"):
        return "%s %s, %s, 0x%04X" % (n, R[i.rt], R[i.rs], i.imm)
    if c in ("alu", "trap"):
        return "%s %s, %s, %d" % (n, R[i.rt], R[i.rs], i.simm)
    if c in ("load", "store"):
        return "%s %s, %d(%s)" % (n, R[i.rt], i.simm, R[i.rs])
    if c == "fls":
        return "%s $f%d, %d(%s)" % (n, i.ft, i.simm, R[i.rs])
    if n.startswith("bc1"):
        return "%s 0x%08X" % (n, i.target)
    if n in ("beq", "bne", "beql", "bnel"):
        return "%s %s, %s, 0x%08X" % (n, R[i.rs], R[i.rt], i.target)
    if c == "branch":
        return "%s %s, 0x%08X" % (n, R[i.rs], i.target)
    if c == "jump":
        return "%s 0x%08X" % (n, i.target)
    if n in ("mfc1", "mtc1"):
        return "%s %s, $f%d" % (n, R[i.rt], i.fs)
    if n in ("cfc1", "ctc1"):
        return "%s %s, $fcr%d" % (n, R[i.rt], i.fs)
    if n in ("mfc0", "mtc0"):
        return "%s %s, $%d" % (n, R[i.rt], i.rd)
    if c == "fpu":
        if n in ("adda.s", "suba.s", "mula.s", "madda.s", "msuba.s") or n.startswith("c."):
            return "%s $f%d, $f%d" % (n, i.fs, i.ft)
        if n == "sqrt.s":
            return "sqrt.s $f%d, $f%d" % (i.fd, i.ft)
        if n in ("abs.s", "mov.s", "neg.s", "cvt.w.s", "cvt.s.w"):
            return "%s $f%d, $f%d" % (n, i.fd, i.fs)
        return "%s $f%d, $f%d, $f%d" % (n, i.fd, i.fs, i.ft)
    return "%s (0x%08X)" % (n, i.word)
