/* GBA BIOS support (XBA-153): direct boot, and the built-in BIOS used when no BIOS image is given.
 *
 * The built-in BIOS is our own: a few dozen ARM words (vectors, the IRQ dispatcher every game relies on, the SWI
 * dispatcher, Halt / Stop and IntrWait / VBlankIntrWait, which must run as code so interrupts can be taken while
 * waiting) plus the other SWIs implemented in C (HLE, reached through one stub each), from their behaviour as
 * documented in GBATEK « BIOS Functions ». SWIs take the Nintendo BIOS's path and cycles: its dispatcher as code, the
 * HLE routines charging what the BIOS's loops cost (XBA-205, fitted to the mGBA suite's hardware timings). The IRQ
 * dispatcher follows the documented BIOS contract: push r0-r3, r12, lr; call the handler at [0x03007FFC];
 * pop; return. Games acknowledge their interrupts in BIOS_IF at 0x03007FF8, which IntrWait polls.
 *
 * With a real BIOS image (the open-source Cult-of-GBA BIOS, tools/consoles/build_gba_bios.sh), none of this is
 * used: SWIs and IRQs go through the image (LLE). */
#include "gba_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Built-in BIOS words, {address, opcode}, hand-assembled; the source is in the comments. The IRQ dispatcher and
 * the SWI entry / exit sit at the addresses GBATEK documents for the BIOS, so the "last fetched BIOS opcode"
 * that games read back from the protected BIOS ([0x134+8] during an IRQ, [0x13C+8] after one, [0x188+8] after a
 * SWI, [0xDC+8] after boot) has the documented values. */
