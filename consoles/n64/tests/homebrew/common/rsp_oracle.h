/* rsp_oracle.h -- RSP differential test programs (N-5, XBA-173), shared by the host test (consoles/n64/tests/test_rsp.c, against
 * n64_rsp.c) and the homebrew ROM ../rsp (run on the ares oracle, tools/consoles/run_homebrew.sh n64). Our own
 * code, plain C99 so both compilers take it.
 *
 * Each group builds small microcodes with rsp_asm.h, fills DMEM with pseudo-random inputs (a fixed xorshift seed per
 * group, edge values mixed in), runs them to their BREAK and hashes what they left in DMEM (FNV-1a) into one 32-bit
 * checksum per group. The ROM writes the checksums into its result block; the host test runs the same groups on our
 * RSP and compares them with the values recorded from ares (rsp_oracle_expected.h). Groups:
 *   0..63   every vector computational funct (reserved ones included) x 16 element selectors x 16 input sets: the
 *           result, the three accumulator slices (VSAR), VCO / VCC / VCE, DIVOUT
 *   64      VRCP / VRSQ / VRCPH+VRCPL / VRSQH+VRSQL sweep over inputs of every magnitude (2048 per op)
 *   65..76  vector loads (LWC2 functions 0..11) x 16 elements x 12 addresses (some at the 4 KB wrap)
 *   77..88  vector stores (SWC2 functions 0..11), the whole DMEM hashed
 *   89      SP DMA from the CPU side: lengths, counts, skips, unaligned addresses, bank wraparound, register readback
 *   90      SP_STATUS set / clear pairs on signals and intr-break, broke clear, the semaphore
 *   91      scalar unit: ALU ops, branches and delay slots, links, unaligned and wrapping loads / stores
 *   92      the divide ROMs entry by entry: VRCP of 0x4000 + 32 i (the result is the RCP entry i) and VRSQL of
 *           2^29 + i 2^21 / 2^28 + i 2^20 (the result is RSQ entry 256 + i / i)
 * The I/O goes through rspo_io so the same code drives the real register interface on the console and n64_rsp.c. */
#ifndef RSP_ORACLE_H
#define RSP_ORACLE_H

#include <stdint.h>

#include "rsp_asm.h"

#define RSPO_GROUPS 93
#define RSPO_DRAM_SIZE 0x3000u /* the DMA group's RDRAM buffer */

typedef struct rspo_io {
    void *ctx;
    void (*wmem)(void *ctx, uint32_t addr, uint32_t word); /* SP memory, 0x0000..0x1FFC (DMEM then IMEM) */
    uint32_t (*rmem)(void *ctx, uint32_t addr);
    void (*wreg)(void *ctx, uint32_t reg, uint32_t v); /* SP registers as n64_rsp_write_reg numbers them */
    uint32_t (*rreg)(void *ctx, uint32_t reg);
    void (*wdram)(void *ctx, uint32_t off, uint32_t word); /* the DMA buffer */
    uint32_t (*rdram)(void *ctx, uint32_t off);
    void (*run)(void *ctx); /* SP_PC = 0, clear halt and broke, wait for the halt */
    uint32_t dram_base;     /* physical address of the DMA buffer (8-aligned) */
} rspo_io;

typedef struct rspo_rng {
    uint32_t x;
} rspo_rng;

static uint32_t rspo_next(rspo_rng *r)
{
    uint32_t x = r->x;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->x = x;
    return x;
}

/* 16-bit inputs with the edge values over-represented. */
static uint32_t rspo_r16(rspo_rng *r)
{
    static const uint16_t edge[8] = {0x0000, 0x0001, 0xFFFF, 0x7FFF, 0x8000, 0x7FFE, 0x8001, 0x0020};
    uint32_t v = rspo_next(r);
    switch (v & 7) {
    case 0: return edge[(v >> 3) & 7];
    case 1: return (((v >> 3) & 0xFF) - 128) & 0xFFFF; /* small, both signs */
    case 2: return (v >> 8) & 0x003F;
    default: return v >> 16;
    }
}

static uint32_t rspo_word16(rspo_rng *r)
{
    return rspo_r16(r) << 16 | rspo_r16(r);
}

