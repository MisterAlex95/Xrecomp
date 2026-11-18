"""The Gekko layer on the shared PowerPC frontend (XBA-225, completed by XBA-260): paired singles, quantised loads /
stores, and what makes lifted code exact on the Gekko (see GekkoLifter).

tools.ppc decodes and lifts the 750's base instruction set; the Gekko adds, on opcodes the base leaves open:

- opcode 4: paired-single arithmetic (ps_add ... ps_nmadd, ps_sum0/1, ps_muls0/1, ps_madds0/1, ps_sel, ps_res,
  ps_rsqrte), moves (ps_mr, ps_neg, ps_abs, ps_nabs, ps_merge00..11), compares (ps_cmpu0/1, ps_cmpo0/1), the
  indexed quantised loads / stores (psq_lx, psq_stx, psq_lux, psq_stux) and dcbz_l (a locked-cache line);
- opcodes 56 / 57 / 60 / 61: psq_l, psq_lu, psq_st, psq_stu (12-bit displacement, W, GQR index I).

Semantics are the runtime's (consoles/gc/runtime gc_cpu.c and gc_recomp_rt.h): arithmetic rounds each slot to single
precision, quantised accesses read the GQR at run time (RC_PSQ_L / RC_PSQ_ST: the GQRs are set by the program and
saved in thread contexts, so they are not constants of the code), single-precision base instructions fill both
slots (RC_FSET_S). Record forms (Rc = 1) go to the interpreter.
"""

from tools.ppc.isa import Decoder, sext
from tools.ppc.lift import RA0, FnLifter, R, hx

PS10 = {0: "ps_cmpu0", 32: "ps_cmpo0", 40: "ps_neg", 64: "ps_cmpu1", 72: "ps_mr", 96: "ps_cmpo1", 136: "ps_nabs",
        264: "ps_abs", 528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10", 624: "ps_merge11", 1014: "dcbz_l"}
PS6 = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}
PS5 = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0", 15: "ps_madds1",
       18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res", 25: "ps_mul", 26: "ps_rsqrte",
       28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub", 31: "ps_nmadd"}
PSQ_D = {56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}


class Quant:
    """W, GQR index and displacement of a psq_* instruction."""
    __slots__ = ("w", "i", "d", "text")

    def __init__(self, w, i, d, text):
        self.w, self.i, self.d, self.text = w, i, d, text


class GekkoDecoder(Decoder):
    def decode_ext(self, i):
        w = i.word
        if i.op in PSQ_D:
            i.name = PSQ_D[i.op]
            q = Quant((w >> 15) & 1, (w >> 12) & 7, sext(w & 0xFFF, 12), None)
            q.text = "%s f%d, %d(r%d), %d, %d" % (i.name, i.rd, q.d, i.ra, q.w, q.i)
            i.ext = q
            return
        xo10, xo6, xo5 = (w >> 1) & 0x3FF, (w >> 1) & 63, (w >> 1) & 31
        if xo10 in PS10:
            i.name = PS10[xo10]
        elif xo6 in PS6:
            i.name = PS6[xo6]
            q = Quant((w >> 10) & 1, (w >> 7) & 7, 0, None)
            q.text = "%s f%d, r%d, r%d, %d, %d" % (i.name, i.rd, i.ra, i.rb, q.w, q.i)
            i.ext = q
        elif xo5 in PS5:
            i.name = PS5[xo5]

    def disasm(self, i):
        if i.op == 4 and i.name and i.ext is None:
            if i.name == "dcbz_l":
                return "dcbz_l r%d, r%d" % (i.ra, i.rb)
            if i.name.startswith("ps_cmp"):
                return "%s cr%d, f%d, f%d" % (i.name, i.crfd, i.ra, i.rb)
            return "%s%s f%d, f%d, f%d, f%d" % (i.name, "." if i.rec else "", i.rd, i.ra, i.rc, i.rb)
        return super().disasm(i)


