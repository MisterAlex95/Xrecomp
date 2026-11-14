/* GBA sound (XBA-155): the APU's channels and Direct Sound, audible (gba_run --wav) and self-checking.
 *   phase A, 60 frames: square 1 at 440 Hz (sweep off, no envelope); square 2 beeps with length 0 (1/4 s)
 *   phase B, 30 frames: noise (15-bit), envelope down
 *   phase C, 60 frames: wave channel, a 32-sample triangle at 220 Hz
 *   phase D, 90 frames: Direct Sound A, a 440 + 660 Hz chord at 16384 Hz streamed by DMA1 on timer 0
 * then sound master off. Registers are written by address (GBATEK « GBA Sound Controller »).
 * Result protocol: README.md. Our own code. */
#include <tonc.h>

#include "../common/xba_result.h"

#define REG16(a) (*(volatile u16 *)(0x04000000u + (a)))
#define REG32(a) (*(volatile u32 *)(0x04000000u + (a)))

#define SND1CNT_L REG16(0x60)
#define SND1CNT_H REG16(0x62)
#define SND1CNT_X REG16(0x64)
#define SND2CNT_L REG16(0x68)
#define SND2CNT_H REG16(0x6C)
#define SND3CNT_L REG16(0x70)
#define SND3CNT_H REG16(0x72)
#define SND3CNT_X REG16(0x74)
#define SND4CNT_L REG16(0x78)
#define SND4CNT_H REG16(0x7C)
#define SNDCNT_L REG16(0x80)
#define SNDCNT_H REG16(0x82)
#define SNDCNT_X REG16(0x84)
#define SNDBIAS REG16(0x88)
#define WAVE_RAM(i) REG32(0x90 + 4 * (i))
#define FIFO_A_ADDR 0x040000A0u

#define PCM_RATE 16384
#define PCM_LEN (PCM_RATE * 2) /* 2 s: more than phase D plays */

static volatile u32 dma1_irqs, total_vblanks;
EWRAM_BSS static s8 pcm[PCM_LEN] __attribute__((aligned(4))); /* 32 KB: EWRAM */

static void on_vblank(void) { XBA_RESULT->frames = ++total_vblanks; }
static void on_dma1(void) { dma1_irqs++; }

static void wait_frames(int n)
{
    while (n-- > 0)
        VBlankIntrWait();
}

