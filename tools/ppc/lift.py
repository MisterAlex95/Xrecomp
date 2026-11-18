"""PowerPC to C (XBA-225): one C function per discovered function, console-agnostic.

The C uses a small vocabulary of macros that the console's runtime header defines (the GameCube's is
consoles/gc/runtime/gc_recomp_rt.h); the lifter never names a console's types or functions, so the Xbox 360 layer
can reuse it with its own header:

  registers     R(n) (32-bit GPR), F0(n) (FPR / paired-single slot 0), c->lr, c->ctr, c->cr, c->xer_ca,
                c->xer_so, c->msr, c->fpscr; RC_XER() / RC_SET_XER(v)
  memory        RC_LBZ / RC_LHZ / RC_LWZ / RC_LD64 (ea), RC_STB / RC_STH / RC_STW / RC_ST64 (ea, v): big-endian
  system        RC_MFSPR(n), RC_MTSPR(n, v), RC_MTMSR(v), RC_DCBZ(ea), RC_ICBI(ea)
  FP results    RC_FSET_S(d, v) (single precision: the console decides what it does to the rest of the register),
                RC_FSET_D(d, v)
  control       RC_MACHINE (the machine type), RC_PROLOGUE, RC_BLOCK(pc, n) (event check + count), RC_EXIT(pc), RC_INTERP(pc) (the interpreter
                runs the instruction at pc), RC_CHECK(next) (leave when an event became due), RC_FPCHECK(pc)
                (MSR[FP] clear: let the interpreter raise FP unavailable), RC_IDLE(pc), RC_NOT_COVERED(),
                RC_CR0(v); helpers of ppc_ops.h (ppc_rotl, ppc_cmps, ppc_carry, ppc_fcmp...).

Shape: the function is cut into blocks at its leaders (entry, branch targets, the instruction after any branch or
call, instructions handed to the interpreter and the one after, the instruction after a system-register write); an
entry switch on PC jumps to the block, so every leader is a resume point the core can enter at (after a call
returns, after an event check). Branches inside the function are gotos; calls, returns, indirect jumps and branches
out of it set PC and return to the dispatcher. Rare or privileged instructions (rfi, sc, tw, segment registers,
string ops, FPSCR moves, OE / record forms of FP) are handed to the interpreter.

A console layer subclasses FnLifter and adds `op_<name>` methods for the mnemonics its decoder adds (`.` in a
mnemonic becomes `_rec`). It can also change the vocabulary's shape through a few hooks: `block_head` (the
RC_BLOCK line of a block, e.g. with a cycle estimate), `mem` (memory macros, e.g. with the instruction's address
for a precise data-storage exception), `guard` (the checks before FP instructions) and `FSET_LOAD` (the macro lfs
results go through). While an instruction is lifted, `self.cycles_after` / `self.cycles_from` hold the block's
cycles (`cycles(i)`, 1 by default) after it / from it on, for layers that keep a cycle-exact clock.
"""

from .isa import BCCTR, BCLR, BCOND, BRANCH, INVALID


def hx(v):
    return "0x%08Xu" % (v & 0xFFFFFFFF)


def mask(mb, me):
    a = 0xFFFFFFFF >> mb
    b = 0xFFFFFFFF if me == 31 else ~(0xFFFFFFFF >> (me + 1)) & 0xFFFFFFFF
    return (a & b) if mb <= me else (a | b)


def R(n):
    return "R(%d)" % n


def RA0(n):
    """(rA|0)."""
    return "0u" if n == 0 else R(n)


INTERP = {
    "rfi", "sc", "tw", "twi", "mtsr", "mtsrin", "mfsr", "mfsrin", "lswi", "stswi", "lswx", "stswx", "eciwx",
    "ecowx", "mcrfs", "mtfsf", "mtfsb0", "mtfsb1", "mtfsfi", "mffs", "mcrxr", "tlbie", "tlbia", "tlbsync",
}
NOPS = {"sync", "isync", "eieio", "dcbst", "dcbf", "dcbt", "dcbtst", "dcbi"}
FP_NAMES = {
    "lfs", "lfsu", "lfd", "lfdu", "stfs", "stfsu", "stfd", "stfdu", "lfsx", "lfsux", "lfdx", "lfdux", "stfsx",
    "stfsux", "stfdx", "stfdux", "stfiwx",
}

LOADS = {"lwz": ("RC_LWZ", ""), "lbz": ("RC_LBZ", ""), "lhz": ("RC_LHZ", ""),
         "lha": ("RC_LHZ", "(uint32_t)(int32_t)(int16_t)")}
