/* GBA BIOS support (XBA-153): direct boot, and the built-in BIOS used when no BIOS image is given.
 *
 * The built-in BIOS is our own: a few dozen ARM words (vectors, the IRQ dispatcher every game relies on, and
 * IntrWait / VBlankIntrWait, which must run as code so interrupts can be taken while waiting) plus the other
 * SWIs implemented in C (HLE), from their behaviour as documented in GBATEK « BIOS Functions ». The IRQ
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
    /* swi: only SWI 0x04 / 0x05 come here (the others are HLE'd at the SWI instruction) */
    {0x140, 0xE92D5800}, /* stmfd sp!, {r11, r12, lr} */
    {0x144, 0xE55EC002}, /* ldrb  r12, [lr, #-2]        comment byte, ARM or Thumb */
    {0x148, 0xE35C0005}, /* cmp   r12, #5 */
    {0x14C, 0x0A00002B}, /* beq   vblankintrwait (0x200) */
    {0x150, 0xE35C0004}, /* cmp   r12, #4 */
    {0x154, 0x0A00002B}, /* beq   intrwait (0x208) */
    {0x158, GBA_HLE_TRAP}, /* hle   r12 */
    {0x15C, 0xEA000008}, /* b     swi_ret (0x184) */
    {0x184, 0xE8BD5800}, /* swi_ret: ldmfd sp!, {r11, r12, lr} */
    {0x188, 0xE1B0F00E}, /* movs  pc, lr */
    {0x190, 0xE3A02004}, /* (value read back after a SWI) */
    {0x200, 0xE3A00001}, /* vblankintrwait: mov r0, #1 */
    {0x204, 0xE3A01001}, /* mov   r1, #1 */
    {0x208, 0xE92D000C}, /* intrwait: stmfd sp!, {r2, r3} */
    {0x20C, 0xE3A03301}, /* mov   r3, #0x04000000 */
    {0x210, 0xE3A02001}, /* mov   r2, #1 */
    {0x214, 0xE5832208}, /* str   r2, [r3, #0x208]      IME = 1 */
    {0x218, 0xE3500000}, /* cmp   r0, #0 */
    {0x21C, 0x115320B8}, /* ldrneh r2, [r3, #-8]        BIOS_IF (0x03FFFFF8 = 0x03007FF8) */
    {0x220, 0x11C22001}, /* bicne r2, r2, r1            discard old flags */
    {0x224, 0x114320B8}, /* strneh r2, [r3, #-8] */
    {0x228, 0xE10F2000}, /* mrs   r2, cpsr */
    {0x22C, 0xE3C22080}, /* bic   r2, r2, #0x80 */
    {0x230, 0xE121F002}, /* msr   cpsr_c, r2            IRQs on while waiting */
    {0x234, 0xE3A02000}, /* wait: mov r2, #0 */
    {0x238, 0xE5C32301}, /* strb  r2, [r3, #0x301]      HALTCNT: halt */
    {0x23C, 0xE15320B8}, /* ldrh  r2, [r3, #-8] */
    {0x240, 0xE1120001}, /* tst   r2, r1 */
    {0x244, 0x0AFFFFFA}, /* beq   wait */
    {0x248, 0xE1C22001}, /* bic   r2, r2, r1 */
    {0x24C, 0xE14320B8}, /* strh  r2, [r3, #-8] */
    {0x250, 0xE10F2000}, /* mrs   r2, cpsr */
    {0x254, 0xE3822080}, /* orr   r2, r2, #0x80 */
    {0x258, 0xE121F002}, /* msr   cpsr_c, r2 */
    {0x25C, 0xE8BD000C}, /* ldmfd sp!, {r2, r3} */
    {0x260, 0xEAFFFFC7}, /* b     swi_ret (0x184) */
    /* reset: SoftReset in C */
    {0x300, 0xE3A0C000}, /* mov   r12, #0 */
    {0x304, GBA_HLE_TRAP}, /* hle   r12 */
};

void gba_bios_install_builtin(gba_machine *m)
{
    memset(m->bios, 0, sizeof m->bios);
    for (size_t i = 0; i < sizeof builtin_code / sizeof builtin_code[0]; i++)
        wr32(&m->bios[builtin_code[i][0]], builtin_code[i][1]);
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
    if (flags & 0x40)
        memset(&m->io[0x60], 0, 0x50);
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

/* ArcTan of a 1.14 tangent in [-1, 1]: the BIOS's polynomial (GBATEK), result in 0x4000 = pi/2 units. */
static int32_t arctan(int32_t t)
{
    int32_t a = -((t * t) >> 14);
    int32_t b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (b * t) >> 16;
}

static uint32_t arctan2(int32_t x, int32_t y)
{
    int32_t r;
    if (y == 0)
        return x >= 0 ? 0 : 0x8000;
    if (x == 0)
        return y >= 0 ? 0x4000 : 0xC000;
    int64_t ax = x < 0 ? -(int64_t)x : x, ay = y < 0 ? -(int64_t)y : y;
    if (ax >= ay) {
        r = arctan((int32_t)(((int64_t)y << 14) / x));
        if (x < 0)
            r += 0x8000;
        else if (y < 0)
            r += 0x10000;
    } else {
        r = (y > 0 ? 0x4000 : 0xC000) - arctan((int32_t)(((int64_t)x << 14) / y));
    }
    return (uint32_t)r & 0xFFFF;
}

static void cpu_set(gba_machine *m, uint32_t src, uint32_t dst, uint32_t ctl)
{
    uint32_t count = ctl & 0x1FFFFF;
    bool fill = ctl & (1u << 24);
    if (ctl & (1u << 26)) {
        src &= ~3u;
        dst &= ~3u;
        uint32_t v = fill ? rd32m(m, src) : 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!fill)
                v = rd32m(m, src + 4 * i);
            gba_write32(m, dst + 4 * i, v, true);
        }
    } else {
        src &= ~1u;
        dst &= ~1u;
        uint16_t v = fill ? (uint16_t)rd16m(m, src) : 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!fill)
                v = (uint16_t)rd16m(m, src + 2 * i);
            gba_write16(m, dst + 2 * i, v, true);
        }
    }
}

static void cpu_fast_set(gba_machine *m, uint32_t src, uint32_t dst, uint32_t ctl)
{
    uint32_t count = ((ctl & 0x1FFFFF) + 7) & ~7u;
    cpu_set(m, src, dst, (ctl & (1u << 24)) | (1u << 26) | count);
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
    case 0x06: bios_div(m, (int32_t)R[0], (int32_t)R[1]); break;
    case 0x07: bios_div(m, (int32_t)R[1], (int32_t)R[0]); break;
    case 0x08: R[0] = isqrt(R[0]); break;
    case 0x09: R[0] = (uint32_t)arctan((int16_t)R[0]) & 0xFFFF; break;
    case 0x0A: R[0] = arctan2((int16_t)R[0], (int16_t)R[1]); break;
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
    case 0x27: /* CustomHalt */
        gba_io_write8(m, 0x04000301, (uint8_t)R[2]);
        break;
    default:
        break; /* SoundBias, the sound driver, multiboot, ...: no-ops for now */
    }
}
