"""xenos_glsl (XBA-270): Xenos microcode translated to GLSL ES 3.00 and run on a real GLES implementation (Mesa
llvmpipe, surfaceless EGL: tests/xenos_glsl_run.c, built and run by tests/run_glsl_mesa.sh, which sets
$XENOS_GLSL_RUN), against a reference interpreter written here from the instruction descriptions (the same ones
xenos_glsl follows: Xenia's ucode.h). Random co-issued vector + scalar instructions on random constants, swizzles,
negation, absolute values, clamps, write masks, predication, ps, exports; control flow on booleans and p0; vertex
fetches; pixel shaders compiled and linked. Without $XENOS_GLSL_RUN only the translation itself is checked."""

import math
import os
import random
import struct
import subprocess

import pytest

from . import CONSOLE  # noqa: F401
from .xenos_asm import (alu_words, cf_branch, cf_exec, cf_loop_end, cf_loop_start, export, fetch_words,
                        flow_program, loop_const, program)
from .xenos_glsl import Unsupported, translate

RUN = os.environ.get("XENOS_GLSL_RUN", "")
TINY = 1.17549435e-38
FLT_MAX = 3.4028234663852886e38
VSRCS = {8: 1, 9: 1, 10: 1, 11: 3, 12: 3, 13: 3, 14: 3, 17: 3, 19: 1}
VOPS = list(range(30))
SOPS = [s for s in range(51) if s != 41] + [63]
VALUES = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, -3.25, 7.0, 0.001, 100.0, -0.75, 3.0, 0.25, -2.0, 10.0, 1.5]


