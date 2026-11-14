/* PS1 hello (XBA-149): prints a fixed text with PSn00bSDK's debug font and writes known values to
 * the result block. Result protocol: README.md. Our own code. */
#include <stdint.h>
#include <psxgpu.h>

#include "../common/xba_result.h"
#include "../common/screen.h"

static volatile uint32_t in_data = 0xC0FFEE01;
static volatile uint32_t in_bss;

int main(void)
{
    xba_begin(XBA_ID_HELLO);
    screen_init(0, 0, 64);

    FntLoad(960, 0);
    FntOpen(16, 16, 288, 200, 0, 256);
    FntPrint(-1, "XBA HELLO PS1\n\nhomebrew corpus XBA-149\n");
    FntFlush(-1);
    DrawSync(0);
    VSync(0);

    /* table[0] = known constant, table[1] = .data copied and .bss cleared,
     * table[2] = video standard reported by the GPU library (0 NTSC, 1 PAL; informative). */
    xba_check(0, 0x12345678, 0x12345678);
    xba_check(1, in_data + in_bss, 0xC0FFEE01);
    uint32_t mode = GetVideoMode();
    xba_check(2, mode, mode);
    xba_finish();
}
