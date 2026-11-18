/* nds_cpu.c -- the ARM946E-S (ARMv5TE) interpreter of the DS spike (XBA-223): ARM and Thumb, banked modes,
 * exceptions, CP15 (control, TCM regions, wait for interrupt), and the BIOS in C (HLE). Not cycle-accurate: one
 * cycle per instruction, as lifted code counts (nds_recomp_rt.h). Its semantics are the lifted code's: same register
 * results and flags for every encoding the lifter emits (gba_tools + nds_tools/lift.py), so a run may switch between
 * the two at any instruction boundary. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nds_internal.h"

#define CPU (c->h.cpu)
#define M (c->m)
#define T_BIT 0x20u
#define I_BIT 0x80u

static int bank_of(uint32_t mode)
{
    switch (mode & 0x1F) {
    case 0x11: return 1; /* FIQ */
    case 0x12: return 2; /* IRQ */
    case 0x13: return 3; /* SVC */
    case 0x17: return 4; /* ABT */
    case 0x1B: return 5; /* UND */
    default: return 0;   /* USR / SYS */
    }
}

void nds_cpu_stop(nds_arm *c, const char *what, uint32_t pc, uint32_t op)
{
    if (!M->error[0])
        snprintf(M->error, sizeof M->error, "ARM%d: %s at 0x%08X (opcode 0x%08X)", c->id ? 7 : 9, what, pc, op);
}
#define stop nds_cpu_stop

uint32_t nds_cpu_cpsr(const nds_arm *c)
{
    const nds_cpu *c = &c->h.cpu;
    return c->n << 31 | c->z << 30 | c->c << 29 | c->v << 28 | c->q << 27 | c->cpsr_ctl;
}

static void switch_mode(nds_arm *c, uint32_t newmode)
{
    uint32_t old = CPU.cpsr_ctl & 0x1F;
    int ob = bank_of(old), nb = bank_of(newmode);
    if (ob == nb)
        return;
    CPU.bank_r13[ob] = CPU.r[13];
    CPU.bank_r14[ob] = CPU.r[14];
    if (ob == 1) {
        memcpy(CPU.fiq_r8_12, &CPU.r[8], sizeof CPU.fiq_r8_12);
        memcpy(&CPU.r[8], CPU.usr_r8_12, sizeof CPU.usr_r8_12);
    } else if (nb == 1) {
        memcpy(CPU.usr_r8_12, &CPU.r[8], sizeof CPU.usr_r8_12);
        memcpy(&CPU.r[8], CPU.fiq_r8_12, sizeof CPU.fiq_r8_12);
    }
    CPU.r[13] = CPU.bank_r13[nb];
    CPU.r[14] = CPU.bank_r14[nb];
}

void nds_cpu_set_cpsr(nds_arm *c, uint32_t v)
{
    switch_mode(c, v & 0x1F);
    CPU.n = v >> 31;
    CPU.z = (v >> 30) & 1;
    CPU.c = (v >> 29) & 1;
    CPU.v = (v >> 28) & 1;
    CPU.q = (v >> 27) & 1;
    CPU.cpsr_ctl = v & 0xFF;
}

static void set_pc(nds_arm *c, uint32_t target, bool thumb)
{
    c->branched = true;
    if (thumb) {
        CPU.cpsr_ctl |= T_BIT;
        CPU.r[15] = (target & ~1u) + 4;
    } else {
        CPU.cpsr_ctl &= ~T_BIT;
        CPU.r[15] = (target & ~3u) + 8;
    }
}

static void exception(nds_arm *c, uint32_t mode, uint32_t vector, uint32_t lr)
{
    uint32_t cpsr = nds_cpu_cpsr(c);
    switch_mode(c, mode);
    CPU.spsr[bank_of(mode)] = cpsr;
    CPU.r[14] = lr;
    CPU.cpsr_ctl = (CPU.cpsr_ctl & ~0x3Fu & ~T_BIT) | mode | I_BIT;
    uint32_t base = (!c->id && (M->cp15_ctl & (1u << 13))) ? 0xFFFF0000u : 0;
    set_pc(c, base + vector, false);
    CPU.halted = false;
}

void nds_cpu_irq(nds_arm *c)
{
    uint32_t next = CPU.r[15] - ((CPU.cpsr_ctl & T_BIT) ? 4 : 8);
    exception(c, 0x12, 0x18, next + 4);
}

void nds_cpu_reset(nds_arm *c, uint32_t entry)
{
    memset(&CPU, 0, sizeof CPU);
    CPU.cpsr_ctl = 0xD3; /* SVC, IRQ and FIQ off */
    /* What the firmware leaves for a direct boot (GBATEK « DS Firmware Boot »). */
    if (c->id) {
        CPU.bank_r13[0] = 0x0380FF00; /* USR / SYS */
        CPU.bank_r13[2] = 0x0380FFB0; /* IRQ */
        CPU.bank_r13[3] = 0x0380FFDC; /* SVC */
        CPU.r[13] = 0x0380FFDC;
    } else {
        CPU.bank_r13[0] = 0x03002F7C;
        CPU.bank_r13[2] = 0x03003F80;
        CPU.bank_r13[3] = 0x03003FC0;
        CPU.r[13] = 0x03003FC0;
    }
    CPU.r[12] = CPU.r[14] = entry;
    set_pc(c, entry, entry & 1);
}

/* ---- memory through the bus ---- */

static inline uint32_t rd32(nds_arm *c, uint32_t a) { return nds_read32(c, a); }
static inline void wr32(nds_arm *c, uint32_t a, uint32_t v) { nds_write32(c, a, v); }

