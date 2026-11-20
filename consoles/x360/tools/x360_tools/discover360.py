"""Xbox 360 function discovery (XBA-270): what MSVC's PowerPC code generator adds to tools/ppc/discover.py.

- Register save / restore helpers. MSVC calls shared routines in prologues and epilogues instead of storing the
  non-volatile registers inline: `mflr r12; bl __savegprlr_N` ... `b __restgprlr_N`, and the FPR (__savefpr_N /
  __restfpr_N, base r12) and VMX (__savevmx_N / __restvmx_N, `li r11, off; stvx vN, r11, r12`, and the VMX128 ones
  for v64..v127) counterparts. Each is one straight run of stores (or loads) of consecutive registers, one entry
  point per register, ending in `blr`. They are recognised from their code (`find_helpers`, a conservative shape
  check: at least 8 consecutive registers ending at r31 / f31 / v31 / v127, one base register, regular
  displacements), hidden from discovery (they are not functions: no entry per register), and the lifter inlines
  them at each call (`XenonLifter`): a `bl` becomes the stores, a `b` (the epilogue's tail call) the loads and the
  return.
- Jump tables. A `bctr` whose CTR comes from a table indexed by a bounded register (`cmplwi rI, N` + a branch on GT
  before it) is resolved by evaluating the code before it backwards (`JumpTables`): absolute tables (`lwzx` of
  addresses: MSVC), tables of offsets added to a base (`lbzx` / `lhzx` / `lwzx`, optionally shifted, plus a constant:
  MSVC's compact forms, and GCC's relative tables). The index is the register as the bound check compared it: what
  computed it before that (MSVC's `addi rI, rX, -lowest case`) is not part of the table's address, and a load of the
  stack slot the compared value came from is that value again (code built without optimisation). A table without a
  check of its own takes the bound of the check further up that every path to it passes (`_shared_bound`: two
  switches on one value), or, MSVC's absolute table only, is read to its end (`_unbounded`). The targets become
  branch targets inside the function and the lifter turns the `bctr` into a switch. Anything else stays an indirect
  jump through the dispatcher, counted as unresolved.

Nothing here can be checked against MSVC output in this repository (no title code): the shapes are what XenonRecomp
and Xenia describe; recognition is deliberately strict, and `stats()` reports what was found so a title's sweep shows
what slipped through.

Memory (XBA-293): Prepare runs on a handheld, so nothing here grows by a Python object per guest word. Decoded
instructions live in a small bounded cache, explore() remembers one byte per word (`_flow`), and a finished function
keeps its addresses in an array (`PackedFunction`).
"""

from array import array
from bisect import bisect_right
from struct import iter_unpack

from tools.ppc.discover import Discovery
from tools.ppc.isa import BCCTR, BCLR, BCOND, BRANCH, INVALID, NORMAL, RFI

STORE_GPR = ("std", "ld")
MIN_REGS = 8


class Helper:
    """One recognised helper sequence. entries: {addr: first register saved / restored from there}."""
    __slots__ = ("kind", "start", "end", "entries", "tail")

    def __init__(self, kind, start, end, entries, tail):
        self.kind = kind          # savegpr, restgpr, savefpr, restfpr, savevmx, restvmx
        self.start = start
        self.end = end            # address of the final blr
        self.entries = entries
        self.tail = tail          # addresses of the instructions after the register run, blr included

    def __repr__(self):
        return "<%s %08X-%08X %d entries>" % (self.kind, self.start, self.end, len(self.entries))


def _run(view, dec, a, limit):
    """Decode straight-line from a while in code."""
    out = []
    while len(out) < limit and view.in_code(a):
        out.append(dec.decode(view.u32(a), a))
        a += 4
    return out


def find_helpers(view, dec, ranges):
    """{entry addr: (Helper, first register)} for every save / restore helper in the code ranges."""
    found = {}
    for lo, hi in ranges:
        a = lo
        while a < hi:
            h = _match(view, dec, a)
            if h is None:
                a += 4
                continue
            for e, reg in h.entries.items():
                found[e] = (h, reg)
            a = h.end + 4
    return found


def _continues(view, a, w):
    """The word after the store / load w at a handles the next register the same way (a helper has MIN_REGS)."""
    if not view.in_code(a + 4):
        return False
    n = view.u32(a + 4)
    same = 0xFC1F0003 if (w >> 26) in (58, 62) else 0xFC1F0000  # opcode, base register, the DS form's sub-opcode
    return (n & same) == (w & same) and ((n >> 21) & 31) == ((w >> 21) & 31) + 1