static const uint32_t builtin_code[][2] = {
    /* vectors */
    {0x000, 0xEA0000BE}, /* b     reset (0x300) */
    {0x004, 0xE1B0F00E}, /* movs  pc, lr                undefined: return */
    {0x008, 0xEA00004C}, /* b     swi (0x140) */
    {0x00C, 0xE25EF004}, /* subs  pc, lr, #4            prefetch abort */
    {0x010, 0xE25EF008}, /* subs  pc, lr, #8            data abort */
    {0x014, 0xEAFFFFFE}, /* b     . */
    {0x018, 0xEA000042}, /* b     irq (0x128) */
    {0x01C, 0xE25EF004}, /* subs  pc, lr, #4            FIQ */
    {0x0E4, 0xE129F000}, /* (value read back after boot) */
    /* irq */
    {0x128, 0xE92D500F}, /* stmfd sp!, {r0-r3, r12, lr} */
    {0x12C, 0xE3A00301}, /* mov   r0, #0x04000000 */
    {0x130, 0xE28FE000}, /* add   lr, pc, #0            lr = 0x138 */
    {0x134, 0xE510F004}, /* ldr   pc, [r0, #-4]         handler at 0x03007FFC */
    {0x138, 0xE8BD500F}, /* ldmfd sp!, {r0-r3, r12, lr} */
    {0x13C, 0xE25EF004}, /* subs  pc, lr, #4 */
    /* swi: the Nintendo BIOS's dispatcher, instruction for instruction (so SWIs cost what they cost there): save
     * r11, r12, lr and SPSR on the supervisor stack, fetch the function from the table by the comment byte, run it in
     * system mode with the caller's IRQ mask (r2 and lr saved on that stack), return with MOVS at 0x188. */
    {0x140, 0xE92D5800}, /* stmfd sp!, {r11, r12, lr} */
    {0x144, 0xE55EC002}, /* ldrb  r12, [lr, #-2]        comment byte, ARM or Thumb */
    {0x148, 0xE28FBFAC}, /* add   r11, pc, #0x2B0       r11 = the table (0x400) */
    {0x14C, 0xE79BC10C}, /* ldr   r12, [r11, r12, lsl #2] */
    {0x150, 0xE14FB000}, /* mrs   r11, spsr */
    {0x154, 0xE92D0800}, /* stmfd sp!, {r11} */
    {0x158, 0xE20BB080}, /* and   r11, r11, #0x80 */
    {0x15C, 0xE38BB01F}, /* orr   r11, r11, #0x1F */
    {0x160, 0xE129F00B}, /* msr   cpsr_fc, r11          system mode, the caller's I */
    {0x164, 0xE92D4004}, /* stmfd sp!, {r2, lr} */
    {0x168, 0xE28FE000}, /* add   lr, pc, #0            lr = 0x170 */
    {0x16C, 0xE12FFF1C}, /* bx    r12 */
    {0x170, 0xE8BD4004}, /* ldmfd sp!, {r2, lr} */
    {0x174, 0xE3A0C0D3}, /* mov   r12, #0xD3 */
    {0x178, 0xE129F00C}, /* msr   cpsr_fc, r12          back to supervisor mode, IRQs off */
    {0x17C, 0xE8BD0800}, /* ldmfd sp!, {r11} */
    {0x180, 0xE169F00B}, /* msr   spsr_fc, r11 */
    {0x184, 0xE8BD5800}, /* ldmfd sp!, {r11, r12, lr} */
    {0x188, 0xE1B0F00E}, /* movs  pc, lr */
    {0x190, 0xE3A02004}, /* (value read back after a SWI) */
    /* VBlankIntrWait / IntrWait (r0: discard old flags, r1: the flags): halt until the IRQ handler has set one of
     * them in BIOS_IF (0x03FFFFF8 = 0x03007FF8), acknowledging it there with IME off; run in system mode with the
     * caller's IRQ mask, as the dispatcher calls them. The Nintendo BIOS's loop, call for call: halt, BL to the
     * check (which sets r12 again), again while none came; a wake costs what it costs there (timer count-up). */
    {0x200, 0xE3A00001}, /* vblankintrwait: mov r0, #1 */
    {0x204, 0xE3A01001}, /* mov   r1, #1 */
    {0x208, 0xE92D4010}, /* intrwait: stmfd sp!, {r4, lr} */
    {0x20C, 0xE3A03000}, /* mov   r3, #0 */
    {0x210, 0xE3A04001}, /* mov   r4, #1 */
    {0x214, 0xE3A0C301}, /* mov   r12, #0x04000000 */
    {0x218, 0xE3500000}, /* cmp   r0, #0 */
    {0x21C, 0x1B000004}, /* blne  check (0x234)         discard what already came */
    {0x220, 0xE5CC3301}, /* halt: strb r3, [r12, #0x301]  HALTCNT */
    {0x224, 0xEB000002}, /* bl    check (0x234) */
    {0x228, 0x0AFFFFFC}, /* beq   halt (0x220) */
    {0x22C, 0xE8BD4010}, /* ldmfd sp!, {r4, lr} */
    {0x230, 0xE12FFF1E}, /* bx    lr */
    {0x234, 0xE3A0C301}, /* check: mov r12, #0x04000000 */
    {0x238, 0xE5CC3208}, /* strb  r3, [r12, #0x208]     IME = 0 */
    {0x23C, 0xE15C20B8}, /* ldrh  r2, [r12, #-8]        BIOS_IF */
    {0x240, 0xE0110002}, /* ands  r0, r1, r2 */
    {0x244, 0x10222000}, /* eorne r2, r2, r0 */
    {0x248, 0x114C20B8}, /* strneh r2, [r12, #-8] */
    {0x24C, 0xE5CC4208}, /* strb  r4, [r12, #0x208]     IME = 1 */
    {0x250, 0xE12FFF1E}, /* bx    lr */
    /* Halt, Stop, CustomHalt (r2 = HALTCNT value; the dispatcher keeps r2) */
    {0x2C0, 0xE3A02000}, /* halt: mov r2, #0 */
    {0x2C4, 0xE3A0C301}, /* mov   r12, #0x04000000 */
    {0x2C8, 0xE5CC2301}, /* strb  r2, [r12, #0x301]     HALTCNT */
    {0x2CC, 0xE12FFF1E}, /* bx    lr */
    {0x2D0, 0xE3A02080}, /* stop: mov r2, #0x80 */
    {0x2D4, 0xE3A0C301}, /* mov   r12, #0x04000000 */
    {0x2D8, 0xE5CC2301}, /* strb  r2, [r12, #0x301] */
    {0x2DC, 0xE12FFF1E}, /* bx    lr */
    {0x2E0, 0xE3A0C301}, /* customhalt: mov r12, #0x04000000 */
    {0x2E4, 0xE5CC2301}, /* strb  r2, [r12, #0x301] */
    {0x2E8, 0xE12FFF1E}, /* bx    lr */
    /* reset: SoftReset in C */
    {0x300, 0xE3A0C000}, /* mov   r12, #0 */
    {0x304, GBA_HLE_TRAP}, /* hle   r12 */
};

