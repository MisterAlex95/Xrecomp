/* ps1_runtime.c -- see ps1_runtime.h (XBA-146). */
#include "ps1_runtime.h"

#include <stdio.h>

const char *ps1_runtime_version(void)
{
    return "ps1_runtime " PS1_RUNTIME_VERSION;
}

int ps1_runtime_start(const char *rom, const char *pack_lib, const char *title_key)
{
    printf("ps1 runtime ready (%s, stub; title %s, pack %s, game %s)\n", ps1_runtime_version(),
           title_key ? title_key : "?", pack_lib ? pack_lib : "?", rom ? rom : "?");
    fflush(stdout);
    return 0;
}
