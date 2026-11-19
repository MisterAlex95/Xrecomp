"""Generate src/kernel/xbox_interp_gen.inc: the x86 interpreter's x87, SSE,
MMX and system instructions, from the lifter's own statements (XBA-303).

    python3 -m tools.recomp.gen_interp            # writes the .inc
    python3 -m tools.recomp.gen_interp --check    # exit 1 when it is stale

The interpreter (src/kernel/xbox_interp_core.c) decodes an instruction and
runs, for these families, exactly the C the lifter emits for it, so the two
cannot drift apart: a lifter change shows up as a stale .inc
(tools/recomp/test_gen_interp.py). Each encoding is lifted once with sentinel
operands, which are then renamed to the interpreter's run-time fields:

    modrm reg field    xmm2 / mm2 / edx         -> XR / MR / GR
    modrm rm field     xmm6 / mm6 / esi         -> XM / MM_ / GM
    memory operand     [ebx + 0x12345678]       -> _ea
    imm8               0xA5                     -> _imm8
    the instruction    0x00ABC000u              -> _va

and lifted again with other registers and another immediate (xmm1 / xmm0,
0x5A): the two must rename to the same text, or the encoding is expanded
into concrete cases (every register pair, or every immediate) instead. A
leftover sentinel fails the generation. The x87 register forms are concrete
(one case per modrm byte), nothing renamed.

The operands are copies (XR, XM, ...) written back when they changed, so a
statement written for two distinct registers is also right when reg == rm
(the CPU reads every source before it writes).

Instructions whose lifted form talks to the lifter's lazy flags get a hook
the interpreter defines: comiss / ucomiss / comisd / ucomisd store their
operands into _fca / _fcb (INTERP_FCMP_FLAGS), the fcomi family the
condition bits into _fa (INTERP_FCOMI_FLAGS). Anything that lifts to
RECOMP_UNIMPL, a TODO or an operand the lifter cannot write is left out: the
interpreter reports it when it is reached.
"""
import argparse
import os
import re
import sys

from .disasm import Disassembler
from .lifter import Lifter
from .translator import FP_STACK_MACROS

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = os.path.join(ROOT, "src", "kernel", "xbox_interp_gen.inc")

VA = 0x00ABC000
DISP = 0x12345678
PFX = (b"", b"\x66", b"\xF3", b"\xF2")       # index = the key's prefix class

# Two-byte opcodes the interpreter runs by hand (integer, flags) or not at all.
HAND_0F = set(range(0x40, 0x50)) | set(range(0x80, 0xA0)) | {0xA0, 0xA1, 0xA3, 0xA4, 0xA5} \
    | set(range(0xA8, 0xAE)) | {0xAF} | set(range(0xB0, 0xC2)) | {0xC7} | set(range(0xC8, 0xD0)) \
    | {0x0B, 0x18, 0x1F, 0x0D, 0x05, 0x07, 0x0F, 0x38, 0x3A, 0x34, 0x35, 0x00}

FCOMI = {"fcomi", "fcomip", "fcompi", "fucomi", "fucomip", "fucompi"}
COMIS = {"comiss", "comisd", "ucomiss", "ucomisd"}

_dis = None


def _decode(code):
    global _dis
    if _dis is None:
        _dis = Disassembler()
    for cs in _dis._cs.disasm(code, VA, 1):
        return _dis._decode_instruction(cs)
    return None


BAD = ("RECOMP_UNIMPL", "TODO", "unknown operand", "cannot write", "bad operands", "recomp_misdecoded",
       "_flags", "_cf", "_fas", "_fbs", "STK", "unhandled", "not supported")


def _lift(code):
    """(insn, C text) of the bytes, or None when Capstone or the lifter refuse."""
    insn = _decode(code)
    if insn is None:
        return None
    lifter = Lifter()
    lifter.func_start = VA
    lifter.func_end = VA + 0x100
    try:
        lines = lifter.lift_instruction(insn)
    except Exception:
        return None
    if not lines:
        return None
    text = "\n".join(lines)
    if lifter.unimplemented or any(b in text for b in BAD):
        return None
    if insn.mnemonic in COMIS:
        if "_fca" not in text:
            return None
    elif insn.mnemonic in FCOMI:
        if "_fa" not in text:
            return None
    elif re.search(r"\b_f[ab]s?\b", text) or "goto" in text or "return" in text:
        return None
    return insn, text


