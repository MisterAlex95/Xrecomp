"""Xenos shader microcode: decode and disassemble (XBA-270, epic XBA-229 step 3: the start of the translator).

    lines = disassemble(words, stage="vertex")      # words: the shader's dwords as the GPU reads them
    python3 -m x360_tools.xenos_ucode FILE.bin [--pixel]   (big-endian dwords, e.g. what the command processor keeps)

A shader is a control-flow program followed by ALU and fetch instructions, all in 96-bit units (three dwords):
- control flow: two 48-bit instructions per unit (the first in dword 0 and the low half of dword 1, the second in the
  high half of dword 1 and dword 2). exec / exece / conditional execs name a clause: an address and a count of ALU
  / fetch instructions, with two sequence bits per instruction (fetch or ALU, serialize); alloc reserves exports;
  loops, calls, jumps, returns. The control-flow program ends where the first clause starts.
- ALU: one vector operation (to four components) and one scalar operation co-issued, three sources (temporary or
  constant, swizzled, negated, absolute), predication, exports.
- fetch: vertex fetch (from a vertex fetch constant: format, stride, offset) and texture fetch (from a texture fetch
  constant: filters, dimension, LOD, offsets) and the texture queries.

Field layouts follow Xenia's ucode.h (BSD); they were checked against Mesa freedreno's a2xx definitions (Adreno 200
descends from Xenos; instr-a2xx.h, MIT): test_xenos_ucode encodes instructions from Mesa's field order and decodes
them here. Where the two differ, Xenos (Xenia) is followed: 12-bit exec addresses and 13-bit loop / jump / call
addresses (Mesa: 9 + 3 reserved, 10), alloc size 3 bits (Mesa 4), ALU dword 0 bit 7 "abs constants" (Mesa: low
precision), vertex fetch prefetch count and a signed 23-bit offset (Mesa: reserved, 22 bits), texture fetch
use_reg_lod 1 bit + 1 unknown (Mesa: 2 bits). xenos_glsl.py translates shaders to GLSL ES on top of this.
"""

import struct
import sys

CF_NAMES = {0: "nop", 1: "exec", 2: "exece", 3: "cexec", 4: "cexece", 5: "cexec_pred", 6: "cexece_pred",
            7: "loop", 8: "endloop", 9: "ccall", 10: "ret", 11: "cjmp", 12: "alloc", 13: "cexec_pred_clean",
            14: "cexece_pred_clean", 15: "vfetch_end"}
EXEC_OPS = {1, 2, 3, 4, 5, 6, 13, 14}
ALLOC_NAMES = {0: "none", 1: "position", 2: "interpolators/colors", 3: "memory"}

VECTOR = ["add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor", "mad", "cndeq", "cndge",
          "cndgt", "dp4", "dp3", "dp2add", "cube", "max4", "setp_eq_push", "setp_ne_push", "setp_gt_push",
          "setp_ge_push", "kill_eq", "kill_gt", "kill_ge", "kill_ne", "dst", "maxa", None, "nop"]
VECTOR_SRCS = {8: 1, 9: 1, 10: 1, 11: 3, 12: 3, 13: 3, 14: 3, 17: 3, 19: 1, 29: 1}  # default 2
SCALAR = ["adds", "adds_prev", "muls", "muls_prev", "muls_prev2", "maxs", "mins", "seqs", "sgts", "sges", "snes",
          "frcs", "truncs", "floors", "exp", "logc", "log", "rcpc", "rcpf", "rcp", "rsqc", "rsqf", "rsq", "maxas",
          "maxasf", "subs", "subs_prev", "setp_eq", "setp_ne", "setp_gt", "setp_ge", "setp_inv", "setp_pop",
          "setp_clr", "setp_rstr", "kills_eq", "kills_gt", "kills_ge", "kills_ne", "kills_one", "sqrt", None,
          "mulsc", "mulsc", "addsc", "addsc", "subsc", "subsc", "sin", "cos", "retain_prev"] + [None] * 12 + ["nop"]