int main(void)
{
    xba_begin(XBA_FOURCC('S', 'N', 'D', ' '));
    REG_DISPCNT = DCNT_MODE0;
    irq_init(NULL);
    irq_add(II_VBLANK, on_vblank);

    /* The PCM: 440 Hz + 660 Hz, amplitude 60 each (sin_lut is .12 fixed, a full turn is 0x10000). */
    u32 p1 = 0, p2 = 0;
    for (int i = 0; i < PCM_LEN; i++) {
        pcm[i] = (s8)((lu_sin(p1) * 60 + lu_sin(p2) * 60) >> 12);
        p1 += 440 * 65536 / PCM_RATE;
        p2 += 660 * 65536 / PCM_RATE;
    }

    u32 bias = SNDBIAS & 0x3FE;
    SNDCNT_X = 0x80;      /* master on */
    SNDCNT_L = 0xFF77;    /* PSG 1-4 left and right, volume 7 */
    SNDCNT_H = 0x0002;    /* PSG 100 % */

    /* Phase A: square 1, 440 Hz = 131072 / (2048 - 1750); square 2 beeps 1/4 s at 880 Hz. */
    SND1CNT_L = 0;                  /* no sweep */
    SND1CNT_H = 0xA0BF;             /* volume 10, no envelope, duty 50 %, length 63 (unused) */
    u32 duty_read = SND1CNT_H;      /* length bits are write-only (GBATEK; mGBA 0.10.1 returns them) */
    SND1CNT_X = 0x8000 | 1750;
    u32 stat_a = SNDCNT_X & 0x8F;
    SND2CNT_L = 0x6080;             /* volume 6, duty 50 %, length 0 = 64 / 256 s */
    SND2CNT_H = 0xC000 | 1899;      /* trigger, length enabled; 131072 / 149 = 880 Hz */
    u32 beep_on = (SNDCNT_X >> 1) & 1;
    wait_frames(20);
    u32 beep_off = !((SNDCNT_X >> 1) & 1);
    wait_frames(40);
    SND1CNT_H = 0; /* DAC off: channel 1 stops */
    u32 sq1_off = !(SNDCNT_X & 1);

    /* Phase B: noise, 15-bit, r = 1 s = 3, envelope 15 down by 1/64 s x 2. */
    SND4CNT_L = 0xF200;
    SND4CNT_H = 0x8031;
    u32 noise_on = (SNDCNT_X >> 3) & 1;
    wait_frames(30);
    SND4CNT_L = 0; /* DAC off */

    /* Phase C: wave. Bank 0 plays while the CPU fills bank 1, then bank 1 plays and the CPU sees bank 0. */
    SND3CNT_L = 0x0000;
    static const u32 tri[4] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476};
    for (int i = 0; i < 4; i++)
        WAVE_RAM(i) = tri[i];
    SND3CNT_L = 0x0040;             /* select bank 1 (DAC still off) */
    for (int i = 0; i < 4; i++)
        WAVE_RAM(i) = 0;            /* bank 0 */
    u32 bank_ok = 1;
    SND3CNT_L = 0x0000;             /* the CPU sees bank 1 again */
    for (int i = 0; i < 4; i++)
        bank_ok &= WAVE_RAM(i) == tri[i];
    SND3CNT_L = 0x00C0;             /* play bank 1, DAC on */
    SND3CNT_H = 0x2000;             /* 100 % */
    SND3CNT_X = 0x8000 | 1748;      /* 2097152 / 300 / 32 = 218 Hz */
    u32 wave_on = (SNDCNT_X >> 2) & 1;
    wait_frames(60);
    SND3CNT_L = 0;

    /* Phase D: Direct Sound A from the PCM, DMA1 refilling FIFO A on timer 0 (16384 Hz). */
    SNDCNT_H = 0x0B0E;              /* PSG 100 %, A 100 %, A left + right, timer 0, FIFO A reset */
    REG_DMA1SAD = (u32)pcm;
    REG_DMA1DAD = FIFO_A_ADDR;
    REG_DMA1CNT = DMA_DST_FIXED | DMA_REPEAT | DMA_32 | DMA_AT_SPECIAL | DMA_IRQ | DMA_ENABLE;
    irq_add(II_DMA1, on_dma1);
    REG_TM0D = 0x10000 - (1 << 24) / PCM_RATE;
    REG_TM0CNT = TM_ENABLE;
    VBlankIntrWait();
    dma1_irqs = 0;
    wait_frames(60);
    u32 dmas = dma1_irqs; /* 16 bytes each: 60 x 280896 / 1024 / 16 = 1028.6 */
    wait_frames(30);
    REG_TM0CNT = 0;
    REG_DMA1CNT = 0;
    irq_delete(II_DMA1);

    /* Master off: the PSG registers clear. */
    SNDCNT_X = 0;
    u32 cleared = (SNDCNT_X & 0x8F) == 0 && SND2CNT_L == 0 && SNDCNT_L == 0;

    u32 i = 0;
    xba_check(i++, bias, 0x200);
    xba_check(i++, duty_read & 0xFFC0, 0xA080);
    xba_check(i++, stat_a, 0x81);
    xba_check(i++, beep_on, 1);
    xba_check(i++, beep_off, 1);
    xba_check(i++, sq1_off, 1);
    xba_check(i++, noise_on, 1);
    xba_check(i++, bank_ok, 1);
    xba_check(i++, wave_on, 1);
    xba_check(i++, dmas >= 1018 && dmas <= 1039, 1);
    xba_check(i++, cleared, 1);
    xba_check(i++, dmas, dmas); /* raw, informative */
    xba_check(i++, duty_read, duty_read);
    xba_finish();
}
