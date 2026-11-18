/* x360_crypt2.c -- the hash, cipher and big-number exports beyond x360_crypt.c (XBA-296). */
#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

const x360_hle_row x360_tab_crypt2[] = {
    {0, 0, NULL}, /* placeholder: an empty initialiser is not C */
};
const unsigned x360_tab_crypt2_n = 0;
