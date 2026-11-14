/* gba_play: plays a GBA ROM on a desktop (Linux, macOS) through the runtime the Android app uses (gba_runtime_run:
 * host_window, host_pad, host_audio, saves, pacing). XBA-157; a title runs here before it is debugged on Android.
 *
 *   gba_play ROM [--frames N] [--save-dir DIR] [--bios PATH] [--scale aspect|integer|stretch] [--filter nearest|linear]
 *                [--headless] [--unpaced] [--keys MASK] [--screenshot OUT.ppm] [--capture N OUT.ppm] [--key KEY]
 *                [--pack LIB.so] [--missed FILE]
 *
 * --frames N        stop after N frames (default: until the window is closed)
 * --save-dir DIR    saves in DIR/gba-<KEY>/game.sav (default $RECOMP_SAVE_DIR, else $XDG_DATA_HOME/xrecomp/save, else
 *                   ~/.local/share/xrecomp/save); "" turns saves off
 * --bios PATH       a 16 KB BIOS image (default $RECOMP_GBA_BIOS, else the built-in one)
 * --scale, --filter how the picture fills the window (default aspect, nearest; $RECOMP_GBA_SCALE / _FILTER, else
 *                   $RECOMP_SCALE / $RECOMP_FILTER); --scale stretch fills the whole window
 * --headless        no window (also with SDL_VIDEODRIVER=dummy or no DISPLAY): frames, pads, audio, saves only
 * --unpaced         no pacing: as fast as the host goes
 * --keys MASK       GBA keys held the whole run (KEYINPUT order, 1 = pressed)
 * --screenshot OUT  the last frame (240 x 160) as a PPM
 * --capture N OUT   the window's whole drawable at frame N, read back before the swap (the real present path)
 * --key KEY         the title key (default: computed from the ROM, as the app does)
 * --pack LIB.so     a title pack library (Prepare, or tools/consoles/gba_recomp_build.py): lifted code, interpreter
 *                   elsewhere; with a pack.json beside it, gba_pack_check first (gba_pack.h)
 * --missed FILE     with --pack: at exit, add the cartridge addresses the interpreter had to run to FILE (the
 *                   discovery loop: gba_tools.recomp --discovered FILE)
 * Keyboard: arrows, X = A, Z = B, A = L, S = R, Enter = Start, Backspace = Select; any game controller (Xbox layout).
 * Save states (XBA-182): F1-F4 save slots 1-4, Shift+F1-F4 load them (<save dir>/gba-<KEY>/state<N>.st); Tab held
 * fast-forwards (RECOMP_FF_SPEED: 2..16 times, default 4, or "max"); on a pad, Back + right trigger held does the same.
 * Exit 0, 1 when the ROM cannot run, 2 on a usage error. */
#include "gba_runtime.h"
#include "host_window.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr, "usage: gba_play ROM [--frames N] [--save-dir DIR] [--bios PATH] [--scale aspect|integer|stretch]\n"
                    "                [--filter nearest|linear] [--headless] [--unpaced] [--keys MASK]\n"
                    "                [--screenshot OUT.ppm] [--capture N OUT.ppm] [--key KEY] [--pack LIB.so]\n"
                    "                [--missed FILE]\n");
    return 2;
}

int main(int argc, char **argv)
{
    gba_runtime_opts o;
    static char save_dir[1024];
    int save_set = 0;

    gba_runtime_opts_init(&o);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--frames") && more)
            o.frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--save-dir") && more) {
            snprintf(save_dir, sizeof save_dir, "%s", argv[++i]);
            save_set = 1;
        } else if (!strcmp(a, "--bios") && more)
            o.bios = argv[++i];
        else if (!strcmp(a, "--scale") && more) {
            o.scale = host_window_scale_parse(argv[++i]);
            if (o.scale < 0)
                return usage();
        } else if (!strcmp(a, "--filter") && more) {
            o.filter = host_window_filter_parse(argv[++i]);
            if (o.filter < 0)
                return usage();
        } else if (!strcmp(a, "--headless"))
            o.headless = 1;
        else if (!strcmp(a, "--unpaced"))
            o.unpaced = 1;
        else if (!strcmp(a, "--keys") && more)
            o.keys = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--screenshot") && more)
            o.screenshot = argv[++i];
        else if (!strcmp(a, "--capture") && i + 2 < argc) {
            o.capture_frame = strtoull(argv[++i], NULL, 10);
            o.capture_path = argv[++i];
        } else if (!strcmp(a, "--key") && more)
            o.title_key = argv[++i];
        else if (!strcmp(a, "--pack") && more)
            o.pack_lib = argv[++i];
        else if (!strcmp(a, "--missed") && more)
            o.discovered_path = argv[++i];
        else if (a[0] == '-' || o.rom)
            return usage();
        else
            o.rom = a;
    }
    if (!o.rom)
        return usage();
    if (!save_set) {
        /* $RECOMP_SAVE_DIR (the macOS launcher's per-title folder, XBA-181), else the XDG data folder. */
        const char *env = getenv("RECOMP_SAVE_DIR"), *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        if (env && *env)
            snprintf(save_dir, sizeof save_dir, "%s", env);
        else if (xdg && *xdg)
            snprintf(save_dir, sizeof save_dir, "%s/xrecomp/save", xdg);
        else if (home && *home)
            snprintf(save_dir, sizeof save_dir, "%s/.local/share/xrecomp/save", home);
    }
    o.save_dir = save_dir[0] ? save_dir : NULL;

    if (SDL_Init(SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "gba_play: SDL: %s\n", SDL_GetError());
        return 1;
    }
    int rc = gba_runtime_run(&o);
    SDL_Quit();
    return rc == 0 ? 0 : 1;
}
