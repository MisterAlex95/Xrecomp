"""Xenos shader microcode to GLSL ES 3.00 (XBA-270, epic XBA-229 step 3: the first cut of the shader translator).

    src, info = translate(words, "vertex")        # or "pixel", interpolators=N
    python3 -m x360_tools.xenos_glsl FILE.bin [--pixel] [--interpolators N]

On top of xenos_ucode's decoder. Semantics follow the instruction descriptions in Xenia's ucode.h (BSD; no code
taken), among them: the scalar unit's operands `a` (the W of src3's swizzle) and `b` (X); mulsc / addsc / subsc's
constant + temporary pair; Direct3D 9 multiplication (+-0 or a denormal times anything is +0: mul, mad, dot products,
muls); max / min as `a >= b ? a : b` / `a < b ? a : b`; the predicate and kill instructions; exports (vector and
scalar results both go to the export register, components written by both get 1.0); ps, the previous scalar result,
kept when an instruction's predicate fails.

What the GLSL expects of the host (the renderer, xenos_gl.c): one std140 uniform block per stage,
`XeVertexConstants` / `XePixelConstants` { vec4 c[256] (the stage's float constants); uvec4 b[2] (the 256 boolean
constants); uvec4 l[8] (the 32 loop constants); vec4 at (the alpha test: enable, function, reference, as
RB_COLORCONTROL / RB_ALPHA_REF say; the pixel shader discards a failing colour 0); vec4 pg (the pixel parameter
register SQ_CONTEXT_MISC names, or -1; r[pg.x] gets the pixel's top-left-origin position gl_FragCoord.xy * pg.zw +
(0, pg.y) - 0.5 -- the host's target orientation and internal resolution scale --, X negative for a back face, as
Xenia's registers.h describes PsParamGen); vec4 vp (the guest viewport as a transform
of clip-space xy: host xy = xy * vp.xy + w * vp.zw) } xe_k -- 4304 bytes, the host
picks the binding points (GLES 3.0
guarantees 16 KB blocks, while 256 loose vec4 would take all of a pixel shader's guaranteed uniforms); `in vec4
xe_v<n>` per vertex fetch, its comment giving the fetch constant, offset, format, stride and signedness (the renderer
reads it back and converts formats); `uniform sampler*D xe_t<n>` per texture fetch constant used (info's
`textures`); the interpolators `xe_i<n>`, the vertex shader's outputs (all sixteen, so any pixel shader links) and the
pixel shader's inputs (copied to r0..); gl_Position (Xenos's, before the host's viewport conversion) and gl_PointSize;
the pixel shader's `layout(location = n) out vec4 xe_color<n>` and gl_FragDepth (written only when exported).

Control flow: exec clauses (conditional on a boolean or p0, ends only when the condition holds), and when a shader
has loops, calls or jumps, a state machine on the control-flow index with the sequencer's four-deep loop (counter, aL)
and return stacks (`xe_l`, in the block: the 32 loop constants, count / start / signed step bytes). Relative
addressing: r[aL + n], c[aL + n], c[a0 + n] (clamped to the register files).

Textures: 1D / 2D / 3D / cube fetches (cube: tfetchCube's face coordinates turned back into a direction), offsets
(half texels), LOD bias (1/16), register LOD (setTexLOD) and gradients (setGradientH / V).

Memory export (XBA-282, translate(memexport=True)): eA (export register 32) and eM0-4 (33-37) are kept in
variables and flushed, at the next eA write and at the end, into a shader storage buffer as a log of (byte address,
dword) pairs the renderer applies to guest memory after the draw (xenos_gl.c, GLES 3.1 with vertex storage blocks,
else the draw is skipped). eA as Xenia reads it: X the base in dwords (top bits 01), Y the element index, W the
index bound (both floats in [2^23, 2^24): the integer is the mantissa), Z the format (bits 8-13), endianness (0-2),
signedness (16), integer (17), red / blue swap (19); element k lands at base + (index + k) * element size. Formats
8_8_8_8, 2_10_10_10, 16_16, 16_16_16_16, their _FLOAT forms, 32_FLOAT, 32_32_FLOAT, 32_32_32_32_FLOAT; other formats
write nothing. Such a shader is GLSL ES 3.10; without memexport=True it is Unsupported, as before.

Not translated yet (Unsupported is raised, naming the instruction): the texture queries (getBCF,
getCompTexLOD, getGradients, getWeights); multiply-add is
written unfused (Xenos rounds once: small differences); log / rsq / sqrt of negative numbers are left to the host
(GLSL ES leaves them undefined: llvmpipe gives 0). Integer and signed-normalised vertex formats are the host's
conversion, exp_adjust too.

Vertex fetches: at the untouched r0.x (the vertex index) a fetch is an attribute `xe_v<n>`; at any other index
(another register or component, or r0 once the shader wrote it: instancing's index / count) it reads `uniform highp
sampler2D xe_vb<n>`, the whole buffer converted by the renderer to vec4s (texelFetch, 1024 elements a row: GLES 3.0
has no buffer textures), at the index truncated.
"""

import re
import struct
import sys

from .xenos_ucode import EXEC_OPS, VECTOR, decode_alu, decode_cf, decode_fetch, unpack_cf

FLT_MAX = "3.402823466e+38"

# Stamped in every translation; the renderer (xenos_gl.c XENOS_GLSL_STAMP) refuses a cached shader without it, so a
# cache made before a change of the host contract is translated again (bringup does it) rather than drawn wrong.
# 2: XBA-280 (PsParamGen from pg.zw, xe_vb<n> buffers for computed-index fetches).
GLSL_VERSION = 2


class Unsupported(Exception):
    pass