typedef struct rspo_hash {
    uint32_t h;
} rspo_hash;

#ifdef RSPO_TRACE
void rspo_trace(uint32_t w); /* debugging: every hashed word, so two runs can be diffed word by word */
#endif

static void rspo_hword(rspo_hash *h, uint32_t w)
{
#ifdef RSPO_TRACE
    rspo_trace(w);
#endif
    for (int i = 3; i >= 0; i--) {
        h->h ^= (w >> (8 * i)) & 0xFF;
        h->h *= 16777619u;
    }
}

static void rspo_load_prog(const rspo_io *io, const uint32_t *p, int n)
{
    for (int i = 0; i < n; i++)
        io->wmem(io->ctx, 0x1000u + 4u * (uint32_t)i, p[i]);
}

static void rspo_hash_mem(const rspo_io *io, rspo_hash *h, uint32_t from, uint32_t to)
{
    for (uint32_t a = from; a < to; a += 4)
        rspo_hword(h, io->rmem(io->ctx, a));
}

static void rspo_fill_dmem(const rspo_io *io, rspo_rng *r, uint32_t from, uint32_t to)
{
    for (uint32_t a = from; a < to; a += 4)
        io->wmem(io->ctx, a, rspo_word16(r));
}

/* ---- 0..63: vector computational ops ---- */
static uint32_t rspo_group_vop(const rspo_io *io, unsigned fn)
{
    rspo_rng rng = {0x1234567u ^ (fn * 0x9E3779B9u)};
    rspo_hash h = {2166136261u};
    uint32_t p[64];
    for (unsigned e = 0; e < 16; e++) {
        for (unsigned batch = 0; batch < 16; batch++) {
            int n = 0;
            unsigned vs = (e & 1) ? 3 : 1;
            p[n++] = R_LQV(8, 0, 5, 0);                 /* v8 <- 0x50 */
            p[n++] = R_LQV(0, 0, 6, 0);                 /* v0 <- 0x60 */
            p[n++] = R_VOP(RV_VRCP, 8, 0, 0, 8);        /* DIVDP = 0, v8[0] = rcp(v0[0]) */
            p[n++] = R_LQV(1, 0, 0, 0);
            p[n++] = R_LQV(2, 0, 1, 0);
            p[n++] = R_LQV(3, 0, 2, 0);
            p[n++] = R_LQV(4, 0, 3, 0);
            p[n++] = R_LQV(6, 0, 5, 0);
            p[n++] = R_VOP(RV_VMUDH, 5, 1, 3, 0);       /* accumulator: v1 * v3 << 16 + u(v2) * v4 */
            p[n++] = R_VOP(RV_VMADN, 5, 2, 4, 0);
            p[n++] = R_LW(1, 0x40, 0);
            p[n++] = R_CTC2(1, 0);
            p[n++] = R_LW(2, 0x44, 0);
            p[n++] = R_CTC2(2, 1);
            p[n++] = R_LW(3, 0x48, 0);
            p[n++] = R_CTC2(3, 2);
            if (batch & 1)
                p[n++] = R_VOP(RV_VRCPH, 9, 0, 3, 8 + (batch >> 1)); /* DIVDP = 1, DIVIN = v3[batch / 2] */
            p[n++] = R_VOP(fn, 6, vs, 2, e);
            p[n++] = R_SQV(6, 0, 16, 0);
            p[n++] = R_VOP(RV_VSAR, 7, 0, 0, 8);
            p[n++] = R_SQV(7, 0, 17, 0);
            p[n++] = R_VOP(RV_VSAR, 7, 0, 0, 9);
            p[n++] = R_SQV(7, 0, 18, 0);
            p[n++] = R_VOP(RV_VSAR, 7, 0, 0, 10);
            p[n++] = R_SQV(7, 0, 19, 0);
            p[n++] = R_CFC2(1, 0);
            p[n++] = R_SW(1, 0x140, 0);
            p[n++] = R_CFC2(2, 1);
            p[n++] = R_SW(2, 0x144, 0);
            p[n++] = R_CFC2(3, 2);
            p[n++] = R_SW(3, 0x148, 0);
            p[n++] = R_VOP(RV_VRCPH, 8, 1, 0, 8);       /* v8[1] = DIVOUT */
            p[n++] = R_SQV(8, 0, 21, 0);
            p[n++] = R_BREAK;
            rspo_load_prog(io, p, n);
            rspo_fill_dmem(io, &rng, 0x000, 0x070);
            io->wmem(io->ctx, 0x40, rspo_next(&rng) & 0xFFFF);
            io->wmem(io->ctx, 0x44, rspo_next(&rng) & 0xFFFF);
            io->wmem(io->ctx, 0x48, rspo_next(&rng) & 0xFF);
            io->run(io->ctx);
            rspo_hash_mem(io, &h, 0x100, 0x160);
        }
    }
    return h.h;
}

