"""The DS spike's ARMv5TE discovery and lifter (nds_tools, over gba_tools), extended to the 3DS ARM11's ARMv6K +
VFPv2 (XBA-228).

gba_tools and nds_tools stay unchanged (the GBA and DS outputs are byte-identical); these subclasses add what the
ARM11 has and the ARM946E-S has not, and undo the assumptions that break on it:

- ARMv6's ARMv4T-shaped encodings that gba_tools' classifier gets wrong: LDREX / STREX (and the B / H / D forms)
  and UMAAL decode as data processing there, the media instructions (REV, SXT* / UXT*, parallel add / subtract, SEL,
  SSAT / USAT, PKH*, the dual and most-significant-word multiplies, USAD8) and every VFP encoding as undefined, which
  ends a function in discovery; the hints (NOP, YIELD, WFE...) as an MSR;
- loads and stores may be unaligned (ARMv6 with U = 1, as the 3DS runs): no rotation of an unaligned LDR / LDRH,
  LDRSH at an odd address is a halfword load;
- the GE flags join the CPSR an MRS reads;
- SVC is the operating system: discovery goes on after it, the lifter hands it to the interpreter (the HLE kernel);
- CP15: MRC c13 (the TLS pointer libctru reads constantly) and the c7 barriers are lifted;
- literals fold only from the code segment (read-only at run time): Image.contains is the code segment;
- discovery is seeded with the 3DSX's relocations that point into code (function pointers in data and literal
  pools: C tables, thread entries, callbacks), validated like gba_tools' weak candidates.

Media and VFP instructions are emitted as calls of the static inline functions of consoles/3ds/runtime/
ctr_recomp_rt.h with the opcode as a constant (rc_media, rc_vfp_*), which the interpreter calls too.
"""

from gba_tools.discover import MAX_WEAK_INSNS, Discovery
from gba_tools.lift import COND, hx
from nds_tools.lift import NdsDiscovery, NdsLifter


def media_known(op: int) -> bool:
    """The ARMv6 media encodings rc_media implements (the interpreter's arm_media)."""
    if (op & 0x0F800010) == 0x06000010:  # parallel add / subtract
        pre, kind = (op >> 20) & 7, (op >> 5) & 7
        return pre not in (0, 4) and kind not in (5, 6)
    if (op & 0x0F8003F0) == 0x06800070:  # SXT* / UXT*
        return ((op >> 20) & 7) not in (1, 5)
    return any((op & m) == v for (m, v) in (
        (0x0FF00030, 0x06800010),  # PKHBT / PKHTB
        (0x0FF00FF0, 0x06800FB0),  # SEL
        (0x0FFF0FF0, 0x06BF0F30),  # REV
        (0x0FFF0FF0, 0x06BF0FB0),  # REV16
        (0x0FFF0FF0, 0x06FF0FB0),  # REVSH
        (0x0FF00FF0, 0x06A00F30),  # SSAT16
        (0x0FF00FF0, 0x06E00F30),  # USAT16
        (0x0FA00030, 0x06A00010),  # SSAT / USAT
        (0x0FF000D0, 0x07000010),  # SMLAD / SMUAD
        (0x0FF000D0, 0x07000050),  # SMLSD / SMUSD
        (0x0FF000D0, 0x07500010),  # SMMLA / SMMUL
        (0x0FF000D0, 0x075000D0),  # SMMLS
        (0x0FF000F0, 0x07800010),  # USAD8 / USADA8
    ))


def media_dest_hi(op: int) -> bool:
    return (op & 0x0F800010) == 0x07000010 or (op & 0x0FF000F0) == 0x07800010