static inline uint32_t ror32(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

/* ---- CP15 ---- */

static void cp15_update(nds_arm *c)
{
    M->dtcm_base = M->cp15_dtcm & 0xFFFFF000u;
    M->dtcm_size = 512u << ((M->cp15_dtcm >> 1) & 0x1F);
    M->itcm_size = 512u << ((M->cp15_itcm >> 1) & 0x1F);
}

static uint32_t cp15_read(nds_arm *c, uint32_t cn, uint32_t cm, uint32_t op2)
{
    switch (cn) {
    case 0: return op2 == 1 ? 0x0F0D2112u : op2 == 2 ? 0x00140180u : 0x41059461u;
    case 1: return M->cp15_ctl;
    case 9:
        if (cm == 1)
            return op2 ? M->cp15_itcm : M->cp15_dtcm;
        return 0;
    case 13: return M->cp15_c13[op2 & 1];
    default: return M->cp15_misc[cn & 15];
    }
}

static void cp15_write(nds_arm *c, uint32_t cn, uint32_t cm, uint32_t op2, uint32_t v)
{
    switch (cn) {
    case 1:
        M->cp15_ctl = (v & 0x000FF085u) | 0x78u;
        cp15_update(c);
        break;
    case 7:
        if ((cm == 0 && op2 == 4) || (cm == 8 && op2 == 2))
            CPU.halted = true; /* wait for interrupt */
        break;                 /* cache and write buffer operations: nothing to do */
    case 9:
        if (cm == 1) {
            if (op2 == 0)
                M->cp15_dtcm = v;
            else if (op2 == 1)
                M->cp15_itcm = v;
            cp15_update(c);
        }
        break;
    case 13: M->cp15_c13[op2 & 1] = v; break;
    default: M->cp15_misc[cn & 15] = v; break; /* MPU regions, cache config: kept, not enforced */
    }
}

/* ---- the BIOS (HLE) ---- */

/* ---- BIOS helpers both CPUs share (GBATEK « BIOS Functions ») ---- */

static uint32_t bios_crc16(nds_arm *c, uint32_t crc, uint32_t addr, uint32_t len)
{
    for (uint32_t i = 0; i < (len & ~1u); i++) {
        crc ^= nds_read8(c, addr + i);
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001u : crc >> 1;
    }
    return crc & 0xFFFF;
}

/* Byte sink for the decompressors: VRAM variants write halfwords. */
typedef struct bios_out {
    nds_arm *c;
    uint32_t d;
    bool vram;
    uint32_t pend;
} bios_out;

static void out_byte(bios_out *o, uint8_t b)
{
    if (!o->vram) {
        nds_write8(o->c, o->d++, b);
        return;
    }
    if (o->d & 1) {
        nds_write16(o->c, o->d & ~1u, (uint16_t)(o->pend | (uint32_t)b << 8));
    } else {
        o->pend = b;
    }
    o->d++;
}

static uint8_t out_peek(bios_out *o, uint32_t back)
{
    uint32_t a = o->d - back;
    if (o->vram && (a & ~1u) == (o->d & ~1u) && (o->d & 1))
        return (uint8_t)o->pend; /* the low byte not written yet */
    return nds_read8(o->c, a);
}

static void bios_lz77(nds_arm *c, uint32_t s, uint32_t d, bool vram)
{
    uint32_t hdr = nds_read32(c, s);
    uint32_t size = hdr >> 8;
    s += 4;
    bios_out o = {c, d, vram, 0};
    uint32_t done = 0;
    while (done < size) {
        uint8_t flags = nds_read8(c, s++);
        for (int k = 0; k < 8 && done < size; k++, flags <<= 1) {
            if (flags & 0x80) {
                uint32_t b0 = nds_read8(c, s), b1 = nds_read8(c, s + 1);
                s += 2;
                uint32_t len = (b0 >> 4) + 3, back = ((b0 & 15) << 8 | b1) + 1;
                for (uint32_t i = 0; i < len && done < size; i++, done++)
                    out_byte(&o, out_peek(&o, back));
            } else {
                out_byte(&o, nds_read8(c, s++));
                done++;
            }
        }
    }
}

static void bios_rl(nds_arm *c, uint32_t s, uint32_t d, bool vram)
{
    uint32_t size = nds_read32(c, s) >> 8;
    s += 4;
    bios_out o = {c, d, vram, 0};
    uint32_t done = 0;
    while (done < size) {
        uint8_t f = nds_read8(c, s++);
        if (f & 0x80) {
            uint32_t len = (f & 0x7F) + 3;
            uint8_t b = nds_read8(c, s++);
            for (uint32_t i = 0; i < len && done < size; i++, done++)
                out_byte(&o, b);
        } else {
            uint32_t len = (f & 0x7F) + 1;
            for (uint32_t i = 0; i < len && done < size; i++, done++)
                out_byte(&o, nds_read8(c, s++));
        }
    }
}

/* BitUnpack: r2 -> {u16 source length, u8 source width, u8 dest width, u32 offset (bit 31: offset zero too)}. */
static void bios_bitunpack(nds_arm *c, uint32_t s, uint32_t d, uint32_t info)
{
    uint32_t len = nds_read16(c, info), sw = nds_read8(c, info + 2), dw = nds_read8(c, info + 3);
    uint32_t off = nds_read32(c, info + 4), add = off & 0x7FFFFFFF;
    bool zero = off >> 31;
    if (!sw || !dw || sw > 8 || dw > 32)
        return;
    uint32_t acc = 0, bits = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = nds_read8(c, s + i);
        for (uint32_t k = 0; k < 8; k += sw) {
            uint32_t v = (b >> k) & ((1u << sw) - 1);
            if (v || zero)
                v += add;
            acc |= (dw == 32 ? v : v & ((1u << dw) - 1)) << bits;
            bits += dw;
            if (bits >= 32) {
                nds_write32(c, d, acc);
                d += 4;
                acc = 0;
                bits = 0;
            }
        }
    }
}

/* ARM7-only functions. The sound tables are computed (no BIOS image): GBATEK's formulas. */
static bool bios7_swi(nds_arm *c, uint32_t num)
{
    switch (num) {
    case 0x07: /* Sleep */
        c->h.cpu.halted = true;
        return true;
    case 0x08: /* SoundBias */
        return true;
    case 0x1A: { /* GetSineTable: sin(r0 * pi / 128) * 0x8000, r0 0..63 */
        double v = sin((double)(c->h.cpu.r[0] & 63) * 3.14159265358979323846 / 128.0) * 32768.0;
        c->h.cpu.r[0] = (uint32_t)(int32_t)(v >= 32767.0 ? 32767 : (int)v) & 0xFFFF;
        return true;
    }
    case 0x1B: { /* GetPitchTable: (2^(r0/768) - 1) * 0x10000, r0 0..767 */
        double v = (pow(2.0, (double)(c->h.cpu.r[0] % 768) / 768.0) - 1.0) * 65536.0;
        c->h.cpu.r[0] = (uint32_t)v & 0xFFFF;
        return true;
    }
    case 0x1C: { /* GetVolumeTable: r0 0..723, 128 * 2^((r0 - 723) / 128) for the top octave, ... */
        uint32_t i = c->h.cpu.r[0] % 724;
        uint32_t oct = i / 128, pos = i % 128;
        double base = pow(2.0, (double)pos / 128.0) * 64.0; /* 64..127 */
        int shift = 5 - (int)oct;                           /* the last octave keeps 64..127 */
        uint32_t v = (uint32_t)(base) >> (shift > 0 ? shift : 0);
        if (i >= 0x2C4)
            v = (uint32_t)base;
        c->h.cpu.r[0] = v & 0x7F;
        return true;
    }
    case 0x1D: /* GetBootProcs */
        c->h.cpu.r[0] = c->h.cpu.r[1] = c->h.cpu.r[2] = 0;
        return true;
    case 0x1F: /* CustomHalt (r2 = HALTCNT value) */
        if ((c->h.cpu.r[2] & 0xC0) == 0x80 || (c->h.cpu.r[2] & 0xC0) == 0xC0)
            c->h.cpu.halted = true;
        return true;
    default:
        return false;
    }
}

