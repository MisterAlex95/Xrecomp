"""Function discovery in a GBA ROM without symbols (XBA-150).

Recursive descent from the cartridge entry (0x08000000, ARM), following B / Bcc inside a function and taking BL
targets, BX to known constants (interworking by bit 0, `bx pc` veneers), the IRQ handler stored to 0x03007FFC and
Thumb code pointers in literal pools as new function entries. Jump tables: ARM `ldr pc, [pc, rN, lsl #2]` and
`add pc, pc, rN, lsl #2` branch tables, Thumb `ldr / mov pc` tables and the libgcc __gnu_thumb1_case_* helpers, each
bounded by the `cmp rN, #max` before it. Literal pools and jump tables become data barriers that no function runs
into. Addresses from earlier runs (`--discovered`, bit 0 = Thumb) are entries too.

Conservative by construction: an entry that only a literal pool suggests (weak) is kept only when its code decodes
cleanly (no undefined encoding, no branch out of the cartridge, no torn BL pair, no data barrier); everything the
static pass misses or refuses is run by the interpreter at run time and reported back by the discovery loop (the
core's gba_recomp_missed). Code a game copies to IWRAM / EWRAM is not lifted: it always runs in the interpreter.
Lifting data as code would only cost size, never correctness: the lifter gives every encoding the interpreter's
meaning and the cartridge is read-only.
"""

from .isa import arm_branch_target, arm_class, ror, sext, thumb_bl_target, thumb_class
from .rom import ROM_BASE

MAX_FN_INSNS = 4000      # bigger functions are cut; the rest become functions of their own
MAX_WEAK_INSNS = 2000    # a weak candidate whose body grows past this is refused
TABLE_MAX = 1024         # entries of one jump table at most
IRQ_VECTOR = (0x03007FFC, 0x03FFFFFC)


class Function:
    def __init__(self, entry: int, thumb: bool):
        self.entry = entry
        self.thumb = thumb
        self.addrs = set()
        self.calls = set()      # (addr, thumb) entries this function's code names
        self.weak = set()       # (addr, thumb) code pointers seen in literal pools / IRQ vector stores
        self.data = set()       # (addr, size) literal pool words, jump tables
        self.bad = None         # why a strict exploration refused it
        self.frontier = set()   # targets left out by the size cap

    @property
    def key(self):
        return self.entry | int(self.thumb)