/* ---- 64: divide sweep ---- */
static uint32_t rspo_group_div(const rspo_io *io)
{
    rspo_rng rng = {0xD1D1D1u};
    rspo_hash h = {2166136261u};
    uint32_t p[80];
    int n = 0;
    p[n++] = R_LQV(1, 0, 0, 0);
    p[n++] = R_LQV(2, 0, 1, 0);
    for (unsigned i = 0; i < 8; i++)
        p[n++] = R_VOP(RV_VRCP, 3, i, 1, 8 + i);
    for (unsigned i = 0; i < 8; i++)
        p[n++] = R_VOP(RV_VRSQ, 4, i, 2, 8 + i);
    for (unsigned i = 0; i < 8; i++) {
        p[n++] = R_VOP(RV_VRCPH, 5, i, 1, 8 + i);
        p[n++] = R_VOP(RV_VRCPL, 6, i, 2, 8 + i);
    }
    for (unsigned i = 0; i < 8; i++) {
        p[n++] = R_VOP(RV_VRSQH, 7, i, 2, 8 + i);
        p[n++] = R_VOP(RV_VRSQL, 9, i, 1, 8 + i);
    }
    for (unsigned i = 0; i < 6; i++)
        p[n++] = R_SQV(i == 5 ? 9 : 3 + i, 0, 16 + i, 0);
    p[n++] = R_BREAK;
    rspo_load_prog(io, p, n);
    for (unsigned batch = 0; batch < 256; batch++) {
        for (uint32_t a = 0; a < 0x20; a += 4) {
            uint32_t w = 0;
            for (int k = 0; k < 2; k++) {
                uint32_t x = rspo_next(&rng), v = (x >> 16) >> (x & 15);
                if (x & 0x10)
                    v = (uint32_t)-(int32_t)v;
                if ((x & 0x3E0) == 0)
                    v = (x & 0x400) ? 0x8000u : 0;
                w = w << 16 | (v & 0xFFFF);
            }
            io->wmem(io->ctx, a, w);
        }
        io->run(io->ctx);
        rspo_hash_mem(io, &h, 0x100, 0x160);
    }
    return h.h;
}

/* ---- 65..88: vector loads and stores ---- */
static const uint8_t rspo_ls_scale[12] = {1, 2, 4, 8, 16, 16, 8, 8, 16, 16, 16, 16};

static uint32_t rspo_group_ls(const rspo_io *io, unsigned fn, int store)
{
    rspo_rng rng = {0xABCDEFu ^ (fn * 0x51ED27u) ^ (store ? 0x5A5A5A5Au : 0)};
    rspo_hash h = {2166136261u};
    uint32_t p[32];
    for (unsigned e = 0; e < 16; e++) {
        for (unsigned batch = 0; batch < 12; batch++) {
            int n = 0;
            int32_t off = (int32_t)(rspo_next(&rng) & 0x7F) - 64;
            unsigned vt = 8 + (rspo_next(&rng) & 7);
            uint32_t base;
            if (batch % 4 == 0) /* land near the end of DMEM, so the access wraps */
                base = (uint32_t)(0x1000 - (int32_t)(rspo_next(&rng) & 31) - off * rspo_ls_scale[fn]) & 0xFFFF;
            else
                base = rspo_next(&rng) & 0xFFFF;
            p[n++] = R_LW(1, 0xFF0, 0);
            p[n++] = R_ORI(2, 0, 0xF00);
            for (unsigned k = 0; k < 8; k++)
                p[n++] = R_LQV(8 + k, 0, k, 2);
            if (store) {
                p[n++] = R_SWC2(fn, vt, e, off, 1);
            } else {
                p[n++] = R_LWC2(fn, vt, e, off, 1);
                p[n++] = R_ORI(3, 0, 0xE00);
                for (unsigned k = 0; k < 8; k++)
                    p[n++] = R_SQV(8 + k, 0, k, 3);
            }
            p[n++] = R_BREAK;
            rspo_load_prog(io, p, n);
            rspo_fill_dmem(io, &rng, 0x000, 0x1000);
            io->wmem(io->ctx, 0xFF0, base);
            io->run(io->ctx);
            if (store)
                rspo_hash_mem(io, &h, 0x000, 0x1000);
            else
                rspo_hash_mem(io, &h, 0xE00, 0xE80);
        }
    }
    return h.h;
}

