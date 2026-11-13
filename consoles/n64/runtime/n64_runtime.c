/* n64_runtime.c -- see n64_runtime.h (XBA-146). */
#include "n64_runtime.h"

#include <stdio.h>

const char *n64_runtime_version(void)
{
    return "n64_runtime " N64_RUNTIME_VERSION;
}

int n64_runtime_start(const char *rom, const char *pack_lib, const char *title_key)
{
    printf("n64 runtime ready (%s, stub; title %s, pack %s, game %s)\n", n64_runtime_version(),
           title_key ? title_key : "?", pack_lib ? pack_lib : "?", rom ? rom : "?");
    fflush(stdout);
    return 0;
}