def _strip_comments(text):
    return re.sub(r"/\*.*?\*/", "", text).strip()


def _rename(text, regs, imm, mem=True):
    """Sentinels to fields. regs: {name: field}."""
    t = _strip_comments(text)
    t = t.replace(f"0x{VA:08X}u", "_va")
    if mem:
        t = t.replace(f"ebx + 0x{DISP:X}", "_ea")
    for name, field in regs.items():
        t = re.sub(rf"\b{name}\b", field, t)
    if imm is not None:
        t = re.sub(rf"\b0x{imm:X}u?\b", "_imm8", t)
        t = re.sub(rf"(?<![\w.]){imm}u?\b", "_imm8", t)
    return t


def _leftovers(t, extra=()):
    bad = [f"{VA:08X}", f"{DISP:X}", "ebx"] + list(extra)
    return [b for b in bad if re.search(rf"\b{b}\b", t) or b in t and len(b) > 4]


def _regsets(insn, reg_n, rm_n):
    """The operand registers the encoding names, by kind, from Capstone's text."""
    names = {}
    for kind, fmt in (("x", "xmm{}"), ("m", "mm{}"), ("g", None)):
        pass
    xr, xm = f"xmm{reg_n}", f"xmm{rm_n}"
    mr, mm = f"mm{reg_n}", f"mm{rm_n}"
    gpr = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
    ops = insn.op_str
    toks = set(re.findall(r"[a-z0-9]+", ops))
    r = None
    m = None
    if xr in toks: r = "x"
    elif mr in toks: r = "m"
    elif gpr[reg_n] in toks: r = "g"
    if xm in toks: m = "x"
    elif mm in toks: m = "m"
    elif gpr[rm_n] in toks: m = "g"
    return r, m


FIELD = {("r", "x"): "XR", ("r", "m"): "MR", ("r", "g"): "GR",
         ("m", "x"): "XM", ("m", "m"): "MM_", ("m", "g"): "GM"}


def _rename_map(r, m, reg_n, rm_n):
    gpr = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
    out = {}
    for side, kind, n in (("r", r, reg_n), ("m", m, rm_n)):
        if kind == "x":
            out[f"xmm{n}"] = FIELD[(side, kind)]
        elif kind == "m":
            out[f"mm{n}"] = FIELD[(side, kind)]
        elif kind == "g":
            out[gpr[n]] = FIELD[(side, kind)]
    return out


def _form(prefix, opcode, reg, rm, mod3, imm, has_imm):
    modrm = ((3 if mod3 else 2) << 6) | (reg << 3) | (rm if mod3 else 3)
    code = prefix + opcode + bytes([modrm])
    if not mod3:
        code += DISP.to_bytes(4, "little")
    if has_imm:
        code += bytes([imm])
    return code


class Case:
    """One generated case: a C body with its operand kinds, or a concrete
    sub-switch."""

    def __init__(self, r, m, body, mnem):
        self.r, self.m, self.body, self.mnem = r, m, body, mnem


def _one(prefix, opcode, reg, mod3, has_imm, group):
    """The renamed body of one modrm form, or a dict of concrete bodies, or None."""
    s_reg = reg if group else 2
    v_reg = reg if group else 1
    first = _lift(_form(prefix, opcode, s_reg, 6, mod3, 0xA5, has_imm))
    if first is None:
        return None
    insn, text = first
    r, m = _regsets(insn, s_reg, 6)
    if group:
        r = None
    if not mod3:
        m = None
    ren = _rename_map(r, m, s_reg, 6)
    body = _rename(text, ren, 0xA5 if has_imm else None, mem=not mod3)
    second = _lift(_form(prefix, opcode, v_reg, 0, mod3, 0x5A, has_imm))
    ok = second is not None and second[0].mnemonic == insn.mnemonic
    if ok:
        r2, m2 = _regsets(second[0], v_reg, 0)
        if group:
            r2 = None
        if not mod3:
            m2 = None
        ok = (r2, m2) == (r, m)
    if ok:
        body2 = _rename(second[1], _rename_map(r, m, v_reg, 0), 0x5A if has_imm else None, mem=not mod3)
        ok = body2 == body
    left = _leftovers(body, [f"xmm{s_reg}", f"xmm6", f"mm{s_reg}", "mm6"] if (r or m) else [])
    if ok and not left:
        return Case(r, m, body, insn.mnemonic)
    if has_imm:
        by_imm = _imm_expanded(prefix, opcode, s_reg, v_reg, mod3, group, r, m)
        if by_imm:
            groups = {}
            for im, b in sorted(by_imm.items()):
                groups.setdefault(b, []).append(im)
            sw = ["switch (_imm8) {"]
            for b, ims in groups.items():
                sw += [f"case 0x{im:02X}u:" for im in ims]
                sw += ["    " + line for line in b.splitlines()]
                sw.append("    break;")
            sw += ["default: return 0;", "}"]
            return Case(r, m, "\n".join(sw), insn.mnemonic)
    return _concrete(prefix, opcode, reg, mod3, has_imm, group, insn.mnemonic)


