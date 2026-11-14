"""VR4300 (MIPS III) to C (XBA-170): one C function per discovered function, in the shape of n64_recomp_rt.h.

Every instruction has a label and a case in the entry switch (the dispatcher resumes anywhere: after an interrupt's
ERET, a scheduler stop, a call's return, an instruction left to the interpreter), except delay slots, which run
inline after their branch (and get a label of their own only when a branch of the function targets them).
Semantics are the interpreter's (n64_cpu.c / n64_fpu.c), instruction for instruction:
- 64-bit registers in locals (only those the function uses are loaded, only those it writes stored back), 32-bit
  results sign-extended, SRA / SRAV shifting the whole register, the DIV / DDIV results for a zero divisor;
- fetch cost, count and the scheduler / interrupt test before every instruction (RC_I / RC_I_SLOT);
- branch delay slots inline; branch-likely skips its slot when not taken; a branch in a delay slot, COP0 (but
  CACHE), SYSCALL / BREAK, LL / SC, COP2 / COP3 and reserved encodings go to the interpreter, and so does any
  instruction whose exception condition holds (overflow, a trap taken, a faulting access, COP1 unusable): RC_INTERP,
  nothing of it done, so the interpreter raises with the exact EPC / BD;
- memory through RC_LOAD / RC_STORE / RC_LOADP / RC_STOREP (cached RDRAM inline, everything else through the core's
  translation, costs and devices);
- COP1 moves, loads and stores inline (the FR register views), arithmetic and CTC1 through n64_fpu.c;
- calls, returns, indirect jumps and jumps out of the function return to the dispatcher (bounded C stack).
"""

from .discover import is_direct
from .isa import sx32


def hx(v):
    return "0x%08Xu" % (v & 0xFFFFFFFF)


def c64(v):
    return "0x%016XULL" % (v & 0xFFFFFFFFFFFFFFFF)


def sx(v):
    """The sign-extended 64-bit form of a 32-bit address, as a C constant."""
    return c64(sx32(v))


def R(r):
    return "0ull" if r == 0 else "r%d" % r


LOAD_CONV = {"lb": "RC_S8", "lbu": "RC_U8", "lh": "RC_S16", "lhu": "RC_U16", "lw": "RC_S32", "lwu": "RC_U32",
             "ld": "RC_U64"}
COST = {"mult": "N64_RC_COST_MULT", "multu": "N64_RC_COST_MULT", "div": "N64_RC_COST_DIV",
        "divu": "N64_RC_COST_DIV", "dmult": "N64_RC_COST_DMULT", "dmultu": "N64_RC_COST_DMULT",
        "ddiv": "N64_RC_COST_DDIV", "ddivu": "N64_RC_COST_DDIV"}
MULFN = {"mult": "rc_mult", "multu": "rc_multu", "div": "rc_div", "divu": "rc_divu", "dmult": "rc_dmult",
         "dmultu": "rc_mulu64", "ddiv": "rc_ddiv", "ddivu": "rc_ddivu"}


