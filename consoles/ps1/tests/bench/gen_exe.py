#!/usr/bin/env python3
"""Generates the C of a large synthetic PS1 program for compile-time and run-time benchmarks of the recompiler
(XBA-161).

    gen_exe.py OUTDIR [--functions N] [--seed S]

Writes OUTDIR/main.c, OUTDIR/bench.h and OUTDIR/fNN.c: N functions (default 1000, about 200k MIPS instructions once
built by PSn00bSDK's GCC at -O2) of game-like code assembled from random chunks: loops over RAM arrays, entity structs,
switch state machines (jump tables), fixed-point maths and divisions, byte / halfword buffers, constant tables, the
scratchpad, GTE transforms (inline_c.h), I/O register reads and writes, calls to earlier functions. Sizes vary from a
few dozen to a few thousand instructions, as in a commercial game. main() runs every function in a loop forever
under VSync and a fast root counter IRQ (scheduler events land anywhere in lifted code, as with a game's sound
timer): `ps1_run --frames N` measures lifted code. Deterministic for a seed. Our own code; build it with
build_exe.sh (PSn00bSDK, image xba-ps1-homebrew).
"""

import argparse
import os
import random

GROUP = 32  # functions called by one group function


class Gen:
    def __init__(self, rnd, nfuncs):
        self.r = rnd
        self.n = nfuncs

    def k(self, lo=1, hi=255):
        return self.r.randint(lo, hi)

    def chunk(self, i):
        """One block of statements on the locals a, b, s, t (u32) and i (the function index)."""
        r, k = self.r, self.k
        c = r.randrange(16)
        if c == 0:
            return ("for (int j = 0; j < (int)(a & 15) + %d; j++) { s += ram_u32[(j * %d + b) & 1023] * %du;"
                    " s ^= s >> %d; }" % (k(1, 8), k(1, 9), k(3, 99), k(1, 7)))
        if c == 1:
            return ("{ struct ent *e = &ents[(a + %d) & 63]; for (int j = 0; j < %d; j++, e++) {"
                    " if (e - ents >= 64) e = ents; e->x += e->vx; e->y += e->vy;"
                    " if (e->x > %d || e->x < -%d) { e->vx = -e->vx; e->flags |= %du; }"
                    " if (e->flags & %du) s += (u32)e->hp; else e->hp -= %d; } }"
                    % (k(0, 63), k(2, 9), k(100, 4000), k(100, 4000), 1 << k(0, 7), 1 << k(0, 7), k(1, 5)))
        if c == 2:
            cases = []
            for v in range(r.randint(5, 12)):
                cases.append("case %d: s = s * %du + %du; t ^= s << %d; break;" % (v, k(3, 200), k(0, 9999), k(1, 9)))
            return "switch ((a ^ t) %% %d) { %s default: s -= t; break; }" % (len(cases) + 1, " ".join(cases))
        if c == 3:
            return ("{ s32 d = (s32)(b & 0x%X) + %d; s32 q = ((s32)(a * %d) >> 4) / d; s += (u32)(q + ((s32)t %% d)); }"
                    % (k(0x1F, 0x3FF), k(1, 50), k(2, 300)))
        if c == 4:
            return ("for (int j = 0; j < %d; j++) { u8 v = ram_u8[(t + j) & 4095]; ram_u8[(s + j * %d) & 4095] = "
                    "(u8)(v + %d); s += (u32)(s8)v; }" % (k(4, 24), k(1, 7), k(1, 200)))
        if c == 5:
            return ("for (int j = 0; j < %d; j++) { u16 h = sp_u16[(a + j) & 255]; sp_u16[(b + j) & 255] = (u16)(h * %d"
                    " + j); t += (u32)(s16)h; }" % (k(4, 16), k(3, 17)))
        if c == 6:
            return ("s += const_tab[(a + %d) & 255] + const_tab16[(t >> %d) & 255]; t = t * 1103515245u + %du;"
                    % (k(0, 255), k(0, 8), k(1, 99999)))
        if c == 7:
            return ("{ s += GPU_GP1 & 0x%Xu; TIMER_RELOAD(0) = (u16)(t + %d); for (int j = 0; j < %d; j++)"
                    " ot_buf[(s + j) & 255] = (t + j) & 0xFFFFFFu; }" % (1 << k(20, 30), k(0, 99), k(2, 12)))
        if c == 8 and i > 0:
            callees = [r.randrange(max(0, i - 400), i) for _ in range(r.randint(1, 3))]
            return " ".join("s += f%d(a + %d, t ^ s);" % (j, k(0, 99)) for j in callees)
        if c == 9:
            return ("if (a & %du) { s += %du; if (t > %du) t -= s; else t += %d; } else if (b & %du) { s ^= t << %d; }"
                    " else { s = (s >> 1) | (s << 31); }" % (1 << k(0, 15), k(), k(100, 90000), k(), 1 << k(0, 15),
                                                           k(1, 12)))
        if c == 10:
            return ("{ u32 m = (s & 0xFFFF) * (t >> 16); u64 w = (u64)a * (b | %du); s += m + (u32)(w >> 32); }"
                    % k(1, 1 << 20))
        if c == 11:
            return ("for (int j = 0; j < 8; j++) { if (bits & (1u << j)) s += %d * j; else t ^= %du; }"
                    " bits = (bits << 1) | (s & 1);" % (k(), k(1, 1 << 30)))
        if c == 12:
            return ("{ u32 n = 0, x = s ^ t; while (x && n < 32) { x &= x - 1; n++; } s += n * %d; }" % k())
        if c == 13:
            return ("{ SVECTOR *v = &verts[(a + %d) & 63]; gte_ldv0(v); gte_rtps(); gte_stsxy(&sxy[(s + %d) & 63]);"
                    " gte_stsz(&sz); s += (u32)sz + (u32)sxy[(t + %d) & 63]; }" % (k(0, 63), k(0, 63), k(0, 63)))
        if c == 14:
            return ("{ struct ent *e = &ents[(s >> %d) & 63]; s32 dx = e->x - ents[(t + %d) & 63].x, dy = e->y - "
                    "ents[(a + %d) & 63].y; if (dx * dx + dy * dy < %d) { e->hp -= %d; s += 1; } }"
                    % (k(0, 9), k(0, 63), k(0, 63), k(1000, 90000), k(1, 9)))
        rot = k(1, 15)
        return "s = (s << %d) | (s >> %d); t += s ^ %du; a = a * %du + 1;" % (rot, 32 - rot, k(1, 1 << 30), k(3, 99))

    def function(self, i):
        r = self.r
        roll = r.random()
        n = r.randint(1, 4) if roll < 0.55 else r.randint(5, 14) if roll < 0.93 else r.randint(20, 60)
        body = "\n    ".join(self.chunk(i) for _ in range(n))
        return ("__attribute__((noinline, noclone)) u32 f%d(u32 a, u32 b)\n{\n    u32 s = a ^ %du, t = b + %du;\n"
                "    %s\n    return s ^ t;\n}\n" % (i, self.k(1, 1 << 30), self.k(1, 1 << 30), body))