def _imm_expanded(prefix, opcode, s_reg, v_reg, mod3, group, r, m):
    """{imm: body} with the registers renamed, for an encoding whose lifted
    form depends on the immediate (cmpps's predicate); None when the
    registers do not rename either."""
    out = {}
    for im in range(256):
        a = _lift(_form(prefix, opcode, s_reg, 6, mod3, im, True))
        if a is None:
            continue
        b = _lift(_form(prefix, opcode, v_reg, 0, mod3, im, True))
        if b is None:
            return None
        ta = _rename(a[1], _rename_map(r, m, s_reg, 6), None, mem=not mod3)
        tb = _rename(b[1], _rename_map(r, m, v_reg, 0), None, mem=not mod3)
        if ta != tb or _leftovers(ta, ["xmm6", "mm6", f"xmm{s_reg}", f"mm{s_reg}"] if (r or m) else []):
            return None
        out[im] = ta
    return out or None


def _concrete(prefix, opcode, reg, mod3, has_imm, group, mnem):
    """Every register pair (and immediate) as its own case: {(reg, rm, imm): body}."""
    out = {}
    regs = [reg] if group else range(8)
    rms = range(8) if mod3 else [3]
    imms = range(256) if has_imm else [None]
    for rg in regs:
        for rm in rms:
            for im in imms:
                code = _form(prefix, opcode, rg, rm, mod3, im or 0, has_imm)
                if not mod3:
                    code = prefix + opcode + bytes([(2 << 6) | (rg << 3) | 3]) + DISP.to_bytes(4, "little") \
                        + (bytes([im]) if has_imm else b"")
                got = _lift(code)
                if got is None:
                    continue            # left to the interpreter's report
                body = _rename(got[1], {}, None, mem=not mod3)
                if _leftovers(body):
                    continue
                out[(rg, rm if mod3 else None, im)] = body
    return out or None


def _shape(prefix, opcode):
    """(has_modrm, has_imm8, is_group) of a two-byte opcode under a prefix, or None."""
    base = _decode(prefix + opcode + bytes([0x93]) + DISP.to_bytes(4, "little") + b"\xA5\xA5")
    n = len(prefix) + len(opcode)
    if base is None:
        # register forms only (movmskps, pextrw, the shifts by an immediate)
        reg = _decode(prefix + opcode + bytes([0xD6]) + b"\xA5\xA5")
        if reg is None or reg.size not in (n + 1, n + 2):
            return None
        mn = set()
        for r in range(8):
            d = _decode(prefix + opcode + bytes([0xC6 | (r << 3)]) + b"\xA5\xA5")
            mn.add(d.mnemonic if d else None)
        return (True, reg.size == n + 2, len(mn) > 1)
    if base.size == n:
        return (False, False, False)
    if base.size == n + 5:
        imm = False
    elif base.size == n + 6:
        imm = True
    else:
        return None
    mn = set()
    for reg in range(8):
        d = _decode(prefix + opcode + bytes([0x83 | (reg << 3)]) + DISP.to_bytes(4, "little") + b"\xA5\xA5")
        mn.add(d.mnemonic if d else None)
    return (True, imm, len(mn) > 1)


def _x87():
    """{(op, 'm', reg): body} for memory forms, {(op, 'r', modrm): body} for register forms."""
    out = {}
    for op in range(0xD8, 0xE0):
        for reg in range(8):
            code = bytes([op, (2 << 6) | (reg << 3) | 3]) + DISP.to_bytes(4, "little")
            got = _lift(code)
            if got:
                body = _rename(got[1], {}, None)
                if not _leftovers(body):
                    out[(op, "m", reg)] = (got[0].mnemonic, body)
        for modrm in range(0xC0, 0x100):
            got = _lift(bytes([op, modrm]))
            if got:
                body = _rename(got[1], {}, None, mem=False)
                if not _leftovers(body):
                    out[(op, "r", modrm)] = (got[0].mnemonic, body)
    return out