/* ---- 89: SP DMA ---- */
static uint32_t rspo_group_dma(const rspo_io *io)
{
    rspo_rng rng = {0xD3A0D3A0u};
    rspo_hash h = {2166136261u};
    for (unsigned c = 0; c < 40; c++) {
        uint32_t x = rspo_next(&rng);
        uint32_t mem = rspo_next(&rng) & 0x1FFF;
        uint32_t doff = rspo_next(&rng) & 0x7FF;
        uint32_t len, count, skip;
        if (c % 8 == 7) { /* one long row: wraps around the 4 KB bank */
            len = 0x800 | (x & 0x7FF);
            count = 0;
            skip = 0;
        } else {
            len = x & 0x1FF;
            count = (x >> 9) & 7;
            skip = (x >> 12) & 0xFF;
        }
        uint32_t lenreg = len | count << 12 | skip << 20;
        for (uint32_t a = 0; a < 0x2000; a += 4)
            io->wmem(io->ctx, a, rspo_next(&rng));
        for (uint32_t a = 0; a < RSPO_DRAM_SIZE; a += 4)
            io->wdram(io->ctx, a, rspo_next(&rng));
        io->wreg(io->ctx, 0, mem);
        io->wreg(io->ctx, 1, io->dram_base + doff);
        io->wreg(io->ctx, (x & 0x80000000u) ? 3 : 2, lenreg);
        for (int spin = 0; spin < 1000000 && (io->rreg(io->ctx, 6) || (io->rreg(io->ctx, 4) & 0x0C)); spin++)
            ;
        rspo_hword(&h, io->rreg(io->ctx, 0));
        rspo_hword(&h, io->rreg(io->ctx, 1) - io->dram_base);
        rspo_hword(&h, io->rreg(io->ctx, 2));
        rspo_hword(&h, io->rreg(io->ctx, 3));
        rspo_hash_mem(io, &h, 0x0000, 0x2000);
        for (uint32_t a = 0; a < RSPO_DRAM_SIZE; a += 4)
            rspo_hword(&h, io->rdram(io->ctx, a));
    }
    return h.h;
}

/* ---- 90: SP_STATUS pairs and the semaphore (halt, interrupt and single-step bits left alone) ---- */
static uint32_t rspo_group_status(const rspo_io *io)
{
    rspo_rng rng = {0x57A7u};
    rspo_hash h = {2166136261u};
    const uint32_t allowed = 0x01FFFF84u; /* broke clear, intr-break pair, the eight signal pairs */
    for (unsigned i = 0; i < 64; i++) {
        io->wreg(io->ctx, 4, rspo_next(&rng) & allowed);
        rspo_hword(&h, io->rreg(io->ctx, 4));
    }
    io->wreg(io->ctx, 7, 0);
    rspo_hword(&h, io->rreg(io->ctx, 7));
    rspo_hword(&h, io->rreg(io->ctx, 7));
    rspo_hword(&h, io->rreg(io->ctx, 7));
    io->wreg(io->ctx, 7, 0x12345678u);
    rspo_hword(&h, io->rreg(io->ctx, 7));
    io->wreg(io->ctx, 7, 0);
    io->wreg(io->ctx, 4, 0x00AAAA80u); /* clear every signal and intr-break */
    rspo_hword(&h, io->rreg(io->ctx, 4));
    return h.h;
}