def _slots(expr0, expr1):
    return "F0(d_) = %s; F1(d_) = %s;" % (expr0, expr1)


def cycles(word):
    """The cycle estimate of one instruction: the interpreter's gc_cycles() (consoles/gc/runtime/gc_cpu.c), the same
    function on the raw word so the two cannot disagree on any encoding. 1 for most instructions; mulli 3, mullw 4,
    mulhw / mulhwu 5, divw / divwu 19, fdivs / ps_div 17, fdiv 31, double-precision multiplies and multiply-adds 2,
    lmw / stmw one per register plus one, sc / rfi / isync 2, sync 3. An estimate (the 750's latencies, rounded),
    not a pipeline model: it paces the time base and the decrementer."""
    op, xo, x5 = word >> 26, (word >> 1) & 0x3FF, (word >> 1) & 31
    if op in (4, 59):
        return 17 if x5 == 18 else 1
    if op == 7:
        return 3
    if op == 17:
        return 2
    if op == 19:
        return 2 if xo in (150, 50) else 1
    if op == 31:
        x9 = xo & 0x1FF
        if x9 in (459, 491):
            return 19
        if x9 == 235:
            return 4
        if x9 in (11, 75):
            return 5
        return 3 if xo == 598 else 1
    if op in (46, 47):
        return 33 - ((word >> 21) & 31)
    if op == 63:
        if not x5 & 16:
            return 1
        return 31 if x5 == 18 else 2 if (x5 == 25 or x5 >= 28) else 1
    return 1


HID2_LSQE, HID2_PSE, HID2_LCE = 0x80000000, 0x20000000, 0x10000000
TIME_SPRS = (22, 268, 269)  # DEC, TBL, TBU


def _trap_cond(to, a, b):
    """C condition of tw / twi with TO = to on signed operands a, b (C expressions)."""
    terms = []
    if to & 16:
        terms.append("(int32_t)%s < (int32_t)%s" % (a, b))
    if to & 8:
        terms.append("(int32_t)%s > (int32_t)%s" % (a, b))
    if to & 4:
        terms.append("%s == %s" % (a, b))
    if to & 2:
        terms.append("(uint32_t)%s < (uint32_t)%s" % (a, b))
    if to & 1:
        terms.append("(uint32_t)%s > (uint32_t)%s" % (a, b))
    return " || ".join(terms)


