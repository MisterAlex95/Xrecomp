/* x360_vd.c -- the Vd start-up calls, the audio driver exports, the variable exports (XBA-296). */
#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

const x360_hle_row x360_tab_vd[] = {
    {0, 0, NULL}, /* placeholder: an empty initialiser is not C */
};
const unsigned x360_tab_vd_n = 0;
