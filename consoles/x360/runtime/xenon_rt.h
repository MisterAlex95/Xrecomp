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
    /* after every older field: what xe_ptr adds to an address in the 0xE0000000 view (x360_mem.c: 0 where the host
     * maps 4 KB pages, 0x1000 where its pages are larger). Code built with XE_VIEW4K_DYNAMIC reads it. */
    uint32_t view4k_skew;
    /* WP-16, after every older field: what the direct calls of a pack (XE_RC_FEAT_DIRECT_CALLS, RC_CALL) read: the
     * run's instruction budget (rc_native at which the dispatcher stops, x360_run; ~0 where there is none) and how
     * many direct calls deep the running lifted code is (0 at the dispatcher) */
    uint64_t rc_stop;
    uint32_t rc_depth;
    /* and for the indirect ones (RC_CALL_IND, bcctrl): the runtime's PC -> lifted function table over
     * [rc_code_lo, rc_code_lo + rc_code_span) (x360_bind's fn_tab), and the import thunks' range, which the dispatcher
     * tests first (x360_thunk_at) */
    void (**rc_fn_tab)(struct xe_machine *m);
    uint32_t rc_code_lo, rc_code_span, rc_thunk_lo, rc_thunk_span;
    /* after every older field (XE_RC_FEAT_SYNC_FLAG): 1 when another host thread shares guest memory with the lifted
     * code (guest threads on host threads, XBA-283; the GPU thread's CPU worker, WP-22), so sync / lwsync / eieio
     * (RC_SYNC) are a real fence; 0 in the cooperative mode, where every guest thread runs on this host thread and
     * nothing else touches guest memory meanwhile: a compiler barrier keeps the order of the guest's accesses */
    uint8_t rc_fence;
} xe_machine;

/* XE_VIEW4K_DYNAMIC: 1, xe_ptr reads view4k_skew and the code runs on a host of any page size; 0, it does not (one
 * comparison and one addition less per access) and the code runs where the host's pages are 4 KB only. By default 0
 * on x86, whose pages are 4 KB (but for Android's x86 images, which may show 16 KB ones), and 1 elsewhere (arm64: 4,
 * 16 or 64 KB). The runtime's own build sets 1 everywhere (runtime/CMakeLists.txt: it reaches guest memory far less
 * often than a pack, and the tests use it); a pack says which it is in xe_rc_features (XE_RC_FEAT_VIEW4K), and
 * x360_bind refuses one that cannot follow the machine. */
#ifndef XE_VIEW4K_DYNAMIC
#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) && !defined(__ANDROID__)
#define XE_VIEW4K_DYNAMIC 0
#else
#define XE_VIEW4K_DYNAMIC 1
#endif
#endif
#define XE_RC_FEAT_VIEW4K 2u /* xe_rc_features: the pack's memory accesses go through view4k_skew */
/* what this build of a pack adds to them (XE_RC_FEAT_SYNC_FLAG, defined below: RC_SYNC) */
#define XE_RC_PACK_FEATURES ((XE_VIEW4K_DYNAMIC ? XE_RC_FEAT_VIEW4K : 0u) | XE_RC_FEAT_SYNC_FLAG)
/* WP-16: the pack's lifted functions call each other straight from C (RC_CALL) and read xe_machine's rc_stop and
 * rc_depth. A runtime made before has neither and ignores feature bits it does not know, so such a pack also names its
 * CRC xe_rc_image_crc2 instead of xe_rc_image_crc: an older runtime does not find it and refuses the pack ("not an
 * x360_tools.recomp pack"); this one reads either, and refuses a pack with a feature bit it does not know. */
#define XE_RC_FEAT_DIRECT_CALLS 4u
/* RC_SYNC reads xe_machine's rc_fence: a runtime made before has no such field (it refuses the unknown bit) */
#define XE_RC_FEAT_SYNC_FLAG 8u
#define XE_RC_FEATS_KNOWN (1u | XE_RC_FEAT_VIEW4K | XE_RC_FEAT_DIRECT_CALLS | XE_RC_FEAT_SYNC_FLAG) /* what this runtime can run */
/* the declaration recomp.py gives a function another one calls directly: hidden, so the call is a plain branch (no
 * PLT; nothing looks the functions up by name, the tables hold their addresses) */
#if defined(__GNUC__) && !defined(_WIN32)
#define XE_RC_LOCAL __attribute__((visibility("hidden")))
#else
#define XE_RC_LOCAL
#endif

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
 * address maps to mem + ea (pages the guest has not allocated fault in the host: x360_mem.c reports them). The one
 * exception is the 0xE0000000 view of physical memory on a host whose pages are larger than 4 KB: the view starts
 * 0x1000 into physical memory, which such a host cannot map, so its addresses sit view4k_skew higher (x360_mem.c). */
