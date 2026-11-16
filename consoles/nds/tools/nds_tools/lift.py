"""The GBA ARM / Thumb discovery and lifter (gba_tools), extended to the DS ARM9's ARMv5TE (XBA-223).

Everything ARMv4T stays gba_tools' code, unchanged (the GBA's output is byte-identical); these subclasses add what
ARMv5TE has and the ARM7TDMI has not, and undo two GBA assumptions:

- BLX (immediate, ARM and Thumb; register, ARM and Thumb), CLZ, QADD / QSUB / QDADD / QDSUB (the Q flag),
  SMULxy / SMLAxy / SMULWy / SMLAWy / SMLALxy, LDRD / STRD (no-ops on the ARM7TDMI), PLD (a no-op here);
- loads to PC interwork on ARMv5 (LDR pc, LDM {pc}, POP {pc}: bit 0 picks Thumb), data processing to PC does not;
- MCR / MRC to CP15 are ordinary instructions (the ARM7TDMI has no coprocessor: gba_tools' discovery ends a function
  at one, as undefined); the lifter still hands them, BKPT and the other coprocessor encodings to the interpreter;
- literals fold from the run-time image (code runs from RAM and ITCM, not from a cartridge at 0x08000000), and no
  cartridge open bus: the open-bus argument of the bus macros is 0.

Long multiplies with S leave C alone on ARMv5 (the ARM7TDMI sets it, rc_mull_carry): nds_recomp_rt.h defines
rc_mull_carry so that lifted code keeps C.
"""

from gba_tools.discover import MAX_WEAK_INSNS, Discovery
from gba_tools.isa import sext
from gba_tools.lift import COND, FnLifter, hx


def arm_v5_class(op: int):
    """ARMv5TE encodings the ARMv4T classifier (gba_tools.isa.arm_class) does not know, else None."""
    if op >> 28 == 0xF:
        if (op & 0xFE000000) == 0xFA000000:
            return "blx_imm"
        if (op & 0xFD70F000) == 0xF550F000:
            return "pld"
        return "nv"
    if (op & 0x0FFFFFF0) == 0x012FFF30:
        return "blx_reg"
    if (op & 0x0FFF0FF0) == 0x016F0F10:
        return "clz"
    if (op & 0x0F900FF0) == 0x01000050:
        return "qarith"
    if (op & 0x0F900090) == 0x01000080:
        return "smulxy"
    if (op & 0x0FF000F0) == 0x01200070:
        return "bkpt"
    return None


def blx_imm_target(op: int, a: int) -> int:
    return (a + 8 + sext(op & 0xFFFFFF, 24) * 4 + ((op >> 23) & 2)) & 0xFFFFFFFF


