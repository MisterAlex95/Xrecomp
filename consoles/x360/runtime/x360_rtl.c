/* x360_rtl.c -- the Rtl string family, the wide and counting printf family, image and stack helpers (XBA-296). */
#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

const x360_hle_row x360_tab_rtl[] = {
    {0, 0, NULL}, /* placeholder: an empty initialiser is not C */
};
const unsigned x360_tab_rtl_n = 0;
