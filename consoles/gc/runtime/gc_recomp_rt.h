/* gc_recomp_rt.h -- the ABI between lifted GameCube code (gc_tools.recomp, on the shared PowerPC lifter tools/ppc)
 * and the core (XBA-225 spike, Gekko layer completed by XBA-260). Part of GC_RECOMP_ABI (gc_recomp.h): change it
 * together with the tools and bump GC_RECOMP_ABI.
 *
 * Lifted code works on the machine's registers in place (gc_cpu below: no locals to sync around bus calls), one C
 * function per discovered function, cut into blocks:
 * - every block starts with RC_BLOCK(pc, cycles, n): when the clock has reached the next event (VI line, decrementer,
 *   interrupt made pending by a register write) it returns to the dispatcher with PC = the block, else it adds the
 *   block's cycle estimate (the sum of gc_cycles() of its n instructions, the interpreter's per-instruction cost) to
 *   the clock. Time-base / decrementer reads mid-block take back the cycles of the instructions after them
 *   (RC_MFSPR_T), and hand-offs mid-block give back the cycles not run (RC_INTERP_T), so lifted code and the
 *   interpreter read the same time at the same instruction;
 * - calls, returns, indirect jumps and branches out of the function return to the dispatcher with PC set (bounded C
 *   stack: libogc switches threads by swapping r1 and LR under the code's feet);
 * - an instruction the lifter does not take (rfi, sc, mtsr, FPSCR moves, unknown encodings) is a block of its own:
 *   RC_INTERP hands it to the interpreter, which runs it and then dispatch goes on;
 * - an FP instruction with MSR[FP] clear, a paired-single one with HID2[PSE] / [LSQE] clear, dcbz_l with HID2[LCE]
 *   clear, a privileged instruction with MSR[PR] set go to the interpreter too, which raises FP unavailable / program;
 *   tw / twi raise the program exception themselves (RC_TRAP);
 * - memory: MEM1 (24 MB) inline, byte-swapped (PowerPC is big-endian), when the address translation is the default
 *   one (see rc_pa: real mode, or the BATs map 0x80000000 and 0xC0000000 onto MEM1 the way the IPL and the SDK set
 *   them: the physical address is then EA & 0x3FFFFFFF); everything else goes through the core (gc_load / gc_store
 *   with the instruction's address: BATs, page table, locked cache, hardware registers), which on a DSI sets up the
 *   exception and longjmps back to the run loop (the instruction has no effect, the rest of the block does not run).
 * The Gekko layer is at the end: single-precision results fill both paired-single slots, psq_l / psq_st quantise
 * through the GQRs, paired-single arithmetic rounds to single, fres / frsqrte use the 750's estimate tables.
 * The host executable (gc_run) exports the bus and SPR functions for the pack to bind to. */
#ifndef GC_RECOMP_RT_H
#define GC_RECOMP_RT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ppc_ops.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the GameCube runtime assumes a little-endian host (it byte-swaps guest memory)"
#endif

#include "gc_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC_RAM_SIZE 0x01800000u

typedef struct gc_cpu {
    uint32_t gpr[32];
    double ps0[32];    /* the FPRs */
    double ps1[32];    /* Gekko: the second paired-single slot */
    uint32_t pc;       /* the next instruction */
    uint32_t lr, ctr, cr;
    uint32_t xer_so, xer_ov, xer_ca; /* 0 or 1 */
    uint32_t xer_bc;                 /* XER byte count (lswx / stswx) */
    uint32_t msr, fpscr;
    uint32_t reserve;                /* lwarx reservation held */
    uint32_t sr[16];
    uint32_t spr[1024];              /* every other SPR (GQRs at 912..919, HID2 at 920) */
} gc_cpu;