/* XE_GIDX: a guest address as an index the compiler knows nothing of (an empty asm, no instruction), so that it never
 * proves two accesses through two views of one physical byte apart (the lifted accesses below say more) */
#if defined(__GNUC__)
#define XE_GIDX(i) __asm__("" : "+r"(i))
#else
#define XE_GIDX(i) ((void)0)
#endif
static inline uint8_t *xe_ptr(xe_machine *m, uint32_t ea, uint32_t n)
{
    uint64_t i = ea;
    (void)n;
    XE_GIDX(i);
#if XE_VIEW4K_DYNAMIC
    return m->mem + i + (i >= 0xE0000000u ? m->view4k_skew : 0u);
#else
    return m->mem + i;
#endif
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

/* ---- the lifted code's GPR loads and stores (RC_LBZ .. RC_ST64, below) ----
 * In the runtime, xe_ptr never fails (mem + ea), so these leave out xe_lwz's NULL test: the compiler cannot prove
 * mem != NULL, and kept a test and a branch around every access. The base (and the 4 KB view's skew) come from
 * locals RC_PROLOGUE reads once per call (mb_, sk_): read through m_ at each access, they were loaded again after
 * every guest store, which may write any memory (m_ too). The index goes through an empty asm (XE_GIDX), so the
 * compiler knows nothing of its value: with one known base, two constant guest addresses would be two provably
 * different host addresses, while the physical views (0xA0000000, 0xC0000000, 0xE0000000) put the same byte at
 * several of them; it could then move a store past a load of the same byte through another view, or forward a
 * store over it. Opaque indices keep every access in order against every other, as the base read at each access
 * did. The asm is not volatile and touches no memory: no instruction, nothing else held back (two accesses of the
 * same index value may share it: they are the same address). Zero-extended to 64 bits before the asm, so x86-64 does
 * not extend it again after. ldr + rev (aarch64) / mov + bswap (x86-64) per access, the address [base, index].
 * With clang, RC_PROLOGUE also says that guest memory is a separate allocation from the context and the machine
 * (__builtin_assume_separate_storage): guest stores then no longer make it read guest registers, FPRs and m_'s
 * fields again from memory. xe_ptr (the VMX accesses, the interpreter, the runtime) takes an opaque index too. */
#if defined(XE_RUNTIME) && defined(__GNUC__)
#define XE_GA_HOT static inline __attribute__((always_inline))
#if XE_VIEW4K_DYNAMIC
XE_GA_HOT uint8_t *xe_gp(uint8_t *mb, uint32_t sk, uint32_t ea)
{
    uint64_t i = ea;
    XE_GIDX(i);
    return mb + i + (i >= 0xE0000000u ? sk : 0u);
}
#else
XE_GA_HOT uint8_t *xe_gp(uint8_t *mb, uint32_t sk, uint32_t ea)
{
    uint64_t i = ea;
    (void)sk;
    XE_GIDX(i);
    return mb + i;
}
#endif

XE_GA_HOT uint32_t xe_glbz(uint8_t *mb, uint32_t sk, uint32_t ea)
{
    return *xe_gp(mb, sk, ea);
}

XE_GA_HOT uint32_t xe_glhz(uint8_t *mb, uint32_t sk, uint32_t ea)
{
    uint16_t v;
    memcpy(&v, xe_gp(mb, sk, ea), 2);
    return __builtin_bswap16(v);
}

XE_GA_HOT uint32_t xe_glwz(uint8_t *mb, uint32_t sk, uint32_t ea)
{
    uint32_t v;
    memcpy(&v, xe_gp(mb, sk, ea), 4);
    return __builtin_bswap32(v);
}

XE_GA_HOT uint64_t xe_gld(uint8_t *mb, uint32_t sk, uint32_t ea)
{
    uint64_t v;
    memcpy(&v, xe_gp(mb, sk, ea), 8);
    return __builtin_bswap64(v);
}

XE_GA_HOT void xe_gstb(uint8_t *mb, uint32_t sk, uint32_t ea, uint32_t v)
{
    *xe_gp(mb, sk, ea) = (uint8_t)v;
}

XE_GA_HOT void xe_gsth(uint8_t *mb, uint32_t sk, uint32_t ea, uint32_t v)
{
    uint16_t x = __builtin_bswap16((uint16_t)v);
    memcpy(xe_gp(mb, sk, ea), &x, 2);
}

XE_GA_HOT void xe_gstw(uint8_t *mb, uint32_t sk, uint32_t ea, uint32_t v)
{
    uint32_t x = __builtin_bswap32(v);
    memcpy(xe_gp(mb, sk, ea), &x, 4);
}

XE_GA_HOT void xe_gstd(uint8_t *mb, uint32_t sk, uint32_t ea, uint64_t v)
{
    uint64_t x = __builtin_bswap64(v);
    memcpy(xe_gp(mb, sk, ea), &x, 8);
}
#define XE_GA_LOCALS 1
#else
#define XE_GA_LOCALS 0
#endif

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

XV_HOT xe_v128 xv_load(xe_machine *m, uint32_t ea)
{
    uint8_t *p = xe_ptr(m, ea & ~15u, 16);
    uint8_t z[16] = {0};
    return xv_from_bytes(p ? p : z);
}

XV_HOT void xv_store(xe_machine *m, uint32_t ea, xe_v128 v)
{
    uint8_t *p = xe_ptr(m, ea & ~15u, 16);
    if (p)
        xv_to_bytes(v, p);
}

/* lvlx: the bytes from ea to the end of its 16-byte block, left-aligned, zeros after. */
XV_HOT xe_v128 xv_lvlx(xe_machine *m, uint32_t ea)
{
#if XV_U128
    return xv_from_u128(xv_to_u128(xv_load(m, ea)) << (8 * (ea & 15)));
#endif
    uint8_t blk[16], out[16] = {0};
    uint32_t eb = ea & 15;
    xv_to_bytes(xv_load(m, ea), blk);
    memcpy(out, blk + eb, 16 - eb);
    return xv_from_bytes(out);
}

/* lvrx: the bytes from the block start up to ea, right-aligned, zeros before (all zeros when ea is aligned: no
 * access at all then, so a copy loop's tail may point past the end of a buffer). */
XV_HOT xe_v128 xv_lvrx(xe_machine *m, uint32_t ea)
{
#if XV_U128
    if (!(ea & 15))
        return xv_splat32(0);
    return xv_from_u128(xv_to_u128(xv_load(m, ea)) >> (8 * (16 - (ea & 15))));
#endif
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
#if XV_LE
    if (p) /* bytes 0 .. 15 - eb of v from ea on (no variable-length memcpy: a libc call) */
        for (uint32_t i = 0; i < 16 - eb; i++)
            p[i] = v.u8[i ^ 3];
    return;
#endif
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
#if XV_LE
    if (p) /* bytes 16 - eb .. 15 of v up to ea */
        for (uint32_t i = 0; i < eb; i++)
            p[i] = v.u8[(16 - eb + i) ^ 3];
    return;
#endif
    xv_to_bytes(v, b);
    if (p)
        memcpy(p, b + 16 - eb, eb);
}

/* lvebx / lvehx / lvewx: one element at its place in the register (the others are left as they were). */
XV_HOT void xv_lve(xe_machine *m, xe_v128 *v, uint32_t ea, uint32_t size)
{
    uint8_t b[16];
    ea &= ~(size - 1);
    uint8_t *p = xe_ptr(m, ea, size);
#if XV_LE
    /* the element in place, in host order: byte i is u8[i ^ 3], halfword i u16[i ^ 1], word i u32[i] */
    if (!p)
        return;
    if (size == 4) {
        uint32_t w;
        memcpy(&w, p, 4);
        v->u32[(ea & 15) >> 2] = __builtin_bswap32(w);
    } else if (size == 2) {
        uint16_t h;
        memcpy(&h, p, 2);
        v->u16[((ea & 15) >> 1) ^ 1] = __builtin_bswap16(h);
    } else {
        v->u8[(ea & 15) ^ 3] = *p;
    }
    return;
#endif
    xv_to_bytes(*v, b);
    if (p)
        memcpy(b + (ea & 15), p, size);
    *v = xv_from_bytes(b);
}

XV_HOT void xv_stve(xe_machine *m, uint32_t ea, xe_v128 v, uint32_t size)
{
    uint8_t b[16];
    ea &= ~(size - 1);
    uint8_t *p = xe_ptr(m, ea, size);
#if XV_LE
    if (!p)
        return;
    if (size == 4) {
        uint32_t w = __builtin_bswap32(v.u32[(ea & 15) >> 2]);
        memcpy(p, &w, 4);
    } else if (size == 2) {
        uint16_t h = __builtin_bswap16(v.u16[((ea & 15) >> 1) ^ 1]);
        memcpy(p, &h, 2);
    } else {
        *p = v.u8[(ea & 15) ^ 3];
    }
    return;
#endif
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
#if XE_GA_LOCALS
/* Guest memory (the 4 GB reservation mb_ points into) holds neither the context nor the machine (host heap or
 * stack): clang may keep guest registers and m_'s fields in host registers across guest stores. */
#if defined(__has_builtin)
#if __has_builtin(__builtin_assume_separate_storage)
#define XE_GA_SEPARATE()                                                                                               \
    __builtin_assume_separate_storage(mb_, c);                                                                         \
    __builtin_assume_separate_storage(mb_, m_)
#endif
#endif
#ifndef XE_GA_SEPARATE
#define XE_GA_SEPARATE() ((void)0)
#endif
#define RC_PROLOGUE                                                                                                    \
    xe_cpu *c = m_->cur;                                                                                               \
    uint8_t *const mb_ = m_->mem;                                                                                      \
    const uint32_t sk_ = XE_VIEW4K_DYNAMIC ? m_->view4k_skew : 0u;                                                     \
    XE_GA_SEPARATE();                                                                                                  \
    (void)c, (void)mb_, (void)sk_
#else
#define RC_PROLOGUE                                                                                                    \
    xe_cpu *c = m_->cur;                                                                                               \
    (void)c
#endif
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
/* `or 31,31,31` / `or 1,1,1` (x360_tools/xenon.py): the SMT priority hints of a spin-wait loop (MSVC's waits for the
 * GPU, spin locks). The hardware gives the spinning thread's cycles to the core's other thread; the cooperative
 * scheduler would spend a whole time slice of host time on the loop instead. So the first hint of a turn ends the
 * thread's slice (at the next block: the other threads and the devices get their turn, as fair as without the hint)
 * and charges XE_SPIN_COST instructions of virtual time, once per turn whatever the number of hints in the loop: 32000
 * instructions are 10 us of the virtual clock (x360_thread.c: one 100 ns tick per 320), so the event the loop polls
 * for (the next vertical blank, another thread's flag) comes in little host time and is overshot by at most that.
 * Adventure Time, 30 s of game headless: 55 s of host without the hint, 24 s with it, the same 1388 swaps (a charge
 * per hint without ending the slice: 20 s but 1.2 % fewer swaps, the other threads lose time to the spinner; a turn
 * of 1 us: 55 s, the scheduler's own work then dominates). The charge counts in rc_native ("native" in the stats);
 * x360_interp.c does the same for interpreted code. A thread past its slice (MSR[EE] clear, XE_EE_GRACE) pays nothing
 * more. Code built without XE_RUNTIME (the tests' harness) has no slice: each hint charges. */
#ifndef XE_SPIN_COST
#define XE_SPIN_COST 32000
#endif
#ifndef XE_SPIN_HINT
#ifdef XE_RUNTIME
#define XE_SPIN_HINT()                                                                                                 \
    do {                                                                                                               \
        if (m_->slice > 0) {                                                                                           \
            m_->rc_native += XE_SPIN_COST;                                                                             \
            m_->slice = 0;                                                                                             \
        }                                                                                                              \
    } while (0)
#else
#define XE_SPIN_HINT() (m_->rc_native += XE_SPIN_COST)
#endif
#endif
/* WP-16: `bl T` to a function of the pack (x360_tools/lift360.py; the lifter has set LR to ret_): call T's function
 * fn_ from C instead of leaving to the dispatcher, then go on at lbl_ (ret_'s label in this function) when the callee
 * came back there, exactly where the dispatcher would have run this function from ret_ next (lift360.py only emits it
 * where the resume table maps ret_ to this function): PC == ret_, no RC_INTERP / not-covered exit, the slice not run
 * out and the budget not reached (run_thread's checks between two lifted functions; the HLE, SEH and APC return
 * addresses are never ret_). Anything else (a preemption at a block, an exit to the interpreter, a jump elsewhere: a
 * thunk, a tail call, an exception handler) returns up the chain to the dispatcher with PC as the callee left it, which
 * resumes there through the resume table as before. The call itself is made only where run_thread would make it too:
 * the slice not run out (a thread past it with MSR[EE] clear goes through the dispatcher), the budget not reached, and
 * fewer than XE_CALL_DEPTH direct calls on the host stack (deeper ones, recursion, leave to the dispatcher). The
 * guest's state, instruction count and slices are the same either way: only the host's route differs. */
#ifndef XE_CALL_DEPTH
#define XE_CALL_DEPTH 64
#endif
#ifdef XE_RUNTIME
#define RC_CALL(fn_, t_, ret_, lbl_)                                                                                   \
    do {                                                                                                               \
        if (m_->rc_depth < XE_CALL_DEPTH && m_->slice >= 0 && m_->rc_native < m_->rc_stop) {                           \
            c->pc = (t_);                                                                                              \
            m_->rc_depth++;                                                                                            \
            fn_(m_);                                                                                                   \
            m_->rc_depth--;                                                                                            \
            if (c->pc == (ret_) && !m_->rc_exit && m_->slice >= 0 && m_->rc_native < m_->rc_stop)                      \
                goto lbl_;                                                                                             \
            return;                                                                                                    \
        }                                                                                                              \
        RC_EXIT(t_);                                                                                                   \
    } while (0)
/* `bctrl` (a call through a pointer: virtual functions, callbacks): the same, the function found as run_thread finds
 * it (fn_tab at the target, which must be in the code range, 4-byte aligned and not an import thunk; the dispatcher's
 * special return addresses are outside the code range); a target the table does not hold leaves to the dispatcher
 * (thunk, interpreter). The lifter has set LR to ret_. */
#define RC_CALL_IND(t_, ret_, lbl_)                                                                                    \
    do {                                                                                                               \
        uint32_t it_ = (t_);                                                                                           \
        void (*if_)(xe_machine *);                                                                                     \
        if (it_ - m_->rc_code_lo < m_->rc_code_span && !(it_ & 3u) && it_ - m_->rc_thunk_lo >= m_->rc_thunk_span &&    \
            (if_ = m_->rc_fn_tab[(it_ - m_->rc_code_lo) >> 2]) != 0 && m_->rc_depth < XE_CALL_DEPTH &&                 \
            m_->slice >= 0 && m_->rc_native < m_->rc_stop) {                                                           \
            c->pc = it_;                                                                                               \
            m_->rc_depth++;                                                                                            \
            if_(m_);                                                                                                   \
            m_->rc_depth--;                                                                                            \
            if (c->pc == (ret_) && !m_->rc_exit && m_->slice >= 0 && m_->rc_native < m_->rc_stop)                      \
                goto lbl_;                                                                                             \
            return;                                                                                                    \
        }                                                                                                              \
        RC_EXIT(it_);                                                                                                  \
    } while (0)
#else
#define RC_CALL(fn_, t_, ret_, lbl_) RC_EXIT(t_) /* the tests' harness: no slice, its own dispatcher */
#define RC_CALL_IND(t_, ret_, lbl_) RC_EXIT(t_)
#endif
#define RC_CHECK(next_) ((void)0)
#define RC_FPCHECK(pc_) ((void)0)
#define RC_IDLE(pc_) RC_EXIT(pc_)

#define R(n) c->gpr[n]
#define F0(n) c->fpr[n]
#define V(n) c->vr[n]
#if XE_GA_LOCALS
#define RC_LBZ(ea) xe_glbz(mb_, sk_, (ea))
#define RC_LHZ(ea) xe_glhz(mb_, sk_, (ea))
#define RC_LWZ(ea) xe_glwz(mb_, sk_, (ea))
#define RC_LD64(ea) xe_gld(mb_, sk_, (ea))
#define RC_STB(ea, v) xe_gstb(mb_, sk_, (ea), (v))
#define RC_STH(ea, v) xe_gsth(mb_, sk_, (ea), (v))
#define RC_STW(ea, v) xe_gstw(mb_, sk_, (ea), (v))
#define RC_ST64(ea, v) xe_gstd(mb_, sk_, (ea), (v))
#else
#define RC_LBZ(ea) xe_lbz(m_, (ea))
#define RC_LHZ(ea) xe_lhz(m_, (ea))
#define RC_LWZ(ea) xe_lwz(m_, (ea))
#define RC_LD64(ea) xe_ld(m_, (ea))
#define RC_STB(ea, v) xe_stb(m_, (ea), (v))
#define RC_STH(ea, v) xe_sth(m_, (ea), (v))
#define RC_STW(ea, v) xe_stw(m_, (ea), (v))
#define RC_ST64(ea, v) xe_std(m_, (ea), (v))
#endif
/* sync / lwsync / eieio: a host fence only when another host thread shares guest memory (xe_machine.rc_fence) */
#define RC_SYNC()                                                                                                     \
    do {                                                                                                               \
        if (__builtin_expect(m_->rc_fence, 0))                                                                         \
            __atomic_thread_fence(__ATOMIC_SEQ_CST);                                                                   \
        else                                                                                                           \
            __atomic_signal_fence(__ATOMIC_SEQ_CST);                                                                   \
    } while (0)
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
