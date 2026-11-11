/*
 * Burnout 3: Takedown (Xbox, 2004, PAL Fr/De/It), title ID 4541005B -- what
 * is left in the title layer. XDK 5849, statically linked D3D8LTCG / DSOUND /
 * XMV / XPP: the XDK layers (title.json "layers") do the rest, with D3D's
 * addresses from title.json "params". Addresses, measurements and runs:
 * NOTES.md here.
 */
#include <stdlib.h>

void bo3_configure(void)
{
    /* The whole game is locked to the flip rate (title.json: the GL back
     * end's cap at 60, frame generation off). The software rasteriser has no
     * cap: the flip hold keeps it at 60. */
#ifndef __ANDROID__
    if (!getenv("RECOMP_GPU") && !getenv("RECOMP_FLIP_PACING")) {
#ifdef _WIN32
        _putenv_s("RECOMP_FLIP_PACING", "1");
#else
        setenv("RECOMP_FLIP_PACING", "1", 0);
#endif
    }
#endif
}

void bo3_start(const char *game_dir, const char *save_dir)
{
    (void)game_dir; (void)save_dir;
}
