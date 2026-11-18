/* xenon_rt.h -- the ABI between lifted Xbox 360 code (x360_tools.recomp, on the shared PowerPC lifter tools/ppc and
 * the Xenon layer) and a host, XBA-230 spike.
 *
 * There is no Xbox 360 runtime yet (no kernel / XAM HLE, no threads, no Xenos): this header is what the spike's test
 * harness links lifted functions against, and what a real runtime would start from. Model:
 * - one hardware thread: xe_cpu holds 64-bit GPRs, doubles for the FPRs, 128 VMX128 registers, LR / CTR (64-bit),
 *   CR, XER bits, FPSCR, VSCR, a reservation flag;
 * - memory: one flat big-endian window [mem_base, mem_base + mem_size) of guest addresses (32-bit effective
 *   addresses, as titles use); an access outside it counts a fault and reads 0 / drops the write;
 * - control: the shape of tools/ppc/lift.py: blocks at resume points, calls / returns / indirect jumps set PC and
 *   return to the caller (a dispatcher); RC_INTERP stops with XE_RC_EXIT_INTERP (no interpreter in the spike:
 *   the harness reports it); no events, so RC_BLOCK only counts.
 * Byte-swapping is done per access (little-endian host). */
#ifndef XENON_RT_H
#define XENON_RT_H

#include <stdint.h>
#include <string.h>

#include "xenon_ops.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the Xbox 360 spike runtime assumes a little-endian host (it byte-swaps guest memory)"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xe_cpu {
    uint64_t gpr[32];
    double fpr[32];
    xe_v128 vr[128];
    uint64_t lr, ctr;
    uint32_t pc;
    uint32_t cr;
    uint32_t xer_so, xer_ov, xer_ca;
    uint32_t fpscr, vscr, msr;
    uint32_t reserve;
} xe_cpu;

typedef struct xe_machine {
    xe_cpu cpu;
    uint8_t *mem;
    uint32_t mem_base, mem_size;
    uint32_t faults;     /* accesses outside the window */
    uint8_t rc_exit;     /* XE_RC_EXIT_* */
    uint64_t rc_native;  /* instructions run natively */
#ifdef XE_RUNTIME
    xe_cpu *cur;         /* the running guest thread's context (runtime/x360_thread.c) */
    int64_t slice;       /* blocks left before the scheduler gets the host thread back */
    uint64_t tb_base;    /* time base offset (virtual time skipped while every thread waited) */
    void *rt;            /* the runtime (x360_machine) */
#endif
    /* XBA-283, after every older field (packs made before read none of these): the reservation of lwarx / ldarx
     * (xe_lwarx), per machine: the scheduler clears it at each switch (cooperative threads), each host thread has its
     * own machine (parallel threads) */
    uint32_t resv_addr;
    uint64_t resv_val;
} xe_machine;

enum {
    XE_RC_EXIT_NORMAL = 0,      /* PC says where execution continues */
    XE_RC_EXIT_INTERP = 1,      /* the instruction at PC was not lifted */
    XE_RC_EXIT_NOT_COVERED = 2, /* this function does not hold PC */
};

typedef void (*xe_rc_fn)(xe_machine *m);

typedef struct xe_rc_entry {
    uint32_t entry;
    xe_rc_fn fn;
} xe_rc_entry;

/* XBA-284: a big pack's resume table in parts (xe_rc_resume_parts, xe_rc_resume_part_count instead of xe_rc_resume),
 * each sorted, the parts in address order of their functions (a point may sort before the previous part's last) */
typedef struct xe_rc_part {
    const xe_rc_entry *entries;
    unsigned count;
} xe_rc_part;

/* ---- memory ---- */

#ifdef XE_RUNTIME
/* The runtime (consoles/x360/runtime, XBA-270): mem is the base of a 4 GB host reservation, every 32-bit guest
 * address maps to mem + ea (pages the guest has not allocated fault in the host: x360_mem.c reports them). */
static inline uint8_t *xe_ptr(xe_machine *m, uint32_t ea, uint32_t n)
{
    (void)n;
    return m->mem + ea;
}
#else
static inline uint8_t *xe_ptr(xe_machine *m, uint32_t ea, uint32_t n)
{
    uint32_t o = ea - m->mem_base;
    if (o > m->mem_size || n > m->mem_size - o) {
        m->faults++;
        return NULL;
    }
    return m->mem + o;
}
#endif

