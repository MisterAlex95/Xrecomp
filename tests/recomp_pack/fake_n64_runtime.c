/* A stand-in for the N64 runtime in the fake shells (XBA-174): the real one (consoles/n64/runtime/n64_runtime.c)
 * plays the cartridge with the core and the host layer (SDL), which this test does not link. It logs the line the
 * test checks and what it was handed, so the routing is what is tested here. */
#include "n64_runtime.h"

#include <stdio.h>

const char *n64_runtime_version(void)
{
    return "n64_runtime (test stand-in)";
}

int n64_runtime_start(const char *rom, const char *pack_lib, const char *title_key)
{
    printf("n64 runtime ready (stand-in; title %s, pack %s, game %s)\n", title_key ? title_key : "?",
           pack_lib ? pack_lib : "none", rom ? rom : "?");
    fflush(stdout);
    return 0;
}

void n64_runtime_set_save_dir(const char *dir)
{
    printf("n64 save dir %s\n", dir ? dir : "(none)");
    fflush(stdout);
}