def vfp_dp_ok(op: int) -> bool:
    """The VFPv2 data-processing encodings rc_vfp_dp implements (ctr_recomp_rt.h rc_vfp_dp_ok)."""
    if op & 0x10:
        return False
    opc = ((op >> 23) & 1) << 3 | ((op >> 21) & 1) << 2 | ((op >> 20) & 1) << 1 | ((op >> 6) & 1)
    if opc <= 8:
        return True
    if opc != 15:
        return False
    ext = ((op >> 16) & 15) << 1 | ((op >> 7) & 1)
    return ext in (0x00, 0x01, 0x02, 0x03, 0x08, 0x09, 0x0A, 0x0B, 0x0F, 0x10, 0x11, 0x18, 0x19, 0x1A, 0x1B)


def arm_v6_class(op: int):
    """ARMv6K / VFPv2 / Horizon encodings the ARMv5TE layer does not handle as the ARM11 runs them, else None."""
    cond = op >> 28
    if cond == 0xF:
        if op == 0xF57FF01F:
            return "clrex"
        if (op & 0xFFF1FE20) == 0xF1000000:
            return "cps"
        if (op & 0xFFFFFDFF) == 0xF1010000:
            return "setend"
        return None  # BLX imm, PLD, the rest: nds_tools
    k = (op >> 25) & 7
    if k == 0:
        if (op & 0x0F8000F0) == 0x01800090 and ((op >> 8) & 15) == 15:
            return "ldrex" if op & (1 << 20) else "strex"
        if (op & 0x0FF000F0) == 0x00400090:
            return "umaal"
        return None
    if k == 1:
        if (op & 0x0FFFFF00) == 0x0320F000:
            return "hint"
        return None
    if k == 3 and op & 0x10:
        if (op & 0x0FF000F0) == 0x07F000F0:
            return "udf"
        if (op & 0x0FF000D0) == 0x07400010:
            return "media_long"
        return "media" if media_known(op) else "media_undef"
    cp = (op >> 8) & 15
    if k == 6 and cp in (10, 11):
        if (op & 0x0FE00000) == 0x0C400000:
            return "vfp_mrrc" if op & (1 << 20) else "vfp_mcrr"
        return "vfp_ls"
    if k == 7 and not op & (1 << 24):
        if cp in (10, 11):
            if not op & 0x10:
                return "vfp_dp"
            return "vfp_mrc" if op & (1 << 20) else "vfp_mcr"
        if cp == 15 and op & 0x10:
            cn, cm, op2 = (op >> 16) & 15, op & 15, (op >> 5) & 7
            if op & (1 << 20) and cn == 13 and cm == 0 and op2 == 3:
                return "mrc_tls"
            if not op & (1 << 20) and cn == 7:
                return "mcr_c7"
            return "cp15_other"
    if k == 7 and op & (1 << 24):
        return "svc"
    return None


def written_regs(cls: str, op: int):
    """Core registers an ARMv6 / VFP instruction writes (discovery forgets their constants)."""
    rd12, rd16 = (op >> 12) & 15, (op >> 16) & 15
    if cls == "ldrex":
        return (rd12, rd12 + 1) if ((op >> 21) & 3) == 1 else (rd12,)
    if cls in ("strex", "mrc_tls"):
        return (rd12,)
    if cls in ("umaal", "media_long", "vfp_mrrc"):
        return (rd12, rd16)
    if cls == "media":
        return (rd16,) if media_dest_hi(op) else (rd12,)
    if cls == "vfp_mrc":
        return () if rd12 == 15 else (rd12,)
    if cls == "vfp_ls" and op & (1 << 21):
        return (rd16,)
    return ()


