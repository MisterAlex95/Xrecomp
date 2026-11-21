/* n64_rsp.c -- the Reality Signal Processor, low level (N-5, XBA-173): the scalar unit and the vector unit (VU)
 * interpreted instruction by instruction, so every microcode (audio and graphics, libultra's and libdragon's) runs as
 * on the hardware. Implements the RSP part of n64_devices.h. Our own code, written from the n64brew wiki's RSP pages
 * and the behaviour of ares (ISC) and cen64 (BSD-3) as documented by their authors; nothing copied.
 *
 * Scalar unit: a 32-bit MIPS subset (no multiply / divide, no exceptions: ADD / ADDI / SUB never trap, unknown opcodes
 * are no-ops), 12-bit PC in IMEM, branch delay slots (pc / npc pair, so a branch in a delay slot behaves as on a MIPS
 * pipeline), loads and stores anywhere in DMEM (unaligned accesses allowed, addresses wrap at 4 KB), BREAK = halt +
 * broke (+ the SP interrupt when SP_STATUS.intr_break is set). COP0: 0..7 the SP registers, 8..15 the DPC registers
 * of the attached RDP (reads 0 without one).
 *
 * Vector unit: 32 registers of 8 x 16-bit lanes (element 0 = the most significant halfword, bytes 0-1 in the
 * big-endian byte view the loads and stores use), a 48-bit accumulator per lane kept as bits 47..16 (int32) and
 * 15..0 (uint16), so every lane operation is 32-bit,
 * VCO (carry / not-equal), VCC (compare / clip), VCE, and the divide unit (DIVIN / DIVOUT / DIVDP) with the
 * reciprocal and inverse square root ROMs computed at create time from their defining formulas. Computational ops are
 * branchless 8-lane loops over 16 / 32-bit values the compiler vectorises; on arm64 the hot ones (multiply-accumulate
 * family, add / sub, compares, clips, merge, logic, divide moves), the element selectors and the aligned quad / double
 * loads and stores are NEON intrinsics instead (vu_op_neon), on x86-64 the same set in SSE2 (vu_op_sse2), both
 * bit-identical to the loops.
 *
 * Dispatch: every IMEM word runs from a predecoded entry (rsp_dec, rsp_loop) that checks itself against IMEM at each
 * fetch, so IMEM writers never invalidate anything; each handler ends in its own fetch and indirect jump (GCC and
 * clang); the vector ops are one function per opcode and selector class. Loops that only wait (a status poll, a
 * delay countdown) have their whole iterations counted at once, the state left exactly as running them leaves it
 * (rsp_idle, RK_CDOWN).
 *
 * Timing: one RSP cycle per instruction (no dual issue, no pipeline stalls); SP DMA completes instantly (DMA_BUSY and
 * DMA_FULL always read 0). */
#include "n64_devices.h"

#include <stdlib.h>
#include <stddef.h>
#include <string.h>

/* arm64: the vector unit's hot ops and the quad loads / stores as NEON intrinsics (XBA-173), the portable C
 * everywhere else and for the remaining ops. -DN64_RSP_NO_NEON forces the portable code (to compare). */
#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(N64_RSP_NO_NEON)
#define RSP_NEON 1
#include <arm_neon.h>
#else
#define RSP_NEON 0
#endif
/* x86-64: the same ops and loads / stores in SSE2 (every x86-64 CPU has it). -DN64_RSP_NO_SSE2 forces the portable
 * code. */
#if !RSP_NEON && (defined(__SSE2__) || defined(_M_X64)) && !defined(N64_RSP_NO_SSE2)
#define RSP_SSE2 1
#include <emmintrin.h>
#else
#define RSP_SSE2 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#define RSP_NOINLINE __attribute__((noinline))
#define RSP_INLINE inline __attribute__((always_inline))
#define RSP_UNLIKELY(x) __builtin_expect(!!(x), 0)
static inline unsigned clz32(uint32_t x) /* x != 0 */
{
    return (unsigned)__builtin_clz(x);
}
#else
#define RSP_NOINLINE
#ifdef _MSC_VER
#define RSP_INLINE __forceinline
#else
#define RSP_INLINE inline
#endif
#define RSP_UNLIKELY(x) (x)
static inline unsigned clz32(uint32_t x)
{
    unsigned n = 0;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
}
#endif

/* SP_STATUS read bits. */
#define ST_HALT 0x0001u
#define ST_BROKE 0x0002u
#define ST_DMA_BUSY 0x0004u
#define ST_DMA_FULL 0x0008u
#define ST_IO_FULL 0x0010u
#define ST_SSTEP 0x0020u
#define ST_INTR_BREAK 0x0040u
#define ST_SIG0 0x0080u

/* A predecoded IMEM word (n64_rsp_run): what to run and its operands, so the loop does no field extraction or nested
 * decode. word is the IMEM word as the host loads it (the four big-endian bytes in host order): an entry is always the
 * decode of its own word at its own slot, and the loop compares the word it fetches against it and decodes again when
 * they differ. So nothing that writes IMEM (the CPU, SP DMA, the HLE boot, save state loads, tests poking
 * n64_rsp_mem) has to tell the table anything, and a stale entry cannot run. */
typedef struct rsp_dec {
    uint32_t word;
    uint8_t kind;    /* RK_* */
    uint8_t a, b, c; /* scalar: rs, rt, rd; vector ops: vs, vt, vd */
    uint32_t imm;    /* immediate (sign / zero extended, LUI's already shifted), shift amount, branch / jump target, the
                      * whole instruction (COP2 moves, LWC2, SWC2); vector ops: vt's offset in n64_rsp << 16 | vs's */
    uint16_t link;   /* this slot's address + 8 (JAL, JALR, BLTZAL, BGEZAL); vector ops: vd's offset in n64_rsp */
    uint8_t e, vop;  /* vector ops: element selector, index into vu_fns (the opcode, + 64 for selectors 0 / 1) */
} rsp_dec;

struct n64_rsp {
    uint8_t mem[0x2000]; /* DMEM 0x000..0xFFF, IMEM 0x1000..0x1FFF, big-endian */
    int16_t vr[32][8];   /* lane i = element i */
    int32_t acch[8];     /* accumulator bits 47..16 (signed) */
    uint16_t accl[8];    /* accumulator bits 15..0 */
    uint16_t vcol[8], vcoh[8], vccl[8], vcch[8], vce[8]; /* one flag per lane, 0 or 1 */
    int16_t divin, divout;
    int divdp;
    uint32_t r[32];
    uint32_t pc, npc; /* IMEM offsets (word aligned, 12 bits) */
    uint32_t status;
    uint32_t mem_addr, dram_addr, dma_len;
    uint32_t semaphore;
    n64_rcp_bus bus;
    n64_rdp *dp;
    uint16_t rcp_rom[512];
    uint16_t rsq_rom[512];
    rsp_dec dec[1024]; /* one per IMEM word; host only, not in save states (it checks itself against IMEM) */
    uint32_t idle_b, idle_n, idle_r[32]; /* rsp_idle's note: the branch, the instruction count, the registers */
};

/* ---- ROMs --------------------------------------------------------------------------------------------------------- */

/* Reciprocal ROM: entry i is 1 / (1 + i / 512) as 1.16 fixed point with the leading one dropped, rounded:
 * ((2^34 / (512 + i)) + 1) >> 8, minus 2^16 (entry 0, exactly 1.0, saturates to 0xFFFF). Inverse square root ROM:
 * entries 0..255 hold 2 / sqrt(1 + i / 256) and 256..511 hold sqrt(2 / (1 + (i - 256) / 256)), both 1.16 with the
 * leading one dropped: the largest b with a * b^2 < 2^44 (a = 256 + i, or 512 + 2 (i - 256)), halved. Both checked
 * entry by entry against the ares oracle (oracle group 92). */
static void build_roms(n64_rsp *s)
{
    for (uint32_t i = 0; i < 512; i++) {
        uint64_t v = ((((uint64_t)1 << 34) / (512 + i)) + 1) >> 8;
        s->rcp_rom[i] = v > 0x1FFFF ? 0xFFFF : (uint16_t)(v - 0x10000);
    }
    for (uint32_t i = 0; i < 512; i++) {
        uint64_t a = i < 256 ? 256 + i : 512 + ((i - 256) << 1);
        /* b in [2^17, 2^18): the largest b with a * b^2 < 2^44 (then a * (b + 1)^2 >= 2^44). */
        uint64_t lo = (uint64_t)1 << 17, hi = (uint64_t)1 << 18;
        while (lo + 1 < hi) {
            uint64_t mid = (lo + hi) >> 1;
            if (a * mid * mid < ((uint64_t)1 << 44))
                lo = mid;
            else
                hi = mid;
        }
        uint64_t b = lo;
        s->rsq_rom[i] = (uint16_t)((b >> 1) & 0xFFFF);
    }
}

/* ---- Helpers ------------------------------------------------------------------------------------------------------ */