PRELUDE = """const float XE_FLT_MAX = 3.402823466e+38;
const float XE_TINY = 1.17549435e-38;
float xe_mul(float a, float b) { return (abs(a) < XE_TINY || abs(b) < XE_TINY) ? 0.0 : a * b; }
vec4 xe_mul(vec4 a, vec4 b) {
    bvec4 z = bvec4(uvec4(lessThan(abs(a), vec4(XE_TINY))) | uvec4(lessThan(abs(b), vec4(XE_TINY))));
    return mix(a * b, vec4(0.0), z);
}
vec4 xe_max(vec4 a, vec4 b) { return mix(b, a, greaterThanEqual(a, b)); }
vec4 xe_min(vec4 a, vec4 b) { return mix(b, a, lessThan(a, b)); }
float xe_dot4(vec4 a, vec4 b) { vec4 m = xe_mul(a, b); return m.x + m.y + m.z + m.w; }
float xe_dot3(vec4 a, vec4 b) { vec4 m = xe_mul(a, b); return m.x + m.y + m.z; }
float xe_max4(vec4 a) {
    if (a.x > a.y && a.x > a.z && a.x > a.w) return a.x;
    if (a.y > a.z && a.y > a.w) return a.y;
    return a.z > a.w ? a.z : a.w;
}
bool xe_bool(int n) { return ((xe_b[n >> 7][(n >> 5) & 3] >> uint(n & 31)) & 1u) != 0u; }
uint xe_loop(int n) { return xe_l[n >> 2][n & 3]; }
float xe_clampinf(float t, float lo, float hi) { return isinf(t) ? (t < 0.0 ? lo : hi) : t; }
// cube: (T, S, 2 * major axis, face) of the direction in src0's z, w, x (shaders pass .zzxy)
vec4 xe_cube(vec4 s) {
    float x = s.z, y = s.w, z = s.x;
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return vec4(-y, z < 0.0 ? -x : x, 2.0 * z, z < 0.0 ? 5.0 : 4.0);
    if (abs(y) >= abs(x)) return vec4(y < 0.0 ? -z : z, x, 2.0 * y, y < 0.0 ? 3.0 : 2.0);
    return vec4(-y, x < 0.0 ? z : -z, 2.0 * x, x < 0.0 ? 1.0 : 0.0);
}
// tfetchCube's (S, T, face), S and T in [1, 2), back to a direction for samplerCube
vec3 xe_cubedir(vec3 c) {
    float s = c.x * 2.0 - 3.0, t = c.y * 2.0 - 3.0;
    int f = int(c.z + 0.5);
    if (f == 0) return vec3(1.0, -t, -s);
    if (f == 1) return vec3(-1.0, -t, s);
    if (f == 2) return vec3(s, 1.0, t);
    if (f == 3) return vec3(s, -1.0, -t);
    if (f == 4) return vec3(s, -t, 1.0);
    return vec3(-s, -t, -1.0);
}
"""

# scalar ops: name -> GLSL of a (and b, ps); None = handled specially
SCALAR_EXPR = {
    0: "A + B", 1: "A + ps", 2: "xe_mul(A, B)", 3: "xe_mul(A, ps)",
    4: "(ps == -XE_FLT_MAX || isinf(ps) || isnan(ps) || isinf(B) || isnan(B) || B <= 0.0) ? -XE_FLT_MAX : "
       "xe_mul(A, ps)",
    5: "(A >= B ? A : B)", 6: "(A < B ? A : B)", 7: "(A == 0.0 ? 1.0 : 0.0)", 8: "(A > 0.0 ? 1.0 : 0.0)",
    9: "(A >= 0.0 ? 1.0 : 0.0)", 10: "(A != 0.0 ? 1.0 : 0.0)", 11: "(A - floor(A))", 12: "trunc(A)", 13: "floor(A)",
    14: "(A == 0.0 ? 1.0 : exp2(A))", 15: "xe_clampinf(A == 1.0 ? 0.0 : log2(A), -XE_FLT_MAX, XE_FLT_MAX)",
    16: "(A == 1.0 ? 0.0 : log2(A))", 17: "xe_clampinf(A == 1.0 ? 1.0 : 1.0 / A, -XE_FLT_MAX, XE_FLT_MAX)",
    18: "xe_clampinf(A == 1.0 ? 1.0 : 1.0 / A, -0.0, 0.0)", 19: "(A == 1.0 ? 1.0 : 1.0 / A)",
    20: "xe_clampinf(A == 1.0 ? 1.0 : inversesqrt(A), -XE_FLT_MAX, XE_FLT_MAX)",
    21: "xe_clampinf(A == 1.0 ? 1.0 : inversesqrt(A), -0.0, 0.0)", 22: "(A == 1.0 ? 1.0 : inversesqrt(A))",
    23: "(A >= B ? A : B)", 24: "(A >= B ? A : B)", 25: "A - B", 26: "A - ps", 40: "sqrt(A)",
    42: "xe_mul(A, B)", 43: "xe_mul(A, B)", 44: "A + B", 45: "A + B", 46: "A - B", 47: "A - B",
    48: "sin(A)", 49: "cos(A)", 50: "ps",
}
SCALAR_TWO = {0, 2, 4, 5, 6, 23, 24, 25}          # operand: W and X of src3
SCALAR_NONE = {33, 50}                            # no operand
SCALAR_CONST_TEMP = {42, 43, 44, 45, 46, 47}      # c.w (op) r.x
SETP = {27: "A == 0.0", 28: "A != 0.0", 29: "A > 0.0", 30: "A >= 0.0"}
KILLS = {35: "A == 0.0", 36: "A > 0.0", 37: "A >= 0.0", 38: "A != 0.0", 39: "A == 1.0"}
PUSH = {20: "==", 21: "!=", 22: ">", 23: ">="}
KILLV = {24: "equal", 25: "greaterThan", 26: "greaterThanEqual", 27: "notEqual"}
VSRCS = {8: 1, 9: 1, 10: 1, 11: 3, 12: 3, 13: 3, 14: 3, 17: 3, 19: 1}


def _subst(expr, a, b):
    """the scalar operands into a SCALAR_EXPR / SETP / KILLS pattern (whole words A and B only)"""
    return re.sub(r"\b[AB]\b", lambda m: a if m.group(0) == "A" else b, expr)


def _comp(swz, j):
    return "xyzw"[(j + ((swz >> (2 * j)) & 3)) & 3]