STORES = {"stw": "RC_STW", "stb": "RC_STB", "sth": "RC_STH"}
X_LOADS = {"lwzx": "lwz", "lbzx": "lbz", "lhzx": "lhz", "lhax": "lha",
           "lwzux": "lwz", "lbzux": "lbz", "lhzux": "lhz", "lhaux": "lha"}
X_STORES = {"stwx": "stw", "stbx": "stb", "sthx": "sth", "stwux": "stw", "stbux": "stb", "sthux": "sth"}


class FnLifter:
    FSET_LOAD = "RC_FSET_S"
    # A load, a compare of what it loaded and a conditional branch back to the load (a flag polled until an interrupt
    # or a DMA changes it) is an idle loop too: RC_IDLE when the branch is taken and the flag is in RAM (RC_IN_RAM(ea),
    # the layer's). Layers with one CPU turn it on; with several (the Xbox 360), another thread could store the flag.
    IDLE_POLL = False
    POLL_LOADS = ("lwz", "lbz", "lhz", "lha")

    def __init__(self, image, decode, func, name, comments=True):
        """decode: addr -> Insn (the discovery's cache)."""
        self.image = image
        self.decode = decode
        self.f = func
        self.name = name
        self.comments = comments
        self.fallbacks = 0
        self.cycles_after = self.cycles_from = 0
        self.leaders = self.find_leaders()

    # ---- hooks for console layers ----
    def cycles(self, i):
        """The instruction's cycle estimate (layers with a clock override it)."""
        return 1

    def block_head(self, a, insns):
        """The statement opening the block at a (insns: its instructions)."""
        return "RC_BLOCK(%s, %d);" % (hx(a), len(insns))

    def mem(self, macro, i, *args):
        """A memory macro call for instruction i."""
        return "%s(%s)" % (macro, ", ".join(args))

    def guard(self, i, state):
        """Checks before instruction i (state: a dict the block's earlier guards filled), or ""."""
        if self.is_fp(i) and not state.get("fp"):
            state["fp"] = True
            return "RC_FPCHECK(%s);" % hx(i.addr)
        return ""

    # ---- what needs the interpreter, what ends a block ----
    def is_fp(self, i):
        return i.op in (59, 63) or i.name in FP_NAMES

    def needs_interp(self, i):
        n = i.name
        if n is None or i.kind == INVALID or n in INTERP:
            return True
        if i.op == 31 and i.oe and n in ("subfc", "addc", "subf", "neg", "subfe", "adde", "subfze", "addze",
                                         "subfme", "addme", "mullw", "add", "divwu", "divw"):
            return True  # OE forms: rare, the interpreter sets OV / SO
        if i.op in (59, 63) and i.rec:
            return True  # FP record forms: CR1 from the FPSCR
        return not hasattr(self, "op_" + n.replace(".", "_rec"))

    def checks_after(self, i):
        """Instructions after which an event may be due: the next one is a resume point."""
        n = i.name
        if n == "mtmsr" or n == "icbi":
            return True
        if n == "mtspr" and i.spr not in (1, 8, 9):
            return True
        return False

    def find_leaders(self):
        f = self.f
        lead = {f.entry}
        for a in f.addrs:
            i = self.decode(a)
            nxt = a + 4
            if i.kind in (BRANCH, BCOND, BCLR, BCCTR):
                lead.add(nxt)
                if i.kind in (BRANCH, BCOND) and not i.lk and i.target in f.addrs:
                    lead.add(i.target)
            elif self.needs_interp(i):
                lead.add(a)
                lead.add(nxt)
            elif self.checks_after(i):
                lead.add(nxt)
        return {a for a in lead if a in f.addrs}

    # ---- the function ----
    def function(self):
        f = self.f
        addrs = sorted(f.addrs)
        out = ["RC_FN %s(RC_MACHINE *m_)\n{\n    RC_PROLOGUE;\n    switch (c->pc) {\n" % self.name]
        for a in sorted(self.leaders):
            out.append("    case %s: goto L_%08X;\n" % (hx(a), a))
        out.append("    default: RC_NOT_COVERED();\n    }\n")
        k = 0
        while k < len(addrs):
            a = addrs[k]
            # a run: consecutive addresses until a gap
            run = [a]
            while k + 1 < len(addrs) and addrs[k + 1] == run[-1] + 4:
                k += 1
                run.append(addrs[k])
            k += 1
            if run[0] not in self.leaders:
                self.leaders.add(run[0])  # reached only by fall-through from a gap: cannot happen, kept safe
            state = {}
            rest = []
            for j, a in enumerate(run):
                if a in self.leaders:
                    n = 1
                    while j + n < len(run) and run[j + n] not in self.leaders:
                        n += 1
                    block = [self.decode(x) for x in run[j:j + n]]
                    rest = [self.cycles(x) for x in block]
                    out.append("L_%08X:\n    %s\n" % (a, self.block_head(a, block)))
                    state = {}
                i = self.decode(a)
                own = rest.pop(0)
                self.cycles_after = sum(rest)
                self.cycles_from = own + self.cycles_after
                if self.comments:
                    out.append("    /* %08X: %s */\n" % (a, i.text))
                if self.needs_interp(i):
                    self.fallbacks += 1
                    out.append("    RC_INTERP(%s);\n" % hx(a))
                    continue
                g = self.guard(i, state)
                if g:
                    out.append("    " + g + "\n")
                body = self.insn(i)
                if body:
                    out.append("    " + body + "\n")
            last = self.decode(run[-1])
            if not self.terminates(last):
                out.append("    RC_EXIT(%s);\n" % hx(run[-1] + 4))
        out.append("}\n")
        # labels the switch names but no block defined (cannot happen) would not compile: checked by the C compiler
        return "".join(out)

    def terminates(self, i):
        if self.needs_interp(i):
            return True
        if i.kind == BRANCH:
            return True
        if i.kind in (BCOND, BCLR, BCCTR) and (i.rd & 0x14) == 0x14:
            return True
        return i.name == "icbi"

    # ---- one instruction ----
    def go(self, t):
        return "goto L_%08X;" % t if t in self.leaders else "RC_EXIT(%s);" % hx(t)

    def insn(self, i):
        return getattr(self, "op_" + i.name.replace(".", "_rec"))(i)

    def rec(self, i, reg):
        return " RC_CR0(%s);" % R(reg) if i.rec else ""

    @staticmethod
    def cond(i):
        """(pre-statement, C condition) of a bc / bclr / bcctr."""
        bo, bi = i.rd, i.ra
        pre, terms = "", []
        if not (bo & 4) and i.kind != BCCTR:
            pre = "c->ctr--; "
            terms.append("c->ctr == 0" if bo & 2 else "c->ctr != 0")
        if not (bo & 16):
            bit = "(c->cr & %s)" % hx(1 << (31 - bi))
            terms.append(bit if bo & 8 else "!" + bit)
        return pre, " && ".join(terms)

    # branches
    def op_b(self, i):
        t = i.target
        if i.lk:
            return "c->lr = %s; RC_EXIT(%s);" % (hx(i.addr + 4), hx(t))
        if t == i.addr:
            return "RC_IDLE(%s);" % hx(t)
        return self.go(t) if t in self.f.addrs else "RC_EXIT(%s);" % hx(t)

    def is_poll_loop(self, i):
        """i (a bc) closes `load rD, d(rA); cmp(l)i rD, imm; bc` back to the load, its counter left alone."""
        t = i.target
        if not self.IDLE_POLL or i.lk or not (i.rd & 4) or t != i.addr - 8 or t not in self.leaders:
            return False
        ld, cmp = self.decode(t), self.decode(t + 4)
        if ld is None or cmp is None or (t + 4) in self.leaders:
            return False
        return (ld.name in self.POLL_LOADS and ld.ra != ld.rd and ld.ra != 0
                and cmp.name in ("cmpi", "cmpli") and cmp.ra == ld.rd)

    def op_bc(self, i):
        pre, cond = self.cond(i)
        t = i.target
        lr = "c->lr = %s; " % hx(i.addr + 4) if i.lk else ""
        if i.lk:
            then = "RC_EXIT(%s);" % hx(t)
        elif self.is_poll_loop(i):
            # only a flag in RAM: a hardware register polled (a beam position, a timer) changes with time alone
            then = "if (RC_IN_RAM(%s)) RC_IDLE(%s); %s" % (self._ea_d(self.decode(t)), hx(t), self.go(t))
        else:
            then = self.go(t) if t in self.f.addrs else "RC_EXIT(%s);" % hx(t)
        if not cond:
            return pre + lr + then
        return "%s%sif (%s) { %s }" % (pre, lr, cond, then)

    def op_bclr(self, i):
        pre, cond = self.cond(i)
        lr = " c->lr = %s;" % hx(i.addr + 4) if i.lk else ""
        if not cond:
            return "{ uint32_t t_ = c->lr & ~3u; %s%s RC_EXIT(t_); }" % (pre, lr)
        return "{ uint32_t t_ = c->lr & ~3u; %s%s if (%s) RC_EXIT(t_); }" % (pre, lr, cond)

    def op_bcctr(self, i):
        pre, cond = self.cond(i)
        lr = " c->lr = %s;" % hx(i.addr + 4) if i.lk else ""
        if not cond:
            return "{ uint32_t t_ = c->ctr & ~3u;%s RC_EXIT(t_); }" % lr
        return "{ uint32_t t_ = c->ctr & ~3u;%s if (%s) RC_EXIT(t_); }" % (lr, cond)

    # CR logic
    def op_mcrf(self, i):
        return "c->cr = ppc_cr_set(c->cr, %d, c->cr >> %d);" % (i.crfd, 28 - 4 * i.crfs)

    def _crop(self, i, expr):
        return "{ uint32_t x_ = ppc_cr_bit(c->cr, %d), y_ = ppc_cr_bit(c->cr, %d); " \
               "c->cr = ppc_cr_setbit(c->cr, %d, %s); }" % (i.ra, i.rb, i.rd, expr)

    def op_crnor(self, i): return self._crop(i, "!(x_ | y_)")
    def op_crandc(self, i): return self._crop(i, "x_ & !y_")
    def op_crxor(self, i): return self._crop(i, "x_ ^ y_")
    def op_crnand(self, i): return self._crop(i, "!(x_ & y_)")
    def op_crand(self, i): return self._crop(i, "x_ & y_")
    def op_creqv(self, i): return self._crop(i, "!(x_ ^ y_)")
    def op_crorc(self, i): return self._crop(i, "x_ | !y_")
    def op_cror(self, i): return self._crop(i, "x_ | y_")

    # immediate arithmetic and logic
    def op_addi(self, i):
        return "%s = %s + %s;" % (R(i.rd), RA0(i.ra), hx(i.simm))

    def op_addis(self, i):
        return "%s = %s + %s;" % (R(i.rd), RA0(i.ra), hx(i.uimm << 16))

    def op_addic(self, i):
        return "{ uint32_t a_ = %s; %s = a_ + %s; c->xer_ca = ppc_carry(a_, %s, 0); }%s" % (
            R(i.ra), R(i.rd), hx(i.simm), hx(i.simm), "")

    def op_addic_rec(self, i):
        return self.op_addic(i) + " RC_CR0(%s);" % R(i.rd)

    def op_subfic(self, i):
        return "{ uint32_t a_ = %s; %s = ~a_ + %s + 1u; c->xer_ca = ppc_carry(~a_, %s, 1); }" % (
            R(i.ra), R(i.rd), hx(i.simm), hx(i.simm))

    def op_mulli(self, i):
        return "%s = (uint32_t)((int32_t)%s * %d);" % (R(i.rd), R(i.ra), i.simm)

    def op_cmpi(self, i):
        return "c->cr = ppc_cr_set(c->cr, %d, ppc_cmps((int32_t)%s, %d, c->xer_so));" % (i.crfd, R(i.ra), i.simm)

    def op_cmpli(self, i):
        return "c->cr = ppc_cr_set(c->cr, %d, ppc_cmpu(%s, %s, c->xer_so));" % (i.crfd, R(i.ra), hx(i.uimm))

    def op_cmp(self, i):
        return "c->cr = ppc_cr_set(c->cr, %d, ppc_cmps((int32_t)%s, (int32_t)%s, c->xer_so));" % (
            i.crfd, R(i.ra), R(i.rb))

    def op_cmpl(self, i):
        return "c->cr = ppc_cr_set(c->cr, %d, ppc_cmpu(%s, %s, c->xer_so));" % (i.crfd, R(i.ra), R(i.rb))

    def op_ori(self, i): return "%s = %s | %s;" % (R(i.ra), R(i.rd), hx(i.uimm))
    def op_oris(self, i): return "%s = %s | %s;" % (R(i.ra), R(i.rd), hx(i.uimm << 16))
    def op_xori(self, i): return "%s = %s ^ %s;" % (R(i.ra), R(i.rd), hx(i.uimm))
    def op_xoris(self, i): return "%s = %s ^ %s;" % (R(i.ra), R(i.rd), hx(i.uimm << 16))

    def op_andi_rec(self, i):
        return "%s = %s & %s; RC_CR0(%s);" % (R(i.ra), R(i.rd), hx(i.uimm), R(i.ra))

    def op_andis_rec(self, i):
        return "%s = %s & %s; RC_CR0(%s);" % (R(i.ra), R(i.rd), hx(i.uimm << 16), R(i.ra))

    def op_rlwinm(self, i):
        return "%s = ppc_rotl(%s, %d) & %s;%s" % (R(i.ra), R(i.rd), i.rb, hx(mask(i.mb, i.me)), self.rec(i, i.ra))

    def op_rlwnm(self, i):
        return "%s = ppc_rotl(%s, %s & 31) & %s;%s" % (R(i.ra), R(i.rd), R(i.rb), hx(mask(i.mb, i.me)),
                                                     self.rec(i, i.ra))

    def op_rlwimi(self, i):
        m = mask(i.mb, i.me)
        return "%s = (ppc_rotl(%s, %d) & %s) | (%s & %s);%s" % (R(i.ra), R(i.rd), i.rb, hx(m), R(i.ra), hx(~m),
                                                               self.rec(i, i.ra))

    # register arithmetic (opcode 31 XO-form, OE = 0)
    def _xo(self, i, body):
        return "{ uint32_t a_ = %s, b_ = %s; (void)b_; %s }%s" % (R(i.ra), R(i.rb), body, self.rec(i, i.rd))

    def op_add(self, i): return self._xo(i, "%s = a_ + b_;" % R(i.rd))
    def op_subf(self, i): return self._xo(i, "%s = b_ - a_;" % R(i.rd))
    def op_addc(self, i): return self._xo(i, "%s = a_ + b_; c->xer_ca = ppc_carry(a_, b_, 0);" % R(i.rd))
    def op_subfc(self, i): return self._xo(i, "%s = ~a_ + b_ + 1u; c->xer_ca = ppc_carry(~a_, b_, 1);" % R(i.rd))

    def op_adde(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = a_ + b_ + ci_; c->xer_ca = ppc_carry(a_, b_, ci_);"
                        % R(i.rd))

    def op_subfe(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = ~a_ + b_ + ci_; c->xer_ca = ppc_carry(~a_, b_, ci_);"
                        % R(i.rd))

    def op_addze(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = a_ + ci_; c->xer_ca = ppc_carry(a_, 0, ci_);" % R(i.rd))

    def op_subfze(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = ~a_ + ci_; c->xer_ca = ppc_carry(~a_, 0, ci_);"
                        % R(i.rd))

    def op_addme(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = a_ - 1u + ci_; c->xer_ca = ppc_carry(a_, 0xFFFFFFFFu, ci_);"
                        % R(i.rd))

    def op_subfme(self, i):
        return self._xo(i, "uint32_t ci_ = c->xer_ca; %s = ~a_ - 1u + ci_; "
                           "c->xer_ca = ppc_carry(~a_, 0xFFFFFFFFu, ci_);" % R(i.rd))

    def op_neg(self, i): return self._xo(i, "%s = 0u - a_;" % R(i.rd))
    def op_mullw(self, i): return self._xo(i, "%s = (uint32_t)((int32_t)a_ * (int64_t)(int32_t)b_);" % R(i.rd))
    def op_mulhw(self, i): return self._xo(i, "%s = ppc_mulhw(a_, b_);" % R(i.rd))
    def op_mulhwu(self, i): return self._xo(i, "%s = ppc_mulhwu(a_, b_);" % R(i.rd))
    def op_divw(self, i): return self._xo(i, "%s = ppc_divw(a_, b_);" % R(i.rd))
    def op_divwu(self, i): return self._xo(i, "%s = ppc_divwu(a_, b_);" % R(i.rd))

    # logic and shifts (opcode 31 X-form: rA = f(rS, rB))
    def _x(self, i, expr):
        return "%s = %s;%s" % (R(i.ra), expr % {"s": R(i.rd), "b": R(i.rb)}, self.rec(i, i.ra))

    def op_and(self, i): return self._x(i, "%(s)s & %(b)s")
    def op_andc(self, i): return self._x(i, "%(s)s & ~%(b)s")
    def op_or(self, i): return self._x(i, "%(s)s | %(b)s")
    def op_orc(self, i): return self._x(i, "%(s)s | ~%(b)s")
    def op_xor(self, i): return self._x(i, "%(s)s ^ %(b)s")
    def op_nor(self, i): return self._x(i, "~(%(s)s | %(b)s)")
    def op_nand(self, i): return self._x(i, "~(%(s)s & %(b)s)")
    def op_eqv(self, i): return self._x(i, "~(%(s)s ^ %(b)s)")
    def op_slw(self, i): return self._x(i, "ppc_slw(%(s)s, %(b)s)")
    def op_srw(self, i): return self._x(i, "ppc_srw(%(s)s, %(b)s)")
    def op_sraw(self, i): return self._x(i, "ppc_sraw(%(s)s, %(b)s, &c->xer_ca)")
    def op_srawi(self, i): return self._x(i, "ppc_srawi(%%(s)s, %d, &c->xer_ca)" % i.rb)
    def op_cntlzw(self, i): return self._x(i, "ppc_cntlzw(%(s)s)")
    def op_extsh(self, i): return self._x(i, "(uint32_t)(int32_t)(int16_t)%(s)s")
    def op_extsb(self, i): return self._x(i, "(uint32_t)(int32_t)(int8_t)%(s)s")

    # loads and stores
    def _ea_d(self, i):
        return "%s + %s" % (RA0(i.ra), hx(i.simm)) if i.simm else RA0(i.ra)

    def _load(self, i, kind, ea, update):
        fn, conv = LOADS[kind]
        if update:
            return "{ uint32_t ea_ = %s; %s = %s%s; %s = ea_; }" % (ea, R(i.rd), conv, self.mem(fn, i, "ea_"), R(i.ra))
        return "%s = %s%s;" % (R(i.rd), conv, self.mem(fn, i, ea))

    def _store(self, i, kind, ea, update):
        fn = STORES[kind]
        if update:
            return "{ uint32_t ea_ = %s; %s; %s = ea_; }" % (ea, self.mem(fn, i, "ea_", R(i.rd)), R(i.ra))
        return "%s;" % self.mem(fn, i, ea, R(i.rd))

    def op_lwz(self, i): return self._load(i, "lwz", self._ea_d(i), False)
    def op_lbz(self, i): return self._load(i, "lbz", self._ea_d(i), False)
    def op_lhz(self, i): return self._load(i, "lhz", self._ea_d(i), False)
    def op_lha(self, i): return self._load(i, "lha", self._ea_d(i), False)
    def op_lwzu(self, i): return self._load(i, "lwz", "%s + %s" % (R(i.ra), hx(i.simm)), True)
    def op_lbzu(self, i): return self._load(i, "lbz", "%s + %s" % (R(i.ra), hx(i.simm)), True)
    def op_lhzu(self, i): return self._load(i, "lhz", "%s + %s" % (R(i.ra), hx(i.simm)), True)
    def op_lhau(self, i): return self._load(i, "lha", "%s + %s" % (R(i.ra), hx(i.simm)), True)
    def op_stw(self, i): return self._store(i, "stw", self._ea_d(i), False)
    def op_stb(self, i): return self._store(i, "stb", self._ea_d(i), False)
    def op_sth(self, i): return self._store(i, "sth", self._ea_d(i), False)
    def op_stwu(self, i): return self._store(i, "stw", "%s + %s" % (R(i.ra), hx(i.simm)), True)
    def op_stbu(self, i): return self._store(i, "stb", "%s + %s" % (R(i.ra), hx(i.simm)), True)
    def op_sthu(self, i): return self._store(i, "sth", "%s + %s" % (R(i.ra), hx(i.simm)), True)

    def _xload(self, i):
        upd = i.name.endswith("ux")
        ea = "%s + %s" % (R(i.ra) if upd else RA0(i.ra), R(i.rb))
        return self._load(i, X_LOADS[i.name], ea, upd)

    def _xstore(self, i):
        upd = i.name.endswith("ux")
        ea = "%s + %s" % (R(i.ra) if upd else RA0(i.ra), R(i.rb))
        return self._store(i, X_STORES[i.name], ea, upd)

    op_lwzx = op_lbzx = op_lhzx = op_lhax = op_lwzux = op_lbzux = op_lhzux = op_lhaux = _xload
    op_stwx = op_stbx = op_sthx = op_stwux = op_stbux = op_sthux = _xstore

    def _eax(self, i):
        return "%s + %s" % (RA0(i.ra), R(i.rb))

    def op_lwbrx(self, i):
        return "%s = __builtin_bswap32(%s);" % (R(i.rd), self.mem("RC_LWZ", i, self._eax(i)))

    def op_lhbrx(self, i):
        return "%s = __builtin_bswap16((uint16_t)%s);" % (R(i.rd), self.mem("RC_LHZ", i, self._eax(i)))

    def op_stwbrx(self, i):
        return "%s;" % self.mem("RC_STW", i, self._eax(i), "__builtin_bswap32(%s)" % R(i.rd))

    def op_sthbrx(self, i):
        return "%s;" % self.mem("RC_STH", i, self._eax(i), "__builtin_bswap16((uint16_t)%s)" % R(i.rd))

    def op_lwarx(self, i):
        return "%s = %s; c->reserve = 1;" % (R(i.rd), self.mem("RC_LWZ", i, self._eax(i)))

    def op_stwcx_rec(self, i):
        return ("if (c->reserve) { %s; c->reserve = 0; "
                "c->cr = ppc_cr_set(c->cr, 0, PPC_CR_EQ | c->xer_so); } "
                "else c->cr = ppc_cr_set(c->cr, 0, c->xer_so);" % self.mem("RC_STW", i, self._eax(i), R(i.rd)))

    def op_lmw(self, i):
        parts = ["uint32_t ea_ = %s;" % self._ea_d(i)]
        for k, r in enumerate(range(i.rd, 32)):
            parts.append("%s = %s;" % (R(r), self.mem("RC_LWZ", i, "ea_ + %du" % (4 * k))))
        return "{ " + " ".join(parts) + " }"

    def op_stmw(self, i):
        parts = ["uint32_t ea_ = %s;" % self._ea_d(i)]
        for k, r in enumerate(range(i.rd, 32)):
            parts.append("%s;" % self.mem("RC_STW", i, "ea_ + %du" % (4 * k), R(r)))
        return "{ " + " ".join(parts) + " }"

    # system registers
    def op_mfcr(self, i): return "%s = c->cr;" % R(i.rd)

    def op_mtcrf(self, i):
        crm = (i.word >> 12) & 0xFF
        m = 0
        for k in range(8):
            if crm & (0x80 >> k):
                m |= 0xF0000000 >> (4 * k)
        return "c->cr = (c->cr & %s) | (%s & %s);" % (hx(~m), R(i.rd), hx(m))

    def op_mfmsr(self, i): return "%s = c->msr;" % R(i.rd)
    def op_mtmsr(self, i): return "RC_MTMSR(%s); RC_CHECK(%s);" % (R(i.rd), hx(i.addr + 4))

    def op_mfspr(self, i):
        s = i.spr
        if s == 8:
            return "%s = c->lr;" % R(i.rd)
        if s == 9:
            return "%s = c->ctr;" % R(i.rd)
        if s == 1:
            return "%s = RC_XER();" % R(i.rd)
        return "%s = RC_MFSPR(%d);" % (R(i.rd), s)

    op_mftb = op_mfspr

    def op_mtspr(self, i):
        s = i.spr
        if s == 8:
            return "c->lr = %s;" % R(i.rd)
        if s == 9:
            return "c->ctr = %s;" % R(i.rd)
        if s == 1:
            return "RC_SET_XER(%s);" % R(i.rd)
        return "RC_MTSPR(%d, %s); RC_CHECK(%s);" % (s, R(i.rd), hx(i.addr + 4))

    # cache and ordering
    def op_dcbz(self, i): return "%s;" % self.mem("RC_DCBZ", i, self._eax(i))
    def op_icbi(self, i): return "%s; RC_EXIT(%s);" % (self.mem("RC_ICBI", i, self._eax(i)), hx(i.addr + 4))

    def _nop(self, i):
        return ""

    op_sync = op_isync = op_eieio = op_dcbst = op_dcbf = op_dcbt = op_dcbtst = op_dcbi = _nop

    # FP loads and stores
    def _fea(self, i):
        n = i.name
        upd = n.endswith("u") or n.endswith("ux")
        if i.op == 31:
            ea = "%s + %s" % (R(i.ra) if upd else RA0(i.ra), R(i.rb))
        else:
            ea = "%s + %s" % (R(i.ra), hx(i.simm)) if upd else self._ea_d(i)
        return ea, upd

    def _fmem(self, i):
        ea, upd = self._fea(i)
        base = i.name.rstrip("ux") if i.op == 31 else i.name.rstrip("u")
        d = i.rd
        body = {
            "lfs": "%s(%d, ppc_lfs_bits(%s));" % (self.FSET_LOAD, d, self.mem("RC_LWZ", i, "ea_")),
            "lfd": "F0(%d) = ppc_u2f(%s);" % (d, self.mem("RC_LD64", i, "ea_")),
            "stfs": "%s;" % self.mem("RC_STW", i, "ea_", "ppc_stfs_bits(F0(%d))" % d),
            "stfd": "%s;" % self.mem("RC_ST64", i, "ea_", "ppc_f2u(F0(%d))" % d),
            "stfiw": "%s;" % self.mem("RC_STW", i, "ea_", "(uint32_t)ppc_f2u(F0(%d))" % d),
        }[base]
        return "{ uint32_t ea_ = %s; %s%s }" % (ea, body, " %s = ea_;" % R(i.ra) if upd else "")

    op_lfs = op_lfsu = op_lfd = op_lfdu = op_stfs = op_stfsu = op_stfd = op_stfdu = _fmem
    op_lfsx = op_lfsux = op_lfdx = op_lfdux = op_stfsx = op_stfsux = op_stfdx = op_stfdux = op_stfiwx = _fmem

    # FP arithmetic (record forms go to the interpreter)
    def _fa(self, i, expr, single):
        e = expr % {"a": "F0(%d)" % i.ra, "b": "F0(%d)" % i.rb, "c": "F0(%d)" % i.rc}
        return ("RC_FSET_S(%d, %s);" if single else "RC_FSET_D(%d, %s);") % (i.rd, e)

    def op_fadd(self, i): return self._fa(i, "%(a)s + %(b)s", False)
    def op_fsub(self, i): return self._fa(i, "%(a)s - %(b)s", False)
    def op_fmul(self, i): return self._fa(i, "%(a)s * %(c)s", False)
    def op_fdiv(self, i): return self._fa(i, "%(a)s / %(b)s", False)
    def op_fsqrt(self, i): return self._fa(i, "sqrt(%(b)s)", False)
    def op_fmadd(self, i): return self._fa(i, "ppc_fmadd(%(a)s, %(c)s, %(b)s)", False)
    def op_fmsub(self, i): return self._fa(i, "ppc_fmsub(%(a)s, %(c)s, %(b)s)", False)
    def op_fnmadd(self, i): return self._fa(i, "ppc_fnmadd(%(a)s, %(c)s, %(b)s)", False)
    def op_fnmsub(self, i): return self._fa(i, "ppc_fnmsub(%(a)s, %(c)s, %(b)s)", False)
    def op_fadds(self, i): return self._fa(i, "%(a)s + %(b)s", True)
    def op_fsubs(self, i): return self._fa(i, "%(a)s - %(b)s", True)
    def op_fmuls(self, i): return self._fa(i, "%(a)s * %(c)s", True)
    def op_fdivs(self, i): return self._fa(i, "%(a)s / %(b)s", True)
    def op_fsqrts(self, i): return self._fa(i, "sqrt(%(b)s)", True)
    def op_fmadds(self, i): return self._fa(i, "ppc_fmadd(%(a)s, %(c)s, %(b)s)", True)
    def op_fmsubs(self, i): return self._fa(i, "ppc_fmsub(%(a)s, %(c)s, %(b)s)", True)
    def op_fnmadds(self, i): return self._fa(i, "ppc_fnmadd(%(a)s, %(c)s, %(b)s)", True)
    def op_fnmsubs(self, i): return self._fa(i, "ppc_fnmsub(%(a)s, %(c)s, %(b)s)", True)
    def op_fres(self, i): return self._fa(i, "ppc_fres(%(b)s)", True)
    def op_frsqrte(self, i): return "F0(%d) = ppc_frsqrte(F0(%d));" % (i.rd, i.rb)
    def op_fsel(self, i): return "F0(%d) = ppc_fsel(F0(%d), F0(%d), F0(%d));" % (i.rd, i.ra, i.rc, i.rb)
    def op_frsp(self, i): return "RC_FSET_S(%d, F0(%d));" % (i.rd, i.rb)
    def op_fctiw(self, i): return "F0(%d) = ppc_fctiw(F0(%d), 0);" % (i.rd, i.rb)
    def op_fctiwz(self, i): return "F0(%d) = ppc_fctiw(F0(%d), 1);" % (i.rd, i.rb)
    def op_fneg(self, i): return "F0(%d) = ppc_fneg(F0(%d));" % (i.rd, i.rb)
    def op_fabs(self, i): return "F0(%d) = ppc_fabs(F0(%d));" % (i.rd, i.rb)
    def op_fnabs(self, i): return "F0(%d) = ppc_fnabs(F0(%d));" % (i.rd, i.rb)
    def op_fmr(self, i): return "F0(%d) = F0(%d);" % (i.rd, i.rb)

    def op_fcmpu(self, i):
        return ("{ uint32_t f_ = ppc_fcmp(F0(%d), F0(%d)); c->cr = ppc_cr_set(c->cr, %d, f_); "
                "c->fpscr = (c->fpscr & ~0xF000u) | (f_ << 12); }" % (i.ra, i.rb, i.crfd))

    op_fcmpo = op_fcmpu