static inline int16_t sclamp(int32_t v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* acc[15:0], or 0 / 0xFFFF when the accumulator does not fit in 32 signed bits (VMADL, VMADN). */
static inline int16_t clamp_lo(int32_t hm, uint16_t lo)
{
    return hm < -32768 ? 0 : hm > 32767 ? -1 : (int16_t)lo;
}

/* Unsigned result of VMULU / VMACU: 0 when the accumulator is negative, 0xFFFF when acc[47:16] exceeds 0x7FFF,
 * else acc[31:16]. */
static inline int16_t clamp_u(int32_t hm)
{
    return hm < 0 ? 0 : hm > 32767 ? -1 : (int16_t)hm;
}

static inline uint8_t vgetb(const int16_t *v, unsigned b)
{
    uint16_t x = (uint16_t)v[(b >> 1) & 7];
    return (uint8_t)(b & 1 ? x : x >> 8);
}

static inline void vsetb(int16_t *v, unsigned b, uint8_t x)
{
    uint16_t w = (uint16_t)v[(b >> 1) & 7];
    w = b & 1 ? (uint16_t)((w & 0xFF00) | x) : (uint16_t)((w & 0x00FF) | x << 8);
    v[(b >> 1) & 7] = (int16_t)w;
}

#define DM(a) (s->mem[(a) & 0xFFF])


/* ---- Lifecycle ---------------------------------------------------------------------------------------------------- */

n64_rsp *n64_rsp_create(n64_rcp_bus bus)
{
    n64_rsp *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->bus = bus;
    build_roms(s);
    /* calloc: IMEM and every s->dec entry are word 0 (SLL $0, $0, 0), which decodes to RK_NOP = 0 (n64_rsp_run) */
    n64_rsp_reset(s);
    return s;
}

void n64_rsp_destroy(n64_rsp *s)
{
    free(s);
}

/* Registers and state back to power-on; DMEM / IMEM keep their contents (the boot code may already be there). */
void n64_rsp_reset(n64_rsp *s)
{
    memset(s->vr, 0, sizeof s->vr);
    memset(s->acch, 0, sizeof s->acch);
    memset(s->accl, 0, sizeof s->accl);
    memset(s->vcol, 0, sizeof s->vcol);
    memset(s->vcoh, 0, sizeof s->vcoh);
    memset(s->vccl, 0, sizeof s->vccl);
    memset(s->vcch, 0, sizeof s->vcch);
    memset(s->vce, 0, sizeof s->vce);
    s->divin = s->divout = 0;
    s->divdp = 0;
    memset(s->r, 0, sizeof s->r);
    s->pc = 0;
    s->npc = 4;
    s->status = ST_HALT;
    s->mem_addr = s->dram_addr = 0;
    s->dma_len = 0;
    s->semaphore = 0;
}

/* Save states (XBA-183): everything up to the host side (the bus, the RDP; the ROMs after them are constants). */
#define RSP_STATE_SIZE offsetof(n64_rsp, bus)

size_t n64_rsp_state_size(n64_rsp *s)
{
    (void)s;
    return RSP_STATE_SIZE;
}

void n64_rsp_state_save(n64_rsp *s, uint8_t *out) { memcpy(out, s, RSP_STATE_SIZE); }

/* PC and the next PC index IMEM (and dec[pc / 4]) unmasked in n64_rsp_run: word offsets of 12 bits. */
int n64_rsp_state_check(n64_rsp *s, const uint8_t *in, size_t n)
{
    (void)s;
    if (n != RSP_STATE_SIZE)
        return -1;
    uint32_t pc, npc;
    memcpy(&pc, in + offsetof(n64_rsp, pc), 4);
    memcpy(&npc, in + offsetof(n64_rsp, npc), 4);
    if ((pc & ~0xFFCu) || (npc & ~0xFFCu))
        return -2;
    return 0;
}

int n64_rsp_state_load(n64_rsp *s, const uint8_t *in, size_t n)
{
    int rc = n64_rsp_state_check(s, in, n);
    if (rc != 0)
        return rc;
    memcpy(s, in, RSP_STATE_SIZE);
    return 0;
}

void n64_rsp_attach_rdp(n64_rsp *s, n64_rdp *dp)
{
    s->dp = dp;
    if (dp)
        n64_rdp_attach_dmem(dp, s->mem);
}

uint8_t *n64_rsp_mem(n64_rsp *s)
{
    return s->mem;
}

int n64_rsp_halted(n64_rsp *s)
{
    return (s->status & ST_HALT) != 0;
}

uint32_t n64_rsp_get_gpr(n64_rsp *s, int r)
{
    return (r > 0 && r < 32) ? s->r[r] : 0;
}

void n64_rsp_set_gpr(n64_rsp *s, int r, uint32_t v)
{
    if (r > 0 && r < 32)
        s->r[r] = v;
}

int n64_rsp_semaphore_held(n64_rsp *s)
{
    return s->semaphore != 0;
}

/* ---- SP registers ------------------------------------------------------------------------------------------------- */

static void raise_sp(n64_rsp *s)
{
    if (s->bus.raise)
        s->bus.raise(s->bus.ctx, N64_MI_SP);
}

static void clear_sp(n64_rsp *s)
{
    if (s->bus.clear)
        s->bus.clear(s->bus.ctx, N64_MI_SP);
}

/* SP DMA: (count + 1) rows of (length | 7) + 1 bytes, RDRAM advancing by skip between rows, SP memory wrapping
 * inside its 4 KB bank. Completes at once; the address registers end just past the last byte moved (no skip after
 * the last row), the length register reads length 0xFF8, count 0, skip (8-aligned) unchanged. */
static void sp_dma(n64_rsp *s, int to_rdram, uint32_t lenreg)
{
    uint32_t len = (lenreg & 0xFF8) + 8;
    uint32_t count = ((lenreg >> 12) & 0xFF) + 1;
    uint32_t skip = (lenreg >> 20) & 0xFF8;
    uint32_t bank = s->mem_addr & 0x1000, off = s->mem_addr & 0xFF8;
    uint32_t da = s->dram_addr & 0xFFFFF8;
    uint8_t *rd = s->bus.rdram;
    uint32_t size = s->bus.rdram_size;
    for (uint32_t row = 0; row < count; row++) {
        /* In runs that stay inside the SP bank, inside the 24-bit RDRAM address space and on one side of RDRAM's
         * end: the bytes of the 8-byte steps the hardware takes, one memcpy and one write notice per run (XBA-191). */
        for (uint32_t b = 0; b < len;) {
            uint32_t n = len - b;
            if (n > 0x1000 - off)
                n = 0x1000 - off;
            if (n > 0x1000000 - da)
                n = 0x1000000 - da;
            int in = rd && da + 8 <= size;
            if (in && n > size - da)
                n = size - da;
            uint8_t *sp = s->mem + (bank | off);
            if (to_rdram) {
                if (in) {
                    memcpy(rd + da, sp, n);
                    if (s->bus.wrote)
                        s->bus.wrote(s->bus.ctx, da, n);
                }
            } else {
                if (in)
                    memcpy(sp, rd + da, n);
                else
                    memset(sp, 0, n);
            }
            b += n;
            off = (off + n) & 0xFF8;
            da = (da + n) & 0xFFFFF8;
        }
        if (row + 1 < count)
            da = (da + skip) & 0xFFFFF8;
    }
    s->mem_addr = bank | off;
    s->dram_addr = da;
    s->dma_len = 0xFF8 | (lenreg & 0xFF800000u);
}

/* SP_STATUS write: clear / set pairs (both bits set = no change), clear broke, MI SP interrupt clear / raise. */
static void status_write(n64_rsp *s, uint32_t v)
{
    uint32_t st = s->status;
#define SP_PAIR(clr, set, bit)                                                                                         \
    do {                                                                                                               \
        if ((v & (clr)) && !(v & (set)))                                                                               \
            st &= ~(uint32_t)(bit);                                                                                    \
        else if ((v & (set)) && !(v & (clr)))                                                                          \
            st |= (bit);                                                                                               \
    } while (0)
    SP_PAIR(1u << 0, 1u << 1, ST_HALT);
    if (v & (1u << 2))
        st &= ~ST_BROKE;
    SP_PAIR(1u << 5, 1u << 6, ST_SSTEP);
    SP_PAIR(1u << 7, 1u << 8, ST_INTR_BREAK);
    for (unsigned k = 0; k < 8; k++)
        SP_PAIR(1u << (9 + 2 * k), 1u << (10 + 2 * k), ST_SIG0 << k);
#undef SP_PAIR
    s->status = st;
    if ((v & (1u << 3)) && !(v & (1u << 4)))
        clear_sp(s);
    else if ((v & (1u << 4)) && !(v & (1u << 3)))
        raise_sp(s);
}

uint32_t n64_rsp_read_reg(n64_rsp *s, uint32_t reg)
{
    switch (reg) {
    case 0: return s->mem_addr;
    case 1: return s->dram_addr;
    case 2:
    case 3: return s->dma_len;
    case 4: return s->status;
    case 5: /* DMA_FULL */
    case 6: return 0; /* DMA_BUSY */
    case 7: {
        uint32_t v = s->semaphore;
        s->semaphore = 1;
        return v;
    }
    case 8: return s->pc & 0xFFC;
    default: return 0;
    }
}

void n64_rsp_write_reg(n64_rsp *s, uint32_t reg, uint32_t v)
{
    switch (reg) {
    case 0: s->mem_addr = v & 0x1FF8; break;
    case 1: s->dram_addr = v & 0xFFFFF8; break;
    case 2: sp_dma(s, 0, v); break;
    case 3: sp_dma(s, 1, v); break;
    case 4: status_write(s, v); break;
    case 7: s->semaphore = 0; break;
    case 8:
        s->pc = v & 0xFFC;
        s->npc = (s->pc + 4) & 0xFFC;
        break;
    default: break;
    }
}

static uint32_t cop0_read(n64_rsp *s, unsigned r)
{
    if (r < 8)
        return n64_rsp_read_reg(s, r);
    return s->dp ? n64_rdp_read_reg(s->dp, r - 8) : 0;
}

static void cop0_write(n64_rsp *s, unsigned r, uint32_t v)
{
    if (r < 8) {
        n64_rsp_write_reg(s, r, v);
        return;
    }
    if (!s->dp)
        return;
    n64_rdp_write_reg(s->dp, r - 8, v);
    if (r == 9) /* DPC_END: the RDP works in parallel with the RSP, let it catch up now (XBUS lists live in DMEM) */
        n64_rdp_run(s->dp);
}

/* ---- Vector unit: computational ops ------------------------------------------------------------------------------- */

/* VRCP / VRSQ (and their L forms) on a 16 or 32-bit input: the result's low half goes to the lane, the high half to
 * DIVOUT. Negative inputs use the absolute value (one's complement below -32768) and complement the result. */
static void vu_divide(n64_rsp *s, int rsq, int32_t input, int16_t *out_lane)
{
    int32_t mask = input >> 31;
    int32_t data = input ^ mask;
    int32_t result;
    if (input > -32768)
        data -= mask;
    if (data == 0) {
        result = 0x7FFFFFFF;
    } else if (input == -32768) {
        result = (int32_t)0xFFFF0000u;
    } else {
        unsigned shift = clz32((uint32_t)data);
        uint32_t m9 = (uint32_t)(((uint64_t)(uint32_t)data << shift) & 0x7FC00000u) >> 22;
        uint32_t base;
        if (rsq)
            base = s->rsq_rom[((shift & 1) ? 0 : 256) | (m9 >> 1)];
        else
            base = s->rcp_rom[m9];
        uint32_t r = (0x10000u | base) << 14;
        r = rsq ? r >> ((31 - shift) >> 1) : r >> (31 - shift);
        result = (int32_t)(r ^ (uint32_t)mask);
    }
    s->divdp = 0;
    s->divout = (int16_t)(result >> 16);
    *out_lane = (int16_t)result;
}

/* acc += addend, the addend given as its high part h (bits 47..16, signed) and low part l (bits 15..0). */
#define ACC_ADD(i, h, l)                                                                                               \
    do {                                                                                                               \
        uint32_t lo_ = (uint32_t)accl[i] + (uint32_t)(l);                                                              \
        acch[i] = (int32_t)((uint32_t)acch[i] + (uint32_t)(h) + (lo_ >> 16));                                          \
        accl[i] = (uint16_t)lo_;                                                                                       \
    } while (0)

#if RSP_NEON
/* ---- Vector unit on NEON (arm64) ---------------------------------------------------------------------------------- */

/* The same results as the portable loops of vu_op, bit for bit (test_rsp.c's ares-checked groups run on both), as
 * explicit intrinsics so they do not depend on what the compiler manages to vectorise: one 8 x 16-bit register per
 * operand, the accumulator's bits 47..16 as two 4 x 32-bit halves. Flags are 0 / 1 per lane in memory, masks here. */

/* Byte shuffles for the 16 element selectors (0, 1: the vector; 2, 3: one element of each pair; 4..7: of each half;
 * 8..15: one element everywhere), element j being bytes 2j, 2j + 1: one table lookup for any selector, no branch. */
#define SB(x) 2 * (x), 2 * (x) + 1
#define SB4(a, b, c, d) SB(a), SB(b), SB(c), SB(d)
static const uint8_t sel_bytes[16][16] = {
    {SB4(0, 1, 2, 3), SB4(4, 5, 6, 7)}, {SB4(0, 1, 2, 3), SB4(4, 5, 6, 7)}, {SB4(0, 0, 2, 2), SB4(4, 4, 6, 6)},
    {SB4(1, 1, 3, 3), SB4(5, 5, 7, 7)}, {SB4(0, 0, 0, 0), SB4(4, 4, 4, 4)}, {SB4(1, 1, 1, 1), SB4(5, 5, 5, 5)},
    {SB4(2, 2, 2, 2), SB4(6, 6, 6, 6)}, {SB4(3, 3, 3, 3), SB4(7, 7, 7, 7)}, {SB4(0, 0, 0, 0), SB4(0, 0, 0, 0)},
    {SB4(1, 1, 1, 1), SB4(1, 1, 1, 1)}, {SB4(2, 2, 2, 2), SB4(2, 2, 2, 2)}, {SB4(3, 3, 3, 3), SB4(3, 3, 3, 3)},
    {SB4(4, 4, 4, 4), SB4(4, 4, 4, 4)}, {SB4(5, 5, 5, 5), SB4(5, 5, 5, 5)}, {SB4(6, 6, 6, 6), SB4(6, 6, 6, 6)},
    {SB4(7, 7, 7, 7), SB4(7, 7, 7, 7)},
};
#undef SB4
#undef SB

static inline int16x8_t nv_sel(const int16_t *tv, unsigned e, int whole)
{
    if (whole) /* selectors 0 and 1, known when the op was decoded */
        return vld1q_s16(tv);
    return vreinterpretq_s16_u8(vqtbl1q_u8(vreinterpretq_u8_s16(vld1q_s16(tv)), vld1q_u8(sel_bytes[e])));
}

static inline uint16x8_t nv_mask(const uint16_t *f) /* 0 / 1 flags -> 0 / all-ones */
{
    return vreinterpretq_u16_s16(vnegq_s16(vreinterpretq_s16_u16(vld1q_u16(f))));
}

static inline void nv_flag(uint16_t *f, uint16x8_t m) /* mask -> 0 / 1 flags */
{
    vst1q_u16(f, vshrq_n_u16(m, 15));
}

static inline int16x8_t nv_sclamp(int32x4_t h0, int32x4_t h1)
{
    return vcombine_s16(vqmovn_s32(h0), vqmovn_s32(h1));
}

/* clamp_u: 0 below 0, 0xFFFF above 0x7FFF. */
static inline int16x8_t nv_clamp_u(int32x4_t h0, int32x4_t h1)
{
    int16x8_t x = vreinterpretq_s16_u16(vcombine_u16(vqmovun_s32(h0), vqmovun_s32(h1)));
    return vorrq_s16(x, vshrq_n_s16(x, 15));
}

/* clamp_lo: acc[15:0], 0 when acc[47:16] < -32768, 0xFFFF when it is > 32767. */
static inline int16x8_t nv_clamp_lo(int32x4_t h0, int32x4_t h1, uint16x8_t lo)
{
    uint16x8_t lt = vuzp1q_u16(vreinterpretq_u16_u32(vcltq_s32(h0, vdupq_n_s32(-32768))),
                               vreinterpretq_u16_u32(vcltq_s32(h1, vdupq_n_s32(-32768))));
    uint16x8_t gt = vuzp1q_u16(vreinterpretq_u16_u32(vcgtq_s32(h0, vdupq_n_s32(32767))),
                               vreinterpretq_u16_u32(vcgtq_s32(h1, vdupq_n_s32(32767))));
    return vreinterpretq_s16_u16(vbicq_u16(vorrq_u16(lo, gt), lt));
}

/* sa * ub (signed x unsigned 16-bit) as 32-bit products, lanes 0..3 and 4..7. */
static inline int32x4_t nv_mulsu_lo(int16x8_t sa, int16x8_t ub)
{
    return vmulq_s32(vmovl_s16(vget_low_s16(sa)),
                     vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vreinterpretq_u16_s16(ub)))));
}

static inline int32x4_t nv_mulsu_hi(int16x8_t sa, int16x8_t ub)
{
    return vmulq_s32(vmovl_high_s16(sa), vreinterpretq_s32_u32(vmovl_high_u16(vreinterpretq_u16_s16(ub))));
}

/* acc += addend, given as its low 16 bits l16 and its bits 47..16 (h0, h1): ACC_ADD on 8 lanes. */
#define NV_ACC_ADD(l16, h0, h1)                                                                                        \
    do {                                                                                                               \
        uint32x4_t s0_ = vaddl_u16(vget_low_u16(accl), vget_low_u16(l16));                                             \
        uint32x4_t s1_ = vaddl_high_u16(accl, l16);                                                                    \
        acch0 = vaddq_s32(acch0, vaddq_s32(vreinterpretq_s32_u32(vshrq_n_u32(s0_, 16)), (h0)));                        \
        acch1 = vaddq_s32(acch1, vaddq_s32(vreinterpretq_s32_u32(vshrq_n_u32(s1_, 16)), (h1)));                        \
        accl = vaddq_u16(accl, l16);                                                                                   \
    } while (0)

#define NV_ACC_LOAD()                                                                                                  \
    do {                                                                                                               \
        acch0 = vld1q_s32(s->acch);                                                                                    \
        acch1 = vld1q_s32(s->acch + 4);                                                                                \
        accl = vld1q_u16(s->accl);                                                                                     \
    } while (0)