class _Shader:
    def __init__(self, stage, interpolators, memexport=False):
        self.memexport = memexport
        self.uses_memexport = False
        self.stage = stage
        self.interpolators = interpolators
        self.attributes = []      # (key, dict)
        self.textures = {}        # const index -> dimension
        self.outputs = set()      # vertex interpolators / pixel colours written
        self.body = []
        self.depth = 1
        self.uses_a0 = False
        self.uses_al = False
        self.exports_depth = self.exports_psize = False
        self.last_full = None  # (fetch constant, stride) of the last full vertex fetch, for mini fetches
        self.vbuffers = []        # (key, dict): vertex fetches at a computed index, read from a buffer texture
        self.r0_written = False   # r0 (the vertex index) overwritten so far
        self.force_computed = False

    def emit(self, line):
        self.body.append("    " * self.depth + line)

    # ---- operands ----
    def _const_addressed(self, a, n):
        if n == 1:
            return a["const_0_rel_abs"]
        if n == 2:
            return a["const_0_rel_abs"] if a["src1"]["temp"] else a["const_1_rel_abs"]
        return a["const_0_rel_abs"] if (a["src1"]["temp"] and a["src2"]["temp"]) else a["const_1_rel_abs"]

    def temp(self, n, rel):
        """r[n], or r[aL + n] for loop-relative addressing (clamped: GLSL leaves out-of-range indexing undefined)"""
        if not rel:
            return "r[%d]" % n
        self.uses_al = True
        return "r[clamp(aL + %d, 0, 63)]" % n

    def const(self, a, n, index):
        """c[index], c[a0 + index] or c[aL + index] (the instruction's relative bit picks a0)"""
        if not self._const_addressed(a, n):
            return "xe_c[%d]" % index
        if a["relative"]:
            self.uses_a0 = True
            return "xe_c[clamp(a0 + %d, 0, 255)]" % index
        self.uses_al = True
        return "xe_c[clamp(aL + %d, 0, 255)]" % index

    def operand(self, a, n, where):
        """src n (1..3) as a GLSL vec4 with its swizzle, absolute value and negation."""
        s = a["src%d" % n]
        if s["temp"]:
            base, absolute = self.temp(s["reg"] & 0x3F, s["reg"] & 0x40), s["reg"] >> 7
        else:
            base, absolute = self.const(a, n, s["reg"]), a["abs_constants"]
        e = base + "." + "".join(_comp(s["swizzle"], j) for j in range(4))
        if absolute:
            e = "abs(%s)" % e
        return "(-%s)" % e if s["negate"] else e

    def scalar_operands(self, a, op, where):
        s = a["src3"]
        if op in SCALAR_CONST_TEMP:
            temp = (op & 1) | (s["temp"] << 1) | (s["swizzle"] & 0x3C)
            ca = "%s.%s" % (self.const(a, 3, s["reg"]), _comp(s["swizzle"], 3))
            tb = "r[%d].%s" % (temp, _comp(s["swizzle"], 0))
            if a["abs_constants"]:
                ca, tb = "abs(%s)" % ca, "abs(%s)" % tb
            if s["negate"]:
                ca, tb = "(-%s)" % ca, "(-%s)" % tb
            return ca, tb
        v = self.operand(a, 3, where)
        # the vec4 expression's components are already swizzled: W is a, X is b
        return "(%s).w" % v, "(%s).x" % v

    # ---- destinations ----
    def export_target(self, reg, where):
        if 32 <= reg <= 37 and self.memexport:   # XBA-282: eA, eM0-4
            self.uses_memexport = True
            return "xe_eA" if reg == 32 else "xe_eM[%d]" % (reg - 33)
        if self.stage == "vertex":
            if reg == 62:
                return "xe_pos"
            if reg == 63:
                self.exports_psize = True
                return "xe_psize"
            if reg < 16:
                self.outputs.add(reg)
                return "xe_i%d" % reg
        else:
            if reg < 4:
                self.outputs.add(reg)
                return "xe_color%d" % reg
            if reg == 61:
                self.exports_depth = True
                return "xe_depth"
        raise Unsupported("%s: export to register %d" % (where, reg))

    def write(self, target, mask, value, clamp):
        if not mask:
            return
        comps = "".join("xyzw"[i] for i in range(4) if mask >> i & 1)
        if target == "r[0]" or target.startswith("r[clamp("):
            self.r0_written = True
        v = "clamp(%s, 0.0, 1.0)" % value if clamp else value
        if target == "xe_eA":   # a new export address: what the previous one collected goes out first
            self.emit("if (xe_eMw != 0u) xe_mx_flush();")
        self.emit("%s.%s = (%s).%s;" % (target, comps, v, comps))
        if target.startswith("xe_eM["):
            self.emit("xe_eMw |= %du;" % (1 << int(target[6])))

    # ---- instructions ----
    def alu(self, words, at):
        a = decode_alu(*words)
        where = "alu %d" % at
        vop, sop = a["vector_op"], a["scalar_op"]
        export = a["export"]
        vmask, smask = a["vector_mask"], a["scalar_mask"]
        if export:
            target = self.export_target(a["vector_dest"], where)
            v_write, s_write = vmask & ~smask, smask & ~vmask
            one_mask = vmask & smask
            zero_mask = (0xF & ~(vmask | smask)) if a["scalar_dest_rel"] else 0
            v_target = s_target = target
        else:
            v_write, s_write, one_mask, zero_mask = vmask, smask, 0, 0
            v_target = self.temp(a["vector_dest"], a["vector_dest_rel"])
            s_target = self.temp(a["scalar_dest"], a["scalar_dest_rel"])
        pre, post = [], []
        # vector
        vname = VECTOR[vop] if vop < len(VECTOR) else None
        if vname is None or vname == "nop":
            vexpr = None
        else:
            n = VSRCS.get(vop, 2)
            S = [self.operand(a, k, where) for k in range(1, n + 1)]
            vexpr = {
                0: lambda: "%s + %s" % (S[0], S[1]), 1: lambda: "xe_mul(%s, %s)" % (S[0], S[1]),
                2: lambda: "xe_max(%s, %s)" % (S[0], S[1]), 3: lambda: "xe_min(%s, %s)" % (S[0], S[1]),
                4: lambda: "vec4(equal(%s, %s))" % (S[0], S[1]), 5: lambda: "vec4(greaterThan(%s, %s))" % (S[0], S[1]),
                6: lambda: "vec4(greaterThanEqual(%s, %s))" % (S[0], S[1]),
                7: lambda: "vec4(notEqual(%s, %s))" % (S[0], S[1]),
                8: lambda: "%s - floor(%s)" % (S[0], S[0]), 9: lambda: "trunc(%s)" % S[0],
                10: lambda: "floor(%s)" % S[0], 11: lambda: "xe_mul(%s, %s) + %s" % (S[0], S[1], S[2]),
                12: lambda: "mix(%s, %s, equal(%s, vec4(0.0)))" % (S[2], S[1], S[0]),
                13: lambda: "mix(%s, %s, greaterThanEqual(%s, vec4(0.0)))" % (S[2], S[1], S[0]),
                14: lambda: "mix(%s, %s, greaterThan(%s, vec4(0.0)))" % (S[2], S[1], S[0]),
                15: lambda: "vec4(xe_dot4(%s, %s))" % (S[0], S[1]), 16: lambda: "vec4(xe_dot3(%s, %s))" % (S[0], S[1]),
                17: lambda: "vec4(xe_mul((%s).x, (%s).x) + xe_mul((%s).y, (%s).y) + (%s).x)" % (S[0], S[1], S[0], S[1],
                                                                                               S[2]),
                18: lambda: "xe_cube(%s)" % S[0],
                19: lambda: "vec4(xe_max4(%s))" % S[0],
                28: lambda: "vec4(1.0, xe_mul((%s).y, (%s).y), (%s).z, (%s).w)" % (S[0], S[1], S[0], S[1]),
                29: lambda: "xe_max(%s, %s)" % (S[0], S[1]),
            }.get(vop)
            if vop in PUSH:
                o = PUSH[vop]
                pre.append("vec4 pa_ = %s, pb_ = %s;" % (S[0], S[1]))
                vexpr = "((pa_.x == 0.0 && pb_.x %s 0.0) ? vec4(0.0) : vec4(pa_.x + 1.0))" % o
                post.append("p0 = (pa_.w == 0.0 && pb_.w %s 0.0);" % o)
            elif vop in KILLV:
                pre.append("bool kv_ = any(%s(%s, %s));" % (KILLV[vop], S[0], S[1]))
                vexpr = "(kv_ ? vec4(1.0) : vec4(0.0))"
                if self.stage == "pixel":
                    post.append("if (kv_) discard;")
            elif vop == 29:
                self.uses_a0 = True
                post.append("a0 = int(clamp(floor((%s).w + 0.5), -256.0, 255.0));" % S[0])
                vexpr = vexpr()
            elif vexpr is None:
                raise Unsupported("%s: vector op %s" % (where, vname))
            else:
                vexpr = vexpr()
        # scalar
        sexpr = None
        known = sop in SCALAR_EXPR or sop in SETP or sop in KILLS or sop in (31, 32, 33, 34)
        if sop != 63 and not known:
            raise Unsupported("%s: scalar op %d" % (where, sop))
        if sop != 63:
            A = B = "0.0"
            if sop not in SCALAR_NONE:
                A, B = self.scalar_operands(a, sop, where)
            pre.append("float A_ = %s, B_ = %s;" % (A, B))
            A, B = "A_", "B_"
            if sop in SETP:
                pre.append("bool sp_ = %s;" % _subst(SETP[sop], A, B))
                sexpr = "(sp_ ? 0.0 : 1.0)"
                post.append("p0 = sp_;")
            elif sop in KILLS:
                pre.append("bool ks_ = %s;" % _subst(KILLS[sop], A, B))
                sexpr = "(ks_ ? 1.0 : 0.0)"
                if self.stage == "pixel":
                    post.append("if (ks_) discard;")
            elif sop == 31:
                sexpr = "(%s == 1.0 ? 0.0 : (%s == 0.0 ? 1.0 : %s))" % (A, A, A)
                post.append("p0 = (%s == 1.0);" % A)
            elif sop == 32:
                sexpr = "(%s - 1.0 <= 0.0 ? 0.0 : %s - 1.0)" % (A, A)
                post.append("p0 = (%s - 1.0 <= 0.0);" % A)
            elif sop == 33:
                sexpr = "XE_FLT_MAX"
                post.append("p0 = false;")
            elif sop == 34:
                sexpr = "(%s == 0.0 ? 0.0 : %s)" % (A, A)
                post.append("p0 = (%s == 0.0);" % A)
            else:
                sexpr = _subst(SCALAR_EXPR[sop], A, B)
                if sop in (23, 24):
                    self.uses_a0 = True
                    post.append("a0 = int(clamp(floor(%s%s), -256.0, 255.0));" % (A, " + 0.5" if sop == 23 else ""))
        if vexpr is None and sexpr is None:
            return
        pred = a["predicated"]
        if pred:
            self.emit("if (%sp0) {" % ("" if a["pred_condition"] else "!"))
            self.depth += 1
        self.emit("{")
        self.depth += 1
        for line in pre:
            self.emit(line)
        if vexpr is not None:
            self.emit("vec4 v_ = %s;" % vexpr)
        if sexpr is not None:
            self.emit("float s_ = %s;" % sexpr)
        if vexpr is not None:
            self.write(v_target, v_write, "v_", a["vector_clamp"])
        if sexpr is not None:
            self.write(s_target, s_write, "vec4(s_)", a["scalar_clamp"])
            if sop != 50:
                self.emit("ps = s_;")
        if one_mask:
            self.write(v_target, one_mask, "vec4(1.0)", False)
        if zero_mask:
            self.write(v_target, zero_mask, "vec4(0.0)", False)
        for line in post:
            self.emit(line)
        self.depth -= 1
        self.emit("}")
        if pred:
            self.depth -= 1
            self.emit("}")

    def fetch(self, words, at):
        f = decode_fetch(*words)
        where = "fetch %d" % at
        if f["op"] == 0:
            # a mini fetch takes the fetch constant and stride of the full fetch before it (Xenia's
            # ParseVertexFetchInstruction); its format, offset and signedness are its own
            if not f["mini"]:
                self.last_full = (f["const_index"] * 3 + f["const_index_sel"], f["stride"])
            if f["mini"] and self.last_full:
                fc, stride = self.last_full
            else:
                fc, stride = f["const_index"] * 3 + f["const_index_sel"], f["stride"]
            key = (fc, f["offset"])
            desc = {"fetch_constant": fc, "offset": f["offset"], "format": f["format"], "stride": stride,
                    "signed": f["signed"], "integer": f["integer"], "exp_adjust": f["exp_adjust"]}
            # the index: the untouched r0.x is the vertex index (an attribute); anything else -- another register or
            # component, r0 after the shader wrote it (instancing: index / count) -- reads the buffer as a texture
            # at the computed index (truncated, as Xenia's fetch does when the index is not rounded)
            if f["src_reg"] or f["src_rel"] or f["src_swizzle"] or self.r0_written or self.force_computed:
                names = [k for k, _ in self.vbuffers]
                if key not in names:
                    self.vbuffers.append((key, desc))
                    names.append(key)
                index = "int(floor((%s).%s))" % (self.temp(f["src_reg"], f["src_rel"]), "xyzw"[f["src_swizzle"]])
                src = "xe_vfetch(xe_vb%d, %s)" % (names.index(key), index)
            else:
                names = [k for k, _ in self.attributes]
                if key not in names:
                    self.attributes.append((key, desc))
                    names.append(key)
                src = "xe_v%d" % names.index(key)
        elif f["op"] in (24, 25, 26):
            # setTexLOD / setGradientH / setGradientV: kept for the texture fetches that use them
            sw = f["src_swizzle"]
            c = "%s.%s" % (self.temp(f["src_reg"], f["src_rel"]), "".join("xyzw"[(sw >> (2 * i)) & 3]
                                                                           for i in range(3)))
            target = {24: "xe_lod", 25: "xe_gh.xyz", 26: "xe_gv.xyz"}[f["op"]]
            self.emit("%s = %s;" % (target, ("(%s).x" % c) if f["op"] == 24 else c))
            return
        elif f["op"] == 1:
            dim = f["dimension"]
            k = f["const_index"]
            if self.textures.setdefault(k, dim) != dim:
                raise Unsupported("%s: texture %d fetched with two dimensions" % (where, k))
            sw = f["src_swizzle"]
            sel = "".join("xyzw"[(sw >> (2 * i)) & 3] for i in range(3))
            c = "%s.%s" % (self.temp(f["src_reg"], f["src_rel"]), sel)
            coord = {0: "vec2((%s).x, 0.5)" % c, 1: "(%s).xy" % c, 2: "(%s).xyz" % c,
                     3: "xe_cubedir((%s).xyz)" % c}[dim]
            size = "vec%d(textureSize(xe_t%d, 0))" % (2 if dim < 2 else 3, k)
            if f["unnormalized"] and dim != 3:
                coord = "%s / %s" % (coord, size)
            # offsets: half texels (Xenia's ucode.h), 1D / 2D use x / y, 3D also z
            offs = [f["offset_x"], f["offset_y"], f["offset_z"]][:{0: 1, 1: 2, 2: 3, 3: 0}[dim]]
            if any(offs):
                if dim == 0:
                    coord = "(%s + vec2(%r, 0.0) / %s)" % (coord, offs[0] * 0.5, size)
                else:
                    coord = "(%s + vec%d(%s) / %s)" % (coord, len(offs), ", ".join(repr(o * 0.5) for o in offs), size)
            bias = f["lod_bias"] / 16.0  # 1/16 steps (Xenia's ucode.h)
            gd = {0: ".xy", 1: ".xy", 2: ".xyz", 3: ".xyz"}[dim]
            if f["use_reg_gradients"]:
                if dim == 0:
                    src = "textureGrad(xe_t%d, %s, vec2(xe_gh.x, 0.0), vec2(xe_gv.x, 0.0))" % (k, coord)
                else:
                    src = "textureGrad(xe_t%d, %s, xe_gh%s, xe_gv%s)" % (k, coord, gd, gd)
            elif f["use_reg_lod"]:
                src = "textureLod(xe_t%d, %s, xe_lod + %r)" % (k, coord, bias)
            elif self.stage == "vertex":
                src = "textureLod(xe_t%d, %s, %r)" % (k, coord, bias)
            elif bias:
                src = "texture(xe_t%d, %s, %r)" % (k, coord, bias)
            else:
                src = "texture(xe_t%d, %s)" % (k, coord)
        else:
            raise Unsupported("%s: %s" % (where, f["name"]))
        parts, mask = [], 0
        for i in range(4):
            sel = (f["dst_swizzle"] >> (3 * i)) & 7
            if sel == 7:
                continue
            mask |= 1 << i
            parts.append("f_.%s" % "xyzw"[sel] if sel < 4 else ("0.0" if sel == 4 else "1.0"))
        if not mask:
            return
        pred = f["predicated"]
        if pred:
            self.emit("if (%sp0) {" % ("" if f["pred_condition"] else "!"))
            self.depth += 1
        comps = "".join("xyzw"[i] for i in range(4) if mask >> i & 1)
        value = "vec%d(%s)" % (len(parts), ", ".join(parts)) if len(parts) > 1 else parts[0]
        dst = self.temp(f["dst_reg"], f["dst_rel"])
        if dst == "r[0]" or dst.startswith("r[clamp("):
            self.r0_written = True
        self.emit("{ vec4 f_ = %s; %s.%s = %s; }" % (src, dst, comps, value))
        if pred:
            self.depth -= 1
            self.emit("}")