static inline uint32_t xe_lbz(xe_machine *m, uint32_t ea)
{
    uint8_t *p = xe_ptr(m, ea, 1);
    return p ? *p : 0;
}

static inline uint32_t xe_lhz(xe_machine *m, uint32_t ea)
{
    uint8_t *p = xe_ptr(m, ea, 2);
    uint16_t v = 0;
    if (p)
        memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}

static inline uint32_t xe_lwz(xe_machine *m, uint32_t ea)
{
    uint8_t *p = xe_ptr(m, ea, 4);
    uint32_t v = 0;
    if (p)
        memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

static inline uint64_t xe_ld(xe_machine *m, uint32_t ea)
{
    uint8_t *p = xe_ptr(m, ea, 8);
    uint64_t v = 0;
    if (p)
        memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

static inline void xe_stb(xe_machine *m, uint32_t ea, uint32_t v)
{
    uint8_t *p = xe_ptr(m, ea, 1);
    if (p)
        *p = (uint8_t)v;
}

static inline void xe_sth(xe_machine *m, uint32_t ea, uint32_t v)
{
    uint8_t *p = xe_ptr(m, ea, 2);
    uint16_t x = __builtin_bswap16((uint16_t)v);
    if (p)
        memcpy(p, &x, 2);
}

static inline void xe_stw(xe_machine *m, uint32_t ea, uint32_t v)
{
    uint8_t *p = xe_ptr(m, ea, 4);
    uint32_t x = __builtin_bswap32(v);
    if (p)
        memcpy(p, &x, 4);
}

static inline void xe_std(xe_machine *m, uint32_t ea, uint64_t v)
{
    uint8_t *p = xe_ptr(m, ea, 8);
    uint64_t x = __builtin_bswap64(v);
    if (p)
        memcpy(p, &x, 8);
}

static inline void xe_zero(xe_machine *m, uint32_t ea, uint32_t line)
{
    uint8_t *p = xe_ptr(m, ea & ~(line - 1), line);
    if (p)
        memset(p, 0, line);
}

/* ---- reservations (XBA-283) ----
 * lwarx / ldarx note the address and the value read; stwcx. / stdcx. store only when the reservation is still held
 * (c->reserve: cleared at a thread switch and by a successful store) and the memory still holds that value, as one
 * atomic compare-and-swap: with guest threads on host threads (x360_run --host-threads) a store by another thread in
 * between makes it fail, as the hardware's reservation does (an ABA change back to the same value is not seen: what
 * Xenia does too). CR0 = EQ on success, then SO. */

static inline uint32_t xe_lwarx(xe_machine *m, xe_cpu *c, uint32_t ea)
{
    uint8_t *p = xe_ptr(m, ea & ~3u, 4);
    uint32_t v = 0;
    if (p)
        v = __builtin_bswap32(__atomic_load_n((uint32_t *)p, __ATOMIC_ACQUIRE));
    m->resv_addr = ea & ~3u;
    m->resv_val = v;
    c->reserve = 1;
    return v;
}

static inline uint64_t xe_ldarx(xe_machine *m, xe_cpu *c, uint32_t ea)
{
    uint8_t *p = xe_ptr(m, ea & ~7u, 8);
    uint64_t v = 0;
    if (p)
        v = __builtin_bswap64(__atomic_load_n((uint64_t *)p, __ATOMIC_ACQUIRE));
    m->resv_addr = ea & ~7u;
    m->resv_val = v;
    c->reserve = 1;
    return v;
}

static inline void xe_stcx(xe_machine *m, xe_cpu *c, uint32_t ea, uint64_t v, unsigned n)
{
    int ok = 0;
    uint32_t a = ea & ~(n - 1);
    if (c->reserve && m->resv_addr == a) {
        uint8_t *p = xe_ptr(m, a, n);
        if (p && n == 4) {
            uint32_t e = __builtin_bswap32((uint32_t)m->resv_val), w = __builtin_bswap32((uint32_t)v);
            ok = __atomic_compare_exchange_n((uint32_t *)p, &e, w, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        } else if (p) {
            uint64_t e = __builtin_bswap64(m->resv_val), w = __builtin_bswap64(v);
            ok = __atomic_compare_exchange_n((uint64_t *)p, &e, w, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
    }
    c->reserve = 0;
    c->cr = ppc_cr_set(c->cr, 0, (ok ? PPC_CR_EQ : 0) | c->xer_so);
}

/* ---- vector loads / stores (16-byte blocks, big-endian) ---- */

static inline xe_v128 xv_load(xe_machine *m, uint32_t ea)
{
    uint8_t *p = xe_ptr(m, ea & ~15u, 16);
    uint8_t z[16] = {0};
    return xv_from_bytes(p ? p : z);
}

static inline void xv_store(xe_machine *m, uint32_t ea, xe_v128 v)
{
    uint8_t *p = xe_ptr(m, ea & ~15u, 16);
    if (p)
        xv_to_bytes(v, p);
}

/* lvlx: the bytes from ea to the end of its 16-byte block, left-aligned, zeros after. */
static inline xe_v128 xv_lvlx(xe_machine *m, uint32_t ea)
{
    uint8_t blk[16], out[16] = {0};
    uint32_t eb = ea & 15;
    xv_to_bytes(xv_load(m, ea), blk);
    memcpy(out, blk + eb, 16 - eb);
    return xv_from_bytes(out);
}

/* lvrx: the bytes from the block start up to ea, right-aligned, zeros before (all zeros when ea is aligned: no
 * access at all then, so a copy loop's tail may point past the end of a buffer). */
static inline xe_v128 xv_lvrx(xe_machine *m, uint32_t ea)
{
    uint8_t blk[16], out[16] = {0};
    uint32_t eb = ea & 15;
    if (eb) {
        xv_to_bytes(xv_load(m, ea), blk);
        memcpy(out + 16 - eb, blk, eb);
    }
    return xv_from_bytes(out);
}

static inline void xv_stvlx(xe_machine *m, uint32_t ea, xe_v128 v)
{
    uint8_t b[16];
    uint32_t eb = ea & 15;
    uint8_t *p = xe_ptr(m, ea, 16 - eb);
    xv_to_bytes(v, b);
    if (p)
        memcpy(p, b, 16 - eb);
}

static inline void xv_stvrx(xe_machine *m, uint32_t ea, xe_v128 v)
{
    uint8_t b[16];
    uint32_t eb = ea & 15;
    if (!eb)
        return;
    uint8_t *p = xe_ptr(m, ea & ~15u, eb);
    xv_to_bytes(v, b);
    if (p)
        memcpy(p, b + 16 - eb, eb);
}

/* lvebx / lvehx / lvewx: one element at its place in the register (the others are left as they were). */
static inline void xv_lve(xe_machine *m, xe_v128 *v, uint32_t ea, uint32_t size)
{
    uint8_t b[16];
    ea &= ~(size - 1);
    uint8_t *p = xe_ptr(m, ea, size);
    xv_to_bytes(*v, b);
    if (p)
        memcpy(b + (ea & 15), p, size);
    *v = xv_from_bytes(b);
}

static inline void xv_stve(xe_machine *m, uint32_t ea, xe_v128 v, uint32_t size)
{
    uint8_t b[16];
    ea &= ~(size - 1);
    uint8_t *p = xe_ptr(m, ea, size);
    xv_to_bytes(v, b);
    if (p)
        memcpy(p, b + (ea & 15), size);
}

/* ---- lifted-code vocabulary (tools/ppc/lift.py, x360_tools/xenon.py) ---- */

#define RC_FN void
#define RC_MACHINE xe_machine
#ifdef XE_RUNTIME
/* Runtime: the context is the running guest thread's; every block counts against the thread's time slice and
 * hands the host thread back to the scheduler at a resume point when it runs out (cooperative threads). While the
 * thread has MSR[EE] clear (mtmsrd: a short section with interrupts off, which the kernel never reschedules) it keeps
 * running, up to XE_EE_GRACE blocks past its slice. */
#define RC_PROLOGUE                                                                                                    \
    xe_cpu *c = m_->cur;                                                                                               \
    (void)c
#define XE_EE_GRACE 65536 /* blocks a thread with MSR[EE] clear may run past its slice before it is preempted anyway */
#define RC_BLOCK(pc_, n)                                                                                               \
    do {                                                                                                               \
        m_->rc_native += (n);                                                                                          \
        if (--m_->slice < 0 && ((c->msr & 0x8000u) || m_->slice < -XE_EE_GRACE)) {                                     \
            c->pc = (pc_);                                                                                             \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)
#else
#define RC_PROLOGUE                                                                                                    \
    xe_cpu *c = &m_->cpu;                                                                                              \
    (void)c
#define RC_BLOCK(pc_, n) (m_->rc_native += (n))
#endif
#define RC_EXIT(pc_)                                                                                                   \
    do {                                                                                                               \
        c->pc = (pc_);                                                                                                 \
        return;                                                                                                        \
    } while (0)
#define RC_INTERP(pc_)                                                                                                 \
    do {                                                                                                               \
        c->pc = (pc_);                                                                                                 \
        m_->rc_exit = XE_RC_EXIT_INTERP;                                                                               \
        return;                                                                                                        \
    } while (0)
#define RC_NOT_COVERED()                                                                                               \
    do {                                                                                                               \
        m_->rc_exit = XE_RC_EXIT_NOT_COVERED;                                                                          \
        return;                                                                                                        \
    } while (0)
#define RC_CHECK(next_) ((void)0)
#define RC_FPCHECK(pc_) ((void)0)
#define RC_IDLE(pc_) RC_EXIT(pc_)

#define R(n) c->gpr[n]
#define F0(n) c->fpr[n]
#define V(n) c->vr[n]
#define RC_LBZ(ea) xe_lbz(m_, (ea))
#define RC_LHZ(ea) xe_lhz(m_, (ea))
#define RC_LWZ(ea) xe_lwz(m_, (ea))
#define RC_LD64(ea) xe_ld(m_, (ea))
#define RC_STB(ea, v) xe_stb(m_, (ea), (v))
#define RC_STH(ea, v) xe_sth(m_, (ea), (v))
#define RC_STW(ea, v) xe_stw(m_, (ea), (v))
#define RC_ST64(ea, v) xe_std(m_, (ea), (v))
#define RC_SYNC() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define RC_LWARX(ea) xe_lwarx(m_, c, (ea))
#define RC_LDARX(ea) xe_ldarx(m_, c, (ea))
#define RC_STWCX(ea, v) xe_stcx(m_, c, (ea), (v), 4)
#define RC_STDCX(ea, v) xe_stcx(m_, c, (ea), (v), 8)
#define RC_DCBZ(ea) xe_zero(m_, (ea), 32)
#define RC_DCBZ128(ea) xe_zero(m_, (ea), 128)
#define RC_ICBI(ea) ((void)(ea))
#define RC_MFSPR(n) xe_mfspr(m_, (n))
#define RC_MTSPR(n, v) xe_mtspr(m_, (n), (v))
#define RC_MTMSR(v) (c->msr = (uint32_t)(v))
#define RC_XER() (c->xer_so << 31 | c->xer_ov << 30 | c->xer_ca << 29)
#define RC_SET_XER(v)                                                                                                  \
    do {                                                                                                               \
        uint32_t x_ = (uint32_t)(v);                                                                                   \
        c->xer_so = x_ >> 31;                                                                                          \
        c->xer_ov = (x_ >> 30) & 1;                                                                                    \
        c->xer_ca = (x_ >> 29) & 1;                                                                                    \
    } while (0)
/* Record forms: titles run in 32-bit mode (MSR[SF] = 0), CR0 compares the low word of the result with 0 (xenon_ops.h
 * xe_carry says more). */
#define RC_CR0(v) (c->cr = ppc_cr_set(c->cr, 0, ppc_cmps((int32_t)(uint32_t)(v), 0, c->xer_so)))
/* OE forms (XBA-284): OV is the overflow, SO sticks */
#define RC_OE(ov)                                                                                                      \
    do {                                                                                                               \
        c->xer_ov = (ov);                                                                                              \
        c->xer_so |= c->xer_ov;                                                                                        \
    } while (0)
#define RC_FSET_S(d, v) (c->fpr[d] = ppc_single(v))
#define RC_FSET_D(d, v) (c->fpr[d] = (v))

/* SPRs other than LR / CTR / XER: the time base reads a counter, the rest is kept in a small table. */
static inline uint64_t xe_mfspr(xe_machine *m, unsigned spr)
{
#ifdef XE_RUNTIME
    if (spr == 268 || spr == 269 || spr == 284 || spr == 285)
        return m->rc_native / 64 + m->tb_base; /* the time base: 50 MHz against a ~3.2 GHz guest */
#endif
    if (spr == 268 || spr == 269)
        return m->rc_native;
    return 0;
}

static inline void xe_mtspr(xe_machine *m, unsigned spr, uint64_t v)
{
    (void)m, (void)spr, (void)v;
}

#ifdef __cplusplus
}
#endif

#endif
