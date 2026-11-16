"""SH-4 to C lifter (XBA-227, Dreamcast spike).

Discovery: function starts are the entry point, the symbols (ELF), static call targets (BSR, and JSR / JMP / BSRF
through a register loaded from a PC-relative literal a few instructions before), the exception and interrupt
vectors of a constant LDC Rn,VBR (VBR + 0x100 / 0x400 / 0x600), addresses the interpreter reported
(--discovered), and for a raw binary without symbols the words of the image that point into its code. Each
function is everything reachable from its start by branches and fall-through, a call's return site included,
up to another function's start.

Emission: one C function per function start; a switch on c->pc enters it at any of its table entries (its start,
the return sites of its calls, the resume points after SR writes / TRAPA / SLEEP, jump-table targets and loop
heads, all of which also check whether an event is due). Each instruction is a call to its sh4_ops.h semantics
with constant operands; branches become gotos; delay slots run after the branch's condition / target is taken and
before the branch. Calls nest: the callee's C function runs on the host stack and, when it comes back with PC at the
return site, execution goes on (dc_recomp.h); anything else (returns, register jumps the table does not hold,
exceptions, events) goes back to the dispatcher with PC set. Jumps through registers keep a runtime check against
the target the static pass guessed, so a wrong guess is only slower, never wrong.
"""

from __future__ import annotations

import bisect
from dataclasses import dataclass, field

from . import sh4
from .image import Image

# Instructions that write no general register (stores, compares, FPU arithmetic, system register loads...).
_NO_GPR_WRITE = {
    "MOVB_S", "MOVW_S", "MOVL_S", "MOVB_S0", "MOVW_S0", "MOVL_S0", "MOVL_SD", "MOVB_S4", "MOVW_S4", "MOVB_SG",
    "MOVW_SG", "MOVL_SG", "MOVCA_L", "FMOV_S", "FMOV_S0", "TST", "TSTI", "TSTB", "ANDB", "ORB", "XORB", "TAS_B",
    "CMP_EQ", "CMP_HS", "CMP_GE", "CMP_HI", "CMP_GT", "CMP_PZ", "CMP_PL", "CMP_STR", "CMP_EQI", "PREF", "OCBI",
    "OCBP", "OCBWB", "NOP", "CLRT", "SETT", "CLRS", "SETS", "CLRMAC", "DIV0U", "DIV0S", "MUL_L", "MULU_W",
    "MULS_W", "DMULU_L", "DMULS_L", "LDS_MACH", "LDS_MACL", "LDS_PR", "LDS_FPUL", "LDS_FPSCR", "LDC_GBR",
    "LDC_VBR", "LDC_SSR", "LDC_SPC", "LDC_DBR", "LDC_BANK", "LDC_SR", "LDTLB", "FMOV_L", "FMOV_L0", "FMOV",
    "FADD", "FSUB", "FMUL", "FDIV", "FCMP_EQ", "FCMP_GT", "FMAC", "FSTS", "FLDS", "FLOAT", "FTRC", "FNEG", "FABS",
    "FSQRT", "FSRRA", "FLDI0", "FLDI1", "FCNVSD", "FCNVDS", "FIPR", "FTRV", "FSCA", "FSCHG", "FRCHG",
}
_WRITES_R0 = {"MOVB_L4", "MOVW_L4", "MOVB_LG", "MOVW_LG", "MOVL_LG", "MOVA", "ANDI", "XORI", "ORI"}
_WRITES_M = {"FMOV_LP"}
_WRITES_NM = {"MOVB_LP", "MOVW_LP", "MOVL_LP", "MAC_L", "MAC_W"}


def gpr_writes(name: str, op: int) -> set[int]:
    """General registers an instruction may write (conservative)."""
    n, m = (op >> 8) & 15, (op >> 4) & 15
    if name in _NO_GPR_WRITE:
        return set()
    if name in _WRITES_R0:
        return {0}
    if name in _WRITES_M:
        return {m}
    if name in _WRITES_NM:
        return {n, m}
    return {n}


@dataclass
class Func:
    entry: int
    insns: dict[int, int] = field(default_factory=dict)  # standalone instruction address -> opcode
    table: set[int] = field(default_factory=set)  # addresses the table maps to this function (checked labels)
    labels: set[int] = field(default_factory=set)
    calls: set[int] = field(default_factory=set)  # static call / tail-jump targets found
    guesses: dict[int, int] = field(default_factory=dict)  # register branch address -> guessed target
    jtables: dict[int, list[int]] = field(default_factory=dict)  # BRAF address -> case targets
    fallbacks: set[int] = field(default_factory=set)  # addresses handed to the interpreter