def _exec_cond(cf):
    op = cf["op"]
    if op in (3, 4, 13, 14):
        return "%sxe_bool(%d)" % ("" if cf["condition"] else "!", cf["bool_address"])
    if op in (5, 6):
        return "%sp0" % ("" if cf["condition"] else "!")
    return None


def _clause(sh, cf, words):
    for i in range(cf["count"]):
        at = cf["address"] + i
        d = words[3 * at:3 * at + 3]
        if len(d) < 3:
            raise Unsupported("clause past the end of the shader")
        if cf["sequence"] >> (2 * i) & 1:
            sh.fetch(d, at)
        else:
            sh.alu(d, at)


def _straight_body(sh, cfs, words):
    """No loops, calls or jumps: the clauses in order, inside do { } while (false) so that a conditional end
    (cexece: the shader ends only when its condition holds) can break out; return is a no-op outside a call."""
    sh.emit("do {")
    sh.depth += 1
    for cf in cfs:
        op = cf["op"]
        if op not in EXEC_OPS:
            if op in (0, 10, 12, 15):
                continue
            raise Unsupported("control flow: %s" % cf["name"])
        cond = _exec_cond(cf)
        end = op in (2, 4, 6, 14)
        if cond:
            sh.emit("if (%s) {" % cond)
            sh.depth += 1
        _clause(sh, cf, words)
        if cond:
            if end:
                sh.emit("break;")
            sh.depth -= 1
            sh.emit("}")
        elif end:
            break
    sh.depth -= 1
    sh.emit("} while (false);")


