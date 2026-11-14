/* gba_runtime.c -- see gba_runtime.h (XBA-146). */
#include "gba_runtime.h"

#include <stdio.h>

const char *gba_runtime_version(void)
{
    return "gba_runtime " GBA_RUNTIME_VERSION;
}

int gba_runtime_start(const char *rom, const char *pack_lib, const char *title_key)
{
    printf("gba runtime ready (%s, stub; title %s, pack %s, game %s)\n", gba_runtime_version(),
           title_key ? title_key : "?", pack_lib ? pack_lib : "?", rom ? rom : "?");
    fflush(stdout);
    return 0;
}
