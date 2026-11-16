/* gc_recomp_rt.h -- the ABI between lifted GameCube code (gc_tools.recomp, on the shared PowerPC lifter tools/ppc)
 * and the core (XBA-225 spike). Part of GC_RECOMP_ABI (gc_recomp.h): change it together with the tools and bump
 * GC_RECOMP_ABI.
 *
 * Lifted code works on the machine's registers in place (gc_cpu below: no locals to sync around bus calls), one C
 * function per discovered function, cut into blocks:
 * - every block starts with RC_BLOCK(pc, n): when the clock has reached the next event (VI line, decrementer,
 *   interrupt made pending by a register write) it returns to the dispatcher with PC = the block, else it counts the
 *   block's n instructions (one cycle each, like the interpreter);
 * - calls, returns, indirect jumps and branches out of the function return to the dispatcher with PC set (bounded C
 *   stack: libogc switches threads by swapping r1 and LR under the code's feet);
 * - an instruction the lifter does not take (rfi, sc, tw, mtsr, the cache-locking dcbz_l, unknown encodings) is a
 *   block of its own: RC_INTERP hands it to the interpreter, which runs it and then dispatch goes on;
 * - an FP instruction with MSR[FP] clear goes to the interpreter too (it raises the FP-unavailable exception that
 *   libogc's lazy FPU context switch relies on);
 * - memory: MEM1 (24 MB) inline, byte-swapped (PowerPC is big-endian); everything else through the core's bus
 *   (gc_read8 / 16 / 32, gc_write8 / 16 / 32). Effective addresses are not translated through the BATs: the
 *   physical address is EA & 0x3FFFFFFF, which is what libogc's BAT setup gives for 0x8xxxxxxx (cached) and
 *   0xCxxxxxxx (uncached), and what real mode gives for low addresses.
 * The Gekko layer is at the end: single-precision results fill both paired-single slots, psq_l / psq_st quantise
 * through the GQRs, paired-single arithmetic rounds to single.
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
    uint64_t now;        /* CPU cycles (one per instruction) since creation */
    uint64_t next_event; /* the CPU runs until now reaches it */
    uint8_t *ram;        /* MEM1, GC_RAM_SIZE bytes, big-endian as on the console */
    uint8_t rc_exit;     /* GC_RC_EXIT_* */
    uint8_t pad_[7];
    uint64_t rc_native;  /* instructions run natively (packs built with -DGC_RC_COUNT) */
} gc_rc_head;

enum {
    GC_RC_EXIT_NORMAL = 0,      /* PC says where execution continues */
    GC_RC_EXIT_INTERP = 1,      /* the interpreter runs the instruction at PC, then dispatch goes on */
    GC_RC_EXIT_NOT_COVERED = 2, /* this function does not hold PC: nothing ran */
};

/* The core (gc_bus.c, gc_cpu.c). */
uint8_t gc_read8(gc_machine *m, uint32_t ea);
uint16_t gc_read16(gc_machine *m, uint32_t ea);
uint32_t gc_read32(gc_machine *m, uint32_t ea);
void gc_write8(gc_machine *m, uint32_t ea, uint8_t v);
void gc_write16(gc_machine *m, uint32_t ea, uint16_t v);
void gc_write32(gc_machine *m, uint32_t ea, uint32_t v);
uint32_t gc_mfspr(gc_machine *m, unsigned spr);
void gc_mtspr(gc_machine *m, unsigned spr, uint32_t v);  /* may make an event due now */
void gc_mtmsr(gc_machine *m, uint32_t v);                /* idem (MSR[EE] set with an interrupt pending) */
void gc_cache_op(gc_machine *m, uint32_t ea, unsigned kind); /* GC_CACHE_* */

enum { GC_CACHE_DCBZ = 0, GC_CACHE_ICBI = 1 };

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

static inline uint32_t rc_lbz(gc_machine *m, uint32_t ea)
{
    uint32_t pa = ea & 0x3FFFFFFFu;
    return pa < GC_RAM_SIZE ? RC_HEAD(m)->ram[pa] : gc_read8(m, ea);
}