class GekkoLifter(FnLifter):
    """Paired singles and quantised accesses, plus what makes lifted code exact on the Gekko (XBA-260): blocks carry
    the cycle estimate (RC_BLOCK(pc, cycles, n), time reads and hand-offs mid-block give back the cycles after
    them), memory macros carry the instruction's address (DSI), FP / paired-single / quantised / dcbz_l /
    privileged instructions are guarded (MSR[FP], HID2 enables, MSR[PR]: the interpreter raises the exception),
    tw / twi raise the program exception themselves, lfs loads without rounding, fres / frsqrte use the 750's
    estimate tables."""

    IDLE_POLL = True  # one CPU: a polled flag only changes at an event (interrupt, DMA)

    FSET_LOAD = "RC_FSET_L"

    def cycles(self, i):
        return cycles(i.word)

    def block_head(self, a, insns):
        native = [i for i in insns if not self.needs_interp(i)]
        return "RC_BLOCK(%s, %d, %d);" % (hx(a), sum(self.cycles(i) for i in native), len(native))

    def mem(self, macro, i, *args):
        return "%s(%s)" % (macro, ", ".join((hx(i.addr),) + args))

    def gekko_bits(self, i):
        if i.op in PSQ_D or i.name in PS6.values():
            return HID2_LSQE
        if i.op == 4:
            return HID2_PSE
        return 0

    def guard(self, i, state):
        if not self.is_fp(i):
            return ""
        bits = self.gekko_bits(i)
        if state.get("fp") and (state["bits"] | bits) == state["bits"]:
            return ""
        state["fp"] = True
        state["bits"] = state.get("bits", 0) | bits
        return "RC_GKCHECK_T(%s, %s, %d);" % (hx(i.addr), hx(state["bits"]), self.cycles_from)

    def is_fp(self, i):
        if i.op in PSQ_D or (i.op == 4 and i.name and i.name != "dcbz_l"):
            return True
        return super().is_fp(i)

    def needs_interp(self, i):
        if i.op == 4 and i.name and i.name.startswith("ps_") and i.rec:
            return True
        if i.name in ("tw", "twi"):
            return False
        return super().needs_interp(i)

    # privileged instructions, traps, time reads
    def priv(self, i):
        return "RC_PRIV_T(%s, %d); " % (hx(i.addr), self.cycles_from)

    def op_mfmsr(self, i):
        return self.priv(i) + super().op_mfmsr(i)

    def op_mtmsr(self, i):
        return self.priv(i) + super().op_mtmsr(i)

    def op_mfspr(self, i):
        if i.spr in TIME_SPRS:
            body = "%s = RC_MFSPR_T(%d, %d);" % (R(i.rd), i.spr, self.cycles_after)
        else:
            body = super().op_mfspr(i)
        return (self.priv(i) if i.name == "mfspr" and i.spr & 0x10 else "") + body

    op_mftb = op_mfspr

    def op_mtspr(self, i):
        return (self.priv(i) if i.spr & 0x10 else "") + super().op_mtspr(i)

    def op_dcbi(self, i):
        return self.priv(i).rstrip()

    def _trap(self, i, b):
        cond = _trap_cond(i.rd, R(i.ra), b)
        if not cond:
            return ""
        return "if (%s) RC_TRAP(%s, %d);" % (cond, hx(i.addr), self.cycles_after)

    def op_tw(self, i): return self._trap(i, R(i.rb))
    def op_twi(self, i): return self._trap(i, hx(i.simm))

    def op_dcbz_l(self, i):
        return "RC_LCCHECK_T(%s, %d); %s;" % (hx(i.addr), self.cycles_from,
                                             self.mem("RC_DCBZ_L", i, "%s + %s" % (RA0(i.ra), R(i.rb))))

    # estimates
    def op_fres(self, i): return "RC_FSET_S(%d, gc_fres(F0(%d)));" % (i.rd, i.rb)
    def op_frsqrte(self, i): return "F0(%d) = gc_frsqrte(F0(%d));" % (i.rd, i.rb)


    # quantised loads and stores
    def _psq(self, i):
        q, n = i.ext, i.name
        upd = n.endswith("u") or n.endswith("ux")
        if i.op == 4:
            ea = "%s + %s" % (R(i.ra) if upd else RA0(i.ra), R(i.rb))
        else:
            base = R(i.ra) if upd else RA0(i.ra)
            ea = "%s + %s" % (base, hx(q.d)) if q.d else base
        fn = "RC_PSQ_ST" if "st" in n else "RC_PSQ_L"
        call = self.mem(fn, i, str(i.rd), "ea_", str(q.w), str(q.i))
        return "{ uint32_t ea_ = %s; %s;%s }" % (ea, call, " %s = ea_;" % R(i.ra) if upd else "")

    op_psq_l = op_psq_lu = op_psq_st = op_psq_stu = op_psq_lx = op_psq_stx = op_psq_lux = op_psq_stux = _psq

    # paired-single arithmetic: operands read first (frD may be one of them)
    def _ps(self, i, e0, e1):
        return ("{ const unsigned d_ = %d; double a0 = F0(%d), a1 = F1(%d), b0 = F0(%d), b1 = F1(%d), "
                "c0 = F0(%d), c1 = F1(%d); (void)a0; (void)a1; (void)b0; (void)b1; (void)c0; (void)c1; %s }"
                % (i.rd, i.ra, i.ra, i.rb, i.rb, i.rc, i.rc, _slots(e0, e1)))

    def op_ps_add(self, i): return self._ps(i, "RC_PS(a0 + b0)", "RC_PS(a1 + b1)")
    def op_ps_sub(self, i): return self._ps(i, "RC_PS(a0 - b0)", "RC_PS(a1 - b1)")
    def op_ps_mul(self, i): return self._ps(i, "RC_PS(a0 * c0)", "RC_PS(a1 * c1)")
    def op_ps_div(self, i): return self._ps(i, "RC_PS(a0 / b0)", "RC_PS(a1 / b1)")
    def op_ps_madd(self, i): return self._ps(i, "RC_PS(fma(a0, c0, b0))", "RC_PS(fma(a1, c1, b1))")
    def op_ps_msub(self, i): return self._ps(i, "RC_PS(fma(a0, c0, -b0))", "RC_PS(fma(a1, c1, -b1))")
    def op_ps_nmadd(self, i): return self._ps(i, "ppc_fnma(RC_PS(fma(a0, c0, b0)))", "ppc_fnma(RC_PS(fma(a1, c1, b1)))")
    def op_ps_nmsub(self, i):
        return self._ps(i, "ppc_fnma(RC_PS(fma(a0, c0, -b0)))", "ppc_fnma(RC_PS(fma(a1, c1, -b1)))")
    def op_ps_sum0(self, i): return self._ps(i, "RC_PS(a0 + b1)", "c1")
    def op_ps_sum1(self, i): return self._ps(i, "c0", "RC_PS(a0 + b1)")
    def op_ps_muls0(self, i): return self._ps(i, "RC_PS(a0 * c0)", "RC_PS(a1 * c0)")
    def op_ps_muls1(self, i): return self._ps(i, "RC_PS(a0 * c1)", "RC_PS(a1 * c1)")
    def op_ps_madds0(self, i): return self._ps(i, "RC_PS(fma(a0, c0, b0))", "RC_PS(fma(a1, c0, b1))")
    def op_ps_madds1(self, i): return self._ps(i, "RC_PS(fma(a0, c1, b0))", "RC_PS(fma(a1, c1, b1))")
    def op_ps_sel(self, i): return self._ps(i, "ppc_fsel(a0, c0, b0)", "ppc_fsel(a1, c1, b1)")
    def op_ps_res(self, i): return self._ps(i, "gc_fres(b0)", "gc_fres(b1)")
    def op_ps_rsqrte(self, i): return self._ps(i, "RC_PS(gc_frsqrte(b0))", "RC_PS(gc_frsqrte(b1))")
    def op_ps_mr(self, i): return self._ps(i, "b0", "b1")
    def op_ps_neg(self, i): return self._ps(i, "ppc_fneg(b0)", "ppc_fneg(b1)")
    def op_ps_abs(self, i): return self._ps(i, "ppc_fabs(b0)", "ppc_fabs(b1)")
    def op_ps_nabs(self, i): return self._ps(i, "ppc_fnabs(b0)", "ppc_fnabs(b1)")
    def op_ps_merge00(self, i): return self._ps(i, "a0", "b0")
    def op_ps_merge01(self, i): return self._ps(i, "a0", "b1")
    def op_ps_merge10(self, i): return self._ps(i, "a1", "b0")
    def op_ps_merge11(self, i): return self._ps(i, "a1", "b1")

    def _pscmp(self, i, slot):
        return ("{ uint32_t f_ = ppc_fcmp(F%d(%d), F%d(%d)); c->cr = ppc_cr_set(c->cr, %d, f_); "
                "c->fpscr = (c->fpscr & ~0xF000u) | (f_ << 12); }" % (slot, i.ra, slot, i.rb, i.crfd))

    def op_ps_cmpu0(self, i): return self._pscmp(i, 0)
    def op_ps_cmpo0(self, i): return self._pscmp(i, 0)
    def op_ps_cmpu1(self, i): return self._pscmp(i, 1)
    def op_ps_cmpo1(self, i): return self._pscmp(i, 1)