class Discovery:
    def __init__(self, rom, discovered=()):
        self.rom = rom
        self.entries = {}       # key (addr | thumb) -> 'entry' / 'call' / 'bx' / 'discovered' / 'weak' / 'split'
        self.data = {}          # byte address -> 1 for every data byte (pools, tables)
        self.rejected = {}      # key -> reason (weak candidates refused)
        self.case_helpers = {}  # thumb helper address -> kind ('uqi', 'sqi', 'uhi', 'shi', 'si') or None
        self.functions = []
        self.discovered = [a for a in discovered]

    # ---- ROM helpers ---------------------------------------------------------------------------------------

    def in_rom(self, addr, size=2):
        return self.rom.contains(addr, size)

    def is_data(self, addr, size):
        return any((addr + i) in self.data for i in range(size))

    def mark_data(self, addr, size):
        for i in range(size):
            self.data[addr + i] = 1

    # ---- case helpers --------------------------------------------------------------------------------------

    def case_helper(self, addr):
        """Kind of libgcc __gnu_thumb1_case_* helper at addr (Thumb), or None."""
        if addr in self.case_helpers:
            return self.case_helpers[addr]
        kind = None
        ops = []
        for i in range(12):
            op = self.rom.u16(addr + 2 * i)
            if op is None:
                break
            ops.append(op)
            if op in (0x4770, 0x46F7):  # bx lr / mov pc, lr
                break
        if ops and ops[-1] in (0x4770, 0x46F7) and 0x4671 in ops and (0x448E in ops or 0x4686 in ops):
            loads = {3: "sqi", 4: "si", 5: "uhi", 6: "uqi", 7: "shi"}
            for op in ops:
                if (op >> 12) == 5:  # load / store with register offset
                    k = (op >> 9) & 7
                    if k in loads:
                        kind = loads[k]
                        break
        self.case_helpers[addr] = kind
        return kind

    def case_table(self, kind, ret, count):
        """(targets, data range) of a case table after a BL to a helper; ret = the BL's return address."""
        targets = []
        if kind in ("uqi", "sqi"):
            for i in range(count):
                v = self.rom.u8(ret + i)
                if v is None:
                    return None
                targets.append(ret + 2 * (sext(v, 8) if kind == "sqi" else v))
            return targets, (ret, count)
        if kind in ("uhi", "shi"):
            for i in range(count):
                v = self.rom.u16(ret + 2 * i)
                if v is None:
                    return None
                targets.append(ret + 2 * (sext(v, 16) if kind == "shi" else v))
            return targets, (ret, 2 * count)
        base = (ret + 3) & ~3
        for i in range(count):
            v = self.rom.u32(base + 4 * i)
            if v is None:
                return None
            targets.append((base + v) & 0xFFFFFFFE)
        return targets, (ret, base + 4 * count - ret)

    # ---- one function --------------------------------------------------------------------------------------

    def explore(self, entry, thumb, strict=False, stops=None, cap=MAX_FN_INSNS):
        """Walks a function's code from entry. stops: entry keys that end the function (other functions)."""
        f = Function(entry, thumb)
        size = 2 if thumb else 4
        todo = [(entry, {}, None)]  # (address, known constants, bound of the last `cmp rN, #imm`)
        seen_blocks = set()
        while todo:
            a, consts, bound = todo.pop()
            if a in seen_blocks:
                continue
            seen_blocks.add(a)
            while True:
                if a != entry and stops is not None and (a | int(thumb)) in stops:
                    break  # another function: leave through the dispatcher
                if a in f.addrs:
                    break
                if not self.in_rom(a, size) or a & (size - 1):
                    if strict:
                        f.bad = "runs out of the cartridge at 0x%08X" % a
                        return f
                    break
                if self.is_data(a, size):
                    if strict:
                        f.bad = "runs into data at 0x%08X" % a
                        return f
                    break
                if len(f.addrs) >= cap:
                    if strict:
                        f.bad = "too big"
                        return f
                    f.frontier.add(a)
                    break
                f.addrs.add(a)
                res = (self.step_thumb if thumb else self.step_arm)(f, a, consts, bound, strict)
                if f.bad:
                    return f
                succ, fall, consts, bound = res
                for t in succ:
                    if strict and not self.in_rom(t, size):
                        f.bad = "branch out of the cartridge to 0x%08X" % t
                        return f
                    todo.append((t, {}, None))
                if not fall:
                    break
                a += size
        return f

    def _cmp_bound(self, bound):
        return None if bound is None else bound[1]

    def step_arm(self, f, a, consts, bound, strict):
        """(intra successors, falls through, constants, cmp bound) after the ARM instruction at a."""
        op = self.rom.u32(a)
        cond = op >> 28
        if cond == 0xF:
            if strict:
                f.bad = "NV condition at 0x%08X" % a
            return [], True, consts, bound
        cls = arm_class(op)
        al = cond == 0xE
        consts = dict(consts)
        if cls == "branch":
            t = arm_branch_target(op, a)
            if op & (1 << 24):  # BL
                f.calls.add((t, False))
                for r in (0, 1, 2, 3, 12, 14):
                    consts.pop(r, None)
                return [], True, consts, None
            return [t], not al, consts, bound
        if cls == "bx":
            rm = op & 15
            v = consts.get(rm, (a + 8) if rm == 15 else None)
            if v is not None and self.in_rom(v & ~1):
                f.calls.add((v & ~1, bool(v & 1)))
            # mov lr, pc ; bx rN: a call, it comes back here
            call = self.rom.u32(a - 4) == 0xE1A0E00F
            return [], (not al) or call, consts, bound
        if cls == "undef":
            if strict:
                f.bad = "undefined ARM encoding at 0x%08X" % a
            return [], False, consts, bound
        if cls == "swi":
            if strict and ((op & 0xFFFF) or ((op >> 16) & 0xFF) > 0x2B):
                f.bad = "odd SWI at 0x%08X" % a
            return [], True, {}, bound
        if cls == "dp":
            opc, s, rn, rd = (op >> 21) & 15, (op >> 20) & 1, (op >> 16) & 15, (op >> 12) & 15
            if opc == 0xA and (op & (1 << 25)) and al:  # cmp rN, #imm: bound of a table to come
                bound = (rn, ror(op & 0xFF, (op >> 7) & 0x1E))
            if rd == 15 and not (8 <= opc <= 11):
                if strict and s:
                    f.bad = "exception return at 0x%08X" % a
                succ = []
                # addls pc, pc, rN, lsl #2 ; b default ; b case0 ; b case1 ...
                if opc == 4 and rn == 15 and not (op & (1 << 25)) and (op & 0xFF0) == 0x100 and bound:
                    n = min(bound[1] + 1, TABLE_MAX)
                    succ = [a + 8 + 4 * i for i in range(n)]
                v = self.arm_dp_value(op, a, consts)
                if v is not None:
                    succ = [v & ~3]
                return succ, not al, consts, bound
            if not (8 <= opc <= 11):
                v = self.arm_dp_value(op, a, consts)
                if v is None:
                    consts.pop(rd, None)
                else:
                    consts[rd] = v
                    # adr rd, label (add / sub rd, pc, #imm): a code address taken, maybe jumped to later
                    if rn == 15 and (op & (1 << 25)) and opc in (2, 4) and not v & 3 and self.in_rom(v, 4):
                        f.weak.add((v, False))
            return [], True, consts, bound
        if cls == "single":
            p, u, b, w, l = (op >> 24) & 1, (op >> 23) & 1, (op >> 22) & 1, (op >> 21) & 1, (op >> 20) & 1
            rn, rd = (op >> 16) & 15, (op >> 12) & 15
            if l:
                v = None
                if rn == 15 and not (op & (1 << 25)) and p and not w:
                    addr = (a + 8 + (op & 0xFFF)) if u else (a + 8 - (op & 0xFFF))
                    if not b and not (addr & 3) and self.in_rom(addr, 4):
                        v = self.rom.u32(addr)
                        self.pool_word(f, addr, v)
                if rd == 15:
                    succ = []
                    if v is not None:
                        succ = [v & ~3]
                    elif rn == 15 and (op & (1 << 25)) and (op & 0xFF0) == 0x100 and p and bound:
                        # ldrls pc, [pc, rN, lsl #2] ; b default ; .word case0, case1 ...
                        n = min(bound[1] + 1, TABLE_MAX)
                        words = [self.rom.u32(a + 8 + 4 * i) for i in range(n)]
                        if all(x is not None and self.in_rom(x & ~3, 4) for x in words):
                            self.mark_data(a + 8, 4 * n)
                            f.data.add((a + 8, 4 * n))
                            succ = [x & ~3 for x in words]
                    return succ, not al, consts, bound
                if v is None:
                    consts.pop(rd, None)
                else:
                    consts[rd] = v
            else:
                base = consts.get(rn)
                off = op & 0xFFF if not (op & (1 << 25)) else None
                if base is not None and off is not None and not b:
                    addr = (base + off if u else base - off) if p else base
                    if addr in IRQ_VECTOR and rd in consts and self.in_rom(consts[rd] & ~3, 4):
                        f.weak.add((consts[rd] & ~3, False))
            if (not p) or w:
                consts.pop(rn, None)
            return [], True, consts, bound
        if cls == "half":
            if (op >> 20) & 1:
                rd = (op >> 12) & 15
                consts.pop(rd, None)
                if rd == 15:
                    return [], not al, consts, bound
            if not ((op >> 24) & 1) or ((op >> 21) & 1):
                consts.pop((op >> 16) & 15, None)
            return [], True, consts, bound
        if cls == "block":
            lst, l = op & 0xFFFF, (op >> 20) & 1
            if (op >> 21) & 1:
                consts.pop((op >> 16) & 15, None)
            if l:
                for r in range(16):
                    if lst >> r & 1:
                        consts.pop(r, None)
                if lst & 0x8000:
                    return [], not al, consts, bound
            return [], True, consts, bound
        if cls in ("mul",):
            consts.pop((op >> 16) & 15, None)
        elif cls == "mull":
            consts.pop((op >> 16) & 15, None)
            consts.pop((op >> 12) & 15, None)
        elif cls in ("mrs", "swp"):
            consts.pop((op >> 12) & 15, None)
        return [], True, consts, bound

    def arm_dp_value(self, op, a, consts):
        """Value an ARM data op writes when its operands are known (MOV / ADD / SUB / ORR immediate)."""
        if not (op & (1 << 25)):
            if ((op >> 21) & 15) == 0xD and (op & 0xFF0) == 0:  # mov rd, rm
                rm = op & 15
                return (a + 8) if rm == 15 else consts.get(rm)
            return None
        opc, rn = (op >> 21) & 15, (op >> 16) & 15
        imm = ror(op & 0xFF, (op >> 7) & 0x1E)
        if opc == 0xD:
            return imm
        base = (a + 8) if rn == 15 else consts.get(rn)
        if base is None:
            return None
        if opc == 0x4:
            return (base + imm) & 0xFFFFFFFF
        if opc == 0x2:
            return (base - imm) & 0xFFFFFFFF
        if opc == 0xC:
            return base | imm
        return None

    def pool_word(self, f, addr, v):
        """A literal pool word at addr: data from now on (unless this very path runs it as code: a load of an
        opcode); a Thumb code pointer (bit 0 set) is a weak entry."""
        if not any(x in f.addrs for x in range(addr, addr + 4, 2)):
            self.mark_data(addr, 4)
            f.data.add((addr, 4))
        if v & 1 and self.in_rom(v & ~1, 2):
            f.weak.add((v & ~1, True))

    def step_thumb(self, f, a, consts, bound, strict):
        op = self.rom.u16(a)
        cls = thumb_class(op)
        consts = dict(consts)
        if cls == "b":
            return [(a + 4 + sext(op & 0x7FF, 11) * 2) & 0xFFFFFFFF], False, consts, bound
        if cls == "bcond":
            return [(a + 4 + sext(op & 0xFF, 8) * 2) & 0xFFFFFFFF], True, consts, bound
        if cls == "blhi":
            nxt = self.rom.u16(a + 2)
            if nxt is None or thumb_class(nxt) != "bllo":
                if strict:
                    f.bad = "torn BL pair at 0x%08X" % a
                return [], True, consts, bound
            return [], True, consts, bound
        if cls == "bllo":
            prev = self.rom.u16(a - 2)
            if prev is not None and thumb_class(prev) == "blhi":
                t = thumb_bl_target(prev, op, a - 2)
                kind = self.case_helper(t) if self.in_rom(t, 2) else None
                if kind:
                    n = None if bound is None else bound[1] + 1
                    if n is not None and n <= TABLE_MAX:
                        tab = self.case_table(kind, a + 2, n)
                        if tab:
                            targets, (start, length) = tab
                            self.mark_data(start, length)
                            f.data.add((start, length))
                            f.calls.add((t, True))
                            return [x for x in targets if self.in_rom(x, 2)], False, {}, None
                    f.calls.add((t, True))
                    return [], False, {}, None  # a table we cannot bound: the discovery loop will find the cases
                f.calls.add((t, True))
                # libgcc _call_via_rN (`bx rN`): an indirect call through a register we may know
                first = self.rom.u16(t) if self.in_rom(t, 2) else None
                if first is not None and (first & 0xFF87) == 0x4700:
                    v = consts.get((first >> 3) & 15)
                    if v is not None and self.in_rom(v & ~1, 2):
                        f.calls.add(((v & ~1) if v & 1 else (v & ~3), bool(v & 1)))
            for r in (0, 1, 2, 3, 12, 14):
                consts.pop(r, None)
            return [], True, consts, None
        if cls == "undef":
            if strict:
                f.bad = "undefined Thumb encoding at 0x%08X" % a
            return [], False, consts, bound
        if cls == "swi":
            if strict and (op & 0xFF) > 0x2B:
                f.bad = "odd SWI at 0x%08X" % a
            return [], True, {}, bound
        if cls == "hireg":
            rd, rs, k = (op & 7) | ((op >> 4) & 8), (op >> 3) & 15, (op >> 8) & 3
            src = (a + 4) if rs == 15 else consts.get(rs)
            if k == 3:  # BX
                if src is not None and self.in_rom(src & ~1):
                    f.calls.add(((src & ~1) if src & 1 else (src & ~3), bool(src & 1)))
                return [], self.rom.u16(a - 2) == 0x46FE, consts, bound  # mov lr, pc ; bx rN: a call
            if rd == 15 and k in (0, 2):
                if k == 2 and src is not None:
                    return [src & ~1], False, consts, bound
                if k == 0 and src is not None:  # add pc, rN with rN known
                    return [(a + 4 + src) & 0xFFFFFFFE], False, consts, bound
                return self.thumb_table(f, a, consts, bound), False, consts, bound
            if k == 2:
                if src is None:
                    consts.pop(rd, None)
                else:
                    consts[rd] = src
            elif k == 0:
                d = (a + 4) if rd == 15 else consts.get(rd)
                if d is not None and src is not None:
                    consts[rd] = (d + src) & 0xFFFFFFFF
                else:
                    consts.pop(rd, None)
            return [], True, consts, bound
        if cls == "imm8":
            k, rd, imm = op >> 11, (op >> 8) & 7, op & 0xFF
            if k == 4:
                consts[rd] = imm
            elif k == 5:
                bound = (rd, imm)
            elif k == 6 and rd in consts:
                consts[rd] = (consts[rd] + imm) & 0xFFFFFFFF
            else:
                consts.pop(rd, None)
            return [], True, consts, bound
        if cls == "ldrpc":
            addr = ((a + 4) & ~2) + (op & 0xFF) * 4
            rd = (op >> 8) & 7
            if self.in_rom(addr, 4):
                v = self.rom.u32(addr)
                self.pool_word(f, addr, v)
                consts[rd] = v
                consts["pool"] = consts.get("pool", ())[-3:] + (v,)  # table bases, even once overwritten
            else:
                consts.pop(rd, None)
            return [], True, consts, bound
        if cls == "addpc":
            v = ((a + 4) & ~2) + (op & 0xFF) * 4
            consts[(op >> 8) & 7] = v
            if self.in_rom(v, 2):
                f.weak.add((v, True))  # adr: often data, so only kept when it decodes cleanly
            return [], True, consts, bound
        if cls == "pushpop":
            if op & 0x800:
                for r in range(8):
                    if op >> r & 1:
                        consts.pop(r, None)
                if op & 0x100:
                    return [], False, consts, bound
            return [], True, consts, bound
        if cls in ("immoff", "regoff", "sprel", "half"):
            if cls == "immoff" and not (op & 0x800) and not (op & 0x1000):  # str rd, [rb, #imm]
                base = consts.get((op >> 3) & 7)
                rd = op & 7
                if base is not None and base + ((op >> 6) & 31) * 4 in IRQ_VECTOR and rd in consts:
                    v = consts[rd]
                    if self.in_rom(v & ~3, 4):
                        f.weak.add(((v & ~1) if v & 1 else (v & ~3), bool(v & 1)))
            loads = (cls == "regoff" and ((op >> 9) & 7) >= 3) or (cls != "regoff" and op & 0x800)
            if loads:
                consts.pop((op >> 8) & 7 if cls == "sprel" else op & 7, None)
            return [], True, consts, bound
        if cls == "ldmstm":
            if op & 0x800:
                for r in range(8):
                    if op >> r & 1:
                        consts.pop(r, None)
            consts.pop((op >> 8) & 7, None)
            return [], True, consts, bound
        if cls == "spadj":
            return [], True, consts, bound
        # shift, addsub, alu, addsp: rd = low register written
        rd = (op >> 8) & 7 if cls == "addsp" else op & 7
        if cls == "addsub" and (op & 0x400) and ((op >> 3) & 7) in consts and not (op & 0x200):
            consts[rd] = (consts[(op >> 3) & 7] + ((op >> 6) & 7)) & 0xFFFFFFFF
        elif cls == "shift" and (op >> 11) == 0 and ((op >> 3) & 7) in consts:
            consts[rd] = (consts[(op >> 3) & 7] << ((op >> 6) & 31)) & 0xFFFFFFFF
        elif cls == "alu" and ((op >> 6) & 15) in (8, 0xA, 0xB):
            pass  # TST / CMP / CMN write nothing
        else:
            consts.pop(rd, None)
        return [], True, consts, bound

    def thumb_table(self, f, a, consts, bound):
        """`mov pc, rN` / `add pc, rN` through a word table: cases when a table base is known and a bound too."""
        if bound is None:
            return []
        n = bound[1] + 1
        if n > TABLE_MAX:
            return []
        values = [v for k, v in consts.items() if isinstance(k, int)] + list(consts.get("pool", ()))
        for base in sorted(set(values)):
            if not (self.in_rom(base, 4) and not (base & 3) and abs(base - a) < 0x10000 and base > a):
                continue
            words = [self.rom.u32(base + 4 * i) for i in range(n)]
            if all(w is not None and self.in_rom(w & ~1, 2) and abs((w & ~1) - a) < 0x10000 for w in words):
                self.mark_data(base, 4 * n)
                f.data.add((base, 4 * n))
                return [w & ~1 for w in words]
        return []

    # ---- the whole ROM -------------------------------------------------------------------------------------

    def add_entry(self, addr, thumb, why):
        key = addr | int(thumb)
        if key not in self.entries:
            self.entries[key] = why
            return True
        return False

    def run(self):
        rom = self.rom
        self.add_entry(ROM_BASE, False, "entry")
        for a in self.discovered:
            addr, thumb = a & ~1, bool(a & 1)
            if self.in_rom(addr, 2 if thumb else 4) and not addr & (1 if thumb else 3):
                self.add_entry(addr, thumb, "discovered")
        # Pass 1: every entry, its calls (strong) and its code pointers (weak, validated).
        done = set()
        weak_todo = []
        while True:
            pending = [k for k in self.entries if k not in done]
            if not pending:
                if not weak_todo:
                    break
                addr, thumb = weak_todo.pop()
                key = addr | int(thumb)
                if key in self.entries or key in self.rejected:
                    continue
                g = self.explore(addr, thumb, strict=True, cap=MAX_WEAK_INSNS)
                if g.bad:
                    self.rejected[key] = g.bad
                    continue
                self.add_entry(addr, thumb, "weak")
                continue
            for key in sorted(pending):
                done.add(key)
                f = self.explore(key & ~1, bool(key & 1))
                for (t, th) in sorted(f.calls):
                    if self.in_rom(t, 2 if th else 4) and not t & (1 if th else 3):
                        self.add_entry(t, th, "call")
                weak_todo.extend(sorted(f.weak))
                for t in f.frontier:
                    self.add_entry(t, bool(key & 1), "split")
        # Pass 2: final bodies, cut at the data barriers and at other functions' entries. Code two functions both
        # reach (a shared tail: an error path, a merged epilogue) becomes a function of its own, so it is lifted once.
        for _ in range(8):
            self.final_bodies()
            owners = {}
            for f in self.functions:
                for a in f.addrs:
                    k = a | int(f.thumb)
                    owners[k] = owners.get(k, 0) + 1
            shared = set()
            for f in self.functions:
                size = 2 if f.thumb else 4
                for a in f.addrs:
                    k = a | int(f.thumb)
                    if owners[k] > 1 and (owners.get((a - size) | int(f.thumb), 0) < 2 or (a - size) not in f.addrs):
                        shared.add(k)
            shared -= set(self.entries)
            if not shared:
                break
            for k in shared:
                self.entries[k] = "shared"
        return self.functions

    def final_bodies(self):
        stops = set(self.entries)
        self.functions = []
        queue = sorted(self.entries)
        seen = set()
        while queue:
            key = queue.pop(0)
            if key in seen:
                continue
            seen.add(key)
            f = self.explore(key & ~1, bool(key & 1), stops=stops)
            for t in sorted(f.frontier):
                k = t | (key & 1)
                if k not in self.entries:
                    self.entries[k] = "split"
                    stops.add(k)
                    queue.append(k)
            if f.addrs:
                self.functions.append(f)
        self.functions.sort(key=lambda f: f.key)

    def stats(self) -> dict:
        kinds = {}
        for why in self.entries.values():
            kinds[why] = kinds.get(why, 0) + 1
        return {
            "functions": len(self.functions),
            "thumb_functions": sum(1 for f in self.functions if f.thumb),
            "instructions": sum(len(f.addrs) for f in self.functions),
            "entries_by_origin": kinds,
            "weak_rejected": len(self.rejected),
            "data_bytes": len(self.data),
        }


def islands(functions):
    """Table entries: (addr, thumb, function) at the start of each contiguous run of a function's instructions,
    runs cut so that no two overlap (the core finds the function holding a PC as the run at or before it)."""
    runs = []
    for f in functions:
        size = 2 if f.thumb else 4
        addrs = sorted(f.addrs)
        start = prev = addrs[0]
        for x in addrs[1:]:
            if x != prev + size:
                runs.append((start, prev + size, f))
                start = x
            prev = x
        runs.append((start, prev + size, f))
    out = []
    for thumb in (False, True):
        rs = sorted((r for r in runs if r[2].thumb == thumb), key=lambda r: (r[0], -r[1]))
        end = 0
        for (s, e, f) in rs:
            if e <= end:
                continue  # inside an earlier run: that function resumes there
            s = max(s, end)
            out.append((s, thumb, f))
            end = e
    out.sort(key=lambda t: (t[0], t[1]))
    return out