HEADER = """/* Generated by consoles/ps1/tests/bench/gen_exe.py -- a synthetic benchmark program, our own code. */
#include "bench.h"
"""

BENCH_H = """#include <stdint.h>
#include <psxgte.h>
#include <inline_c.h>
#include <hwregs_c.h>
typedef uint8_t u8; typedef int8_t s8; typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64;
struct ent { s32 x, y, vx, vy; s16 hp; u16 flags; };
extern u32 ram_u32[1024];
extern u8 ram_u8[4096];
extern struct ent ents[64];
extern u32 bits;
extern u32 ot_buf[256];
extern SVECTOR verts[64];
extern u32 sxy[64];
extern u32 sz;
extern const u32 const_tab[256];
extern const u16 const_tab16[256];
#define sp_u16 ((volatile u16 *)0x1F800000) /* the scratchpad */
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("outdir")
    ap.add_argument("--functions", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    g = Gen(rnd, a.functions)
    os.makedirs(a.outdir, exist_ok=True)
    n = a.functions
    protos = "".join("u32 f%d(u32 a, u32 b);\n" % i for i in range(n))
    with open(os.path.join(a.outdir, "bench.h"), "w") as f:
        f.write(BENCH_H + protos)
    per_file = 100
    for base in range(0, n, per_file):
        with open(os.path.join(a.outdir, "f%03d.c" % (base // per_file)), "w") as f:
            f.write(HEADER)
            for i in range(base, min(n, base + per_file)):
                f.write("\n" + g.function(i))
    groups = (n + GROUP - 1) // GROUP
    m = [HEADER, "#include <psxapi.h>\n#include <psxetc.h>\n#include <psxgpu.h>\n",
         '#include "../common/xba_result.h"\n#include "../common/screen.h"\n',
         "u32 ram_u32[1024];\nu8 ram_u8[4096];\nstruct ent ents[64];\nu32 bits;\nu32 ot_buf[256];\n"
         "SVECTOR verts[64];\nu32 sxy[64];\nu32 sz;\n"]
    m.append("const u32 const_tab[256] = {%s};\n" % ", ".join("%du" % rnd.randrange(1 << 32) for _ in range(256)))
    m.append("const u16 const_tab16[256] = {%s};\n" % ", ".join("%d" % rnd.randrange(1 << 16) for _ in range(256)))
    for gi in range(groups):
        calls = "".join("    acc += f%d(acc, acc >> 3);\n" % i for i in range(gi * GROUP, min(n, (gi + 1) * GROUP)))
        m.append("__attribute__((noinline)) static u32 g%d(u32 acc)\n{\n%s    return acc;\n}\n" % (gi, calls))
    m.append("""
static volatile u32 vblanks, ticks;
static void on_vblank(void) { XBA_RESULT->frames = ++vblanks; }
static void on_timer(void) { ticks++; }

int main(void)
{
    xba_begin(XBA_FOURCC('B', 'N', 'C', 'H'));
    screen_init(0, 0, 64);
    InitGeom();
    for (int j = 0; j < 64; j++) {
        verts[j].vx = (s16)(j * 37 - 1000);
        verts[j].vy = (s16)(j * 11 - 300);
        verts[j].vz = (s16)(j * 5 + 200);
    }
    TIMER_CTRL(2) = 0;
    EnterCriticalSection();
    InterruptCallback(IRQ_TIMER2, &on_timer);
    VSyncCallback(&on_vblank);
    ExitCriticalSection();
    /* system clock, IRQ every 2000 cycles at the target: events land anywhere in the lifted code */
    TIMER_RELOAD(2) = 2000;
    TIMER_CTRL(2) = 0x0040 | 0x0010 | 0x0008;
    u32 acc = 1;
    for (;;) {
""")
    m.append("".join("        acc = g%d(acc);\n" % gi for gi in range(groups)))
    m.append("""        XBA_RESULT->checksum = acc;
        XBA_RESULT->count++;
        XBA_RESULT->stage = ticks;
    }
}
""")
    with open(os.path.join(a.outdir, "main.c"), "w") as f:
        f.write("".join(m))


if __name__ == "__main__":
    main()