/* The function table (256 entries, by comment byte) and the HLE stubs: SWI n runs `hle; bx lr` at GBA_HLE_STUBS + 8n,
 * the trap runs it in C (gba_bios_hle_swi). Halt, Stop, CustomHalt and the interrupt waits are code above. */
#define GBA_HLE_TABLE 0x400u

void gba_bios_install_builtin(gba_machine *m)
{
    memset(m->bios, 0, sizeof m->bios);
    for (size_t i = 0; i < sizeof builtin_code / sizeof builtin_code[0]; i++)
        wr32(&m->bios[builtin_code[i][0]], builtin_code[i][1]);
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t fn = GBA_HLE_STUBS + 8 * n;
        wr32(&m->bios[fn], GBA_HLE_TRAP);
        wr32(&m->bios[fn + 4], 0xE12FFF1E); /* bx lr */
        switch (n) {
        case 0x02: fn = 0x2C0; break;
        case 0x03: fn = 0x2D0; break;
        case 0x04: fn = 0x208; break;
        case 0x05: fn = 0x200; break;
        case 0x27: fn = 0x2E0; break;
        default: break;
        }
        wr32(&m->bios[GBA_HLE_TABLE + 4 * n], fn);
    }
    m->builtin_bios = true;
}

/* Stacks and modes as the BIOS leaves them before jumping to the cartridge. */
static void boot_state(gba_machine *m, uint32_t entry)
{
    gba_cpu_reset(m, entry, MODE_SVC | CPSR_I | CPSR_F);
    m->cpu.r[13] = 0x03007FE0;
    gba_set_mode(m, MODE_IRQ);
    m->cpu.r[13] = 0x03007FA0;
    gba_set_mode(m, MODE_SYS);
    m->cpu.r[13] = 0x03007F00;
    m->cpu.cpsr_ctl = MODE_SYS;
    gba_cpu_jump(m, entry, false);
}

void gba_bios_direct_boot(gba_machine *m)
{
    boot_state(m, 0x08000000);
    m->io[0x300] = 1; /* POSTFLG */
    m->bios_latch = 0xE129F000;
}

/* ---- HLE SWIs ------------------------------------------------------------------------------------------- */

#define R (m->cpu.r)

static uint32_t rd8m(gba_machine *m, uint32_t a) { return gba_read8(m, a, true); }
static uint32_t rd16m(gba_machine *m, uint32_t a) { return gba_read16(m, a & ~1u, true); }
static uint32_t rd32m(gba_machine *m, uint32_t a) { return gba_read32(m, a & ~3u, true); }

static void soft_reset(gba_machine *m)
{
    bool ewram = gba_peek8(m, 0x03007FFA) != 0;
    memset(&m->iwram[0x7E00], 0, 0x200);
    gba_cpu_flush_blocks(m);
    gba_cpu_reset(m, 0, MODE_SVC | CPSR_I | CPSR_F);
    boot_state(m, ewram ? 0x02000000 : 0x08000000);
    m->cpu.branched = true; /* the SWI must not advance PC past the new entry */
}