static void bios_return(nds_arm *c)
{
    uint32_t spsr = CPU.spsr[bank_of(CPU.cpsr_ctl)];
    uint32_t lr = CPU.r[14];
    nds_cpu_set_cpsr(c, spsr);
    set_pc(c, lr, (spsr & T_BIT) != 0);
}

static void bios_swi(nds_arm *c)
{
    uint32_t spsr = CPU.spsr[bank_of(CPU.cpsr_ctl)];
    uint32_t lr = CPU.r[14];
    uint32_t num = (spsr & T_BIT) ? nds_read16(c, lr - 2) & 0xFF : (rd32(c, lr - 4) >> 16) & 0xFF;
    switch (num) {
    case 0x03: /* WaitByLoop */
        c->h.now += CPU.r[0] * 4u;
        break;
    case 0x04: /* IntrWait (r0: discard old flags, r1: mask) */
    case 0x05: { /* VBlankIntrWait */
        uint32_t mask = num == 5 ? 1u : CPU.r[1];
        bool discard = num == 5 ? true : CPU.r[0] != 0;
        uint32_t flags_at = c->id ? 0x0380FFF8u : M->dtcm_base + 0x3FF8;
        if (c->swi_wait_first && discard) {
            nds_write32(c, flags_at, rd32(c, flags_at) & ~mask);
            c->swi_wait_first = false;
        }
        uint32_t f = rd32(c, flags_at);
        if (!(f & mask) || (c->swi_wait_first && discard)) {
            c->swi_wait_first = false;
            c->ime = 1;
            nds_irq_update(c);
            CPU.cpsr_ctl &= ~I_BIT;   /* the BIOS waits with IRQs on: the IRQ returns here and checks again */
            CPU.halted = true;
            return;                    /* PC stays at the SWI vector */
        }
        nds_write32(c, flags_at, f & ~mask);
        c->swi_wait_first = true;
        break;
    }
    case 0x06: /* Halt */
        CPU.halted = true;
        break;
    case 0x09: { /* Div */
        int32_t n = (int32_t)CPU.r[0], d = (int32_t)CPU.r[1];
        if (d == 0) {
            CPU.r[0] = n < 0 ? 0xFFFFFFFFu : 1;
            CPU.r[1] = (uint32_t)n;
        } else if (n == INT32_MIN && d == -1) {
            CPU.r[0] = (uint32_t)INT32_MIN;
            CPU.r[1] = 0;
        } else {
            CPU.r[0] = (uint32_t)(n / d);
            CPU.r[1] = (uint32_t)(n % d);
        }
        CPU.r[3] = (uint32_t)((int32_t)CPU.r[0] < 0 ? -(int32_t)CPU.r[0] : (int32_t)CPU.r[0]);
        break;
    }
    case 0x0B: { /* CpuSet */
        uint32_t s = CPU.r[0], d = CPU.r[1], cnt = CPU.r[2] & 0x1FFFFF;
        bool fill = (CPU.r[2] >> 24) & 1, word = (CPU.r[2] >> 26) & 1;
        for (uint32_t i = 0; i < cnt; i++) {
            if (word) {
                wr32(c, d, rd32(c, s));
                d += 4;
                if (!fill)
                    s += 4;
            } else {
                nds_write16(c, d, nds_read16(c, s));
                d += 2;
                if (!fill)
                    s += 2;
            }
        }
        break;
    }
    case 0x0C: { /* CpuFastSet */
        uint32_t s = CPU.r[0], d = CPU.r[1], cnt = ((CPU.r[2] & 0x1FFFFF) + 7) & ~7u;
        bool fill = (CPU.r[2] >> 24) & 1;
        for (uint32_t i = 0; i < cnt; i++) {
            wr32(c, d, rd32(c, s));
            d += 4;
            if (!fill)
                s += 4;
        }
        break;
    }
    case 0x0D: { /* Sqrt */
        uint32_t v = CPU.r[0], r = 0, bit = 1u << 30;
        while (bit > v)
            bit >>= 2;
        while (bit) {
            if (v >= r + bit) {
                v -= r + bit;
                r = (r >> 1) + bit;
            } else {
                r >>= 1;
            }
            bit >>= 2;
        }
        CPU.r[0] = r;
        break;
    }
    case 0x0E: /* GetCRC16 (r0 initial, r1 address, r2 length in bytes) */
        CPU.r[0] = bios_crc16(c, CPU.r[0] & 0xFFFF, CPU.r[1], CPU.r[2]);
        break;
    case 0x10: /* BitUnpack */
        bios_bitunpack(c, CPU.r[0], CPU.r[1], CPU.r[2]);
        break;
    case 0x11: /* LZ77UncompWram */
    case 0x12: /* LZ77UncompVram */
        bios_lz77(c, CPU.r[0], CPU.r[1], num == 0x12);
        break;
    case 0x14: /* RLUncompWram */
    case 0x15: /* RLUncompVram */
        bios_rl(c, CPU.r[0], CPU.r[1], num == 0x15);
        break;
    case 0x0F: /* IsDebugger */
        CPU.r[0] = 0;
        break;
    default:
        if (c->id && bios7_swi(c, num))
            break;
        stop(c, "unsupported BIOS SWI", lr, num);
        return;
    }
    bios_return(c);
}

/* The BIOS IRQ handler (high vectors): r0-r3, r12, lr pushed, the user handler at DTCM + 0x3FFC called. */
static void bios_irq(nds_arm *c)
{
    uint32_t sp = CPU.r[13] - 24;
    uint32_t regs[6] = {CPU.r[0], CPU.r[1], CPU.r[2], CPU.r[3], CPU.r[12], CPU.r[14]};
    for (int i = 0; i < 6; i++)
        wr32(c, sp + 4u * i, regs[i]);
    CPU.r[13] = sp;
    CPU.r[0] = 0x04000000;
    CPU.r[14] = c->id ? NDS_BIOS7_IRQ_RET : NDS_BIOS9_IRQ_RET;
    uint32_t handler = rd32(c, c->id ? 0x0380FFFCu : M->dtcm_base + 0x3FFC);
    set_pc(c, handler, handler & 1);
}