# One-byte system instructions: (bytes without the immediate, has imm8, prefix 66 too)
ONE_BYTE = [(0xF4, False), (0xFA, False), (0xFB, False), (0xCC, False), (0xCD, True), (0xCE, False),
            (0xE4, True), (0xE5, True), (0xE6, True), (0xE7, True),
            (0xEC, False), (0xED, False), (0xEE, False), (0xEF, False)]


def _one_byte():
    out = {}
    for op, has_imm in ONE_BYTE:
        for p in (0, 1):
            prefix = PFX[p]
            imms = range(256) if has_imm else [None]
            bodies = {}
            for im in imms:
                code = prefix + bytes([op]) + (bytes([im]) if im is not None else b"")
                got = _lift(code)
                if not got:
                    continue
                body = _rename(got[1], {}, None, mem=False)
                if _leftovers(body):
                    continue
                bodies[im] = (got[0].mnemonic, body)
            if bodies:
                out[(p, op)] = bodies
    return out


def _indent(body, n):
    pad = " " * n
    return "\n".join(pad + line.strip() for line in body.splitlines() if line.strip())


def _frame(case, indent):
    """C for one case with its operand copies."""
    pad = " " * indent
    pre, post = [], []
    if case.r == "x":
        pre.append("RecompXmm XR = *gen_xmm(c, d->reg), XR0 = XR;")
        post.append("if (memcmp(&XR, &XR0, sizeof XR)) *gen_xmm(c, d->reg) = XR;")
    elif case.r == "m":
        pre.append("RecompMmx MR = *gen_mmx(c, d->reg), MR0 = MR;")
        post.append("if (MR.q != MR0.q) *gen_mmx(c, d->reg) = MR;")
    elif case.r == "g":
        pre.append("uint32_t GR = *REGP(c, d->reg), GR0 = GR;")
        post.append("if (GR != GR0) *REGP(c, d->reg) = GR;")
    if case.m == "x":
        pre.append("RecompXmm XM = *gen_xmm(c, d->rm), XM0 = XM;")
        post.insert(0, "if (memcmp(&XM, &XM0, sizeof XM)) *gen_xmm(c, d->rm) = XM;")
    elif case.m == "m":
        pre.append("RecompMmx MM_ = *gen_mmx(c, d->rm), MM_0 = MM_;")
        post.insert(0, "if (MM_.q != MM_0.q) *gen_mmx(c, d->rm) = MM_;")
    elif case.m == "g":
        pre.append("uint32_t GM = *REGP(c, d->rm), GM0 = GM;")
        post.insert(0, "if (GM != GM0) *REGP(c, d->rm) = GM;")
    hook = ""
    if case.mnem in COMIS:
        hook = "INTERP_FCMP_FLAGS();"
    elif case.mnem in FCOMI:
        hook = "INTERP_FCOMI_FLAGS();"
    lines = [f"{pad}{{ /* {case.mnem} */"]
    lines += [f"{pad}    {p}" for p in pre]
    lines.append(_indent(case.body, indent + 4))
    if hook:
        lines.append(f"{pad}    {hook}")
    lines += [f"{pad}    {p}" for p in post]
    lines.append(f"{pad}    return 1;")
    lines.append(f"{pad}}}")
    return "\n".join(lines)


def _concrete_c(table, mnem, mod3, indent):
    """A switch over the concrete bodies, grouped by identical text."""
    pad = " " * indent
    hook = "INTERP_FCMP_FLAGS(); " if mnem in COMIS else "INTERP_FCOMI_FLAGS(); " if mnem in FCOMI else ""
    by_body = {}
    for (rg, rm, im), body in table.items():
        key = (rg if True else 0) * 0x10000 + ((rm or 0) * 0x100) + (im or 0)
        by_body.setdefault(body, []).append(key)
    lines = [f"{pad}switch (((uint32_t)d->reg << 16) | ((uint32_t)(d->mod == 3 ? d->rm : 0) << 8) | "
             f"{'(d->imm & 0xFFu)' if any(k[2] is not None for k in table) else '0u'}) {{ /* {mnem} */"]
    for body, keys in by_body.items():
        for k in sorted(keys):
            lines.append(f"{pad}case 0x{k:06X}u:")
        lines.append(_indent(body, indent + 4))
        if hook:
            lines.append(f"{pad}    {hook}")
        lines.append(f"{pad}    return 1;")
    lines.append(f"{pad}}}")
    lines.append(f"{pad}return 0;")
    return "\n".join(lines)


