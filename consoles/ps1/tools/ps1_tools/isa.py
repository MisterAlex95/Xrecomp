"""MIPS I (R3000A) decoder (XBA-160): every instruction the PS1 CPU runs, with what the lifter and the discovery need
to know (class, registers read, plain and delayed destinations, branch targets). Our own table, pure Python.

Classes:
  alu      plain register write (rd or rt), no exception
  trap     ADD / ADDI / SUB: an overflow raises
  muldiv   MULT / MULTU / DIV / DIVU / MTHI / MTLO (HI / LO)
  load     delayed load into rt (LB LH LW LBU LHU LWL LWR)
  store    SB SH SW SWL SWR
  branch   conditional PC-relative branch (BEQ BNE BLEZ BGTZ and REGIMM), optional link
  jump     J / JAL (static target)
  jr       JR / JALR (target in a register)
  cop2     GTE: cmd (imm25), mfc2 / cfc2 (delayed loads), mtc2 / ctc2, lwc2, swc2
  interp   left to the interpreter: SYSCALL, BREAK, COP0 (MFC0 MTC0 RFE ...), COP1 / COP3, LWCz / SWCz z != 2,
           reserved encodings
"""

R0, AT, V0, V1, A0, A1, A2, A3 = range(8)
T0, T1, T2 = 8, 9, 10
GP, SP, FP, RA = 28, 29, 30, 31

REG_NAMES = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
             "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav", 0x08: "jr", 0x09: "jalr",
    0x0C: "syscall", 0x0D: "break", 0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo", 0x18: "mult",
    0x19: "multu", 0x1A: "div", 0x1B: "divu", 0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu", 0x24: "and",
    0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu",
}
PRIMARY = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz", 0x08: "addi", 0x09: "addiu",
    0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori", 0x0E: "xori", 0x0F: "lui", 0x20: "lb", 0x21: "lh",
    0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr", 0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw",
    0x2E: "swr", 0x32: "lwc2", 0x3A: "swc2",
}
LOADS = {"lb", "lh", "lw", "lbu", "lhu", "lwl", "lwr"}
STORES = {"sb", "sh", "sw", "swl", "swr"}


class Ins:
    __slots__ = ("addr", "word", "name", "cls", "rs", "rt", "rd", "sa", "imm", "simm", "target", "link")

    def __init__(self, addr, word):
        self.addr = addr
        self.word = word
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.imm = word & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = None  # static branch / jump target
        self.link = None    # register written with the return address
        _decode(self)

    # ---- register usage (the interpreter's semantics) ----
    def reads(self):
        """Registers read as operands (LWL / LWR: the base only; their merge with rt is not a read here)."""
        n, c = self.name, self.cls
        if c == "interp":
            return {self.rt} if n == "mtc0" else set()
        if n in ("sll", "srl", "sra"):
            return {self.rt}
        if n in ("jr", "jalr", "mthi", "mtlo"):
            return {self.rs}
        if n in ("mfhi", "mflo", "lui", "j", "jal"):
            return set()
        if c in ("alu", "trap", "muldiv") and (self.word >> 26) == 0:
            return {self.rs, self.rt}
        if n in ("beq", "bne"):
            return {self.rs, self.rt}
        if c == "branch":
            return {self.rs}
        if c in ("alu", "trap"):
            return {self.rs}
        if c == "load":
            return {self.rs}
        if c == "store":
            return {self.rs, self.rt}
        if n in ("mtc2", "ctc2"):
            return {self.rt}
        if n in ("lwc2", "swc2"):
            return {self.rs}
        return set()

    def dest(self):
        """The register written at once (not delayed), or None (r0 counts as none)."""
        n, c = self.name, self.cls
        d = None
        if c in ("alu", "trap"):
            d = self.rd if (self.word >> 26) == 0 else self.rt
        elif n in ("mfhi", "mflo"):
            d = self.rd
        elif self.link is not None:
            d = self.link
        return d or None

    def ldest(self):
        """The register a delayed load writes (loads, MFC2, CFC2, MFC0), or None."""
        if self.cls == "load" or self.name in ("mfc2", "cfc2", "mfc0"):
            return self.rt or None
        return None

    def is_transfer(self):
        return self.cls in ("branch", "jump", "jr")

    def is_unconditional(self):
        """Control never falls through to addr + 8."""
        if self.cls in ("jump", "jr"):
            return True
        if self.name == "beq" and self.rs == self.rt:
            return True
        if self.name in ("bgez", "bgezal") and self.rs == 0:
            return True
        return False

    def text(self):
        return disasm(self)


def conflicts(n, r):
    """Must a load in flight to r stay in flight across instruction n? (n reads r, or loads into r other than LWL /
    LWR, which forward the value in flight.) Mirrors conflicts() of ps1_recomp.c."""
    if n.cls == "interp" and n.name not in ("mtc0", "mfc0", "rfe", "syscall", "break", "cop0"):
        return True  # reserved / unknown: let the interpreter decide
    if r in n.reads():
        return True
    return n.ldest() == r and n.name not in ("lwl", "lwr")


