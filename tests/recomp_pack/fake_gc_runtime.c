/* A stand-in for the GameCube runtime in the fake shells (XBA-259): the real one (consoles/gc/runtime/gc_runtime.c)
 * plays the DOL with the core and the host layer (SDL), which this test does not link. It logs the line the test
 * checks and what it was handed, so the routing is what is tested here. */
#include "gc_runtime.h"

#include <stdio.h>

const char *gc_runtime_version(void)
{
    return "gc_runtime (test stand-in)";
}

int gc_runtime_start(const char *dol, const char *pack_lib, const char *title_key)
{
    printf("gc runtime ready (stand-in; title %s, pack %s, program %s)\n", title_key ? title_key : "?",
           pack_lib ? pack_lib : "none", dol ? dol : "?");
    fflush(stdout);
    return 0;
}

void gc_runtime_set_work_dir(const char *dir)
{
    printf("gc work dir %s\n", dir ? dir : "(none)");
    fflush(stdout);
}