def _match(view, dec, a):
    # on the raw word first: this runs on every word of the code sections
    w = view.u32(a)
    op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    if op in (58, 62) and ra == 1 and not w & 3:  # ld, std
        return _match_gpr(view, dec, a, "ld" if op == 58 else "std") if _continues(view, a, w) else None
    if op in (50, 54) and ra == 12:               # lfd, stfd
        return _match_fpr(view, dec, a, "lfd" if op == 50 else "stfd") if _continues(view, a, w) else None
    if op == 14 and ra == 0 and rd == 11:         # li r11, off
        return _match_vmx(view, dec, a)
    return None


def _regular(regs, disps, last):
    if len(regs) < MIN_REGS or regs[-1] != last:
        return False
    return all(regs[k + 1] == regs[k] + 1 for k in range(len(regs) - 1)) and \
        all(disps[k + 1] - disps[k] == disps[1] - disps[0] for k in range(len(disps) - 1)) and disps[1] > disps[0]


def _match_gpr(view, dec, a, n):
    run = _run(view, dec, a, 24)
    regs, disps = [], []
    k = 0
    while k < len(run) and run[k].name == n and run[k].ra == 1:
        regs.append(run[k].rd)
        disps.append(run[k].ext.ds)
        k += 1
    if not _regular(regs, disps, 31) or len(run) < k + 2:
        return None
    t = run[k:]
    if n == "std":  # stw r12, d(r1); blr
        if not (t[0].name == "stw" and t[0].rd == 12 and t[0].ra == 1 and t[1].name == "bclr"
                and (t[1].rd & 0x14) == 0x14 and not t[1].lk):
            return None
        tail = t[:2]
        kind = "savegpr"
    else:           # lwz r12, d(r1); mtlr r12; blr
        if len(t) < 3 or not (t[0].name == "lwz" and t[0].rd == 12 and t[0].ra == 1 and t[1].name == "mtspr"
                              and t[1].spr == 8 and t[1].rd == 12 and t[2].name == "bclr"
                              and (t[2].rd & 0x14) == 0x14 and not t[2].lk):
            return None
        tail = t[:3]
        kind = "restgpr"
    entries = {run[j].addr: regs[j] for j in range(len(regs))}
    return Helper(kind, a, tail[-1].addr, entries, [x.addr for x in tail])


def _match_fpr(view, dec, a, n):
    run = _run(view, dec, a, 20)
    regs, disps = [], []
    k = 0
    while k < len(run) and run[k].name == n and run[k].ra == 12:
        regs.append(run[k].rd)
        disps.append(run[k].simm)
        k += 1
    if not _regular(regs, disps, 31) or k >= len(run):
        return None
    b = run[k]
    if not (b.name == "bclr" and (b.rd & 0x14) == 0x14 and not b.lk):
        return None
    entries = {run[j].addr: regs[j] for j in range(len(regs))}
    return Helper("savefpr" if n == "stfd" else "restfpr", a, b.addr, entries, [b.addr])


VMX_ST = ("stvx", "stvx128")
VMX_LD = ("lvx", "lvx128")


def _match_vmx(view, dec, a):
    run = _run(view, dec, a, 2 * 64 + 2)
    regs, disps, entries = [], [], {}
    k = 0
    kind = None
    while k + 1 < len(run):
        li, m = run[k], run[k + 1]
        if not (li.name == "addi" and li.ra == 0 and li.rd == 11):
            break
        if m.name in VMX_ST:
            mk = "savevmx"
        elif m.name in VMX_LD:
            mk = "restvmx"
        else:
            break
        if kind is None:
            kind = mk
        if mk != kind or m.ra != 11 or m.rb != 12:
            break
        regs.append(m.ext.vd)
        disps.append(li.simm)
        entries[li.addr] = m.ext.vd
        k += 2
    if kind is None or k >= len(run):
        return None
    last = 127 if regs and regs[-1] > 31 else 31
    if not _regular(regs, disps, last):
        return None
    b = run[k]
    if not (b.name == "bclr" and (b.rd & 0x14) == 0x14 and not b.lk):
        return None
    return Helper(kind, a, b.addr, entries, [b.addr])


# ---------------------------------------------------------------- functions only a pointer reaches

POINTER_PAIR_SPAN = 32  # instructions after a lis in which the other half of an address is looked for
_WRITES_RD = frozenset((7, 8, 12, 13, 14, 15, 32, 33, 34, 35, 40, 41, 42, 43, 58))  # the destination is rD
# the destination is rA: rotates and logical immediates, and the update forms of loads and stores
_WRITES_RA = frozenset((20, 21, 23, 24, 25, 26, 27, 28, 29, 30, 33, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55))