def f32(x):
    if x != x:
        return x
    try:
        return struct.unpack("<f", struct.pack("<f", x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def lmul(a, b):
    return 0.0 if abs(a) < TINY or abs(b) < TINY else f32(a * b)


def log2(a):
    if a != a or a < 0:
        return math.nan
    if a == 0:
        return -math.inf
    return f32(math.log2(a))


def rcp(a):
    return math.copysign(math.inf, a) if a == 0 else f32(1.0 / a)


def rsq(a):
    if a != a or a < 0:
        return math.nan
    return math.copysign(math.inf, a) if a == 0 else f32(1.0 / math.sqrt(a))   # sqrt(-0) is -0


def clampinf(t, lo, hi):
    return (lo if t < 0 else hi) if math.isinf(t) else t


def trunc(a):
    return float(math.trunc(a)) if math.isfinite(a) else a


# ---- encoding ----

# ---- reference interpreter (on the instruction descriptions, not on the words) ----

def comps(swz):
    return [(j + ((swz >> (2 * j)) & 3)) & 3 for j in range(4)]


class Ref:
    def __init__(self, consts, bools=0, interpolators=(), stage="vertex"):
        self.c = consts
        self.bools = bools
        self.stage = stage
        self.r = [[0.0] * 4 for _ in range(64)]
        for i, v in enumerate(interpolators):
            self.r[i] = list(v)
        self.o = [[0.0] * 4 for _ in range(16)]
        self.ps = 0.0
        self.p0 = False
        self.killed = False

    def operand(self, src, abs_c):
        reg, temp, swz, neg = src
        if temp:
            v, ab = self.r[reg & 63], reg >> 7
        else:
            v, ab = self.c[reg], abs_c
        out = [v[k] for k in comps(swz)]
        if ab:
            out = [abs(x) for x in out]
        return [-x for x in out] if neg else out

    def alu(self, ins):
        g = dict(vop=0, sop=50, vdest=0, sdest=0, vmask=0, smask=0, abs_c=0, export=0, vclamp=0, sclamp=0, pred=0,
                 pcond=0, sdest_rel=0, srcs=((0, 0, 0, 0),) * 3)
        g.update(ins)
        if self.killed or (g["pred"] and self.p0 != bool(g["pcond"])):
            return
        vop, sop = g["vop"], g["sop"]
        kill = False
        S = [self.operand(g["srcs"][k], g["abs_c"]) for k in range(VSRCS.get(vop, 2))]
        a, b = S[0], S[1] if len(S) > 1 else None
        c = S[2] if len(S) > 2 else None
        p0_new = None
        if vop == 0:
            v = [f32(x + y) for x, y in zip(a, b)]
        elif vop == 1:
            v = [lmul(x, y) for x, y in zip(a, b)]
        elif vop in (2, 29):
            v = [x if x >= y else y for x, y in zip(a, b)]
        elif vop == 3:
            v = [x if x < y else y for x, y in zip(a, b)]
        elif vop in (4, 5, 6, 7):
            op = {4: lambda x, y: x == y, 5: lambda x, y: x > y, 6: lambda x, y: x >= y, 7: lambda x, y: x != y}[vop]
            v = [1.0 if op(x, y) else 0.0 for x, y in zip(a, b)]
        elif vop == 8:
            v = [f32(x - math.floor(x)) for x in a]
        elif vop == 9:
            v = [trunc(x) for x in a]
        elif vop == 10:
            v = [float(math.floor(x)) for x in a]
        elif vop == 11:
            v = [f32(lmul(x, y) + z) for x, y, z in zip(a, b, c)]
        elif vop in (12, 13, 14):
            op = {12: lambda x: x == 0, 13: lambda x: x >= 0, 14: lambda x: x > 0}[vop]
            v = [y if op(x) else z for x, y, z in zip(a, b, c)]
        elif vop in (15, 16):
            m = [lmul(x, y) for x, y in zip(a, b)]
            v = [f32(f32(f32(m[0] + m[1]) + m[2]) + (m[3] if vop == 15 else 0.0))] * 4
        elif vop == 17:
            v = [f32(f32(lmul(a[0], b[0]) + lmul(a[1], b[1])) + c[0])] * 4
        elif vop == 18:
            x, y, z = a[2], a[3], a[0]
            if abs(z) >= abs(x) and abs(z) >= abs(y):
                v = [-y, -x if z < 0 else x, f32(2.0 * z), 5.0 if z < 0 else 4.0]
            elif abs(y) >= abs(x):
                v = [-z if y < 0 else z, x, f32(2.0 * y), 3.0 if y < 0 else 2.0]
            else:
                v = [-y, z if x < 0 else -z, f32(2.0 * x), 1.0 if x < 0 else 0.0]
        elif vop == 19:
            x, y, z, w = a
            v = [x if (x > y and x > z and x > w) else y if (y > z and y > w) else z if z > w else w] * 4
        elif vop in (20, 21, 22, 23):
            op = {20: lambda x: x == 0, 21: lambda x: x != 0, 22: lambda x: x > 0, 23: lambda x: x >= 0}[vop]
            p0_new = a[3] == 0 and op(b[3])
            v = [0.0] * 4 if (a[0] == 0 and op(b[0])) else [f32(a[0] + 1.0)] * 4
        elif vop in (24, 25, 26, 27):
            op = {24: lambda x, y: x == y, 25: lambda x, y: x > y, 26: lambda x, y: x >= y,
                  27: lambda x, y: x != y}[vop]
            v = [1.0 if any(op(x, y) for x, y in zip(a, b)) else 0.0] * 4
            kill = v[0] == 1.0
        elif vop == 28:
            v = [1.0, lmul(a[1], b[1]), a[2], b[3]]
        else:
            raise AssertionError(vop)
        s = None
        if sop != 63:
            if sop in (42, 43, 44, 45, 46, 47):
                reg, temp, swz, neg = g["srcs"][2]
                A = self.c[reg][comps(swz)[3]]
                B = self.r[(sop & 1) | (temp << 1) | (swz & 0x3C)][comps(swz)[0]]
                if g["abs_c"]:
                    A, B = abs(A), abs(B)
                if neg:
                    A, B = -A, -B
            elif sop in (33, 50):
                A = B = 0.0
            else:
                o = self.operand(g["srcs"][2], g["abs_c"])
                A, B = o[3], o[0]
            ps = self.ps
            s = {
                0: lambda: f32(A + B), 1: lambda: f32(A + ps), 2: lambda: lmul(A, B), 3: lambda: lmul(A, ps),
                4: lambda: -FLT_MAX if (ps == -FLT_MAX or not math.isfinite(ps) or not math.isfinite(B) or B <= 0)
                else lmul(A, ps),
                5: lambda: A if A >= B else B, 6: lambda: A if A < B else B, 7: lambda: 1.0 if A == 0 else 0.0,
                8: lambda: 1.0 if A > 0 else 0.0, 9: lambda: 1.0 if A >= 0 else 0.0, 10: lambda: 1.0 if A != 0 else 0.0,
                11: lambda: f32(A - math.floor(A)), 12: lambda: trunc(A), 13: lambda: float(math.floor(A)),
                14: lambda: 1.0 if A == 0 else f32(2.0 ** A),
                15: lambda: clampinf(0.0 if A == 1 else log2(A), -FLT_MAX, FLT_MAX),
                16: lambda: 0.0 if A == 1 else log2(A),
                17: lambda: clampinf(1.0 if A == 1 else rcp(A), -FLT_MAX, FLT_MAX),
                18: lambda: clampinf(1.0 if A == 1 else rcp(A), -0.0, 0.0), 19: lambda: 1.0 if A == 1 else rcp(A),
                20: lambda: clampinf(1.0 if A == 1 else rsq(A), -FLT_MAX, FLT_MAX),
                21: lambda: clampinf(1.0 if A == 1 else rsq(A), -0.0, 0.0), 22: lambda: 1.0 if A == 1 else rsq(A),
                23: lambda: A if A >= B else B, 24: lambda: A if A >= B else B, 25: lambda: f32(A - B),
                26: lambda: f32(A - ps), 40: lambda: f32(math.sqrt(A)) if A >= 0 else math.nan,
                42: lambda: lmul(A, B), 43: lambda: lmul(A, B), 44: lambda: f32(A + B), 45: lambda: f32(A + B),
                46: lambda: f32(A - B), 47: lambda: f32(A - B), 48: lambda: f32(math.sin(A)),
                49: lambda: f32(math.cos(A)), 50: lambda: ps,
            }.get(sop, lambda: None)()
            if sop in (27, 28, 29, 30):
                t = {27: A == 0, 28: A != 0, 29: A > 0, 30: A >= 0}[sop]
                s, p0_new = (0.0 if t else 1.0), t
            elif sop in (35, 36, 37, 38, 39):
                s = 1.0 if {35: A == 0, 36: A > 0, 37: A >= 0, 38: A != 0, 39: A == 1}[sop] else 0.0
                kill = kill or s == 1.0
            elif sop == 31:
                s, p0_new = (0.0 if A == 1 else 1.0 if A == 0 else A), A == 1
            elif sop == 32:
                s, p0_new = (0.0 if A - 1.0 <= 0 else f32(A - 1.0)), A - 1.0 <= 0
            elif sop == 33:
                s, p0_new = FLT_MAX, False
            elif sop == 34:
                s, p0_new = (0.0 if A == 0 else A), A == 0
            assert s is not None, sop

        def clamp(x, on):
            return min(max(x, 0.0), 1.0) if on and x == x else x

        if g["export"]:
            tgt = self.o[g["vdest"]]
            vm, sm = g["vmask"], g["smask"]
            for i in range(4):
                if vm & ~sm >> i & 1:
                    tgt[i] = clamp(v[i], g["vclamp"])
                if s is not None and sm & ~vm >> i & 1:
                    tgt[i] = clamp(s, g["sclamp"])
                if vm & sm >> i & 1:
                    tgt[i] = 1.0
        else:
            for i in range(4):
                if g["vmask"] >> i & 1:
                    self.r[g["vdest"]][i] = clamp(v[i], g["vclamp"])
            for i in range(4):
                if s is not None and g["smask"] >> i & 1:
                    self.r[g["sdest"]][i] = clamp(s, g["sclamp"])
        if s is not None and sop != 50:
            self.ps = s
        if p0_new is not None:
            self.p0 = p0_new
        if kill and self.stage == "pixel":
            self.killed = True  # discard: nothing after it is seen


def src(rng, temp_ok=True, temps=(5, 6)):
    if temp_ok and rng.random() < 0.25:
        return (rng.choice(temps) | (0x80 if rng.random() < 0.3 else 0), 1, rng.getrandbits(8), rng.getrandbits(1))
    return (rng.randrange(16), 0, rng.getrandbits(8), rng.getrandbits(1))


def make_case(rng, stage="vertex"):
    vop = rng.choice(VOPS)
    sop = rng.choice(SOPS)
    if vop in (20, 21, 22, 23) and sop in (27, 28, 29, 30, 31, 32, 33, 34):
        sop = 0
    ins = dict(vop=vop, sop=sop, vdest=1, sdest=2, vmask=rng.choice([0xF, 0xF, 0x5, 0xA, 0x7]),
               smask=rng.choice([0xF, 0xF, 0x3, 0x8]), abs_c=int(rng.random() < 0.2), vclamp=int(rng.random() < 0.2),
               sclamp=int(rng.random() < 0.2), srcs=tuple(src(rng, temps=(0, 2, 5, 6) if stage == "pixel" else (5, 6))
                                                          for _ in range(3)))
    pre = [dict(vop=2, vdest=5, vmask=0xF, srcs=((8, 0, rng.getrandbits(8), 0), (8, 0, 0x00, 0), (0, 0, 0, 0))),
           dict(vop=2, vdest=6, vmask=0xF, srcs=((9, 0, 0, 0), (9, 0, 0, 0), (0, 0, 0, 0)))]
    if sop in (42, 43, 44, 45, 46, 47):
        reg, temp, swz, neg = ins["srcs"][2]
        t = (sop & 1) | (temp << 1) | (swz & 0x3C)
        pre.append(dict(vop=2, vdest=t, vmask=0xF, srcs=((10, 0, 0, 0), (10, 0, 0, 0), (0, 0, 0, 0))))
    post = [dict(vop=2, vdest=3, vmask=0xF, pred=1, pcond=1, srcs=((11, 0, 0, 0), (11, 0, 0, 0), (0, 0, 0, 0))),
            dict(vop=0, vmask=0, sop=50, sdest=4, smask=0xF)]
    if stage == "pixel":  # one colour target: the vector result
        return pre + [ins, export(0, 1)]
    exports = [export(0, 1), export(1, 2), export(2, 3), export(3, 4)]
    return pre + [ins] + post + exports


def to_words(seq):
    instrs = [("alu", alu_words(**i)) for i in seq]
    clauses = []
    for k in range(0, len(instrs), 6):
        clauses.append([1, instrs[k:k + 6], {}])
    clauses[-1][0] = 2
    return program([tuple(c) for c in clauses])


def consts_blob(consts, bools=0, attrs=(), loops=()):
    b = b"".join(struct.pack("<4f", *c) for c in consts) + struct.pack("<8I", *[(bools >> (32 * i)) & 0xFFFFFFFF
                                                                               for i in range(8)])
    att = list(attrs) + [[0.0] * 4] * (16 - len(attrs))
    lp = list(loops) + [0] * (32 - len(loops))
    return b + b"".join(struct.pack("<4f", *a) for a in att) + struct.pack("<32I", *lp)


def A(**kw):
    return ("alu", alu_words(**kw))


def MOV(dst, c, **kw):
    """r[dst] = c[c] (max c, c), extra fields through kw"""
    return A(vop=2, vdest=dst, vmask=0xF, srcs=((c, 0, 0, 0), (c, 0, 0, 0), (0, 0, 0, 0)), **kw)


def ADD_RC(dst, r, c, **kw):
    return A(vop=0, vdest=dst, vmask=0xF, srcs=((r, 1, 0, 0), (c, 0, 0, 0), (0, 0, 0, 0)), **kw)


def flow_cases():
    """(name, words, loops, bools, expected o0.., consts) for loops, aL / a0 addressing, calls, jumps"""
    cs = [[float(10 * k + 1), float(10 * k + 2), float(10 * k + 3), float(k)] for k in range(16)] + \
        [[0.0] * 4] * 240
    out = []
    E = [A(**export(0, 1)), A(**export(1, 2))]
    # sum of c[aL] over a loop: count 3, start 2, step 1 -> c2 + c3 + c4 (const_0_rel_abs, aL-relative)
    loop_sum = flow_program([("exec", 1, [MOV(1, 20)], {}), ("cf", cf_loop_start(5, 0)),
                             ("exec", 1, [ADD_RC(1, 1, 0, const_0_rel_abs=1)], {}), ("cf", cf_loop_end(2, 0)),
                             ("exec", 2, E, {})])
    s = [sum(cs[k][i] for k in (2, 3, 4)) for i in range(4)]
    out.append(("loop sum", loop_sum, [loop_const(3, 2, 1)], 0, [s, [0.0] * 4]))
    s = [sum(cs[k][i] for k in (9, 7, 5, 3)) for i in range(4)]
    out.append(("loop step -2", loop_sum, [loop_const(4, 9, -2)], 0, [s, [0.0] * 4]))
    out.append(("loop count 0", loop_sum, [loop_const(0, 2, 1)], 0, [[0.0] * 4, [0.0] * 4]))
    # predicated break: r7 = c[aL] - c5, setp_ge r7.w (c[k].w = k): the loop breaks once aL reaches 5, so the body
    # runs for aL 3, 4, 5
    brk = flow_program([("exec", 1, [MOV(1, 20)], {}), ("cf", cf_loop_start(5, 1)),
                        ("exec", 1, [ADD_RC(1, 1, 0, const_0_rel_abs=1),
                                     A(vop=0, vdest=7, vmask=0xF, const_0_rel_abs=1,
                                       srcs=((0, 0, 0, 0), (5, 0, 0, 1), (0, 0, 0, 0))),
                                     A(sop=30, sdest=7, smask=0, srcs=((0, 0, 0, 0), (0, 0, 0, 0), (7, 1, 0, 0)))],
                         {}),
                        ("cf", cf_loop_end(2, 1, predicated_break=1, condition=1)), ("exec", 2, E, {})])
    s = [sum(cs[k][i] for k in (3, 4, 5)) for i in range(4)]
    out.append(("predicated break", brk, [0, loop_const(10, 3, 1)], 0, [s, [0.0] * 4]))
    # r[aL] destinations: loop start 1, count 2 writes r1 = c1 and r2 = c2 (vector_dest_rel, aL-relative constant)
    reld = flow_program([("cf", cf_loop_start(3, 2)),
                         ("exec", 1, [A(vop=2, vdest=0, vdest_rel=1, vmask=0xF, const_0_rel_abs=1,
                                        const_1_rel_abs=1, srcs=((0, 0, 0, 0), (0, 0, 0, 0), (0, 0, 0, 0)))], {}),
                         ("cf", cf_loop_end(1, 2)), ("exec", 2, E, {})])
    out.append(("relative destination", reld, [0, 0, loop_const(2, 1, 1)], 0, [cs[1], cs[2]]))
    # a0: maxa sets a0 = floor(c6.w + 0.5) = 6, then r1 = c[a0 + 2] = c8 (relative bit: a0)
    a0 = flow_program([("exec", 2, [A(vop=29, vdest=7, vmask=0, srcs=((6, 0, 0, 0), (6, 0, 0, 0), (0, 0, 0, 0))),
                                    A(vop=2, vdest=1, vmask=0xF, const_0_rel_abs=1, const_1_rel_abs=1,
                                      relative=1, srcs=((2, 0, 0, 0), (2, 0, 0, 0), (0, 0, 0, 0)))] + E, {})])
    out.append(("a0 addressing", a0, [], 0, [cs[8], [0.0] * 4]))
    # call / return: call cf 3 (r1 = c5), back to cf 1 (export), cf 2 never runs; a call on a false boolean skips
    for taken in (1, 0):
        call = flow_program([("cf", cf_branch(9, 3, bool_address=4, condition=1)), ("exec", 2, E, {}),
                             ("exec", 1, [MOV(2, 9)], {}), ("exec", 1, [MOV(1, 5)], {}), ("cf", cf_branch(10, 0))])
        out.append(("call taken %d" % taken, call, [], taken << 4, [cs[5] if taken else [0.0] * 4, [0.0] * 4]))
    # jump over an exec on a boolean; an unconditional jump backwards is bounded by a boolean-guarded exece
    for taken in (1, 0):
        jmp = flow_program([("exec", 1, [MOV(1, 3)], {}), ("cf", cf_branch(11, 3, bool_address=200, condition=1)),
                            ("exec", 1, [MOV(1, 4)], {}), ("exec", 2, E, {})])
        out.append(("jump taken %d" % taken, jmp, [], taken << 200, [cs[3] if taken else cs[4], [0.0] * 4]))
    # cexece ends only when its condition holds
    for b in (1, 0):
        ce = program([(4, [MOV(1, 3)] + E, dict(bool_address=9, condition=1)), (2, [MOV(1, 4)] + E, {})])
        out.append(("cexece %d" % b, ce, [], b << 9, [cs[3] if b else cs[4], [0.0] * 4]))
    return cs, out


def close(got, want, loose):
    if want != want:
        # log / rsq / sqrt of a negative number: NaN here, undefined in GLSL ES (llvmpipe gives 0); what Xenos gives
        # is not described either, so no check
        return True
    if math.isinf(want):
        return got == want
    tol = 2e-4 if loose else 1e-5
    return abs(got - want) <= tol * max(1.0, abs(want))


def test_mini_fetch_inherits_constant_and_stride():
    words = program([(2, [("fetch", fetch_words(0, 0, 1, 0o3210, 5, sel=2, format=38, stride=9, offset=0)),
                          ("fetch", fetch_words(0, 0, 2, 0o3210, 0, sel=0, format=6, stride=0, offset=4, mini=1)),
                          ("alu", alu_words(**export(0, 2)))], {})])
    _, info = translate(words, "vertex")
    assert [(a["fetch_constant"], a["offset"], a["stride"], a["format"]) for a in info["attributes"]] == \
        [(17, 0, 9, 38), (17, 4, 9, 6)]


def test_translation_shape():
    rng = random.Random(1)
    src_, info = translate(to_words(make_case(rng)), "vertex")
    assert src_.startswith("#version 300 es") and "xe_i3 = vec4(0.0);" in src_ and info["outputs"] == [0, 1, 2, 3]
    with pytest.raises(Unsupported):
        translate(program([(2, [("fetch", fetch_words(2, 0, 1, 0o3210, 0))], {})]), "pixel")  # no such fetch opcode
    with pytest.raises(Unsupported):
        translate(to_words([dict(vop=2, vdest=32, vmask=0xF, export=1)]), "vertex")  # memory export


@pytest.mark.skipif(not os.path.isfile(RUN), reason="$XENOS_GLSL_RUN not set (tests/run_glsl_mesa.sh sets it)")
def test_against_mesa(tmp_path):
    rng = random.Random(2701)
    cases = []
    n = 0
    for _ in range(400):
        seq = make_case(rng)
        consts = [[rng.choice(VALUES) for _ in range(4)] for _ in range(16)] + [[0.0] * 4] * 240
        ref = Ref(consts)
        for ins in seq:
            ref.alu(ins)
        src_, _ = translate(to_words(seq), "vertex")
        (tmp_path / ("%d.vert" % n)).write_text(src_)
        (tmp_path / ("%d.in" % n)).write_bytes(consts_blob(consts))
        (tmp_path / ("%d.nout" % n)).write_text("4")
        cases.append((n, seq[len(seq) - 7], ref.o[:4]))
        n += 1
    # control flow: cexec on a boolean (taken / not taken), cexec_pred, a vertex fetch with a destination swizzle
    for bools, cond in ((1 << 37, 1), (0, 1), (1 << 37, 0)):
        consts = [[float(k + 1)] * 4 for k in range(16)] + [[0.0] * 4] * 240
        body = [("alu", alu_words(**dict(vop=2, vdest=1, vmask=0xF, srcs=((3, 0, 0, 0), (3, 0, 0, 0),
                                                                           (0, 0, 0, 0)))))]
        words = program([(3, body, dict(bool_address=37, condition=cond)),
                         (1, [("alu", alu_words(sop=29, sdest=7, smask=0, srcs=((0, 0, 0, 0),) * 2 + ((2, 0, 0, 0),)))],
                          {}),
                         (5, [("alu", alu_words(**dict(vop=2, vdest=2, vmask=0xF, srcs=((4, 0, 0, 0), (4, 0, 0, 0),
                                                                                         (0, 0, 0, 0)))))],
                          dict(condition=1)),
                         (2, [("alu", alu_words(**export(0, 1))), ("alu", alu_words(**export(1, 2)))], {})])
        src_, _ = translate(words, "vertex")
        (tmp_path / ("%d.vert" % n)).write_text(src_)
        (tmp_path / ("%d.in" % n)).write_bytes(consts_blob(consts, bools))
        (tmp_path / ("%d.nout" % n)).write_text("2")
        taken = bool(bools >> 37 & 1) == bool(cond)
        cases.append((n, "cexec", [[4.0] * 4 if taken else [0.0] * 4, [5.0] * 4]))
        n += 1
    # loops, relative addressing, calls, jumps, conditional ends
    cs, flows = flow_cases()
    for name, words, loops, bools, want in flows:
        src_, _ = translate(words, "vertex")
        (tmp_path / ("%d.vert" % n)).write_text(src_)
        (tmp_path / ("%d.in" % n)).write_bytes(consts_blob(cs, bools, (), loops))
        (tmp_path / ("%d.nout" % n)).write_text("2")
        cases.append((n, name, want))
        n += 1
    # vertex fetch: dst swizzle w, 1, 0, x (sel 3, 5, 4, 0) from attribute (1, 2, 3, 4)
    words = program([(1, [("fetch", fetch_words(0, 0, 1, 3 | 5 << 3 | 4 << 6 | 0 << 9, 2, sel=1))], {}),
                     (2, [("alu", alu_words(**export(0, 1)))], {})])
    src_, info = translate(words, "vertex")
    assert info["attributes"][0]["fetch_constant"] == 7
    (tmp_path / ("%d.vert" % n)).write_text(src_)
    (tmp_path / ("%d.in" % n)).write_bytes(consts_blob([[0.0] * 4] * 256, 0, [[1.0, 2.0, 3.0, 4.0]]))
    (tmp_path / ("%d.nout" % n)).write_text("1")
    cases.append((n, "vfetch", [[4.0, 1.0, 0.0, 1.0]]))
    n += 1
    # pixel shaders: compiled and linked (interpolators, kill, texture fetches 1D / 2D / 3D, colour and depth exports)
    frags = []
    for dim in (0, 1, 2, 3):
        frags.append(program([(1, [("fetch", fetch_words(1, 0, 1, 0 | 1 << 3 | 2 << 6 | 3 << 9, dim, dimension=dim)),
                                   ("alu", alu_words(vop=24, sop=36, vdest=2, sdest=3, vmask=0xF, smask=1,
                                                     srcs=((1, 1, 0, 0), (0, 0, 0, 0), (1, 1, 0, 0))))], {}),
                              (2, [("alu", alu_words(**export(0, 1))),
                                   ("alu", alu_words(vop=2, vdest=61, vmask=1, export=1,
                                                     srcs=((2, 1, 0, 0), (2, 1, 0, 0), (0, 0, 0, 0))))], {})]))
    # offsets and a LOD bias, register LOD, register gradients (3D, with a z offset)
    frags.append(program([(2, [("fetch", fetch_words(1, 0, 1, 0o3210, 2, dimension=1, ox=-3, oy=5, lod_bias=-20)),
                               A(**export(0, 1))], {})]))
    frags.append(program([(2, [("fetch", fetch_words(24, 0, 0, 0o7777, 0)),
                               ("fetch", fetch_words(1, 0, 1, 0o3210, 2, dimension=1, use_reg_lod=1)),
                               A(**export(0, 1))], {})]))
    frags.append(program([(2, [("fetch", fetch_words(25, 0, 0, 0o7777, 0)), ("fetch", fetch_words(26, 1, 0, 0o7777, 0)),
                               ("fetch", fetch_words(1, 0, 1, 0o3210, 2, dimension=2, use_reg_gradients=1, oz=2)),
                               A(**export(0, 1))], {})]))
    for words in frags:
        src_, info = translate(words, "pixel", interpolators=2)
        (tmp_path / ("%d.frag" % n)).write_text(src_)
        (tmp_path / ("%d.in" % n)).write_bytes(consts_blob([[0.0] * 4] * 256))
        cases.append((n, "frag", None))
        n += 1
    # pixel shaders run: random instructions on constants and interpolators (r0..r7), colour export, kills
    for _ in range(150):
        seq = make_case(rng, "pixel")
        consts = [[rng.choice(VALUES) for _ in range(4)] for _ in range(16)] + [[0.0] * 4] * 240
        interp = [[rng.choice(VALUES) for _ in range(4)] for _ in range(8)]
        ref = Ref(consts, interpolators=interp, stage="pixel")
        for ins in seq:
            ref.alu(ins)
        src_, _ = translate(to_words(seq), "pixel", interpolators=8)
        (tmp_path / ("%d.frag" % n)).write_text(src_)
        (tmp_path / ("%d.in" % n)).write_bytes(consts_blob(consts, 0, interp))
        cases.append((n, dict(seq[-2], pixel=1), "killed" if ref.killed else [ref.o[0]]))
        n += 1
    r = subprocess.run([RUN, str(tmp_path)], capture_output=True, text=True, timeout=600)
    if r.returncode == 77:
        pytest.skip(r.stderr.strip())
    assert r.returncode == 0, r.stderr
    lines = {}
    cur = None
    for line in r.stdout.splitlines():
        parts = line.split()
        if parts and parts[0].isdigit() and len(parts) >= 2 and parts[1] in ("vert", "frag", "error"):
            cur = int(parts[0])
            lines[cur] = parts[1:]
        elif cur is not None:
            lines[cur] = lines[cur] + [line]
    bad = []
    seen = {"vert": 0, "frag": 0, "killed": 0, "linked": 0}
    for n, what, want in cases:
        got = lines.get(n)
        if not got or got[0] == "error":
            bad.append((n, "no result", got and " ".join(got[1:])[:400]))
            continue
        if what == "frag":
            continue
        if got[0] == "frag" and got[1:2] == ["linked"]:
            seen["linked"] += 1
            continue  # no float render target: compiled and linked only
        if want == "killed" or got[1:2] == ["killed"]:
            seen["killed"] += got[1:2] == ["killed"]
            if (want == "killed") != (got[1:2] == ["killed"]):
                bad.append((n, "vop %d sop %d" % (what["vop"], what["sop"]), "killed %s, want %s" % (
                    got[1:2] == ["killed"], want == "killed")))
            continue
        seen[got[0]] += 1
        vals = [struct.unpack("<f", struct.pack("<I", int(h, 16)))[0] for h in got[1:]]
        flat = [x for o in want for x in o]
        loose = isinstance(what, dict) and (what.get("sop") in (14, 15, 16, 17, 18, 19, 20, 21, 22, 40, 48, 49))
        for i, (g, w) in enumerate(zip(vals, flat)):
            if not close(g, w, loose):
                bad.append((n, what if not isinstance(what, dict) else "vop %d sop %d" % (what["vop"], what["sop"]),
                            "o%d.%s got %r want %r" % (i // 4, "xyzw"[i % 4], g, w)))
                break
    assert not bad, "%d of %d cases differ: %s" % (len(bad), len(cases), bad[:8])
    print("xenos_glsl on GLES:", seen)
    # the pixel shaders did run (unless the implementation has no float target), some of them killed
    assert seen["vert"] > 400 and (seen["linked"] or (seen["frag"] > 50 and seen["killed"] > 5)), seen


# ---- memory export (XBA-282) ----

def _bits_f(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def _ea(base, index, bound, fmt, endian=0, signed=0, swap_rb=0):
    """eA as a title sets it (Xenia's reading): X the base in dwords (top bits 01), Y / W the index and its bound, Z the
    format bits, Y / Z / W as floats in [2^23, 2^24)."""
    info = endian | fmt << 8 | signed << 16 | swap_rb << 19
    return [_bits_f(0x40000000 | base >> 2), _bits_f(0x4B000000 | index), _bits_f(0x4B000000 | info),
            _bits_f(0x4B000000 | bound)]


def _memexport_program(n_data):
    """eA = c0, eM0.. = c1.. (mov through max(c, c)), then a position export."""
    body = [("alu", alu_words(vop=2, vdest=32, vmask=0xF, export=1, srcs=((0, 0, 0, 0), (0, 0, 0, 0), (0, 0, 0, 0))))]
    for k in range(n_data):
        body.append(("alu", alu_words(vop=2, vdest=33 + k, vmask=0xF, export=1,
                                      srcs=((1 + k, 0, 0, 0), (1 + k, 0, 0, 0), (0, 0, 0, 0)))))
    body.append(("alu", alu_words(**export(62, 0))))
    return program([(2, body, {})])


def _bswap(v):
    return struct.unpack("<I", struct.pack(">I", v))[0]


def _fbits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


MEMEXPORT_CASES = [
    # 32_32_32_32_FLOAT, 8in32: two elements at index 3, 4 (bound 5)
    ("f32x4", _ea(0x10000, 3, 5, 38, endian=2), [[1.0, 2.0, 3.0, 4.0], [-1.0, 0.5, 0.0, 8.0]],
     [(0x10000 + 3 * 16 + 4 * j, _bswap(_fbits(v))) for j, v in enumerate([1.0, 2.0, 3.0, 4.0])] +
     [(0x10000 + 4 * 16 + 4 * j, _bswap(_fbits(v))) for j, v in enumerate([-1.0, 0.5, 0.0, 8.0])]),
    # 8_8_8_8 unorm, no swap, three data registers but a bound of index + 2: two writes
    ("rgba8", _ea(0x20000, 7, 9, 6), [[1.0, 0.0, 0.5, 1.0], [0.0, 1.0, 0.0, 0.25], [1.0, 1.0, 1.0, 1.0]],
     [(0x20000 + 7 * 4, 0xFF | 0 << 8 | 128 << 16 | 0xFF << 24), (0x20000 + 8 * 4, 0 | 0xFF << 8 | 0 << 16 | 64 << 24)]),
    # 32_FLOAT, 16in32
    ("f32", _ea(0x30000, 0, 1, 36, endian=3), [[2.0, 9.0, 9.0, 9.0]],
     [(0x30000, ((_fbits(2.0) << 16) | (_fbits(2.0) >> 16)) & 0xFFFFFFFF)]),
]


def test_memexport_translation():
    words = _memexport_program(2)
    with pytest.raises(Unsupported):
        translate(words, "vertex")                 # without memexport: as before
    src_, info = translate(words, "vertex", memexport=True)
    assert info["memexport"] and src_.startswith("#version 310 es")
    assert "buffer XeMemExport" in src_ and "xe_eMw |= 2u;" in src_ and "if (xe_eMw != 0u) xe_mx_flush();" in src_
    plain, info = translate(to_words([dict(vop=2, vdest=1, vmask=0xF)]), "vertex", memexport=True)
    assert plain.startswith("#version 300 es") and not info["memexport"]   # no export: GLSL ES 3.00 as before


@pytest.mark.skipif(not os.path.isfile(RUN), reason="$XENOS_GLSL_RUN not set (tests/run_glsl_mesa.sh sets it)")
def test_memexport_against_mesa(tmp_path):
    for n, (name, ea, data, want) in enumerate(MEMEXPORT_CASES):
        src_, _ = translate(_memexport_program(len(data)), "vertex", memexport=True)
        (tmp_path / ("%d.vert" % n)).write_text(src_)
        consts = [ea] + data + [[0.0] * 4] * (255 - len(data))
        (tmp_path / ("%d.in" % n)).write_bytes(consts_blob(consts))
        (tmp_path / ("%d.nout" % n)).write_text("1")
    r = subprocess.run([RUN, str(tmp_path)], capture_output=True, text=True, timeout=600)
    got = {}
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] == "mem":
            vals = [int(x, 16) for x in p[3:]]
            got[int(p[0])] = sorted(zip(vals[0::2], vals[1::2]))
    assert "error" not in r.stdout, r.stdout + r.stderr
    for n, (name, _, _, want) in enumerate(MEMEXPORT_CASES):
        assert got.get(n) == sorted(want), (name, got.get(n), sorted(want))


# ---- texture queries (XBA-296) ----

def _query_ref(op, dim, size, modes, base, dx, dy, x, y, ins, scale):
    """the reference for one pixel: the instruction's description, written apart from the translator"""
    c = [base[i] + dx[i] * x + dy[i] * y for i in range(4)]
    if op == 18:
        return [dx[0] * scale, dy[0] * scale, dx[1] * scale, dy[1] * scale]
    n = {0: 1, 1: 2, 2: 3, 3: 2}[dim]
    norm = not ins["unnormalized"] or dim == 3
    k = [size[i] if norm else 1.0 for i in range(3)]
    u = [c[i] * k[i] if i < n else 0.0 for i in range(3)]
    gx = [dx[i] * k[i] * scale if i < n else 0.0 for i in range(3)]
    gy = [dy[i] * k[i] * scale if i < n else 0.0 for i in range(3)]
    r = max(sum(v * v for v in gx), sum(v * v for v in gy))
    lod = 0.5 * math.log2(r) if r > 0 else -FLT_MAX
    if op == 17:
        return [lod, 0.0, 0.0, 0.0]
    offs = [ins["ox"] * 0.5, ins["oy"] * 0.5, ins["oz"] * 0.5] if dim != 3 else [0.0] * 3
    offs = [offs[i] if i < n else 0.0 for i in range(3)]
    if op == 16:
        if dim == 3:
            return [0.0] * 4
        inside = 1.0
        for i in range(n):
            m = (modes >> (2 * i)) & 3
            if not m:
                continue
            v = abs(u[i] + offs[i]) if m == 2 else u[i] + offs[i]
            t = v - 0.5
            j = math.floor(t)
            f = t - j
            inside *= (1.0 - f if 0 <= j < size[i] else 0.0) + (f if 0 <= j + 1 < size[i] else 0.0)
        return [1.0 - inside, 0.0, 0.0, 0.0]
    w = [(u[i] + offs[i] - 0.5 + 1.5 / 1024) % 1.0 if i < n else 0.0 for i in range(3)]
    level = max(lod + ins["lod_bias"] / 16.0, 0.0)
    return w + [level - math.floor(level)]


def _query_close(got, want):
    """fract wraps at 1: 0.9999 and 0.0001 are neighbours"""
    return abs(got - want) <= 5e-4 or abs(abs(got - want) - 1.0) <= 5e-4


def test_query_translation():
    for op in (16, 17, 18, 19):
        words = program([(2, [("fetch", fetch_words(op, 0, 1, 0o3210, 3, dimension=1)), A(**export(0, 1))], {})])
        for stage in ("pixel", "vertex"):
            src_, info = translate(words, stage, interpolators=1)
            assert ("dFdx" in src_.split("void main")[1]) == (stage == "pixel" and op in (17, 18, 19))
            assert info["tqueries"] == ([] if op == 18 else [3])


@pytest.mark.skipif(not os.path.isfile(RUN), reason="$XENOS_GLSL_RUN not set (tests/run_glsl_mesa.sh sets it)")
def test_queries_against_mesa(tmp_path):
    rng = random.Random(2960)
    cases = []
    for n in range(240):
        op, dim = rng.choice((16, 17, 18, 19)), rng.randrange(4)
        ins = dict(unnormalized=rng.getrandbits(1), ox=rng.randrange(-8, 8), oy=rng.randrange(-8, 8),
                   oz=rng.randrange(-8, 8), lod_bias=rng.randrange(-32, 32))
        scale = rng.choice((1.0, 2.0, 4.0))
        size = [float(rng.choice((4, 8, 16, 64))) for _ in range(3)]
        modes = rng.randrange(64)
        base = [rng.uniform(-2.0, 2.0) for _ in range(4)]
        dx = [rng.choice((0.0, 0.125, -0.25, 0.0625, 1.5)) for _ in range(4)]
        dy = [rng.choice((0.0, 0.25, -0.125, 0.03125, 2.0)) for _ in range(4)]
        words = program([(2, [("fetch", fetch_words(op, 0, 1, 0o3210, 5, dimension=dim, **ins)),
                              A(**export(0, 1))], {})])
        src_, _ = translate(words, "pixel", interpolators=1)
        (tmp_path / ("%d.frag" % n)).write_text(src_)
        (tmp_path / ("%d.in" % n)).write_bytes(consts_blob([[0.0] * 4] * 256, 0, [base, dx, dy]))
        tq = [[0.0] * 4 for _ in range(32)]
        tq[5] = size + [float(modes)]
        pg = [-1.0, 0.0, 1.0 / scale, 1.0 / scale]
        (tmp_path / ("%d.grid" % n)).write_bytes(b"".join(struct.pack("<4f", *v) for v in tq + [pg]))
        cases.append((n, op, dim, size, modes, base, dx, dy, ins, scale))
    r = subprocess.run([RUN, str(tmp_path)], capture_output=True, text=True, timeout=600)
    if r.returncode == 77:
        pytest.skip(r.stderr.strip())
    assert r.returncode == 0, r.stderr
    got = {int(p[0]): p[2:] for p in (ln.split() for ln in r.stdout.splitlines()) if len(p) > 2 and p[1] == "grid"}
    bad = []
    for n, op, dim, size, modes, base, dx, dy, ins, scale in cases:
        assert n in got, r.stdout[:800]
        vals = [struct.unpack("<f", struct.pack("<I", int(h, 16)))[0] for h in got[n]]
        for y in range(4):
            for x in range(4):
                want = _query_ref(op, dim, size, modes, base, dx, dy, x, y, ins, scale)
                have = vals[4 * (4 * y + x):4 * (4 * y + x) + 4]
                if not all(_query_close(h, w) for h, w in zip(have, want)):
                    bad.append((n, op, dim, (x, y), have, want))
    assert not bad, "%d pixels differ: %s" % (len(bad), bad[:6])