/* The hot computational ops; returns 0 for the others, which the portable code runs. */
static RSP_INLINE int vu_op_neon(n64_rsp *s, uint32_t op, int whole, unsigned e, const int16_t *pt,
                                 const int16_t *ps, int16_t *pd, unsigned vs)
{
    int16x8_t a = vld1q_s16(ps), t = nv_sel(pt, e, whole), r;
    int32x4_t acch0, acch1;
    uint16x8_t accl, z = vdupq_n_u16(0);
    switch (op & 63) {
    case 0x00: /* VMULF */
    case 0x01: /* VMULU */
    {
        int32x4_t p0 = vmull_s16(vget_low_s16(a), vget_low_s16(t)), p1 = vmull_high_s16(a, t);
        acch0 = vrshrq_n_s32(p0, 15); /* (p + 0x4000) >> 15 */
        acch1 = vrshrq_n_s32(p1, 15);
        accl = vaddq_u16(vshlq_n_u16(vreinterpretq_u16_s16(vmulq_s16(a, t)), 1), vdupq_n_u16(0x8000));
        r = (op & 1) ? nv_clamp_u(acch0, acch1) : vqrdmulhq_s16(a, t); /* sat((2ab + 0x8000) >> 16) = sclamp(hm) */
        break;
    }
    case 0x04: /* VMUDL */
    {
        uint16x8_t ua = vreinterpretq_u16_s16(a), ut = vreinterpretq_u16_s16(t);
        uint32x4_t p0 = vmull_u16(vget_low_u16(ua), vget_low_u16(ut)), p1 = vmull_high_u16(ua, ut);
        accl = vuzp2q_u16(vreinterpretq_u16_u32(p0), vreinterpretq_u16_u32(p1)); /* the high halves */
        acch0 = acch1 = vdupq_n_s32(0);
        r = vreinterpretq_s16_u16(accl);
        break;
    }
    case 0x05: /* VMUDM: a * ub */
    case 0x06: /* VMUDN: ua * b */
    {
        int16x8_t sx = (op & 1) ? a : t, ux = (op & 1) ? t : a;
        int32x4_t p0 = nv_mulsu_lo(sx, ux), p1 = nv_mulsu_hi(sx, ux);
        acch0 = vshrq_n_s32(p0, 16);
        acch1 = vshrq_n_s32(p1, 16);
        accl = vreinterpretq_u16_s16(vmulq_s16(a, t));
        r = (op & 1) ? vuzp2q_s16(vreinterpretq_s16_s32(p0), vreinterpretq_s16_s32(p1)) : vreinterpretq_s16_u16(accl);
        break;
    }
    case 0x07: /* VMUDH */
        acch0 = vmull_s16(vget_low_s16(a), vget_low_s16(t));
        acch1 = vmull_high_s16(a, t);
        accl = z;
        r = nv_sclamp(acch0, acch1);
        break;
    case 0x08: /* VMACF */
    case 0x09: /* VMACU */
    {
        int32x4_t p0 = vmull_s16(vget_low_s16(a), vget_low_s16(t)), p1 = vmull_high_s16(a, t);
        uint16x8_t l = vshlq_n_u16(vreinterpretq_u16_s16(vmulq_s16(a, t)), 1);
        NV_ACC_LOAD();
        NV_ACC_ADD(l, vshrq_n_s32(p0, 15), vshrq_n_s32(p1, 15));
        r = (op & 1) ? nv_clamp_u(acch0, acch1) : nv_sclamp(acch0, acch1);
        break;
    }
    case 0x0C: /* VMADL */
    {
        uint16x8_t ua = vreinterpretq_u16_s16(a), ut = vreinterpretq_u16_s16(t);
        uint32x4_t p0 = vmull_u16(vget_low_u16(ua), vget_low_u16(ut)), p1 = vmull_high_u16(ua, ut);
        uint16x8_t l = vuzp2q_u16(vreinterpretq_u16_u32(p0), vreinterpretq_u16_u32(p1));
        NV_ACC_LOAD();
        NV_ACC_ADD(l, vdupq_n_s32(0), vdupq_n_s32(0));
        r = nv_clamp_lo(acch0, acch1, accl);
        break;
    }
    case 0x0D: /* VMADM: a * ub */
    case 0x0E: /* VMADN: ua * b */
    {
        int16x8_t sx = (op & 1) ? a : t, ux = (op & 1) ? t : a;
        int32x4_t p0 = nv_mulsu_lo(sx, ux), p1 = nv_mulsu_hi(sx, ux);
        uint16x8_t l = vreinterpretq_u16_s16(vmulq_s16(a, t));
        NV_ACC_LOAD();
        NV_ACC_ADD(l, vshrq_n_s32(p0, 16), vshrq_n_s32(p1, 16));
        r = (op & 1) ? nv_sclamp(acch0, acch1) : nv_clamp_lo(acch0, acch1, accl);
        break;
    }
    case 0x0F: /* VMADH: ACCL unchanged */
        acch0 = vaddq_s32(vld1q_s32(s->acch), vmull_s16(vget_low_s16(a), vget_low_s16(t)));
        acch1 = vaddq_s32(vld1q_s32(s->acch + 4), vmull_high_s16(a, t));
        vst1q_s32(s->acch, acch0);
        vst1q_s32(s->acch + 4, acch1);
        vst1q_s16(pd, nv_sclamp(acch0, acch1));
        return 1;
    case 0x10: /* VADD */
    case 0x11: /* VSUB */
    {
        int16x8_t c = vreinterpretq_s16_u16(vld1q_u16(s->vcol));
        int32x4_t v0, v1;
        if (op & 1) {
            v0 = vsubw_s16(vsubl_s16(vget_low_s16(a), vget_low_s16(t)), vget_low_s16(c));
            v1 = vsubw_high_s16(vsubl_high_s16(a, t), c);
        } else {
            v0 = vaddw_s16(vaddl_s16(vget_low_s16(a), vget_low_s16(t)), vget_low_s16(c));
            v1 = vaddw_high_s16(vaddl_high_s16(a, t), c);
        }
        vst1q_u16(s->accl, vuzp1q_u16(vreinterpretq_u16_s32(v0), vreinterpretq_u16_s32(v1)));
        vst1q_u16(s->vcol, z);
        vst1q_u16(s->vcoh, z);
        vst1q_s16(pd, nv_sclamp(v0, v1));
        return 1;
    }
    case 0x14: /* VADDC */
    {
        uint16x8_t ua = vreinterpretq_u16_s16(a), sum = vaddq_u16(ua, vreinterpretq_u16_s16(t));
        vst1q_u16(s->accl, sum);
        nv_flag(s->vcol, vcltq_u16(sum, ua));
        vst1q_u16(s->vcoh, z);
        vst1q_s16(pd, vreinterpretq_s16_u16(sum));
        return 1;
    }
    case 0x15: /* VSUBC */
    {
        uint16x8_t ua = vreinterpretq_u16_s16(a), ut = vreinterpretq_u16_s16(t), d = vsubq_u16(ua, ut);
        vst1q_u16(s->accl, d);
        nv_flag(s->vcol, vcltq_u16(ua, ut));
        nv_flag(s->vcoh, vmvnq_u16(vceqq_u16(ua, ut)));
        vst1q_s16(pd, vreinterpretq_s16_u16(d));
        return 1;
    }
    case 0x20: /* VLT */
    case 0x21: /* VEQ */
    case 0x22: /* VNE */
    case 0x23: /* VGE */
    {
        uint16x8_t eq = vceqq_s16(a, t), coh = nv_mask(s->vcoh), col = nv_mask(s->vcol), c;
        switch (op & 3) {
        case 0: c = vorrq_u16(vcltq_s16(a, t), vandq_u16(eq, vandq_u16(coh, col))); break;
        case 1: c = vbicq_u16(eq, coh); break;
        case 2: c = vorrq_u16(vmvnq_u16(eq), coh); break;
        default: c = vorrq_u16(vcgtq_s16(a, t), vbicq_u16(eq, vandq_u16(coh, col))); break;
        }
        r = vbslq_s16(c, a, t);
        vst1q_u16(s->accl, vreinterpretq_u16_s16(r));
        nv_flag(s->vccl, c);
        vst1q_u16(s->vcch, z);
        vst1q_u16(s->vcol, z);
        vst1q_u16(s->vcoh, z);
        vst1q_s16(pd, r);
        return 1;
    }
    case 0x24: /* VCL */
    {
        uint16x8_t ua = vreinterpretq_u16_s16(a), ut = vreinterpretq_u16_s16(t), sum = vaddq_u16(ua, ut);
        uint16x8_t zero = vceqzq_u16(sum), nocy = vcgeq_u16(sum, ua);
        uint16x8_t carry = nv_mask(s->vcol), ne = nv_mask(s->vcoh), ce = nv_mask(s->vce);
        uint16x8_t lo_new = vbslq_u16(ce, vorrq_u16(zero, nocy), vandq_u16(zero, nocy));
        uint16x8_t hi_new = vcgeq_u16(ua, ut);
        uint16x8_t ccl = vbslq_u16(vbicq_u16(carry, ne), lo_new, nv_mask(s->vccl));
        uint16x8_t cch = vbslq_u16(vorrq_u16(carry, ne), nv_mask(s->vcch), hi_new);
        uint16x8_t sel = vbslq_u16(carry, ccl, cch);
        uint16x8_t other = vbslq_u16(carry, vreinterpretq_u16_s16(vnegq_s16(t)), ut);
        uint16x8_t lo = vbslq_u16(sel, other, ua);
        nv_flag(s->vccl, ccl);
        nv_flag(s->vcch, cch);
        vst1q_u16(s->accl, lo);
        vst1q_u16(s->vcol, z);
        vst1q_u16(s->vcoh, z);
        vst1q_u16(s->vce, z);
        vst1q_s16(pd, vreinterpretq_s16_u16(lo));
        return 1;
    }
    case 0x25: /* VCH: a + b (signs differ) or a - b (same signs) never overflows 16 bits */
    {
        uint16x8_t dm = vreinterpretq_u16_s16(vshrq_n_s16(veorq_s16(a, t), 15));
        uint16x8_t tnm = vreinterpretq_u16_s16(vshrq_n_s16(t, 15));
        int16x8_t v = vbslq_s16(dm, vaddq_s16(a, t), vsubq_s16(a, t));
        uint16x8_t lem = vclezq_s16(v), gem = vcgezq_s16(v);
        int16x8_t lo = vbslq_s16(vbslq_u16(dm, lem, gem), vbslq_s16(dm, vnegq_s16(t), t), a);
        uint16x8_t ne = vbicq_u16(vmvnq_u16(vceqzq_s16(v)), vceqq_s16(a, vmvnq_s16(t)));
        nv_flag(s->vccl, vbslq_u16(dm, lem, tnm));
        nv_flag(s->vcch, vbslq_u16(dm, tnm, gem));
        nv_flag(s->vcol, dm);
        nv_flag(s->vcoh, ne);
        nv_flag(s->vce, vandq_u16(dm, vceqq_s16(v, vdupq_n_s16(-1))));
        vst1q_u16(s->accl, vreinterpretq_u16_s16(lo));
        vst1q_s16(pd, lo);
        return 1;
    }
    case 0x27: /* VMRG */
        r = vbslq_s16(nv_mask(s->vccl), a, t);
        vst1q_u16(s->vcol, z);
        vst1q_u16(s->vcoh, z);
        goto logic;
    case 0x28: r = vandq_s16(a, t); goto logic;            /* VAND */
    case 0x29: r = vmvnq_s16(vandq_s16(a, t)); goto logic; /* VNAND */
    case 0x2A: r = vorrq_s16(a, t); goto logic;            /* VOR */
    case 0x2B: r = vmvnq_s16(vorrq_s16(a, t)); goto logic; /* VNOR */
    case 0x2C: r = veorq_s16(a, t); goto logic;            /* VXOR */
    case 0x2D: r = vmvnq_s16(veorq_s16(a, t)); goto logic; /* VNXOR */
    logic:
        vst1q_u16(s->accl, vreinterpretq_u16_s16(r));
        vst1q_s16(pd, r);
        return 1;
    case 0x30: /* VRCP */
    case 0x31: /* VRCPL */
    case 0x34: /* VRSQ */
    case 0x35: /* VRSQL */
    {
        const int16_t *tv = pt;
        int32_t input = (op & 1) && s->divdp ? (int32_t)((uint32_t)(uint16_t)s->divin << 16 | (uint16_t)tv[e & 7])
                                             : (int32_t)tv[e & 7];
        vst1q_u16(s->accl, vreinterpretq_u16_s16(t));
        vu_divide(s, (op & 4) != 0, input, &pd[vs & 7]);
        return 1;
    }
    case 0x32: /* VRCPH */
    case 0x36: /* VRSQH */
        vst1q_u16(s->accl, vreinterpretq_u16_s16(t));
        s->divdp = 1;
        s->divin = pt[e & 7];
        pd[vs & 7] = s->divout;
        return 1;
    case 0x37: /* VNOP */
    case 0x3F: /* VNULL */
        return 1;
    default:
        return 0;
    }
    vst1q_s32(s->acch, acch0);
    vst1q_s32(s->acch + 4, acch1);
    vst1q_u16(s->accl, accl);
    vst1q_s16(pd, r);
    return 1;
}
#endif /* RSP_NEON */

#if RSP_SSE2
/* ---- Vector unit on SSE2 (x86-64) --------------------------------------------------------------------------------- */

/* The ops vu_op_neon runs, the same results as the portable loops of vu_exec bit for bit (test_rsp.c's ares-checked
 * groups), in SSE2 only (every x86-64 CPU): 32-bit products from the low and high 16-bit halves, unsigned compares as
 * signed ones on values with bit 15 flipped, signed x unsigned products as signed ones corrected by the multiplicand
 * where the unsigned operand has bit 15 set. Flags are 0 / 1 per lane in memory, masks here. */
#define SV_LD(p) _mm_loadu_si128((const __m128i *)(const void *)(p))
#define SV_ST(p, v) _mm_storeu_si128((__m128i *)(void *)(p), (v))

static inline __m128i sv_sel(const int16_t *tv, unsigned e, int whole)
{
    __m128i v = SV_LD(tv);
    if (whole) /* selectors 0 and 1, known when the op was decoded */
        return v;
    switch (e) {
    case 0:
    case 1: return v;
    case 2: return _mm_shufflehi_epi16(_mm_shufflelo_epi16(v, 0xA0), 0xA0); /* 0 0 2 2 4 4 6 6 */
    case 3: return _mm_shufflehi_epi16(_mm_shufflelo_epi16(v, 0xF5), 0xF5); /* 1 1 3 3 5 5 7 7 */
    case 4: return _mm_shufflehi_epi16(_mm_shufflelo_epi16(v, 0x00), 0x00); /* 0 x 4, 4 x 4 */
    case 5: return _mm_shufflehi_epi16(_mm_shufflelo_epi16(v, 0x55), 0x55);
    case 6: return _mm_shufflehi_epi16(_mm_shufflelo_epi16(v, 0xAA), 0xAA);
    case 7: return _mm_shufflehi_epi16(_mm_shufflelo_epi16(v, 0xFF), 0xFF);
    default: return _mm_set1_epi16(tv[e & 7]); /* 8..15: one element everywhere */
    }
}

static inline __m128i sv_bsl(__m128i m, __m128i x, __m128i y) /* m ? x : y, bit by bit */
{
    return _mm_or_si128(_mm_and_si128(m, x), _mm_andnot_si128(m, y));
}

static inline __m128i sv_not(__m128i x)
{
    return _mm_xor_si128(x, _mm_set1_epi32(-1));
}

static inline __m128i sv_mask(const uint16_t *f) /* 0 / 1 flags -> 0 / all-ones */
{
    return _mm_sub_epi16(_mm_setzero_si128(), SV_LD(f));
}

static inline void sv_flag(uint16_t *f, __m128i m) /* mask -> 0 / 1 flags */
{
    SV_ST(f, _mm_srli_epi16(m, 15));
}

static inline __m128i sv_ltu(__m128i x, __m128i y) /* unsigned x < y */
{
    const __m128i k = _mm_set1_epi16((short)0x8000);
    return _mm_cmplt_epi16(_mm_xor_si128(x, k), _mm_xor_si128(y, k));
}

static inline __m128i sv_lo32(__m128i x) /* lanes 0..3 sign-extended to 32 bits */
{
    return _mm_srai_epi32(_mm_unpacklo_epi16(x, x), 16);
}

static inline __m128i sv_hi32(__m128i x) /* lanes 4..7 */
{
    return _mm_srai_epi32(_mm_unpackhi_epi16(x, x), 16);
}

/* clamp_u: 0 below 0, 0xFFFF above 0x7FFF. */
static inline __m128i sv_clamp_u(__m128i h0, __m128i h1)
{
    __m128i x = _mm_packs_epi32(h0, h1);
    __m128i k = _mm_set1_epi32(32767);
    __m128i gt = _mm_packs_epi32(_mm_cmpgt_epi32(h0, k), _mm_cmpgt_epi32(h1, k));
    return _mm_or_si128(_mm_andnot_si128(_mm_srai_epi16(x, 15), x), gt);
}

