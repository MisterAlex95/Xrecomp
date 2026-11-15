/*
 * mmio_decode_a64.h -- the AArch64 twin of mmio_decode.h, for trapped MMIO on
 * POSIX hosts (macOS arm64, Linux arm64).
 *
 * The device page is PROT_NONE; the access arrives as SIGBUS (Darwin) or
 * SIGSEGV (Linux) with the faulting data address in si_addr and the thread
 * state in the ucontext. Unlike x86-64, the decoder never has to work out the
 * address: si_addr is exact for a data abort, so every addressing mode is
 * served by the same code. What it decodes from the instruction word is the
 * direction, the width, the destination/source register, sign extension and
 * any base-register writeback; the instruction is always 4 bytes.
 *
 * Covered: LDR/STR, LDRB/STRB, LDRH/STRH, LDRSB/LDRSH/LDRSW in the unsigned
 * immediate, unscaled (LDUR/STUR), pre/post-index and register-offset forms;
 * the SIMD&FP LDR/STR of B/H/S/D/Q; LDP/STP/LDPSW (int and S/D). Exclusive,
 * acquire/release, atomic and unprivileged forms return 0 -- stepping an
 * instruction that was not understood corrupts the guest silently, which is
 * worse than a fault naming the opcode.
 *
 * Clang emits exactly these for a `volatile` MEM32()/MEM16()/MEM8() in lifted
 * code (volatile accesses are never merged into pairs, so LDP/STP is a
 * backstop, not the common case).
 */
#ifndef MMIO_DECODE_A64_H
#define MMIO_DECODE_A64_H

#include <stdint.h>

#if defined(__aarch64__) && !defined(_WIN32)

#if defined(__APPLE__)
#include <sys/ucontext.h>
#if defined(__DARWIN_OPAQUE_ARM_THREAD_STATE64) && __DARWIN_OPAQUE_ARM_THREAD_STATE64
/* arm64e: pc/lr/sp/fp are ptrauth-signed opaque fields; use the
 * __darwin_arm_thread_state64_{get,set}_* accessors there. This runtime is
 * built arm64 (unsigned), so the plain fields are what the kernel reads back. */
#error "mmio_decode_a64.h: arm64e opaque thread state not handled"
#endif
#define A64_CTX_PC(uc)   ((uc)->uc_mcontext->__ss.__pc)
#define A64_CTX_X(uc, n) ((uc)->uc_mcontext->__ss.__x[(n)])
#define A64_CTX_FP(uc)   ((uc)->uc_mcontext->__ss.__fp)
#define A64_CTX_LR(uc)   ((uc)->uc_mcontext->__ss.__lr)
#define A64_CTX_SP(uc)   ((uc)->uc_mcontext->__ss.__sp)
#define A64_CTX_HAS_V 1
#elif defined(__linux__)
#include <ucontext.h>
#include <asm/sigcontext.h>      /* struct _aarch64_ctx, fpsimd_context */
#define A64_CTX_PC(uc)   ((uc)->uc_mcontext.pc)
#define A64_CTX_X(uc, n) ((uc)->uc_mcontext.regs[(n)])
#define A64_CTX_FP(uc)   ((uc)->uc_mcontext.regs[29])
#define A64_CTX_LR(uc)   ((uc)->uc_mcontext.regs[30])
#define A64_CTX_SP(uc)   ((uc)->uc_mcontext.sp)
#define A64_CTX_HAS_V 1
/* The FP/SIMD file is in uc_mcontext.__reserved: a chain of _aarch64_ctx
 * headers (magic, size) ended by a zero magic. An extra_context record there
 * points at more records past the 4 KB area (a large SVE vector length), with
 * their own zero terminator. ESR, ZA, ZT, TPIDR2, FPMR... records are
 * skipped by size. The two that matter here:
 *  - fpsimd_context, always present: V0..V31, FPSR, FPCR;
 *  - sve_context, once the thread has run SVE code since its last syscall
 *    (qemu-user with -cpu max: always). Bare (16 bytes: vl, flags) when no
 *    register data is live; otherwise Z0..Z31 of vl bytes each from +16,
 *    then P0..P15 and FFR. SVE_SIG_FLAG_SM: the thread is in streaming mode
 *    and the data is the streaming Z file, same layout at the streaming vl,
 *    so it is handled the same way.
 * On delivery V0..V31 are the same in both (Documentation/arch/arm64/sve.rst,
 * « Signal handling »: "the content of V0..V31 is duplicated"), so a store
 * reads the FPSIMD copy. On sigreturn, the kernel restores bits 127:0 of Zn
 * from fpsimd_context and the rest from sve_context; qemu-user restores the
 * whole of Zn from sve_context, so a write to vregs[] alone is lost there.
 * A load therefore writes both: vregs[n], and Zn as an architectural write
 * to Vn leaves it -- the value in 127:0, zeroes above (which a real kernel
 * would otherwise restore from the stale record). Header-independent: the
 * magic and the layout are spelled out here, as bionic, glibc and musl ship
 * different <asm/sigcontext.h> vintages. */