static void bios_irq_return(nds_arm *c)
{
    uint32_t sp = CPU.r[13];
    CPU.r[0] = rd32(c, sp);
    CPU.r[1] = rd32(c, sp + 4);
    CPU.r[2] = rd32(c, sp + 8);
    CPU.r[3] = rd32(c, sp + 12);
    CPU.r[12] = rd32(c, sp + 16);
    CPU.r[14] = rd32(c, sp + 20);
    CPU.r[13] = sp + 24;
    uint32_t spsr = CPU.spsr[bank_of(CPU.cpsr_ctl)];
    uint32_t ret = CPU.r[14] - 4;
    nds_cpu_set_cpsr(c, spsr);
    set_pc(c, ret, (spsr & T_BIT) != 0);
}

/* ---- ARM ---- */

static bool cond_ok(const nds_arm *c, uint32_t c)
{
    const nds_cpu *p = &c->h.cpu;
    switch (c) {
    case 0x0: return p->z;
    case 0x1: return !p->z;
    case 0x2: return p->c;
    case 0x3: return !p->c;
    case 0x4: return p->n;
    case 0x5: return !p->n;
    case 0x6: return p->v;
    case 0x7: return !p->v;
    case 0x8: return p->c && !p->z;
    case 0x9: return !p->c || p->z;
    case 0xA: return p->n == p->v;
    case 0xB: return p->n != p->v;
    case 0xC: return !p->z && p->n == p->v;
    case 0xD: return p->z || p->n != p->v;
    default: return true;
    }
}

static uint32_t shift_imm(nds_arm *c, uint32_t typ, uint32_t v, uint32_t amt, uint32_t *carry)
{
    switch (typ) {
    case 0:
        if (amt == 0) {
            *carry = CPU.c;
            return v;
        }
        *carry = (v >> (32 - amt)) & 1;
        return v << amt;
    case 1:
        if (amt == 0) {
            *carry = v >> 31;
            return 0;
        }
        *carry = (v >> (amt - 1)) & 1;
        return v >> amt;
    case 2:
        if (amt == 0) {
            *carry = v >> 31;
            return (uint32_t)((int32_t)v >> 31);
        }
        *carry = (v >> (amt - 1)) & 1;
        return (uint32_t)((int32_t)v >> amt);
    default:
        if (amt == 0) {
            *carry = v & 1;
            return CPU.c << 31 | v >> 1;
        }
        *carry = (v >> (amt - 1)) & 1;
        return ror32(v, amt);
    }
}

static uint32_t add_flags(nds_arm *c, uint32_t x, uint32_t y, uint32_t cin, bool s)
{
    uint64_t w = (uint64_t)x + y + cin;
    uint32_t r = (uint32_t)w;
    if (s) {
        CPU.n = r >> 31;
        CPU.z = r == 0;
        CPU.c = (uint32_t)(w >> 32);
        CPU.v = (~(x ^ y) & (x ^ r)) >> 31;
    }
    return r;
}

/* Writes PC from a load (ARMv5: bit 0 picks Thumb). */
static void load_pc(nds_arm *c, uint32_t v)
{
    if (c->id) { /* ARMv4T: no interworking on loads to PC */
        set_pc(c, v, (CPU.cpsr_ctl & T_BIT) != 0);
        return;
    }
    set_pc(c, v, v & 1);
}

static void arm_dp(nds_arm *c, uint32_t op, uint32_t pc)
{
    uint32_t opc = (op >> 21) & 15, s = (op >> 20) & 1, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t o2, sc, a;
    if (op & (1u << 25)) {
        uint32_t rot = (op >> 7) & 0x1E;
        o2 = ror32(op & 0xFF, rot);
        sc = rot ? o2 >> 31 : CPU.c;
        a = rn == 15 ? pc + 8 : CPU.r[rn];
    } else if (op & 0x10) {
        uint32_t rm = op & 15, rs = (op >> 8) & 15;
        uint32_t vm = rm == 15 ? pc + 12 : CPU.r[rm];
        uint32_t vs = (rs == 15 ? pc + 8 : CPU.r[rs]) & 0xFF;
        o2 = rc_shift_reg((op >> 5) & 3, vm, vs, CPU.c, &sc);
        a = rn == 15 ? pc + 12 : CPU.r[rn];
    } else {
        uint32_t rm = op & 15;
        o2 = shift_imm(c, (op >> 5) & 3, rm == 15 ? pc + 8 : CPU.r[rm], (op >> 7) & 31, &sc);
        a = rn == 15 ? pc + 8 : CPU.r[rn];
    }
    bool write = !(opc >= 8 && opc <= 11);
    bool sf = s && !(rd == 15 && write);
    uint32_t res = 0;
    bool logical = false;
    switch (opc) {
    case 0x0: res = a & o2; logical = true; break;
    case 0x1: res = a ^ o2; logical = true; break;
    case 0x2: res = add_flags(c, a, ~o2, 1, sf); break;
    case 0x3: res = add_flags(c, o2, ~a, 1, sf); break;
    case 0x4: res = add_flags(c, a, o2, 0, sf); break;
    case 0x5: res = add_flags(c, a, o2, CPU.c, sf); break;
    case 0x6: res = add_flags(c, a, ~o2, CPU.c, sf); break;
    case 0x7: res = add_flags(c, o2, ~a, CPU.c, sf); break;
    case 0x8: res = a & o2; logical = true; break;
    case 0x9: res = a ^ o2; logical = true; break;
    case 0xA: res = add_flags(c, a, ~o2, 1, true); break;
    case 0xB: res = add_flags(c, a, o2, 0, true); break;
    case 0xC: res = a | o2; logical = true; break;
    case 0xD: res = o2; logical = true; break;
    case 0xE: res = a & ~o2; logical = true; break;
    default: res = ~o2; logical = true; break;
    }
    if (logical && (sf || (!write && s))) {
        CPU.n = res >> 31;
        CPU.z = res == 0;
        CPU.c = sc;
    }
    if (!write)
        return;
    if (rd == 15) {
        if (s) { /* MOVS pc / SUBS pc: CPSR = SPSR */
            uint32_t spsr = CPU.spsr[bank_of(CPU.cpsr_ctl)];
            nds_cpu_set_cpsr(c, spsr);
            set_pc(c, res, (spsr & T_BIT) != 0);
        } else {
            set_pc(c, res, false); /* no interworking for data processing on ARMv5 */
        }
        return;
    }
    CPU.r[rd] = res;
}