typedef struct gc_rc_head {
    gc_cpu cpu;
    uint64_t now;        /* CPU cycles (gc_cycles() estimate per instruction) since creation */
    uint64_t next_event; /* the CPU runs until now reaches it */
    uint8_t *ram;        /* MEM1, GC_RAM_SIZE bytes, big-endian as on the console */
    uint8_t rc_exit;     /* GC_RC_EXIT_* */
    uint8_t pad_[7];
    uint64_t rc_native;  /* instructions run natively (packs built with -DGC_RC_COUNT) */
    /* Data accesses take the inline MEM1 path when rc_pa() < GC_RAM_SIZE (gc_mmu.c keeps these up to date):
     * real mode 0 / 0, default BATs 0x80000000 / 0x80000000 (EA >= 0x80000000), anything else 0 / ~0 (never). */
    uint32_t dfast_mask, dfast_val;
} gc_rc_head;

enum {
    GC_RC_EXIT_NORMAL = 0,      /* PC says where execution continues */
    GC_RC_EXIT_INTERP = 1,      /* the interpreter runs the instruction at PC, then dispatch goes on */
    GC_RC_EXIT_NOT_COVERED = 2, /* this function does not hold PC: nothing ran */
};

/* The core (gc_mmu.c, gc_cpu.c). pc: the address of the instruction making the access (SRR0 of a DSI). */
uint32_t gc_load(gc_machine *m, uint32_t ea, unsigned size, uint32_t pc);
void gc_store(gc_machine *m, uint32_t ea, uint32_t v, unsigned size, uint32_t pc);
uint32_t gc_mfspr(gc_machine *m, unsigned spr);
uint32_t gc_mfspr_t(gc_machine *m, unsigned spr, unsigned back); /* time read `back` cycles before the clock */
void gc_mtspr(gc_machine *m, unsigned spr, uint32_t v);  /* may make an event due now */
void gc_mtmsr(gc_machine *m, uint32_t v);                /* idem (MSR[EE] set with an interrupt pending) */
void gc_cache_op(gc_machine *m, uint32_t ea, unsigned kind, uint32_t pc); /* GC_CACHE_* */
void gc_program(gc_machine *m, uint32_t pc, uint32_t srr1_bits); /* program exception at pc (SRR1 cause bits) */
/* Translated accesses for code that is not an instruction (no DSI: unmapped reads 0). */
uint8_t gc_read8(gc_machine *m, uint32_t ea);
uint16_t gc_read16(gc_machine *m, uint32_t ea);
uint32_t gc_read32(gc_machine *m, uint32_t ea);
void gc_write8(gc_machine *m, uint32_t ea, uint8_t v);
void gc_write16(gc_machine *m, uint32_t ea, uint16_t v);
void gc_write32(gc_machine *m, uint32_t ea, uint32_t v);

enum { GC_CACHE_DCBZ = 0, GC_CACHE_ICBI = 1, GC_CACHE_DCBZ_L = 2 };

/* HID2 (Gekko): load / store quantised, write-gather pipe, paired singles, locked cache enables. */
#define GC_HID2_LSQE 0x80000000u
#define GC_HID2_WPE 0x40000000u
#define GC_HID2_PSE 0x20000000u
#define GC_HID2_LCE 0x10000000u

/* ---- memory (big-endian guest) ---- */

static inline uint16_t rc_be16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}

static inline uint32_t rc_be32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

static inline void rc_put16(uint8_t *p, uint16_t v)
{
    v = __builtin_bswap16(v);
    memcpy(p, &v, 2);
}

static inline void rc_put32(uint8_t *p, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(p, &v, 4);
}

#define RC_HEAD(m) ((gc_rc_head *)(m))

/* The MEM1 offset of a data access when translation is the default one, else an address >= GC_RAM_SIZE. */
static inline uint32_t rc_pa(gc_machine *m, uint32_t ea)
{
    const gc_rc_head *h = RC_HEAD(m);
    return (ea & 0x3FFFFFFFu) | ((ea & h->dfast_mask) ^ h->dfast_val);
}

static inline uint32_t rc_lbz(gc_machine *m, uint32_t ea, uint32_t pc)
{
    uint32_t pa = rc_pa(m, ea);
    return pa < GC_RAM_SIZE ? RC_HEAD(m)->ram[pa] : gc_load(m, ea, 1, pc);
}