/* clamp_lo: acc[15:0], 0 when acc[47:16] < -32768, 0xFFFF when it is > 32767. */
static inline __m128i sv_clamp_lo(__m128i h0, __m128i h1, __m128i lo)
{
    __m128i kl = _mm_set1_epi32(-32768), kh = _mm_set1_epi32(32767);
    __m128i lt = _mm_packs_epi32(_mm_cmplt_epi32(h0, kl), _mm_cmplt_epi32(h1, kl));
    __m128i gt = _mm_packs_epi32(_mm_cmpgt_epi32(h0, kh), _mm_cmpgt_epi32(h1, kh));
    return _mm_andnot_si128(lt, _mm_or_si128(lo, gt));
}

/* sx * ux, sx signed and ux unsigned 16-bit: the 32-bit products' high halves (the low ones are _mm_mullo_epi16). */
static inline __m128i sv_mulhi_su(__m128i sx, __m128i ux)
{
    return _mm_add_epi16(_mm_mulhi_epi16(sx, ux), _mm_and_si128(sx, _mm_srai_epi16(ux, 15)));
}

/* acc += addend, given as its low 16 bits l16 and its bits 47..16 (h0, h1): ACC_ADD on 8 lanes. */
#define SV_ACC_ADD(l16, h0, h1)                                                                                        \
    do {                                                                                                               \
        __m128i nl_ = _mm_add_epi16(accl, (l16));                                                                      \
        __m128i c_ = sv_ltu(nl_, accl); /* the carry out of bit 15, as a mask */                                       \
        acch0 = _mm_sub_epi32(_mm_add_epi32(acch0, (h0)), _mm_unpacklo_epi16(c_, c_));                                 \
        acch1 = _mm_sub_epi32(_mm_add_epi32(acch1, (h1)), _mm_unpackhi_epi16(c_, c_));                                 \
        accl = nl_;                                                                                                    \
    } while (0)

#define SV_ACC_LOAD()                                                                                                  \
    do {                                                                                                               \
        acch0 = SV_LD(s->acch);                                                                                        \
        acch1 = SV_LD(s->acch + 4);                                                                                    \
        accl = SV_LD(s->accl);                                                                                         \
    } while (0)

/* The hot computational ops; returns 0 for the others, which the portable code runs. */
static RSP_INLINE int vu_op_sse2(n64_rsp *s, uint32_t op, int whole, unsigned e, const int16_t *pt,
                                 const int16_t *ps, int16_t *pd, unsigned vs)
{
    __m128i a = SV_LD(ps), t = sv_sel(pt, e, whole), r;
    __m128i acch0, acch1, accl, z = _mm_setzero_si128();
    switch (op & 63) {
    case 0x00: /* VMULF */
    case 0x01: /* VMULU */
    {
        __m128i lo = _mm_mullo_epi16(a, t), hi = _mm_mulhi_epi16(a, t), rnd = _mm_set1_epi32(0x4000);
        acch0 = _mm_srai_epi32(_mm_add_epi32(_mm_unpacklo_epi16(lo, hi), rnd), 15); /* (p + 0x4000) >> 15 */
        acch1 = _mm_srai_epi32(_mm_add_epi32(_mm_unpackhi_epi16(lo, hi), rnd), 15);
        accl = _mm_add_epi16(_mm_slli_epi16(lo, 1), _mm_set1_epi16((short)0x8000));
        r = (op & 1) ? sv_clamp_u(acch0, acch1) : _mm_packs_epi32(acch0, acch1);
        break;
    }
    case 0x04: /* VMUDL */
        accl = _mm_mulhi_epu16(a, t);
        acch0 = acch1 = z;
        r = accl;
        break;
    case 0x05: /* VMUDM: a * ub */
    case 0x06: /* VMUDN: ua * b */
    {
        __m128i hi = (op & 1) ? sv_mulhi_su(a, t) : sv_mulhi_su(t, a);
        accl = _mm_mullo_epi16(a, t);
        acch0 = sv_lo32(hi);
        acch1 = sv_hi32(hi);
        r = (op & 1) ? hi : accl;
        break;
    }
    case 0x07: /* VMUDH */
    {
        __m128i lo = _mm_mullo_epi16(a, t), hi = _mm_mulhi_epi16(a, t);
        acch0 = _mm_unpacklo_epi16(lo, hi);
        acch1 = _mm_unpackhi_epi16(lo, hi);
        accl = z;
        r = _mm_packs_epi32(acch0, acch1);
        break;
    }
    case 0x08: /* VMACF */
    case 0x09: /* VMACU */
    {
        __m128i lo = _mm_mullo_epi16(a, t), hi = _mm_mulhi_epi16(a, t);
        __m128i h0 = _mm_srai_epi32(_mm_unpacklo_epi16(lo, hi), 15), h1 = _mm_srai_epi32(_mm_unpackhi_epi16(lo, hi), 15);
        SV_ACC_LOAD();
        SV_ACC_ADD(_mm_slli_epi16(lo, 1), h0, h1);
        r = (op & 1) ? sv_clamp_u(acch0, acch1) : _mm_packs_epi32(acch0, acch1);
        break;
    }
    case 0x0C: /* VMADL */
        SV_ACC_LOAD();
        SV_ACC_ADD(_mm_mulhi_epu16(a, t), z, z);
        r = sv_clamp_lo(acch0, acch1, accl);
        break;
    case 0x0D: /* VMADM: a * ub */
    case 0x0E: /* VMADN: ua * b */
    {
        __m128i hi = (op & 1) ? sv_mulhi_su(a, t) : sv_mulhi_su(t, a);
        SV_ACC_LOAD();
        SV_ACC_ADD(_mm_mullo_epi16(a, t), sv_lo32(hi), sv_hi32(hi));
        r = (op & 1) ? _mm_packs_epi32(acch0, acch1) : sv_clamp_lo(acch0, acch1, accl);
        break;
    }
    case 0x0F: /* VMADH: ACCL unchanged */
    {
        __m128i lo = _mm_mullo_epi16(a, t), hi = _mm_mulhi_epi16(a, t);
        acch0 = _mm_add_epi32(SV_LD(s->acch), _mm_unpacklo_epi16(lo, hi));
        acch1 = _mm_add_epi32(SV_LD(s->acch + 4), _mm_unpackhi_epi16(lo, hi));
        SV_ST(s->acch, acch0);
        SV_ST(s->acch + 4, acch1);
        SV_ST(pd, _mm_packs_epi32(acch0, acch1));
        return 1;
    }
    case 0x10: /* VADD */
    case 0x11: /* VSUB */
    {
        __m128i c = SV_LD(s->vcol), v0, v1;
        if (op & 1) {
            v0 = _mm_sub_epi32(_mm_sub_epi32(sv_lo32(a), sv_lo32(t)), sv_lo32(c));
            v1 = _mm_sub_epi32(_mm_sub_epi32(sv_hi32(a), sv_hi32(t)), sv_hi32(c));
            SV_ST(s->accl, _mm_sub_epi16(_mm_sub_epi16(a, t), c));
        } else {
            v0 = _mm_add_epi32(_mm_add_epi32(sv_lo32(a), sv_lo32(t)), sv_lo32(c));
            v1 = _mm_add_epi32(_mm_add_epi32(sv_hi32(a), sv_hi32(t)), sv_hi32(c));
            SV_ST(s->accl, _mm_add_epi16(_mm_add_epi16(a, t), c));
        }
        SV_ST(s->vcol, z);
        SV_ST(s->vcoh, z);
        SV_ST(pd, _mm_packs_epi32(v0, v1));
        return 1;
    }
    case 0x14: /* VADDC */
    {
        __m128i sum = _mm_add_epi16(a, t);
        SV_ST(s->accl, sum);
        sv_flag(s->vcol, sv_ltu(sum, a));
        SV_ST(s->vcoh, z);
        SV_ST(pd, sum);
        return 1;
    }
    case 0x15: /* VSUBC */
    {
        __m128i d = _mm_sub_epi16(a, t);
        SV_ST(s->accl, d);
        sv_flag(s->vcol, sv_ltu(a, t));
        sv_flag(s->vcoh, sv_not(_mm_cmpeq_epi16(a, t)));
        SV_ST(pd, d);
        return 1;
    }
    case 0x20: /* VLT */
    case 0x21: /* VEQ */
    case 0x22: /* VNE */
    case 0x23: /* VGE */
    {
        __m128i eq = _mm_cmpeq_epi16(a, t), coh = sv_mask(s->vcoh), col = sv_mask(s->vcol), c;
        switch (op & 3) {
        case 0: c = _mm_or_si128(_mm_cmplt_epi16(a, t), _mm_and_si128(eq, _mm_and_si128(coh, col))); break;
        case 1: c = _mm_andnot_si128(coh, eq); break;
        case 2: c = _mm_or_si128(sv_not(eq), coh); break;
        default: c = _mm_or_si128(_mm_cmpgt_epi16(a, t), _mm_andnot_si128(_mm_and_si128(coh, col), eq)); break;
        }
        r = sv_bsl(c, a, t);
        SV_ST(s->accl, r);
        sv_flag(s->vccl, c);
        SV_ST(s->vcch, z);
        SV_ST(s->vcol, z);
        SV_ST(s->vcoh, z);
        SV_ST(pd, r);
        return 1;
    }
    case 0x24: /* VCL */
    {
        __m128i sum = _mm_add_epi16(a, t);
        __m128i zero = _mm_cmpeq_epi16(sum, z), nocy = sv_not(sv_ltu(sum, a));
        __m128i carry = sv_mask(s->vcol), ne = sv_mask(s->vcoh), ce = sv_mask(s->vce);
        __m128i lo_new = sv_bsl(ce, _mm_or_si128(zero, nocy), _mm_and_si128(zero, nocy));
        __m128i hi_new = sv_not(sv_ltu(a, t));
        __m128i ccl = sv_bsl(_mm_andnot_si128(ne, carry), lo_new, sv_mask(s->vccl));
        __m128i cch = sv_bsl(_mm_or_si128(carry, ne), sv_mask(s->vcch), hi_new);
        __m128i sel = sv_bsl(carry, ccl, cch);
        __m128i other = sv_bsl(carry, _mm_sub_epi16(z, t), t);
        __m128i lo = sv_bsl(sel, other, a);
        sv_flag(s->vccl, ccl);
        sv_flag(s->vcch, cch);
        SV_ST(s->accl, lo);
        SV_ST(s->vcol, z);
        SV_ST(s->vcoh, z);
        SV_ST(s->vce, z);
        SV_ST(pd, lo);
        return 1;
    }
    case 0x25: /* VCH: a + b (signs differ) or a - b (same signs) never overflows 16 bits */
    {
        __m128i dm = _mm_srai_epi16(_mm_xor_si128(a, t), 15), tnm = _mm_srai_epi16(t, 15);
        __m128i v = sv_bsl(dm, _mm_add_epi16(a, t), _mm_sub_epi16(a, t));
        __m128i lem = sv_not(_mm_cmpgt_epi16(v, z)), gem = sv_not(_mm_srai_epi16(v, 15));
        __m128i lo = sv_bsl(sv_bsl(dm, lem, gem), sv_bsl(dm, _mm_sub_epi16(z, t), t), a);
        __m128i ne = _mm_andnot_si128(_mm_cmpeq_epi16(a, sv_not(t)), sv_not(_mm_cmpeq_epi16(v, z)));
        sv_flag(s->vccl, sv_bsl(dm, lem, tnm));
        sv_flag(s->vcch, sv_bsl(dm, tnm, gem));
        sv_flag(s->vcol, dm);
        sv_flag(s->vcoh, ne);
        sv_flag(s->vce, _mm_and_si128(dm, _mm_cmpeq_epi16(v, _mm_set1_epi32(-1))));
        SV_ST(s->accl, lo);
        SV_ST(pd, lo);
        return 1;
    }
    case 0x27: /* VMRG */
        r = sv_bsl(sv_mask(s->vccl), a, t);
        SV_ST(s->vcol, z);
        SV_ST(s->vcoh, z);
        goto logic;
    case 0x28: r = _mm_and_si128(a, t); goto logic;         /* VAND */
    case 0x29: r = sv_not(_mm_and_si128(a, t)); goto logic; /* VNAND */
    case 0x2A: r = _mm_or_si128(a, t); goto logic;          /* VOR */
    case 0x2B: r = sv_not(_mm_or_si128(a, t)); goto logic;  /* VNOR */
    case 0x2C: r = _mm_xor_si128(a, t); goto logic;         /* VXOR */
    case 0x2D: r = sv_not(_mm_xor_si128(a, t)); goto logic; /* VNXOR */
    logic:
        SV_ST(s->accl, r);
        SV_ST(pd, r);
        return 1;
    case 0x30: /* VRCP */
    case 0x31: /* VRCPL */
    case 0x34: /* VRSQ */
    case 0x35: /* VRSQL */
    {
        int32_t input = (op & 1) && s->divdp ? (int32_t)((uint32_t)(uint16_t)s->divin << 16 | (uint16_t)pt[e & 7])
                                             : (int32_t)pt[e & 7];
        SV_ST(s->accl, t);
        vu_divide(s, (op & 4) != 0, input, &pd[vs & 7]);
        return 1;
    }
    case 0x32: /* VRCPH */
    case 0x36: /* VRSQH */
        SV_ST(s->accl, t);
        s->divdp = 1;
        s->divin = pt[e & 7];
        pd[vs & 7] = s->divout;
        return 1;
    case 0x37: /* VNOP */
    case 0x3F: /* VNULL */
        return 1;
    default:
        return 0;
    }
    SV_ST(s->acch, acch0);
    SV_ST(s->acch + 4, acch1);
    SV_ST(s->accl, accl);
    SV_ST(pd, r);
    return 1;
}
#endif /* RSP_SSE2 */

/* One computational op, op being its 6-bit opcode (and only that: the rest of the word is in e, the registers vt,
 * vs and vd as pt, ps and pd, and vs again for the ops that use it as an element number), whole
 * set when the element selector is 0 or 1 (b is vt as it is). Always inlined into the vu_fn_* below, each with
 * constant op and whole, so each is one case of the switches here with nothing left to dispatch at run time. */
