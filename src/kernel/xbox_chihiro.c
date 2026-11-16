/* Sega Chihiro mode: the setting (xbox_chihiro.h, docs/technical/chihiro.md). */
#include "xbox_chihiro.h"

#include <stdlib.h>

int xbox_ChihiroMode(void)
{
    const char *e = getenv("RECOMP_CHIHIRO");
    return e && *e && !(e[0] == '0' && e[1] == '\0');
}