static void arm_psr(nds_arm *c, uint32_t op, uint32_t pc)
{
    bool spsr = (op >> 22) & 1;
    if ((op & 0x0FBF0FFF) == 0x010F0000) { /* MRS */
        uint32_t rd = (op >> 12) & 15;
        CPU.r[rd] = spsr ? CPU.spsr[bank_of(CPU.cpsr_ctl)] : nds_cpu_cpsr(c);
        return;
    }
    uint32_t v = (op & (1u << 25)) ? ror32(op & 0xFF, (op >> 7) & 0x1E) : CPU.r[op & 15];
    uint32_t mask = 0;
    if (op & (1u << 16))
        mask |= 0x000000FF;
    if (op & (1u << 17))
        mask |= 0x0000FF00;
    if (op & (1u << 18))
        mask |= 0x00FF0000;
    if (op & (1u << 19))
        mask |= 0xFF000000;
    (void)pc;
    if (spsr) {
        int b = bank_of(CPU.cpsr_ctl);
        if (b)
            CPU.spsr[b] = (CPU.spsr[b] & ~mask) | (v & mask);
        return;
    }
    if ((CPU.cpsr_ctl & 0x1F) == 0x10)
        mask &= 0xFF000000; /* user mode: flags only */
    uint32_t cur = nds_cpu_cpsr(c);
    uint32_t nv = (cur & ~mask) | (v & mask);
    nv = (nv & ~T_BIT) | (cur & T_BIT); /* MSR does not change the state */
    nds_cpu_set_cpsr(c, nv);
}

static void arm_single(nds_arm *c, uint32_t op, uint32_t pc)
{
    uint32_t p = (op >> 24) & 1, u = (op >> 23) & 1, b = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t off;
    if (op & (1u << 25)) {
        uint32_t c, rm = op & 15;
        off = shift_imm(c, (op >> 5) & 3, rm == 15 ? pc + 8 : CPU.r[rm], (op >> 7) & 31, &c);
    } else {
        off = op & 0xFFF;
    }
    uint32_t base = rn == 15 ? pc + 8 : CPU.r[rn];
    uint32_t moved = u ? base + off : base - off;
    uint32_t ad = p ? moved : base;
    bool wb = !p || w;
    if (l) {
        uint32_t v = b ? nds_read8(c, ad) : ror32(rd32(c, ad), (ad & 3) * 8);
        if (wb)
            CPU.r[rn] = moved;
        if (rd == 15)
            load_pc(c, v);
        else
            CPU.r[rd] = v;
    } else {
        uint32_t v = rd == 15 ? pc + 12 : CPU.r[rd];
        if (b)
            nds_write8(c, ad, (uint8_t)v);
        else
            wr32(c, ad, v);
        if (wb)
            CPU.r[rn] = moved;
    }
}

static void arm_half(nds_arm *c, uint32_t op, uint32_t pc)
{
    uint32_t p = (op >> 24) & 1, u = (op >> 23) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, sh = (op >> 5) & 3;
    uint32_t off = (op & (1u << 22)) ? (((op >> 4) & 0xF0) | (op & 0xF)) : CPU.r[op & 15];
    uint32_t base = rn == 15 ? pc + 8 : CPU.r[rn];
    uint32_t moved = u ? base + off : base - off;
    uint32_t ad = p ? moved : base;
    bool wb = !p || w;
    if (l) {
        uint32_t v;
        if (sh == 1)
            v = ror32(nds_read16(c, ad), (ad & 1) * 8);
        else if (sh == 2)
            v = (uint32_t)(int32_t)(int8_t)nds_read8(c, ad);
        else
            v = (ad & 1) ? (uint32_t)(int32_t)(int8_t)nds_read8(c, ad) : (uint32_t)(int32_t)(int16_t)nds_read16(c, ad);
        if (wb)
            CPU.r[rn] = moved;
        if (rd == 15)
            load_pc(c, v);
        else
            CPU.r[rd] = v;
        return;
    }
    if (sh == 1) {
        nds_write16(c, ad, (uint16_t)(rd == 15 ? pc + 12 : CPU.r[rd]));
    } else if (sh == 2) { /* LDRD */
        uint32_t v0 = rd32(c, ad), v1 = rd32(c, ad + 4);
        if (wb)
            CPU.r[rn] = moved;
        CPU.r[rd & 14] = v0;
        CPU.r[(rd & 14) + 1] = v1;
        return;
    } else { /* STRD */
        wr32(c, ad, CPU.r[rd & 14]);
        wr32(c, ad + 4, CPU.r[(rd & 14) + 1]);
    }
    if (wb)
        CPU.r[rn] = moved;
}

/* LDM / STM with the S bit and no PC: the user bank's registers. */
static void user_bank(nds_arm *c, bool enter, uint32_t mode)
{
    if (enter) {
        switch_mode(c, 0x1F);
        CPU.cpsr_ctl = (CPU.cpsr_ctl & ~0x1Fu) | 0x1F;
    } else {
        switch_mode(c, mode);
        CPU.cpsr_ctl = (CPU.cpsr_ctl & ~0x1Fu) | mode;
    }
}

static void arm_block(nds_arm *c, uint32_t op, uint32_t pc)
{
    uint32_t p = (op >> 24) & 1, u = (op >> 23) & 1, s = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15, lst = op & 0xFFFF;
    int n = __builtin_popcount(lst);
    if (!lst) {
        stop(c, "LDM / STM with an empty list", pc, op);
        return;
    }
    uint32_t base = CPU.r[rn];
    uint32_t ad = u ? base + (p ? 4 : 0) : base - 4u * n + (p ? 0 : 4);
    uint32_t nb = u ? base + 4u * n : base - 4u * n;
    bool user = s && !(l && (lst & 0x8000)); /* user-bank transfer */
    uint32_t mode = CPU.cpsr_ctl & 0x1F;
    if (user)
        user_bank(c, true, mode);
    if (l) {
        if (w && !(lst >> rn & 1))
            CPU.r[rn] = nb;
        uint32_t pcv = 0;
        for (int i = 0; i < 16; i++) {
            if (!(lst >> i & 1))
                continue;
            uint32_t v = rd32(c, ad);
            ad += 4;
            if (i == 15)
                pcv = v;
            else
                CPU.r[i] = v;
        }
        if (user)
            user_bank(c, false, mode);
        if (lst & 0x8000) {
            if (s) { /* LDM {..., pc}^: CPSR = SPSR */
                uint32_t spsr = CPU.spsr[bank_of(CPU.cpsr_ctl)];
                nds_cpu_set_cpsr(c, spsr);
                set_pc(c, pcv, (spsr & T_BIT) != 0);
            } else {
                load_pc(c, pcv);
            }
        }
        return;
    }
    int first = __builtin_ctz(lst);
    for (int i = 0; i < 16; i++) {
        if (!(lst >> i & 1))
            continue;
        uint32_t v = i == 15 ? pc + 12 : (i == (int)rn && w && i != first) ? nb : CPU.r[i];
        wr32(c, ad, v);
        ad += 4;
    }
    if (user)
        user_bank(c, false, mode);
    if (w)
        CPU.r[rn] = nb;
}

