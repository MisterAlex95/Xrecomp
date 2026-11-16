"""R5900 to C (XBA-226): ps1_tools' discovery and lifter (consoles/ps1/tools), subclassed. ps1_tools stays unchanged
(the PS1's output is byte-identical); the function's shape is the PS1's (blocks entered from a switch, one event
check per segment, delay slots inline, calls and returns through the dispatcher, RC_INTERP / RC_HAND hand-offs),
every instruction's C is the R5900's:

- GPRs are 128 bits: the low 64 in uint64_t locals (function() widens the PS1's uint32_t declarations), the high 64
  in the head (h->rh[], used in place by the MMI ops and LQ / SQ: the PS1 lifter's local-register regex only knows
  r1-r31 / hi / lo). 32-bit ops sign-extend their result (PS2_SX32); SLL / SRA ... read the low word; HI / LO are
  64-bit locals, HI1 / LO1 stay in the head;
- no load delay slot: commit() always lands a value at once (the PS1 lifter would stop with the load in flight);
- memory: every load and store is a "RAM access" (no IO heuristic: io_accesses() knows the PS1's map): main RAM
  inline, anything else through the core (ps2_recomp_rt.h), handed to the interpreter only when it faults;
- likely branches (slot only when taken) and BC1F / BC1T(L); MULT / MULTU write rd; the pipeline-1 forms; MADD;
- the FPU through ps2_alu.h's helpers (the FPU registers are bit patterns in the head: h->f[], h->acc, h->fcr31);
- MMI ops through ps2_alu.h's ps2_mmi_* (the same functions the interpreter calls);
- SYSCALL, BREAK, COP0, COP2, the SA register and the MMI ops not in isa's tables go to the interpreter (RC_INTERP).
"""

import re

from ps1_tools.discover import Discovery, Image
from ps1_tools.lift import FnLifter, R, hx

from .isa import FPU_S, MMI_ONE, MMI_SHIFTS, decode


def c64(v):
    return "0x%016XULL" % (v & 0xFFFFFFFFFFFFFFFF)


def sx32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


class Ps2Image(Image):
    """ps1_tools' Image with the R5900 decoder."""

    def ins(self, addr):
        i = self._cache.get(addr)
        if i is None:
            i = self._cache[addr] = decode(self.word(addr), addr)
        return i


class Ps2Discovery(Discovery):
    """ps1_tools' discovery as it is (linear walks, JAL / J targets, LUI + ADDIU pointers, jump tables, code
    pointers in the code) over a Ps2Image: the R5900 encodings are no longer "reserved" (which ends a function)."""


def F(n):
    return "h->f[%d]" % n


def Q(r):
    """The 128-bit value of GPR r."""
    return "RC_Q(%s, %d)" % (R(r), r)


LOADS = {"lb": ("int8_t", 0), "lh": ("int16_t", 1), "lw": ("int32_t", 3), "lbu": ("uint8_t", 0),
         "lhu": ("uint16_t", 1), "lwu": ("uint32_t", 3), "ld": ("uint64_t", 7)}
STORES = {"sb": ("uint8_t", 0), "sh": ("uint16_t", 1), "sw": ("uint32_t", 3), "sd": ("uint64_t", 7)}
UNALIGNED = {"lwl": 0, "lwr": 1, "ldl": 2, "ldr": 3, "swl": 4, "swr": 5, "sdl": 6, "sdr": 7}
TCOND = {"tge": "(int64_t)%s >= (int64_t)%s", "tgeu": "%s >= %s", "tlt": "(int64_t)%s < (int64_t)%s",
         "tltu": "%s < %s", "teq": "%s == %s", "tne": "%s != %s"}
FPU_BIN = {"add.s": "ps2_fadd", "sub.s": "ps2_fsub", "mul.s": "ps2_fmul", "div.s": "ps2_fdiv",
           "max.s": "ps2_fmax", "min.s": "ps2_fmin", "rsqrt.s": "ps2_frsqrt"}