/* ---- 91: scalar unit ---- */
static uint32_t rspo_group_scalar(const rspo_io *io)
{
    rspo_rng rng = {0x5CA1A2u};
    rspo_hash h = {2166136261u};
    uint32_t p[128];
    int n = 0, j;
    for (unsigned k = 1; k <= 8; k++)
        p[n++] = R_LW(k, 4 * (k - 1), 0);
    p[n++] = R_ADDU(9, 1, 2);
    p[n++] = R_SUBU(10, 1, 3);
    p[n++] = R_AND(11, 2, 4);
    p[n++] = R_OR(12, 3, 5);
    p[n++] = R_XOR(13, 4, 6);
    p[n++] = R_NOR(14, 5, 7);
    p[n++] = R_SLT(15, 1, 5);
    p[n++] = R_SLTU(16, 1, 5);
    p[n++] = R_SLL(17, 6, 7);
    p[n++] = R_SRL(18, 6, 13);
    p[n++] = R_SRA(19, 6, 9);
    p[n++] = R_SLLV(20, 7, 8);
    p[n++] = R_SRLV(21, 7, 8);
    p[n++] = R_SRAV(22, 7, 8);
    p[n++] = R_ADDI(23, 1, -1234);
    p[n++] = R_SLTI(24, 2, -5);
    p[n++] = R_SLTIU(25, 2, 0x8000);
    p[n++] = R_ANDI(26, 3, 0xF0F0);
    p[n++] = R_ORI(27, 4, 0x1234);
    p[n++] = R_XORI(28, 5, 0xFFFF);
    p[n++] = R_LUI(29, 0x8001);
    p[n++] = R_ADD(30, 1, 2);
    p[n++] = R_SUB(0, 1, 2); /* r0 stays 0 */
    for (unsigned k = 0; k < 23; k++)
        p[n++] = R_SW(k == 22 ? 0 : 9 + k, 0x100 + 4 * k, 0);
    /* Branches: r9 collects which paths ran, delay slots included. */
    p[n++] = R_ORI(9, 0, 0);
    p[n++] = R_BEQ(1, 2, 2);
    p[n++] = R_ADDIU(9, 9, 1);          /* delay slot: always */
    p[n++] = R_ADDIU(9, 9, 0x10);       /* skipped when taken */
    p[n++] = R_BLTZ(3, 2);
    p[n++] = R_ADDIU(9, 9, 0x100);
    p[n++] = R_ADDIU(9, 9, 0x1000);
    p[n++] = R_BGEZAL(4, 2);            /* links r31 whether taken or not */
    p[n++] = R_ADDIU(9, 9, 0x2);
    p[n++] = R_ADDIU(9, 9, 0x20);
    p[n++] = R_BLEZ(5, 2);
    p[n++] = R_ADDIU(9, 9, 0x200);
    p[n++] = R_ADDIU(9, 9, 0x2000);
    p[n++] = R_BGTZ(6, 2);
    p[n++] = R_NOP;
    p[n++] = R_ADDIU(9, 9, 0x4);
    p[n++] = R_BNE(7, 8, 2);
    p[n++] = R_NOP;
    p[n++] = R_ADDIU(9, 9, 0x40);
    p[n++] = R_BLTZAL(1, 1);
    p[n++] = R_ADDU(10, 31, 0);         /* delay slot sees the new link */
    p[n++] = R_NOP;                     /* skipped when taken */
    j = n;
    p[n++] = R_JAL(4 * (j + 3));        /* skip one instruction */
    p[n++] = R_ADDU(11, 31, 0);
    p[n++] = R_ADDIU(9, 9, 0x400);      /* skipped */
    p[n++] = R_ADDU(12, 31, 0);
    j = n;
    p[n++] = R_ORI(13, 0, 4 * (j + 4));
    p[n++] = R_JALR(14, 13);
    p[n++] = R_ADDIU(9, 9, 0x8);
    p[n++] = R_ADDIU(9, 9, 0x800);      /* skipped */
    p[n++] = R_SW(9, 0x180, 0);
    p[n++] = R_SW(10, 0x184, 0);
    p[n++] = R_SW(11, 0x188, 0);
    p[n++] = R_SW(12, 0x18C, 0);
    p[n++] = R_SW(14, 0x190, 0);
    p[n++] = R_SW(31, 0x194, 0);
    /* Unaligned, wrapping loads and stores. */
    p[n++] = R_LH(15, 0xFFF, 0);
    p[n++] = R_LW(16, 0xFFE, 0);
    p[n++] = R_LHU(17, 0x0FFF, 0);
    p[n++] = R_LB(18, 0x123, 0);
    p[n++] = R_LBU(19, 0x123, 0);
    p[n++] = R_ANDI(20, 5, 0xFFF);
    p[n++] = R_LW(21, 1, 20);
    p[n++] = R_LH(22, -3, 20);
    p[n++] = R_SW(6, 0xFFD, 0);
    p[n++] = R_SH(7, 0x0FFF, 0);
    p[n++] = R_SB(8, 0x1FF, 0);
    p[n++] = R_SW(1, 0x203, 0);
    for (unsigned k = 0; k < 8; k++)
        p[n++] = R_SW(15 + k, 0x1A0 + 4 * k, 0);
    p[n++] = R_BREAK;
    rspo_load_prog(io, p, n);
    for (unsigned batch = 0; batch < 64; batch++) {
        rspo_fill_dmem(io, &rng, 0x000, 0x1000);
        for (uint32_t a = 0; a < 0x20; a += 4)
            io->wmem(io->ctx, a, rspo_next(&rng));
        if (batch & 1) /* equal operands for BEQ / BNE */
            io->wmem(io->ctx, 0x04, io->rmem(io->ctx, 0x00));
        if (batch & 2)
            io->wmem(io->ctx, 0x1C, io->rmem(io->ctx, 0x18));
        io->run(io->ctx);
        rspo_hash_mem(io, &h, 0x000, 0x1000);
    }
    return h.h;
}