class CtrDiscovery(NdsDiscovery):
    """The DS spike's discovery on the 3DSX's code segment, ARMv6K-aware, seeded by the entry and the relocations."""

    def step_arm(self, f, a, consts, bound, strict):
        op = self.rom.u32(a)
        cls = arm_v6_class(op)
        if cls is None:
            return super().step_arm(f, a, consts, bound, strict)
        if cls == "svc":
            return [], True, {}, bound
        if cls in ("udf", "media_undef", "cp15_other", "setend"):
            if strict:
                f.bad = "%s at 0x%08X" % (cls, a)
            return [], cls not in ("udf", "media_undef"), consts, bound
        c = dict(consts)
        for r in written_regs(cls, op):
            c.pop(r, None)
        return [], True, c, bound

    def run(self):
        for (addr, thumb) in self.rom.seeds:
            self.add_entry(addr, thumb, "entry")
        # Pointers into the code segment that the relocations name: weak entries, kept when their code decodes
        # cleanly (a relocated word may also be the address of data inside .text).
        for (_where, t, thumb) in sorted(set(self.rom.code_pointers())):
            key = t | int(thumb)
            if t & (1 if thumb else 3) or key in self.entries or key in self.rejected:
                continue
            g = self.explore(t, thumb, strict=True, cap=MAX_WEAK_INSNS)
            if g.bad:
                self.rejected[key] = g.bad
            else:
                self.add_entry(t, thumb, "reloc")
        return Discovery.run(self)


UNALIGNED = (
    # ARMv4T's rotated unaligned word / halfword loads and odd LDRSH: plain unaligned loads on ARMv6 with U = 1.
    ("rc_ror(RD32(ad_, 0, 0u), (ad_ & 3) * 8)", "RD32(ad_, 0, 0u)"),
    ("rc_ror(RD16(ad_, 0, 0u), (ad_ & 1) * 8)", "RD16(ad_, 0, 0u)"),
    ("(ad_ & 1) ? (uint32_t)(int32_t)(int8_t)RD8(ad_, 0, 0u) : (uint32_t)(int32_t)(int16_t)RD16(ad_, 0, 0u)",
     "(uint32_t)(int32_t)(int16_t)RD16(ad_, 0, 0u)"),
    # MRS: the GE flags too
    ("h->cpu.q << 27 | h->cpu.cpsr_ctl", "h->cpu.q << 27 | (h->cpu.ge & 15) << 16 | h->cpu.cpsr_ctl"),
)


def armv6_text(body):
    if body is None:
        return None
    out = []
    for s in body:
        for (old, new) in UNALIGNED:
            s = s.replace(old, new)
        out.append(s)
    return out


