/* A stand-in for the Xbox 360 runtime in the fake shells (XBA-280): the real one (consoles/x360/runtime/
 * x360_runtime.c) plays the title with x360_core and the host layer (SDL), which this test does not link. It logs
 * what it was handed (the title, the pack, the key, the disc and shader cache pack_loader.c set), so the routing is
 * what is tested here. */
#include "x360_runtime.h"

#include <stdio.h>
#include <stdlib.h>

const char *x360_runtime_version(void)
{
    return "x360_runtime (test stand-in)";
}

int x360_runtime_start(const char *xex, const char *pack_lib, const char *title_key)
{
    const char *disc = getenv("RECOMP_X360_DISC"), *shaders = getenv("RECOMP_X360_SHADERS");
    printf("x360 runtime ready (stand-in; title %s, pack %s, xex %s, disc %s, shaders %s)\n",
           title_key ? title_key : "?", pack_lib ? pack_lib : "none", xex ? xex : "?", disc ? disc : "none",
           shaders ? shaders : "none");
    fflush(stdout);
    return 0;
}

void x360_runtime_set_work_dir(const char *dir)
{
    printf("x360 work dir %s\n", dir ? dir : "(none)");
    fflush(stdout);
}

void x360_runtime_set_save_dir(const char *dir)
{
    (void)dir;
}