static void register_ram_reset(gba_machine *m, uint32_t flags)
{
    wr16(&m->io[0], 0x0080); /* DISPCNT: forced blank */
    if (flags & 0x01)
        memset(m->ewram, 0, sizeof m->ewram);
    if (flags & 0x02)
        memset(m->iwram, 0, 0x7E00);
    if (flags & 0x03)
        gba_cpu_flush_blocks(m);
    if (flags & 0x04)
        memset(m->pal, 0, sizeof m->pal);
    if (flags & 0x08)
        memset(m->vram, 0, sizeof m->vram);
    if (flags & 0x10)
        memset(m->oam, 0, sizeof m->oam);
    if (flags & 0x20) {
        memset(&m->io[0x120], 0, 0x40);
        wr16(&m->io[0x134], 0x8000);
    }
    if (flags & 0x40) { /* sound registers: master off (clears the PSG), SOUNDCNT_H and both FIFOs */
        gba_apu_write8(m, 0x04000084, 0);
        gba_apu_write8(m, 0x04000082, 0);
        gba_apu_write8(m, 0x04000083, 0x88);
    }
    if (flags & 0x80) {
        memset(&m->io[2], 0, 0x5E);
        wr16(&m->io[0x20], 0x100);
        wr16(&m->io[0x26], 0x100);
        wr16(&m->io[0x30], 0x100);
        wr16(&m->io[0x36], 0x100);
    }
}

static void bios_div(gba_machine *m, int32_t num, int32_t den)
{
    if (den == 0) { /* the BIOS loops for a long time and returns garbage; pick something stable */
        R[0] = num < 0 ? 0xFFFFFFFFu : 1;
        R[1] = (uint32_t)num;
        R[3] = 1;
        return;
    }
    int64_t q = (int64_t)num / den, r = (int64_t)num % den;
    R[0] = (uint32_t)q;
    R[1] = (uint32_t)r;
    R[3] = (uint32_t)(q < 0 ? -q : q);
}

/* ---- the BIOS's cycles: what its routines cost beyond the HLE stub (trap; bx lr), so a SWI costs what it costs on
 * hardware. The dispatcher runs as code (builtin_code); the math routines follow the Nintendo BIOS's loops (the shift
 * loop and the subtract loop of its division: 5 and 8 cycles an iteration; Sqrt: Newton steps on that division),
 * whose iteration counts and the constants were checked against the mGBA suite's hardware timings (XBA-205: BIOS
 * Division / Sqrt / ArcTan, CpuFastSet, every wait-state column). */

/* The division loops on |num| / |den| (unsigned): the shifts align den under num, then one subtract a bit. */
static uint32_t div_loop_cycles(uint32_t num, uint32_t den)
{
    uint32_t c = 0, r2 = den;
    for (int k = 0; k < 32; k++) { /* cmp r2, r0, lsr #1; movls r2, r2, lsl #1; bcc */
        bool cs = r2 >= (num >> 1), ls = r2 <= (num >> 1);
        if (ls)
            r2 <<= 1;
        if (cs) {
            c += 3;
            break;
        }
        c += 5;
    }
    for (int k = 0; k < 33; k++) { /* cmp; adc; subcs; teq r2, r1; movne r2, r2, lsr #1; bne */
        if (num >= r2)
            num -= r2;
        if (r2 == den) {
            c += 6;
            break;
        }
        r2 >>= 1;
        c += 8;
    }
    return c;
}

static uint32_t div_cycles(int32_t num, int32_t den)
{
    uint32_t n = num < 0 ? 0u - (uint32_t)num : (uint32_t)num, d = den < 0 ? 0u - (uint32_t)den : (uint32_t)den;
    return 9 + div_loop_cycles(n, d);
}

/* Sqrt: r1 = the power of two above sqrt(x) (halve x, double r1), then r1 = (r1 + x / r1) / 2 until it stops
 * falling. */
static uint32_t sqrt_cycles(uint32_t x)
{
    uint32_t c = 7, v = x, r1 = 1;
    while (v > r1) { /* cmp; movhi; movhi; bhi */
        v >>= 1;
        r1 <<= 1;
        c += 6;
    }
    c += 4;
    for (int k = 0; k < 64; k++) {
        uint32_t prev = r1, q = r1 ? x / r1 : 0;
        c += 4 + div_loop_cycles(x, r1) + 3; /* mov x4; the division; add; movs lsr #1; cmp */
        r1 = (r1 + q) >> 1;
        if (r1 >= prev) {
            c += 1;
            break;
        }
        c += 3;
    }
    return c;
}

