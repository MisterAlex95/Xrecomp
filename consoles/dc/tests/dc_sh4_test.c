/* dc_sh4_test -- SH-4 semantics on synthetic programs (XBA-227), no toolchain and no homebrew needed.
 *
 *   dc_sh4_test                 runs every program in the interpreter and checks the words it stores
 *   dc_sh4_test --write DIR     also writes each program as DIR/<name>.bin (a raw binary for 0x8C010000) and
 *                               DIR/<name>.expect (the words, hex), for run_sh4_rc.sh: the same programs lifted
 *
 * Each program is assembled here (a few helpers and a literal pool), stores its results from 0x8C001000 on (r14),
 * and returns to the BIOS (rts with the PR it started with). Expected values are worked out by hand or by C on the
 * host. Covers what is easy to get wrong: DIV0U / DIV0S / DIV1 sequences, carries and borrows (ADDC / SUBC / NEGC),
 * SHAD / SHLD both ways, 64-bit and MAC multiplies (S saturation), CMP/STR, delay slots (target taken before the
 * slot, T read before the slot, PR set for the slot), TRAPA and RTE with the register banks, the FPU in single and
 * double precision, 64-bit FMOV pairs into XD, FRCHG, FIPR, FTRV, FSCA, FTRC saturation, FCNVSD / FCNVDS. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dc_core.h"

#define RES 0x8C001000u
#define MAXW 512

typedef struct {
    uint16_t w[MAXW];
    int n;
    struct {
        int at, reg;
        uint32_t val;
    } lit[64];
    int nlit;
    uint32_t expect[32];
    int nexpect;
} prog;

static void e(prog *p, uint16_t op)
{
    p->w[p->n++] = op;
}
/* mov.l @(pool),Rn: patched when the pool is laid out after the code. */
static void li(prog *p, int n, uint32_t v)
{
    p->lit[p->nlit].at = p->n;
    p->lit[p->nlit].reg = n;
    p->lit[p->nlit].val = v;
    p->nlit++;
    e(p, 0xD000 | (uint16_t)(n << 8));
}
static void st(prog *p, int reg, uint32_t expect) /* mov.l Rreg,@(k*4,r14) and the value it must hold */
{
    int k = p->nexpect;
    if (k < 16) {
        e(p, (uint16_t)(0x1E00 | (reg << 4) | k));
    } else { /* r13 = r14 + k*4 */
        li(p, 13, RES + (uint32_t)k * 4);
        e(p, (uint16_t)(0x2D02 | (reg << 4)));
    }
    p->expect[p->nexpect++] = expect;
}
static void finish(prog *p)
{
    e(p, 0x4F26); /* lds.l @r15+,pr */
    e(p, 0x000B); /* rts */
    e(p, 0x0009);
    if (p->n & 1)
        e(p, 0x0009);
    int pool = p->n; /* words; 4-byte aligned since n is even */
    for (int i = 0; i < p->nlit; i++) {
        int at = p->lit[i].at;
        uint32_t lit_addr = 0x8C010000u + (uint32_t)(pool + 2 * i) * 2;
        uint32_t pc = 0x8C010000u + (uint32_t)at * 2;
        uint32_t disp = (lit_addr - ((pc & ~3u) + 4)) / 4;
        if (disp > 255) {
            fprintf(stderr, "literal out of reach\n");
            exit(2);
        }
        p->w[at] |= (uint16_t)disp;
        p->w[pool + 2 * i] = (uint16_t)p->lit[i].val;
        p->w[pool + 2 * i + 1] = (uint16_t)(p->lit[i].val >> 16);
    }
    p->n = pool + 2 * p->nlit;
}
static void start(prog *p)
{
    memset(p, 0, sizeof *p);
    e(p, 0x4F22); /* sts.l pr,@-r15 */
    li(p, 14, RES);
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* ---- the programs ---- */

static void p_div(prog *p)
{
    static const uint32_t cases[][2] = {{1000003, 97}, {0xFFFFFFFFu, 3}, {123456789, 1}, {7, 10}};
    start(p);
    for (int k = 0; k < 4; k++) {
        li(p, 2, cases[k][0]); /* dividend */
        li(p, 0, cases[k][1]); /* divisor */
        e(p, 0xE100);          /* mov #0,r1 */
        e(p, 0x0019);          /* div0u */
        for (int i = 0; i < 32; i++) {
            e(p, 0x4224); /* rotcl r2 */
            e(p, 0x3104); /* div1 r0,r1 */
        }
        e(p, 0x4224); /* rotcl r2 */
        st(p, 2, cases[k][0] / cases[k][1]);
    }
    /* signed: -1000 / 7 by the manual's sequence (64-bit sign extension, one's complement in, two's out) */
    li(p, 2, (uint32_t)-1000);
    li(p, 0, 7);
    e(p, 0x6323); /* mov r2,r3 */
    e(p, 0x4324); /* rotcl r3: T = sign */
    e(p, 0x311A); /* subc r1,r1: r1 = -T */
    e(p, 0x233A); /* xor r3,r3 */
    e(p, 0x323A); /* subc r3,r2 */
    e(p, 0x2107); /* div0s r0,r1 */
    for (int i = 0; i < 32; i++) {
        e(p, 0x4224); /* rotcl r2 */
        e(p, 0x3104); /* div1 r0,r1 */
    }
    e(p, 0x4224); /* rotcl r2 */
    e(p, 0x323E); /* addc r3,r2 */
    st(p, 2, (uint32_t)(-1000 / 7));
    finish(p);
}

static void p_alu(prog *p)
{
    start(p);
    /* 64-bit add: 0x00000001FFFFFFFF + 0x0000000000000001 */
    li(p, 1, 0xFFFFFFFFu);
    e(p, 0xE001); /* mov #1,r0 */
    e(p, 0xE201); /* mov #1,r2 (high) */
    e(p, 0xE300); /* mov #0,r3 */
    e(p, 0x0008); /* clrt */
    e(p, 0x310E); /* addc r0,r1 */
    e(p, 0x323E); /* addc r3,r2 */
    st(p, 1, 0);
    st(p, 2, 2);
    /* 64-bit negate of 5 (negc) */
    e(p, 0xE105); /* mov #5,r1 */
    e(p, 0xE200); /* mov #0,r2 */
    e(p, 0x0008); /* clrt */
    e(p, 0x611A); /* negc r1,r1 */
    e(p, 0x622A); /* negc r2,r2 */
    st(p, 1, (uint32_t)-5);
    st(p, 2, 0xFFFFFFFFu);
    /* subc borrow: 0 - 1 */
    e(p, 0xE100);
    e(p, 0xE001);
    e(p, 0x0008);
    e(p, 0x310A); /* subc r0,r1 */
    e(p, 0x0029); /* movt r0 */
    st(p, 1, 0xFFFFFFFFu);
    st(p, 0, 1);
    /* shad / shld */
    li(p, 1, 0x80000010u);
    e(p, 0xE0FC); /* mov #-4,r0 */
    e(p, 0x6213); /* mov r1,r2 */
    e(p, 0x420C); /* shad r0,r2 */
    st(p, 2, 0xF8000001u);
    e(p, 0x6213);
    e(p, 0x420D); /* shld r0,r2 */
    st(p, 2, 0x08000001u);
    e(p, 0xE003); /* mov #3,r0 */
    e(p, 0x6213);
    e(p, 0x420C);
    st(p, 2, 0x00000080u);
    li(p, 0, 0xFFFFFFE0u); /* -32: shad gives the sign, shld 0 */
    e(p, 0x6213);
    e(p, 0x420C);
    st(p, 2, 0xFFFFFFFFu);
    e(p, 0x6213);
    e(p, 0x420D);
    st(p, 2, 0);
    /* dmuls / dmulu */
    li(p, 1, (uint32_t)-77777);
    li(p, 2, 1000003);
    e(p, 0x312D); /* dmuls.l r2,r1 */
    e(p, 0x000A); /* sts mach,r0 */
    e(p, 0x031A); /* sts macl,r3 */
    st(p, 0, (uint32_t)((uint64_t)(-77777ll * 1000003) >> 32));
    st(p, 3, (uint32_t)(-77777ll * 1000003));
    e(p, 0x3125); /* dmulu.l r2,r1 */
    e(p, 0x000A);
    st(p, 0, (uint32_t)(((uint64_t)(uint32_t)-77777 * 1000003) >> 32));
    /* cmp/str, swap, xtrct */
    li(p, 1, 0x12345678u);
    li(p, 2, 0xAB34CDEFu);
    e(p, 0x212C); /* cmp/str r2,r1 */
    e(p, 0x0029);
    st(p, 0, 1);
    e(p, 0x6018); /* swap.b r1,r0 */
    st(p, 0, 0x12347856u);
    e(p, 0x6019); /* swap.w r1,r0 */
    st(p, 0, 0x56781234u);
    e(p, 0x6013); /* mov r1,r0 */
    e(p, 0x202D); /* xtrct r2,r0 */
    st(p, 0, 0xCDEF1234u);
    /* mac.l with S=1 saturates at 48 bits */
    li(p, 4, RES + 0x100);
    li(p, 1, 0x7FFFFFFFu);
    e(p, 0x2412); /* mov.l r1,@r4 */
    e(p, 0x6543); /* mov r4,r5 */
    e(p, 0x0028); /* clrmac */
    e(p, 0x0058); /* sets */
    for (int k = 0; k < 18; k++) {
        e(p, 0x6643); /* mov r4,r6 */
        e(p, 0x6743); /* mov r4,r7 */
        e(p, 0x076F); /* mac.l @r6+,@r7+: (2^31 - 1)^2 each, past 2^47 at once */
    }
    e(p, 0x0048); /* clrs */
    e(p, 0x000A);
    e(p, 0x011A);
    st(p, 0, 0x00007FFFu);
    st(p, 1, 0xFFFFFFFFu);
    finish(p);
}

static void p_slots(prog *p)
{
    start(p);
    /* bt/s: T read before the slot (slot clears T); taken */
    e(p, 0x0018); /* sett */
    e(p, 0xE000); /* mov #0,r0 */
    e(p, 0x8D01); /* bt/s +2 (skip one) */
    e(p, 0x0008); /* clrt (slot) */
    e(p, 0xE063); /* mov #99,r0 (skipped) */
    e(p, 0x7001); /* add #1,r0 */
    st(p, 0, 1);
    /* bf/s not taken: slot still runs */
    e(p, 0x0018);
    e(p, 0xE000);
    e(p, 0x8F01); /* bf/s */
    e(p, 0x7005); /* add #5,r0 (slot) */
    e(p, 0x7010); /* add #16,r0 */
    st(p, 0, 21);
    /* jmp @r1 with the slot changing r1: target is r1 before the slot */
    li(p, 1, 0); /* patched below: the address of "after" */
    e(p, 0x412B); /* jmp @r1 */
    e(p, 0xE100); /* mov #0,r1 (slot) */
    e(p, 0xE263); /* mov #99,r2 (skipped) */
    int after = p->n;
    p->lit[p->nlit - 1].val = 0x8C010000u + (uint32_t)after * 2;
    st(p, 1, 0);
    /* bsr with sts pr in the slot: PR is the return address already */
    e(p, 0xB002); /* bsr to the subroutine */
    e(p, 0x032A); /* sts pr,r3 (slot) */
    e(p, 0xA002); /* bra over the subroutine */
    e(p, 0x0009);
    e(p, 0x000B); /* subroutine: rts */
    e(p, 0x7201); /* add #1,r2 (slot of rts) */
    uint32_t ret = 0x8C010000u + (uint32_t)(p->n - 6 + 2) * 2;
    st(p, 3, ret);
    finish(p);
}

static void p_trap(prog *p)
{
    /* VBR at 0x8C011000: handler at +0x100 reads TRA into r0 (bank 1 is live), puts it in r8, returns with rte.
     * The program runs in bank 0 (SR.RB cleared first); after the trap r0 (bank 0) is untouched. */
    start(p);
    li(p, 0, 0x8C011000u);
    e(p, 0x402E); /* ldc r0,vbr */
    li(p, 0, 0x400000F0u); /* MD=1, RB=0, BL=0, IMASK=15 */
    e(p, 0x400E); /* ldc r0,sr */
    e(p, 0xE07B); /* mov #123,r0 */
    e(p, 0xE800); /* mov #0,r8 */
    e(p, 0xC32A); /* trapa #42 */
    e(p, 0x0402); /* stc sr,r4 */
    st(p, 0, 123);
    st(p, 8, 42 * 4);
    st(p, 4, 0x400000F0u);
    /* stc r0_bank reads the other bank's r0: the handler left 0x5A there */
    e(p, 0x0582); /* stc r0_bank,r5 */
    st(p, 5, 0x5A);
    li(p, 0, 0x700000F0u); /* back to bank 1 for the return */
    e(p, 0x400E);
    finish(p);
}

/* The trap handler, placed at 0x8C011100 by the test driver. */
static const uint16_t trap_handler[] = {
    0xD103, /* mov.l @(12,pc),r1 -> TRA address (0xFF000020) */
    0x6012, /* mov.l @r1,r0 */
    0x6803, /* mov r0,r8: r8-r15 are not banked */
    0xE05A, /* mov #0x5A,r0 (bank 1 r0) */
    0x002B, /* rte */
    0x0009, /* nop */
    0x0009, 0x0009,
    0x0020, 0xFF00, /* .long 0xFF000020 */
};

static void p_fpu(prog *p)
{
    start(p);
    li(p, 8, RES + 0x200); /* scratch */
    /* single: (2.5 * 4) / 0.5 = 20, fsqrt(16) = 4 */
    li(p, 0, fbits(2.5f));
    e(p, 0x405A); /* lds r0,fpul */
    e(p, 0xF00D); /* fsts fpul,fr0 */
    li(p, 0, fbits(4.0f));
    e(p, 0x405A);
    e(p, 0xF10D); /* fsts fpul,fr1 */
    li(p, 0, fbits(0.5f));
    e(p, 0x405A);
    e(p, 0xF20D); /* fr2 */
    e(p, 0xF012); /* fmul fr1,fr0 */
    e(p, 0xF023); /* fdiv fr2,fr0 */
    e(p, 0xF01D); /* flds fr0,fpul */
    e(p, 0x005A); /* sts fpul,r0 */
    st(p, 0, fbits(20.0f));
    li(p, 0, fbits(16.0f));
    e(p, 0x405A);
    e(p, 0xF30D);
    e(p, 0xF36D); /* fsqrt fr3 */
    e(p, 0xF31D);
    e(p, 0x005A);
    st(p, 0, fbits(4.0f));
    /* ftrc saturation and float */
    li(p, 0, fbits(3e10f));
    e(p, 0x405A);
    e(p, 0xF40D);
    e(p, 0xF43D); /* ftrc fr4,fpul */
    e(p, 0x005A);
    st(p, 0, 0x7FFFFFFFu);
    e(p, 0xE0F9); /* mov #-7,r0 */
    e(p, 0x405A);
    e(p, 0xF52D); /* float fpul,fr5 */
    e(p, 0xF51D);
    e(p, 0x005A);
    st(p, 0, fbits(-7.0f));
    /* double: PR=1. 1/3 via fdiv on dr0 / dr2, then fcnvds; and sqrt(2) */
    li(p, 0, 0x00080000u); /* PR */
    e(p, 0x406A); /* lds r0,fpscr */
    e(p, 0xE001);
    e(p, 0x405A);
    e(p, 0xF02D); /* float fpul,dr0 = 1 */
    e(p, 0xE003);
    e(p, 0x405A);
    e(p, 0xF22D); /* float fpul,dr2 = 3 */
    e(p, 0xF023); /* fdiv dr2,dr0 */
    e(p, 0xF0BD); /* fcnvds dr0,fpul */
    e(p, 0x005A);
    st(p, 0, fbits((float)(1.0 / 3.0)));
    e(p, 0xE002);
    e(p, 0x405A);
    e(p, 0xF42D); /* float fpul,dr4 = 2 */
    e(p, 0xF46D); /* fsqrt dr4 */
    /* store dr4 as two words (SZ=0 fmov.s fr4 / fr5) */
    e(p, 0xE000);
    e(p, 0x406A); /* PR=0 */
    e(p, 0xF84A); /* fmov.s fr4,@r8 */
    e(p, 0x6183); /* mov r8,r1 */
    e(p, 0x7104); /* add #4,r1 */
    e(p, 0xF15A); /* fmov.s fr5,@r1 */
    e(p, 0x6282); /* mov.l @r8,r2 */
    e(p, 0x6312); /* mov.l @r1,r3 */
    {
        double s2 = sqrt(2.0);
        uint64_t b;
        memcpy(&b, &s2, 8);
        st(p, 2, (uint32_t)(b >> 32));
        st(p, 3, (uint32_t)b);
    }
    /* SZ=1 pair moves: write fr0..fr3 = 1,2,3,4 to memory, load as xd0/xd2, frchg, read back fr0..fr3 */
    for (int i = 0; i < 4; i++) {
        e(p, (uint16_t)(0xE001 + i)); /* mov #1+i,r0 */
        e(p, 0x405A);
        e(p, (uint16_t)(0xF02D | (i << 8))); /* float fpul,fr_i */
    }
    li(p, 0, 0x00100000u); /* SZ */
    e(p, 0x406A);
    e(p, 0x6183);          /* mov r8,r1 */
    e(p, 0xF10A);          /* fmov dr0,@r1 */
    e(p, 0x7108);          /* add #8,r1 */
    e(p, 0xF12A);          /* fmov dr2,@r1 */
    e(p, 0x6183);
    e(p, 0xF119);          /* fmov @r1+,xd0 */
    e(p, 0xF319);          /* fmov @r1+,xd2 */
    e(p, 0xF08D);          /* fldi0 fr0 */
    e(p, 0xFBFD);          /* frchg: the XD bank becomes FR */
    e(p, 0xF3FD);          /* fschg: SZ=0 */
    e(p, 0xF21D);          /* flds fr2,fpul */
    e(p, 0x005A);
    st(p, 0, fbits(3.0f));
    e(p, 0xF01D);
    e(p, 0x005A);
    st(p, 0, fbits(1.0f));
    e(p, 0xFBFD);          /* frchg back */
    e(p, 0xF01D);
    e(p, 0x005A);
    st(p, 0, 0);           /* fldi0 fr0 above */
    /* fipr: (1,2,3,4).(1,2,3,4) = 30 in fr3 of fv0 (fr1, fr2, fr3 still 2, 3, 4) */
    e(p, 0xF09D);          /* fldi1 fr0 */
    e(p, 0xF0ED);          /* fipr fv0,fv0 */
    e(p, 0xF31D);
    e(p, 0x005A);
    st(p, 0, fbits(30.0f));
    /* fsca 0x2000 (45 degrees): sin = cos */
    li(p, 0, 0x2000);
    e(p, 0x405A);
    e(p, 0xF4FD);          /* fsca fpul,dr4 */
    e(p, 0xF41D);
    e(p, 0x005A);
    st(p, 0, fbits((float)sin(6.283185307179586 / 8)));
    e(p, 0xF51D);
    e(p, 0x005A);
    st(p, 0, fbits((float)cos(6.283185307179586 / 8)));
    finish(p);
}

typedef void (*build_fn)(prog *);
static const struct {
    const char *name;
    build_fn build;
} progs[] = {{"div", p_div}, {"alu", p_alu}, {"slots", p_slots}, {"trap", p_trap}, {"fpu", p_fpu}};

static int run_one(const char *name, prog *p, const char *dir)
{
    uint8_t img[0x2000];
    memset(img, 0, sizeof img);
    for (int i = 0; i < p->n; i++) {
        img[2 * i] = (uint8_t)p->w[i];
        img[2 * i + 1] = (uint8_t)(p->w[i] >> 8);
    }
    for (size_t i = 0; i < sizeof trap_handler / 2; i++) {
        img[0x1100 + 2 * i] = (uint8_t)trap_handler[i];
        img[0x1100 + 2 * i + 1] = (uint8_t)(trap_handler[i] >> 8);
    }
    size_t size = 0x1100 + sizeof trap_handler;
    if (dir) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s.bin", dir, name);
        FILE *f = fopen(path, "wb");
        if (!f || fwrite(img, 1, size, f) != size)
            return 2;
        fclose(f);
        snprintf(path, sizeof path, "%s/%s.expect", dir, name);
        f = fopen(path, "w");
        if (!f)
            return 2;
        for (int i = 0; i < p->nexpect; i++)
            fprintf(f, "%s%08X", i % 8 ? " " : i ? "\n" : "peek ", p->expect[i]);
        fprintf(f, "\n");
        fclose(f);
    }
    dc_machine *m = dc_create();
    char why[128];
    if (!m || dc_load(m, img, size, why, sizeof why))
        return 2;
    dc_run_frames(m, 2);
    int code, bad = 0;
    if (!dc_exited(m, &code)) {
        printf("%s: did not return\n", name);
        bad = 1;
    }
    for (int i = 0; i < p->nexpect; i++) {
        uint32_t got = dc_peek32(m, RES + 4 * (uint32_t)i);
        if (got != p->expect[i]) {
            printf("%s: word %d = %08X, want %08X\n", name, i, got, p->expect[i]);
            bad = 1;
        }
    }
    printf("%s: %s (%d words)\n", name, bad ? "FAIL" : "ok", p->nexpect);
    dc_destroy(m);
    return bad;
}

int main(int argc, char **argv)
{
    const char *dir = argc == 3 && !strcmp(argv[1], "--write") ? argv[2] : NULL;
    int bad = 0;
    for (size_t i = 0; i < sizeof progs / sizeof progs[0]; i++) {
        static prog p;
        progs[i].build(&p);
        bad |= run_one(progs[i].name, &p, dir);
    }
    return bad ? 1 : 0;
}
