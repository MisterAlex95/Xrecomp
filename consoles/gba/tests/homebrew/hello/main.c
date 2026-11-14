/* GBA hello (XBA-149): prints a fixed text with libtonc's tile text engine (mode 0, BG0) and
 * writes known values to the result block. Result protocol: README.md. Our own code. */
#include <tonc.h>

#include "../common/xba_result.h"

int main(void)
{
    xba_begin(XBA_ID_HELLO);

    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;
    tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));
    tte_write("#{P:8,8}XBA HELLO GBA");
    tte_write("#{P:8,24}homebrew corpus XBA-149");

    /* table[0] = known constant, table[1] = a value only right if .data was copied by crt0 and
     * .bss was cleared, table[2] = REG_DISPCNT read back. */
    static volatile u32 in_data = 0xC0FFEE01;
    static volatile u32 in_bss;
    xba_check(0, 0x12345678, 0x12345678);
    xba_check(1, in_data + in_bss, 0xC0FFEE01);
    xba_check(2, REG_DISPCNT, DCNT_MODE0 | DCNT_BG0);
    xba_finish();
}