/* ArcTan: straight-line code, nine MULs whose cycles depend on their Rs (t, then the polynomial's terms). */
static uint32_t mul_m(int32_t rs)
{
    uint32_t v = rs < 0 ? ~(uint32_t)rs : (uint32_t)rs;
    return v < 0x100 ? 1 : v < 0x10000 ? 2 : v < 0x1000000 ? 3 : 4;
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
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
    return r;
}

/* 32-bit ARM arithmetic: MUL wraps, ASR keeps the sign. */
static inline int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static inline int32_t asr32(int32_t v, int n) { return v < 0 ? ~(~v >> n) : v >> n; }

/* ArcTan (SWI 0x09): the BIOS's polynomial (GBATEK) on the whole 32-bit r0, in 32-bit arithmetic as the BIOS runs it,
 * result in 0x4000 = pi/2 units. Like the BIOS it leaves -(t * t) >> 14 in r1 and the polynomial in r3 (the mGBA
 * test suite checks all three, XBA-201). */
static int32_t arctan(gba_machine *m, int32_t t)
{
    static const int32_t k[7] = {0x390, 0x91C, 0xFB6, 0x16AA, 0x2081, 0x3651, 0xA2F9};
    uint32_t cyc = 32 + 2 * mul_m(t) + mul_m(0xA9); /* t * t and the last MUL by t, the first term */
    int32_t a = -asr32(mul32(t, t), 14);
    int32_t b = asr32(mul32(0xA9, a), 14) + k[0];
    for (int i = 1; i < 7; i++) {
        cyc += mul_m(b);
        b = asr32(mul32(b, a), 14) + k[i];
    }
    R[1] = (uint32_t)a;
    R[3] = (uint32_t)b;
    m->now += cyc;
    return asr32(mul32(t, b), 16);
}

/* ArcTan2 (SWI 0x0A), r0 = x, r1 = y: the octant method of the Cult-of-GBA BIOS (MIT, bios_calls/math/arctan.s), which
 * gives the Nintendo BIOS's results: the angle of the octant plus ArcTan of y / x (or -x / y), computed with Div and
 * ArcTan, so r1 is what ArcTan leaves; r3 is always 0x170 on return. With x = 0 the BIOS returns before its Div:
 * r1 keeps y. Result in [0, 0x10000) for [0, 2 pi). */
static void arctan2(gba_machine *m, int32_t x, int32_t y)
{
    static const uint16_t octant[8] = {0x0000, 0x0040, 0x0040, 0x0080, 0x0080, 0x00C0, 0x00C0, 0x0100};
    int idx = 0;
    int32_t ay = y, ax = x;
    if (y < 0) {
        idx += 4;
        ay = -y;
    }
    int32_t sx = x < 0 ? -1 : 0;
    int32_t e = y ^ sx;
    if (e < 0)
        idx += 2;
    if (x < 0)
        ax = -x;
    int32_t d = ax - ay; /* >= 0: |x| >= |y|, y / x; else x / y, negated after */
    int32_t num = d >= 0 ? y : x, den = d >= 0 ? x : y;
    if ((e ^ d) < 0)
        idx += 1;
    m->now += 30 + div_cycles(mul32(num, 1 << 14), den); /* the octant logic and the division (ArcTan adds its own) */
    bios_div(m, mul32(num, 1 << 14), den);
    int32_t r = arctan(m, (int32_t)R[0]);
    if (d < 0)
        r = -r;
    R[0] = (uint32_t)(r + (octant[idx] << 8));
    R[3] = 0x170;
    if (x == 0)
        R[1] = (uint32_t)y;
}

/* CpuSet / CpuFastSet as the BIOS runs them (its loops: LDRH / STRH, LDM / STM, LDR / STR; Cult-of-GBA's
 * bios_calls/cpu_set.s, MIT): unaligned addresses behave as those instructions do (a rotated LDRH / LDR, LDM / STM
 * aligned, the save chip's 8-bit bus seeing the whole address). */
static inline uint32_t ror32b(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}
static uint32_t ldrh_rot(gba_machine *m, uint32_t a) { return ror32b(gba_read16(m, a, true), (a & 1) * 8); }
static uint32_t ldr_rot(gba_machine *m, uint32_t a) { return ror32b(gba_read32(m, a, true), (a & 3) * 8); }

