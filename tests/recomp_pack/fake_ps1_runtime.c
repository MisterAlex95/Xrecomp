/* A stand-in for the PS1 runtime in the fake shells (XBA-168): the real one (consoles/ps1/runtime/ps1_runtime.c)
 * plays the disc with the core and the host layer (SDL), which this test does not link. It logs the line the test
 * checks and what it was handed, so the routing is what is tested here. */
#include "ps1_runtime.h"

#include <stdio.h>

const char *ps1_runtime_version(void)
{
    return "ps1_runtime (test stand-in)";
}

int ps1_runtime_start(const char *rom, const char *pack_lib, const char *title_key)
{
    printf("ps1 runtime ready (stand-in; title %s, pack %s, game %s)\n", title_key ? title_key : "?",
           pack_lib ? pack_lib : "none", rom ? rom : "?");
    fflush(stdout);
    return 0;
}

void ps1_runtime_set_work_dir(const char *dir)
{
    printf("ps1 work dir %s\n", dir ? dir : "(none)");
    fflush(stdout);
}