class NdsDiscovery(Discovery):
    """gba_tools' recursive descent on the run-time image, seeded by the image's hand-over points, ARMv5-aware."""

    def __init__(self, image, discovered=()):
        super().__init__(image, discovered)

    def step_arm(self, f, a, consts, bound, strict):
        op = self.rom.u32(a)
        cls = arm_v5_class(op)
        al = (op >> 28) == 0xE
        if cls == "blx_imm":
            f.calls.add((blx_imm_target(op, a), True))
            return [], True, {k: v for k, v in consts.items() if k not in (0, 1, 2, 3, 12, 14)}, None
        if cls == "pld":
            return [], True, consts, bound
        if cls == "blx_reg":
            rm = op & 15
            v = consts.get(rm)
            if v is not None and self.in_rom(v & ~1):
                f.calls.add(((v & ~1) if v & 1 else (v & ~3), bool(v & 1)))
            return [], True, {k: v for k, v in consts.items() if k not in (0, 1, 2, 3, 12, 14)}, None
        if cls in ("clz", "qarith", "smulxy"):
            c = dict(consts)
            c.pop((op >> 16) & 15 if cls == "smulxy" else (op >> 12) & 15, None)
            if cls == "smulxy" and ((op >> 21) & 3) == 2:
                c.pop((op >> 12) & 15, None)
            return [], True, c, bound
        if (op & 0x0F000010) == 0x0E000010 and ((op >> 8) & 15) == 15:  # MRC / MCR p15: CP15, not undefined
            c = dict(consts)
            if op & (1 << 20):
                c.pop((op >> 12) & 15, None)
            return [], True, c, bound
        if cls in ("bkpt", "nv"):
            if strict:
                f.bad = "%s at 0x%08X" % (cls, a)
            return [], cls != "bkpt", consts, bound
        # LDR pc, =literal with bit 0 set: a Thumb target (loads to PC interwork on ARMv5).
        if (op & 0x0F7F0000) == 0x051F0000 and ((op >> 12) & 15) == 15 and (op & (1 << 20)):
            addr = (a + 8 + (op & 0xFFF)) if op & (1 << 23) else (a + 8 - (op & 0xFFF))
            v = self.rom.u32(addr) if not addr & 3 else None
            if v is not None and v & 1:
                self.pool_word(f, addr, v)
                f.calls.add((v & ~1, True))
                return [], not al, consts, bound
        return super().step_arm(f, a, consts, bound, strict)

    def step_thumb(self, f, a, consts, bound, strict):
        op = self.rom.u16(a)
        if (op >> 11) == 0x1D:  # BLX suffix (to ARM)
            prev = self.rom.u16(a - 2)
            if prev is not None and (prev >> 11) == 0x1E:
                t = (a - 2 + 4 + (sext(prev & 0x7FF, 11) << 12) + (op & 0x7FF) * 2) & 0xFFFFFFFC
                f.calls.add((t, False))
            elif strict:
                f.bad = "torn BLX pair at 0x%08X" % a
            return [], True, {k: v for k, v in consts.items() if k not in (0, 1, 2, 3, 12, 14)}, None
        if (op & 0xFF87) == 0x4780:  # BLX rN
            rs = (op >> 3) & 15
            v = consts.get(rs)
            if v is not None and self.in_rom(v & ~1):
                f.calls.add(((v & ~1) if v & 1 else (v & ~3), bool(v & 1)))
            return [], True, {k: v for k, v in consts.items() if k not in (0, 1, 2, 3, 12, 14)}, None
        if (op >> 11) == 0x1E and self.rom.u16(a + 2) is not None and (self.rom.u16(a + 2) >> 11) == 0x1D:
            return [], True, consts, bound  # BL / BLX prefix, its suffix follows
        if (op >> 8) == 0xBE:  # BKPT
            if strict:
                f.bad = "BKPT at 0x%08X" % a
            return [], False, consts, bound
        return super().step_thumb(f, a, consts, bound, strict)

    def run(self):
        for (addr, thumb) in self.rom.seeds:
            self.add_entry(addr, thumb, "entry")
        # Thumb code pointers anywhere in the image (bit 0 set): C function tables (newlib's devoptab, libnds'
        # console hooks) live in .rodata / .data, not in literal pools, so gba_tools' pool scan misses them. Weak:
        # kept only when the code decodes cleanly, as gba_tools does for its pool pointers.
        for s in self.rom.segments:
            for off in range(0, len(s.data) - 3, 4):
                v = int.from_bytes(s.data[off:off + 4], "little")
                if not v & 1:
                    continue
                a = v & ~1
                if not self.in_rom(a, 2) or (a | 1) in self.entries or (a | 1) in self.rejected:
                    continue
                g = self.explore(a, True, strict=True, cap=MAX_WEAK_INSNS)
                if g.bad:
                    self.rejected[a | 1] = g.bad
                else:
                    self.add_entry(a, True, "data")
        return super().run()

    def add_entry(self, addr, thumb, why):
        # gba_tools' run() seeds the cartridge entry (0x08000000) first: not DS code.
        if not self.in_rom(addr, 2 if thumb else 4):
            return False
        return super().add_entry(addr, thumb, why)


