"""ARM / Thumb to C (XBA-151): one C function per discovered function, on the ABI of gba_recomp_rt.h.

Every instruction is emitted from its opcode bits with the interpreter's semantics (consoles/gba/runtime/gba_cpu.c):
same results, same flags, same cycles in the same order (fetch, data accesses, internal cycles, refill). The function
is cut into blocks, each reachable from its entry switch, with one event check per segment (« blocks and the event
checks » below): lifted code stops where the interpreter would (scheduler event, IRQ) and the interpreter runs the
rest of that block. Instructions the interpreter handles better (SWI, exception returns, LDM / STM with the S
bit, SWP, undefined encodings, odd PC write-backs) return to the dispatcher with GBA_RC_EXIT_INTERP.
"""

import re

from .isa import arm_class, ror, sext, thumb_class, disasm

COND = {
    0x0: "fZ",
    0x1: "!fZ",
    0x2: "fC",
    0x3: "!fC",
    0x4: "fN",
    0x5: "!fN",
    0x6: "fV",
    0x7: "!fV",
    0x8: "(fC && !fZ)",
    0x9: "(!fC || fZ)",
    0xA: "(fN == fV)",
    0xB: "(fN != fV)",
    0xC: "(!fZ && fN == fV)",
    0xD: "(fZ || fN != fV)",
}


def hx(v: int) -> str:
    return "0x%08Xu" % (v & 0xFFFFFFFF)


def label(addr: int) -> str:
    return "L_%08X" % addr