def _flow_body(sh, cfs, words):
    """Loops, calls and jumps: a state machine on the control-flow index (pc), with the loop counter / aL stacks and
    the return stack four deep, as the sequencer has them, and an iteration bound against a runaway shader."""
    sh.uses_al = True
    sh.uses_loops = True
    sh.emit("int pc = 0, lsp = 0, rsp = 0;")
    sh.emit("int lc[4], la[4], rs[4];")
    sh.emit("for (int g_ = 0; g_ < 65536 && pc >= 0; g_++) {")
    sh.depth += 1
    sh.emit("switch (pc) {")
    n = len(cfs)
    for k, cf in enumerate(cfs):
        op = cf["op"]
        nxt = k + 1 if k + 1 < n else -1
        sh.emit("case %d: {" % k)
        sh.depth += 1
        if op in EXEC_OPS:
            cond = _exec_cond(cf)
            end = op in (2, 4, 6, 14)
            if cond:
                sh.emit("if (%s) {" % cond)
                sh.depth += 1
            _clause(sh, cf, words)
            if end:
                sh.emit("pc = -1;")
                sh.emit("break;")
            if cond:
                sh.depth -= 1
                sh.emit("}")
        elif op == 7:
            sh.emit("uint L_ = xe_loop(%d);" % cf["loop_id"])
            sh.emit("if ((L_ & 255u) == 0u) { pc = %d; break; }" % cf["address"])
            sh.emit("if (lsp < 4) { lc[lsp] = int(L_ & 255u); la[lsp] = aL; lsp++; }")
            if not cf["repeat"]:
                sh.emit("aL = int((L_ >> 8) & 255u);")
        elif op == 8:
            sh.emit("uint L_ = xe_loop(%d);" % cf["loop_id"])
            sh.emit("int t_ = max(lsp - 1, 0);")
            sh.emit("lc[t_] -= 1;")
            brk = " || %sp0" % ("" if cf["condition"] else "!") if cf["predicated_break"] else ""
            sh.emit("if (lc[t_] <= 0%s) { lsp = t_; aL = la[t_]; }" % brk)
            sh.emit("else { aL += int(L_ << 8) >> 24; pc = %d; break; }" % cf["address"])
        elif op in (9, 11):
            if cf["unconditional"]:
                cond = "true"
            elif cf["predicated"]:
                cond = "%sp0" % ("" if cf["condition"] else "!")
            else:
                cond = "%sxe_bool(%d)" % ("" if cf["condition"] else "!", cf["bool_address"])
            if op == 9:
                sh.emit("if (%s) { if (rsp < 4) { rs[rsp] = %d; rsp++; } pc = %d; break; }" % (cond, nxt,
                                                                                               cf["address"]))
            else:
                sh.emit("if (%s) { pc = %d; break; }" % (cond, cf["address"]))
        elif op == 10:
            sh.emit("if (rsp > 0) { rsp--; pc = rs[rsp]; break; }")
        sh.emit("pc = %d;" % nxt)
        sh.emit("break;")
        sh.depth -= 1
        sh.emit("}")
    sh.emit("default: pc = -1; break;")
    sh.emit("}")
    sh.depth -= 1
    sh.emit("}")