static RSP_INLINE void vu_exec(n64_rsp *s, uint32_t op, int whole, unsigned e, const int16_t *pt, const int16_t *ps,
                               int16_t *pd, unsigned vs)
{
#if RSP_NEON
    if (vu_op_neon(s, op, whole, e, pt, ps, pd, vs))
        return;
#elif RSP_SSE2
    if (vu_op_sse2(s, op, whole, e, pt, ps, pd, vs))
        return;
#endif
    const int16_t *tv = pt;
    int16_t a[8], t[8], r[8];
    int32_t *acch = s->acch;
    uint16_t *accl = s->accl;
    uint16_t *vcol = s->vcol, *vcoh = s->vcoh, *vccl = s->vccl, *vcch = s->vcch, *vce = s->vce;
    memcpy(a, ps, sizeof a);
    if (whole || e < 2) {
        memcpy(t, tv, sizeof t);
    } else if (e >= 8) {
        int16_t x = tv[e & 7];
        for (int i = 0; i < 8; i++)
            t[i] = x;
    } else if (e >= 4) { /* 4..7: element e - 4 of each half */
        int16_t x0 = tv[e - 4], x1 = tv[e];
        for (int i = 0; i < 4; i++) {
            t[i] = x0;
            t[i + 4] = x1;
        }
    } else { /* 2, 3: element e - 2 of each pair */
        for (int i = 0; i < 8; i += 2)
            t[i] = t[i + 1] = tv[i + (e & 1)];
    }

    switch (op & 63) {
    case 0x00: /* VMULF: acc = a * b * 2 + 0x8000 */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            int32_t hm = (p + 0x4000) >> 15;
            acch[i] = hm;
            accl[i] = (uint16_t)(((uint32_t)p << 1) + 0x8000u);
            r[i] = sclamp(hm);
        }
        break;
    case 0x01: /* VMULU */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            int32_t hm = (p + 0x4000) >> 15;
            acch[i] = hm;
            accl[i] = (uint16_t)(((uint32_t)p << 1) + 0x8000u);
            r[i] = clamp_u(hm);
        }
        break;
    case 0x02: /* VRNDP: acc += b (or b << 16 when vs is odd) when acc >= 0 */
    case 0x0A: /* VRNDN: the same when acc < 0 */
    {
        int neg = (op & 63) == 0x0A, sh = vs & 1;
        for (int i = 0; i < 8; i++) {
            int c = neg ? acch[i] < 0 : acch[i] >= 0;
            int32_t h = sh ? t[i] : (t[i] >> 16);
            uint32_t l = sh ? 0 : (uint16_t)t[i];
            ACC_ADD(i, c ? h : 0, c ? l : 0);
            r[i] = sclamp(acch[i]);
        }
        break;
    }
    case 0x03: /* VMULQ: acc = (a * b, + 31 when negative) << 16 */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            p += (p >> 31) & 31;
            acch[i] = p;
            accl[i] = 0;
            r[i] = (int16_t)(sclamp(p >> 1) & ~15);
        }
        break;
    case 0x04: /* VMUDL: acc = (ua * ub) >> 16 */
        for (int i = 0; i < 8; i++) {
            uint16_t p = (uint16_t)(((uint32_t)(uint16_t)a[i] * (uint16_t)t[i]) >> 16);
            acch[i] = 0;
            accl[i] = p;
            r[i] = (int16_t)p;
        }
        break;
    case 0x05: /* VMUDM: acc = a * ub */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * (int32_t)(uint16_t)t[i];
            acch[i] = p >> 16;
            accl[i] = (uint16_t)p;
            r[i] = (int16_t)(p >> 16);
        }
        break;
    case 0x06: /* VMUDN: acc = ua * b */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)(uint16_t)a[i] * (int32_t)t[i];
            acch[i] = p >> 16;
            accl[i] = (uint16_t)p;
            r[i] = (int16_t)p;
        }
        break;
    case 0x07: /* VMUDH: acc = (a * b) << 16 */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            acch[i] = p;
            accl[i] = 0;
            r[i] = sclamp(p);
        }
        break;
    case 0x08: /* VMACF: acc += a * b * 2 */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            ACC_ADD(i, p >> 15, ((uint32_t)p << 1) & 0xFFFF);
            r[i] = sclamp(acch[i]);
        }
        break;
    case 0x09: /* VMACU */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            ACC_ADD(i, p >> 15, ((uint32_t)p << 1) & 0xFFFF);
            r[i] = clamp_u(acch[i]);
        }
        break;
    case 0x0B: /* VMACQ: rounds acc[47:16] towards bit 5 (MPEG), result (acc[47:17] clamped) & ~15 */
        for (int i = 0; i < 8; i++) {
            int32_t p = acch[i];
            int32_t adj = ((p >> 31) & 32) | (-(int32_t)(p >= 32) & -32);
            p += adj & -(int32_t)((p & 32) == 0);
            acch[i] = p;
            r[i] = (int16_t)(sclamp(p >> 1) & ~15);
        }
        break;
    case 0x0C: /* VMADL: acc += (ua * ub) >> 16 */
        for (int i = 0; i < 8; i++) {
            uint32_t p = ((uint32_t)(uint16_t)a[i] * (uint16_t)t[i]) >> 16;
            ACC_ADD(i, 0, p);
            r[i] = clamp_lo(acch[i], accl[i]);
        }
        break;
    case 0x0D: /* VMADM: acc += a * ub */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * (int32_t)(uint16_t)t[i];
            ACC_ADD(i, p >> 16, (uint32_t)p & 0xFFFF);
            r[i] = sclamp(acch[i]);
        }
        break;
    case 0x0E: /* VMADN: acc += ua * b */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)(uint16_t)a[i] * (int32_t)t[i];
            ACC_ADD(i, p >> 16, (uint32_t)p & 0xFFFF);
            r[i] = clamp_lo(acch[i], accl[i]);
        }
        break;
    case 0x0F: /* VMADH: acc += (a * b) << 16 */
        for (int i = 0; i < 8; i++) {
            int32_t p = (int32_t)a[i] * t[i];
            acch[i] = (int32_t)((uint32_t)acch[i] + (uint32_t)p);
            r[i] = sclamp(acch[i]);
        }
        break;
    case 0x10: /* VADD: a + b + carry, clamped; ACCL unclamped; VCO cleared */
        for (int i = 0; i < 8; i++) {
            int32_t v = (int32_t)a[i] + t[i] + vcol[i];
            accl[i] = (uint16_t)v;
            r[i] = sclamp(v);
            vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x11: /* VSUB: a - b - carry */
        for (int i = 0; i < 8; i++) {
            int32_t v = (int32_t)a[i] - t[i] - vcol[i];
            accl[i] = (uint16_t)v;
            r[i] = sclamp(v);
            vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x13: /* VABS: b with the sign of a (0 when a is 0); -(-32768) = 0x7FFF, ACCL 0x8000 */
        for (int i = 0; i < 8; i++) {
            int16_t neg = (int16_t)-(uint16_t)t[i];
            int16_t lo = a[i] < 0 ? neg : a[i] == 0 ? 0 : t[i];
            accl[i] = (uint16_t)lo;
            r[i] = (a[i] < 0 && t[i] == -32768) ? 32767 : lo;
        }
        break;
    case 0x14: /* VADDC: unsigned a + b, carry into VCO low */
        for (int i = 0; i < 8; i++) {
            uint32_t v = (uint32_t)(uint16_t)a[i] + (uint16_t)t[i];
            accl[i] = (uint16_t)v;
            r[i] = (int16_t)(uint16_t)v;
            vcol[i] = (uint16_t)(v >> 16);
            vcoh[i] = 0;
        }
        break;
    case 0x15: /* VSUBC: unsigned a - b, borrow into VCO low, not-equal into VCO high */
        for (int i = 0; i < 8; i++) {
            int32_t v = (int32_t)(uint16_t)a[i] - (int32_t)(uint16_t)t[i];
            accl[i] = (uint16_t)v;
            r[i] = (int16_t)(uint16_t)v;
            vcol[i] = v < 0;
            vcoh[i] = v != 0;
        }
        break;
    case 0x1D: /* VSAR: reads a slice of the accumulator, leaves it unchanged */
        if (e == 8)
            for (int i = 0; i < 8; i++)
                r[i] = (int16_t)(acch[i] >> 16);
        else if (e == 9)
            for (int i = 0; i < 8; i++)
                r[i] = (int16_t)acch[i];
        else if (e == 10)
            memcpy(r, accl, sizeof r);
        else
            memset(r, 0, sizeof r);
        break;
    case 0x20: /* VLT */
        for (int i = 0; i < 8; i++) {
            int c = (a[i] < t[i]) | ((a[i] == t[i]) & vcoh[i] & vcol[i]);
            r[i] = c ? a[i] : t[i];
            accl[i] = (uint16_t)r[i];
            vccl[i] = (uint16_t)c;
            vcch[i] = vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x21: /* VEQ */
        for (int i = 0; i < 8; i++) {
            int c = (a[i] == t[i]) & (vcoh[i] ^ 1);
            r[i] = c ? a[i] : t[i];
            accl[i] = (uint16_t)r[i];
            vccl[i] = (uint16_t)c;
            vcch[i] = vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x22: /* VNE */
        for (int i = 0; i < 8; i++) {
            int c = (a[i] != t[i]) | vcoh[i];
            r[i] = c ? a[i] : t[i];
            accl[i] = (uint16_t)r[i];
            vccl[i] = (uint16_t)c;
            vcch[i] = vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x23: /* VGE */
        for (int i = 0; i < 8; i++) {
            int c = (a[i] > t[i]) | ((a[i] == t[i]) & ((vcoh[i] & vcol[i]) ^ 1));
            r[i] = c ? a[i] : t[i];
            accl[i] = (uint16_t)r[i];
            vccl[i] = (uint16_t)c;
            vcch[i] = vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x24: /* VCL: the low half of a double-precision clip, after VCH */
        for (int i = 0; i < 8; i++) {
            int32_t us = (uint16_t)a[i], ut = (uint16_t)t[i], sum = us + ut;
            int32_t zero = (sum & 0xFFFF) == 0, nocy = sum <= 0xFFFF;
            int32_t carry = vcol[i], ne = vcoh[i], ce = vce[i];
            int32_t lo_new = ce ? (zero | nocy) : (zero & nocy);
            int32_t hi_new = us >= ut;
            int32_t ccl = (carry & (ne ^ 1)) ? lo_new : vccl[i];
            int32_t cch = ((carry | ne) ^ 1) ? hi_new : vcch[i];
            int32_t sel = carry ? ccl : cch;
            int32_t other = carry ? -ut : ut;
            int32_t lo = sel ? other : us;
            vccl[i] = (uint16_t)ccl;
            vcch[i] = (uint16_t)cch;
            accl[i] = (uint16_t)lo;
            r[i] = (int16_t)lo;
            vcol[i] = vcoh[i] = vce[i] = 0;
        }
        break;
    case 0x25: /* VCH: clip test, high half / single precision */
        for (int i = 0; i < 8; i++) {
            int32_t sv = a[i], tt = t[i];
            int32_t dm = (sv ^ tt) >> 31; /* -1: signs differ (compare against -b) */
            int32_t other = (tt ^ dm) - dm; /* -b or b */
            int32_t v = sv + ((tt ^ ~dm) + 1 + dm); /* a + b or a - b */
            int32_t lem = -(int32_t)(v <= 0), gem = -(int32_t)(v >= 0), tnm = tt >> 31;
            int32_t take = (lem & dm) | (gem & ~dm);
            int32_t lo = (other & take) | (sv & ~take);
            int32_t ne = (v != 0) & ((sv & 0xFFFF) != (~tt & 0xFFFF));
            vccl[i] = (uint16_t)(((lem & dm) | (tnm & ~dm)) & 1);
            vcch[i] = (uint16_t)(((tnm & dm) | (gem & ~dm)) & 1);
            vcol[i] = (uint16_t)(dm & 1);
            vcoh[i] = (uint16_t)ne;
            vce[i] = (uint16_t)(dm & (v == -1) & 1);
            accl[i] = (uint16_t)lo;
            r[i] = (int16_t)lo;
        }
        break;
    case 0x26: /* VCR: one's complement clip */
        for (int i = 0; i < 8; i++) {
            int32_t sv = a[i], tt = t[i];
            int32_t dm = (sv ^ tt) >> 31, tnm = tt >> 31;
            int32_t v = sv + (tt ^ ~dm); /* a + b (signs differ) or a - b - 1 */
            int32_t take = (-(int32_t)(v + 1 <= 0) & dm) | (-(int32_t)(v + 1 >= 0) & ~dm);
            int32_t other = tt ^ dm; /* ~b or b */
            int32_t lo = (other & take) | (sv & ~take);
            vccl[i] = (uint16_t)(((take & dm) | (tnm & ~dm)) & 1);
            vcch[i] = (uint16_t)(((tnm & dm) | (take & ~dm)) & 1);
            accl[i] = (uint16_t)lo;
            r[i] = (int16_t)lo;
            vcol[i] = vcoh[i] = vce[i] = 0;
        }
        break;
    case 0x27: /* VMRG: VCC low picks a */
        for (int i = 0; i < 8; i++) {
            r[i] = vccl[i] ? a[i] : t[i];
            accl[i] = (uint16_t)r[i];
            vcol[i] = vcoh[i] = 0;
        }
        break;
    case 0x28: /* VAND */
        for (int i = 0; i < 8; i++)
            accl[i] = (uint16_t)(r[i] = (int16_t)(a[i] & t[i]));
        break;
    case 0x29: /* VNAND */
        for (int i = 0; i < 8; i++)
            accl[i] = (uint16_t)(r[i] = (int16_t)~(a[i] & t[i]));
        break;
    case 0x2A: /* VOR */
        for (int i = 0; i < 8; i++)
            accl[i] = (uint16_t)(r[i] = (int16_t)(a[i] | t[i]));
        break;
    case 0x2B: /* VNOR */
        for (int i = 0; i < 8; i++)
            accl[i] = (uint16_t)(r[i] = (int16_t)~(a[i] | t[i]));
        break;
    case 0x2C: /* VXOR */
        for (int i = 0; i < 8; i++)
            accl[i] = (uint16_t)(r[i] = (int16_t)(a[i] ^ t[i]));
        break;
    case 0x2D: /* VNXOR */
        for (int i = 0; i < 8; i++)
            accl[i] = (uint16_t)(r[i] = (int16_t)~(a[i] ^ t[i]));
        break;
    case 0x30: /* VRCP */
    case 0x31: /* VRCPL */
    case 0x34: /* VRSQ */
    case 0x35: /* VRSQL */
    {
        int low = op & 1, sq = (op & 4) != 0;
        int32_t input = low && s->divdp ? (int32_t)((uint32_t)(uint16_t)s->divin << 16 | (uint16_t)tv[e & 7])
                                         : (int32_t)tv[e & 7];
        memcpy(accl, t, sizeof t);
        vu_divide(s, sq, input, &pd[vs & 7]);
        return;
    }
    case 0x32: /* VRCPH */
    case 0x36: /* VRSQH */
        memcpy(accl, t, sizeof t);
        s->divdp = 1;
        s->divin = tv[e & 7];
        pd[vs & 7] = s->divout;
        return;
    case 0x33: /* VMOV: vd[de] = (b with the selector)[de] */
        memcpy(accl, t, sizeof t);
        pd[vs & 7] = t[vs & 7];
        return;
    case 0x37: /* VNOP */
    case 0x3F: /* VNULL */
        return;
    default: /* reserved opcodes: the accumulator takes a + b, the result is 0 */
        for (int i = 0; i < 8; i++) {
            accl[i] = (uint16_t)(a[i] + t[i]);
            r[i] = 0;
        }
        break;
    }
    memcpy(pd, r, sizeof r);
}

/* The computational ops two functions each, named by the opcode in octal (vu_fn_57 is 057 = 0x2F): vu_fn_* for any
 * element selector, vu_fw_* for selectors 0 and 1 (the whole vector, no shuffle). n64_rsp_run calls them through
 * vu_fns (vu_fn_* at the opcode, vu_fw_* at 64 + the opcode). Out of the run loop: inlined there, the scalar path and
 * the vector bodies fight over registers and both get slower (measured). */
typedef void (*vu_fn)(n64_rsp *s, const rsp_dec *d);
#define RSP_VR(s, off) ((int16_t *)((uint8_t *)(s) + (off))) /* a vector register from its offset in n64_rsp */
#define VU_FN(hi, lo)                                                                                                  \
    static void vu_fn_##hi##lo(n64_rsp *s, const rsp_dec *d)                                                           \
    {                                                                                                                  \
        vu_exec(s, 0##hi##lo, 0, d->e, RSP_VR(s, d->imm >> 16), RSP_VR(s, d->imm & 0xFFFF), RSP_VR(s, d->link), d->a); \
    }                                                                                                                  \
    static void vu_fw_##hi##lo(n64_rsp *s, const rsp_dec *d)                                                           \
    {                                                                                                                  \
        vu_exec(s, 0##hi##lo, 1, d->e, RSP_VR(s, d->imm >> 16), RSP_VR(s, d->imm & 0xFFFF), RSP_VR(s, d->link), d->a); \
    }
#define VU_ROW(hi)                                                                                                     \
    VU_FN(hi, 0) VU_FN(hi, 1) VU_FN(hi, 2) VU_FN(hi, 3) VU_FN(hi, 4) VU_FN(hi, 5) VU_FN(hi, 6) VU_FN(hi, 7)
VU_ROW(0) VU_ROW(1) VU_ROW(2) VU_ROW(3) VU_ROW(4) VU_ROW(5) VU_ROW(6) VU_ROW(7)
#undef VU_ROW
#undef VU_FN
#define VU_ROW(f, hi) f##hi##0, f##hi##1, f##hi##2, f##hi##3, f##hi##4, f##hi##5, f##hi##6, f##hi##7
#define VU_ALL(f)                                                                                                      \
    VU_ROW(f, 0), VU_ROW(f, 1), VU_ROW(f, 2), VU_ROW(f, 3), VU_ROW(f, 4), VU_ROW(f, 5), VU_ROW(f, 6), VU_ROW(f, 7)
static const vu_fn vu_fns[128] = {VU_ALL(vu_fn_), VU_ALL(vu_fw_)};
#undef VU_ALL
#undef VU_ROW

/* ---- Vector unit: loads and stores (LWC2 / SWC2) ------------------------------------------------------------------ */

/* A register as its 16 big-endian bytes (byte 0 = high byte of element 0) and back: the byte-granular loads and
 * stores work on this view. */
static inline void vr_unpack(const int16_t *v, uint8_t *b)
{
    for (int i = 0; i < 8; i++) {
        b[2 * i] = (uint8_t)((uint16_t)v[i] >> 8);
        b[2 * i + 1] = (uint8_t)v[i];
    }
}

static inline void vr_pack(const uint8_t *b, int16_t *v)
{
    for (int i = 0; i < 8; i++)
        v[i] = (int16_t)(uint16_t)(b[2 * i] << 8 | b[2 * i + 1]);
}

/* Four or eight lanes from / to big-endian halfwords (fixed counts, so the portable loops vectorise too). */
static inline void ld_be16x8(int16_t *v, const uint8_t *p)
{
#if RSP_NEON
    vst1q_s16(v, vreinterpretq_s16_u8(vrev16q_u8(vld1q_u8(p))));
#elif RSP_SSE2
    __m128i x = SV_LD(p);
    SV_ST(v, _mm_or_si128(_mm_slli_epi16(x, 8), _mm_srli_epi16(x, 8)));
#else
    for (int k = 0; k < 8; k++)
        v[k] = (int16_t)n64_be16(p + 2 * k);
#endif
}

static inline void ld_be16x4(int16_t *v, const uint8_t *p)
{
#if RSP_NEON
    vst1_s16(v, vreinterpret_s16_u8(vrev16_u8(vld1_u8(p))));
#elif RSP_SSE2
    __m128i x = _mm_loadl_epi64((const __m128i *)(const void *)p);
    _mm_storel_epi64((__m128i *)(void *)v, _mm_or_si128(_mm_slli_epi16(x, 8), _mm_srli_epi16(x, 8)));
#else
    for (int k = 0; k < 4; k++)
        v[k] = (int16_t)n64_be16(p + 2 * k);
#endif
}

static inline void st_be16x8(uint8_t *p, const int16_t *v)
{
#if RSP_NEON
    vst1q_u8(p, vrev16q_u8(vreinterpretq_u8_s16(vld1q_s16(v))));
#elif RSP_SSE2
    __m128i x = SV_LD(v);
    SV_ST(p, _mm_or_si128(_mm_slli_epi16(x, 8), _mm_srli_epi16(x, 8)));
#else
    for (int k = 0; k < 8; k++)
        n64_wbe16(p + 2 * k, (uint16_t)v[k]);
#endif
}

static inline void st_be16x4(uint8_t *p, const int16_t *v)
{
#if RSP_NEON
    vst1_u8(p, vrev16_u8(vreinterpret_u8_s16(vld1_s16(v))));
#elif RSP_SSE2
    __m128i x = _mm_loadl_epi64((const __m128i *)(const void *)v);
    _mm_storel_epi64((__m128i *)(void *)p, _mm_or_si128(_mm_slli_epi16(x, 8), _mm_srli_epi16(x, 8)));
#else
    for (int k = 0; k < 4; k++)
        n64_wbe16(p + 2 * k, (uint16_t)v[k]);
#endif
}

/* n bytes of DMEM from addr (wrapping at 4 KB) into dst. */
static inline void dm_read(const n64_rsp *s, uint32_t addr, uint8_t *dst, unsigned n)
{
    addr &= 0xFFF;
    if (addr + n <= 0x1000) {
        memcpy(dst, s->mem + addr, n);
        return;
    }
    for (unsigned i = 0; i < n; i++)
        dst[i] = s->mem[(addr + i) & 0xFFF];
}

static inline void dm_write(n64_rsp *s, uint32_t addr, const uint8_t *src, unsigned n)
{
    addr &= 0xFFF;
    if (addr + n <= 0x1000) {
        memcpy(s->mem + addr, src, n);
        return;
    }
    for (unsigned i = 0; i < n; i++)
        s->mem[(addr + i) & 0xFFF] = src[i];
}

/* n bytes of DMEM from addr into register bytes e.. (e + n <= 16); whole halfwords when aligned in the register. */
static inline void ld_bytes(n64_rsp *s, int16_t *v, unsigned e, uint32_t addr, unsigned n)
{
    addr &= 0xFFF;
    if (!((e | n) & 1) && addr + n <= 0x1000) {
        const uint8_t *p = s->mem + addr;
        if (n == 16) { /* LQV / LRV of a whole aligned register: e = 0 */
            ld_be16x8(v, p);
            return;
        }
        if (n == 8) { /* LDV */
            ld_be16x4(v + e / 2, p);
            return;
        }
        for (unsigned k = 0; k < n / 2; k++)
            v[e / 2 + k] = (int16_t)n64_be16(p + 2 * k);
        return;
    }
    for (unsigned k = 0; k < n; k++)
        vsetb(v, e + k, s->mem[(addr + k) & 0xFFF]);
}

/* Register bytes e.. (wrapping at 16) to n bytes of DMEM from addr. */
static inline void st_bytes(n64_rsp *s, const int16_t *v, unsigned e, uint32_t addr, unsigned n)
{
    addr &= 0xFFF;
    if (!((e | n) & 1) && addr + n <= 0x1000) {
        uint8_t *p = s->mem + addr;
        if (n == 16 && e == 0) { /* SQV of an aligned register */
            st_be16x8(p, v);
            return;
        }
        if (n == 8 && e <= 8) { /* SDV */
            st_be16x4(p, v + e / 2);
            return;
        }
        for (unsigned k = 0; k < n / 2; k++)
            n64_wbe16(p + 2 * k, (uint16_t)v[(e / 2 + k) & 7]);
        return;
    }
    for (unsigned k = 0; k < n; k++)
        s->mem[(addr + k) & 0xFFF] = vgetb(v, (e + k) & 15);
}

static RSP_NOINLINE void vu_load(n64_rsp *s, uint32_t op)
{
    unsigned vt = (op >> 16) & 31, f = (op >> 11) & 31, e = (op >> 7) & 15;
    int32_t off = (int32_t)(op << 25) >> 25;
    uint32_t base = s->r[(op >> 21) & 31];
    int16_t *v = s->vr[vt];
    uint8_t vb[16];
    uint32_t addr;
    switch (f) {
    case 0x00: /* LBV */
    case 0x01: /* LSV */
    case 0x02: /* LLV */
    case 0x03: /* LDV: 1 << f bytes into bytes e.., none past byte 15 */
    {
        unsigned n = 1u << f;
        addr = base + (uint32_t)(off * (int32_t)n);
        ld_bytes(s, v, e, addr, e + n > 16 ? 16 - e : n);
        break;
    }
    case 0x04: /* LQV: bytes e.. up to the 16-byte boundary of the address */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned n = 16 - (addr & 15);
        ld_bytes(s, v, e, addr, n > 16 - e ? 16 - e : n);
        break;
    }
    case 0x05: /* LRV: the bytes before the address in its 16-byte block, into the end of the register */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned start = 16 - ((addr & 15) - e); /* (addr & 15) < e: start > 16, nothing loaded */
        if (start < 16)
            ld_bytes(s, v, start, addr & ~15u, 16 - start);
        break;
    }
    case 0x06: /* LPV: 8 bytes << 8 into the lanes, from a 16-byte window */
    case 0x07: /* LUV: << 7 */
    {
        addr = base + (uint32_t)(off * 8);
        unsigned idx = (addr & 7) - e;
        unsigned sh = f == 6 ? 8 : 7;
        uint8_t w[16];
        dm_read(s, addr & ~7u, w, 16);
        for (unsigned i = 0; i < 8; i++)
            v[i] = (int16_t)(uint16_t)(w[(idx + i) & 15] << sh);
        break;
    }
    case 0x08: /* LHV: every other byte << 7 */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned idx = (addr & 7) - e;
        uint8_t w[16];
        dm_read(s, addr & ~7u, w, 16);
        for (unsigned i = 0; i < 8; i++)
            v[i] = (int16_t)(uint16_t)(w[(idx + i * 2) & 15] << 7);
        break;
    }
    case 0x09: /* LFV: every fourth byte << 7 into four lanes (bytes e .. e + 7 of the register) */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned idx = (addr & 7) - e;
        uint8_t w[16], tb[16];
        int16_t tmp[8];
        dm_read(s, addr & ~7u, w, 16);
        for (unsigned i = 0; i < 4; i++) {
            tmp[i] = (int16_t)(uint16_t)(w[(idx + i * 4) & 15] << 7);
            tmp[i + 4] = (int16_t)(uint16_t)(w[(idx + i * 4 + 8) & 15] << 7);
        }
        vr_unpack(tmp, tb);
        vr_unpack(v, vb);
        unsigned end = e + 8 > 16 ? 16 : e + 8;
        for (unsigned b = e; b < end; b++)
            vb[b] = tb[b];
        vr_pack(vb, v);
        break;
    }
    case 0x0B: /* LTV: element i of register (vt & ~7) + ((e / 2 + i) & 7) from halfword i of the 16-byte block */
    {
        addr = base + (uint32_t)(off * 16);
        uint32_t begin = addr & ~7u;
        unsigned pos = (e + (addr & 8)) & 15, vbase = vt & ~7u, voff = e >> 1;
        uint8_t w[16];
        dm_read(s, begin, w, 16);
        for (unsigned i = 0; i < 8; i++) {
            int16_t *dst = s->vr[vbase + voff];
            dst[i] = (int16_t)(uint16_t)(w[pos] << 8 | w[(pos + 1) & 15]);
            pos = (pos + 2) & 15;
            voff = (voff + 1) & 7;
        }
        break;
    }
    default: /* LWV and the reserved ones do nothing */
        break;
    }
}

static RSP_NOINLINE void vu_store(n64_rsp *s, uint32_t op)
{
    unsigned vt = (op >> 16) & 31, f = (op >> 11) & 31, e = (op >> 7) & 15;
    int32_t off = (int32_t)(op << 25) >> 25;
    uint32_t base = s->r[(op >> 21) & 31];
    const int16_t *v = s->vr[vt];
    uint8_t w[16];
    uint32_t addr;
    switch (f) {
    case 0x00: /* SBV */
    case 0x01: /* SSV */
    case 0x02: /* SLV */
    case 0x03: /* SDV: 1 << f bytes from bytes e.. (wrapping in the register) */
    {
        unsigned n = 1u << f;
        addr = base + (uint32_t)(off * (int32_t)n);
        st_bytes(s, v, e, addr, n);
        break;
    }
    case 0x04: /* SQV: bytes e.. (wrapping) up to the 16-byte boundary of the address */
        addr = base + (uint32_t)(off * 16);
        st_bytes(s, v, e, addr, 16 - (addr & 15));
        break;
    case 0x05: /* SRV: the bytes before the address in its 16-byte block */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned n = addr & 15;
        st_bytes(s, v, (e + 16 - n) & 15, addr & ~15u, n);
        break;
    }
    case 0x06: /* SPV: high bytes of the lanes (byte index < 8), else lane >> 7 */
    case 0x07: /* SUV: the reverse */
    {
        addr = base + (uint32_t)(off * 8);
        for (unsigned i = 0; i < 8; i++) {
            unsigned b = e + i;
            int packed = ((b & 15) < 8) == (f == 6);
            w[i] = packed ? (uint8_t)((uint16_t)v[b & 7] >> 8) : (uint8_t)((uint16_t)v[b & 7] >> 7);
        }
        dm_write(s, addr, w, 8);
        break;
    }
    case 0x08: /* SHV: lane bits 14..7 to every other byte of a 16-byte window */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned idx = addr & 7;
        addr &= ~7u;
        for (unsigned i = 0; i < 8; i++) {
            unsigned b = e + i * 2;
            DM(addr + ((idx + i * 2) & 15)) = (uint8_t)(vgetb(v, b & 15) << 1 | vgetb(v, (b + 1) & 15) >> 7);
        }
        break;
    }
    case 0x09: /* SFV: four lanes >> 7 to every fourth byte; only some elements select lanes, the others store 0 */
    {
        static const int8_t lanes[16][4] = {
            {0, 1, 2, 3}, {6, 7, 4, 5}, {-1}, {-1}, {1, 2, 3, 0}, {7, 4, 5, 6}, {-1}, {-1},
            {4, 5, 6, 7}, {-1}, {-1}, {3, 0, 1, 2}, {5, 6, 7, 4}, {-1}, {-1}, {0, 1, 2, 3},
        };
        addr = base + (uint32_t)(off * 16);
        unsigned bs = addr & 7;
        addr &= ~7u;
        for (unsigned i = 0; i < 4; i++) {
            int l = lanes[e][0] < 0 ? -1 : lanes[e][i];
            DM(addr + ((bs + i * 4) & 15)) = l < 0 ? 0 : (uint8_t)((uint16_t)v[l] >> 7);
        }
        break;
    }
    case 0x0A: /* SWV: bytes e.. (wrapping) to the 16-byte window, starting at the address' offset in it */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned bs = addr & 7;
        addr &= ~7u;
        for (unsigned i = 0; i < 16; i++)
            DM(addr + ((bs + i) & 15)) = vgetb(v, (e + i) & 15);
        break;
    }
    case 0x0B: /* STV: element (e / 2 + k) & 7 of register (vt & ~7) + k to halfword k (a transpose) */
    {
        addr = base + (uint32_t)(off * 16);
        unsigned start = vt & ~7u, el = 16 - (e & ~1u), bs = (addr & 7) - (e & ~1u);
        addr &= ~7u;
        for (unsigned k = start; k < start + 8; k++) {
            DM(addr + (bs++ & 15)) = vgetb(s->vr[k], el++ & 15);
            DM(addr + (bs++ & 15)) = vgetb(s->vr[k], el++ & 15);
        }
        break;
    }
    default:
        break;
    }
}