def pointer_seeds(img, view, decode):
    """Leaf functions that only a pointer reaches. MSVC gives a leaf function no .pdata entry (nothing to unwind), so
    one that is never called directly -- a virtual method, a callback -- is in no table and after no `bl`. Two places
    name it all the same: a word holding its address (a vtable, a table of callbacks; in a data section, or in a
    code section among other addresses), and a `lis` + `addi` / `ori` pair in code building it (a callback passed
    as an argument, a table filled in code). A candidate is a word-aligned code address outside every .pdata
    function (inside one it would be a label or a table of that function) whose first word decodes and is not
    itself an address in the image (a table of addresses in a code section). Without a .pdata table there is
    nothing to tell a function from a label: no candidates."""
    if img.image is None or not img.pdata:
        return set()
    starts = sorted(s for s, _ in img.pdata)
    ends = {s: s + n for s, n in img.pdata}
    lo, hi = img.base, img.base + len(img.image)
    code = sorted((s.va, s.va + s.size) for s in img.sections if s.code)
    if not code:
        return set()
    c_lo, c_hi = code[0][0], code[-1][1]
    seen = {}

    def candidate(t):
        r = seen.get(t)
        if r is None:
            r = False
            if not t & 3 and view.in_code(t):
                k = bisect_right(starts, t) - 1
                if k < 0 or t >= ends[starts[k]]:
                    w = view.u32(t)
                    r = not lo <= w < hi and decode(t).kind != INVALID
            seen[t] = r
        return r

    def aligned_code(w):
        return c_lo <= w < c_hi and not w & 3 and view.in_code(w)

    out = set()
    image = memoryview(img.image)
    for s in img.sections:
        a, b = max(s.va, lo), min(s.va + s.size, hi) & ~3
        if a >= b:
            continue
        if not s.code:  # (a) data: every aligned word
            for (w,) in iter_unpack(">I", image[a - lo:a - lo + ((b - a) & ~3)]):
                if c_lo <= w < c_hi and candidate(w):
                    out.add(w)
            continue
        # (b) code: lis rD, hi; then, down the straight run and while rD keeps that value, addi rX, rD, lo or
        # ori rX, rD, lo. Several addresses are often prepared together (the lis first, the addi where each is
        # stored), and one lis may serve more than one address.
        # (c) code sections also hold tables of addresses (vtables the linker merged into .text): a word that is a
        # candidate, next to another aligned code address, outside every .pdata function (inside one it is that
        # function's jump table, or an instruction: `lwz r16..r23` words look like addresses of the image)
        at, before = a, False
        for (w,) in iter_unpack(">I", image[a - lo:a - lo + ((b - a) & ~3)]):
            if c_lo <= w < c_hi and not w & 3:
                if (before or (at + 4 < b and aligned_code(img.u32(at + 4)))) and candidate(w):
                    k = bisect_right(starts, at) - 1
                    if k < 0 or at >= ends[starts[k]]:
                        out.add(w)
                before = view.in_code(w)
            else:
                before = False
            if w & 0xFC1F0000 == 0x3C000000:
                rd, top = (w >> 21) & 31, (w & 0xFFFF) << 16
                for p in range(at + 4, min(at + 4 + 4 * POINTER_PAIR_SPAN, b), 4):
                    v = img.u32(p)
                    op, d, s_ = v >> 26, (v >> 21) & 31, (v >> 16) & 31
                    if op == 18 or (op == 19 and (v >> 1) & 0x3FF in (16, 528) and d & 0x14 == 0x14):
                        if not v & 1 or rd < 14:  # the run ends, or a call leaves a volatile register undefined
                            break
                        continue
                    if op == 14 and s_ == rd:
                        t = (top + ((v & 0xFFFF) ^ 0x8000) - 0x8000) & 0xFFFFFFFF
                    elif op == 24 and d == rd:
                        t = top | (v & 0xFFFF)
                    else:
                        t = None
                    if t is not None and c_lo <= t < c_hi and candidate(t):
                        out.add(t)
                    if (op in _WRITES_RD and d == rd) or (op in _WRITES_RA and s_ == rd) \
                            or (op == 31 and rd in (d, s_)):
                        break  # rD is written again (op 31: either field may be the destination)
            at += 4
    return out


# ---------------------------------------------------------------- jump tables

# Which general registers an instruction writes, for the instructions that may stand in a shared-bound region
# (JumpTables._shared_bound). Extended opcodes of primary 31, OE bit dropped:
_X31_WRITES_RD = frozenset((8, 9, 10, 11, 19, 21, 23, 40, 73, 75, 87, 104, 136, 138, 200, 202, 232, 233, 234, 235,
                            266, 279, 339, 341, 343, 457, 459, 489, 491))
_X31_WRITES_RA = frozenset((24, 26, 27, 28, 58, 60, 124, 284, 316, 412, 444, 476, 536, 539, 792, 794, 824, 826, 827,
                            922, 954, 986))
_X31_WRITES_NONE = frozenset((0, 32, 144, 149, 151, 215, 407, 467))   # compares, mtcrf, stores, mtspr
_X31_OE = frozenset((8, 10, 40, 104, 136, 138, 200, 202, 232, 233, 234, 235, 266, 457, 459, 489, 491))