static inline uint32_t rc_lhz(gc_machine *m, uint32_t ea, uint32_t pc)
{
    uint32_t pa = rc_pa(m, ea);
    return pa <= GC_RAM_SIZE - 2 ? rc_be16(RC_HEAD(m)->ram + pa) : gc_load(m, ea, 2, pc);
}

static inline uint32_t rc_lwz(gc_machine *m, uint32_t ea, uint32_t pc)
{
    uint32_t pa = rc_pa(m, ea);
    return pa <= GC_RAM_SIZE - 4 ? rc_be32(RC_HEAD(m)->ram + pa) : gc_load(m, ea, 4, pc);
}

static inline uint64_t rc_ld64(gc_machine *m, uint32_t ea, uint32_t pc)
{
    uint32_t hi = rc_lwz(m, ea, pc);
    return (uint64_t)hi << 32 | rc_lwz(m, ea + 4, pc);
}

static inline void rc_stb(gc_machine *m, uint32_t ea, uint32_t v, uint32_t pc)
{
    uint32_t pa = rc_pa(m, ea);
    if (pa < GC_RAM_SIZE)
        RC_HEAD(m)->ram[pa] = (uint8_t)v;
    else
        gc_store(m, ea, v & 0xFFu, 1, pc);
}

static inline void rc_sth(gc_machine *m, uint32_t ea, uint32_t v, uint32_t pc)
{
    uint32_t pa = rc_pa(m, ea);
    if (pa <= GC_RAM_SIZE - 2)
        rc_put16(RC_HEAD(m)->ram + pa, (uint16_t)v);
    else
        gc_store(m, ea, v & 0xFFFFu, 2, pc);
}

static inline void rc_stw(gc_machine *m, uint32_t ea, uint32_t v, uint32_t pc)
{
    uint32_t pa = rc_pa(m, ea);
    if (pa <= GC_RAM_SIZE - 4)
        rc_put32(RC_HEAD(m)->ram + pa, v);
    else
        gc_store(m, ea, v, 4, pc);
}

static inline void rc_st64(gc_machine *m, uint32_t ea, uint64_t v, uint32_t pc)
{
    rc_stw(m, ea, (uint32_t)(v >> 32), pc);
    rc_stw(m, ea + 4, (uint32_t)v, pc);
}

/* ---- the Gekko layer: paired singles, quantised loads / stores, estimates ---- */

/* Single-precision results fill both slots (Gekko); double-precision ones write ps0 only. */
static inline void gc_fset_single(gc_cpu *c, unsigned d, double v)
{
    double s = ppc_single(v);
    c->ps0[d] = s;
    c->ps1[d] = s;
}

/* lfs / lfsx...: the loaded single (exact, an SNaN stays signalling) in both slots, no rounding. */
static inline void gc_fset_pair(gc_cpu *c, unsigned d, double v)
{
    c->ps0[d] = v;
    c->ps1[d] = v;
}

static inline double gc_ps_round(double v)
{
    return ppc_single(v);
}

/* fres / ps_res: the 750's reciprocal estimate, bit for bit. A 32-entry table indexed by the top 5 fraction bits,
 * linear in the next 10 (base - (dec * offset + 1) / 2), giving a 23-bit fraction; exponent 2045 - e. Inputs whose
 * reciprocal leaves the single range give the largest single (tiny inputs, denormals included) or zero (huge
 * ones). The table values are the hardware's (measured on consoles and published by the emulation community):
 * facts, no code taken from anywhere. */