/* ---- 92: the divide ROMs ---- */
static uint32_t rspo_group_roms(const rspo_io *io)
{
    rspo_hash h = {2166136261u};
    uint32_t p[48];
    int n = 0;
    p[n++] = R_LQV(1, 0, 0, 0); /* VRCP inputs */
    p[n++] = R_LQV(2, 0, 1, 0); /* VRSQL inputs, high halves (the low halves are 0: v5) */
    p[n++] = R_VOP(RV_VXOR, 5, 5, 5, 0);
    for (unsigned i = 0; i < 8; i++)
        p[n++] = R_VOP(RV_VRCP, 3, i, 1, 8 + i);
    for (unsigned i = 0; i < 8; i++) {
        p[n++] = R_VOP(RV_VRSQH, 6, i, 2, 8 + i);
        p[n++] = R_VOP(RV_VRSQL, 4, i, 5, 8 + i);
    }
    p[n++] = R_SQV(3, 0, 16, 0);
    p[n++] = R_SQV(4, 0, 17, 0);
    p[n++] = R_BREAK;
    rspo_load_prog(io, p, n);
    for (uint32_t run = 0; run < 64; run++) {
        for (uint32_t i = 0; i < 8; i += 2) {
            uint32_t k0 = run * 8 + i, k1 = k0 + 1;
            uint32_t x0 = k0 < 256 ? (0x2000u + k0 * 0x20u) : (0x1000u + (k0 - 256) * 0x10u);
            uint32_t x1 = k1 < 256 ? (0x2000u + k1 * 0x20u) : (0x1000u + (k1 - 256) * 0x10u);
            io->wmem(io->ctx, 2 * i, (0x4000u + 32 * k0) << 16 | (0x4000u + 32 * k1));
            io->wmem(io->ctx, 0x10 + 2 * i, x0 << 16 | x1);
        }
        io->run(io->ctx);
        rspo_hash_mem(io, &h, 0x100, 0x120);
    }
    return h.h;
}

static uint32_t rspo_group(const rspo_io *io, unsigned g)
{
    if (g < 64)
        return rspo_group_vop(io, g);
    if (g == 64)
        return rspo_group_div(io);
    if (g < 77)
        return rspo_group_ls(io, g - 65, 0);
    if (g < 89)
        return rspo_group_ls(io, g - 77, 1);
    if (g == 89)
        return rspo_group_dma(io);
    if (g == 90)
        return rspo_group_status(io);
    if (g == 91)
        return rspo_group_scalar(io);
    return rspo_group_roms(io);
}

#endif