def generate(log=lambda *a: None):
    shapes = {}            # (pfx, op) -> flags
    bodies0f = {}          # (pfx, op) -> {"r": case or dict or {reg: ...}, "m": ...}
    for p, prefix in enumerate(PFX):
        for op in range(256):
            if op in HAND_0F:
                continue
            opcode = bytes([0x0F, op])
            sh = _shape(prefix, opcode)
            if sh is None:
                continue
            has_modrm, has_imm, group = sh
            if not has_modrm:
                got = _lift(prefix + opcode)
                if not got:
                    continue
                body = _rename(got[1], {}, None, mem=False)
                if _leftovers(body):
                    continue
                shapes[(p, op)] = 0x10
                bodies0f[(p, op)] = {"none": Case(None, None, body, got[0].mnemonic)}
                continue
            entry = {}
            regs = range(8) if group else [None]
            for mod3 in (True, False):
                for reg in regs:
                    got = _one(prefix, opcode, reg or 0, mod3, has_imm, group)
                    if got is not None:
                        entry[(mod3, reg)] = got
            if not entry and op in (0x20, 0x21, 0x22, 0x23):
                pass
            if entry:
                shapes[(p, op)] = 0x10 | 0x01 | (0x02 if has_imm else 0)
                bodies0f[(p, op)] = entry
            else:
                log(f"  skipped {prefix.hex()} 0f {op:02x}")
    # mov to / from control and debug registers: every pair concrete
    for op in (0x20, 0x21, 0x22, 0x23):
        table = {}
        mnem = None
        for rg in range(8):
            for rm in range(8):
                got = _lift(bytes([0x0F, op, 0xC0 | (rg << 3) | rm]))
                if not got:
                    continue
                body = _rename(got[1], {}, None, mem=False)
                if _leftovers(body):
                    continue
                table[(rg, rm, None)] = body
                mnem = got[0].mnemonic
        if table:
            shapes[(0, op)] = 0x10 | 0x01
            bodies0f[(0, op)] = {(True, None): table, "mnem": mnem}
    x87 = _x87()
    one = _one_byte()
    return shapes, bodies0f, x87, one


HEADER = """/* xbox_interp_gen.inc - GENERATED by tools/recomp/gen_interp.py from the
 * lifter (tools/recomp/lifter.py); do not edit. Regenerate with
 *     python3 -m tools.recomp.gen_interp
 * The x86 interpreter's x87, SSE, MMX and system instructions: the lifter's
 * own statement for each encoding, its operands renamed to the decoded
 * instruction's (XBA-303, src/kernel/xbox_interp_core.c). */
"""