def is_nop(w):
    """`ori rX, rX, 0` (`nop` is `ori r0, r0, 0`; MSVC pads code with it, table code included): writes nothing."""
    return w >> 26 == 24 and (w >> 21) & 31 == (w >> 16) & 31 and not w & 0xFFFF


def is_call(w):
    """bl, bcl, blrl, bctrl: control comes back after it with the volatile registers undefined."""
    op = w >> 26
    return bool(w & 1) and (op in (16, 18) or (op == 19 and (w >> 1) & 0x3FF in (16, 528)))


def gpr_writes(w):
    """The general registers the instruction word writes, as a tuple; None for a call (`is_call`), a system call, a
    trap, or anything this does not know (the caller then gives up)."""
    op, d, a = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    if op in (7, 8, 12, 13, 14, 15, 32, 34, 40, 42):          # mulli .. addis, lwz, lbz, lhz, lha
        return (d,)
    if op in (33, 35, 41, 43):                                # the same loads with update
        return (d, a)
    if is_nop(w):
        return ()
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29, 30):        # rotates, logical immediates
        return (a,)
    if op in (10, 11, 36, 38, 44, 48, 50, 52, 54):            # compares, stores, FP loads and stores
        return ()
    if op in (37, 39, 45, 49, 51, 53, 55):                    # stores and FP loads with update
        return (a,)
    if op in (16, 18):                                        # bc, b: not a call
        return None if w & 1 else ()
    if op == 19:                                              # bclr / bcctr (not a call), CR logic, mcrf
        x = (w >> 1) & 0x3FF
        if x in (16, 528):
            return None if w & 1 else ()
        return () if x in (0, 33, 129, 193, 225, 257, 289, 417, 449) else None
    if op == 58:                                              # ld, ldu, lwa
        return (d, a) if w & 3 == 1 else (d,)
    if op == 62:                                              # std, stdu
        return (a,) if w & 3 == 1 else ()
    if op in (59, 63):                                        # FP arithmetic
        return ()
    if op == 31:
        x = (w >> 1) & 0x3FF
        if x & 0x200 and (x & 0x1FF) in _X31_OE:
            x &= 0x1FF
        if x in _X31_WRITES_RD:
            return (d,)
        if x in _X31_WRITES_RA:
            return (a,)
        if x in _X31_WRITES_NONE:
            return ()
    return None


class _Expr:
    """A value computed before a bctr: const, the index register, a table load, a shift or a sum."""
    __slots__ = ("k", "a", "b", "c")

    def __init__(self, k, a=None, b=None, c=None):
        self.k, self.a, self.b, self.c = k, a, b, c


