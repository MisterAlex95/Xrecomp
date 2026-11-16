"""Allegrex to C (XBA-224): ps1_tools' discovery and lifter (consoles/ps1/tools), subclassed. ps1_tools stays
unchanged (the PS1's output is byte-identical); these classes add the Allegrex and undo the PS1 assumptions that
break on the PSP:

- no load delay slot: a load's value is there for the next instruction, so commit() always lands it at once (the
  PS1 lifter stops with the load in flight when the next instruction reads the register: on the PSP that would be
  wrong, not slow). A load in the delay slot of JR / JALR still leaves through RC_EXIT_LD, which psp_recomp_rt.h
  lands right after the function returns;
- every load and store is a "RAM access" (no IO register heuristic: io_accesses() knows the PS1's map): main RAM
  inline, anything else (VRAM, the scratchpad, IO) through the core, never handed to the interpreter unless it
  faults (psp_recomp_rt.h);
- opcode 0x12 / 0x32 / 0x3A are the VFPU, not the GTE: handed to the interpreter (which stops: no VFPU yet);
- the likely branches (slot only when taken) and BC1F / BC1T(L) (FPU condition), the Allegrex ALU additions,
  MADD / MSUB, LL / SC, SYNC / CACHE, and the single-precision FPU (FPU registers stay in the machine head,
  h->f[], not in locals);
- the cycle model is the PS1's (one per instruction, 3 more per LW): only the event checks use it, the PSP spike
  has no cycle-exact timing to match.
"""

from ps1_tools.discover import Discovery, Image
from ps1_tools.lift import FnLifter, R, hx

from .isa import decode


class PspImage(Image):
    """ps1_tools' Image with the Allegrex decoder."""

    def ins(self, addr):
        i = self._cache.get(addr)
        if i is None:
            i = self._cache[addr] = decode(self.word(addr), addr)
        return i


class PspDiscovery(Discovery):
    """ps1_tools' discovery as it is (linear walks, JAL / J targets, LUI + ADDIU pointers, jump tables, code
    pointers in data) over a PspImage: the likely branches are branches, the Allegrex encodings are no longer
    "reserved" (which ends a function)."""


def F(n):
    return "h->f[%d]" % n