#define MMIO_A64_SVE_MAGIC   0x53564501u
#define MMIO_A64_EXTRA_MAGIC 0x45585401u
#define MMIO_A64_CTX_HDR     16u   /* struct sve_context without register data */
#define MMIO_A64_SVE_VL      8u    /* struct sve_context: u16 vl at +8 */

/* The record with this magic, extra_context followed, or NULL. */
static inline struct _aarch64_ctx *mmio_a64_linux_record(ucontext_t *uc, uint32_t magic)
{
    unsigned char *p = (unsigned char *)uc->uc_mcontext.__reserved;
    unsigned char *end = p + sizeof(uc->uc_mcontext.__reserved);
    unsigned char *extra = 0;
    uint32_t extra_size = 0;
    int in_extra = 0;

    while (p + sizeof(struct _aarch64_ctx) <= end) {
        struct _aarch64_ctx *h = (struct _aarch64_ctx *)p;
        if (h->magic == 0) {
            if (!extra || in_extra)
                break;
            p = extra; end = extra + extra_size; in_extra = 1;
            continue;
        }
        if (h->size < sizeof(*h) || h->size > (uintptr_t)(end - p))
            break;
        if (h->magic == magic)
            return h;
        if (h->magic == MMIO_A64_EXTRA_MAGIC && !in_extra && h->size >= 24) {
            uint64_t datap;          /* struct extra_context: u64 datap, u32 size */
            __builtin_memcpy(&datap, p + 8, 8);
            __builtin_memcpy(&extra_size, p + 16, 4);
            extra = (unsigned char *)(uintptr_t)datap;
        }
        p += h->size;
    }
    return 0;
}
#endif

#if A64_CTX_HAS_V
/* The V registers of a frame: where a store reads them and where a load
 * writes them (plus the live SVE Z registers on Linux, see above). */
typedef struct {
    __uint128_t *v;
    unsigned char *z;     /* Z0 in the sve_context, or NULL */
    uint32_t vl;          /* bytes per Z register */
} mmio_a64_vfile;

static inline int mmio_a64_vfile_get(ucontext_t *uc, mmio_a64_vfile *f)
{
    f->z = 0;
    f->vl = 0;
#if defined(__APPLE__)
    /* Darwin's mcontext has the 128-bit NEON file only: no SVE, and SME
     * state, where the CPU has it, is not part of the signal context. */
    f->v = (__uint128_t *)uc->uc_mcontext->__ns.__v;
    return 1;
#else
    struct _aarch64_ctx *h = mmio_a64_linux_record(uc, FPSIMD_MAGIC);
    if (!h || h->size < sizeof(struct fpsimd_context))
        return 0;                                /* no FP/SIMD record to edit */
    f->v = ((struct fpsimd_context *)h)->vregs;
    h = mmio_a64_linux_record(uc, MMIO_A64_SVE_MAGIC);
    if (h && h->size > MMIO_A64_CTX_HDR) {
        uint16_t vl;
        __builtin_memcpy(&vl, (unsigned char *)h + MMIO_A64_SVE_VL, 2);
        if (vl && vl % 16 == 0 && MMIO_A64_CTX_HDR + 32u * vl <= h->size) {
            f->z = (unsigned char *)h + MMIO_A64_CTX_HDR;
            f->vl = vl;
        }
    }
    return 1;
#endif
}