static inline double gc_fres(double d)
{
    static const uint32_t base[32] = {
        0x7ff800, 0x783800, 0x70ea00, 0x6a0800, 0x638800, 0x5d6200, 0x579000, 0x520800,
        0x4cc800, 0x47ca00, 0x430800, 0x3e8000, 0x3a2c00, 0x360800, 0x321400, 0x2e4a00,
        0x2aa800, 0x272c00, 0x23d600, 0x209e00, 0x1d8800, 0x1a9000, 0x17ae00, 0x14f800,
        0x124400, 0x0fbe00, 0x0d3800, 0x0ade00, 0x088400, 0x065000, 0x041c00, 0x020c00,
    };
    static const uint16_t dec[32] = {
        0x3e1, 0x3a7, 0x371, 0x340, 0x313, 0x2ea, 0x2c4, 0x2a0, 0x27f, 0x261, 0x245, 0x22a, 0x212, 0x1fb, 0x1e5, 0x1d1,
        0x1be, 0x1ac, 0x19b, 0x18b, 0x17c, 0x16e, 0x15b, 0x15b, 0x143, 0x143, 0x12d, 0x12d, 0x11a, 0x11a, 0x108, 0x106,
    };
    uint64_t u = ppc_f2u(d), sign = u & 0x8000000000000000ull, frac = u & 0x000FFFFFFFFFFFFFull;
    unsigned e = (unsigned)(u >> 52) & 0x7FF;
    if (e == 0x7FF)
        return frac ? ppc_u2f(u | 0x0008000000000000ull) : ppc_u2f(sign); /* NaN: quiet; inf: 0 */
    if (e == 0 && frac == 0)
        return ppc_u2f(sign | 0x7FF0000000000000ull);
    if (e < 895) /* 1/x above the single range: the largest single */
        return ppc_u2f(sign | 0x47EFFFFFE0000000ull);
    if (e >= 1149)
        return ppc_u2f(sign);
    unsigned i = (unsigned)(frac >> 37), k = i >> 10, off = i & 0x3FF;
    uint64_t f = base[k] - (dec[k] * off + 1) / 2;
    return ppc_u2f(sign | (uint64_t)(0x7FD - e) << 52 | f << 29);
}

/* frsqrte / ps_rsqrte: the 750's reciprocal square root estimate, bit for bit. A 32-entry table: the exponent's
 * parity picks a half, the top 4 fraction bits the entry, linear in the next 11 (base + dec * offset, dec < 0),
 * a 26-bit fraction; exponent 1022 - floor((e - 1023) / 2). Denormals are normalised first. Hardware values as for
 * gc_fres. */
static inline double gc_frsqrte(double d)
{
    static const int32_t base[32] = {
        0x1a7e800, 0x17cb800, 0x1552800, 0x130c000, 0x10f2000, 0x0eff000, 0x0d2e000, 0x0b7c000,
        0x09e5000, 0x0867000, 0x06ff000, 0x05ab800, 0x046a000, 0x0339800, 0x0218800, 0x0105800,
        0x3ffa000, 0x3c29000, 0x38aa000, 0x3572000, 0x3279000, 0x2fb7000, 0x2d26000, 0x2ac0000,
        0x2881000, 0x2665000, 0x2468000, 0x2287000, 0x20c1000, 0x1f12000, 0x1d79000, 0x1bf4000,
    };
    static const int16_t dec[32] = {
        -0x568, -0x4f3, -0x48d, -0x435, -0x3e7, -0x3a2, -0x365, -0x32e,
        -0x2fc, -0x2d0, -0x2a8, -0x283, -0x261, -0x243, -0x226, -0x20b,
        -0x7a4, -0x700, -0x670, -0x5f2, -0x584, -0x524, -0x4cc, -0x47e,
        -0x43a, -0x3fa, -0x3c2, -0x38e, -0x35e, -0x332, -0x30a, -0x2e6,
    };
    uint64_t u = ppc_f2u(d), sign = u & 0x8000000000000000ull, frac = u & 0x000FFFFFFFFFFFFFull;
    int e = (int)(u >> 52) & 0x7FF;
    if (e == 0x7FF) {
        if (frac)
            return ppc_u2f(u | 0x0008000000000000ull); /* NaN: quiet */
        return sign ? ppc_u2f(0x7FF8000000000000ull) : 0.0;
    }
    if (e == 0 && frac == 0)
        return ppc_u2f(sign | 0x7FF0000000000000ull);
    if (sign)
        return ppc_u2f(0x7FF8000000000000ull);
    if (e == 0) { /* denormal: normalise (value = 1.f * 2^(e - 1023) with e = 1 - shifts) */
        e = 1;
        while (!(frac & 0x0010000000000000ull)) {
            frac <<= 1;
            e--;
        }
        frac &= 0x000FFFFFFFFFFFFFull;
    }
    unsigned i = (unsigned)((((uint64_t)(e & 1) << 52) | frac) >> 37), k = i >> 11, off = i & 0x7FF;
    uint64_t f = (uint64_t)(base[k] + dec[k] * (int32_t)off);
    int re = 1022 - ((e - 1023) >> 1); /* arithmetic shift: floor */
    return ppc_u2f((uint64_t)(re & 0x7FF) << 52 | f << 26);
}