static void arm_execute(nds_arm *c, uint32_t op, uint32_t pc)
{
    uint32_t cond = op >> 28;
    if (cond == 0xF) {
        if ((op & 0xFE000000) == 0xFA000000) { /* BLX imm */
            uint32_t t = pc + 8 + (uint32_t)(((int32_t)(op << 8)) >> 6) + ((op >> 23) & 2);
            CPU.r[14] = pc + 4;
            set_pc(c, t, true);
            return;
        }
        if ((op & 0xFD70F000) == 0xF550F000)
            return; /* PLD */
        stop(c, "undefined ARM encoding", pc, op);
        return;
    }
    if (!cond_ok(c, cond))
        return;
    switch ((op >> 25) & 7) {
    case 0:
        if ((op & 0x0FFFFFD0) == 0x012FFF10) { /* BX / BLX rm */
            uint32_t v = CPU.r[op & 15];
            if (op & 0x20)
                CPU.r[14] = pc + 4;
            set_pc(c, v, v & 1);
            return;
        }
        if ((op & 0x0FC000F0) == 0x00000090) { /* MUL / MLA */
            uint32_t rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
            uint32_t r = CPU.r[rm] * CPU.r[rs];
            if (op & (1u << 21))
                r += CPU.r[rn];
            CPU.r[rd] = r;
            if (op & (1u << 20)) {
                CPU.n = r >> 31;
                CPU.z = r == 0;
            }
            return;
        }
        if ((op & 0x0F8000F0) == 0x00800090) { /* UMULL / UMLAL / SMULL / SMLAL */
            uint32_t hi = (op >> 16) & 15, lo = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
            uint64_t r = (op & (1u << 22)) ? (uint64_t)((int64_t)(int32_t)CPU.r[rm] * (int32_t)CPU.r[rs])
                                           : (uint64_t)CPU.r[rm] * CPU.r[rs];
            if (op & (1u << 21))
                r += (uint64_t)CPU.r[hi] << 32 | CPU.r[lo];
            CPU.r[lo] = (uint32_t)r;
            CPU.r[hi] = (uint32_t)(r >> 32);
            if (op & (1u << 20)) {
                CPU.n = (uint32_t)(r >> 63);
                CPU.z = r == 0;
            }
            return;
        }
        if ((op & 0x0FB00FF0) == 0x01000090) { /* SWP / SWPB */
            uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15, a = CPU.r[rn];
            if (op & (1u << 22)) {
                uint32_t v = nds_read8(c, a);
                nds_write8(c, a, (uint8_t)CPU.r[rm]);
                CPU.r[rd] = v;
            } else {
                uint32_t v = ror32(rd32(c, a), (a & 3) * 8);
                wr32(c, a, CPU.r[rm]);
                CPU.r[rd] = v;
            }
            return;
        }
        if ((op & 0x90) == 0x90 && (op & 0x60)) {
            arm_half(c, op, pc);
            return;
        }
        if ((op & 0x0FFF0FF0) == 0x016F0F10) { /* CLZ */
            CPU.r[(op >> 12) & 15] = rc_clz(CPU.r[op & 15]);
            return;
        }
        if ((op & 0x0F900FF0) == 0x01000050) { /* QADD / QSUB / QDADD / QDSUB */
            uint32_t rd = (op >> 12) & 15, kind = (op >> 21) & 3, q = 0;
            int32_t n = (int32_t)CPU.r[(op >> 16) & 15];
            if (kind >= 2)
                n = rc_qadd(n, n, &q);
            CPU.r[rd] = (uint32_t)((kind & 1) ? rc_qsub((int32_t)CPU.r[op & 15], n, &q)
                                              : rc_qadd((int32_t)CPU.r[op & 15], n, &q));
            CPU.q |= q;
            return;
        }
        if ((op & 0x0F900090) == 0x01000080) { /* SMLAxy / SMLAWy / SMULWy / SMLALxy / SMULxy */
            uint32_t kind = (op >> 21) & 3, rd = (op >> 16) & 15, rn = (op >> 12) & 15, q = 0;
            uint32_t vm = CPU.r[op & 15], vs = CPU.r[(op >> 8) & 15];
            int32_t hx = (op & 0x20) ? (int16_t)(vm >> 16) : (int16_t)vm;
            int32_t hy = (op & 0x40) ? (int16_t)(vs >> 16) : (int16_t)vs;
            if (kind == 1) {
                int32_t prod = (int32_t)(((int64_t)(int32_t)vm * hy) >> 16);
                CPU.r[rd] = (op & 0x20) ? (uint32_t)prod : (uint32_t)rc_qadd_flag(prod, (int32_t)CPU.r[rn], &q);
            } else if (kind == 3) {
                CPU.r[rd] = (uint32_t)(hx * hy);
            } else if (kind == 0) {
                CPU.r[rd] = (uint32_t)rc_qadd_flag(hx * hy, (int32_t)CPU.r[rn], &q);
            } else {
                uint64_t acc = (uint64_t)CPU.r[rd] << 32 | CPU.r[rn];
                acc += (uint64_t)(int64_t)(hx * hy);
                CPU.r[rn] = (uint32_t)acc;
                CPU.r[rd] = (uint32_t)(acc >> 32);
            }
            CPU.q |= q;
            return;
        }
        if ((op & 0x0FF000F0) == 0x01200070) {
            stop(c, "BKPT", pc, op);
            return;
        }
        if ((op & 0x01900000) == 0x01000000) {
            if ((op & 0x0FBF0FFF) == 0x010F0000 || (op & 0x0DB0F000) == 0x0120F000) {
                arm_psr(c, op, pc);
                return;
            }
            stop(c, "undefined ARM encoding", pc, op);
            return;
        }
        arm_dp(c, op, pc);
        return;
    case 1:
        if ((op & 0x01900000) == 0x01000000) {
            if ((op & 0x0FB0F000) == 0x0320F000) {
                arm_psr(c, op, pc);
                return;
            }
            stop(c, "undefined ARM encoding", pc, op);
            return;
        }
        arm_dp(c, op, pc);
        return;
    case 2:
        arm_single(c, op, pc);
        return;
    case 3:
        if (op & 0x10) {
            stop(c, "undefined ARM encoding", pc, op);
            return;
        }
        arm_single(c, op, pc);
        return;
    case 4:
        arm_block(c, op, pc);
        return;
    case 5: {
        uint32_t t = pc + 8 + (uint32_t)(((int32_t)(op << 8)) >> 6);
        if (op & (1u << 24))
            CPU.r[14] = pc + 4;
        set_pc(c, t, false);
        return;
    }
    case 6:
        stop(c, "coprocessor load / store", pc, op);
        return;
    default:
        if (op & (1u << 24)) { /* SWI */
            exception(c, 0x13, 0x08, pc + 4);
            return;
        }
        if ((op & 0x10) && ((op >> 8) & 15) == 15) { /* MRC / MCR p15 */
            uint32_t cn = (op >> 16) & 15, cm = op & 15, op2 = (op >> 5) & 7, rd = (op >> 12) & 15;
            if (op & (1u << 20)) {
                uint32_t v = cp15_read(c, cn, cm, op2);
                if (rd == 15) {
                    CPU.n = v >> 31;
                    CPU.z = (v >> 30) & 1;
                    CPU.c = (v >> 29) & 1;
                    CPU.v = (v >> 28) & 1;
                } else {
                    CPU.r[rd] = v;
                }
            } else {
                cp15_write(c, cn, cm, op2, rd == 15 ? pc + 12 : CPU.r[rd]);
            }
            return;
        }
        stop(c, "coprocessor operation", pc, op);
        return;
    }
}

