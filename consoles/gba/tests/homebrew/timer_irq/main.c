/* GBA timer_irq (XBA-149): interrupts through libtonc's IRQ switchboard and VBlankIntrWait (BIOS SWI 0x05).
 *   phase A, 60 frames: VBlank + timer 0 overflow IRQs, timer 1 cascaded on timer 0
 *   phase B, 10 frames: VBlank + HBlank IRQs
 * Result protocol: README.md. Our own code. */
#include <tonc.h>

#include "../common/xba_result.h"

#define FRAMES_A 60
#define FRAMES_B 10

static volatile u32 vblanks, hblanks, t0_irqs, total_vblanks;

static void on_vblank(void)
{
    vblanks++;
    XBA_RESULT->frames = ++total_vblanks;
}
static void on_hblank(void) { hblanks++; }
static void on_timer0(void) { t0_irqs++; }

static void sync_frame(void)
{
    VBlankIntrWait();
    vblanks = 0;
}

int main(void)
{
    xba_begin(XBA_ID_TIMER);
    REG_DISPCNT = DCNT_MODE0;

    irq_init(NULL);
    irq_add(II_VBLANK, on_vblank);

    /* Phase A. Timer 0: 1024-cycle ticks, overflow every 256 ticks = 262144 cycles.
     * 60 frames * 280896 cycles = 16853760 cycles = 64.3 overflows. Timer 1 counts them. */
    sync_frame();
    irq_add(II_TIMER0, on_timer0);
    REG_TM0CNT = 0;
    REG_TM1CNT = 0;
    REG_TM1D = 0;
    REG_TM1CNT = TM_CASCADE | TM_ENABLE;
    REG_TM0D = 0x10000 - 256;
    REG_TM0CNT = TM_FREQ_1024 | TM_IRQ | TM_ENABLE;
    while (vblanks < FRAMES_A)
        VBlankIntrWait();
    REG_TM0CNT = 0;
    irq_delete(II_TIMER0);
    u32 t1 = REG_TM1D;
    u32 t0 = t0_irqs;

    /* Phase B. HBlank fires on all 228 lines of a frame: 2280 in 10 frames. */
    sync_frame();
    hblanks = 0;
    irq_add(II_HBLANK, on_hblank);
    while (vblanks < FRAMES_B)
        VBlankIntrWait();
    irq_delete(II_HBLANK);
    u32 hb = hblanks;

    u32 i = 0;
    xba_check(i++, (t0 >= 63 && t0 <= 65), 1);
    xba_check(i++, (t1 >= 63 && t1 <= 65), 1);
    xba_check(i++, (t1 + 1 >= t0 && t1 <= t0 + 1), 1);
    xba_check(i++, (hb + 228 >= 2280 && hb <= 2280 + 228), 1);
    xba_check(i++, t0, t0); /* raw counts, informative */
    xba_check(i++, t1, t1);
    xba_check(i++, hb, hb);
    xba_finish();
}