class JumpTables:
    """Resolve bctr jump tables by evaluating the straight-line code before them backwards."""

    BACK = 24

    def __init__(self, view, decode):
        self.view = view
        self.decode = decode
        self.tables = {}      # bctr addr -> [targets]
        self.unresolved = set()
        self._index = None    # while a table is evaluated: (index register, where its bound check is in the window)
        self._slot = None     # and, when the compared value was loaded from the stack: (displacement, where)
        self._functions = None  # the .pdata functions, sorted (made when a table has no bound check)
        self._busy = set()      # the bctr being resolved (a table's proof may ask for the function's other tables)

    def resolve(self, a):
        if a in self.tables:
            return self.tables[a]
        if a in self._busy:
            return ()
        self._busy.add(a)
        try:
            t = self._resolve(a)
        finally:
            self._busy.discard(a)
        if t is None:
            self.unresolved.add(a)
            return ()
        self.unresolved.discard(a)
        self.tables[a] = t
        return t

    def _back(self, a):
        """The instructions before a, nearest first, until a non-conditional control transfer."""
        out = []
        p = a - 4
        while len(out) < self.BACK and self.view.in_code(p):
            i = self.decode(p)
            if i.kind in (BRANCH, BCLR, BCCTR) or (i.kind == BCOND and (i.rd & 0x14) == 0x14):
                break
            out.append(i)
            p -= 4
        return out

    def _def(self, insns, start, reg):
        """Index in insns (nearest first) of the instruction that last wrote reg, at or after start. The index
        register is followed no further back than its bound check (`_index`): the table is indexed by the value
        that was compared, whatever computed it (MSVC subtracts the lowest case first: `addi rI, rX, -min`)."""
        end = len(insns)
        if self._index is not None and reg == self._index[0]:
            end = min(end, self._index[1])
        for k in range(start, end):
            i = insns[k]
            n = i.name
            if n is None:
                return None
            if is_nop(i.word):
                continue   # a `nop` between the table code's instructions writes nothing
            if n in ("addi", "addis", "lwzx", "lhzx", "lbzx", "lwz", "lhz", "lbz", "add", "mfspr", "lis",
                     "subf", "mulli", "lwax", "ld", "ldx") and i.rd == reg:
                return k
            if n in ("ori", "oris", "rlwinm", "or", "rldicr", "rldicl", "extsw", "and", "andi.", "xor",
                     "clrlwi", "slw", "srw", "sraw", "srawi", "extsh", "extsb") and i.ra == reg:
                return k
            if n in ("lwzu", "lbzu", "lhzu", "ldu") and (i.rd == reg or i.ra == reg):
                return k
        return -1  # not written in the window: a leaf

    def _ev(self, insns, start, reg, depth=0):
        if depth > 12:
            return None
        k = self._def(insns, start, reg)
        if k is None:
            return None
        if k < 0:
            return _Expr("reg", reg)
        i = insns[k]
        n = i.name
        nxt = k + 1
        if n == "addi":
            if i.ra == 0:
                return _Expr("const", i.simm & 0xFFFFFFFF)
            return self._add(self._ev(insns, nxt, i.ra, depth + 1), _Expr("const", i.simm & 0xFFFFFFFF))
        if n == "addis":
            if i.ra == 0:
                return _Expr("const", (i.uimm << 16) & 0xFFFFFFFF)
            return self._add(self._ev(insns, nxt, i.ra, depth + 1), _Expr("const", (i.uimm << 16) & 0xFFFFFFFF))
        if n == "ori":
            x = self._ev(insns, nxt, i.rd, depth + 1)
            if x is not None and x.k == "const":
                return _Expr("const", x.a | i.uimm)
            return None
        if n == "or" and i.rd == i.rb:  # mr
            return self._ev(insns, nxt, i.rd, depth + 1)
        if n == "add":
            return self._add(self._ev(insns, nxt, i.ra, depth + 1), self._ev(insns, nxt, i.rb, depth + 1))
        if n == "rlwinm":
            sh, mb, me = i.rb, i.mb, i.me
            x = self._ev(insns, nxt, i.rd, depth + 1)
            if x is None:
                return None
            if mb == 0 and me == 31 - sh:            # slwi
                return _Expr("shl", x, sh)
            if sh == 0 and mb > 0 and me == 31:      # clrlwi: an index already bounded
                return x
            return None
        if n == "lwz":
            # the index loaded again from the stack slot the compared value came from (code built without
            # optimisation keeps every local in memory: `lwz rX, d(r1); cmplwi rX, N; bgt ...; lwz rY, d(r1)`): the
            # same value, as nothing between the two loads stores to memory or moves the stack pointer
            s = self._slot
            if s is not None and i.ra == 1 and i.simm == s[0] and k < s[1] \
                    and all(self._keeps_stack(x) for x in insns[k + 1:s[1]]):
                return _Expr("reg", self._index[0])
            return None
        if n in ("lwzx", "lhzx", "lbzx"):
            a, b = self._ev(insns, nxt, i.ra, depth + 1) if i.ra else _Expr("const", 0), \
                self._ev(insns, nxt, i.rb, depth + 1)
            if a is None or b is None:
                return None
            if b.k == "const" and a.k != "const":
                a, b = b, a
            if a.k != "const":
                return None
            return _Expr("load", {"lwzx": 4, "lhzx": 2, "lbzx": 1}[n], a.a, b)
        return None

    @staticmethod
    def _keeps_stack(i):
        """An instruction that stores nothing and leaves r1 alone, of the few that stand between a bound check and
        its table code; anything else is taken to do either."""
        if i.kind == BCOND:
            return not i.lk
        n = i.name
        if n in ("cmpli", "cmpi", "cmp", "cmpl"):
            return True
        if n in ("lwz", "lbz", "lhz", "addi", "addis", "lis"):
            return i.rd != 1
        if n in ("ori", "rlwinm"):
            return i.ra != 1
        return False

    def _stack_slot(self, insns, at, reg):
        """(displacement, index in insns) of the `lwz reg, d(r1)` that loaded the register the bound check at
        insns[at] compared, or None."""
        k = self._def(insns, at + 1, reg)
        if k is None or k < 0:
            return None
        i = insns[k]
        if i.name == "lwz" and i.ra == 1 and i.rd == reg and reg != 1:
            return (i.simm, k)
        return None

    @staticmethod
    def _add(x, y):
        if x is None or y is None:
            return None
        if x.k == "const" and y.k == "const":
            return _Expr("const", (x.a + y.a) & 0xFFFFFFFF)
        return _Expr("add", x, y)

    def _calc(self, e, idx_reg, i):
        k = e.k
        if k == "const":
            return e.a
        if k == "reg":
            if e.a != idx_reg:
                raise ValueError("depends on another register")
            return i
        if k == "shl":
            return (self._calc(e.a, idx_reg, i) << e.b) & 0xFFFFFFFF
        if k == "add":
            return (self._calc(e.a, idx_reg, i) + self._calc(e.b, idx_reg, i)) & 0xFFFFFFFF
        if k == "load":
            ea = (e.b + self._calc(e.c, idx_reg, i)) & 0xFFFFFFFF
            raw = self._read(ea, e.a)
            if raw is None:
                raise ValueError("table outside the image")
            return raw
        raise ValueError(k)

    def _read(self, ea, size):
        img = getattr(self.view, "img", None)
        if img is None:
            return None
        b = img.read(ea, size)
        if b is None:
            return None
        return int.from_bytes(b, "big")

    def _resolve(self, a):
        insns = self._back(a)
        k = next((j for j, i in enumerate(insns) if i.name == "mtspr" and i.spr == 9), None)
        if k is None:
            return None
        # the bound: cmplwi crF, rI, N then a conditional branch on crF's GT bit, before the table code
        bound, at = None, len(insns)
        for j in range(k + 1, len(insns)):
            i = insns[j]
            if i.name == "cmpli" and not (i.rd & 1):
                crf = i.rd >> 2
                if any(b.kind == BCOND and b.ra == 4 * crf + 1 for b in insns[k + 1:j]):
                    bound, at = (i.ra, i.uimm), j
                    break
        if bound is None:
            bc = self._bound_before(insns)
            if bc is None:
                return self._unbounded(a, insns, k) or self._shared_bound(a, insns, k)
            bound = bc
        reg, n = bound
        if n > 4096:
            return None
        slot = self._stack_slot(insns, at, reg) if at < len(insns) else None
        self._index, self._slot = (reg, at), slot
        try:
            e = self._ev(insns, k + 1, insns[k].rd)
        finally:
            self._index = self._slot = None
        if e is None or e.k == "const":
            return None
        targets = []
        try:
            for x in range(n + 1):
                t = self._calc(e, reg, x)
                if t is None:
                    return None
                # GCC's relative tables hold signed offsets: the sum wraps the same way
                targets.append(t & 0xFFFFFFFF)
        except ValueError:
            return None
        if not all(self.view.in_code(t) and t % 4 == 0 for t in targets):
            return None
        return targets

    def _function(self, a):
        """The .pdata function holding a, as (start, end), or None."""
        img = getattr(self.view, "img", None)
        pdata = getattr(img, "pdata", None)
        if not pdata:
            return None
        if self._functions is None:
            self._functions = sorted((s, s + n) for s, n in pdata)
        f = bisect_right(self._functions, (a, 0xFFFFFFFF)) - 1
        if f < 0 or not self._functions[f][0] <= a < self._functions[f][1]:
            return None
        return self._functions[f]

    @classmethod
    def _leaves(cls, e, out):
        if e.k == "reg":
            out.add(e.a)
        elif e.k == "shl":
            cls._leaves(e.a, out)
        elif e.k == "add":
            cls._leaves(e.a, out)
            cls._leaves(e.b, out)
        elif e.k == "load":
            cls._leaves(e.c, out)

    SHARED_BOUND_GAP = 3   # instructions between the compare and its branch (loads the table code needs)

    def _shared_bound(self, a, insns, k):
        """A table whose bound check stands further up the function and serves more than one table: `cmplwi crF, rI,
        N; bgt crF, default`, a first switch on rI, and in some of its cases a second switch on rI again, which the
        compiler emits without a check. The bound is that compare's when the table code can only be reached through
        it with rI untouched. That is shown by walking the function backwards from the bctr, over every way into
        each instruction (falling into it, a branch, a case of one of the function's other tables), down to the
        instruction after the `bgt` (`_only_through`):
        - no instruction met writes rI (each is one `gpr_writes` knows; a call is allowed when rI is one of the
          registers a callee preserves, r14 to r31);
        - the walk never reaches the function's entry, nor leaves through the `bgt` itself (its default label
          leading to the table), nor finds a second way to the instruction after the `bgt`;
        - the function's other bctr are resolved tables.
        So every path to the table falls out of the `bgt` and keeps rI: it is at most N. Only inside a .pdata
        function (taken to be entered at its start), and every target must be a word of that function."""
        f = self._function(a)
        if f is None:
            return None
        fs, fe = f
        # the table code proper: what follows the last conditional branch before the bctr, all of it in the function
        cut = next((j for j, i in enumerate(insns) if i.kind == BCOND or i.addr < fs), len(insns))
        insns = insns[:cut]
        if k >= len(insns):
            return None
        e = self._ev(insns, k + 1, insns[k].rd)
        if e is None:
            return None
        regs = set()
        self._leaves(e, regs)
        if len(regs) != 1:
            return None
        idx = regs.pop()
        flow = None
        tried = 0
        for c in range(a - 4, fs - 4, -4):
            w = self.view.u32(c) if self.view.in_code(c) else 0
            if w >> 26 != 10 or (w >> 21) & 1 or (w >> 16) & 31 != idx:
                continue
            crf, n = (w >> 23) & 7, w & 0xFFFF
            g = self._bound_branch(c, crf, idx, min(a, fe - 4))
            if g is None or n > 4096 or insns[-1].addr < g:
                continue
            if flow is None:
                flow = self._function_flow(a, fs, fe)
                if flow is None:
                    return None
            if self._only_through(g, a, idx, fs, flow):
                targets = []
                try:
                    for x in range(n + 1):
                        targets.append(self._calc(e, idx, x) & 0xFFFFFFFF)
                except ValueError:
                    return None
                if all(fs <= t < fe and not t & 3 and self.view.in_code(t) for t in targets):
                    return targets
                return None
            tried += 1
            if tried >= 4:
                break
        return None

    def _bound_branch(self, c, crf, idx, limit):
        """The address after the `bgt crF` that follows the compare at c, within a few loads that leave the index
        alone; None when there is none."""
        for p in range(c + 4, min(c + 4 * (self.SHARED_BOUND_GAP + 2), limit), 4):
            if not self.view.in_code(p):
                return None
            w = self.view.u32(p)
            op = w >> 26
            if op == 16:
                bo, bi = (w >> 21) & 31, (w >> 16) & 31
                return p + 4 if (bo & 0x1C) == 0x0C and bi == 4 * crf + 1 and not w & 3 else None
            # a `nop`, or a load that sets no condition register and leaves the index alone
            if is_nop(w):
                continue
            if not (op in (32, 34, 40) or (op == 31 and (w >> 1) & 0x3FF in (23, 87, 279))) \
                    or (w >> 21) & 31 == idx:
                return None
        return None

    def _function_flow(self, a, fs, fe):
        """Every (source, target) of the function's direct branches and of its other tables; None when one of its
        other bctr is no resolved table."""
        out = []
        for p in range(fs, fe, 4):
            if not self.view.in_code(p):
                continue
            i = self.decode(p)
            if i.kind in (BRANCH, BCOND):
                out.append((p, i.target))
            elif i.kind == BCCTR and not i.lk and p != a:
                if (i.rd & 0x14) != 0x14:
                    return None
                t = self.resolve(p)
                if not t:
                    return None
                out += [(p, x) for x in t]
        return out

    ONLY_THROUGH_LIMIT = 8192   # instructions walked back from a bctr before giving up

    def _only_through(self, g, a, idx, fs, flow):
        """Whether every path of the function to the bctr at a comes through g, entered by falling into it, and
        writes the index nowhere from g on."""
        into = {}
        for src, t in flow:
            into.setdefault(t, []).append(src)
        if g in into:
            return False
        seen, work = {a}, [a]
        while work:
            n = work.pop()
            if n != a:
                w = self.view.u32(n)
                wr = gpr_writes(w)
                if wr is None:
                    if not is_call(w) or idx < 14:
                        return False
                elif idx in wr:
                    return False
            if n == g:
                continue
            if n == fs or n == g - 4 or len(seen) > self.ONLY_THROUGH_LIMIT:
                return False   # the entry, or the `bgt` itself: its default label leads to the table
            before = list(into.get(n, ()))
            p = n - 4
            if self.view.in_code(p):
                i = self.decode(p)
                # everything runs on into n but a jump, a return and a word that is no instruction (a call does, when
                # it returns)
                always = i.kind == BRANCH or (i.kind in (BCOND, BCLR, BCCTR) and (i.rd & 0x14) == 0x14)
                if not (i.kind in (INVALID, RFI) or (always and not i.lk)):
                    before.append(p)
            for p in before:
                if p < fs:
                    return False
                if p not in seen:
                    seen.add(p)
                    work.append(p)
        return True

    def _unbounded(self, a, insns, k):
        """A table with no bound check: the compiler knew the index's range (a switch over every value of a small
        type). Only MSVC's absolute table (`lwzx` of addresses at index << 2) inside a .pdata function: its entries
        are read while they are code addresses of that function, and, for a table placed in the function's own code,
        no further than the first case label after it. A word read too far is one more label of the same function."""
        f = self._function(a)
        if f is None:
            return None
        fs, fe = f
        insns = [i for i in insns if i.addr >= fs]  # what precedes the function (padding, another one) is not its code
        e = self._ev(insns, k + 1, insns[k].rd)
        if e is None or e.k != "load" or e.a != 4:
            return None
        index = e.c
        if index.k != "shl" or index.b != 2 or index.a.k != "reg":
            return None
        table, targets, label = e.b, [], 0xFFFFFFFF
        while len(targets) < 4096:
            p = table + 4 * len(targets)
            if p >= label:
                break
            t = self._read(p, 4)
            if t is None or t & 3 or not fs <= t < fe or not self.view.in_code(t):
                break
            targets.append(t)
            if p < t < label and fs <= table < fe:
                label = t
        return targets if len(targets) > 1 else None

    def _bound_before(self, insns):
        """The bound check may sit just before the straight run (the conditional branch ends it): look one block
        further back, through the branch."""
        if not insns:
            return None
        p = insns[-1].addr - 4
        if not self.view.in_code(p):
            return None
        b = self.decode(p)
        if b.kind == BRANCH and not b.lk:
            # the other way round: `ble crF, <the table code>; b <default>` right before the run
            p -= 4
            if not self.view.in_code(p):
                return None
            b = self.decode(p)
            if b.kind != BCOND or (b.rd & 0x1C) != 0x04 or b.target != insns[-1].addr:
                return None
        elif b.kind == BCLR:
            # a leaf function returns when the index is out of range: `bgtlr crF`
            if (b.rd & 0x1C) != 0x0C or b.lk:
                return None
        elif b.kind != BCOND or (b.rd & 0x14) == 0x14:
            return None
        q = p - 4
        for _ in range(6):
            if not self.view.in_code(q):
                return None
            i = self.decode(q)
            if i.name == "cmpli" and not (i.rd & 1) and b.ra == 4 * (i.rd >> 2) + 1:
                return (i.ra, i.uimm)
            if i.kind != "normal":
                return None
            q -= 4
        return None