static inline uint32_t rc_lhz(gc_machine *m, uint32_t ea)
{
    uint32_t pa = ea & 0x3FFFFFFFu;
    return pa <= GC_RAM_SIZE - 2 ? rc_be16(RC_HEAD(m)->ram + pa) : gc_read16(m, ea);
}

static inline uint32_t rc_lwz(gc_machine *m, uint32_t ea)
{
    uint32_t pa = ea & 0x3FFFFFFFu;
    return pa <= GC_RAM_SIZE - 4 ? rc_be32(RC_HEAD(m)->ram + pa) : gc_read32(m, ea);
}

static inline uint64_t rc_ld64(gc_machine *m, uint32_t ea)
{
    return (uint64_t)rc_lwz(m, ea) << 32 | rc_lwz(m, ea + 4);
}

static inline void rc_stb(gc_machine *m, uint32_t ea, uint32_t v)
{
    uint32_t pa = ea & 0x3FFFFFFFu;
    if (pa < GC_RAM_SIZE)
        RC_HEAD(m)->ram[pa] = (uint8_t)v;
    else
        gc_write8(m, ea, (uint8_t)v);
}

static inline void rc_sth(gc_machine *m, uint32_t ea, uint32_t v)
{
    uint32_t pa = ea & 0x3FFFFFFFu;
    if (pa <= GC_RAM_SIZE - 2)
        rc_put16(RC_HEAD(m)->ram + pa, (uint16_t)v);
    else
        gc_write16(m, ea, (uint16_t)v);
}

static inline void rc_stw(gc_machine *m, uint32_t ea, uint32_t v)
{
    uint32_t pa = ea & 0x3FFFFFFFu;
    if (pa <= GC_RAM_SIZE - 4)
        rc_put32(RC_HEAD(m)->ram + pa, v);
    else
        gc_write32(m, ea, v);
}

static inline void rc_st64(gc_machine *m, uint32_t ea, uint64_t v)
{
    rc_stw(m, ea, (uint32_t)(v >> 32));
    rc_stw(m, ea + 4, (uint32_t)v);
}

/* ---- the Gekko layer: paired singles and quantised loads / stores ---- */

/* Single-precision results fill both slots (Gekko); double-precision ones write ps0 only. */
static inline void gc_fset_single(gc_cpu *c, unsigned d, double v)
{
    double s = ppc_single(v);
    c->ps0[d] = s;
    c->ps1[d] = s;
}

static inline double gc_ps_round(double v)
{
    return ppc_single(v);
}

static inline int gc_gqr_scale(uint32_t field)
{
    return (field & 0x20) ? (int)field - 64 : (int)field;
}

static inline unsigned gc_quant_size(unsigned type)
{
    return type == 4 || type == 6 ? 1u : type == 5 || type == 7 ? 2u : 4u;
}

static inline double gc_dequant(gc_machine *m, uint32_t ea, unsigned type, int scale)
{
    double v;
    switch (type) {
    case 4: v = (double)(uint8_t)rc_lbz(m, ea); break;
    case 5: v = (double)(uint16_t)rc_lhz(m, ea); break;
    case 6: v = (double)(int8_t)rc_lbz(m, ea); break;
    case 7: v = (double)(int16_t)rc_lhz(m, ea); break;
    default: return ppc_lfs_bits(rc_lwz(m, ea));
    }
    return ppc_single(ldexp(v, -scale));
}

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

static inline void gc_quant(gc_machine *m, uint32_t ea, unsigned type, int scale, double v)
{
    if (type < 4) {
        rc_stw(m, ea, ppc_stfs_bits(v));
        return;
    }
    double x = ldexp(v, scale);
    switch (type) {
    case 4: rc_stb(m, ea, (uint32_t)gc_sat(x, 0.0, 255.0)); break;
    case 5: rc_sth(m, ea, (uint32_t)gc_sat(x, 0.0, 65535.0)); break;
    case 6: rc_stb(m, ea, (uint32_t)gc_sat(x, -128.0, 127.0)); break;
    default: rc_sth(m, ea, (uint32_t)gc_sat(x, -32768.0, 32767.0)); break;
    }
}