/* ---- Thumb ---- */

static void thumb_execute(nds_arm *c, uint32_t op, uint32_t pc)
{
    uint32_t k = op >> 11;
    if (k <= 2) { /* LSL / LSR / ASR #imm */
        uint32_t rd = op & 7, rs = (op >> 3) & 7, amt = (op >> 6) & 31, c;
        uint32_t r = shift_imm(c, k, CPU.r[rs], amt, &c);
        CPU.n = r >> 31;
        CPU.z = r == 0;
        CPU.c = c;
        CPU.r[rd] = r;
        return;
    }
    if (k == 3) { /* ADD / SUB */
        uint32_t rd = op & 7, rs = (op >> 3) & 7, x = (op >> 6) & 7;
        uint32_t b = (op & 0x400) ? x : CPU.r[x];
        CPU.r[rd] = (op & 0x200) ? add_flags(c, CPU.r[rs], ~b, 1, true) : add_flags(c, CPU.r[rs], b, 0, true);
        return;
    }
    if (k <= 7) { /* MOV / CMP / ADD / SUB #imm8 */
        uint32_t rd = (op >> 8) & 7, imm = op & 0xFF;
        switch (k) {
        case 4: CPU.r[rd] = imm; CPU.n = 0; CPU.z = imm == 0; break;
        case 5: add_flags(c, CPU.r[rd], ~imm, 1, true); break;
        case 6: CPU.r[rd] = add_flags(c, CPU.r[rd], imm, 0, true); break;
        default: CPU.r[rd] = add_flags(c, CPU.r[rd], ~imm, 1, true); break;
        }
        return;
    }
    if (k == 8) {
        if (!(op & 0x400)) { /* ALU */
            uint32_t rd = op & 7, rs = (op >> 3) & 7, a = CPU.r[rd], b = CPU.r[rs], r = 0, c;
            bool wr = true;
            switch ((op >> 6) & 15) {
            case 0x0: r = a & b; break;
            case 0x1: r = a ^ b; break;
            case 0x2: r = rc_shift_reg(0, a, b & 0xFF, CPU.c, &c); CPU.c = c; break;
            case 0x3: r = rc_shift_reg(1, a, b & 0xFF, CPU.c, &c); CPU.c = c; break;
            case 0x4: r = rc_shift_reg(2, a, b & 0xFF, CPU.c, &c); CPU.c = c; break;
            case 0x5: CPU.r[rd] = add_flags(c, a, b, CPU.c, true); return;
            case 0x6: CPU.r[rd] = add_flags(c, a, ~b, CPU.c, true); return;
            case 0x7: r = rc_shift_reg(3, a, b & 0xFF, CPU.c, &c); CPU.c = c; break;
            case 0x8: r = a & b; wr = false; break;
            case 0x9: CPU.r[rd] = add_flags(c, 0, ~b, 1, true); return;
            case 0xA: add_flags(c, a, ~b, 1, true); return;
            case 0xB: add_flags(c, a, b, 0, true); return;
            case 0xC: r = a | b; break;
            case 0xD: r = a * b; break;
            case 0xE: r = a & ~b; break;
            default: r = ~b; break;
            }
            CPU.n = r >> 31;
            CPU.z = r == 0;
            if (wr)
                CPU.r[rd] = r;
            return;
        }
        /* hi register operations, BX / BLX */
        uint32_t rd = (op & 7) | ((op >> 4) & 8), rs = (op >> 3) & 15, kk = (op >> 8) & 3;
        uint32_t s = rs == 15 ? pc + 4 : CPU.r[rs];
        if (kk == 3) {
            if (op & 0x80)
                CPU.r[14] = (pc + 2) | 1;
            set_pc(c, s, s & 1);
            return;
        }
        uint32_t d = rd == 15 ? pc + 4 : CPU.r[rd];
        if (kk == 1) {
            add_flags(c, d, ~s, 1, true);
            return;
        }
        uint32_t r = kk == 0 ? d + s : s;
        if (rd == 15)
            set_pc(c, r, true);
        else
            CPU.r[rd] = r;
        return;
    }
    if (k == 9) { /* LDR rd, [pc, #imm] */
        CPU.r[(op >> 8) & 7] = rd32(c, ((pc + 4) & ~2u) + (op & 0xFF) * 4);
        return;
    }
    if (k == 0xA || k == 0xB) { /* register offset */
        uint32_t rd = op & 7, ad = CPU.r[(op >> 3) & 7] + CPU.r[(op >> 6) & 7];
        switch ((op >> 9) & 7) {
        case 0: wr32(c, ad, CPU.r[rd]); break;
        case 1: nds_write16(c, ad, (uint16_t)CPU.r[rd]); break;
        case 2: nds_write8(c, ad, (uint8_t)CPU.r[rd]); break;
        case 3: CPU.r[rd] = (uint32_t)(int32_t)(int8_t)nds_read8(c, ad); break;
        case 4: CPU.r[rd] = ror32(rd32(c, ad), (ad & 3) * 8); break;
        case 5: CPU.r[rd] = ror32(nds_read16(c, ad), (ad & 1) * 8); break;
        case 6: CPU.r[rd] = nds_read8(c, ad); break;
        default:
            CPU.r[rd] = (ad & 1) ? (uint32_t)(int32_t)(int8_t)nds_read8(c, ad)
                                 : (uint32_t)(int32_t)(int16_t)nds_read16(c, ad);
            break;
        }
        return;
    }
    if (k <= 0xF) { /* immediate offset, word / byte */
        uint32_t rd = op & 7, off = (op >> 6) & 31, rb = (op >> 3) & 7;
        bool byte = op & 0x1000;
        uint32_t ad = CPU.r[rb] + (byte ? off : off * 4);
        if (op & 0x800)
            CPU.r[rd] = byte ? nds_read8(c, ad) : ror32(rd32(c, ad), (ad & 3) * 8);
        else if (byte)
            nds_write8(c, ad, (uint8_t)CPU.r[rd]);
        else
            wr32(c, ad, CPU.r[rd]);
        return;
    }
    if (k <= 0x11) { /* halfword */
        uint32_t rd = op & 7, ad = CPU.r[(op >> 3) & 7] + ((op >> 6) & 31) * 2;
        if (op & 0x800)
            CPU.r[rd] = ror32(nds_read16(c, ad), (ad & 1) * 8);
        else
            nds_write16(c, ad, (uint16_t)CPU.r[rd]);
        return;
    }
    if (k <= 0x13) { /* SP relative */
        uint32_t rd = (op >> 8) & 7, ad = CPU.r[13] + (op & 0xFF) * 4;
        if (op & 0x800)
            CPU.r[rd] = ror32(rd32(c, ad), (ad & 3) * 8);
        else
            wr32(c, ad, CPU.r[rd]);
        return;
    }
    if (k == 0x14) {
        CPU.r[(op >> 8) & 7] = ((pc + 4) & ~2u) + (op & 0xFF) * 4;
        return;
    }
    if (k == 0x15) {
        CPU.r[(op >> 8) & 7] = CPU.r[13] + (op & 0xFF) * 4;
        return;
    }
    if (k == 0x16 || k == 0x17) {
        if ((op & 0xF00) == 0) {
            uint32_t off = (op & 0x7F) * 4;
            CPU.r[13] = (op & 0x80) ? CPU.r[13] - off : CPU.r[13] + off;
            return;
        }
        if ((op & 0x600) == 0x400) { /* PUSH / POP */
            uint32_t lst = op & 0xFF, extra = (op >> 8) & 1;
            if (op & 0x800) {
                uint32_t ad = CPU.r[13];
                for (int i = 0; i < 8; i++)
                    if (lst >> i & 1) {
                        CPU.r[i] = rd32(c, ad);
                        ad += 4;
                    }
                uint32_t pcv = 0;
                if (extra) {
                    pcv = rd32(c, ad);
                    ad += 4;
                }
                CPU.r[13] = ad;
                if (extra)
                    load_pc(c, pcv);
            } else {
                uint32_t n = (uint32_t)__builtin_popcount(lst) + extra;
                uint32_t ad = CPU.r[13] - 4 * n;
                CPU.r[13] = ad;
                for (int i = 0; i < 8; i++)
                    if (lst >> i & 1) {
                        wr32(c, ad, CPU.r[i]);
                        ad += 4;
                    }
                if (extra)
                    wr32(c, ad, CPU.r[14]);
            }
            return;
        }
        stop(c, (op >> 8) == 0xBE ? "BKPT" : "undefined Thumb encoding", pc, op);
        return;
    }
    if (k == 0x18 || k == 0x19) { /* LDMIA / STMIA */
        uint32_t rb = (op >> 8) & 7, lst = op & 0xFF;
        if (!lst) {
            stop(c, "Thumb LDM / STM with an empty list", pc, op);
            return;
        }
        uint32_t ad = CPU.r[rb], nb = ad + 4u * (uint32_t)__builtin_popcount(lst);
        if (op & 0x800) {
            if (!(lst >> rb & 1))
                CPU.r[rb] = nb;
            for (int i = 0; i < 8; i++)
                if (lst >> i & 1) {
                    CPU.r[i] = rd32(c, ad);
                    ad += 4;
                }
        } else {
            int first = __builtin_ctz(lst);
            for (int i = 0; i < 8; i++)
                if (lst >> i & 1) {
                    wr32(c, ad, (i == (int)rb && i != first) ? nb : CPU.r[i]);
                    ad += 4;
                }
            CPU.r[rb] = nb;
        }
        return;
    }
    if (k == 0x1A || k == 0x1B) {
        uint32_t c = (op >> 8) & 15;
        if (c == 15) {
            exception(c, 0x13, 0x08, pc + 2);
            return;
        }
        if (c == 14) {
            stop(c, "undefined Thumb encoding", pc, op);
            return;
        }
        if (cond_ok(c, c))
            set_pc(c, pc + 4 + (uint32_t)((int32_t)(int8_t)(op & 0xFF) * 2), true);
        return;
    }
    if (k == 0x1C) {
        set_pc(c, pc + 4 + (uint32_t)(((int32_t)(op << 21)) >> 20), true);
        return;
    }
    if (k == 0x1E) { /* BL / BLX prefix */
        CPU.r[14] = pc + 4 + (uint32_t)(((int32_t)(op << 21)) >> 9);
        return;
    }
    if (k == 0x1F) { /* BL suffix */
        uint32_t t = CPU.r[14] + (op & 0x7FF) * 2;
        CPU.r[14] = (pc + 2) | 1;
        set_pc(c, t, true);
        return;
    }
    if (k == 0x1D) { /* BLX suffix: to ARM */
        uint32_t t = (CPU.r[14] + (op & 0x7FF) * 2) & ~3u;
        CPU.r[14] = (pc + 2) | 1;
        set_pc(c, t, false);
        return;
    }
    stop(c, "undefined Thumb encoding", pc, op);
}