class NdsLifter(FnLifter):
    """gba_tools' lifter with the ARMv5TE additions; emits the same RC_* vocabulary, which nds_recomp_rt.h
    defines for the DS."""

    def __init__(self, image, thumb, addrs):
        super().__init__(image, thumb, addrs)
        self._iw = False  # inside a load to PC: interworking jump

    # ---- what changes for every instruction ----------------------------------------------------------------

    def fold_literal(self, addr, size):
        if size == 4:
            return self.rom.u32(addr & ~3)
        return self.rom.u8(addr)

    def aob(self, a):
        return "0u"

    def tob(self, a):
        return "0u"

    def jump(self, target):
        if self._iw:
            if isinstance(target, int):
                if target & 1:
                    return "RC_BX(%s);" % hx(target)
                return super().jump(target)
            return "RC_BX(%s);" % target
        return super().jump(target)

    def _interwork(self, fn, *args):
        self._iw = True
        try:
            return fn(*args)
        finally:
            self._iw = False

    # ---- ARM ----------------------------------------------------------------------------------------------

    def arm(self, op, a):
        cls = arm_v5_class(op)
        if cls is None:
            return super().arm(op, a)
        if cls == "pld":
            return []
        if cls == "blx_imm":
            return ["r14 = %s;" % hx(a + 4), "RC_BX(%s);" % hx(blx_imm_target(op, a) | 1)]
        if cls in ("nv", "bkpt"):
            return None
        body = getattr(self, "arm_" + cls)(op, a)
        if body is None:
            return None
        body = self.after_access(body)
        cond = op >> 28
        if cond == 0xE:
            return body
        return ["if (%s) {" % COND[cond]] + ["    " + s for s in body] + ["}"]

    def arm_blx_reg(self, op, a):
        return ["uint32_t t_ = %s;" % self.reg(op & 15, a + 8), "r14 = %s;" % hx(a + 4), "RC_BX(t_);"]

    def arm_clz(self, op, a):
        rd, rm = (op >> 12) & 15, op & 15
        if rd == 15:
            return None
        return ["r%d = rc_clz(%s);" % (rd, self.reg(rm, a + 8))]

    def arm_qarith(self, op, a):
        rd, rn, rm = (op >> 12) & 15, (op >> 16) & 15, op & 15
        if rd == 15:
            return None
        kind = (op >> 21) & 3  # 0 QADD, 1 QSUB, 2 QDADD, 3 QDSUB
        out = ["uint32_t q_ = 0;", "int32_t n_ = (int32_t)%s;" % self.reg(rn, a + 8)]
        if kind >= 2:
            out.append("n_ = rc_qadd(n_, n_, &q_);")
        fn = "rc_qadd" if kind in (0, 2) else "rc_qsub"
        out.append("r%d = (uint32_t)%s((int32_t)%s, n_, &q_);" % (rd, fn, self.reg(rm, a + 8)))
        out.append("h->cpu.q |= q_;")
        return out

    def arm_smulxy(self, op, a):
        kind = (op >> 21) & 3  # 0 SMLAxy, 1 SMLAWy / SMULWy, 2 SMLALxy, 3 SMULxy
        rd, rn, rs, rm = (op >> 16) & 15, (op >> 12) & 15, (op >> 8) & 15, op & 15
        x, y = (op >> 5) & 1, (op >> 6) & 1
        if rd == 15 or (kind == 2 and rn == 15):
            return None
        M = self.reg(rm, a + 8)
        S = self.reg(rs, a + 8)
        half = lambda v, top: "(int32_t)(int16_t)(%s >> 16)" % v if top else "(int32_t)(int16_t)%s" % v
        if kind == 1:
            prod = "(int32_t)(((int64_t)(int32_t)%s * %s) >> 16)" % (M, half(S, y))
            if x:  # SMULWy
                return ["r%d = (uint32_t)%s;" % (rd, prod)]
            return ["uint32_t q_ = 0;",
                    "r%d = (uint32_t)rc_qadd_flag(%s, (int32_t)%s, &q_);" % (rd, prod, self.reg(rn, a + 8)),
                    "h->cpu.q |= q_;"]
        prod = "(%s * %s)" % (half(M, x), half(S, y))
        if kind == 3:
            return ["r%d = (uint32_t)%s;" % (rd, prod)]
        if kind == 0:
            return ["uint32_t q_ = 0;",
                    "r%d = (uint32_t)rc_qadd_flag(%s, (int32_t)%s, &q_);" % (rd, prod, self.reg(rn, a + 8)),
                    "h->cpu.q |= q_;"]
        # SMLALxy: RdHi = rd, RdLo = rn
        return ["uint64_t acc_ = (uint64_t)r%d << 32 | r%d;" % (rd, rn),
                "acc_ += (uint64_t)(int64_t)%s;" % prod,
                "r%d = (uint32_t)acc_;" % rn, "r%d = (uint32_t)(acc_ >> 32);" % rd]

    def arm_mrs(self, op, a):
        body = super().arm_mrs(op, a)
        if body:
            body = [s.replace("h->cpu.cpsr_ctl;", "h->cpu.q << 27 | h->cpu.cpsr_ctl;") for s in body]
        return body

    def arm_single(self, op, a):
        return self._interwork(super().arm_single, op, a)

    def arm_block(self, op, a):
        return self._interwork(super().arm_block, op, a)

    def arm_half(self, op, a):
        l, sh = (op >> 20) & 1, (op >> 5) & 3
        if l or sh < 2:
            return self._interwork(super().arm_half, op, a)
        # LDRD (sh 2) / STRD (sh 3): rd even, rd + 1, two words.
        p, u, w = (op >> 24) & 1, (op >> 23) & 1, (op >> 21) & 1
        rn, rd = (op >> 16) & 15, (op >> 12) & 15
        writeback = (not p) or w
        if rd & 1 or rd == 14 or (rn == 15 and writeback):
            return None
        off = hx(((op >> 4) & 0xF0) | (op & 0xF)) if op & (1 << 22) else self.reg(op & 15, a + 8)
        out = ["uint32_t b_ = %s;" % self.reg(rn, a + 8),
               "uint32_t mv_ = b_ %s %s;" % ("+" if u else "-", off),
               "uint32_t ad_ = %s;" % ("mv_" if p else "b_")]
        if sh == 2:
            out += ["uint32_t v0_ = RD32(ad_, 0, 0u);", "uint32_t v1_ = RD32(ad_ + 4, 1, 0u);"]
            if writeback:
                out.append(self.wb(rn))
            out += ["r%d = v0_;" % rd, "r%d = v1_;" % (rd + 1)]
        else:
            out += ["WR32(ad_, %s, 0, 0u);" % self.reg(rd, a + 12),
                    "WR32(ad_ + 4, %s, 1, 0u);" % self.reg(rd + 1, a + 12)]
            if writeback:
                out.append(self.wb(rn))
        return out

    # ---- Thumb --------------------------------------------------------------------------------------------

    def thumb_ins(self, op, a):
        if (op >> 11) == 0x1D:  # BLX suffix: to ARM, at (lr + off) & ~3
            return ["uint32_t t_ = (r14 + %d) & ~3u;" % ((op & 0x7FF) * 2), "r14 = %s;" % hx((a + 2) | 1),
                    "RC_BX(t_);"]
        if (op >> 8) == 0xBE:
            return None
        return super().thumb_ins(op, a)

    def t_hireg(self, op, a):
        if (op & 0xFF87) == 0x4780:  # BLX rN
            return ["uint32_t t_ = %s;" % self.reg((op >> 3) & 15, a + 4), "r14 = %s;" % hx((a + 2) | 1),
                    "RC_BX(t_);"]
        return super().t_hireg(op, a)

    def t_pushpop(self, op, a):
        return self._interwork(super().t_pushpop, op, a)