class Lifter:
    def __init__(self, img: Image, discovered: set[int] | None = None):
        self.img = img
        self.discovered = {a for a in (discovered or set()) if img.is_code(a) and not a & 1}
        self.primaries: set[int] = set()
        self.funcs: dict[int, Func] = {}

    # ---- decoding helpers ----

    def op_at(self, a: int) -> int | None:
        if not self.img.is_code(a) or a & 1:
            return None
        return self.img.read16(a)

    def _straight_back(self, f: Func, a: int, limit: int = 24) -> list[tuple[int, int]]:
        """Instructions before a in the same straight-line run (no label in between), nearest first."""
        out = []
        b = a
        while len(out) < limit:
            if b in f.labels:
                break  # control may also arrive here from elsewhere
            p = b - 2
            if p not in f.insns:
                break
            d = sh4.decode(f.insns[p])
            if d is None or d.flags & sh4.BRANCH:
                break
            out.append((p, f.insns[p]))
            if p in f.labels:
                break
            b = p
        return out

    def _const_reg(self, f: Func, a: int, reg: int) -> int | None:
        """The value reg holds at a when a PC-relative literal / MOV #imm in the same run set it."""
        for p, op in self._straight_back(f, a):
            d = sh4.decode(op)
            if d is None:
                return None
            n = (op >> 8) & 15
            if d.name == "MOVL_PC" and n == reg:
                lit = sh4.pc_literal(p, op, d.name)
                return self.img.read32(lit[0]) if lit else None
            if d.name == "MOVW_PC" and n == reg:
                lit = sh4.pc_literal(p, op, d.name)
                v = self.img.read16(lit[0]) if lit else None
                return None if v is None else (v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF
            if d.name == "MOVI" and n == reg:
                return sh4.s8(op) & 0xFFFFFFFF
            if reg in gpr_writes(d.name, op):
                return None
        return None

    def _jump_table(self, f: Func, a: int, reg: int) -> list[int]:
        """Case targets of a GCC switch: mova T,r0; mov.b|w @(r0,Ri),Rn; [extu]; [add Rn,Rn | shll Rn]; braf Rn,
        bounded by a "mov #K,Rk; cmp/hi Rk,Ri" before it when there is one."""
        back = self._straight_back(f, a, 24)
        scale, size, signed, idx, k = 1, 0, True, None, 0
        for k, (p, op) in enumerate(back):
            d = sh4.decode(op)
            n, m = (op >> 8) & 15, (op >> 4) & 15
            if d.name in ("EXTU_B", "EXTU_W") and n == reg and m == reg:
                signed = False
            elif (d.name == "ADD" and n == reg and m == reg) or (d.name == "SHLL" and n == reg):
                scale *= 2
            elif d.name in ("MOVB_L0", "MOVW_L0") and n == reg:
                size, idx = (1 if d.name == "MOVB_L0" else 2), m
                break
            else:
                return []
        if not size:
            return []
        table, count = None, None
        for p, op in back[k + 1 :]:
            d = sh4.decode(op)
            if d.name == "MOVA":
                table = sh4.pc_literal(p, op, d.name)[0]
                break
            if 0 in gpr_writes(d.name, op):
                return []
        if table is None:
            return []
        # The bound check sits just before, usually behind the "bt default" that ends the previous run.
        p = back[k][0]
        for _ in range(10):
            p -= 2
            op = f.insns.get(p)
            d = sh4.decode(op) if op is not None else None
            if d is None or (d.flags & sh4.BRANCH and d.name not in ("BT", "BF")):
                break
            if d.name == "CMP_HI" and (op >> 8) & 15 == idx:
                kv = self._const_reg(f, p, (op >> 4) & 15)
                if kv is not None and kv < 1024:
                    count = kv + 1
                break
            if idx in gpr_writes(d.name, op):
                break
        targets: list[int] = []
        for i in range(count if count is not None else 256):
            ea = table + i * size
            if count is None and targets and ea >= min(targets):
                break
            v = self.img.read8(ea) if size == 1 else self.img.read16(ea)
            if v is None:
                break
            if signed and v & (1 << (8 * size - 1)):
                v -= 1 << (8 * size)
            t = (a + 4 + v * scale) & 0xFFFFFFFF
            if t & 1 or not self.img.is_code(t):
                if count is None:
                    break
                continue
            targets.append(t)
        return sorted(set(targets))

    # ---- discovery ----

    def explore(self, entry: int, seeds: set[int]) -> Func:
        f = Func(entry)
        f.table.add(entry)
        f.labels.add(entry)
        work = [entry] + sorted(seeds)
        for s in seeds:
            f.table.add(s)
            f.labels.add(s)
        pending_reg: list[tuple[int, int, str]] = []  # (addr, op, name) of register branches to resolve
        while work or pending_reg:
            if not work:
                self._resolve(f, pending_reg, work)
                continue
            a = work.pop()
            if a in f.insns or a in f.fallbacks:
                continue
            if a != entry and a in self.primaries and a not in seeds:
                continue  # another function's start: left to it
            op = self.op_at(a)
            d = sh4.decode(op) if op is not None else None
            if d is None:
                f.fallbacks.add(a)
                continue
            if d.flags & sh4.DELAY:
                sop = self.op_at(a + 2)
                sd = sh4.decode(sop) if sop is not None else None
                if sd is None or sd.flags & (sh4.BRANCH | sh4.SYNC):
                    f.fallbacks.add(a)
                    continue
            f.insns[a] = op
            name = d.name
            nxt = a + 4 if d.flags & sh4.DELAY else a + 2
            if not d.flags & sh4.BRANCH:
                if name == "LDC_VBR":
                    pending_reg.append((a, op, name))  # exception vectors start functions
                if d.flags & sh4.SYNC:
                    f.table.add(nxt)
                    f.labels.add(nxt)
                work.append(nxt)
                continue
            t = sh4.branch_target(a, op, name)
            if name in ("BT", "BF", "BT_S", "BF_S"):
                f.labels.add(t)
                work += [t, nxt]
                if t <= a:
                    f.table.add(t)
            elif name == "BRA":
                if t in self.primaries and t != entry:
                    f.calls.add(t)
                else:
                    f.labels.add(t)
                    work.append(t)
                    if t <= a:
                        f.table.add(t)
            elif name in ("BSR", "BSRF", "JSR"):
                f.table.add(nxt)
                f.labels.add(nxt)
                work.append(nxt)
                if name == "BSR":
                    f.calls.add(t)
                else:
                    pending_reg.append((a, op, name))
            elif name in ("JMP", "BRAF"):
                pending_reg.append((a, op, name))
            # RTS / RTE: nothing follows
        return f

    def _resolve(self, f: Func, pending: list[tuple[int, int, str]], work: list[int]) -> None:
        """Register branches, once the straight-line runs before them are known; jump tables add work."""
        while pending:
            ra, rop, rname = pending.pop()
            n = (rop >> 8) & 15
            v = self._const_reg(f, ra, n)
            if rname == "LDC_VBR":
                if v is not None:
                    for off in (0x100, 0x400, 0x600):  # general exception, TLB miss, interrupt
                        if self.img.is_code((v + off) & 0xFFFFFFFF):
                            f.calls.add((v + off) & 0xFFFFFFFF)
                continue
            if rname in ("BSRF", "BRAF") and v is not None:
                v = (ra + 4 + v) & 0xFFFFFFFF
            if v is not None and self.img.is_code(v) and not v & 1:
                f.guesses[ra] = v
                if rname in ("JSR", "BSRF") or v in self.primaries:
                    f.calls.add(v)
                else:
                    f.labels.add(v)
                    f.table.add(v)
                    work.append(v)
            elif rname == "BRAF":
                cases = self._jump_table(f, ra, n)
                if cases:
                    f.jtables[ra] = cases
                    for cse in cases:
                        f.labels.add(cse)
                        f.table.add(cse)
                        work.append(cse)

    def pointer_scan(self) -> set[int]:
        """Words of a symbol-less image that point at plausible code (raw binaries)."""
        out = set()
        for lo, hi in self.img.data_ranges:
            for a in range(lo, hi - 3, 4):
                v = self.img.read32(a)
                if v is None or v & 1 or not self.img.is_code(v):
                    continue
                op = self.img.read16(v)
                if op is None or sh4.decode(op) is None:
                    continue
                out.add(v)
        return out

    def run(self) -> None:
        img = self.img
        self.primaries = {img.entry} | {a for a in img.symbols if img.is_code(a)}
        if not img.symbols:
            self.primaries |= self.pointer_scan()
        # Discovered addresses inside a function's range become seeds of that function, others start functions.
        done: set[int] = set()
        while True:
            order = sorted(self.primaries)
            seeds: dict[int, set[int]] = {}

            for d in self.discovered:
                if d in self.primaries:
                    continue
                i = bisect.bisect_right(order, d) - 1
                if i >= 0:
                    seeds.setdefault(order[i], set()).add(d)
                else:
                    self.primaries.add(d)
            new = False
            for e in sorted(self.primaries):
                if e in done:
                    continue
                f = self.explore(e, seeds.get(e, set()))
                self.funcs[e] = f
                done.add(e)
                for t in f.calls:
                    if img.is_code(t) and not t & 1 and t not in self.primaries:
                        self.primaries.add(t)
                        new = True
            if not new:
                break
        # A discovered seed no function reached from its start still runs: make sure it is in some table.
        covered = set()
        for f in self.funcs.values():
            covered |= f.table
        for d in self.discovered - covered:
            if d not in self.funcs:
                self.funcs[d] = self.explore(d, set())

    # ---- emission ----

    @staticmethod
    def fname(a: int) -> str:
        return "dc_fn_%08x" % a

    def emit_function(self, f: Func, comments: bool) -> tuple[str, int]:
        """C source of one function, and the instructions it holds (delay slots included)."""
        out: list[str] = []
        w = out.append
        addrs = sorted(f.insns)
        succ_ft: dict[int, int | None] = {}
        for a in addrs:
            d = sh4.decode(f.insns[a])
            if d.flags & sh4.BRANCH:
                if d.name in ("BT", "BF"):
                    succ_ft[a] = a + 2
                elif d.name in ("BT_S", "BF_S"):
                    succ_ft[a] = a + 4
                else:
                    succ_ft[a] = None
            elif d.flags & sh4.SYNC:
                succ_ft[a] = None
            else:
                succ_ft[a] = a + 2
        labels = set(f.labels)
        for i, a in enumerate(addrs):
            s = succ_ft[a]
            nxt = addrs[i + 1] if i + 1 < len(addrs) else None
            if s is not None and s != nxt and s in f.insns:
                labels.add(s)
        # SYNC resume points must be labels even though control does not fall into them.
        labels |= {t for t in f.table if t in f.insns}
        table = {t for t in f.table if t in f.insns}

        def jump(t: int) -> str:
            if t in f.insns:
                return "goto L_%08x;" % t
            if t in self.funcs and t not in f.fallbacks:
                return "RC_TAIL(0x%08xu, %s);" % (t, self.fname(t))
            return "RC_EXIT(0x%08xu);" % t

        missing_ret: set[int] = set()

        def ret_label(ret: int) -> None:
            if ret not in f.insns:
                missing_ret.add(ret)

        def call_static(t: int, ret: int) -> str:
            ret_label(ret)
            if t in self.funcs and self.img.is_code(t):
                return "RC_CALL(0x%08xu, %s, 0x%08xu, L_%08x);" % (t, self.fname(t), ret, ret)
            return "RC_EXIT(0x%08xu);" % t

        def call_dyn(a: int, ret: int) -> str:
            ret_label(ret)
            g = f.guesses.get(a)
            if g is not None and g in self.funcs:
                return "RC_CALL_DYN_G(t_, 0x%08xu, %s, 0x%08xu, L_%08x);" % (g, self.fname(g), ret, ret)
            return "RC_CALL_DYN(t_, 0x%08xu, L_%08x);" % (ret, ret)

        def jump_dyn(a: int) -> str:
            g = f.guesses.get(a)
            if g is not None and g in f.insns:
                return "if (t_ == 0x%08xu) goto L_%08x; RC_JUMP_DYN(t_);" % (g, g)
            if g is not None and g in self.funcs:
                return "RC_JUMP_DYN_G(t_, 0x%08xu, %s);" % (g, self.fname(g))
            return "RC_JUMP_DYN(t_);"

        def plain(a: int, op: int) -> str:
            d = sh4.decode(op)
            txt = "sh4i_%s(c, 0x%08xu, 0x%04xu);" % (d.name, a, op)
            if comments:
                txt += " /* %s */" % sh4.disasm(a, op).replace("\t", " ").replace("*/", "* /")
            return txt

        w("void %s(sh4_cpu *c)" % self.fname(f.entry))
        w("{")
        w("    uint32_t t_;")
        w("    (void)t_;")
        w("    switch (c->pc) {")
        for t in sorted(table):
            w("    case 0x%08xu: goto L_%08x;" % (t, t))
        w("    default: return;")
        w("    }")
        count_total = 0
        # Leaders (labels, and where the previous instruction does not simply fall in) and the instruction counts
        # of their straight-line regions.
        leaders: dict[int, int] = {}
        cur = None
        for i, a in enumerate(addrs):
            p = addrs[i - 1] if i else None
            if (a in labels or p is None or succ_ft[p] != a
                    or sh4.decode(f.insns[p]).flags & (sh4.BRANCH | sh4.SYNC)):
                cur = a
                leaders[cur] = 0
            leaders[cur] += 2 if sh4.decode(f.insns[a]).flags & sh4.DELAY else 1
        prev_ft = None
        for i, a in enumerate(addrs):
            op = f.insns[a]
            d = sh4.decode(op)
            if prev_ft is not None and prev_ft != a:
                w("    " + jump(prev_ft))
            if a in labels:
                w("L_%08x:" % a)
                if a in table:
                    w("    RC_CHECK(0x%08xu);" % a)
            if a in leaders:
                w("    RC_CY(%d);" % leaders[a])
            count_total += 2 if d.flags & sh4.DELAY else 1
            name = d.name
            if not d.flags & sh4.BRANCH:
                w("    " + plain(a, op))
                if d.flags & sh4.SYNC:
                    w("    return;")  # the instruction set c->pc
                prev_ft = succ_ft[a]
                continue
            slot = None
            if d.flags & sh4.DELAY:
                slot = "    " + plain(a + 2, self.img.read16(a + 2))
            n = (op >> 8) & 15
            t = sh4.branch_target(a, op, name)
            if name in ("BT", "BF"):
                w("    if (%sc->t) %s" % ("" if name == "BT" else "!", jump(t)))
            elif name in ("BT_S", "BF_S"):
                w("    t_ = %sc->t;" % ("" if name == "BT_S" else "!"))
                w(slot)
                w("    if (t_) %s" % jump(t))
            elif name == "BRA":
                w(slot)
                w("    " + jump(t))
            elif name == "BSR":
                w("    c->pr = 0x%08xu;" % (a + 4))
                w(slot)
                w("    " + call_static(t, a + 4))
            elif name in ("JSR", "BSRF"):
                w("    t_ = %s;" % ("c->r[%d]" % n if name == "JSR" else "0x%08xu + c->r[%d]" % (a + 4, n)))
                w("    c->pr = 0x%08xu;" % (a + 4))
                w(slot)
                w("    " + call_dyn(a, a + 4))
            elif name == "JMP":
                w("    t_ = c->r[%d];" % n)
                w(slot)
                w("    " + jump_dyn(a))
            elif name == "BRAF":
                w("    t_ = 0x%08xu + c->r[%d];" % (a + 4, n))
                w(slot)
                cases = f.jtables.get(a)
                if cases:
                    w("    switch (t_) {")
                    for cse in cases:
                        w("    case 0x%08xu: %s" % (cse, jump(cse)))
                    w("    default: break;")
                    w("    }")
                    w("    RC_JUMP_DYN(t_);")
                else:
                    w("    " + jump_dyn(a))
            elif name == "RTS":
                w("    t_ = c->pr;")
                w(slot)
                w("    RC_RET(t_);")
            elif name == "RTE":
                w("    t_ = c->spc;")
                w("    sh4_set_sr(c, c->ssr);")
                w(slot)
                w("    RC_RET(t_);")
            prev_ft = succ_ft[a]
        if prev_ft is not None:
            w("    " + jump(prev_ft))
        # Return sites that are not in this function (a call that does not come back here, or whose return site
        # starts another function): back to the dispatcher with PC already there.
        for r in sorted(missing_ret):
            w("L_%08x:" % r)
            w("    return;")
        w("}")
        w("")
        return "\n".join(out), count_total