FETCH_NAMES = {0: "vfetch", 1: "tfetch", 16: "getBCF", 17: "getCompTexLOD", 18: "getGradients", 19: "getWeights",
               24: "setTexLOD", 25: "setGradientH", 26: "setGradientV"}
DIMS = {0: "1D", 1: "2D", 2: "3D", 3: "Cube"}
CHAN = "xyzw01?_"


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def sbits(v, lo, n):
    x = bits(v, lo, n)
    return x - (1 << n) if x & (1 << (n - 1)) else x


def unpack_cf(d0, d1, d2):
    """The two 48-bit control-flow instructions of a 96-bit unit, as (low 32 bits, high 16 bits) pairs."""
    return [(d0, d1 & 0xFFFF), (((d1 >> 16) | (d2 << 16)) & 0xFFFFFFFF, d2 >> 16)]


def decode_cf(w0, w1):
    op = bits(w1, 12, 4)
    cf = {"op": op, "name": CF_NAMES[op], "absolute": bits(w1, 11, 1)}
    if op in EXEC_OPS:
        cf.update(address=bits(w0, 0, 12), count=bits(w0, 12, 3), yield_=bits(w0, 15, 1), sequence=bits(w0, 16, 12),
                  vc=bits(w0, 28, 4) | bits(w1, 0, 2) << 4)
        if op in (3, 4, 13, 14):  # 13 / 14: cexec on a boolean with a clean predicate (Xenia's ucode.h)
            cf.update(bool_address=bits(w1, 2, 8), condition=bits(w1, 10, 1))
        elif op in (5, 6):
            cf.update(predicate_clean=bits(w1, 9, 1), condition=bits(w1, 10, 1))
        else:
            cf.update(predicate_clean=bits(w1, 9, 1))
    elif op in (7, 8):
        cf.update(address=bits(w0, 0, 13), loop_id=bits(w0, 16, 5))
        if op == 7:
            cf["repeat"] = bits(w0, 13, 1)
        else:
            cf.update(predicated_break=bits(w0, 21, 1), condition=bits(w1, 10, 1))
    elif op in (9, 11):
        cf.update(address=bits(w0, 0, 13), unconditional=bits(w0, 13, 1), predicated=bits(w0, 14, 1),
                  bool_address=bits(w1, 2, 8), condition=bits(w1, 10, 1))
        if op == 11:
            cf["direction"] = bits(w1, 1, 1)
    elif op == 12:
        cf.update(size=bits(w0, 0, 3), unserialized=bits(w1, 8, 1), buffer=bits(w1, 9, 2))
    return cf


def decode_alu(d0, d1, d2):
    a = {"vector_dest": bits(d0, 0, 6), "vector_dest_rel": bits(d0, 6, 1), "abs_constants": bits(d0, 7, 1),
         "scalar_dest": bits(d0, 8, 6), "scalar_dest_rel": bits(d0, 14, 1), "export": bits(d0, 15, 1),
         "vector_mask": bits(d0, 16, 4), "scalar_mask": bits(d0, 20, 4), "vector_clamp": bits(d0, 24, 1),
         "scalar_clamp": bits(d0, 25, 1), "scalar_op": bits(d0, 26, 6),
         "pred_condition": bits(d1, 27, 1), "predicated": bits(d1, 28, 1), "relative": bits(d1, 29, 1),
         "const_1_rel_abs": bits(d1, 30, 1), "const_0_rel_abs": bits(d1, 31, 1), "vector_op": bits(d2, 24, 5)}
    for n, (swz, neg, reg, sel) in enumerate(((16, 26, 16, 31), (8, 25, 8, 30), (0, 24, 0, 29)), start=1):
        a["src%d" % n] = {"swizzle": bits(d1, swz, 8), "negate": bits(d1, neg, 1), "reg": bits(d2, reg, 8),
                          "temp": bits(d2, sel, 1)}
    return a


