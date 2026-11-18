"""Xenos shader microcode, assembled (XBA-270): the encoders the tests and the draw test use to write shaders without
title data -- ALU and fetch instructions (the field layouts xenos_ucode decodes), control-flow instructions and whole
programs (control flow first, then the clauses)."""


def alu_words(vop=0, sop=50, vdest=0, sdest=0, vmask=0, smask=0, srcs=((0, 0, 0, 0),) * 3, abs_c=0, export=0,
              vclamp=0, sclamp=0, pred=0, pcond=0, sdest_rel=0, vdest_rel=0, const_0_rel_abs=0, const_1_rel_abs=0,
              relative=0):
    """srcs: three (reg, is_temp, swizzle, negate)."""
    d0 = vdest | vdest_rel << 6 | abs_c << 7 | sdest << 8 | sdest_rel << 14 | export << 15 | vmask << 16 | \
        smask << 20 | vclamp << 24 | sclamp << 25 | sop << 26
    (r1, t1, w1, n1), (r2, t2, w2, n2), (r3, t3, w3, n3) = srcs
    d1 = w3 | w2 << 8 | w1 << 16 | n3 << 24 | n2 << 25 | n1 << 26 | pcond << 27 | pred << 28 | relative << 29 | \
        const_1_rel_abs << 30 | const_0_rel_abs << 31
    d2 = r3 | r2 << 8 | r1 << 16 | vop << 24 | t3 << 29 | t2 << 30 | t1 << 31
    return [d0, d1, d2]


def fetch_words(op, src_reg, dst_reg, dst_swz, const_index, **kw):
    """op 0: vertex fetch (sel, src_swz, format, stride, offset in dwords); 1: texture fetch (dimension, offsets...);
    24-26: setTexLOD / setGradientH / V. dst_swz: 3 bits per component (0-3 xyzw, 4: 0, 5: 1, 7: keep)."""
    d0 = op | src_reg << 5 | dst_reg << 12 | const_index << 20
    d1 = dst_swz
    d2 = 0
    if op == 0:
        d0 |= kw.get("sel", 0) << 25 | kw.get("src_swz", 0) << 30
        d1 |= kw.get("format", 0x39) << 16 | kw.get("mini", 0) << 30
        d2 = kw.get("stride", 4) | (kw.get("offset", 0) & 0x7FFFFF) << 8
    else:
        d0 |= kw.get("src_swz", 0b100100) << 26 | kw.get("unnormalized", 0) << 25
        d1 |= kw.get("use_reg_lod", 0) << 29
        d2 = kw.get("dimension", 1) << 14 | kw.get("use_reg_gradients", 0) | (kw.get("lod_bias", 0) & 0x7F) << 2 | \
            (kw.get("ox", 0) & 31) << 16 | (kw.get("oy", 0) & 31) << 21 | (kw.get("oz", 0) & 31) << 26
    return [d0, d1, d2]


def cf_exec(op, address, count, sequence=0, bool_address=0, condition=0):
    w0 = address | count << 12 | sequence << 16
    w1 = op << 12 | bool_address << 2 | condition << 10
    return w0 | w1 << 32


def program(clauses):
    """clauses: list of (cf op, [("alu"|"fetch", words)], extra) -> dwords: control flow first, then the clauses."""
    ncf = len(clauses)
    units = (ncf + 1) // 2
    cfs, body, at = [], [], units
    for op, instrs, extra in clauses:
        seq = 0
        for i, (kind, _) in enumerate(instrs):
            if kind == "fetch":
                seq |= 1 << (2 * i)
        cfs.append(cf_exec(op, at, len(instrs), seq, **extra))
        for _, w in instrs:
            body += w
        at += len(instrs)
    if len(cfs) % 2:
        cfs.append(0)
    words = []
    for k in range(0, len(cfs), 2):
        v = cfs[k] | cfs[k + 1] << 48
        words += [v & 0xFFFFFFFF, (v >> 32) & 0xFFFFFFFF, (v >> 64) & 0xFFFFFFFF]
    return words + body


def export(o, r):
    return dict(vop=2, sop=50, vdest=o, vmask=0xF, export=1, srcs=((r, 1, 0, 0), (r, 1, 0, 0), (0, 0, 0, 0)))


def loop_const(count, start, step):
    return count | start << 8 | (step & 0xFF) << 16


def cf_loop_start(address, loop_id, repeat=0):
    return (address | repeat << 13 | loop_id << 16) | (7 << 12) << 32


def cf_loop_end(address, loop_id, predicated_break=0, condition=0):
    return (address | loop_id << 16 | predicated_break << 21) | (condition << 10 | 8 << 12) << 32


def cf_branch(op, address, unconditional=0, predicated=0, bool_address=0, condition=0):
    return (address | unconditional << 13 | predicated << 14) | (bool_address << 2 | condition << 10 | op << 12) << 32


def flow_program(entries):
    """entries: ("exec", cf op, [instrs], extra) or ("cf", 48-bit word) -> dwords."""
    units = (len(entries) + 1) // 2
    cfs, body, at = [], [], units
    for e in entries:
        if e[0] == "cf":
            cfs.append(e[1])
            continue
        _, op, instrs, extra = e
        seq = sum(1 << (2 * i) for i, (kind, _) in enumerate(instrs) if kind == "fetch")
        cfs.append(cf_exec(op, at, len(instrs), seq, **extra))
        for _, w in instrs:
            body += w
        at += len(instrs)
    if len(cfs) % 2:
        cfs.append(0)
    words = []
    for k in range(0, len(cfs), 2):
        v = cfs[k] | cfs[k + 1] << 48
        words += [v & 0xFFFFFFFF, (v >> 32) & 0xFFFFFFFF, (v >> 64) & 0xFFFFFFFF]
    return words + body