static inline int gc_gqr_scale(uint32_t field)
{
    return (field & 0x20) ? (int)field - 64 : (int)field;
}

static inline unsigned gc_quant_size(unsigned type)
{
    return type == 4 || type == 6 ? 1u : type == 5 || type == 7 ? 2u : 4u;
}

/* Dequantise: u8 / u16 / s8 / s16 times 2^-scale (exact in single precision); types 0-3 load a single as lfs
 * does (exact, NaNs kept as they are; the 750 manual reserves types 1-3, they behave as type 0). */
static inline double gc_dequant(gc_machine *m, uint32_t ea, unsigned type, int scale, uint32_t pc)
{
    double v;
    switch (type) {
    case 4: v = (double)(uint8_t)rc_lbz(m, ea, pc); break;
    case 5: v = (double)(uint16_t)rc_lhz(m, ea, pc); break;
    case 6: v = (double)(int8_t)rc_lbz(m, ea, pc); break;
    case 7: v = (double)(int16_t)rc_lhz(m, ea, pc); break;
    default: return ppc_lfs_bits(rc_lwz(m, ea, pc));
    }
    return ldexp(v, -scale);
}

/* Quantise: v times 2^scale, toward zero, saturated to the type's range; NaN gives 0. */
static inline int32_t gc_sat(double v, double lo, double hi)
{
    if (v != v)
        return 0;
    if (v <= lo)
        return (int32_t)lo;
    if (v >= hi)
        return (int32_t)hi;
    return (int32_t)v;
}

static inline void gc_quant(gc_machine *m, uint32_t ea, unsigned type, int scale, double v, uint32_t pc)
{
    if (type < 4) { /* as stfs: no rounding */
        rc_stw(m, ea, ppc_stfs_bits(v), pc);
        return;
    }
    double x = ldexp(v, scale);
    switch (type) {
    case 4: rc_stb(m, ea, (uint32_t)gc_sat(x, 0.0, 255.0), pc); break;
    case 5: rc_sth(m, ea, (uint32_t)gc_sat(x, 0.0, 65535.0), pc); break;
    case 6: rc_stb(m, ea, (uint32_t)gc_sat(x, -128.0, 127.0), pc); break;
    default: rc_sth(m, ea, (uint32_t)gc_sat(x, -32768.0, 32767.0), pc); break;
    }
}

/* psq_l / psq_lx / psq_lu / psq_lux: GQR i's load half (type bits 16-18, scale 24-29); W = 1 loads one value and
 * sets ps1 to 1.0. Both values are read before either slot is written (a DSI on the second leaves frD as it was). */
static inline void gc_psq_load(gc_machine *m, unsigned d, uint32_t ea, unsigned w, unsigned i, uint32_t pc)
{
    gc_cpu *c = &RC_HEAD(m)->cpu;
    uint32_t gqr = c->spr[PPC_SPR_GQR0 + (i & 7)];
    unsigned type = (gqr >> 16) & 7;
    int scale = gc_gqr_scale((gqr >> 24) & 63);
    double p0 = gc_dequant(m, ea, type, scale, pc);
    double p1 = w ? 1.0 : gc_dequant(m, ea + gc_quant_size(type), type, scale, pc);
    c->ps0[d] = p0;
    c->ps1[d] = p1;
}