/* ---- COP2 moves --------------------------------------------------------------------------------------------------- */

static uint32_t flags_get(const uint16_t *lo, const uint16_t *hi)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint32_t)(lo[i] & 1) << i | (hi ? (uint32_t)(hi[i] & 1) << (8 + i) : 0);
    return v;
}

static void flags_set(uint16_t *lo, uint16_t *hi, uint32_t v)
{
    for (int i = 0; i < 8; i++) {
        lo[i] = (uint16_t)(v >> i & 1);
        if (hi)
            hi[i] = (uint16_t)(v >> (8 + i) & 1);
    }
}

static RSP_NOINLINE void cop2_move(n64_rsp *s, uint32_t op)
{
    unsigned rt = (op >> 16) & 31, rd = (op >> 11) & 31, e = (op >> 7) & 15;
    switch ((op >> 21) & 31) {
    case 0: /* MFC2: bytes e and e + 1 (wrapping) of vs, sign-extended */
        s->r[rt] = (uint32_t)(int32_t)(int16_t)(uint16_t)(vgetb(s->vr[rd], e) << 8 | vgetb(s->vr[rd], (e + 1) & 15));
        break;
    case 2: /* CFC2 */
        switch (rd & 3) {
        case 0: s->r[rt] = (uint32_t)(int32_t)(int16_t)flags_get(s->vcol, s->vcoh); break;
        case 1: s->r[rt] = (uint32_t)(int32_t)(int16_t)flags_get(s->vccl, s->vcch); break;
        default: s->r[rt] = flags_get(s->vce, NULL); break;
        }
        break;
    case 4: /* MTC2: byte e and (unless e is 15) byte e + 1 */
        vsetb(s->vr[rd], e, (uint8_t)(s->r[rt] >> 8));
        if (e != 15)
            vsetb(s->vr[rd], e + 1, (uint8_t)s->r[rt]);
        break;
    case 6: /* CTC2 */
        switch (rd & 3) {
        case 0: flags_set(s->vcol, s->vcoh, s->r[rt]); break;
        case 1: flags_set(s->vccl, s->vcch, s->r[rt]); break;
        default: flags_set(s->vce, NULL, s->r[rt]); break;
        }
        break;
    default:
        break;
    }
}