/* psq_l / psq_lx / psq_lu / psq_lux: GQR i's load half (type bits 16-18, scale 24-29); W = 1 loads one value. */
static inline void gc_psq_load(gc_machine *m, unsigned d, uint32_t ea, unsigned w, unsigned i)
{
    gc_cpu *c = &RC_HEAD(m)->cpu;
    uint32_t gqr = c->spr[PPC_SPR_GQR0 + (i & 7)];
    unsigned type = (gqr >> 16) & 7;
    int scale = gc_gqr_scale((gqr >> 24) & 63);
    double p0 = gc_dequant(m, ea, type, scale);
    double p1 = w ? 1.0 : gc_dequant(m, ea + gc_quant_size(type), type, scale);
    c->ps0[d] = p0;
    c->ps1[d] = p1;
}

/* psq_st family: GQR i's store half (type bits 0-2, scale 8-13). */
static inline void gc_psq_store(gc_machine *m, unsigned s, uint32_t ea, unsigned w, unsigned i)
{
    gc_cpu *c = &RC_HEAD(m)->cpu;
    uint32_t gqr = c->spr[PPC_SPR_GQR0 + (i & 7)];
    unsigned type = gqr & 7;
    int scale = gc_gqr_scale((gqr >> 8) & 63);
    gc_quant(m, ea, type, scale, c->ps0[s]);
    if (!w)
        gc_quant(m, ea + gc_quant_size(type), type, scale, c->ps1[s]);
}

/* ---- lifted-code vocabulary (tools/ppc/lift.py, gc_tools/lift.py) ---- */

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
#define RC_BLOCK(pc_, n)                                                                                               \
    do {                                                                                                               \
        if (h_->now >= h_->next_event) {                                                                               \
            c->pc = (pc_);                                                                                             \
            return;                                                                                                    \
        }                                                                                                              \
        h_->now += (n);                                                                                                \
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
#define RC_NOT_COVERED()                                                                                               \
    do {                                                                                                               \
        h_->rc_exit = GC_RC_EXIT_NOT_COVERED;                                                                          \
        return;                                                                                                        \
    } while (0)
/* After an instruction that can make an event due (mtmsr, mtspr, a write off MEM1): leave if it did. */
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
#define RC_LBZ(ea) rc_lbz(m_, (ea))
#define RC_LHZ(ea) rc_lhz(m_, (ea))
#define RC_LWZ(ea) rc_lwz(m_, (ea))
#define RC_LD64(ea) rc_ld64(m_, (ea))
#define RC_STB(ea, v) rc_stb(m_, (ea), (v))
#define RC_STH(ea, v) rc_sth(m_, (ea), (v))
#define RC_STW(ea, v) rc_stw(m_, (ea), (v))
#define RC_ST64(ea, v) rc_st64(m_, (ea), (v))
#define RC_MFSPR(n) gc_mfspr(m_, (n))
#define RC_MTSPR(n, v) gc_mtspr(m_, (n), (v))
#define RC_MTMSR(v) gc_mtmsr(m_, (v))
#define RC_DCBZ(ea) gc_cache_op(m_, (ea), GC_CACHE_DCBZ)
#define RC_ICBI(ea) gc_cache_op(m_, (ea), GC_CACHE_ICBI)
#define RC_PSQ_L(d, ea, w, i) gc_psq_load(m_, (d), (ea), (w), (i))
#define RC_PSQ_ST(s, ea, w, i) gc_psq_store(m_, (s), (ea), (w), (i))
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
/* Single-precision FP results (fadds..., lfs, frsp): both slots. */
#define RC_FSET_S(d, v) gc_fset_single(c, (d), (v))
#define RC_FSET_D(d, v) (c->ps0[d] = (v))
#define RC_PS(v) gc_ps_round(v)

#ifdef __cplusplus
}
#endif

#endif
