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
  computed it before that (MSVC's `addi rI, rX, -lowest case`) is not part of the table's address. The targets become
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

def pointer_seeds(img, view, decode):
    """Leaf functions that only a pointer reaches. MSVC gives a leaf function no .pdata entry (nothing to unwind), so
    one that is never called directly -- a virtual method, a callback -- is in no table and after no `bl`. Two places
    name it all the same: a word of a data section holding its address (a vtable, a table of callbacks), and a
    `lis` + `addi` / `ori` pair in code building it (a callback passed as an argument). A candidate is a word-aligned
    code address outside every .pdata function (inside one it would be a label or a table of that function) whose
    first word decodes and is not itself an address in the image (a table of addresses in a code section). Without
    a .pdata table there is nothing to tell a function from a label: no candidates."""
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
        # (b) code: lis rD, hi; then within a few instructions addi rX, rD, lo or ori rD, rD, lo
        at = a
        for (w,) in iter_unpack(">I", image[a - lo:a - lo + ((b - a) & ~3)]):
            if w & 0xFC1F0000 == 0x3C000000:
                rd, top = (w >> 21) & 31, (w & 0xFFFF) << 16
                for p in range(at + 4, min(at + 24, b), 4):
                    v = img.u32(p)
                    op = v >> 26
                    if op == 14 and (v >> 16) & 31 == rd:
                        t = (top + ((v & 0xFFFF) ^ 0x8000) - 0x8000) & 0xFFFFFFFF
                    elif op == 24 and (v >> 21) & 31 == rd:
                        t = top | (v & 0xFFFF)
                    else:
                        continue
                    if c_lo <= t < c_hi and candidate(t):
                        out.add(t)
                    break
            at += 4
    return out


# ---------------------------------------------------------------- jump tables

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

    def resolve(self, a):
        if a in self.tables:
            return self.tables[a]
        t = self._resolve(a)
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
                return None
            bound = bc
        reg, n = bound
        if n > 4096:
            return None
        self._index = (reg, at)
        try:
            e = self._ev(insns, k + 1, insns[k].rd)
        finally:
            self._index = None
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

    def _bound_before(self, insns):
        """The bound check may sit just before the straight run (the conditional branch ends it): look one block
        further back, through the branch."""
        if not insns:
            return None
        p = insns[-1].addr - 4
        if not self.view.in_code(p):
            return None
        b = self.decode(p)
        if b.kind != BCOND or (b.rd & 0x14) == 0x14:
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