/* ---- Scalar unit -------------------------------------------------------------------------------------------------- */

static inline uint32_t ld32(const n64_rsp *s, uint32_t a)
{
    a &= 0xFFF;
    if (a <= 0xFFC)
        return n64_be32(s->mem + a);
    return (uint32_t)DM(a) << 24 | (uint32_t)DM(a + 1) << 16 | (uint32_t)DM(a + 2) << 8 | DM(a + 3);
}

static inline uint16_t ld16(const n64_rsp *s, uint32_t a)
{
    return (uint16_t)(DM(a) << 8 | DM(a + 1));
}

static inline void st32(n64_rsp *s, uint32_t a, uint32_t v)
{
    a &= 0xFFF;
    if (a <= 0xFFC) {
        n64_wbe32(s->mem + a, v);
        return;
    }
    DM(a) = (uint8_t)(v >> 24);
    DM(a + 1) = (uint8_t)(v >> 16);
    DM(a + 2) = (uint8_t)(v >> 8);
    DM(a + 3) = (uint8_t)v;
}

/* ---- Predecode and run loop -------------------------------------------------------------------------------------- */

enum {
    RK_NOP = 0, /* calloc'ed tables rely on it being 0 */
    RK_SLL, RK_SRL, RK_SRA, RK_SLLV, RK_SRLV, RK_SRAV, RK_SRLV_RS, RK_JR, RK_JALR, RK_BREAK,
    RK_ADD, RK_SUB, RK_AND, RK_OR, RK_XOR, RK_NOR, RK_SLT, RK_SLTU,
    RK_BLTZ, RK_BGEZ, RK_BLTZAL, RK_BGEZAL, RK_J, RK_JAL, RK_BEQ, RK_BNE, RK_BLEZ, RK_BGTZ,
    RK_ADDI, RK_SLTI, RK_SLTIU, RK_ANDI, RK_ORI, RK_XORI,
    RK_MFC0, RK_MTC0, RK_COP2M,
    RK_LB, RK_LH, RK_LW, RK_LBU, RK_LHU, RK_SB, RK_SH, RK_SW, RK_LWC2, RK_SWC2,
    RK_LDV, RK_LQV, RK_SDV, RK_SQV, /* their common aligned case inline, the rest through vu_load / vu_store */
    RK_VU, /* COP2 computational: vu_fns[vop] */
    RK_IDLE,  /* a backward branch closing a loop that only reads (rsp_idle); e = the branch's own RK_* */
    RK_CDOWN, /* BGTZ / BNEZ rs to itself with ADDI rs, rs, -1 in its delay slot (a delay loop); e = RK_BGTZ / RK_BNE */
    RK_COUNT
};

/* Decodes the IMEM word raw (as loaded, see rsp_dec) at offset ia into x. Same semantics as the hardware notes in the
 * run loop below; unknown opcodes are no-ops. */
static void rsp_decode_word(uint32_t ia, uint32_t raw, rsp_dec *out)
{
    uint32_t op = n64_be32((const uint8_t *)&raw);
    uint32_t imm = (uint32_t)(int32_t)(int16_t)op;
    rsp_dec x;
    memset(&x, 0, sizeof x);
    x.word = raw;
    x.a = (uint8_t)((op >> 21) & 31);
    x.b = (uint8_t)((op >> 16) & 31);
    x.c = (uint8_t)((op >> 11) & 31);
    x.imm = imm;
    x.link = (uint16_t)((ia + 8) & 0xFFC);
    uint32_t btarget = (ia + 4 + (imm << 2)) & 0xFFC;
    int k = RK_NOP;
    switch (op >> 26) {
    case 0x00: /* SPECIAL */
        x.imm = (op >> 6) & 31;
        switch (op & 63) {
        case 0x00: k = RK_SLL; break;
        case 0x02: k = RK_SRL; break;
        case 0x03: k = RK_SRA; break;
        case 0x04: k = RK_SLLV; break;
        case 0x06: k = RK_SRLV; break;
        case 0x07: k = RK_SRAV; break;
        case 0x08: k = RK_JR; break;
        case 0x09: k = RK_JALR; break;
        case 0x0D: k = RK_BREAK; break;
        case 0x20:
        case 0x21: k = RK_ADD; break;
        case 0x22:
        case 0x23: k = RK_SUB; break;
        case 0x24: k = RK_AND; break;
        case 0x25: k = RK_OR; break;
        case 0x26: k = RK_XOR; break;
        case 0x27: k = RK_NOR; break;
        case 0x2A: k = RK_SLT; break;
        case 0x2B: k = RK_SLTU; break;
        /* Every other SPECIAL minor opcode behaves as SRLV rd, rs, rs (rt ignored), not a no-op
         * (sauraen/rsp_inval_instrs, measured on hardware). */
        default: k = RK_SRLV_RS; break;
        }
        if (k == RK_SLL && op == 0)
            k = RK_NOP; /* the usual NOP */
        break;
    case 0x01: /* REGIMM: bit 0 = BGEZ / BLTZ, bit 4 = link; the others no-ops */
    {
        unsigned rt = x.b;
        x.imm = btarget;
        if (!(rt & 0x0E))
            k = (rt & 0x10) ? ((rt & 1) ? RK_BGEZAL : RK_BLTZAL) : ((rt & 1) ? RK_BGEZ : RK_BLTZ);
        break;
    }
    case 0x02: k = RK_J; x.imm = (op << 2) & 0xFFC; break;
    case 0x03: k = RK_JAL; x.imm = (op << 2) & 0xFFC; break;
    case 0x04: k = RK_BEQ; x.imm = btarget; break;
    case 0x05: k = RK_BNE; x.imm = btarget; break;
    case 0x06: k = RK_BLEZ; x.imm = btarget; break;
    case 0x07: k = RK_BGTZ; x.imm = btarget; break;
    case 0x08:
    case 0x09: k = RK_ADDI; break;
    case 0x0A: k = RK_SLTI; break;
    case 0x0B: k = RK_SLTIU; break;
    case 0x0C: k = RK_ANDI; x.imm = imm & 0xFFFF; break;
    case 0x0D: k = RK_ORI; x.imm = imm & 0xFFFF; break;
    case 0x0E: k = RK_XORI; x.imm = imm & 0xFFFF; break;
    case 0x0F: k = RK_ORI; x.a = 0; x.imm = imm << 16; break; /* LUI = ORI rt, $0, imm << 16 */
    case 0x10: /* COP0 */
        if (x.a == 0)
            k = RK_MFC0;
        else if (x.a == 4)
            k = RK_MTC0;
        x.c &= 15;
        break;
    case 0x12: /* COP2 */
        if (op & (1u << 25)) {
            k = RK_VU;
            x.e = (uint8_t)((op >> 21) & 15);
            x.b = (uint8_t)((op >> 16) & 31);
            x.a = (uint8_t)((op >> 11) & 31);
            x.c = (uint8_t)((op >> 6) & 31);
            x.vop = (uint8_t)((op & 63) | (x.e < 2 ? 64 : 0));
            x.imm = (uint32_t)(offsetof(n64_rsp, vr) + sizeof(int16_t[8]) * x.b) << 16 |
                    (uint32_t)(offsetof(n64_rsp, vr) + sizeof(int16_t[8]) * x.a);
            x.link = (uint16_t)(offsetof(n64_rsp, vr) + sizeof(int16_t[8]) * x.c);
        } else {
            k = RK_COP2M;
            x.imm = op;
        }
        break;
    case 0x20: k = RK_LB; break;
    case 0x21: k = RK_LH; break;
    case 0x23: /* LW */
    case 0x27: k = RK_LW; break; /* LWU */
    case 0x24: k = RK_LBU; break;
    case 0x25: k = RK_LHU; break;
    case 0x28: k = RK_SB; break;
    case 0x29: k = RK_SH; break;
    case 0x2B: k = RK_SW; break;
    case 0x32: /* LWC2 */
    case 0x3A: /* SWC2 */
    {
        int st = (op >> 26) == 0x3A;
        unsigned f = (op >> 11) & 31;
        int32_t off = (int32_t)(op << 25) >> 25;
        x.e = (uint8_t)((op >> 7) & 15);
        if (f == 3 && !(x.e & 1) && x.e <= 8) { /* LDV / SDV of whole halfwords: x.imm the byte offset */
            k = st ? RK_SDV : RK_LDV;
            x.imm = (uint32_t)(off * 8);
        } else if (f == 4) { /* LQV / SQV */
            k = st ? RK_SQV : RK_LQV;
            x.imm = (uint32_t)(off * 16);
        } else {
            k = st ? RK_SWC2 : RK_LWC2;
            x.imm = op;
        }
        break;
    }
    default: break;
    }
    switch (k) { /* results that go to $0: nothing (n64_rsp_run never writes r[0]) */
    case RK_SLL: case RK_SRL: case RK_SRA: case RK_SLLV: case RK_SRLV: case RK_SRAV: case RK_SRLV_RS:
    case RK_ADD: case RK_SUB: case RK_AND: case RK_OR: case RK_XOR: case RK_NOR: case RK_SLT: case RK_SLTU:
        if (x.c == 0)
            k = RK_NOP;
        break;
    case RK_JALR:
        if (x.c == 0)
            k = RK_JR;
        break;
    case RK_ADDI: case RK_SLTI: case RK_SLTIU: case RK_ANDI: case RK_ORI: case RK_XORI:
    case RK_LB: case RK_LH: case RK_LW: case RK_LBU: case RK_LHU:
        if (x.b == 0)
            k = RK_NOP;
        break;
    default:
        break;
    }
    x.kind = (uint8_t)k;
    *out = x;
}

/* An instruction an idle loop may hold (rsp_idle): it writes no memory, no vector state and no COP0 register, does
 * not branch, and reads nothing that changes while the RSP runs alone: scalar ALU ops, DMEM loads, MFC0 of any
 * register but SP_SEMAPHORE (whose read sets it). The RDP's registers are plain fields its front end changes only
 * when n64_rdp_run runs, which an idle loop (no MTC0) never calls. */
static int rsp_pure(const rsp_dec *x)
{
    switch (x->kind) {
    case RK_NOP: case RK_SLL: case RK_SRL: case RK_SRA: case RK_SLLV: case RK_SRLV: case RK_SRAV: case RK_SRLV_RS:
    case RK_ADD: case RK_SUB: case RK_AND: case RK_OR: case RK_XOR: case RK_NOR: case RK_SLT: case RK_SLTU:
    case RK_ADDI: case RK_SLTI: case RK_SLTIU: case RK_ANDI: case RK_ORI: case RK_XORI:
    case RK_LB: case RK_LH: case RK_LW: case RK_LBU: case RK_LHU:
        return 1;
    case RK_MFC0:
        return x->c != 7;
    default:
        return 0;
    }
}

/* The body of the loop the branch at b closes, from its target to its delay slot, holds only rsp_pure instructions
 * (the branch itself aside), as IMEM holds them now. */
static int rsp_idle_body(const n64_rsp *s, uint32_t b, uint32_t target)
{
    for (uint32_t a = target; a <= b + 4; a += 4) {
        if (a == b)
            continue;
        uint32_t raw;
        rsp_dec x;
        memcpy(&raw, s->mem + 0x1000 + a, 4);
        rsp_decode_word(a, raw, &x);
        if (!rsp_pure(&x))
            return 0;
    }
    return 1;
}

