/* GBA iwram_copy (XBA-149): code that runs from RAM, the case the interpreter fallback must cover.
 *   1. a C function in section .iwram (copied to IWRAM by crt0 at boot, ARM state)
 *   2. pic_routine (routine.s) copied from ROM to an IWRAM buffer at run time, called
 *   3. the copy patched in place (new immediate), called again: stale code must not run
 *   4. the routine copied to EWRAM with a third immediate, called
 * Result protocol: README.md. Our own code. */
#include <tonc.h>

#include "../common/xba_result.h"

extern const u32 pic_routine[], pic_routine_end[];
extern const u32 pic_patch_offset;
typedef u32 (*routine_fn)(u32);

static u32 iwram_buf[16] __attribute__((aligned(4)));             /* .bss lives in IWRAM */
static u32 ewram_buf[16] __attribute__((aligned(4), section(".ewram")));

__attribute__((section(".iwram"), long_call, target("arm"), noinline))
u32 iwram_fn(u32 a, u32 b)
{
    u32 r = 0;
    for (u32 i = 0; i < a; i++)
        r = (r << 1 | r >> 31) ^ (b + i);
    return r;
}

static routine_fn install(u32 *dst, u32 imm)
{
    u32 words = (u32)(pic_routine_end - pic_routine);
    for (u32 i = 0; i < words; i++)
        dst[i] = pic_routine[i];
    u32 *site = dst + pic_patch_offset / 4;
    *site = (*site & ~0xFFu) | imm;
    return (routine_fn)dst; /* even address: BX switches to ARM */
}

static u32 ref_iwram_fn(u32 a, u32 b)
{
    u32 r = 0;
    for (u32 i = 0; i < a; i++)
        r = (r << 1 | r >> 31) ^ (b + i);
    return r;
}

static volatile u32 v_a = 20, v_b = 0x1234;

int main(void)
{
    xba_begin(XBA_ID_IWRAM);

    u32 i = 0;
    xba_check(i++, ((u32)iwram_fn >> 24) == 0x03, 1); /* really placed in IWRAM */
    xba_check(i++, iwram_fn(v_a, v_b), ref_iwram_fn(v_a, v_b));

    routine_fn f = install(iwram_buf, 0x11);
    xba_check(i++, f(0x100), 0x5A5A0311);
    f = install(iwram_buf, 0x22); /* same address, new code */
    xba_check(i++, f(0x100), 0x5A5A0322);
    iwram_buf[pic_patch_offset / 4] = (iwram_buf[pic_patch_offset / 4] & ~0xFFu) | 0x44;
    xba_check(i++, f(0x200), 0x5A5A0644); /* single-word patch only */

    routine_fn g = install(ewram_buf, 0x33);
    xba_check(i++, ((u32)ewram_buf >> 24) == 0x02, 1);
    xba_check(i++, g(0x100), 0x5A5A0333);
    xba_check(i++, f(0x100), 0x5A5A0344); /* the IWRAM copy is still the patched one */
    /* XBA-281: the EWRAM copy (which no pack lifts: the interpreter decoded it into a block above) patched in place
     * by cartridge code, then the IWRAM copy re-installed with the first immediate: both run their new bytes. */
    ewram_buf[pic_patch_offset / 4] = (ewram_buf[pic_patch_offset / 4] & ~0xFFu) | 0x55;
    xba_check(i++, g(0x100), 0x5A5A0355);
    for (u32 k = 0; k < 4; k++) {
        ewram_buf[pic_patch_offset / 4] = (ewram_buf[pic_patch_offset / 4] & ~0xFFu) | (0x60 + k);
        xba_check(i, g(0x100 + k), 0x5A5A0300 + 3 * k + 0x60 + k);
    }
    i++;
    f = install(iwram_buf, 0x11);
    xba_check(i++, f(0x100), 0x5A5A0311);

    REG_DISPCNT = DCNT_MODE0; /* the verdict on screen (gba_corpus_packs.py screenshots it) */
    pal_bg_mem[0] = XBA_RESULT->detail ? RGB15(31, 0, 0) : RGB15(0, 31, 0);
    xba_finish();
}