void nds_cpu_step(nds_arm *c)
{
    bool thumb = CPU.cpsr_ctl & T_BIT;
    uint32_t pc = CPU.r[15] - (thumb ? 4 : 8);
    c->h.now += 1;
    c->trace[c->trace_at++ & 63] = pc | thumb;
    if (c->id && pc < 0x4000u) {
        if (pc == NDS_BIOS7_SWI)
            bios_swi(c);
        else if (pc == NDS_BIOS7_IRQ)
            bios_irq(c);
        else if (pc == NDS_BIOS7_IRQ_RET)
            bios_irq_return(c);
        else
            stop(c, "jump into the BIOS (no image: HLE only)", pc, 0);
        return;
    }
    if (!c->id && pc >= 0xFFFF0000u) {
        if (pc == NDS_BIOS9_SWI)
            bios_swi(c);
        else if (pc == NDS_BIOS9_IRQ)
            bios_irq(c);
        else if (pc == NDS_BIOS9_IRQ_RET)
            bios_irq_return(c);
        else
            stop(c, "jump into the BIOS (no image: HLE only)", pc, 0);
        return;
    }
    if (thumb) {
        uint32_t op = nds_read16(c, pc);
        CPU.r[15] = pc + 4;
        c->branched = false;
        thumb_execute(c, op, pc);
        if (!c->branched)
            CPU.r[15] = pc + 6;
    } else {
        uint32_t op = rd32(c, pc);
        CPU.r[15] = pc + 8;
        c->branched = false;
        arm_execute(c, op, pc);
        if (!c->branched)
            CPU.r[15] = pc + 12;
    }
}