def decode_fetch(d0, d1, d2):
    op = bits(d0, 0, 5)
    f = {"op": op, "name": FETCH_NAMES.get(op, "fetch%d" % op), "src_reg": bits(d0, 5, 6),
         "src_rel": bits(d0, 11, 1), "dst_reg": bits(d0, 12, 6), "dst_rel": bits(d0, 18, 1),
         "dst_swizzle": bits(d1, 0, 12), "predicated": bits(d1, 31, 1), "pred_condition": bits(d2, 31, 1)}
    if op == 0:
        f.update(mini=bits(d1, 30, 1), const_index=bits(d0, 20, 5), const_index_sel=bits(d0, 25, 2),
                 prefetch_count=bits(d0, 27, 3), src_swizzle=bits(d0, 30, 2), signed=bits(d1, 12, 1),
                 integer=bits(d1, 13, 1), format=bits(d1, 16, 6), exp_adjust=sbits(d1, 24, 6),
                 stride=bits(d2, 0, 8), offset=sbits(d2, 8, 23))
    else:
        f.update(const_index=bits(d0, 20, 5), unnormalized=bits(d0, 25, 1), src_swizzle=bits(d0, 26, 6),
                 mag_filter=bits(d1, 12, 2), min_filter=bits(d1, 14, 2), mip_filter=bits(d1, 16, 2),
                 aniso_filter=bits(d1, 18, 3), use_comp_lod=bits(d1, 28, 1), use_reg_lod=bits(d1, 29, 1),
                 use_reg_gradients=bits(d2, 0, 1), sample_location=bits(d2, 1, 1), lod_bias=sbits(d2, 2, 7),
                 dimension=bits(d2, 14, 2), offset_x=sbits(d2, 16, 5), offset_y=sbits(d2, 21, 5),
                 offset_z=sbits(d2, 26, 5))
    return f


# ---- text ----

def _swizzle(swz):
    if swz == 0:
        return ""
    return "." + "".join("xyzw"[(i + ((swz >> (2 * i)) & 3)) & 3] for i in range(4))


def _src(s, abs_constants):
    """A source: a temporary (6-bit number, bit 6 relative, bit 7 absolute value) or a constant (8-bit index, absolute
    value for every constant when the instruction says so)."""
    if s["temp"]:
        num, absolute, rel = s["reg"] & 0x3F, s["reg"] >> 7, s["reg"] >> 6 & 1
        t = "r%s%d" % ("[aL+]" if rel else "", num)
    else:
        num, absolute = s["reg"], abs_constants
        t = "c%d" % num
    t = "%s%s%s" % ("-" if s["negate"] else "", t, _swizzle(s["swizzle"]))
    return "|%s|" % t if absolute else t


def _dst(reg, mask, export, stage):
    if export:
        if stage == "vertex":
            name = {62: "oPos", 63: "oPts", 61: "oFog?"}.get(reg, "o%d" % reg)
        else:
            name = {61: "oDepth"}.get(reg, "oC%d" % reg)
    else:
        name = "r%d" % reg
    return name + ("" if mask == 0xF else "." + "".join("xyzw"[i] if mask >> i & 1 else "_" for i in range(4)))


def alu_text(a, stage):
    out = []
    pred = ("(p0) " if a["pred_condition"] else "(!p0) ") if a["predicated"] else ""
    vop = a["vector_op"]
    if a["vector_mask"] or a["export"]:
        n = VECTOR_SRCS.get(vop, 2)
        srcs = [a["src1"], a["src2"], a["src3"]][:n]
        name = VECTOR[vop] or "vop%d" % vop
        out.append("%s%s%s %s, %s" % (pred, name, "_sat" if a["vector_clamp"] else "",
                                      _dst(a["vector_dest"], a["vector_mask"], a["export"], stage),
                                      ", ".join(_src(s, a["abs_constants"]) for s in srcs)))
    sop = a["scalar_op"]
    if a["scalar_mask"] or (not a["vector_mask"] and sop != 63):
        name = SCALAR[sop] if sop < len(SCALAR) and SCALAR[sop] else "sop%d" % sop
        out.append("%s%s%s %s, %s" % (pred, name, "_sat" if a["scalar_clamp"] else "",
                                      _dst(a["scalar_dest"], a["scalar_mask"], a["export"], stage),
                                      _src(a["src3"], a["abs_constants"])))
    return out or ["%snop" % pred]


