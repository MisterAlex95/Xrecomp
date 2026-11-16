"""The Gekko layer on the shared PowerPC frontend (XBA-225): paired singles and quantised loads / stores.

tools.ppc decodes and lifts the 750's base instruction set; the Gekko adds, on opcodes the base leaves open:

- opcode 4: paired-single arithmetic (ps_add ... ps_nmadd, ps_sum0/1, ps_muls0/1, ps_madds0/1, ps_sel, ps_res,
  ps_rsqrte), moves (ps_mr, ps_neg, ps_abs, ps_nabs, ps_merge00..11), compares (ps_cmpu0/1, ps_cmpo0/1), the
  indexed quantised loads / stores (psq_lx, psq_stx, psq_lux, psq_stux) and dcbz_l (locked cache: interpreter);
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


class GekkoLifter(FnLifter):
    def is_fp(self, i):
        if i.op in PSQ_D or (i.op == 4 and i.name and i.name != "dcbz_l"):
            return True
        return super().is_fp(i)

    def needs_interp(self, i):
        if i.op == 4 and i.name and i.name.startswith("ps_") and i.rec:
            return True
        return super().needs_interp(i)

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
        return "{ uint32_t ea_ = %s; %s(%d, ea_, %d, %d);%s }" % (ea, fn, i.rd, q.w, q.i,
                                                                 " %s = ea_;" % R(i.ra) if upd else "")

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
    def op_ps_res(self, i): return self._ps(i, "ppc_fres(b0)", "ppc_fres(b1)")
    def op_ps_rsqrte(self, i): return self._ps(i, "RC_PS(ppc_frsqrte(b0))", "RC_PS(ppc_frsqrte(b1))")
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