class CtrLifter(NdsLifter):
    """nds_tools' lifter with the ARMv6K + VFPv2 additions; emits the RC_* vocabulary, which ctr_recomp_rt.h defines
    for the 3DS."""

    def fold_literal(self, addr, size):
        if size == 4 and addr & 3:
            return None  # an unaligned literal: ARMv6 would not rotate it (never seen; leave it to the load)
        return super().fold_literal(addr, size)

    def thumb_ins(self, op, a):
        return armv6_text(super().thumb_ins(op, a))

    def arm(self, op, a):
        cls = arm_v6_class(op)
        if cls is None:
            return armv6_text(super().arm(op, a))
        body = self.v6_body(cls, op, a)
        if body is None:
            return None
        body = self.after_access(body)
        cond = op >> 28
        if cond >= 0xE:
            return body
        return ["if (%s) {" % COND[cond]] + ["    " + s for s in body] + ["}"]

    def v6_body(self, cls, op, a):
        R = lambda n: self.reg(n, a + 8)  # noqa: E731
        rd12, rd16, rd8, rd0 = (op >> 12) & 15, (op >> 16) & 15, (op >> 8) & 15, op & 15
        if cls in ("hint", "mcr_c7"):
            return []
        if cls == "clrex":
            return ["h->cpu.excl = 0;"]
        if cls == "mrc_tls":
            return None if rd12 == 15 else ["r%d = h->cpu.tls;" % rd12]
        if cls == "ldrex":
            kind = (op >> 21) & 3
            if rd16 == 15 or rd12 >= 14 or (kind == 1 and rd12 & 1):
                return None
            out = ["uint32_t ad_ = r%d;" % rd16, "h->cpu.excl = 1;"]
            if kind == 1:
                out += ["uint32_t v0_ = RD32(ad_, 0, 0u);", "uint32_t v1_ = RD32(ad_ + 4, 0, 0u);",
                        "r%d = v0_;" % rd12, "r%d = v1_;" % (rd12 + 1)]
            else:
                rd = {0: "RD32", 2: "RD8", 3: "RD16"}[kind]
                out.append("r%d = %s(ad_, 0, 0u);" % (rd12, rd))
            return out + ["now += 1;"]
        if cls == "strex":
            kind = (op >> 21) & 3
            if 15 in (rd16, rd12, rd0) or (kind == 1 and (rd0 & 1 or rd0 == 14)):
                return None
            out = ["uint32_t ad_ = r%d;" % rd16, "if (h->cpu.excl) {"]
            if kind == 1:
                out += ["    WR32(ad_, r%d, 0, 0u);" % rd0, "    WR32(ad_ + 4, r%d, 0, 0u);" % (rd0 + 1)]
            else:
                wr = {0: "WR32", 2: "WR8", 3: "WR16"}[kind]
                out.append("    %s(ad_, r%d, 0, 0u);" % (wr, rd0))
            return out + ["    h->cpu.excl = 0;", "    r%d = 0;" % rd12, "} else {", "    r%d = 1;" % rd12, "}"]
        if cls == "umaal":
            if 15 in (rd16, rd12, rd8, rd0):
                return None
            return ["uint64_t r_ = (uint64_t)r%d * r%d + r%d + r%d;" % (rd0, rd8, rd16, rd12),
                    "r%d = (uint32_t)r_;" % rd12, "r%d = (uint32_t)(r_ >> 32);" % rd16]
        if cls == "media":
            dest = rd16 if media_dest_hi(op) else rd12
            if dest == 15:
                return None
            return ["r%d = rc_media(%s, %s, %s, %s, %s, &h->cpu.ge, &h->cpu.q);"
                    % (dest, hx(op), R(rd16), R(rd12), R(rd8), R(rd0))]
        if cls == "media_long":
            if 15 in (rd16, rd12):
                return None
            return ["uint64_t r_ = rc_media_long(%s, r%d, r%d, %s, %s);" % (hx(op), rd12, rd16, R(rd8), R(rd0)),
                    "r%d = (uint32_t)r_;" % rd12, "r%d = (uint32_t)(r_ >> 32);" % rd16]
        if cls == "vfp_dp":
            return ["rc_vfp_dp(&h->cpu, %s);" % hx(op)] if vfp_dp_ok(op) else None
        if cls == "vfp_mrc":
            if ((op >> 21) & 7) == 7 and rd16 == 1 and rd12 == 15:  # VMRS APSR_nzcv, FPSCR
                return ["uint32_t f_ = h->cpu.fpscr;",
                        "fN = f_ >> 31; fZ = (f_ >> 30) & 1; fC = (f_ >> 29) & 1; fV = (f_ >> 28) & 1;"]
            if rd12 == 15:
                return None
            return ["r%d = rc_vfp_mrc(&h->cpu, %s);" % (rd12, hx(op))]
        if cls == "vfp_mcr":
            return None if rd12 == 15 else ["rc_vfp_mcr(&h->cpu, %s, r%d);" % (hx(op), rd12)]
        if cls == "vfp_mrrc":
            if 15 in (rd12, rd16):
                return None
            return ["uint64_t v_ = rc_vfp_mrrc(&h->cpu, %s);" % hx(op), "r%d = (uint32_t)v_;" % rd12,
                    "r%d = (uint32_t)(v_ >> 32);" % rd16]
        if cls == "vfp_mcrr":
            if 15 in (rd12, rd16):
                return None
            return ["rc_vfp_mcrr(&h->cpu, %s, r%d, r%d);" % (hx(op), rd12, rd16)]
        if cls == "vfp_ls":
            if rd16 == 15:
                if op & (1 << 21):
                    return None
                return ["(void)rc_vfp_ls(&h->cpu, mem, %s, %s);" % (hx(op), hx((a + 8) & ~3)), "now += 1;"]
            return ["r%d = rc_vfp_ls(&h->cpu, mem, %s, r%d);" % (rd16, hx(op), rd16), "now += 1;"]
        return None  # svc, cps, setend, udf, media_undef, cp15_other: the interpreter
