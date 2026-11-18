/* x360_rtl_test -- the rtl HLE group called directly (XBA-296). */
#include "x360_hle_util.h"

int main(void)
{
    if (hle_util_start())
        return 1;
    return hle_util_end("x360_rtl");
}