MEMEXPORT = """layout(std430, binding = 0) buffer XeMemExport { uint xe_mx_n; uint xe_mx_cap; uint xe_mx_log[]; };
vec4 xe_eA;
vec4 xe_eM[5];
uint xe_eMw;
uint xe_mx_swap(uint v, uint e) {
    if (e == 1u) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if (e == 3u) return (v << 16) | (v >> 16);
    if (e == 2u || e == 4u || e == 5u)
        return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
    return v;
}
uint xe_mx_norm(float v, uint bits, bool sgn, bool integer) {
    float m = sgn ? exp2(float(bits - 1u)) - 1.0 : exp2(float(bits)) - 1.0;
    float f = integer ? clamp(trunc(v), sgn ? -m - 1.0 : 0.0, m)
                      : (sgn ? clamp(v, -1.0, 1.0) * m : clamp(v, 0.0, 1.0) * m);
    int i = integer ? int(f) : int(f + (f < 0.0 ? -0.5 : 0.5));
    return uint(i) & ((1u << bits) - 1u);
}
void xe_mx_flush() {
    uvec4 a = floatBitsToUint(xe_eA);
    uint w = xe_eMw;
    xe_eMw = 0u;
    if ((a.x >> 30) != 1u || (a.y >> 23) != 0x96u || (a.z >> 23) != 0x96u || (a.w >> 23) != 0x96u) return;
    uint base = (a.x & 0x3FFFFFFFu) << 2, index = a.y & 0x7FFFFFu, bound = a.w & 0x7FFFFFu, info = a.z;
    uint fmt = (info >> 8) & 63u, e = info & 7u;
    bool sgn = (info & 0x10000u) != 0u, integer = (info & 0x20000u) != 0u;
    uint n = fmt == 6u || fmt == 7u || fmt == 25u || fmt == 31u || fmt == 36u ? 1u
           : fmt == 26u || fmt == 32u || fmt == 37u ? 2u : fmt == 38u ? 4u : 0u;
    if (n == 0u) return;
    for (uint k = 0u; k < 5u; k++) {
        if ((w >> k & 1u) == 0u || index + k >= bound) continue;
        vec4 v = xe_eM[k];
        if ((info & 0x80000u) != 0u) v = v.zyxw;
        uint d[4];
        d[0] = 0u; d[1] = 0u; d[2] = 0u; d[3] = 0u;
        if (fmt == 6u) d[0] = xe_mx_norm(v.x, 8u, sgn, integer) | xe_mx_norm(v.y, 8u, sgn, integer) << 8
                            | xe_mx_norm(v.z, 8u, sgn, integer) << 16 | xe_mx_norm(v.w, 8u, sgn, integer) << 24;
        else if (fmt == 7u) d[0] = xe_mx_norm(v.x, 10u, sgn, integer) | xe_mx_norm(v.y, 10u, sgn, integer) << 10
                                 | xe_mx_norm(v.z, 10u, sgn, integer) << 20 | xe_mx_norm(v.w, 2u, sgn, integer) << 30;
        else if (fmt == 25u || fmt == 26u) {
            d[0] = xe_mx_norm(v.x, 16u, sgn, integer) | xe_mx_norm(v.y, 16u, sgn, integer) << 16;
            d[1] = xe_mx_norm(v.z, 16u, sgn, integer) | xe_mx_norm(v.w, 16u, sgn, integer) << 16;
        } else if (fmt == 31u || fmt == 32u) {
            d[0] = packHalf2x16(v.xy);
            d[1] = packHalf2x16(v.zw);
        } else {
            uvec4 b = floatBitsToUint(v);
            d[0] = b.x; d[1] = b.y; d[2] = b.z; d[3] = b.w;
        }
        for (uint j = 0u; j < n; j++) d[j] = xe_mx_swap(d[j], e);
        if (e == 4u && n >= 2u) { uint t = d[0]; d[0] = d[1]; d[1] = t; t = d[2]; d[2] = d[3]; d[3] = t; }
        if (e == 5u && n == 4u) { uint t = d[0]; d[0] = d[3]; d[3] = t; t = d[1]; d[1] = d[2]; d[2] = t; }
        uint at = base + (index + k) * 4u * n;
        uint slot = atomicAdd(xe_mx_n, n);
        if (slot + n > xe_mx_cap) continue;
        for (uint j = 0u; j < n; j++) {
            xe_mx_log[2u * (slot + j)] = at + 4u * j;
            xe_mx_log[2u * (slot + j) + 1u] = d[j];
        }
    }
}"""


