"""Function discovery for PS1 code (XBA-160).

Code lives in "images": the EXE's text at its load address, and blobs of code the runtime recorded from RAM (overlays
and anything else not in the EXE: the discovered file of ps1_recomp.h). For each image:

1. Seeds: the EXE entry, the addresses of the discovered file, JAL targets and tail-call J targets found while
   walking functions, code addresses built in registers (LUI + ADDIU / ORI) and code pointers in data (words that
   point at a plausible function start: an ADDIU sp, sp, -N or the instruction after a JR ra + delay slot).
2. Extent of a function: a linear walk from its start that ends after the delay slot of an unconditional transfer
   (J, JR, B) once no branch of the function points further, or at the next known start (or at an invalid word).
   JR through a jump table (LUI / ADDU / LW / JR, bound from SLTIU when present) adds the table's targets.
3. BIOS call stubs (`li t2, 0xA0|0xB0|0xC0 ; jr t2 ; li t1, N`, Psy-Q's and PSn00bSDK's idiom) are recognised and
   named (bios_A0_3C ...); they stay ordinary calls: the lifted stub jumps to the BIOS table, which the interpreter
   runs.
"""

import struct

from .isa import RA, SP, T1, T2, decode

MAX_TABLE = 512


class Image:
    """Bytes of code at a virtual address."""

    def __init__(self, base, data, origin):
        self.base = base
        self.data = bytes(data[: len(data) & ~3])
        self.end = base + len(self.data)
        self.origin = origin  # "exe" or "ram"
        self._cache = {}

    def contains(self, addr):
        return self.base <= addr < self.end and not (addr & 3)

    def word(self, addr):
        return struct.unpack_from("<I", self.data, addr - self.base)[0]

    def ins(self, addr):
        i = self._cache.get(addr)
        if i is None:
            i = self._cache[addr] = decode(self.word(addr), addr)
        return i

    def words(self, start, end):
        return list(struct.unpack_from("<%dI" % ((end - start) // 4), self.data, start - self.base))


class Function:
    __slots__ = ("image", "start", "end", "name", "kind", "hash")

    def __init__(self, image, start, end):
        self.image = image
        self.start = start
        self.end = end
        self.name = None
        self.kind = "code"
        self.hash = recomp_hash(image.words(start, end))

    @property
    def addrs(self):
        return range(self.start, self.end, 4)


def recomp_hash(words):
    """ps1_recomp_hash (ps1_recomp.h): FNV-1a 64 over 32-bit words."""
    h = 0xCBF29CE484222325
    for w in words:
        h = ((h ^ w) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def plausible_code(img, addr):
    """Does addr look like an instruction stream (no reserved encoding in its first instructions)?"""
    for k in range(4):
        a = addr + 4 * k
        if not img.contains(a):
            return k > 0
        if img.ins(a).name in ("reserved", "cop1", "cop3"):
            return False
    return True


def looks_like_start(img, addr):
    """A pointer target that is likely a function: a stack frame setup, or right after a JR ra and its slot."""
    if not img.contains(addr) or not plausible_code(img, addr):
        return False
    i = img.ins(addr)
    if i.name == "addiu" and i.rt == SP and i.rs == SP and i.simm < 0:
        return True
    if img.contains(addr - 8):
        p = img.ins(addr - 8)
        if p.name == "jr" and p.rs == RA:
            return True
    return False


def bios_stub(img, start):
    """'A0_3C' when start is a BIOS call stub, else None."""
    if not img.contains(start + 8):
        return None
    a, b, c = img.ins(start), img.ins(start + 4), img.ins(start + 8)
    tab = fn = None
    for i in (a, c):
        if i.name in ("addiu", "ori") and i.rs == 0:
            if i.rt == T2 and i.imm in (0xA0, 0xB0, 0xC0):
                tab = i.imm
            elif i.rt == T1:
                fn = i.imm
    if b.name == "jr" and b.rs == T2 and tab is not None and fn is not None:
        return "%02X_%02X" % (tab, fn & 0xFF)
    return None


class Discovery:
    def __init__(self, img, seeds, fixed_starts=()):
        self.img = img
        self.seeds = set(a for a in seeds if img.contains(a))
        self.starts = set(self.seeds) | set(a for a in fixed_starts if img.contains(a))
        self.tables = {}  # jump table address -> entries
        self.stats = {"seeds": len(self.seeds), "jal": 0, "tail": 0, "pointers": 0, "jump_tables": 0,
                      "bios_stubs": 0}

    # ---- one function's walk ----
    def _walk(self, start, limit, found):
        """Returns the end (exclusive) of the function at start; adds call targets etc. to found."""
        img = self.img
        furthest = start
        pc = start
        his = {}  # register -> LUI immediate
        while pc < limit and img.contains(pc):
            i = img.ins(pc)
            if i.name == "reserved":
                return pc
            if i.cls == "branch":
                t = i.target
                if start <= t < limit:
                    furthest = max(furthest, t)
                # a conditional branch out of the function is no call: not a seed
            elif i.cls == "jump":
                t = i.target
                if i.name == "jal":
                    if img.contains(t):
                        found.add(("jal", t))
                elif start <= t < limit and (t <= pc or t <= furthest + 64 or t < pc + 256):
                    furthest = max(furthest, t)
                elif img.contains(t):
                    found.add(("tail", t))
            elif i.name == "jr" and i.rs != RA:
                for t in self._jump_table(start, pc, i.rs):
                    if start <= t < limit:
                        furthest = max(furthest, t)
            if i.name == "lui":
                his[i.rt] = i.imm
            elif i.name in ("addiu", "ori") and i.rs in his:
                # LUI rX, hi ... ADDIU / ORI rY, rX, lo: an address built in a register (a function pointer?)
                v = ((his[i.rs] << 16) + (i.simm if i.name == "addiu" else i.imm)) & 0xFFFFFFFF
                if img.contains(v) and looks_like_start(img, v):
                    found.add(("ptr", v))
            d = i.dest()
            if d is not None and i.name != "lui":
                his.pop(d, None)
            if i.ldest() is not None:
                his.pop(i.ldest(), None)
            if i.is_transfer() and i.is_unconditional() and i.name not in ("jal", "jalr", "bgezal"):
                if furthest <= pc + 4:
                    end = pc + 8
                    return end if img.contains(pc + 4) else pc + 4
            pc += 4
        end = min(pc, limit)
        # never cut between a branch and its delay slot
        if end - 4 >= start and img.contains(end - 4) and img.ins(end - 4).is_transfer() and img.contains(end):
            end += 4
        return end

    def _jump_table(self, start, pc, reg):
        """Targets of a JR reg through a table (LUI hi ; ADDU ; LW reg, lo(base) ; JR reg), or []."""
        img = self.img
        lw = None
        a = pc - 4
        while a >= max(start, pc - 64):
            i = img.ins(a)
            if i.name == "lw" and i.rt == reg:
                lw = (a, i)
                break
            if i.dest() == reg:
                return []
            a -= 4
        if not lw:
            return []
        base, off = lw[1].rs, lw[1].simm
        srcs = {base}
        hi = None
        bound = None
        a = lw[0] - 4
        while a >= max(start, lw[0] - 64):
            i = img.ins(a)
            if i.name == "lui" and i.rt in srcs and hi is None:
                hi = i.imm
            elif i.name == "addu" and i.rd in srcs:
                srcs |= {i.rs, i.rt}
            elif i.name == "addiu" and i.rt in srcs and i.rs == i.rt and hi is None:
                off += i.simm  # LUI r, hi ; ADDIU r, r, lo ; ADDU ; LW 0(r)
            elif i.name == "sltiu" and bound is None:
                bound = i.imm
            a -= 4
        if hi is None:
            return []
        table = ((hi << 16) + off) & 0xFFFFFFFF
        n = bound if bound and bound <= MAX_TABLE else MAX_TABLE
        out = []
        for k in range(n):
            e = table + 4 * k
            if not img.contains(e):
                break
            t = img.word(e)
            if not img.contains(t) or t < start:
                break
            out.append(t)
        if out:
            if table not in self.tables:
                self.stats["jump_tables"] += 1
            self.tables[table] = len(out)
        return out

    def _pointer_scan(self):
        img = self.img
        out = set()
        in_table = set()
        for t, n in self.tables.items():
            in_table.update(range(t, t + 4 * n, 4))
        for off in range(0, len(img.data), 4):
            if img.base + off in in_table:
                continue  # jump table entries are case labels, not functions
            v = struct.unpack_from("<I", img.data, off)[0]
            if img.contains(v) and v not in self.starts and looks_like_start(img, v):
                out.add(v)
        return out

    def _explore(self, work, done):
        img = self.img
        while work:
            s = work.pop()
            if s in done:
                continue
            done.add(s)
            found = set()
            self._walk(s, img.end, found)
            for kind, t in found:
                if t not in self.starts and plausible_code(img, t):
                    self.starts.add(t)
                    self.stats[kind if kind in ("jal", "tail") else "pointers"] += 1
                    work.append(t)

    def run(self):
        img = self.img
        done = set()
        # from the seeds first (this finds the jump tables), then from the code pointers in data
        self._explore(sorted(self.starts), done)
        ptrs = self._pointer_scan()
        self.stats["pointers"] += len(ptrs)
        self.starts |= ptrs
        self._explore(sorted(ptrs), done)
        funcs = []
        starts = sorted(self.starts)
        for k, s in enumerate(starts):
            limit = starts[k + 1] if k + 1 < len(starts) else img.end
            end = self._walk(s, limit, set())
            if end <= s:
                continue
            f = Function(img, s, end)
            stub = bios_stub(img, s)
            if stub:
                f.kind = "bios_stub"
                f.name = "bios_" + stub
                self.stats["bios_stubs"] += 1
            funcs.append(f)
        return funcs


def merge_records(records):
    """Recorded RAM code (addr, bytes) merged into blobs: records that overlap with the same bytes become one.
    Returns [(base, bytes, [seed addresses])]."""
    blobs = []
    for addr, data in sorted(records, key=lambda r: (r[0], -len(r[1]))):
        merged = False
        for b in blobs:
            base, buf, seeds = b
            lo, hi = max(base, addr), min(base + len(buf), addr + len(data))
            if lo <= hi and buf[lo - base:hi - base] == data[lo - addr:hi - addr]:
                nb = min(base, addr)
                ne = max(base + len(buf), addr + len(data))
                out = bytearray(ne - nb)
                out[addr - nb:addr - nb + len(data)] = data
                out[base - nb:base - nb + len(buf)] = buf
                b[0], b[1] = nb, bytes(out)
                seeds.append(addr)
                merged = True
                break
        if not merged:
            blobs.append([addr, bytes(data), [addr]])
    return [tuple(b) for b in blobs]