class FnLifter:
    def __init__(self, func, name, resume_all=True):
        self.resume_all = resume_all
        self.f = func
        self.img = func.image
        self.name = name
        self.fallbacks = 0
        self.labels = 0
        self.out = []
        self.targets = set()  # branch / jump targets inside the function
        for a in func.addrs:
            t = self.img.ins(a).target
            if t is not None and func.start <= t < func.end:
                self.targets.add(t)

    def inside(self, a):
        return self.f.start <= a < self.f.end

    def emit(self, s):
        self.out.append(s)

    def go(self, t):
        """Continue at static address t."""
        return "goto L_%08X;" % t if self.inside(t) else "RC_EXIT(%s);" % hx(t)

    def needs_label(self, a):
        """Resume points when labels are sparse: where gotos land, the function's start, after transfers' slots and
        after instructions that may stop (memory, COP1 calls, interpreter hand-overs)."""
        f = self.f
        if a == f.start or a in self.targets:
            return True
        i = self.img.ins(a)
        if i.cls in ("interp", "trap", "ovf") or i.is_transfer():
            return True
        for back in (4, 8):
            p = a - back
            if not self.inside(p):
                continue
            pi = self.img.ins(p)
            if back == 8 and pi.is_transfer():
                return True
            if back == 4 and pi.cls in ("load", "store", "fmem", "fop", "fmove", "interp", "trap", "ovf"):
                return True
        return False

    # ---- one instruction (not a transfer) ----
    def body(self, i, fault, ds):
        """C for instruction i at its address; fault: the statement that hands it to the interpreter; ds: 1 in a
        delay slot."""
        n, c = i.name, i.cls
        rs, rt, rd = R(i.rs), R(i.rt), R(i.rd)
        simm = i.simm

        def w(reg, expr):
            return "" if reg == 0 else "%s = %s;" % (R(reg), expr)

        def dst(reg):
            return "dz" if reg == 0 else R(reg)

        ea = "%s + %s" % (rs, c64(simm)) if simm else rs
        if c == "interp":
            return fault
        if c == "nop":
            return ""
        if c == "alu":
            if (i.word >> 26) == 0:
                sa = i.sa
                ex = {
                    "sll": "RC_SX((uint32_t)%s << %d)" % (rt, sa), "srl": "RC_SX((uint32_t)%s >> %d)" % (rt, sa),
                    "sra": "RC_SX((uint64_t)((int64_t)%s >> %d))" % (rt, sa),
                    "sllv": "RC_SX((uint32_t)%s << (%s & 31))" % (rt, rs),
                    "srlv": "RC_SX((uint32_t)%s >> (%s & 31))" % (rt, rs),
                    "srav": "RC_SX((uint64_t)((int64_t)%s >> (%s & 31)))" % (rt, rs),
                    "dsllv": "%s << (%s & 63)" % (rt, rs), "dsrlv": "%s >> (%s & 63)" % (rt, rs),
                    "dsrav": "(uint64_t)((int64_t)%s >> (%s & 63))" % (rt, rs),
                    "mfhi": "hi", "mflo": "lo",
                    "addu": "RC_SX((uint32_t)%s + (uint32_t)%s)" % (rs, rt),
                    "subu": "RC_SX((uint32_t)%s - (uint32_t)%s)" % (rs, rt),
                    "and": "%s & %s" % (rs, rt), "or": "%s | %s" % (rs, rt), "xor": "%s ^ %s" % (rs, rt),
                    "nor": "~(%s | %s)" % (rs, rt),
                    "slt": "(uint64_t)((int64_t)%s < (int64_t)%s)" % (rs, rt),
                    "sltu": "(uint64_t)(%s < %s)" % (rs, rt),
                    "daddu": "%s + %s" % (rs, rt), "dsubu": "%s - %s" % (rs, rt),
                    "dsll": "%s << %d" % (rt, sa), "dsrl": "%s >> %d" % (rt, sa),
                    "dsra": "(uint64_t)((int64_t)%s >> %d)" % (rt, sa),
                    "dsll32": "%s << %d" % (rt, sa + 32), "dsrl32": "%s >> %d" % (rt, sa + 32),
                    "dsra32": "(uint64_t)((int64_t)%s >> %d)" % (rt, sa + 32),
                }.get(n)
                if n == "sync":
                    return ""
                return w(i.rd, ex)
            ex = {
                "addiu": "RC_SX((uint32_t)%s + %s)" % (rs, hx(simm)),
                "daddiu": "%s + %s" % (rs, c64(simm)),
                "slti": "(uint64_t)((int64_t)%s < %dLL)" % (rs, simm),
                "sltiu": "(uint64_t)(%s < %s)" % (rs, c64(simm)),
                "andi": "%s & 0x%Xull" % (rs, i.imm), "ori": "%s | 0x%Xull" % (rs, i.imm),
                "xori": "%s ^ 0x%Xull" % (rs, i.imm), "lui": c64(sx32(i.imm << 16)),
            }[n]
            return w(i.rt, ex)
        if c == "ovf":
            mac = {"add": "RC_ADD32", "addi": "RC_ADD32", "sub": "RC_SUB32", "dadd": "RC_ADD64",
                   "daddi": "RC_ADD64", "dsub": "RC_SUB64"}[n]
            if n in ("addi", "daddi"):
                return "%s(%s, %s, %dLL, %s);" % (mac, dst(i.rt), rs, simm, fault)
            return "%s(%s, %s, %s, %s);" % (mac, dst(i.rd), rs, rt, fault)
        if c == "trap":
            cond = {
                "tge": "(int64_t)%s >= (int64_t)%s" % (rs, rt), "tgeu": "%s >= %s" % (rs, rt),
                "tlt": "(int64_t)%s < (int64_t)%s" % (rs, rt), "tltu": "%s < %s" % (rs, rt),
                "teq": "%s == %s" % (rs, rt), "tne": "%s != %s" % (rs, rt),
                "tgei": "(int64_t)%s >= %dLL" % (rs, simm), "tgeiu": "%s >= %s" % (rs, c64(simm)),
                "tlti": "(int64_t)%s < %dLL" % (rs, simm), "tltiu": "%s < %s" % (rs, c64(simm)),
                "teqi": "%s == %s" % (rs, c64(simm)), "tnei": "%s != %s" % (rs, c64(simm)),
            }[n]
            return "if (%s) { %s }" % (cond, fault)
        if c == "muldiv":
            if n == "mthi":
                return "hi = %s;" % rs
            if n == "mtlo":
                return "lo = %s;" % rs
            return "%s(%s, %s, &hi, &lo); now += %s;" % (MULFN[n], rs, rt, COST[n])
        if c == "load":
            if n in LOAD_CONV:
                return "RC_LOAD(%s, %s, %d, %s, %s);" % (dst(i.rt), ea, {"lb": 1, "lbu": 1, "lh": 2, "lhu": 2,
                                                                        "lw": 4, "lwu": 4, "ld": 8}[n],
                                                         LOAD_CONV[n], fault)
            sz = 4 if n in ("lwl", "lwr") else 8
            set_ = "%s = rc_%s(%s, w_, ea_);" % (R(i.rt), n, rt) if i.rt else ""
            return "{ uint64_t ea_ = %s, w_ = 0; RC_LOADP(w_, ea_, %d, %s); %s }" % (ea, sz, fault, set_)
        if c == "store":
            if n in ("sb", "sh", "sw", "sd"):
                return "RC_STORE(%s, %d, %s, %s);" % (ea, {"sb": 1, "sh": 2, "sw": 4, "sd": 8}[n], rt, fault)
            sz = 4 if n in ("swl", "swr") else 8
            m = n.upper()
            return ("{ uint64_t ea_ = %s; RC_STOREP(ea_, RC_%s_D(%s, ea_), RC_%s_M(ea_), %d, %s); }"
                    % (ea, m, rt, m, sz, fault))
        cu = "if (!RC_CU1()) { %s } " % fault
        if c == "fmove":
            fs = i.fs
            if n == "mfc1":
                return cu + w(i.rt, "RC_S32(rc_fpu_read_s(h, sr, %d))" % fs)
            if n == "dmfc1":
                return cu + w(i.rt, "rc_fpu_read_d(h, sr, %d)" % fs)
            if n == "cfc1":
                return cu + w(i.rt, "rc_cfc1(h, %d)" % fs)
            if n == "mtc1":
                return cu + "rc_fpu_write_s(h, sr, %d, (uint32_t)%s);" % (fs, rt)
            if n == "dmtc1":
                return cu + "rc_fpu_write_d(h, sr, %d, %s);" % (fs, rt)
            if n == "ctc1":
                return cu + ("RC_CTC1(%s, %s, %d);" % (rt, hx(i.addr), ds) if fs == 31 else "")
        if c == "fmem":
            ft = i.ft
            if n == "lwc1":
                return cu + ("{ uint64_t fv_ = 0; RC_LOAD(fv_, %s, 4, RC_U64, %s); rc_fpu_write_s(h, sr, %d, "
                             "(uint32_t)fv_); }" % (ea, fault, ft))
            if n == "ldc1":
                return cu + ("{ uint64_t fv_ = 0; RC_LOAD(fv_, %s, 8, RC_U64, %s); rc_fpu_write_d(h, sr, %d, fv_); }"
                             % (ea, fault, ft))
            if n == "swc1":
                return cu + "RC_STORE(%s, 4, rc_fpu_read_s(h, sr, %d), %s);" % (ea, ft, fault)
            if n == "sdc1":
                return cu + "RC_STORE(%s, 8, rc_fpu_read_d(h, sr, %d), %s);" % (ea, ft, fault)
        if c == "fop":
            return cu + "RC_COP1(0x%08Xu, %s, %d);" % (i.word, hx(i.addr), ds)
        raise ValueError("cannot lift %s" % i.text())

    # ---- instructions ----
    def plain(self, a):
        i = self.img.ins(a)
        code = self.body(i, "RC_INTERP(%s);" % hx(a), 0)
        if i.cls == "interp":
            self.fallbacks += 1
        if code:
            self.emit("    %s\n" % code)

    def transfer(self, a):
        """A branch / jump at a with its delay slot inline (every path ends in a goto or an exit)."""
        i = self.img.ins(a)
        s = a + 4
        if not self.inside(s) or self.img.ins(s).is_transfer():
            self.emit("    RC_INTERP(%s); /* no delay slot here / branch in a delay slot */\n" % hx(a))
            self.fallbacks += 1
            return
        n = i.name
        rs, rt = R(i.rs), R(i.rt)
        static = i.target is not None
        if i.cls == "branch":
            if n.startswith("bc1"):
                self.emit("    if (!RC_CU1()) RC_INTERP(%s);\n" % hx(a))
                cond = "(h->cpu.fcsr & RC_FCSR_C) %s 0" % ("!=" if n in ("bc1t", "bc1tl") else "==")
            elif n in ("beq", "beql"):
                cond = "%s == %s" % (rs, rt)
            elif n in ("bne", "bnel"):
                cond = "%s != %s" % (rs, rt)
            elif n in ("blez", "blezl"):
                cond = "(int64_t)%s <= 0" % rs
            elif n in ("bgtz", "bgtzl"):
                cond = "(int64_t)%s > 0" % rs
            elif n.startswith("bltz"):
                cond = "(int64_t)%s < 0" % rs
            else:
                cond = "(int64_t)%s >= 0" % rs
            self.emit("    tk = %s;\n" % cond)
        elif i.cls == "jr":
            self.emit("    jt = %s;\n" % rs)
        if i.link:
            self.emit("    %s = %s;\n" % (R(i.link), c64(sx32(a) + 8)))
        tgt = sx(i.target) if static else "jt"
        fall = c64(sx32(a) + 8)
        if i.cls == "branch" and i.likely:
            self.emit("    if (!tk) { %s }\n" % self.go(a + 8))
            npc = tgt
        elif i.cls == "branch":
            npc = "tk ? %s : %s" % (tgt, fall)
        else:
            npc = tgt
        si = self.img.ins(s)
        self.emit("    RC_I_SLOT(%s, %s);\n" % (hx(s), npc))
        code = self.body(si, "RC_INTERP_SLOT(%s, %s);" % (hx(s), npc), 1)
        if si.cls == "interp":
            self.fallbacks += 1
        if code:
            self.emit("    %s\n" % code)
        if i.cls == "branch" and not i.likely:
            self.emit("    if (tk) { %s }\n" % self.go(i.target))
            self.emit("    %s\n" % self.go(a + 8))
        elif static:
            self.emit("    %s\n" % self.go(i.target))
        else:
            self.emit("    RC_EXIT64(jt);\n")

    def function(self, comments=True):
        f = self.f
        cpi = "N64_RC_CPI_UNCACHED" if 0xA0000000 <= f.start < 0xC0000000 else "N64_RC_CPI_CACHED"
        # which instructions are emitted: all but delay slots (they run inline after their branch) unless targeted
        emitted = []
        slot = set()
        for a in f.addrs:
            i = self.img.ins(a)
            if a not in slot or a in self.targets:
                emitted.append(a)
            if i.is_transfer() and self.inside(a + 4) and not self.img.ins(a + 4).is_transfer():
                slot.add(a + 4)
        used, written = set(), set()
        for a in f.addrs:
            i = self.img.ins(a)
            used.update(i.reads())
            used.update(i.writes())
            written.update(i.writes())
        regs = sorted(used)
        self.emit("RC_FN %s(n64_machine *m_)\n{\n" % self.name)
        self.emit("    RC_ENTER(%s);\n" % cpi)
        for k in range(0, len(regs), 6):
            self.emit("    uint64_t %s;\n" % ", ".join("r%d = h->cpu.gpr[%d]" % (r, r) for r in regs[k:k + 6]))
        labels = [a for a in emitted if self.resume_all or self.needs_label(a)]
        self.emit("    switch ((uint32_t)h->cpu.pc) {\n")
        for a in labels:
            self.emit("    case %s: goto L_%08X;\n" % (hx(a), a))
        self.emit("    default: RC_NOT_COVERED();\n    }\n")
        label_set = set(labels)
        self.labels = len(labels)
        for a in emitted:
            i = self.img.ins(a)
            if a in label_set:
                self.emit("L_%08X:%s\n" % (a, " /* %s */" % i.text() if comments else ""))
            elif comments:
                self.emit("    /* %08X: %s */\n" % (a, i.text()))
            self.emit("    RC_I(%s);\n" % hx(a))
            if i.is_transfer():
                self.transfer(a)
            else:
                self.plain(a)
        self.emit("    RC_EXIT(%s);\n" % hx(f.end))
        self.emit("    RC_EPILOGUE_BEGIN();\n")
        for r in sorted(written):
            self.emit("    h->cpu.gpr[%d] = r%d;\n" % (r, r))
        self.emit("    RC_EPILOGUE_END();\n}\n")
        return "".join(self.out)


__all__ = ["FnLifter", "hx", "is_direct"]