def translate(words, stage="vertex", interpolators=0, memexport=False):
    """GLSL ES 3.00 source of one shader and what it needs from the host:
    {"attributes": [...], "textures": {index: dimension}, "outputs": [...], "uses_a0": bool, "memexport": bool}.
    memexport (XBA-282): translate eA / eM exports (GLSL ES 3.10, a storage buffer); Unsupported otherwise."""
    if stage not in ("vertex", "pixel"):
        raise ValueError(stage)
    words = list(words) + [0] * (-len(words) % 3)
    sh = _Shader(stage, interpolators, memexport)
    cfs, limit, k = [], len(words) // 3, 0
    while k < limit:
        for w0, w1 in unpack_cf(*words[3 * k:3 * k + 3]):
            cf = decode_cf(w0, w1)
            cfs.append(cf)
            if cf["op"] in EXEC_OPS and cf["count"]:
                limit = min(limit, cf["address"])
        k += 1
    flow = any(cf["op"] in (7, 8, 9, 11) for cf in cfs)
    if flow:
        _flow_body(sh, cfs, words)
        if stage == "vertex" and sh.r0_written and sh.attributes:
            # loops / jumps: program order is not run order, so once r0 is written anywhere every fetch is computed
            sh = _Shader(stage, interpolators, memexport)
            sh.force_computed = True
            _flow_body(sh, cfs, words)
    else:
        _straight_body(sh, cfs, words)
    out = ["#version 310 es" if sh.uses_memexport else "#version 300 es", "// xenos_glsl %d" % GLSL_VERSION, "precision highp float;", "precision highp int;", "precision highp sampler2D;",
           "precision highp sampler3D;", "precision highp samplerCube;",
           # an instance name: block members are global to a program, and both stages have xe_c / xe_b / xe_l
           "layout(std140) uniform %s { vec4 c[256]; uvec4 b[2]; uvec4 l[8]; vec4 at; vec4 pg; vec4 vp; } xe_k;" % (
               "XeVertexConstants" if stage == "vertex" else "XePixelConstants"),
           "#define xe_c xe_k.c", "#define xe_b xe_k.b", "#define xe_l xe_k.l"]
    for i, (_, a) in enumerate(sh.attributes):
        # the renderer reads this comment back (fetch constant, offset and stride in dwords, format, signedness)
        out.append("in vec4 xe_v%d;  // fetch constant %d, offset %d, format %d, stride %d, signed %d, integer %d, "
                   "exp %d" % (i, a["fetch_constant"], a["offset"], a["format"], a["stride"], a["signed"],
                               a["integer"], a["exp_adjust"]))
    for i, (_, a) in enumerate(sh.vbuffers):
        # a vertex buffer read at computed indices: the renderer converts the whole buffer (the fetch constant's size)
        # to vec4s in an RGBA32F texture, 1024 elements per row (the same comment as the attributes')
        out.append("uniform highp sampler2D xe_vb%d;  // fetch constant %d, offset %d, format %d, stride %d, signed %d, "
                   "integer %d, exp %d" % (i, a["fetch_constant"], a["offset"], a["format"], a["stride"], a["signed"],
                                           a["integer"], a["exp_adjust"]))
    for t, dim in sorted(sh.textures.items()):
        out.append("uniform %s xe_t%d;" % ({0: "sampler2D", 1: "sampler2D", 2: "sampler3D", 3: "samplerCube"}[dim],
                                            t))
    if stage == "vertex":
        # all sixteen interpolators, written or not, so that any pixel shader links with any vertex shader
        out += ["out vec4 xe_i%d;" % o for o in range(16)]
    else:
        out += ["in vec4 xe_i%d;" % i for i in range(interpolators)]
        out += ["layout(location = %d) out vec4 xe_color%d;" % (o, o) for o in sorted(sh.outputs)]
    out.append(PRELUDE.rstrip())
    if sh.uses_memexport:
        out.append(MEMEXPORT)
    if sh.vbuffers:
        out.append("vec4 xe_vfetch(highp sampler2D t, int i) { i = max(i, 0); return texelFetch(t, ivec2(i & 1023, "
                   "i >> 10), 0); }")
    out.append("void main() {")
    out.append("    vec4 r[64];")
    out.append("    for (int i = 0; i < 64; i++) r[i] = vec4(0.0);")
    out.append("    float ps = 0.0;")
    out.append("    bool p0 = false;")
    out.append("    int a0 = 0, aL = 0;")
    out.append("    float xe_lod = 0.0;")
    out.append("    vec4 xe_gh = vec4(0.0), xe_gv = vec4(0.0);")
    if sh.uses_memexport:
        out.append("    xe_eA = vec4(0.0); xe_eMw = 0u;")
        out += ["    xe_eM[%d] = vec4(0.0);" % k for k in range(5)]
    if stage == "vertex":
        out.append("    r[0].x = float(gl_VertexID);")
        out.append("    vec4 xe_pos = vec4(0.0), xe_psize = vec4(0.0);")
        out += ["    xe_i%d = vec4(0.0);" % o for o in range(16)]
    else:
        out += ["    r[%d] = xe_i%d;" % (i, i) for i in range(interpolators)]
        out.append("    vec4 xe_depth = vec4(gl_FragCoord.z);")
        out += ["    if (xe_k.pg.x >= 0.0) {  // PsParamGen: the pixel position (y downwards), the face in X's sign",
                "        vec2 p_ = gl_FragCoord.xy * xe_k.pg.zw + vec2(-0.5, xe_k.pg.y - 0.5);",
                "        r[clamp(int(xe_k.pg.x), 0, 15)] = vec4(gl_FrontFacing ? p_.x : -p_.x, p_.y, 0.0, 0.0);",
                "    }"]
        out += ["    xe_color%d = vec4(0.0);" % o for o in sorted(sh.outputs)]
    out += sh.body
    if sh.uses_memexport:
        out.append("    if (xe_eMw != 0u) xe_mx_flush();")
    if stage == "vertex":
        # the guest viewport (screen-space positions included) to the host's full-target clip space, Direct3D's
        # clip-space depth (0..w) to GL's (-w..w)
        out.append("    gl_Position = vec4(xe_pos.xy * xe_k.vp.xy + xe_pos.w * xe_k.vp.zw, xe_pos.z * 2.0 - xe_pos.w, "
                   "xe_pos.w);")
        if sh.exports_psize:
            out.append("    gl_PointSize = xe_psize.x;")
    else:
        if 0 in sh.outputs:  # the alpha test (GLES has none of its own): Direct3D's compare functions
            out += ["    if (xe_k.at.x != 0.0) {",
                    "        float a_ = xe_color0.a, ref_ = xe_k.at.z;",
                    "        int f_ = int(xe_k.at.y);",
                    "        bool pass_ = f_ == 1 ? a_ < ref_ : f_ == 2 ? a_ == ref_ : f_ == 3 ? a_ <= ref_ :",
                    "                     f_ == 4 ? a_ > ref_ : f_ == 5 ? a_ != ref_ : f_ == 6 ? a_ >= ref_ : f_ == 7;",
                    "        if (!pass_) discard;",
                    "    }"]
        if sh.exports_depth:  # only then: writing it turns early depth testing off
            out.append("    gl_FragDepth = xe_depth.x;")
    out.append("}")
    info = {"attributes": [a for _, a in sh.attributes], "vbuffers": [a for _, a in sh.vbuffers],
            "textures": dict(sh.textures),
            "outputs": sorted(sh.outputs), "uses_a0": sh.uses_a0, "memexport": sh.uses_memexport}
    return "\n".join(out) + "\n", info