class PackedFunction:
    """A discovered function at 4 bytes per instruction. `packed` holds the addresses in the order the explored set
    gave them; `addrs` is that set again while the function is being lifted (`release` drops it)."""
    __slots__ = ("entry", "packed", "_calls", "_addrs")

    def __init__(self, f):
        self.entry = f.entry
        self.packed = array("I", f.addrs)
        self._calls = array("I", f.calls)
        self._addrs = None

    @property
    def addrs(self):
        if self._addrs is None:
            self._addrs = set(self.packed)
        return self._addrs

    @property
    def calls(self):
        return set(self._calls)

    def release(self):
        self._addrs = None

    def __repr__(self):
        return "<fn %08X: %d insns>" % (self.entry, len(self.packed))


class _Plain:
    """What explore() needs of a word known not to change the control flow."""
    kind = NORMAL


_PLAIN = _Plain()
_FLOW_KINDS = (INVALID, BRANCH, BCOND, BCLR, BCCTR, RFI)
INSN_CACHE = 1 << 16   # decoded instructions kept: more than a function holds, far less than a title


class XenonDiscovery(Discovery):
    """Discovery with jump tables resolved (the image's helpers are hidden by the view, see recomp.CodeView)."""

    def __init__(self, image, decoder, seeds=()):
        super().__init__(image, decoder, seeds)
        self.jt = JumpTables(image, self.insn)
        self.flow = self._flow
        self._pages = {}   # 64 KB of guest code -> one byte per word: 0 not decoded yet, 1 plain, 2 control flow

    def insn(self, a):
        i = self._cache.get(a)
        if i is None:
            if len(self._cache) >= INSN_CACHE:
                self._cache.clear()
            i = self.decoder.decode(self.image.u32(a), a)
            self._cache[a] = i
        return i

    def _flow(self, a):
        """explore() reads every word once per pass: a plain word is decoded in the first pass only."""
        page = self._pages.get(a >> 16)
        if page is None:
            page = self._pages[a >> 16] = bytearray(0x4000)
        k = (a & 0xFFFF) >> 2
        known = page[k]
        if known == 1:
            return _PLAIN
        if known == 2:
            return self.insn(a)
        i = self.decoder.decode(self.image.u32(a), a)
        page[k] = 2 if i.kind in _FLOW_KINDS else 1
        return i

    def finished(self, f):
        return PackedFunction(f)

    def indirect_targets(self, f, a, i):
        if (i.rd & 0x14) != 0x14:
            return ()
        return self.jt.resolve(a)

    def stats(self):
        fs = self.functions
        lo = min((min(f.packed) for f in fs if f.packed), default=0)
        hi = max((max(f.packed) for f in fs if f.packed), default=0)
        covered = bytearray(((hi - lo) >> 2) + 1)   # one byte per word: a set of every address is hundreds of MB
        for f in fs:
            for a in f.packed:
                covered[(a - lo) >> 2] = 1
        s = {"functions": len(fs), "instructions": covered.count(1),
             "lifted_instructions": sum(len(f.packed) for f in fs)}
        s["jump_tables"] = len(self.jt.tables)
        s["jump_table_targets"] = sum(len(t) for t in self.jt.tables.values())
        s["unresolved_bctr"] = len(self.jt.unresolved)
        return s