static void cpu_set(gba_machine *m, uint32_t src, uint32_t dst, uint32_t ctl)
{
    uint32_t count = ctl & 0x1FFFFF;
    bool fill = ctl & (1u << 24);
    m->now += 40 + 7 * count; /* the setup, and a load / store / subs / bgt loop an item (beyond the accesses) */
    if (ctl & (1u << 26)) {
        uint32_t v = fill ? ldr_rot(m, src) : 0;
        for (uint32_t i = 0; i < count; i++, dst += 4) {
            if (fill) {
                gba_write32(m, dst, v, true); /* STR */
            } else {
                gba_write32(m, dst & ~3u, gba_read32(m, (src + 4 * i) & ~3u, true), true); /* LDMIA / STMIA */
            }
        }
    } else {
        uint16_t v = fill ? (uint16_t)ldrh_rot(m, src) : 0;
        for (uint32_t i = 0; i < count; i++, dst += 2) {
            if (!fill)
                v = (uint16_t)ldrh_rot(m, src + 2 * i);
            gba_write16(m, dst, v, true);
        }
    }
}

static void cpu_fast_set(gba_machine *m, uint32_t src, uint32_t dst, uint32_t ctl)
{
    uint32_t count = ((ctl & 0x1FFFFF) + 7) & ~7u; /* whole blocks of 8 words */
    if (!(ctl & 0x1FFFFF))
        return;
    bool fill = (ctl & (1u << 24)) != 0;
    /* The setup, then per block of 8 words an LDMIA (fetch + internal cycle) / STMIA (fetch) / SUBS / BGT. */
    m->now += 44 + (fill ? 5 : 7) * (count / 8) - 2;
    uint32_t v = fill ? ldr_rot(m, src) : 0;
    for (uint32_t i = 0; i < count; i++) {
        bool seq = (i & 7) != 0;
        if (!fill)
            v = gba_read32(m, (src + 4 * i) & ~3u, seq);
        gba_write32(m, (dst + 4 * i) & ~3u, v, seq);
    }
}

static void bg_affine_set(gba_machine *m, uint32_t src, uint32_t dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++, src += 20, dst += 16) {
        int32_t ox = (int32_t)rd32m(m, src), oy = (int32_t)rd32m(m, src + 4);
        int32_t cx = (int16_t)rd16m(m, src + 8), cy = (int16_t)rd16m(m, src + 10);
        int32_t sx = (int16_t)rd16m(m, src + 12), sy = (int16_t)rd16m(m, src + 14);
        uint32_t th = (rd16m(m, src + 16) >> 8) & 0xFF;
        int32_t s = m->sine[th], c = m->sine[(th + 64) & 0xFF];
        int32_t pa = (sx * c) >> 14, pb = -((sx * s) >> 14), pc = (sy * s) >> 14, pd = (sy * c) >> 14;
        gba_write16(m, dst, (uint16_t)pa, true);
        gba_write16(m, dst + 2, (uint16_t)pb, true);
        gba_write16(m, dst + 4, (uint16_t)pc, true);
        gba_write16(m, dst + 6, (uint16_t)pd, true);
        gba_write32(m, dst + 8, (uint32_t)(ox - (pa * cx + pb * cy)), true);
        gba_write32(m, dst + 12, (uint32_t)(oy - (pc * cx + pd * cy)), true);
    }
}

static void obj_affine_set(gba_machine *m, uint32_t src, uint32_t dst, uint32_t n, uint32_t stride)
{
    for (uint32_t i = 0; i < n; i++, src += 8, dst += 4 * stride) {
        int32_t sx = (int16_t)rd16m(m, src), sy = (int16_t)rd16m(m, src + 2);
        uint32_t th = (rd16m(m, src + 4) >> 8) & 0xFF;
        int32_t s = m->sine[th], c = m->sine[(th + 64) & 0xFF];
        gba_write16(m, dst, (uint16_t)((sx * c) >> 14), true);
        gba_write16(m, dst + stride, (uint16_t)(-((sx * s) >> 14)), true);
        gba_write16(m, dst + 2 * stride, (uint16_t)((sy * s) >> 14), true);
        gba_write16(m, dst + 3 * stride, (uint16_t)((sy * c) >> 14), true);
    }
}