/* Vn = val, as a load does it: Zn (when live) zeroed above bit 127. */
static inline void mmio_a64_vset(mmio_a64_vfile *f, int n, __uint128_t val)
{
    f->v[n] = val;
    if (f->z) {             /* byte i of Zn at offset i, whatever the endianness */
        unsigned char *z = f->z + (uint32_t)n * f->vl;
        uint32_t i;
        for (i = 0; i < 16; i++)
            z[i] = (unsigned char)(val >> (8 * i));
        for (; i < f->vl; i++)
            z[i] = 0;
    }
}
#endif

typedef uint64_t (*mmio_a64_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_a64_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

/* x0..x28, then fp (x29), lr (x30); 31 is SP when used as a base. */
static inline uint64_t *mmio_a64_xreg(ucontext_t *uc, int r)
{
    if (r < 29) return (uint64_t *)&A64_CTX_X(uc, r);
    if (r == 29) return (uint64_t *)&A64_CTX_FP(uc);
    if (r == 30) return (uint64_t *)&A64_CTX_LR(uc);
    return (uint64_t *)&A64_CTX_SP(uc);
}

/* 1 if the instruction at pc was serviced against (rd, wr) and pc advanced.
 * off is the device offset of the faulting data address (from si_addr). */
static inline int mmio_emulate_a64(ucontext_t *uc, uint32_t off, void *dev,
                                   mmio_a64_read_fn rd, mmio_a64_write_fn wr)
{
    uint32_t insn = *(const uint32_t *)(uintptr_t)A64_CTX_PC(uc);
    uint32_t op29_27 = (insn >> 27) & 7;
    int V = (insn >> 26) & 1;
#if A64_CTX_HAS_V
    mmio_a64_vfile vf = { 0, 0, 0 };
#endif

    if (!rd || !wr)
        return 0;
#if A64_CTX_HAS_V
    if (V && !mmio_a64_vfile_get(uc, &vf))       /* no FP/SIMD record to edit */
        return 0;
#endif

    if (op29_27 == 7) {                          /* load/store register */
        int size = (insn >> 30) & 3;
        int opc  = (insn >> 22) & 3;
        int rt   = insn & 31, rn = (insn >> 5) & 31;
        int form = (insn >> 24) & 3;             /* 01 unsigned imm, 00 other */
        int bytes, is_load, sext64 = 0, sext32 = 0, wb = 0;
        int64_t wb_imm = 0;
        uint64_t v;

        if (form == 0) {
            int bit21 = (insn >> 21) & 1, sub = (insn >> 10) & 3;
            if (bit21 == 0) {
                /* 00 unscaled, 01 post-index, 10 unprivileged, 11 pre-index */
                if (sub == 2) return 0;
                if (sub == 1 || sub == 3) {
                    wb = 1;
                    wb_imm = (int64_t)(int32_t)(((insn >> 12) & 0x1FFu) << 23) >> 23;
                }
            } else if (sub != 2) {
                return 0;                        /* only register offset */
            }
        } else if (form != 1) {
            return 0;
        }

        if (V) {
#if A64_CTX_HAS_V
            if (opc & 2) { if (size != 0) return 0; bytes = 16; }
            else bytes = 1 << size;
            is_load = opc & 1;
            if (is_load) {
                if (bytes == 16) {
                    __uint128_t lo = rd(dev, off, 8), hi = rd(dev, off + 8, 8);
                    mmio_a64_vset(&vf, rt, lo | (hi << 64));
                } else {
                    mmio_a64_vset(&vf, rt, rd(dev, off, bytes));
                }
            } else {
                __uint128_t q = vf.v[rt];
                if (bytes == 16) {
                    wr(dev, off, (uint64_t)q, 8);
                    wr(dev, off + 8, (uint64_t)(q >> 64), 8);
                } else {
                    wr(dev, off, (uint64_t)q, bytes);
                }
            }
#else
            return 0;
#endif
        } else {
            bytes = 1 << size;
            switch (opc) {
            case 0: is_load = 0; break;
            case 1: is_load = 1; break;
            case 2:                              /* LDRS* to 64-bit        */
                if (size == 3) return 0;         /* PRFM                    */
                is_load = 1; sext64 = 1; break;
            default:                             /* LDRS* to 32-bit (B, H) */
                if (size >= 2) return 0;
                is_load = 1; sext32 = 1; break;
            }
            if (is_load) {
                v = rd(dev, off, bytes);
                if (sext64) {
                    int sh = 64 - bytes * 8;
                    v = (uint64_t)(((int64_t)(v << sh)) >> sh);
                } else if (sext32) {
                    int sh = 32 - bytes * 8;
                    v = (uint32_t)(((int32_t)((uint32_t)v << sh)) >> sh);
                } else if (size != 3) {
                    v &= (1ULL << (bytes * 8)) - 1;   /* W write zeroes 63:32 */
                }
                if (rt != 31) *mmio_a64_xreg(uc, rt) = v;
            } else {
                v = rt == 31 ? 0 : *mmio_a64_xreg(uc, rt);
                wr(dev, off, v, bytes);
            }
        }
        if (wb) *mmio_a64_xreg(uc, rn) += (uint64_t)wb_imm;
        A64_CTX_PC(uc) += 4;
        return 1;
    }

    /* LDP/STP: bits 25:23 are 001 post-index, 010 signed offset, 011
     * pre-index; 000 is LDNP/STNP and 1xx is not a pair. */
    if (op29_27 == 5 && ((insn >> 23) & 7) >= 1 && ((insn >> 23) & 7) <= 3) {
        int opc = (insn >> 30) & 3, L = (insn >> 22) & 1;
        int mode = (insn >> 23) & 3;             /* 1 post, 2 offset, 3 pre */
        int rt = insn & 31, rt2 = (insn >> 10) & 31, rn = (insn >> 5) & 31;
        int64_t imm7 = (int64_t)(int32_t)(((insn >> 15) & 0x7Fu) << 25) >> 25;
        int bytes, sext = 0;

        if (V) {
#if A64_CTX_HAS_V
            bytes = 4 << opc;
            if (bytes > 8) return 0;        /* Q pairs; opc 3 is unallocated */
#else
            return 0;
#endif
        } else if (opc == 0) bytes = 4;
        else if (opc == 1) {                /* LDPSW; the store form is STGP */
            if (!L) return 0;
            bytes = 4; sext = 1;
        }
        else if (opc == 2) bytes = 8;
        else return 0;

        if (L) {
            uint64_t a = rd(dev, off, bytes), b = rd(dev, off + bytes, bytes);
            if (sext) {
                a = (uint64_t)(int64_t)(int32_t)a;
                b = (uint64_t)(int64_t)(int32_t)b;
            }
#if A64_CTX_HAS_V
            if (V) { mmio_a64_vset(&vf, rt, a); mmio_a64_vset(&vf, rt2, b); } else
#endif
            {
                if (rt != 31) *mmio_a64_xreg(uc, rt) = a;
                if (rt2 != 31) *mmio_a64_xreg(uc, rt2) = b;
            }
        } else {
            uint64_t a, b;
#if A64_CTX_HAS_V
            if (V) { a = (uint64_t)vf.v[rt]; b = (uint64_t)vf.v[rt2]; } else
#endif
            {
                a = rt == 31 ? 0 : *mmio_a64_xreg(uc, rt);
                b = rt2 == 31 ? 0 : *mmio_a64_xreg(uc, rt2);
            }
            wr(dev, off, a, bytes);
            wr(dev, off + bytes, b, bytes);
        }
        if (mode == 1 || mode == 3)
            *mmio_a64_xreg(uc, rn) += (uint64_t)(imm7 * bytes);
        A64_CTX_PC(uc) += 4;
        return 1;
    }
    return 0;
}

#endif /* __aarch64__ && !_WIN32 */
#endif /* MMIO_DECODE_A64_H */
