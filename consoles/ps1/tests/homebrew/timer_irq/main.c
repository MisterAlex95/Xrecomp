/* PS1 timer_irq (XBA-149): VBlank callback plus root counter interrupts, for 60 frames.
 *   timer 1: HBlank source, reset + IRQ at target 100 (repeat)
 *   timer 2: system clock / 8, IRQ on overflow (repeat)
 * Result protocol: README.md. Our own code. */
#include <stdint.h>
#include <psxapi.h>
#include <psxetc.h>
#include <psxgpu.h>
#include <hwregs_c.h>

#include "../common/xba_result.h"
#include "../common/screen.h"

#define FRAMES 60

static volatile uint32_t vblanks, t1_irqs, t2_irqs, counting;

static void on_vblank(void)
{
    if (counting) {
        vblanks++;
        XBA_RESULT->frames = vblanks;
    }
}
static void on_timer1(void) { t1_irqs += counting; }
static void on_timer2(void) { t2_irqs += counting; }

int main(void)
{
    xba_begin(XBA_ID_TIMER);
    screen_init(0, 0, 64);
    int pal = GetVideoMode() == MODE_PAL;

    TIMER_CTRL(1) = 0;
    TIMER_CTRL(2) = 0;
    EnterCriticalSection();
    InterruptCallback(IRQ_TIMER1, &on_timer1);
    InterruptCallback(IRQ_TIMER2, &on_timer2);
    VSyncCallback(&on_vblank);
    ExitCriticalSection();

    VSync(0);
    TIMER_RELOAD(1) = 100;
    TIMER_CTRL(1) = 0x0100 | 0x0040 | 0x0010 | 0x0008; /* HBlank clock, repeat, IRQ at target, reset at target */
    TIMER_CTRL(2) = 0x0200 | 0x0040 | 0x0020;          /* sysclk/8, repeat, IRQ on 0xFFFF */
    counting = 1;
    while (vblanks < FRAMES)
        VSync(0);
    counting = 0;
    TIMER_CTRL(1) = 0;
    TIMER_CTRL(2) = 0;

    /* NTSC: 263 lines per frame -> 15780 HBlanks -> 157.8 timer 1 IRQs; 60 frames = 1.001 s of
     * 4233600 Hz -> 64.7 timer 2 overflows. PAL: 314 lines -> 188.4; 1.2 s -> 77.5. */
    uint32_t t1_lo = pal ? 180 : 150, t1_hi = pal ? 196 : 165;
    uint32_t t2_lo = pal ? 74 : 62, t2_hi = pal ? 81 : 67;
    uint32_t i = 0;
    xba_check(i++, vblanks, FRAMES);
    xba_check(i++, t1_irqs >= t1_lo && t1_irqs <= t1_hi, 1);
    xba_check(i++, t2_irqs >= t2_lo && t2_irqs <= t2_hi, 1);
    xba_check(i++, t1_irqs, t1_irqs); /* raw counts, informative */
    xba_check(i++, t2_irqs, t2_irqs);
    xba_check(i++, (uint32_t)pal, (uint32_t)pal);

    screen_verdict(XBA_RESULT->detail == 0);
    xba_finish();
}