def fetch_text(f):
    dst = "r%d.%s" % (f["dst_reg"], "".join(CHAN[(f["dst_swizzle"] >> (3 * i)) & 7] for i in range(4)))
    pred = ("(p0) " if f["pred_condition"] else "(!p0) ") if f["predicated"] else ""
    if f["op"] == 0:
        return "%svfetch%s %s, r%d.%s, vf%d, format %d, stride %d, offset %d%s" % (
            pred, "_mini" if f["mini"] else "", dst, f["src_reg"], "xyzw"[f["src_swizzle"]],
            f["const_index"] * 3 + f["const_index_sel"], f["format"], f["stride"], f["offset"],
            ", signed" if f["signed"] else "")
    src = "r%d.%s" % (f["src_reg"], "".join("xyzw"[(f["src_swizzle"] >> (2 * i)) & 3] for i in range(3)))
    return "%s%s%s %s, %s, tf%d" % (pred, f["name"], DIMS[f["dimension"]] if f["op"] == 1 else "", dst, src,
                                    f["const_index"])


def disassemble(words, stage="vertex"):
    """Lines of text: the control-flow program, each clause's instructions under it."""
    if len(words) % 3:
        words = list(words) + [0] * (3 - len(words) % 3)
    lines = []
    cfs = []
    limit = len(words) // 3
    k = 0
    while k < limit:
        for w0, w1 in unpack_cf(*words[3 * k:3 * k + 3]):
            cf = decode_cf(w0, w1)
            cfs.append(cf)
            if cf["op"] in EXEC_OPS and cf["count"]:
                limit = min(limit, cf["address"])
        k += 1
    for cf in cfs:
        name = cf["name"]
        if cf["op"] in EXEC_OPS:
            cond = ""
            if "bool_address" in cf:
                cond = " %sb%d" % ("" if cf["condition"] else "!", cf["bool_address"])
            elif cf["op"] in (5, 6):
                cond = " %sp0" % ("" if cf["condition"] else "!")
            lines.append("%s%s addr %d, count %d" % (name, cond, cf["address"], cf["count"]))
            seq = cf["sequence"]
            for i in range(cf["count"]):
                at = cf["address"] + i
                d = words[3 * at:3 * at + 3] if 3 * at + 3 <= len(words) else [0, 0, 0]
                sync = "(S) " if seq >> (2 * i + 1) & 1 else ""
                if seq >> (2 * i) & 1:
                    lines.append("    %d: %s%s" % (at, sync, fetch_text(decode_fetch(*d))))
                else:
                    for j, t in enumerate(alu_text(decode_alu(*d), stage)):
                        lines.append("    %s %s%s" % ("%d:" % at if j == 0 else " " * (len(str(at)) + 1),
                                                     sync if j == 0 else "", t if j == 0 else "+ " + t))
        elif cf["op"] == 12:
            lines.append("alloc %s, size %d" % (ALLOC_NAMES[cf["buffer"]], cf["size"]))
        elif cf["op"] in (7, 8, 9, 11):
            lines.append("%s addr %d%s" % (name, cf["address"],
                                           ", loop %d" % cf["loop_id"] if "loop_id" in cf else ""))
        elif cf["op"] == 0:
            continue
        else:
            lines.append(name)
        if cf["op"] in (2, 4, 6, 14) or cf["op"] == 10:
            if cf["op"] == 2:
                break
    return lines


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    stage = "pixel" if "--pixel" in argv else "vertex"
    path = [a for a in argv if not a.startswith("--")][0]
    raw = open(path, "rb").read()
    words = list(struct.unpack(">%dI" % (len(raw) // 4), raw[:len(raw) // 4 * 4]))
    print("\n".join(disassemble(words, stage)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
