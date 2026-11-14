/* A stand-in for the GBA runtime in the fake shells (XBA-157): the real one (consoles/gba/runtime/gba_runtime.c)
 * plays the game with the core and the host layer (SDL), which this test does not link. It logs the line the test
 * checks and what it was handed, so the routing is what is tested here. */
#include "gba_runtime.h"

#include <stdio.h>

const char *gba_runtime_version(void)
{
    return "gba_runtime (test stand-in)";
}

int gba_runtime_start(const char *rom, const char *pack_lib, const char *title_key)
{
    printf("gba runtime ready (stand-in; title %s, pack %s, game %s)\n", title_key ? title_key : "?",
           pack_lib ? pack_lib : "none", rom ? rom : "?");
    fflush(stdout);
    return 0;
}

void gba_runtime_set_work_dir(const char *dir)
{
    printf("gba work dir %s\n", dir ? dir : "(none)");
    fflush(stdout);
}