def emit(shapes, bodies0f, x87, one):
    out = [HEADER]
    out.append("/* The x87 stack accessors the lifted statements are written against"
               " (tools/recomp/translator.py FP_STACK_MACROS). */")
    for line in FP_STACK_MACROS:
        if "fpl_" in line or line.strip().startswith("\"") or "_fs[" in line:
            continue
        out.append(line.strip() if not line.startswith("        ") else "    " + line.strip())
    out.append("/* ebp is a C local in lifted code; the interpreter keeps it in g_ebp. */")
    out.append("#define ebp g_ebp")
    out.append("")
    out.append("static unsigned gen_0f_shape(unsigned pfx, unsigned op)\n{\n    switch ((pfx << 8) | op) {")
    for (p, op), fl in sorted(shapes.items()):
        out.append(f"    case 0x{(p << 8) | op:03X}u: return 0x{fl:02X}u;")
    out.append("    }\n    return 0;\n}\n")

    out.append("static int gen_exec_0f(XRun *x)\n{\n    RecompCpu *c = x->c;\n    const XInsn *d = x->d;\n"
               "    uint32_t _ea = x->ea, _imm8 = d->imm & 0xFFu, _va = d->va;\n"
               "    double _fca = 0, _fcb = 0;\n    uint32_t _fa = 0;\n"
               "    unsigned pfx = d->rep == 0xF3 ? 2u : d->rep == 0xF2 ? 3u : d->pfx66 ? 1u : 0u;\n"
               "    (void)_ea; (void)_imm8; (void)_va; (void)_fca; (void)_fcb; (void)_fa; (void)c;\n"
               "    switch ((pfx << 8) | d->op2) {")
    for (p, op), entry in sorted(bodies0f.items()):
        out.append(f"    case 0x{(p << 8) | op:03X}u:")
        if "none" in entry:
            out.append(_frame(entry["none"], 8))
            continue
        if "mnem" in entry:
            out.append(_concrete_c(entry[(True, None)], entry["mnem"], True, 8))
            continue
        for mod3 in (True, False):
            forms = {reg: v for (m3, reg), v in entry.items() if m3 == mod3}
            if not forms:
                continue
            out.append(f"        if ({'d->mod == 3' if mod3 else 'd->mod != 3'}) {{")
            if None in forms:
                v = forms[None]
                out.append(_frame(v, 12) if isinstance(v, Case) else _concrete_c(v, "", mod3, 12))
            else:
                out.append("            switch (d->reg) {")
                for reg, v in sorted(forms.items()):
                    out.append(f"            case {reg}:")
                    out.append(_frame(v, 16) if isinstance(v, Case) else _concrete_c(v, "", mod3, 16))
                out.append("            }")
            out.append("            return 0;")
            out.append("        }")
        out.append("        return 0;")
    out.append("    }\n    return 0;\n}\n")

    out.append("static int gen_exec_x87(XRun *x)\n{\n    RecompCpu *c = x->c;\n    const XInsn *d = x->d;\n"
               "    uint32_t _ea = x->ea, _va = d->va, _fa = 0;\n    (void)_ea; (void)_va; (void)_fa; (void)c;\n"
               "    if (d->mod != 3) {\n        switch ((d->op << 8) | d->reg) {")
    for (op, kind, k), (mnem, body) in sorted(x87.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2])):
        if kind != "m":
            continue
        out.append(f"        case 0x{(op << 8) | k:04X}u: /* {mnem} */")
        out.append(_indent(body, 12))
        out.append("            return 1;")
    out.append("        }\n        return 0;\n    }\n    switch ((d->op << 8) | 0xC0u | (d->reg << 3) | d->rm) {")
    for (op, kind, k), (mnem, body) in sorted(x87.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2])):
        if kind != "r":
            continue
        hook = " INTERP_FCOMI_FLAGS();" if mnem in FCOMI else ""
        out.append(f"    case 0x{(op << 8) | k:04X}u: /* {mnem} */")
        out.append(_indent(body, 8) + ("\n       " + hook if hook else ""))
        out.append("        return 1;")
    out.append("    }\n    return 0;\n}\n")

    out.append("static int gen_exec_1b(XRun *x)\n{\n    RecompCpu *c = x->c;\n    const XInsn *d = x->d;\n"
               "    uint32_t _va = d->va;\n    (void)_va; (void)c;\n"
               "    if (d->op >= 0xD8 && d->op <= 0xDF)\n        return gen_exec_x87(x);\n"
               "    switch ((d->pfx66 ? 0x100u : 0u) | d->op) {")
    for (p, op), bodies in sorted(one.items()):
        out.append(f"    case 0x{(p << 8) | op:03X}u:")
        if list(bodies.keys()) == [None]:
            mnem, body = bodies[None]
            out.append(f"        /* {mnem} */")
            out.append(_indent(body, 8))
            out.append("        return 1;")
            continue
        by_body = {}
        for im, (mnem, body) in bodies.items():
            by_body.setdefault(body, []).append(im)
        out.append("        switch (d->imm & 0xFFu) {")
        for body, ims in by_body.items():
            for im in sorted(ims):
                out.append(f"        case 0x{im:02X}u:")
            out.append(_indent(body, 12))
            out.append("            return 1;")
        out.append("        }\n        return 0;")
    out.append("    }\n    return 0;\n}\n")
    out.append("#undef ebp")
    return "\n".join(out) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="exit 1 when the .inc is not current")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args(argv)
    text = emit(*generate(log=(lambda m: print(m, file=sys.stderr)) if a.v else (lambda m: None)))
    if a.check:
        cur = open(a.out).read() if os.path.exists(a.out) else ""
        if cur != text:
            print(f"{a.out} is stale: python3 -m tools.recomp.gen_interp", file=sys.stderr)
            return 1
        return 0
    with open(a.out, "w") as f:
        f.write(text)
    print(f"wrote {a.out} ({text.count(chr(10))} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