/* psq_st family: GQR i's store half (type bits 0-2, scale 8-13). */
static inline void gc_psq_store(gc_machine *m, unsigned s, uint32_t ea, unsigned w, unsigned i, uint32_t pc)
{
    gc_cpu *c = &RC_HEAD(m)->cpu;
    uint32_t gqr = c->spr[PPC_SPR_GQR0 + (i & 7)];
    unsigned type = gqr & 7;
    int scale = gc_gqr_scale((gqr >> 8) & 63);
    gc_quant(m, ea, type, scale, c->ps0[s], pc);
    if (!w)
        gc_quant(m, ea + gc_quant_size(type), type, scale, c->ps1[s], pc);
}

/* ---- lifted-code vocabulary (tools/ppc/lift.py, gc_tools/gekko.py) ---- */

#define RC_FN void
#define RC_EXPORT __attribute__((visibility("default")))
#define RC_PROLOGUE                                                                                                    \
    gc_rc_head *h_ = (gc_rc_head *)m_;                                                                                 \
    gc_cpu *c = &h_->cpu;                                                                                              \
    (void)c
#ifdef GC_RC_COUNT
#define RC_COUNT(n) (h_->rc_native += (n))
#else
#define RC_COUNT(n) ((void)0)
#endif
#define RC_BLOCK(pc_, cycles, n)                                                                                       \
    do {                                                                                                               \
        if (h_->now >= h_->next_event) {                                                                               \
            c->pc = (pc_);                                                                                             \
            return;                                                                                                    \
        }                                                                                                              \
        h_->now += (cycles);                                                                                           \
        RC_COUNT(n);                                                                                                   \
    } while (0)
#define RC_EXIT(pc_)                                                                                                   \
    do {                                                                                                               \
        c->pc = (pc_);                                                                                                 \
        return;                                                                                                        \
    } while (0)
#define RC_INTERP(pc_)                                                                                                 \
    do {                                                                                                               \
        c->pc = (pc_);                                                                                                 \
        h_->rc_exit = GC_RC_EXIT_INTERP;                                                                               \
        return;                                                                                                        \
    } while (0)
/* A hand-off mid-block: the block's cycles from this instruction on were counted but do not run here. */
#define RC_INTERP_T(pc_, back)                                                                                         \
    do {                                                                                                               \
        h_->now -= (back);                                                                                             \
        RC_INTERP(pc_);                                                                                                \
    } while (0)
#define RC_NOT_COVERED()                                                                                               \
    do {                                                                                                               \
        h_->rc_exit = GC_RC_EXIT_NOT_COVERED;                                                                          \
        return;                                                                                                        \
    } while (0)
