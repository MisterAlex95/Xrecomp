/* x360_xam2_test -- the xam2 HLE group called directly (XBA-296). */
#include "x360_hle_util.h"

int main(void)
{
    if (hle_util_start())
        return 1;
    return hle_util_end("x360_xam2");
}