def _decode(i):
    w = i.word
    op = w >> 26
    i.cls = "interp"
    if op == 0:
        f = w & 63
        n = SPECIAL.get(f)
        i.name = n or "reserved"
        if n is None:
            return
        if n in ("syscall", "break"):
            i.cls = "interp"
        elif n in ("jr", "jalr"):
            i.cls = "jr"
            if n == "jalr":
                i.link = i.rd
        elif n in ("mult", "multu", "div", "divu", "mthi", "mtlo"):
            i.cls = "muldiv"
        elif n in ("add", "sub"):
            i.cls = "trap"
        else:
            i.cls = "alu"
        return
    if op == 1:
        rt = i.rt
        i.name = ("bgez" if rt & 1 else "bltz") + ("al" if (rt & 0x1E) == 0x10 else "")
        i.cls = "branch"
        if (rt & 0x1E) == 0x10:
            i.link = RA
        i.target = (i.addr + 4 + (i.simm << 2)) & 0xFFFFFFFF
        return
    if op == 0x10:
        sub = i.rs
        i.name = {0: "mfc0", 4: "mtc0"}.get(sub, "rfe" if (sub == 0x10 and (w & 63) == 0x10) else "cop0")
        return
    if op == 0x12:
        i.cls = "cop2"
        if w & (1 << 25):
            i.name = "cop2"
        else:
            i.name = {0: "mfc2", 2: "cfc2", 4: "mtc2", 6: "ctc2"}.get(i.rs, "reserved")
            if i.name == "reserved":
                i.cls = "interp"
        return
    n = PRIMARY.get(op)
    i.name = n or ("cop%d" % (op & 3) if op in (0x11, 0x13) else "reserved")
    if n is None:
        return
    if n in ("j", "jal"):
        i.cls = "jump"
        i.target = ((i.addr + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        if n == "jal":
            i.link = RA
    elif n in ("beq", "bne", "blez", "bgtz"):
        i.cls = "branch"
        i.target = (i.addr + 4 + (i.simm << 2)) & 0xFFFFFFFF
    elif n == "addi":
        i.cls = "trap"
    elif n in LOADS:
        i.cls = "load"
    elif n in STORES:
        i.cls = "store"
    elif n in ("lwc2", "swc2"):
        i.cls = "cop2"
    else:
        i.cls = "alu"


def decode(word, addr=0):
    return Ins(addr, word)


def disasm(i):
    R = REG_NAMES
    n = i.name
    if n in ("sll", "srl", "sra"):
        return "nop" if i.word == 0 else "%s %s, %s, %d" % (n, R[i.rd], R[i.rt], i.sa)
    if n in ("sllv", "srlv", "srav"):
        return "%s %s, %s, %s" % (n, R[i.rd], R[i.rt], R[i.rs])
    if n == "jr" or n in ("mthi", "mtlo"):
        return "%s %s" % (n, R[i.rs])
    if n == "jalr":
        return "jalr %s, %s" % (R[i.rd], R[i.rs])
    if n in ("mfhi", "mflo"):
        return "%s %s" % (n, R[i.rd])
    if n in ("mult", "multu", "div", "divu"):
        return "%s %s, %s" % (n, R[i.rs], R[i.rt])
    if i.cls in ("alu", "trap") and (i.word >> 26) == 0:
        return "%s %s, %s, %s" % (n, R[i.rd], R[i.rs], R[i.rt])
    if n == "lui":
        return "lui %s, 0x%04X" % (R[i.rt], i.imm)
    if n in ("andi", "ori", "xori"):
        return "%s %s, %s, 0x%04X" % (n, R[i.rt], R[i.rs], i.imm)
    if i.cls in ("alu", "trap"):
        return "%s %s, %s, %d" % (n, R[i.rt], R[i.rs], i.simm)
    if i.cls in ("load", "store") or n in ("lwc2", "swc2"):
        rt = R[i.rt] if i.cls != "cop2" else "$%d" % i.rt
        return "%s %s, %d(%s)" % (n, rt, i.simm, R[i.rs])
    if n in ("beq", "bne"):
        return "%s %s, %s, 0x%08X" % (n, R[i.rs], R[i.rt], i.target)
    if i.cls == "branch":
        return "%s %s, 0x%08X" % (n, R[i.rs], i.target)
    if i.cls == "jump":
        return "%s 0x%08X" % (n, i.target)
    if n in ("mfc2", "cfc2", "mtc2", "ctc2", "mfc0", "mtc0"):
        return "%s %s, $%d" % (n, R[i.rt], i.rd)
    if n == "cop2":
        return "cop2 0x%07X" % (i.word & 0x1FFFFFF)
    return "%s (0x%08X)" % (n, i.word)