/* After an instruction that can make an event due (mtmsr, mtspr): leave if it did. */
#define RC_CHECK(next_)                                                                                                \
    do {                                                                                                               \
        if (h_->now >= h_->next_event) {                                                                               \
            c->pc = (next_);                                                                                           \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)
#define RC_FPCHECK(pc_)                                                                                                \
    do {                                                                                                               \
        if (!(c->msr & PPC_MSR_FP))                                                                                    \
            RC_INTERP(pc_);                                                                                            \
    } while (0)
/* The guards of a block's first FP / Gekko instruction: MSR[FP] and the HID2 enables `bits` it needs. */
#define RC_GKCHECK_T(pc_, bits, back)                                                                                  \
    do {                                                                                                               \
        if (!(c->msr & PPC_MSR_FP) || (c->spr[PPC_SPR_HID2] & (bits)) != (bits))                                       \
            RC_INTERP_T(pc_, back);                                                                                    \
    } while (0)
/* dcbz_l with the locked cache off: the interpreter raises the program exception. */
#define RC_LCCHECK_T(pc_, back)                                                                                        \
    do {                                                                                                               \
        if (!(c->spr[PPC_SPR_HID2] & GC_HID2_LCE))                                                                     \
            RC_INTERP_T(pc_, back);                                                                                    \
    } while (0)
/* Privileged instructions in user mode: the interpreter raises the program exception. */
#define RC_PRIV_T(pc_, back)                                                                                           \
    do {                                                                                                               \
        if (c->msr & PPC_MSR_PR)                                                                                       \
            RC_INTERP_T(pc_, back);                                                                                    \
    } while (0)
/* tw / twi whose condition holds: program exception (trap) at pc_; `back`: cycles of the block after it. */
#define RC_TRAP(pc_, back)                                                                                             \
    do {                                                                                                               \
        h_->now -= (back);                                                                                             \
        gc_program(m_, (pc_), 0x00020000u);                                                                            \
        return;                                                                                                        \
    } while (0)
/* A branch to itself: an idle loop; the clock jumps to the next event. */
#define RC_IDLE(pc_)                                                                                                   \
    do {                                                                                                               \
        if (h_->next_event > h_->now)                                                                                  \
            h_->now = h_->next_event;                                                                                  \
        c->pc = (pc_);                                                                                                 \
        return;                                                                                                        \
    } while (0)

#define RC_MACHINE gc_machine
#define R(n) c->gpr[n]
#define F0(n) c->ps0[n]
#define F1(n) c->ps1[n]
/* Memory: the instruction's address first (SRR0 of a DSI). */
#define RC_LBZ(pc, ea) rc_lbz(m_, (ea), (pc))
#define RC_LHZ(pc, ea) rc_lhz(m_, (ea), (pc))
#define RC_LWZ(pc, ea) rc_lwz(m_, (ea), (pc))
#define RC_LD64(pc, ea) rc_ld64(m_, (ea), (pc))
#define RC_STB(pc, ea, v) rc_stb(m_, (ea), (v), (pc))
#define RC_STH(pc, ea, v) rc_sth(m_, (ea), (v), (pc))
#define RC_STW(pc, ea, v) rc_stw(m_, (ea), (v), (pc))
#define RC_ST64(pc, ea, v) rc_st64(m_, (ea), (v), (pc))
#define RC_DCBZ(pc, ea) gc_cache_op(m_, (ea), GC_CACHE_DCBZ, (pc))
#define RC_DCBZ_L(pc, ea) gc_cache_op(m_, (ea), GC_CACHE_DCBZ_L, (pc))
#define RC_ICBI(pc, ea) gc_cache_op(m_, (ea), GC_CACHE_ICBI, (pc))
#define RC_PSQ_L(pc, d, ea, w, i) gc_psq_load(m_, (d), (ea), (w), (i), (pc))
#define RC_PSQ_ST(pc, s, ea, w, i) gc_psq_store(m_, (s), (ea), (w), (i), (pc))
#define RC_MFSPR(n) gc_mfspr(m_, (n))
#define RC_MFSPR_T(n, back) gc_mfspr_t(m_, (n), (back))
#define RC_MTSPR(n, v) gc_mtspr(m_, (n), (v))
#define RC_MTMSR(v) gc_mtmsr(m_, (v))
#define RC_XER() (c->xer_so << 31 | c->xer_ov << 30 | c->xer_ca << 29 | c->xer_bc)
#define RC_SET_XER(v)                                                                                                  \
    do {                                                                                                               \
        uint32_t x_ = (v);                                                                                             \
        c->xer_so = x_ >> 31;                                                                                          \
        c->xer_ov = (x_ >> 30) & 1;                                                                                    \
        c->xer_ca = (x_ >> 29) & 1;                                                                                    \
        c->xer_bc = x_ & 0x7F;                                                                                         \
    } while (0)
/* Record forms: CR0 from a result. */
#define RC_CR0(v) (c->cr = ppc_cr_set(c->cr, 0, ppc_cmps((int32_t)(v), 0, c->xer_so)))
/* Single-precision FP results (fadds..., frsp): both slots, rounded; lfs: both slots, exact. */
#define RC_FSET_S(d, v) gc_fset_single(c, (d), (v))
#define RC_FSET_L(d, v) gc_fset_pair(c, (d), (v))
#define RC_FSET_D(d, v) (c->ps0[d] = (v))
#define RC_PS(v) gc_ps_round(v)

#ifdef __cplusplus
}
#endif

#endif