static void bit_unpack(gba_machine *m, uint32_t src, uint32_t dst, uint32_t info)
{
    uint32_t len = rd16m(m, info), sw = rd8m(m, info + 2), dw = rd8m(m, info + 3), off = rd32m(m, info + 4);
    bool zero = off >> 31;
    off &= 0x7FFFFFFF;
    if (!sw || sw > 8 || (sw & (sw - 1)) || !dw || dw > 32 || (dw & (dw - 1)))
        return;
    uint32_t out = 0, outbits = 0;
    dst &= ~3u;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t b = rd8m(m, src + i);
        for (uint32_t bit = 0; bit < 8; bit += sw) {
            uint32_t v = (b >> bit) & ((1u << sw) - 1);
            if (v || zero)
                v += off;
            out |= (dw == 32 ? v : v & ((1u << dw) - 1)) << outbits;
            outbits += dw;
            if (outbits >= 32) {
                gba_write32(m, dst, out, true);
                dst += 4;
                out = 0;
                outbits = 0;
            }
        }
    }
}

/* Decompressors: decode into a buffer, then write it out (bytes for WRAM, halfwords for VRAM). */
static void write_out(gba_machine *m, uint32_t dst, const uint8_t *buf, uint32_t n, int width)
{
    if (width == 1) {
        for (uint32_t i = 0; i < n; i++)
            gba_write8(m, dst + i, buf[i], true);
    } else if (width == 2) {
        dst &= ~1u;
        for (uint32_t i = 0; i < n; i += 2)
            gba_write16(m, dst + i, (uint16_t)(buf[i] | (i + 1 < n ? buf[i + 1] : 0) << 8), true);
    } else {
        dst &= ~3u;
        for (uint32_t i = 0; i < n; i += 4)
            gba_write32(m, dst + i, rd32(&buf[i]), true);
    }
}

static void decompress(gba_machine *m, uint32_t num, uint32_t src, uint32_t dst)
{
    uint32_t hdr = rd32m(m, src);
    uint32_t size = hdr >> 8;
    if (!size)
        return;
    uint8_t *buf = calloc(1, size + 8);
    if (!buf)
        return;
    uint32_t s = src + 4, n = 0;
    int width = 1;
    switch (num) {
    case 0x11: case 0x12: /* LZ77 */
        width = num == 0x12 ? 2 : 1;
        while (n < size) {
            uint32_t flags = rd8m(m, s++);
            for (int b = 0; b < 8 && n < size; b++, flags <<= 1) {
                if (flags & 0x80) {
                    uint32_t hi = rd8m(m, s++), lo = rd8m(m, s++);
                    uint32_t len = (hi >> 4) + 3, disp = ((hi & 15) << 8 | lo) + 1;
                    for (uint32_t k = 0; k < len && n < size; k++, n++)
                        buf[n] = disp <= n ? buf[n - disp] : 0;
                } else {
                    buf[n++] = (uint8_t)rd8m(m, s++);
                }
            }
        }
        break;
    case 0x13: { /* Huffman, 4- or 8-bit data, 32-bit bitstream */
        uint32_t bits = hdr & 15;
        width = 4;
        if (bits != 4 && bits != 8)
            break;
        uint32_t tree = src + 5, root = tree;
        uint32_t stream = src + 4 + (rd8m(m, src + 4) + 1) * 2;
        uint32_t node = root, nodeval = rd8m(m, root);
        uint32_t acc = 0, accbits = 0;
        while (n < size) {
            uint32_t word = rd32m(m, stream);
            stream += 4;
            for (int b = 31; b >= 0 && n < size; b--) {
                uint32_t dir = (word >> b) & 1;
                uint32_t child = (node & ~1u) + (nodeval & 0x3F) * 2 + 2 + dir;
                bool leaf = nodeval & (dir ? 0x40 : 0x80);
                if (leaf) {
                    acc |= (rd8m(m, child) & ((1u << bits) - 1)) << accbits;
                    accbits += bits;
                    if (accbits == 32) {
                        wr32(&buf[n], acc);
                        n += 4;
                        acc = 0;
                        accbits = 0;
                    }
                    node = root;
                } else {
                    node = child;
                }
                nodeval = rd8m(m, node);
            }
        }
        break;
    }
    case 0x14: case 0x15: /* run-length */
        width = num == 0x15 ? 2 : 1;
        while (n < size) {
            uint32_t f = rd8m(m, s++);
            if (f & 0x80) {
                uint32_t len = (f & 0x7F) + 3;
                uint8_t v = (uint8_t)rd8m(m, s++);
                for (uint32_t k = 0; k < len && n < size; k++)
                    buf[n++] = v;
            } else {
                uint32_t len = (f & 0x7F) + 1;
                for (uint32_t k = 0; k < len && n < size; k++)
                    buf[n++] = (uint8_t)rd8m(m, s++);
            }
        }
        break;
    case 0x16: case 0x17: { /* 8-bit difference filter */
        width = num == 0x17 ? 2 : 1;
        uint8_t acc = 0;
        for (n = 0; n < size; n++) {
            acc = (uint8_t)(acc + rd8m(m, s + n));
            buf[n] = acc;
        }
        break;
    }
    default: { /* 0x18: 16-bit difference filter */
        width = 2;
        uint16_t acc = 0;
        for (n = 0; n < size; n += 2) {
            acc = (uint16_t)(acc + rd16m(m, s + n));
            wr16(&buf[n], acc);
        }
        n = size;
        break;
    }
    }
    write_out(m, dst, buf, size, width);
    free(buf);
}