FUNCT = {v: k for k, v in FPU_S.items()}
DECL_RE = re.compile(r"^    (const )?uint32_t (?=(?:r\d+|hi|lo) = h->)", re.M)


class Ps2Lifter(FnLifter):
    def __init__(self, func, name, resume_all=False):
        super().__init__(func, name, resume_all)
        self.io = set()  # no IO heuristic: every access is a RAM access with the core as its slow path

    def commit(self, rt, val, nxt, static_next):
        """No load delay on the R5900 (body() assigns loads at once and never asks for this)."""
        return "%s = %s;" % (R(rt), val) if rt else ""

    def body(self, i, fault, hand=None):
        n, c = i.name, i.cls
        hand = hand or fault
        rs, rt = R(i.rs), R(i.rt)
        simm = i.simm & 0xFFFFFFFF
        addr = "(uint32_t)%s + %s" % (rs, hx(simm)) if simm else "(uint32_t)%s" % rs

        def w(reg, expr):
            return "" if reg == 0 else "%s = %s;" % (R(reg), expr)

        def sx(expr):
            return "PS2_SX32(%s)" % expr

        if c == "interp":
            return fault, None, False
        if c == "nop":
            return ";", None, False
        if c == "alu":
            return self.alu(i, w, sx), None, False
        if c == "trap":
            return self.trap(i, fault, w, sx), None, False
        if c == "muldiv":
            return self.muldiv(i), None, False
        if c == "load":
            # through ldv, then an assignment the PS1 lifter's register regex sees (a write it must store back)
            land = " %s = ldv;" % rt if i.rt else ""
            if n in LOADS:
                t, al = LOADS[n]
                return "RC_LD_RAM(%s, %s, %du, ldv, %s);%s" % (t, addr, al, hand, land), None, True
            if n == "lq":
                hi = "h->rh[%d]" % i.rt if i.rt else "ldv"
                return "RC_LQ(%s, ldv, %s, %s);%s" % (addr, hi, hand, land), None, True
            return "RC_LR(%d, %s, %s, ldv, %s);%s" % (UNALIGNED[n], addr, rt, hand, land), None, True
        if c == "store":
            if n in STORES:
                t, al = STORES[n]
                return "RC_ST_RAM(%s, %s, %du, %s, %s);" % (t, addr, al, rt, hand), None, True
            if n == "sq":
                hi = "h->rh[%d]" % i.rt if i.rt else "0u"
                return "RC_SQ(%s, %s, %s, %s);" % (addr, rt, hi, hand), None, True
            return "RC_LR(%d, %s, %s, ldv, %s);" % (UNALIGNED[n], addr, rt, hand), None, True
        if c == "fls":
            if n == "lwc1":
                return "RC_LD_RAM(uint32_t, %s, 3u, ldv, %s); %s = (uint32_t)ldv;" % (addr, hand, F(i.ft)), None, True
            return "RC_ST_RAM(uint32_t, %s, 3u, %s, %s);" % (addr, F(i.ft), hand), None, True
        if c == "fpu":
            return self.fpu(i, fault, w, sx), None, False
        if c == "mmi":
            return self.mmi(i), None, False
        raise ValueError("cannot lift %s" % i.text())

    def alu(self, i, w, sx):
        n = i.name
        rs, rt = R(i.rs), R(i.rt)
        sa = i.sa
        if n in ("movz", "movn"):
            if i.rd == 0:
                return ";"
            return "if (%s %s 0u) %s = %s;" % (rt, "==" if n == "movz" else "!=", R(i.rd), rs)
        three = {
            "sll": sx("(uint32_t)%s << %d" % (rt, sa)), "srl": sx("(uint32_t)%s >> %d" % (rt, sa)),
            "sra": sx("(uint32_t)((int32_t)%s >> %d)" % (rt, sa)),
            "sllv": sx("(uint32_t)%s << (%s & 31)" % (rt, rs)), "srlv": sx("(uint32_t)%s >> (%s & 31)" % (rt, rs)),
            "srav": sx("(uint32_t)((int32_t)%s >> (%s & 31))" % (rt, rs)),
            "dsllv": "(uint64_t)%s << (%s & 63)" % (rt, rs), "dsrlv": "(uint64_t)%s >> (%s & 63)" % (rt, rs),
            "dsrav": "(uint64_t)((int64_t)%s >> (%s & 63))" % (rt, rs),
            "mfhi": "hi", "mflo": "lo", "mfhi1": "h->hi1", "mflo1": "h->lo1",
            "addu": sx("(uint32_t)%s + (uint32_t)%s" % (rs, rt)), "subu": sx("(uint32_t)%s - (uint32_t)%s" % (rs, rt)),
            "and": "%s & %s" % (rs, rt), "or": "%s | %s" % (rs, rt), "xor": "%s ^ %s" % (rs, rt),
            "nor": "~((uint64_t)%s | %s)" % (rs, rt),
            "slt": "(uint64_t)((int64_t)%s < (int64_t)%s)" % (rs, rt), "sltu": "(uint64_t)(%s < %s)" % (rs, rt),
            "daddu": "%s + %s" % (rs, rt), "dsubu": "(uint64_t)%s - %s" % (rs, rt),
            "dsll": "(uint64_t)%s << %d" % (rt, sa), "dsrl": "(uint64_t)%s >> %d" % (rt, sa),
            "dsra": "(uint64_t)((int64_t)%s >> %d)" % (rt, sa),
            "dsll32": "(uint64_t)%s << %d" % (rt, sa + 32), "dsrl32": "(uint64_t)%s >> %d" % (rt, sa + 32),
            "dsra32": "(uint64_t)((int64_t)%s >> %d)" % (rt, sa + 32),
            "plzcw": "ps2_plzcw(%s)" % rs,
        }
        if n in three:
            return w(i.rd, three[n])
        imm = {
            "addiu": sx("(uint32_t)%s + %s" % (rs, hx(i.simm))), "daddiu": "%s + %s" % (rs, c64(i.simm)),
            "slti": "(uint64_t)((int64_t)%s < %dLL)" % (rs, i.simm), "sltiu": "(uint64_t)(%s < %s)" % (rs, c64(i.simm)),
            "andi": "%s & 0x%Xull" % (rs, i.imm), "ori": "%s | 0x%Xull" % (rs, i.imm),
            "xori": "%s ^ 0x%Xull" % (rs, i.imm), "lui": c64(sx32(i.imm << 16)),
        }
        return w(i.rt, imm[n])

    def trap(self, i, fault, w, sx):
        n = i.name
        rs, rt = R(i.rs), R(i.rt)
        if n in TCOND:
            return "if (%s) { %s }" % (TCOND[n] % (rs, rt), fault)
        if n in ("tgei", "tgeiu", "tlti", "tltiu", "teqi", "tnei"):
            return "if (%s) { %s }" % (TCOND[n[:-1]] % (rs, c64(i.simm)), fault)
        if n in ("add", "sub", "addi"):
            b, d = (hx(i.simm), i.rt) if n == "addi" else (rt, i.rd)
            op = "-" if n == "sub" else "+"
            ovf = "(ea ^ eb) & (ea ^ er)" if n == "sub" else "~(ea ^ eb) & (ea ^ er)"
            return ("{ uint32_t ea = (uint32_t)%s, eb = (uint32_t)%s, er = ea %s eb; if ((%s) & 0x80000000u) { %s } "
                    "%s }" % (rs, b, op, ovf, fault, w(d, sx("er"))))
        b, d = (c64(i.simm), i.rt) if n == "daddi" else (rt, i.rd)
        op = "-" if n == "dsub" else "+"
        ovf = "(ea ^ eb) & (ea ^ er)" if n == "dsub" else "~(ea ^ eb) & (ea ^ er)"
        return ("{ uint64_t ea = %s, eb = %s, er = ea %s eb; if (((%s) >> 63) & 1u) { %s } %s }"
                % (rs, b, op, ovf, fault, w(d, "er")))

    def muldiv(self, i):
        n = i.name
        rs, rt = R(i.rs), R(i.rt)
        pipe0 = {"mult": "ps2_mult", "multu": "ps2_multu", "div": "ps2_div", "divu": "ps2_divu", "madd": "ps2_madd",
                 "maddu": "ps2_maddu"}
        pipe1 = {"mult1": "ps2_mult", "multu1": "ps2_multu", "div1": "ps2_div", "divu1": "ps2_divu",
                 "madd1": "ps2_madd", "maddu1": "ps2_maddu"}
        setrd = n.startswith("mult") or n.startswith("madd")
        if n in pipe0:
            out = "%s(%s, %s, &hi, &lo);" % (pipe0[n], rs, rt)
            return out + (" %s = lo;" % R(i.rd) if setrd and i.rd else "")
        if n in pipe1:
            out = "%s(%s, %s, &h->hi1, &h->lo1);" % (pipe1[n], rs, rt)
            return out + (" %s = h->lo1;" % R(i.rd) if setrd and i.rd else "")
        return {"mthi": "hi = %s;" % rs, "mtlo": "lo = %s;" % rs, "mthi1": "h->hi1 = %s;" % rs,
                "mtlo1": "h->lo1 = %s;" % rs}[n]

    def fpu(self, i, fault, w, sx):
        n = i.name
        fd, fs, ft = F(i.fd), F(i.fs), F(i.ft)
        rt = R(i.rt)
        if n == "mfc1":
            return w(i.rt, sx(fs))
        if n == "mtc1":
            return "%s = (uint32_t)%s;" % (fs, rt)
        if n == "cfc1":
            return w(i.rt, sx("h->fcr31" if i.fs == 31 else "0x2E30u" if i.fs == 0 else "0u"))
        if n == "ctc1":
            return "h->fcr31 = (uint32_t)%s;" % rt if i.fs == 31 else ";"
        if n in FPU_BIN:
            return "%s = %s(%s, %s);" % (fd, FPU_BIN[n], fs, ft)
        acc = {"adda.s": "h->acc = ps2_fadd(%s, %s);", "suba.s": "h->acc = ps2_fsub(%s, %s);",
               "mula.s": "h->acc = ps2_fmul(%s, %s);", "madda.s": "h->acc = ps2_fadd(h->acc, ps2_fmul(%s, %s));",
               "msuba.s": "h->acc = ps2_fsub(h->acc, ps2_fmul(%s, %s));"}
        if n in acc:
            return acc[n] % (fs, ft)
        if n in ("madd.s", "msub.s"):
            return "%s = %s(h->acc, ps2_fmul(%s, %s));" % (fd, "ps2_fadd" if n == "madd.s" else "ps2_fsub", fs, ft)
        un = {"sqrt.s": "ps2_fsqrt(%s)" % ft, "abs.s": "%s & 0x7FFFFFFFu" % fs, "mov.s": fs,
              "neg.s": "%s ^ 0x80000000u" % fs, "cvt.w.s": "ps2_cvt_w(%s)" % fs, "cvt.s.w": "ps2_cvt_s(%s)" % fs}
        if n in un:
            return "%s = %s;" % (fd, un[n])
        if n.startswith("c."):
            return "h->fcr31 = ps2_fcmp(h->fcr31, %s, %s, 0x%02Xu);" % (fs, ft, FUNCT[n])
        return fault

    def mmi(self, i):
        n = i.name
        d = i.rd

        def put(expr):
            if d == 0:
                return ";"
            return "{ ps2_q q_ = %s; %s = RC_QLO(q_); h->rh[%d] = RC_QHI(q_); }" % (expr, R(d), d)

        if n == "pmfhi":
            return put("ps2_q_make(hi, h->hi1)")
        if n == "pmflo":
            return put("ps2_q_make(lo, h->lo1)")
        if n == "pmthi":
            return "{ ps2_q q_ = %s; hi = RC_QLO(q_); h->hi1 = RC_QHI(q_); }" % Q(i.rs)
        if n == "pmtlo":
            return "{ ps2_q q_ = %s; lo = RC_QLO(q_); h->lo1 = RC_QHI(q_); }" % Q(i.rs)
        if n in ("pmfhl.lw", "pmfhl.uw"):
            return put("ps2_pmfhl_%s(ps2_q_make(lo, h->lo1), ps2_q_make(hi, h->hi1))" % n[-2:])
        if n == "pmthl.lw":
            return ("{ ps2_q l_ = ps2_q_make(lo, h->lo1), u_ = ps2_q_make(hi, h->hi1); ps2_pmthl_lw(%s, &l_, &u_); "
                    "lo = RC_QLO(l_); h->lo1 = RC_QHI(l_); hi = RC_QLO(u_); h->hi1 = RC_QHI(u_); }" % Q(i.rs))
        if n in MMI_SHIFTS.values():
            return put("ps2_mmi_%s(%s, %du)" % (n, Q(i.rt), i.sa))
        if n in MMI_ONE:
            return put("ps2_mmi_%s(ps2_q_make(0, 0), %s)" % (n, Q(i.rt)))
        return put("ps2_mmi_%s(%s, %s)" % (n, Q(i.rs), Q(i.rt)))

    def transfer(self, a):
        """A branch / jump at a with its delay slot inline (likely: the slot only when taken). Returns True when the
        slot was consumed."""
        i = self.img.ins(a)
        s = a + 4
        if not self.inside(s) or self.img.ins(s).is_transfer():
            self.emit("    RC_INTERP(%s); /* no delay slot here / branch in a delay slot */\n" % hx(a))
            self.fallbacks += 1
            return False
        n = i.name
        rs, rt = R(i.rs), R(i.rt)
        static = i.target is not None
        tgt = hx(i.target) if static else "jt"
        if i.cls == "branch":
            base = n[:-1] if i.likely and not n.startswith("bc1") else n
            cond = {"beq": "%s == %s" % (rs, rt), "bne": "%s != %s" % (rs, rt), "blez": "(int64_t)%s <= 0" % rs,
                    "bgtz": "(int64_t)%s > 0" % rs, "bltz": "(int64_t)%s < 0" % rs, "bgez": "(int64_t)%s >= 0" % rs,
                    "bltzal": "(int64_t)%s < 0" % rs, "bgezal": "(int64_t)%s >= 0" % rs,
                    "bc1f": "!((h->fcr31 >> 23) & 1u)", "bc1t": "(h->fcr31 >> 23) & 1u",
                    "bc1fl": "!((h->fcr31 >> 23) & 1u)", "bc1tl": "(h->fcr31 >> 23) & 1u"}[base]
            self.emit("    tk = %s;\n" % cond)
        else:
            self.emit("    tk = 1;\n")
            if i.cls == "jr":
                self.emit("    jt = (uint32_t)%s;\n" % rs)
        if i.link:
            self.emit("    %s = %s;\n" % (R(i.link), c64(sx32(a + 8))))
        si = self.img.ins(s)
        if si.cls == "interp":
            self.fallbacks += 1
        if i.likely:
            code, _, _ = self.body(si, "RC_INTERP_SLOT(%s, %s, 1);" % (hx(s), tgt))
            self.emit("    if (tk) { RC_I(); %s %s }\n" % (code, self.go(i.target)))
            self.emit("    %s\n" % self.go(s + 4))
            return True
        npc = "tk ? %s : %s" % (tgt, hx(s + 4)) if i.cls == "branch" else tgt
        code, _, _ = self.body(si, "RC_INTERP_SLOT(%s, %s, tk);" % (hx(s), npc))
        self.emit("    RC_I();\n    %s\n" % code)
        taken = self.go(i.target) if static else "RC_EXIT(jt);"
        if i.cls == "branch":
            self.emit("    if (tk) { %s }\n    %s\n" % (taken, self.go(s + 4)))
        else:
            self.emit("    %s\n" % taken)
        return True

    def function(self, comments=True):
        text = super().function(comments).replace("(ps1_machine *m_)", "(ps2_machine *m_)")
        return DECL_RE.sub(lambda mo: "    %suint64_t " % (mo.group(1) or ""), text)
