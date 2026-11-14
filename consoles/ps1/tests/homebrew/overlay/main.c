/* PS1 overlay (XBA-149): code loaded from the disc at run time, the case the interpreter fallback
 * must cover. Reads OVL_A.BIN from the CD into 0x80100000, flushes the I-cache, calls it (it calls
 * back into this executable), then loads OVL_B.BIN over the same address and calls it again: the
 * runtime must run the new code, not what it saw there before. Result protocol: README.md.
 * Our own code. */
#include <stdint.h>
#include <psxapi.h>
#include <psxcd.h>
#include <psxgpu.h>

#include "../common/xba_result.h"
#include "../common/screen.h"

#define OVL_ADDR 0x80100000u
typedef uint32_t (*cb_t)(uint32_t);
typedef uint32_t (*ovl_fn)(uint32_t, cb_t);

static volatile uint32_t cb_calls;
static uint32_t callback(uint32_t v)
{
    cb_calls++;
    return v + 0x1000;
}

/* Returns the file size, 0 if not found, or (uint32_t)-1 if the read failed. */
static uint32_t load_overlay(const char *name)
{
    CdlFILE file;
    if (!CdSearchFile(&file, name))
        return 0;
    int sectors = (file.size + 2047) / 2048;
    CdControl(CdlSetloc, &file.pos, 0);
    CdRead(sectors, (uint32_t *)OVL_ADDR, CdlModeSpeed);
    if (CdReadSync(0, 0) < 0)
        return (uint32_t)-1;
    FlushCache(); /* new code at OVL_ADDR: drop stale I-cache lines */
    return (uint32_t)file.size;
}

int main(void)
{
    xba_begin(XBA_ID_OVL);
    screen_init(0, 0, 64);
    uint32_t i = 0;

    xba_check(i++, CdInit() == 1, 1);

    uint32_t size_a = load_overlay("\\OVL_A.BIN;1");
    xba_check(i++, size_a > 0 && size_a != (uint32_t)-1, 1);
    uint32_t ra = ((ovl_fn)OVL_ADDR)(0x21, callback);
    xba_check(i++, ra, 0xA0A01084); /* (3 * 0x21 + 0x1021) ^ 0xA0A00000 */

    uint32_t size_b = load_overlay("\\OVL_B.BIN;1");
    xba_check(i++, size_b > 0 && size_b != (uint32_t)-1, 1);
    uint32_t rb = ((ovl_fn)OVL_ADDR)(0x21, callback);
    xba_check(i++, rb, 0xB0002231); /* (0x1021 + 0x1210) | 0xB0000000 */

    xba_check(i++, cb_calls, 3);
    xba_check(i++, size_a, size_a); /* raw sizes, informative */
    xba_check(i++, size_b, size_b);

    screen_verdict(XBA_RESULT->detail == 0);
    xba_finish();
}