class PspLifter(FnLifter):
    def __init__(self, func, name, resume_all=False):
        super().__init__(func, name, resume_all)
        self.io = set()  # no IO heuristic: every access is a RAM access with the core as its slow path

    def commit(self, rt, val, nxt, static_next):
        """No load delay on the Allegrex: the value lands before the next instruction, whatever it is."""
        return "%s = %s;" % (R(rt), val) if rt else ""

    def body(self, i, fault, hand=None):
        n = i.name
        hand = hand or fault
        rs, rt, rd = R(i.rs), R(i.rt), R(i.rd)
        simm = i.simm & 0xFFFFFFFF
        addr = "%s + %s" % (rs, hx(simm)) if simm else rs

        def w(reg, expr):
            return "" if reg == 0 else "%s = %s;" % (R(reg), expr)

        if n in ("movz", "movn"):
            if i.rd == 0:
                return "", None, False
            return "if (%s %s 0u) %s = %s;" % (rt, "==" if n == "movz" else "!=", rd, rs), None, False
        alu = {
            "clz": "rc_clz(%s)" % rs, "clo": "rc_clz(~%s)" % rs,
            "max": "((int32_t)%s > (int32_t)%s ? %s : %s)" % (rs, rt, rs, rt),
            "min": "((int32_t)%s < (int32_t)%s ? %s : %s)" % (rs, rt, rs, rt),
            "rotr": "rc_rotr(%s, %d)" % (rt, i.sa), "rotrv": "rc_rotr(%s, %s & 31)" % (rt, rs),
            "seb": "(uint32_t)(int32_t)(int8_t)%s" % rt, "seh": "(uint32_t)(int32_t)(int16_t)%s" % rt,
            "wsbh": "rc_wsbh(%s)" % rt, "wsbw": "__builtin_bswap32(%s)" % rt, "bitrev": "rc_bitrev(%s)" % rt,
        }
        if n in alu:
            return w(i.rd, alu[n]), None, False
        if n == "ext":
            size = i.rd + 1
            mask = 0xFFFFFFFF if size >= 32 else (1 << size) - 1
            return w(i.rt, "(%s >> %d) & %s" % (rs, i.sa, hx(mask))), None, False
        if n == "ins":
            size = i.rd - i.sa + 1
            if size <= 0:
                return fault, None, False
            mask = (0xFFFFFFFF if size >= 32 else (1 << size) - 1) << i.sa
            return w(i.rt, "(%s & %s) | ((%s << %d) & %s)" % (rt, hx(~mask), rs, i.sa, hx(mask))), None, False
        if n in ("madd", "maddu", "msub", "msubu"):
            prod = ("(int64_t)(int32_t)%s * (int64_t)(int32_t)%s" % (rs, rt) if n in ("madd", "msub")
                    else "(int64_t)((uint64_t)%s * (uint64_t)%s)" % (rs, rt))
            op = "+" if n.startswith("madd") else "-"
            return ("{ uint64_t ea = (((uint64_t)hi << 32) | lo) %s (uint64_t)(%s); lo = (uint32_t)ea; "
                    "hi = (uint32_t)(ea >> 32); }" % (op, prod)), None, False
        if i.cls == "nop":
            return ";", None, False
        if n == "ll":
            return "RC_LW_RAM(%s, 3u, ldv, %s);" % (addr, hand), i.rt, True
        if n == "sc":
            return "RC_ST_RAM(uint32_t, %s, 3u, %s, %s); %s" % (addr, rt, hand, w(i.rt, "1u")), None, True
        if n == "lwc1":
            return "{ uint32_t ew = 0; RC_LW_RAM(%s, 3u, ew, %s); rc_fset(&%s, ew); }" % (addr, hand, F(i.ft)), \
                None, True
        if n == "swc1":
            return "RC_ST_RAM(uint32_t, %s, 3u, rc_fbits(%s), %s);" % (addr, F(i.ft), hand), None, True
        if i.cls == "fpu":
            return self.fpu(i, fault), None, False
        return super().body(i, fault, hand)

    def fpu(self, i, fault):
        n = i.name
        fd, fs, ft = F(i.fd), F(i.fs), F(i.ft)
        if n == "mfc1":
            return "" if i.rt == 0 else "%s = rc_fbits(%s);" % (R(i.rt), fs)
        if n == "mtc1":
            return "rc_fset(&%s, %s);" % (fs, R(i.rt))
        if n == "cfc1":
            return "" if i.rt == 0 else "%s = %s;" % (R(i.rt), "h->fcr31" if i.fs == 31 else "RC_FCR0")
        if n == "ctc1":
            return "h->fcr31 = %s & 0x0183FFFFu;" % R(i.rt) if i.fs == 31 else ";"
        ops = {"add.s": "%s + %s", "sub.s": "%s - %s", "mul.s": "%s * %s", "div.s": "%s / %s"}
        if n in ops:
            return "%s = %s;" % (fd, ops[n] % (fs, ft))
        un = {"sqrt.s": "rc_sqrtf(%s)", "abs.s": "rc_fabsf(%s)", "mov.s": "%s", "neg.s": "-%s",
              "cvt.s.w": "(float)(int32_t)rc_fbits(%s)"}
        if n in un:
            return "%s = %s;" % (fd, un[n] % fs)
        cv = {"round.w.s": 0, "trunc.w.s": 1, "ceil.w.s": 2, "floor.w.s": 3}
        if n in cv:
            return "rc_fset(&%s, rc_cvt_w(%s, %d));" % (fd, fs, cv[n])
        if n == "cvt.w.s":
            return "rc_fset(&%s, rc_cvt_w(%s, (int)(h->fcr31 & 3u)));" % (fd, fs)
        if n.startswith("c."):
            from .isa import FPU_COND
            cond = FPU_COND.index(n[2:-2])
            return "h->fcr31 = rc_fcmp(h->fcr31, %s, %s, %d);" % (fs, ft, cond)
        return fault

    def transfer(self, a):
        """Likely branches (the slot runs only when taken) and BC1x; the others are ps1_tools'."""
        i = self.img.ins(a)
        if not (i.likely or i.name.startswith("bc1")):
            return super().transfer(a)
        s = a + 4
        if not self.inside(s) or self.img.ins(s).is_transfer():
            self.emit("    RC_INTERP(%s); /* no delay slot here / branch in a delay slot */\n" % hx(a))
            self.fallbacks += 1
            return False
        n = i.name
        rs, rt = R(i.rs), R(i.rt)
        if n.startswith("bc1"):
            cond = "(h->fcr31 >> 23) & 1u" if n in ("bc1t", "bc1tl") else "!((h->fcr31 >> 23) & 1u)"
        else:
            base = n[:-1]  # the likely branch's plain form
            cond = {"beq": "%s == %s" % (rs, rt), "bne": "%s != %s" % (rs, rt), "blez": "(int32_t)%s <= 0" % rs,
                    "bgtz": "(int32_t)%s > 0" % rs, "bltz": "(int32_t)%s < 0" % rs,
                    "bgez": "(int32_t)%s >= 0" % rs, "bltzal": "(int32_t)%s < 0" % rs,
                    "bgezal": "(int32_t)%s >= 0" % rs}[base]
        self.emit("    tk = %s;\n" % cond)
        if i.link:
            self.emit("    %s = %s;\n" % (R(i.link), hx(a + 8)))
        si = self.img.ins(s)
        fault = "RC_INTERP_SLOT(%s, %s, 1);" % (hx(s), hx(i.target))
        code, ld, _ = self.body(si, fault)
        if si.cls == "interp":
            self.fallbacks += 1
        if not i.likely:  # BC1F / BC1T: an ordinary delay slot
            self.emit("    RC_I();\n    %s\n" % code)
            if ld:
                self.emit("    %s\n" % self.commit(ld, "ldv", 0, False))
            self.emit("    if (tk) { %s }\n    %s\n" % (self.go(i.target), self.go(s + 4)))
            return True
        land = (" " + self.commit(ld, "ldv", 0, False)) if ld else ""
        self.emit("    if (tk) { RC_I(); %s%s %s }\n" % (code, land, self.go(i.target)))
        self.emit("    %s\n" % self.go(s + 4))
        return True

    def function(self, comments=True):
        return super().function(comments).replace("(ps1_machine *m_)", "(psp_machine *m_)")
