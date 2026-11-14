"""ARM / Thumb to C (XBA-151): one C function per discovered function, on the ABI of gba_recomp_rt.h.

Every instruction is emitted from its opcode bits with the interpreter's semantics (consoles/gba/runtime/gba_cpu.c):
same results, same flags, same cycles in the same order (fetch, data accesses, internal cycles, refill). Each
instruction gets a label, reachable from the function's entry switch: lifted code can stop anywhere (scheduler
event, IRQ) and resume there. Instructions the interpreter handles better (SWI, MSR, exception returns, LDM / STM
with the S bit, SWP, undefined encodings, odd PC write-backs) return to the dispatcher with GBA_RC_EXIT_INTERP.
"""

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

    def __init__(self, rom, thumb: bool, addrs):
        self.rom = rom
        self.thumb = thumb
        self.addrs = set(addrs)
        self.pipe = 4 if thumb else 8
        self.fallbacks = 0

    # ---- operands -----------------------------------------------------------------------------------------

    @staticmethod
    def reg(n: int, pcval: int) -> str:
        return hx(pcval) if n == 15 else "r%d" % n

    def jump(self, target) -> str:
        """Write to PC in the current state: target is an int (constant) or a C expression."""
        if isinstance(target, int):
            t = target & (0xFFFFFFFE if self.thumb else 0xFFFFFFFC)
            if t in self.addrs:
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
        if (op >> 22) & 1 or rd == 15:
            return None
        return ["r%d = fN << 31 | fZ << 30 | fC << 29 | fV << 28 | h->cpu.cpsr_ctl;" % rd]

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
        RM = self.reg(rm, a + 8)
        out = ["uint32_t vs_ = %s;" % self.reg(rs, a + 8)]
        if sign:
            out.append("uint64_t res_ = (uint64_t)((int64_t)(int32_t)%s * (int32_t)vs_);" % RM)
        else:
            out.append("uint64_t res_ = (uint64_t)%s * vs_;" % RM)
        out.append("now += (uint32_t)rc_mul_cycles(vs_, %d) + 1;" % sign)
        if acc:
            out += ["res_ += (uint64_t)r%d << 32 | r%d;" % (hi, lo), "now += 1;"]
        out += ["r%d = (uint32_t)res_;" % lo, "r%d = (uint32_t)(res_ >> 32);" % hi]
        if s:
            out.append("fN = (uint32_t)(res_ >> 63); fZ = res_ == 0;")
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

    def wb(self, rn):
        return "r%d = mv_;" % rn

    def arm_single(self, op, a):
        p, u, b, w, l = (op >> 24) & 1, (op >> 23) & 1, (op >> 22) & 1, (op >> 21) & 1, (op >> 20) & 1
        rn, rd = (op >> 16) & 15, (op >> 12) & 15
        writeback = (not p) or w
        if rn == 15 and writeback:
            return None
        ob = hx(op)
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
                    out = ["now += h->%s[%d][0];" % ("wait16" if b else "wait32", region), "now += 1;"]
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
            out.append("RC_AFTER_STORE(%s);" % hx(a + 4))
        return out

    def arm_half(self, op, a):
        p, u, w, l = (op >> 24) & 1, (op >> 23) & 1, (op >> 21) & 1, (op >> 20) & 1
        rn, rd, sh = (op >> 16) & 15, (op >> 12) & 15, (op >> 5) & 3
        writeback = (not p) or w
        if rn == 15 and writeback:
            return None
        ob = hx(op)
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
            if sh == 1:
                out.append("RC_AFTER_STORE(%s);" % hx(a + 4))
        return out

    def arm_block(self, op, a):
        p, u, s, w, l = (op >> 24) & 1, (op >> 23) & 1, (op >> 22) & 1, (op >> 21) & 1, (op >> 20) & 1
        rn, lst = (op >> 16) & 15, op & 0xFFFF
        if s or rn == 15 or lst == 0:
            return None
        regs = [i for i in range(16) if lst >> i & 1]
        size = 4 * len(regs)
        ob = hx(op)
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
            for k, i in enumerate(regs):
                dst = "uint32_t pc_" if i == 15 else "r%d" % i
                out.append("%s = RD32(ad_ & ~3u, %d, %s); ad_ += 4;" % (dst, 1 if k else 0, ob))
            out.append("now += 1;")
            if 15 in regs:
                out.append(self.jump("pc_"))
        else:
            first = regs[0]
            for k, i in enumerate(regs):
                if i == 15:
                    val = hx(a + 12)
                elif i == rn and w and i != first:
                    val = "nb_"
                else:
                    val = "r%d" % i
                out.append("WR32(ad_ & ~3u, %s, %d, %s); ad_ += 4;" % (val, 1 if k else 0, ob))
            if w:
                out.append("r%d = nb_;" % rn)
            out.append("RC_AFTER_STORE(%s);" % hx(a + 4))
        return out

    # ---- Thumb --------------------------------------------------------------------------------------------

    def thumb_ins(self, op: int, a: int) -> list:
        cls = thumb_class(op)
        return getattr(self, "t_" + cls)(op, a) if hasattr(self, "t_" + cls) else None

    def tob(self, op):
        return hx(op * 0x00010001)

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
            return ["now += h->wait32[%d][0];" % ((addr >> 24) & 15), "r%d = %s;" % (rd, hx(v)), "now += 1;"]
        return ["r%d = RD32(%s, 0, %s);" % (rd, hx(addr), self.tob(op)), "now += 1;"]

    def t_regoff(self, op, a):
        rd, k = op & 7, (op >> 9) & 7
        ob = self.tob(op)
        out = ["uint32_t ad_ = r%d + r%d;" % ((op >> 3) & 7, (op >> 6) & 7)]
        D = "r%d" % rd
        if k == 0:
            out += ["WR32(ad_, %s, 0, %s);" % (D, ob), "RC_AFTER_STORE(%s);" % hx(a + 2)]
        elif k == 1:
            out += ["WR16(ad_, %s, 0, %s);" % (D, ob), "RC_AFTER_STORE(%s);" % hx(a + 2)]
        elif k == 2:
            out += ["WR8(ad_, %s, 0, %s);" % (D, ob), "RC_AFTER_STORE(%s);" % hx(a + 2)]
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
        ob = self.tob(op)
        out = ["uint32_t ad_ = r%d + %d;" % (rb, off if byte else off * 4)]
        if op & 0x800:
            if byte:
                out.append("r%d = RD8(ad_, 0, %s);" % (rd, ob))
            else:
                out.append("r%d = rc_ror(RD32(ad_, 0, %s), (ad_ & 3) * 8);" % (rd, ob))
            out.append("now += 1;")
        else:
            out.append(("WR8(ad_, r%d, 0, %s);" if byte else "WR32(ad_, r%d, 0, %s);") % (rd, ob))
            out.append("RC_AFTER_STORE(%s);" % hx(a + 2))
        return out

    def t_half(self, op, a):
        rd = op & 7
        ob = self.tob(op)
        out = ["uint32_t ad_ = r%d + %d;" % ((op >> 3) & 7, ((op >> 6) & 31) * 2)]
        if op & 0x800:
            out += ["r%d = rc_ror(RD16(ad_, 0, %s), (ad_ & 1) * 8);" % (rd, ob), "now += 1;"]
        else:
            out += ["WR16(ad_, r%d, 0, %s);" % (rd, ob), "RC_AFTER_STORE(%s);" % hx(a + 2)]
        return out

    def t_sprel(self, op, a):
        rd = (op >> 8) & 7
        ob = self.tob(op)
        out = ["uint32_t ad_ = r13 + %d;" % ((op & 0xFF) * 4)]
        if op & 0x800:
            out += ["r%d = rc_ror(RD32(ad_, 0, %s), (ad_ & 3) * 8);" % (rd, ob), "now += 1;"]
        else:
            out += ["WR32(ad_, r%d, 0, %s);" % (rd, ob), "RC_AFTER_STORE(%s);" % hx(a + 2)]
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
        ob = self.tob(op)
        regs = [i for i in range(8) if lst >> i & 1]
        if op & 0x800:  # POP
            out = ["uint32_t ad_ = r13;"]
            k = 0
            for i in regs:
                out.append("r%d = RD32(ad_ & ~3u, %d, %s); ad_ += 4;" % (i, 1 if k else 0, ob))
                k += 1
            if extra:
                out.append("uint32_t pc_ = RD32(ad_ & ~3u, %d, %s); ad_ += 4;" % (1 if k else 0, ob))
            out += ["r13 = ad_;", "now += 1;"]
            if extra:
                out.append(self.jump("pc_"))
            return out
        n = len(regs) + extra
        out = ["uint32_t ad_ = r13 - %d;" % (4 * n), "r13 = ad_;"]
        k = 0
        for i in regs:
            out.append("WR32(ad_ & ~3u, r%d, %d, %s); ad_ += 4;" % (i, 1 if k else 0, ob))
            k += 1
        if extra:
            out.append("WR32(ad_ & ~3u, r14, %d, %s);" % (1 if k else 0, ob))
        out.append("RC_AFTER_STORE(%s);" % hx(a + 2))
        return out

    def t_ldmstm(self, op, a):
        rb, lst = (op >> 8) & 7, op & 0xFF
        if not lst:
            return None
        ob = self.tob(op)
        regs = [i for i in range(8) if lst >> i & 1]
        out = ["uint32_t ad_ = r%d;" % rb, "uint32_t nb_ = ad_ + %d;" % (4 * len(regs))]
        if op & 0x800:
            if not (lst >> rb & 1):
                out.append("r%d = nb_;" % rb)
            for k, i in enumerate(regs):
                out.append("r%d = RD32(ad_ & ~3u, %d, %s); ad_ += 4;" % (i, 1 if k else 0, ob))
            out.append("now += 1;")
        else:
            first = regs[0]
            for k, i in enumerate(regs):
                val = "nb_" if (i == rb and i != first) else "r%d" % i
                out.append("WR32(ad_ & ~3u, %s, %d, %s); ad_ += 4;" % (val, 1 if k else 0, ob))
            out += ["r%d = nb_;" % rb, "RC_AFTER_STORE(%s);" % hx(a + 2)]
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
        addrs = sorted(self.addrs)
        size = 2 if self.thumb else 4
        out = ["RC_FN %s(gba_machine *m_)" % name, "{", "    RC_ENTER(%d, %d);" % (int(self.thumb), region),
               "    switch (RC_PC()) {"]
        for x in addrs:
            out.append("    case %s: goto %s;" % (hx(x), label(x)))
        out += ["    default: RC_NOT_COVERED();", "    }"]
        for i, x in enumerate(addrs):
            op = self.rom.u16(x) if self.thumb else self.rom.u32(x)
            text = disasm(op, x, self.thumb) if comments else ""
            out.append("%s: /* %0*X %s */" % (label(x), 4 if self.thumb else 8, op, text.replace("*/", "* /")))
            body = self.thumb_ins(op, x) if self.thumb else self.arm(op, x)
            if body is None:
                self.fallbacks += 1
                out.append("    RC_INTERP(%s);" % hx(x))
                continue
            out.append("    RC_I(%s);" % hx(x))
            if body:
                out.append("    {")
                out += ["        " + s for s in body]
                out.append("    }")
            nxt = x + size
            if nxt not in self.addrs:
                out.append("    RC_EXIT(%s);" % hx(nxt))
        out += ["    RC_EPILOGUE();", "}"]
        return "\n".join(out) + "\n"