void gba_bios_hle_swi(gba_machine *m, uint32_t num)
{
    switch (num) {
    case 0x00: soft_reset(m); break;
    case 0x01: register_ram_reset(m, R[0]); break;
    case 0x02: /* Halt */
        if (!(m->ie & m->if_ & 0x3FFF))
            m->cpu.halted = true;
        break;
    case 0x03: m->cpu.stopped = true; break;
    case 0x06:
        m->now += div_cycles((int32_t)R[0], (int32_t)R[1]);
        bios_div(m, (int32_t)R[0], (int32_t)R[1]);
        break;
    case 0x07: /* DivArm: three MOVs to swap, then Div */
        m->now += 3 + div_cycles((int32_t)R[1], (int32_t)R[0]);
        bios_div(m, (int32_t)R[1], (int32_t)R[0]);
        break;
    case 0x08:
        m->now += sqrt_cycles(R[0]);
        R[0] = isqrt(R[0]);
        break;
    case 0x09: R[0] = (uint32_t)arctan(m, (int32_t)R[0]); break;
    case 0x0A: arctan2(m, (int32_t)R[0], (int32_t)R[1]); break;
    case 0x0B: cpu_set(m, R[0], R[1], R[2]); break;
    case 0x0C: cpu_fast_set(m, R[0], R[1], R[2]); break;
    case 0x0D: R[0] = 0xBAAE187F; break; /* GetBiosChecksum: the value games expect */
    case 0x0E: bg_affine_set(m, R[0], R[1], R[2]); break;
    case 0x0F: obj_affine_set(m, R[0], R[1], R[2], R[3]); break;
    case 0x10: bit_unpack(m, R[0], R[1], R[2]); break;
    case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18:
        decompress(m, num, R[0], R[1]);
        break;
    case 0x1F: { /* MidiKey2Freq */
        double f = rd32m(m, R[0] + 4) / pow(2.0, (180.0 - (double)R[1] - (double)R[2] / 256.0) / 12.0);
        R[0] = (uint32_t)f;
        break;
    }
    case 0x19: { /* SoundBias: level 0 or 200h (at once, where the BIOS ramps it) */
        uint16_t sb = gba_io_read16(m, 0x04000088);
        gba_io_write16(m, 0x04000088, (uint16_t)((sb & ~0x3FEu) | (R[0] ? 0x200 : 0)));
        break;
    }
    case 0x27: /* CustomHalt */
        gba_io_write8(m, 0x04000301, (uint8_t)R[2]);
        break;
    default:
        break; /* the sound driver, multiboot, ...: no-ops for now */
    }
}