#define RSP_IDLE_MAX 16 /* instructions in an idle loop's body, the branch and its delay slot included */

/* Decodes the IMEM word raw at offset ia into its slot. A conditional branch or J back to at most RSP_IDLE_MAX - 2
 * words before it becomes RK_IDLE when its loop body is pure now (rsp_idle checks again before it skips anything), or
 * RK_CDOWN for the delay loop "BGTZ / BNEZ rs, ." with "ADDI rs, rs, -1" in the delay slot (checked again when it
 * runs). */
static RSP_NOINLINE const rsp_dec *rsp_decode(n64_rsp *s, uint32_t ia, uint32_t raw)
{
    rsp_dec *d = &s->dec[ia >> 2];
    rsp_decode_word(ia, raw, d);
    switch (d->kind) {
    case RK_BEQ: case RK_BNE: case RK_BLEZ: case RK_BGTZ: case RK_BLTZ: case RK_BGEZ: case RK_J:
        if (d->imm <= ia && ia - d->imm <= 4 * (RSP_IDLE_MAX - 2) && ia < 0xFFC) {
            uint32_t slot = n64_be32(s->mem + 0x1000 + ia + 4);
            if (d->imm == ia && (d->kind == RK_BGTZ || (d->kind == RK_BNE && d->b == 0)) && d->a != 0 &&
                (slot & ~(1u << 26)) == (0x20000000u | (uint32_t)d->a << 21 | (uint32_t)d->a << 16 | 0xFFFFu)) {
                d->e = d->kind;
                d->kind = RK_CDOWN;
            } else if (rsp_idle_body(s, ia, d->imm)) {
                d->e = d->kind;
                d->kind = RK_IDLE;
            }
        }
        break;
    default:
        break;
    }
    return d;
}

/* An idle loop (RK_IDLE) was just taken at b: pc / npc are its delay slot / its target. The first time, the scalar
 * registers are noted; the next time this branch is taken in the same n64_rsp_run with the same registers, the RSP
 * is in a cycle: the body reads only what it cannot change itself (rsp_pure) and writes only registers, so every
 * later iteration runs the same instructions with the same values. Whole iterations up to the cycle budget are then
 * counted as run (n), the state being exactly what running them would leave; the interpreter runs the rest. */
static RSP_NOINLINE uint32_t rsp_idle(n64_rsp *s, uint32_t b, uint32_t target, uint32_t n, uint32_t cycles)
{
    if (s->idle_b == b && memcmp(s->idle_r, s->r, sizeof s->r) == 0) {
        s->idle_b = ~0u;
        if (rsp_idle_body(s, b, target)) {
            uint32_t p = n - s->idle_n;
            n += (cycles - n) / p * p;
        }
        return n;
    }
    s->idle_b = b;
    s->idle_n = n;
    memcpy(s->idle_r, s->r, sizeof s->r);
    return n;
}

/* GCC and clang: each handler ends in its own fetch and indirect jump (labels as values), so the host predicts each
 * jump from the instruction before it; elsewhere one switch in a loop. */
#if defined(__GNUC__) || defined(__clang__)
#define RSP_THREADED 1
#else
#define RSP_THREADED 0
#endif

/* Runs up to cycles instructions; stops early once an instruction may have halted the RSP (BREAK, an SP_STATUS write),
 * halting it then when single-step is on. pc / npc live in locals: nothing the loop calls reads them (COP0 cannot
 * reach SP_PC). Each instruction runs from its predecoded entry (rsp_dec), decoded again first when IMEM no longer
 * holds the word it was made from. No instruction writes r[0] here: the decoder makes the ones that would NOPs
 * (their other effects are none), MFC0 and the COP2 moves clear it again. */
static uint32_t rsp_loop(n64_rsp *s, uint32_t cycles)
{
    uint32_t n = 0, pc = s->pc, npc = s->npc;
    uint32_t *r = s->r;
    const uint8_t *imem = s->mem + 0x1000;
    const rsp_dec *dec = s->dec;
    const rsp_dec *d;
    s->idle_b = ~0u;
#define FETCH()                                                                                                        \
    do {                                                                                                               \
        if (RSP_UNLIKELY(n >= cycles))                                                                                 \
            goto out;                                                                                                  \
        uint32_t raw_;                                                                                                 \
        memcpy(&raw_, imem + pc, 4);                                                                                   \
        d = (const rsp_dec *)((const uint8_t *)dec + pc * (sizeof *dec / 4)); /* dec[pc / 4] */                        \
        if (RSP_UNLIKELY(d->word != raw_))                                                                             \
            d = rsp_decode(s, pc, raw_);                                                                               \
        pc = npc;                                                                                                      \
        npc = (npc + 4) & 0xFFC;                                                                                       \
        n++;                                                                                                           \
    } while (0)
#define RS r[d->a]
#define RT r[d->b]
#define IMM (d->imm)
#if RSP_THREADED
#define K(k) [RK_##k] = &&L_##k
    static const void *const labels[] = {
        K(NOP), K(SLL), K(SRL), K(SRA), K(SLLV), K(SRLV), K(SRAV), K(SRLV_RS), K(JR), K(JALR), K(BREAK),
        K(ADD), K(SUB), K(AND), K(OR), K(XOR), K(NOR), K(SLT), K(SLTU),
        K(BLTZ), K(BGEZ), K(BLTZAL), K(BGEZAL), K(J), K(JAL), K(BEQ), K(BNE), K(BLEZ), K(BGTZ),
        K(ADDI), K(SLTI), K(SLTIU), K(ANDI), K(ORI), K(XORI),
        K(MFC0), K(MTC0), K(COP2M),
        K(LB), K(LH), K(LW), K(LBU), K(LHU), K(SB), K(SH), K(SW), K(LWC2), K(SWC2),
        K(LDV), K(LQV), K(SDV), K(SQV),
        K(VU), K(IDLE), K(CDOWN),
    };
#undef K
    _Static_assert(sizeof labels / sizeof labels[0] == RK_COUNT, "a handler per RK_*");
#define CASE(k) L_##k:
#define NEXT                                                                                                           \
    do {                                                                                                               \
        FETCH();                                                                                                       \
        goto *labels[d->kind];                                                                                         \
    } while (0)
    NEXT;
#else
#define CASE(k) case RK_##k:
#define NEXT continue
    for (;;) {
        FETCH();
        switch (d->kind) {
#endif
    CASE(VU) vu_fns[d->vop](s, d); NEXT; /* most of a microcode */
    CASE(NOP) NEXT;
    CASE(SLL) r[d->c] = RT << IMM; NEXT;
    CASE(SRL) r[d->c] = RT >> IMM; NEXT;
    CASE(SRA) r[d->c] = (uint32_t)((int32_t)RT >> IMM); NEXT;
    CASE(SLLV) r[d->c] = RT << (RS & 31); NEXT;
    CASE(SRLV) r[d->c] = RT >> (RS & 31); NEXT;
    CASE(SRAV) r[d->c] = (uint32_t)((int32_t)RT >> (RS & 31)); NEXT;
    CASE(SRLV_RS) r[d->c] = RS >> (RS & 31); NEXT;
    CASE(JR) npc = RS & 0xFFC; NEXT;
    CASE(JALR) /* the target before the link (rd may be rs) */
    {
        uint32_t tgt = RS & 0xFFC;
        r[d->c] = d->link;
        npc = tgt;
        NEXT;
    }
    CASE(BREAK)
        s->status |= ST_HALT | ST_BROKE;
        if (s->status & ST_INTR_BREAK)
            raise_sp(s);
        goto stop;
    CASE(ADD) r[d->c] = RS + RT; NEXT;
    CASE(SUB) r[d->c] = RS - RT; NEXT;
    CASE(AND) r[d->c] = RS & RT; NEXT;
    CASE(OR) r[d->c] = RS | RT; NEXT;
    CASE(XOR) r[d->c] = RS ^ RT; NEXT;
    CASE(NOR) r[d->c] = ~(RS | RT); NEXT;
    CASE(SLT) r[d->c] = (int32_t)RS < (int32_t)RT; NEXT;
    CASE(SLTU) r[d->c] = RS < RT; NEXT;
    /* REGIMM: the condition before the link (rs may be $31) */
    CASE(BLTZ) if ((int32_t)RS < 0) npc = IMM; NEXT;
    CASE(BGEZ) if ((int32_t)RS >= 0) npc = IMM; NEXT;
    CASE(BLTZAL)
    {
        int taken = (int32_t)RS < 0;
        r[31] = d->link;
        if (taken)
            npc = IMM;
        NEXT;
    }
    CASE(BGEZAL)
    {
        int taken = (int32_t)RS >= 0;
        r[31] = d->link;
        if (taken)
            npc = IMM;
        NEXT;
    }
    CASE(J) npc = IMM; NEXT;
    CASE(JAL)
        r[31] = d->link;
        npc = IMM;
        NEXT;
    CASE(BEQ) if (RS == RT) npc = IMM; NEXT;
    CASE(BNE) if (RS != RT) npc = IMM; NEXT;
    CASE(BLEZ) if ((int32_t)RS <= 0) npc = IMM; NEXT;
    CASE(BGTZ) if ((int32_t)RS > 0) npc = IMM; NEXT;
    CASE(IDLE)
    {
        int taken;
        switch (d->e) {
        case RK_BEQ: taken = RS == RT; break;
        case RK_BNE: taken = RS != RT; break;
        case RK_BLEZ: taken = (int32_t)RS <= 0; break;
        case RK_BGTZ: taken = (int32_t)RS > 0; break;
        case RK_BLTZ: taken = (int32_t)RS < 0; break;
        case RK_BGEZ: taken = (int32_t)RS >= 0; break;
        default: taken = 1; break; /* J */
        }
        if (taken) {
            uint32_t b = (uint32_t)(d - dec) << 2;
            npc = IMM;
            if (pc == b + 4) /* not in another branch's delay slot */
                n = rsp_idle(s, b, IMM, n, cycles);
        }
        NEXT;
    }
    CASE(CDOWN) /* each turn: the branch, then rs - 1 in the delay slot; j turns at once while all are taken */
    {
        uint32_t v = RS, rs = d->a;
        if (d->e == RK_BGTZ ? (int32_t)v > 0 : v != 0) {
            npc = IMM;
            uint32_t slot = n64_be32(imem + pc);
            if (pc == IMM + 4 && (slot & ~(1u << 26)) == (0x20000000u | rs << 21 | rs << 16 | 0xFFFFu)) {
                uint32_t j = v - 1, m = (cycles - n) / 2;
                if (j > m)
                    j = m;
                r[rs] = v - j;
                n += 2 * j;
            }
        }
        NEXT;
    }
    CASE(ADDI) RT = RS + IMM; NEXT;
    CASE(SLTI) RT = (int32_t)RS < (int32_t)IMM; NEXT;
    CASE(SLTIU) RT = RS < IMM; NEXT;
    CASE(ANDI) RT = RS & IMM; NEXT;
    CASE(ORI) RT = RS | IMM; NEXT; /* and LUI */
    CASE(XORI) RT = RS ^ IMM; NEXT;
    CASE(MFC0) /* also into $0: reading SP_SEMAPHORE sets it */
        RT = cop0_read(s, d->c);
        r[0] = 0;
        NEXT;
    CASE(MTC0)
        cop0_write(s, d->c, RT);
        goto stop; /* SP_STATUS may have halted the RSP or turned single-step on */
    CASE(COP2M) /* COP2 moves (the computational ops are RK_VU) */
        cop2_move(s, IMM);
        r[0] = 0;
        NEXT;
    CASE(LB) RT = (uint32_t)(int32_t)(int8_t)DM(RS + IMM); NEXT;
    CASE(LH) RT = (uint32_t)(int32_t)(int16_t)ld16(s, RS + IMM); NEXT;
    CASE(LW) RT = ld32(s, RS + IMM); NEXT; /* and LWU */
    CASE(LBU) RT = DM(RS + IMM); NEXT;
    CASE(LHU) RT = ld16(s, RS + IMM); NEXT;
    CASE(SB) DM(RS + IMM) = (uint8_t)RT; NEXT;
    CASE(SH)
    {
        uint32_t a = RS + IMM, v = RT;
        DM(a) = (uint8_t)(v >> 8);
        DM(a + 1) = (uint8_t)v;
        NEXT;
    }
    CASE(SW) st32(s, RS + IMM, RT); NEXT;
    CASE(LWC2) vu_load(s, IMM); NEXT;
    CASE(SWC2) vu_store(s, IMM); NEXT;
    /* LDV / SDV inside DMEM, LQV / SQV of a whole register at a 16-byte boundary: as ld_bytes / st_bytes do them, the
     * other cases through vu_load / vu_store (the instruction is the entry's word). */
    CASE(LDV)
    {
        uint32_t addr = (RS + IMM) & 0xFFF;
        if (addr <= 0xFF8)
            ld_be16x4(s->vr[d->b] + d->e / 2, s->mem + addr);
        else
            vu_load(s, n64_be32((const uint8_t *)&d->word));
        NEXT;
    }
    CASE(SDV)
    {
        uint32_t addr = (RS + IMM) & 0xFFF;
        if (addr <= 0xFF8)
            st_be16x4(s->mem + addr, s->vr[d->b] + d->e / 2);
        else
            vu_store(s, n64_be32((const uint8_t *)&d->word));
        NEXT;
    }
    CASE(LQV)
    {
        uint32_t addr = RS + IMM;
        if (!((addr & 15) | d->e))
            ld_be16x8(s->vr[d->b], s->mem + (addr & 0xFF0));
        else
            vu_load(s, n64_be32((const uint8_t *)&d->word));
        NEXT;
    }
    CASE(SQV)
    {
        uint32_t addr = RS + IMM;
        if (!((addr & 15) | d->e))
            st_be16x8(s->mem + (addr & 0xFF0), s->vr[d->b]);
        else
            vu_store(s, n64_be32((const uint8_t *)&d->word));
        NEXT;
    }
#if !RSP_THREADED
    default: NEXT;
        }
#endif
    stop:
        if (s->status & ST_SSTEP)
            s->status |= ST_HALT;
        if (s->status & ST_HALT)
            goto out;
        NEXT;
#if !RSP_THREADED
    }
#endif
out:
    s->pc = pc;
    s->npc = npc;
    return n;
#undef FETCH
#undef RS
#undef RT
#undef IMM
#undef CASE
#undef NEXT
}

uint32_t n64_rsp_run(n64_rsp *s, uint32_t cycles)
{
    if (s->status & ST_HALT)
        return 0;
    if (!(s->status & ST_SSTEP))
        return rsp_loop(s, cycles);
    /* Single-step: one instruction, then the RSP halts (unless that instruction turned single-step off). A budget of
     * one never skips anything (rsp_idle, RK_CDOWN). */
    uint32_t n = 0;
    while (n < cycles) {
        n += rsp_loop(s, 1);
        if (s->status & ST_SSTEP)
            s->status |= ST_HALT;
        if (s->status & ST_HALT)
            break;
    }
    return n;
}