class FnLifter:
    """Lifts the instructions of one function (one state) into C statements."""

    def __init__(self, rom, thumb: bool, addrs, seg_resume: bool = False):
        self.rom = rom
        self.seg_resume = seg_resume  # every segment start is a resume point too (« blocks and the event checks »)
        self.lift_psr = seg_resume  # MRS SPSR and MSR lifted (arm_msr); the GBA's packs, both from XBA-281
        self.fetch_runs = seg_resume  # one RC_IN for a run of plain instructions' fetches (fetch_runs, XBA-295)
        self.thumb = thumb
        self.addrs = set(addrs)
        self.pipe = 4 if thumb else 8
        self.fallbacks = 0
        self.targets = set()  # addresses of this function reached by a local jump

    # ---- operands -----------------------------------------------------------------------------------------

    @staticmethod
    def reg(n: int, pcval: int) -> str:
        return hx(pcval) if n == 15 else "r%d" % n

    def jump(self, target) -> str:
        """Write to PC in the current state: target is an int (constant) or a C expression."""
        if isinstance(target, int):
            t = target & (0xFFFFFFFE if self.thumb else 0xFFFFFFFC)
            if t in self.addrs:
                self.targets.add(t)
                return "RC_JUMP_LOCAL(%s, %s);" % (hx(t), label(t))
            return "RC_JUMP_DYN(%s);" % hx(t)
        return "RC_JUMP_DYN(%s);" % target

    def fold_literal(self, addr: int, size: int):
        """Value of a cartridge literal (regions 0x08-0x0C, inside the image), else None."""
        if not (0x08000000 <= addr < 0x0D000000):
            return None
        off = addr & 0x1FFFFFF
        if size == 4:
            a = off & ~3
            if a + 4 > self.rom.size:
                return None
            return int.from_bytes(self.rom.data[a:a + 4], "little")
        if off >= self.rom.size:
            return None
        return self.rom.data[off]

    @staticmethod
    def adc(x: str, y: str, cin: str, flags: bool, res: str = "res_") -> list:
        """x + y + cin and its NZCV; plain 32-bit forms for ADD (cin 0) and SUB (y = ~z, cin 1)."""
        if cin == "0":
            out = ["uint32_t x_ = %s, y_ = %s;" % (x, y), "%s = x_ + y_;" % res]
            if flags:
                out.append("fN = %s >> 31; fZ = %s == 0; fC = %s < x_; fV = (~(x_ ^ y_) & (x_ ^ %s)) >> 31;"
                           % (res, res, res, res))
            return out
        z = None
        if cin == "1" and y.startswith("~"):
            z = y[1:]
        elif cin == "1" and re.fullmatch(r"0x[0-9A-F]{8}u", y):
            z = hx(~int(y[2:10], 16))
        if z is not None:
            out = ["uint32_t x_ = %s, y_ = %s;" % (x, z), "%s = x_ - y_;" % res]
            if flags:
                out.append("fN = %s >> 31; fZ = %s == 0; fC = x_ >= y_; fV = ((x_ ^ y_) & (x_ ^ %s)) >> 31;"
                           % (res, res, res))
            return out
        out = ["uint32_t x_ = %s, y_ = %s;" % (x, y), "uint64_t w_ = (uint64_t)x_ + y_ + %s;" % cin,
               "%s = (uint32_t)w_;" % res]
        if flags:
            out.append("fN = %s >> 31; fZ = %s == 0; fC = (uint32_t)(w_ >> 32); fV = (~(x_ ^ y_) & (x_ ^ %s)) >> 31;"
                       % (res, res, res))
        return out

    def shift_imm(self, typ: int, v: str, amt: int, want_carry: bool):
        """(value expr, carry expr) of a shift by an immediate (LSR/ASR #0 = #32, ROR #0 = RRX)."""
        if typ == 0:
            if amt == 0:
                return v, "fC"
            return "(%s << %d)" % (v, amt), "((%s >> %d) & 1)" % (v, 32 - amt)
        if typ == 1:
            if amt == 0:
                return "0u", "(%s >> 31)" % v
            return "(%s >> %d)" % (v, amt), "((%s >> %d) & 1)" % (v, amt - 1)
        if typ == 2:
            if amt == 0:
                return "(uint32_t)((int32_t)%s >> 31)" % v, "(%s >> 31)" % v
            return "(uint32_t)((int32_t)%s >> %d)" % (v, amt), "((%s >> %d) & 1)" % (v, amt - 1)
        if amt == 0:
            return "(fC << 31 | %s >> 1)" % v, "(%s & 1)" % v
        return "rc_ror(%s, %d)" % (v, amt), "((%s >> %d) & 1)" % (v, amt - 1)

    # ---- ARM ----------------------------------------------------------------------------------------------

    def arm(self, op: int, a: int) -> list:
        """Statements for the ARM instruction op at a (after RC_I), or None: run by the interpreter."""
        cond = op >> 28
        if cond == 0xF:
            return []  # NV: never executes on ARMv4
        cls = arm_class(op)
        fn = getattr(self, "arm_" + cls, None)
        body = fn(op, a) if fn else None
        if body is None:
            return None
        body = self.after_access(body)
        if cond == 0xE:
            return body
        return ["if (%s) {" % COND[cond]] + ["    " + s for s in body] + ["}"]

    def arm_dp(self, op, a):
        opc = (op >> 21) & 15
        s = (op >> 20) & 1
        rn = (op >> 16) & 15
        rd = (op >> 12) & 15
        write = not (8 <= opc <= 11)
        logical = opc in (0, 1, 8, 9, 12, 13, 14, 15)
        if rd == 15 and (not write or s):
            return None  # MOVS pc / SUBS pc (CPSR = SPSR), TSTP and friends
        out = []
        need_sc = logical and s
        if op & (1 << 25):
            rot = (op >> 7) & 0x1E
            imm = ror(op & 0xFF, rot)
            o2 = hx(imm)
            sc = str(imm >> 31) if rot else "fC"
            A = self.reg(rn, a + 8)
        elif op & 0x10:
            rm, rs = op & 15, (op >> 8) & 15
            out.append("uint32_t sc_;")
            out.append("uint32_t o2_ = rc_shift_reg(%d, %s, %s & 0xFF, fC, &sc_);"
                       % ((op >> 5) & 3, self.reg(rm, a + 12), self.reg(rs, a + 8)))
            out.append("now += 1;")
            o2, sc = "o2_", "sc_"
            A = self.reg(rn, a + 12)
        else:
            v, c = self.shift_imm((op >> 5) & 3, self.reg(op & 15, a + 8), (op >> 7) & 31, need_sc)
            out.append("uint32_t o2_ = %s;" % v)
            o2 = "o2_"
            sc = c
            if need_sc:
                out.append("uint32_t sc_ = %s;" % c)
                sc = "sc_"
            A = self.reg(rn, a + 8)
        out.append("uint32_t res_;")
        aflags = (s and rd != 15) if write else True
        if opc == 0x0:
            out.append("res_ = %s & %s;" % (A, o2))
        elif opc == 0x1:
            out.append("res_ = %s ^ %s;" % (A, o2))
        elif opc == 0x2:
            out += self.adc(A, "~" + o2, "1", aflags)
        elif opc == 0x3:
            out += self.adc(o2, "~" + A, "1", aflags)
        elif opc == 0x4:
            out += self.adc(A, o2, "0", aflags)
        elif opc == 0x5:
            out += self.adc(A, o2, "fC", aflags)
        elif opc == 0x6:
            out += self.adc(A, "~" + o2, "fC", aflags)
        elif opc == 0x7:
            out += self.adc(o2, "~" + A, "fC", aflags)
        elif opc == 0x8:
            out.append("res_ = %s & %s;" % (A, o2))
        elif opc == 0x9:
            out.append("res_ = %s ^ %s;" % (A, o2))
        elif opc == 0xA:
            out += self.adc(A, "~" + o2, "1", True)
        elif opc == 0xB:
            out += self.adc(A, o2, "0", True)
        elif opc == 0xC:
            out.append("res_ = %s | %s;" % (A, o2))
        elif opc == 0xD:
            out.append("res_ = %s;" % o2)
        elif opc == 0xE:
            out.append("res_ = %s & ~%s;" % (A, o2))
        else:
            out.append("res_ = ~%s;" % o2)
        if logical and s:
            out.append("fN = res_ >> 31; fZ = res_ == 0; fC = %s;" % sc)
        if write:
            if rd == 15:
                const = self.arm_dp_const(op, a)
                out.append(self.jump(const if const is not None else "res_"))
            else:
                out.append("r%d = res_;" % rd)
        return out

    def arm_dp_const(self, op, a):
        """PC written with a constant (MOV pc, #imm; ADD/SUB pc, pc, #imm), else None."""
        if not (op & (1 << 25)):
            return None
        opc, rn = (op >> 21) & 15, (op >> 16) & 15
        imm = ror(op & 0xFF, (op >> 7) & 0x1E)
        if opc == 0xD:
            return imm
        if rn == 15 and opc == 0x4:
            return (a + 8 + imm) & 0xFFFFFFFF
        if rn == 15 and opc == 0x2:
            return (a + 8 - imm) & 0xFFFFFFFF
        return None

    def arm_mrs(self, op, a):
        rd = (op >> 12) & 15
        if rd == 15 or ((op >> 22) & 1 and not self.lift_psr):
            return None
        if (op >> 22) & 1:
            return ["r%d = RC_SPSR();" % rd]
        return ["r%d = fN << 31 | fZ << 30 | fC << 29 | fV << 28 | h->cpu.cpsr_ctl;" % rd]

    def arm_msr(self, op, a):
        """MSR (lift_psr): the SPSR inline; the CPSR inline, then a write to its control byte leaves (an IRQ it unmasked
        is the dispatcher's). gba_cpu.c arm_psr."""
        if not self.lift_psr:
            return None
        if op & (1 << 25):
            v = hx(ror(op & 0xFF, (op >> 7) & 0x1E))
        else:
            if op & 15 == 15:
                return None
            v = "r%d" % (op & 15)
        mask = 0
        for bit, m in ((19, 0xFF000000), (18, 0x00FF0000), (17, 0x0000FF00), (16, 0x000000FF)):
            if op >> bit & 1:
                mask |= m
        if (op >> 22) & 1:
            return ["RC_MSR_SPSR(%s, %s);" % (hx(mask), v)]
        out = ["RC_MSR_CPSR(%s, %s);" % (hx(mask), v)]
        if mask & 0xFF:
            out.append("RC_EXIT(%s);" % hx(a + 4))
        return out

    def arm_mul(self, op, a):
        rd, rn, rs, rm = (op >> 16) & 15, (op >> 12) & 15, (op >> 8) & 15, op & 15
        if rd == 15:
            return None
        s, acc = (op >> 20) & 1, (op >> 21) & 1
        out = ["uint32_t vs_ = %s;" % self.reg(rs, a + 8), "uint32_t res_ = %s * vs_;" % self.reg(rm, a + 8),
               "now += (uint32_t)rc_mul_cycles(vs_, 1);"]
        if acc:
            out += ["res_ += %s;" % self.reg(rn, a + 8), "now += 1;"]
        out.append("r%d = res_;" % rd)
        if s:
            out.append("fN = res_ >> 31; fZ = res_ == 0;")
        return out

    def arm_mull(self, op, a):
        hi, lo, rs, rm = (op >> 16) & 15, (op >> 12) & 15, (op >> 8) & 15, op & 15
        if hi == 15 or lo == 15:
            return None
        s, acc, sign = (op >> 20) & 1, (op >> 21) & 1, (op >> 22) & 1
        out = ["uint32_t vs_ = %s;" % self.reg(rs, a + 8), "uint32_t vm_ = %s;" % self.reg(rm, a + 8)]
        if sign:
            out.append("uint64_t res_ = (uint64_t)((int64_t)(int32_t)vm_ * (int32_t)vs_);")
        else:
            out.append("uint64_t res_ = (uint64_t)vm_ * vs_;")
        out.append("now += (uint32_t)rc_mul_cycles(vs_, %d) + 1;" % sign)
        if acc:
            out += ["res_ += (uint64_t)r%d << 32 | r%d;" % (hi, lo), "now += 1;"]
        out += ["r%d = (uint32_t)res_;" % lo, "r%d = (uint32_t)(res_ >> 32);" % hi]
        if s:
            out.append("fN = (uint32_t)(res_ >> 63); fZ = res_ == 0; fC = rc_mull_carry(vm_, vs_, %d);" % sign)
        return out

    def arm_bx(self, op, a):
        return ["RC_BX(%s);" % self.reg(op & 15, a + 8)]

    def arm_branch(self, op, a):
        t = (a + 8 + sext(op & 0xFFFFFF, 24) * 4) & 0xFFFFFFFF
        out = []
        if op & (1 << 24):
            out.append("r14 = %s;" % hx(a + 4))
        out.append(self.jump(t))
        return out

    @staticmethod
    def transfer(start: str, ops, ob: str) -> list:
        """A multiple transfer (LDM / STM / PUSH / POP): words at (start & ~3) + 4k, ops[k] = ("r", lvalue) or
        ("w", value). All of it in one RAM region: one check, inline accesses, the waits added at once; else every
        access through the bus, in order."""
        n = len(ops)
        out = ["uint32_t a0_ = (%s) & ~3u;" % start]
        for k, (kind, x) in enumerate(ops):
            if kind == "r" and x.startswith("uint32_t "):
                out.append(x + ";")
        fast, slow = [], []
        for k, (kind, x) in enumerate(ops):
            dst = x[len("uint32_t "):] if x.startswith("uint32_t ") else x
            ad = "a0_ + %d" % (4 * k) if k else "a0_"
            # sequential after the first, unless the run crosses into another region (gba_cpu.c block transfers)
            sq = "((a0_ + %d) >> 24) == ((a0_ + %d) >> 24)" % (4 * k, 4 * k - 4) if k else "0"
            if kind == "r":
                fast.append("%s = RC_FRD32(%s);" % (dst, ad))
                slow.append("%s = RC_BRD32(%s, %s, %s);" % (dst, ad, sq, ob))
            else:
                fast.append("RC_FWR32(%s, %s);" % (ad, x))
                slow.append("RC_BWR32(%s, %s, %s, %s);" % (ad, x, sq, ob))
        out.append("if (RC_RUN_RAM(a0_, %d)) {" % n)
        out.append("    RC_RUN_WAITS(a0_, %d);" % n)
        out.append("    RC_RUN_BASE(a0_);")
        out += ["    " + x for x in fast]
        out.append("} else {")
        out += ["    " + x for x in slow]
        out.append("}")
        return out

    def wb(self, rn):
        return "r%d = mv_;" % rn

    def arm_single(self, op, a):
        p, u, b, w, l = (op >> 24) & 1, (op >> 23) & 1, (op >> 22) & 1, (op >> 21) & 1, (op >> 20) & 1
        rn, rd = (op >> 16) & 15, (op >> 12) & 15
        writeback = (not p) or w
        if rn == 15 and writeback:
            return None
        ob = self.aob(a)
        if op & (1 << 25):
            off, _ = self.shift_imm((op >> 5) & 3, self.reg(op & 15, a + 8), (op >> 7) & 31, False)
        else:
            off = op & 0xFFF
            if rn == 15 and l:  # literal load: fold the value, keep the cycles
                addr = (a + 8 + off) if u else (a + 8 - off)
                v = self.fold_literal(addr, 1 if b else 4)
                if v is not None:
                    if not b:
                        v = ror(v, (addr & 3) * 8)
                    region = (addr >> 24) & 15
                    out = ["now += RC_LIT(h->%s[%d][0]);" % ("wait16" if b else "wait32", region), "now += 1;"]
                    out.append(self.jump(v) if rd == 15 else "r%d = %s;" % (rd, hx(v)))
                    return out
            off = hx(off)
        out = ["uint32_t b_ = %s;" % self.reg(rn, a + 8),
               "uint32_t mv_ = b_ %s %s;" % ("+" if u else "-", off),
               "uint32_t ad_ = %s;" % ("mv_" if p else "b_")]
        if l:
            if b:
                out.append("uint32_t v_ = RD8(ad_, 0, %s);" % ob)
            else:
                out.append("uint32_t v_ = rc_ror(RD32(ad_, 0, %s), (ad_ & 3) * 8);" % ob)
            out.append("now += 1;")
            if writeback:
                out.append(self.wb(rn))
            out.append(self.jump("v_") if rd == 15 else "r%d = v_;" % rd)
        else:
            out.append("uint32_t v_ = %s;" % self.reg(rd, a + 12))
            out.append(("WR8(ad_, v_, 0, %s);" if b else "WR32(ad_, v_, 0, %s);") % ob)
            if writeback:
                out.append(self.wb(rn))
        return out

    def arm_half(self, op, a):
        p, u, w, l = (op >> 24) & 1, (op >> 23) & 1, (op >> 21) & 1, (op >> 20) & 1
        rn, rd, sh = (op >> 16) & 15, (op >> 12) & 15, (op >> 5) & 3
        writeback = (not p) or w
        if rn == 15 and writeback:
            return None
        ob = self.aob(a)
        off = hx(((op >> 4) & 0xF0) | (op & 0xF)) if op & (1 << 22) else self.reg(op & 15, a + 8)
        out = ["uint32_t b_ = %s;" % self.reg(rn, a + 8),
               "uint32_t mv_ = b_ %s %s;" % ("+" if u else "-", off),
               "uint32_t ad_ = %s;" % ("mv_" if p else "b_")]
        if l:
            if sh == 1:
                out.append("uint32_t v_ = rc_ror(RD16(ad_, 0, %s), (ad_ & 1) * 8);" % ob)
            elif sh == 2:
                out.append("uint32_t v_ = (uint32_t)(int32_t)(int8_t)RD8(ad_, 0, %s);" % ob)
            else:
                out.append("uint32_t v_ = (ad_ & 1) ? (uint32_t)(int32_t)(int8_t)RD8(ad_, 0, %s)"
                           " : (uint32_t)(int32_t)(int16_t)RD16(ad_, 0, %s);" % (ob, ob))
            out.append("now += 1;")
            if writeback:
                out.append(self.wb(rn))
            out.append(self.jump("v_") if rd == 15 else "r%d = v_;" % rd)
        else:
            if sh == 1:
                out.append("WR16(ad_, %s, 0, %s);" % (self.reg(rd, a + 12), ob))
            if writeback:
                out.append(self.wb(rn))
        return out

    def arm_block(self, op, a):
        p, u, s, w, l = (op >> 24) & 1, (op >> 23) & 1, (op >> 22) & 1, (op >> 21) & 1, (op >> 20) & 1
        rn, lst = (op >> 16) & 15, op & 0xFFFF
        if s or rn == 15 or lst == 0:
            return None
        regs = [i for i in range(16) if lst >> i & 1]
        size = 4 * len(regs)
        ob = self.aob(a)
        out = ["uint32_t b_ = r%d;" % rn]
        if u:
            out.append("uint32_t ad_ = b_ + %d;" % (4 if p else 0))
            if w:
                out.append("uint32_t nb_ = b_ + %d;" % size)
        else:
            out.append("uint32_t ad_ = b_ - %d;" % (size - (0 if p else 4)))
            if w:
                out.append("uint32_t nb_ = b_ - %d;" % size)
        if l:
            if w and not (lst >> rn & 1):
                out.append("r%d = nb_;" % rn)
            out += self.transfer("ad_", [("r", "uint32_t pc_" if i == 15 else "r%d" % i) for i in regs], ob)
            out.append("now += 1;")
            if 15 in regs:
                out.append(self.jump("pc_"))
        else:
            first = regs[0]
            vals = []
            for i in regs:
                if i == 15:
                    vals.append(hx(a + 12))
                elif i == rn and w and i != first:
                    vals.append("nb_")
                else:
                    vals.append("r%d" % i)
            out += self.transfer("ad_", [("w", v) for v in vals], ob)
            if w:
                out.append("r%d = nb_;" % rn)
        return out

    # ---- Thumb --------------------------------------------------------------------------------------------

    def thumb_ins(self, op: int, a: int) -> list:
        cls = thumb_class(op)
        body = getattr(self, "t_" + cls)(op, a) if hasattr(self, "t_" + cls) else None
        return None if body is None else self.after_access(body)

    @staticmethod
    def after_access(body):
        # After cycles beyond the opcode fetch (a data access, internal cycles) the next opcode fetch is
        # non-sequential (gba_cpu.c step, gba_recomp_rt.h RC_NEXTRA): its extra cycles, unless the instruction jumps
        # (the refill is non-sequential anyway).
        if (is_memory(body) or any(NOW_RE.search(x) for x in body)) and not any("RC_JUMP" in x for x in body):
            return body + ["now += RC_NEXTRA();"]
        return body

    def tob(self, a):
        # Thumb in the cartridge ROM: the open-bus latch holds the prefetched opcode [$+4] in both halves.
        return hx(self.rom_half(a + 4) * 0x00010001)

    def rom_half(self, addr):
        # gba_bus.c rom_read16: past the end of the ROM, the cartridge bus answers the halfword address.
        off = addr & 0x1FFFFFE
        if off < self.rom.size:  # the image is padded with zeros to a word
            return int.from_bytes(bytes(self.rom.data[off:off + 2]).ljust(2, b"\0"), "little")
        return (off >> 1) & 0xFFFF

    def aob(self, a):
        # ARM in the cartridge ROM: the open-bus latch holds the prefetched opcode [$+8].
        return hx(self.rom_half(a + 8) | self.rom_half(a + 10) << 16)

    def t_shift(self, op, a):
        rd, rs, amt, typ = op & 7, (op >> 3) & 7, (op >> 6) & 31, op >> 11
        v, c = self.shift_imm(typ, "r%d" % rs, amt, True)
        return ["uint32_t c_ = %s;" % c, "uint32_t res_ = %s;" % v, "fN = res_ >> 31; fZ = res_ == 0; fC = c_;",
                "r%d = res_;" % rd]

    def t_addsub(self, op, a):
        rd, rs = op & 7, (op >> 3) & 7
        b = hx((op >> 6) & 7) if op & 0x400 else "r%d" % ((op >> 6) & 7)
        out = ["uint32_t res_;"]
        out += self.adc("r%d" % rs, "~" + b if op & 0x200 else b, "1" if op & 0x200 else "0", True)
        out.append("r%d = res_;" % rd)
        return out

    def t_imm8(self, op, a):
        k, rd, imm = op >> 11, (op >> 8) & 7, op & 0xFF
        if k == 4:
            return ["r%d = %s;" % (rd, hx(imm)), "fN = 0; fZ = %d;" % (imm == 0)]
        out = ["uint32_t res_;"]
        if k == 5:
            out += self.adc("r%d" % rd, hx(~imm), "1", True)
            return out
        if k == 6:
            out += self.adc("r%d" % rd, hx(imm), "0", True)
        else:
            out += self.adc("r%d" % rd, hx(~imm), "1", True)
        out.append("r%d = res_;" % rd)
        return out

    def t_alu(self, op, a):
        rd, rs, k = op & 7, (op >> 3) & 7, (op >> 6) & 15
        A, B = "r%d" % rd, "r%d" % rs
        nz = "fN = res_ >> 31; fZ = res_ == 0;"
        if k in (0, 1, 0xC, 0xE, 0xF):
            expr = {0: "%s & %s", 1: "%s ^ %s", 0xC: "%s | %s", 0xE: "%s & ~%s"}.get(k)
            e = expr % (A, B) if expr else "~%s" % B
            return ["uint32_t res_ = %s;" % e, nz, "%s = res_;" % A]
        if k in (2, 3, 4, 7):
            typ = {2: 0, 3: 1, 4: 2, 7: 3}[k]
            return ["uint32_t c_;", "uint32_t res_ = rc_shift_reg(%d, %s, %s & 0xFF, fC, &c_);" % (typ, A, B),
                    nz + " fC = c_;", "%s = res_;" % A, "now += 1;"]
        out = ["uint32_t res_;"]
        if k == 5:
            return out + self.adc(A, B, "fC", True) + ["%s = res_;" % A]
        if k == 6:
            return out + self.adc(A, "~" + B, "fC", True) + ["%s = res_;" % A]
        if k == 8:
            return ["uint32_t res_ = %s & %s;" % (A, B), nz]
        if k == 9:
            return out + self.adc("0u", "~" + B, "1", True) + ["%s = res_;" % A]
        if k == 0xA:
            return out + self.adc(A, "~" + B, "1", True)
        if k == 0xB:
            return out + self.adc(A, B, "0", True)
        # 0xD: MUL (cycles by the destination's old value)
        return ["now += (uint32_t)rc_mul_cycles(%s, 1);" % A, "uint32_t res_ = %s * %s;" % (A, B), nz,
                "%s = res_;" % A]

    def t_hireg(self, op, a):
        rd, rs, k = (op & 7) | ((op >> 4) & 8), (op >> 3) & 15, (op >> 8) & 3
        D, S = self.reg(rd, a + 4), self.reg(rs, a + 4)
        if k == 0:
            if rd == 15:
                if rs == 15:
                    return [self.jump((2 * (a + 4)) & 0xFFFFFFFF)]
                return ["uint32_t res_ = %s + %s;" % (D, S), self.jump("res_")]
            return ["r%d = %s + %s;" % (rd, D, S)]
        if k == 1:
            return ["uint32_t res_;"] + self.adc(D, "~" + S, "1", True)
        if k == 2:
            if rd == 15:
                return [self.jump(a + 4 if rs == 15 else S)]
            return [] if rd == rs else ["r%d = %s;" % (rd, S)]  # mov r8, r8: the nop
        return ["RC_BX(%s);" % S]

    def t_ldrpc(self, op, a):
        rd = (op >> 8) & 7
        addr = ((a + 4) & ~2) + (op & 0xFF) * 4
        v = self.fold_literal(addr, 4)
        if v is not None:
            return ["now += RC_LIT(h->wait32[%d][0]);" % ((addr >> 24) & 15), "r%d = %s;" % (rd, hx(v)), "now += 1;"]
        return ["r%d = RD32(%s, 0, %s);" % (rd, hx(addr), self.tob(a)), "now += 1;"]

    def t_regoff(self, op, a):
        rd, k = op & 7, (op >> 9) & 7
        ob = self.tob(a)
        out = ["uint32_t ad_ = r%d + r%d;" % ((op >> 3) & 7, (op >> 6) & 7)]
        D = "r%d" % rd
        if k == 0:
            out += ["WR32(ad_, %s, 0, %s);" % (D, ob)]
        elif k == 1:
            out += ["WR16(ad_, %s, 0, %s);" % (D, ob)]
        elif k == 2:
            out += ["WR8(ad_, %s, 0, %s);" % (D, ob)]
        elif k == 3:
            out += ["%s = (uint32_t)(int32_t)(int8_t)RD8(ad_, 0, %s);" % (D, ob), "now += 1;"]
        elif k == 4:
            out += ["%s = rc_ror(RD32(ad_, 0, %s), (ad_ & 3) * 8);" % (D, ob), "now += 1;"]
        elif k == 5:
            out += ["%s = rc_ror(RD16(ad_, 0, %s), (ad_ & 1) * 8);" % (D, ob), "now += 1;"]
        elif k == 6:
            out += ["%s = RD8(ad_, 0, %s);" % (D, ob), "now += 1;"]
        else:
            out += ["%s = (ad_ & 1) ? (uint32_t)(int32_t)(int8_t)RD8(ad_, 0, %s) : (uint32_t)(int32_t)(int16_t)RD16(ad_, 0, %s);"
                    % (D, ob, ob), "now += 1;"]
        return out

    def t_immoff(self, op, a):
        rd, off, rb = op & 7, (op >> 6) & 31, (op >> 3) & 7
        byte = op & 0x1000
        ob = self.tob(a)
        out = ["uint32_t ad_ = r%d + %d;" % (rb, off if byte else off * 4)]
        if op & 0x800:
            if byte:
                out.append("r%d = RD8(ad_, 0, %s);" % (rd, ob))
            else:
                out.append("r%d = rc_ror(RD32(ad_, 0, %s), (ad_ & 3) * 8);" % (rd, ob))
            out.append("now += 1;")
        else:
            out.append(("WR8(ad_, r%d, 0, %s);" if byte else "WR32(ad_, r%d, 0, %s);") % (rd, ob))
        return out

    def t_half(self, op, a):
        rd = op & 7
        ob = self.tob(a)
        out = ["uint32_t ad_ = r%d + %d;" % ((op >> 3) & 7, ((op >> 6) & 31) * 2)]
        if op & 0x800:
            out += ["r%d = rc_ror(RD16(ad_, 0, %s), (ad_ & 1) * 8);" % (rd, ob), "now += 1;"]
        else:
            out += ["WR16(ad_, r%d, 0, %s);" % (rd, ob)]
        return out

    def t_sprel(self, op, a):
        rd = (op >> 8) & 7
        ob = self.tob(a)
        out = ["uint32_t ad_ = r13 + %d;" % ((op & 0xFF) * 4)]
        if op & 0x800:
            out += ["r%d = rc_ror(RD32(ad_, 0, %s), (ad_ & 3) * 8);" % (rd, ob), "now += 1;"]
        else:
            out += ["WR32(ad_, r%d, 0, %s);" % (rd, ob)]
        return out

    def t_addpc(self, op, a):
        return ["r%d = %s;" % ((op >> 8) & 7, hx(((a + 4) & ~2) + (op & 0xFF) * 4))]

    def t_addsp(self, op, a):
        return ["r%d = r13 + %d;" % ((op >> 8) & 7, (op & 0xFF) * 4)]

    def t_spadj(self, op, a):
        off = (op & 0x7F) * 4
        return ["r13 %s= %d;" % ("-" if op & 0x80 else "+", off)]

    def t_pushpop(self, op, a):
        lst, extra = op & 0xFF, (op >> 8) & 1
        if not lst and not extra:
            return None
        ob = self.tob(a)
        regs = [i for i in range(8) if lst >> i & 1]
        if op & 0x800:  # POP
            ops = [("r", "r%d" % i) for i in regs] + ([("r", "uint32_t pc_")] if extra else [])
            out = self.transfer("r13", ops, ob)
            out += ["r13 += %d;" % (4 * len(ops)), "now += 1;"]
            if extra:
                out.append(self.jump("pc_"))
            return out
        n = len(regs) + extra
        out = ["r13 -= %d;" % (4 * n)]
        out += self.transfer("r13", [("w", "r%d" % i) for i in regs] + ([("w", "r14")] if extra else []), ob)
        return out

    def t_ldmstm(self, op, a):
        rb, lst = (op >> 8) & 7, op & 0xFF
        if not lst:
            return None
        ob = self.tob(a)
        regs = [i for i in range(8) if lst >> i & 1]
        out = ["uint32_t ad_ = r%d;" % rb, "uint32_t nb_ = ad_ + %d;" % (4 * len(regs))]
        if op & 0x800:
            if not (lst >> rb & 1):
                out.append("r%d = nb_;" % rb)
            out += self.transfer("ad_", [("r", "r%d" % i) for i in regs], ob)
            out.append("now += 1;")
        else:
            first = regs[0]
            out += self.transfer("ad_", [("w", "nb_" if (i == rb and i != first) else "r%d" % i) for i in regs], ob)
            out += ["r%d = nb_;" % rb]
        return out

    def t_bcond(self, op, a):
        c = (op >> 8) & 15
        t = (a + 4 + sext(op & 0xFF, 8) * 2) & 0xFFFFFFFF
        return ["if (%s)" % COND[c], "    " + self.jump(t)]

    def t_b(self, op, a):
        return [self.jump((a + 4 + sext(op & 0x7FF, 11) * 2) & 0xFFFFFFFF)]

    def t_blhi(self, op, a):
        return ["r14 = %s;" % hx(a + 4 + (sext(op & 0x7FF, 11) << 12))]

    def t_bllo(self, op, a):
        return ["uint32_t t_ = r14 + %d;" % ((op & 0x7FF) * 2), "r14 = %s;" % hx((a + 2) | 1), self.jump("t_")]

    # ---- one function -------------------------------------------------------------------------------------

    def function(self, name: str, region: int, comments: bool = True) -> str:
        """The C function: blocks of instructions, each entered at its first instruction (a resume point)."""
        addrs = sorted(self.addrs)
        size = 2 if self.thumb else 4
        ops, bodies = {}, {}
        for x in addrs:
            op = self.rom.u16(x) if self.thumb else self.rom.u32(x)
            ops[x] = op
            bodies[x] = self.thumb_ins(op, x) if self.thumb else self.arm(op, x)
        leaders = block_leaders(addrs, size, bodies, self.targets)
        out = ["RC_FN %s(gba_machine *m_)" % name, "{", "    RC_ENTER(%d, %d);" % (int(self.thumb), region),
               "    switch (RC_PC()) {"]
        segs = set()
        if self.seg_resume:
            segs = {x + size for x in addrs if bodies[x] is not None and is_memory(bodies[x])
                    and x + size in self.addrs and x + size not in leaders and bodies[x + size] is not None}
        left = block_left(addrs, size, leaders | segs)
        for x in addrs:
            if x in leaders or x in segs:
                out.append("    case %s: goto %s;" % (hx(x), label(x)))
        # The other instructions (inside blocks): how many to the block's end, from a table.
        lo = addrs[0]
        tab = [0] * ((addrs[-1] - lo) // size + 1)
        for x in addrs:
            if x not in leaders and x not in segs:
                tab[(x - lo) // size] = left[x]
        if any(tab):
            while not tab[-1]:
                tab.pop()
            out.append("    default: {")
            rows = [", ".join(str(v) for v in tab[k:k + 40]) for k in range(0, len(tab), 40)]
            out.append("        static const uint8_t mid_[%d] = {%s};" % (len(tab), (",\n" + " " * 12).join(rows)))
            out.append("        RC_MID_TABLE(%s, mid_);" % hx(lo))
            out.append("        RC_NOT_COVERED();")
            out.append("    }")
        else:
            out.append("    default: RC_NOT_COVERED();")
        out.append("    }")
        runs, in_run, fetched, folded = {}, set(), set(), set()
        if self.fetch_runs:
            runs = fetch_runs(addrs, size, bodies, leaders | segs | self.targets, region)
            in_run = {x + k * size for x, r in runs.items() for k in range(1, r[0])}
            folded = {x + k * size for x, r in runs.items() for k in range(r[0]) if r[2] >> k & 1}
        for i, x in enumerate(addrs):
            op, body = ops[x], bodies[x]
            text = disasm(op, x, self.thumb) if comments else ""
            head = "%s: " % label(x) if x in leaders or x in self.targets else ""
            out.append("%s/* %08X %0*X %s */" % (head, x, 4 if self.thumb else 8, op, text.replace("*/", "* /")))
            if body is None:
                self.fallbacks += 1
                out.append("    RC_INTERP(%s);" % hx(x))
                continue
            if x in leaders:
                seg = segment_cost(addrs, i, size, leaders, bodies)
                if is_run_segment(runs, x, seg):  # the check and the run's fetches in one (« runs of opcode fetches »)
                    out.append("    RC_STOP_IN(%s, %s, %d, %s);" % (hx(x), seg, left[x], run_args(runs[x], False)))
                    fetched.add(x)
                else:
                    out.append("    RC_STOP(%s, %s, %d);" % (hx(x), seg, left[x]))
            if x in fetched:
                pass
            elif x in runs:
                out.append("    RC_IN(%s, %s);" % (hx(x), run_args(runs[x], True)))
            elif x not in in_run:
                out.append("    RC_I(%s);" % hx(x))
            if body:
                out.append("    {")
                if x in folded:  # its stop of the buffer is in its run's cycles: the load's wait states stay
                    body = [LIT_RE.sub(r"\1", s) for s in body]
                out += ["        " + s for s in body]
                out.append("    }")
            nxt = x + size
            if nxt not in self.addrs:
                out.append("    RC_EXIT(%s);" % hx(nxt))
            elif is_memory(body) and nxt not in leaders and bodies[nxt] is not None:
                seg = segment_cost(addrs, i + 1, size, leaders, bodies)
                lab = "%s:" % label(nxt) if nxt in segs else ""
                if is_run_segment(runs, nxt, seg):
                    out.append("%s    RC_CHECK_IN(%s, %s, %d, %s);" % (lab, hx(nxt), seg, left[nxt],
                                                                    run_args(runs[nxt], False)))
                    fetched.add(nxt)
                else:
                    out.append("%s    RC_CHECK(%s, %s, %d);" % (lab, hx(nxt), seg, left[nxt]))
        out += ["    RC_EPILOGUE();", "}"]
        return "\n".join(out) + "\n"


# ---- blocks and the event checks --------------------------------------------------------------------------------
#
# The interpreter checks `now < next_event` before every instruction. Lifted code stops before the same instruction,
# with one check per segment rather than a label, a test and an exit per instruction (what made the C slow to
# compile). A block starts at a resume point (block_leaders); a segment starts at the block's start or after a memory
# access and runs up to the next memory access or the block's last instruction. Its instructions before the last
# cost static cycles (fetch, internal and multiply cycles, folded literal loads: static_cost), so `now + those cycles
# < ne` at its start means no check inside would stop. A memory access's own cycles, and whatever a bus call does to
# the clock or the next event, are seen by the check after it. RC_STOP, at a block's start, is also the exact check of
# its first instruction: due now, leave for the dispatcher; due inside the segment, the interpreter runs the rest of
# the block (GBA_RC_EXIT_INTERP, rc_hint = how many instructions), as RC_CHECK after a memory access. Entered inside a
# block, the function answers the same from its mid_ table. A bus write that raises an IRQ or halts the CPU sets the
# local next event to 0, so the next check stops there. The bound also counts the last instruction's fetch: its first
# data access then starts before the next event too, as it must (the interpreter's bus runs an immediate DMA whose start
# delay is over before such an access, gba_dma_due_check; lifted code's inline RAM accesses do not look).

MEM_RE = re.compile(r"\b(?:RD|WR)(?:8|16|32)\(|\bRC_RUN_RAM\(")
NOW_RE = re.compile(r"now \+= ([^;]*);")
MUL_RE = re.compile(r"^\(uint32_t\)rc_mul_cycles\(\w+, \d\)( \+ 1)?$")
WAIT_RE = re.compile(r"h->wait(?:16|32)\[\d+\]\[0\]")
LIT_RE = re.compile(r"RC_LIT\((h->wait(?:16|32)\[\d+\]\[0\])\)")
EXITS = ("RC_JUMP_DYN", "RC_BX", "RC_EXIT")  # what follows may be a return point
MAX_BLOCK = 12  # instructions per block at most (an event in a block makes the interpreter run the rest of it)


def is_memory(body) -> bool:
    return any(MEM_RE.search(s) for s in body)


def static_cost(body):
    """(constant cycles, [table terms]) the instruction adds to now beyond its fetch, at most, on the paths that
    stay in the block (refills belong to jumps, which leave it)."""
    const, terms = 0, []
    for s in body:
        for e in NOW_RE.findall(s):
            e = e.strip()
            if e.isdigit():
                const += int(e)
            elif MUL_RE.match(e):
                const += 5 if e.endswith("+ 1") else 4
            elif WAIT_RE.fullmatch(e) or e == "RC_NEXTRA()":
                terms.append(e)
            elif LIT_RE.fullmatch(e):  # a folded literal: the load's wait states, plus the prefetch buffer's stop
                terms.append(LIT_RE.fullmatch(e).group(1))
                const += 1
            else:
                raise ValueError("lift: unknown cycle term %r" % e)
    return const, terms


def segment_cost(addrs, i, size, leaders, bodies):
    """The cycle bound of the segment starting at addrs[i]: the instructions before its last one, and the last one's
    fetch."""
    nfc, const, terms = 0, 0, []
    j = i
    while True:
        body = bodies[addrs[j]]
        if is_memory(body) or j + 1 >= len(addrs) or addrs[j + 1] in leaders or bodies[addrs[j + 1]] is None \
                or addrs[j + 1] != addrs[j] + size:
            break
        c, t = static_cost(body)
        nfc += 1
        const += c
        terms += t
        j += 1
    return "%d, %s" % (nfc + 1, " + ".join([str(const)] + terms))


# ---- runs of opcode fetches (XBA-295) ---------------------------------------------------------------------------
#
# The cartridge's prefetch buffer (rc_pf_fetch) is a call per instruction. Where instructions in a row do nothing to
# the clock but static internal cycles (no memory access, no multiply, no jump or exit, nothing else that reads `now`
# or the machine), their bodies cannot tell when the fetches after theirs happen: RC_IN charges the fetches of such a
# run, plus the next instruction's, at its start, in one call (rc_pf_fetch_n), each fetch at the cycle it would have
# had (EXTRAS: 4 bits per instruction, its internal cycles before the next fetch). A literal folded from the
# function's own region joins a cartridge function's runs too (STOPS: a bit per instruction): its stop of the buffer
# goes into the run's cycles, its body keeps the load's wait states (with the buffer off it is stopped already). The
# run's last instruction (any kind) then runs with `now` where it was before. A run never crosses a label. A segment
# that is one run (RC_STOP_IN / RC_CHECK_IN) checks the next event against the run's exact cycles when the bound
# fails.

PLAIN_NOW = re.compile(r"^now \+= (\d+);$")
RUN_MAX = 16  # fetches per run at most (EXTRAS: 4 bits for each of the first 15)


def plain_extra(body, lit=None):
    """(internal cycles, folded literal) of a plain instruction (one whose fetch a run may move: see above), or None.
    lit: the wait-state term of a literal load that may join the run (RC_LIT(lit) at the top level), if any."""
    if body is None or is_memory(body):
        return None
    extra, folded = 0, False
    for s in body:
        if lit and not folded and s == "now += RC_LIT(%s);" % lit:
            folded = True
            continue
        m = PLAIN_NOW.match(s)  # top level only: a conditional instruction's cycles are not static
        if m:
            extra += int(m.group(1))
            continue
        # RC_NEXTRA() is 0 where the buffer is on (a cartridge function); elsewhere RC_IN charges each fetch as RC_I
        t = s.replace("now += RC_NEXTRA();", "")
        if re.search(r"\bnow\b|\bRC_|\bh->|\bgoto\b|\breturn\b|\bepc\b|\bm_\b|rc_mul_cycles", t):
            return None
    return (extra, folded) if extra < 16 else None


def run_args(run, fetches):
    """RC_IN's N, X, S (RC_STOP_IN / RC_CHECK_IN: X, S; N is the segment's)."""
    n, extras, stops = run
    x = "0x%X%s" % (extras, "u" if extras < 1 << 32 else "ull")
    return ("%d, " % n if fetches else "") + "%s, 0x%Xu" % (x, stops)


def is_run_segment(runs, x, seg):
    """The segment starting at x (segment_cost's text) is one run: RC_STOP_IN / RC_CHECK_IN, whose exact cycles
    decide when the bound says the next event may fall inside."""
    return x in runs and runs[x][0] == int(seg.split(",", 1)[0])


def fetch_runs(addrs, size, bodies, labels, region=0):
    """{first address: (fetches, extras, stops)} for every run of two fetches or more."""
    lit = "h->wait32[%d][0]" % region if 8 <= region <= 13 else None
    runs = {}
    i = 0
    while i < len(addrs):
        j, extras, stops = i, 0, 0
        while j - i + 1 < RUN_MAX and j + 1 < len(addrs) and addrs[j + 1] == addrs[j] + size \
                and addrs[j + 1] not in labels and bodies[addrs[j + 1]] is not None:
            e = plain_extra(bodies[addrs[j]], lit)
            if e is None:
                break
            extras |= e[0] << (4 * (j - i))
            stops |= e[1] << (j - i)
            j += 1
        if j > i and bodies[addrs[i]] is not None:
            runs[addrs[i]] = (j - i + 1, extras, stops)
        i = j + 1
    return runs


def block_left(addrs, size, leaders):
    """For each instruction, how many instructions from it to the end of its block (itself included)."""
    left = {}
    n = 0
    for x in reversed(addrs):
        n = 1 if (x + size not in left or x + size in leaders) else n + 1
        left[x] = n
    return left


def block_leaders(addrs, size, bodies, targets):
    """Resume points: the entry, local jump targets, what follows an instruction that may leave (a call's return
    point, after an interpreted instruction), the first instruction after a gap, and every MAX_BLOCK instructions."""
    leaders = set()
    run = 0
    prev = None
    for x in addrs:
        lead = prev is None or x in targets or prev != x - size or run >= MAX_BLOCK
        if not lead:
            pb = bodies[prev]
            lead = pb is None or any(e in s for s in pb for e in EXITS)
        if lead and bodies[x] is not None:
            leaders.add(x)
            run = 0
        if bodies[x] is None:
            run = MAX_BLOCK  # the instruction after an interpreted one starts a block
        run += 1
        prev = x
    return leaders