def translate_cached(words, stage="vertex", interpolators=0):
    """What the shader cache holds (bringup, tools/prepare/x360.py; XBA-282): GLSL ES 3.00 when the shader can be,
    else, for a vertex shader that memory export stops, the GLSL ES 3.10 memory export translation (the renderer runs
    it where GLES 3.1 allows, and skips its draws elsewhere)."""
    try:
        return translate(words, stage, interpolators)
    except Unsupported as ex:
        # vertex shaders only: the renderer links a 3.10 vertex shader with its pixel shader raised to 3.10, not the
        # other way round
        if stage != "vertex" or not re.search(r"export to register 3[2-7]$", str(ex)):
            raise
    return translate(words, stage, interpolators, memexport=True)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    stage = "pixel" if "--pixel" in argv else "vertex"
    n = int(argv[argv.index("--interpolators") + 1]) if "--interpolators" in argv else 0
    path = [a for i, a in enumerate(argv) if not a.startswith("--") and (i == 0 or argv[i - 1] != "--interpolators")][0]
    raw = open(path, "rb").read()
    words = list(struct.unpack(">%dI" % (len(raw) // 4), raw[:len(raw) // 4 * 4]))
    try:
        src, _ = translate(words, stage, n)
    except Unsupported as ex:
        print("unsupported: %s" % ex, file=sys.stderr)
        return 1
    sys.stdout.write(src)
    return 0


if __name__ == "__main__":
    sys.exit(main())
