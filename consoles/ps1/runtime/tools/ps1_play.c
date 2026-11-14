/* ps1_play: plays a PlayStation disc or EXE on a desktop (Linux, macOS) through the runtime the Android app uses
 * (ps1_runtime_run: host_window, host_pad, host_audio, memory cards, pacing). XBA-168; a title runs here before it is
 * debugged on Android.
 *
 *   ps1_play DISC|EXE [--exe X.exe] [--frames N] [--save-dir DIR] [--bios PATH] [--scale aspect|integer]
 *                     [--filter nearest|linear] [--pad digital|analog] [--players 1|2] [--headless] [--unpaced]
 *                     [--buttons MASK] [--screenshot OUT.ppm] [--capture N OUT.ppm] [--key KEY]
 *                     [--pack LIB.so] [--missed FILE]
 *
 * DISC              a .cue (with its tracks), .bin, .iso or .chd: the BIOS boots it (SYSTEM.CNF); EXE: a PS-X EXE,
 *                   fast-booted where the BIOS would enter its shell
 * --exe X.exe       with a DISC: fast-boot this EXE with the disc in the drive
 * --frames N        stop after N frames (default: until the window is closed)
 * --save-dir DIR    memory cards in DIR/ps1-<KEY>/card1.mcr, card2.mcr (default $XDG_DATA_HOME/xrecomp/save, else
 *                   ~/.local/share/xrecomp/save); "" turns them off
 * --bios PATH       the 512 KB BIOS (default $RECOMP_PS1_BIOS, else build/ps1-bios/openbios.bin from the working
 *                   directory or beside an ancestor of this program: tools/consoles/build_ps1_bios.sh)
 * --scale, --filter how the 4:3 picture fills the window (default aspect, linear; $RECOMP_PS1_SCALE / _FILTER)
 * --pad             the pad in port 1 (default digital; $RECOMP_PS1_PAD); Back + L3 + R3 switches while playing
 * --players 2       host pad 1 -> port 1, host pad 2 -> port 2 (default: every pad drives port 1)
 * --headless        no window (also with SDL_VIDEODRIVER=dummy or no DISPLAY): frames, pads, audio, cards only
 * --unpaced         no pacing: as fast as the host goes
 * --buttons MASK    PS1 buttons held the whole run on pad 1 (PS1_PAD_* bits, 1 = pressed)
 * --screenshot OUT  the last displayed picture at its own size as a PPM
 * --capture N OUT   the window's whole drawable at frame N, read back before the swap (the real present path)
 * --key KEY         the title key (default: read from the disc as the app does, recomp_pack_rom_identify; an EXE or
 *                   an unknown disc: ps1_runtime_fallback_key)
 * --pack LIB.so     a title pack library (python3 -m tools.prepare DISC: $XDG_DATA_HOME/xrecomp/packs/ps1-<KEY>/,
 *                   or tools/consoles/ps1_recomp_build.py): lifted code, interpreter elsewhere; with a pack.json
 *                   beside it, ps1_pack_check first (ps1_pack.h); a pack that is not this game's runs interpreted
 * --missed FILE     with --pack: at exit, add the code the interpreter had to run (address, hash, bytes) to FILE (the
 *                   discovery loop: the Prepare's <work>/discovered.txt, or ps1_tools.recomp --discovered FILE)
 * Keyboard: arrows, X = Cross, C = Circle, Z = Square, S = Triangle, Q / E = L1 / R1, 1 / 3 = L2 / R2,
 * Enter = Start, Backspace = Select; any game controller (Xbox layout).
 * Exit 0, 1 when the game cannot run, 2 on a usage error. */
#include "host_window.h"
#include "ps1_runtime.h"
#include "recomp_pack.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr, "usage: ps1_play DISC|EXE [--exe X.exe] [--frames N] [--save-dir DIR] [--bios PATH]\n"
                    "                [--scale aspect|integer] [--filter nearest|linear] [--pad digital|analog]\n"
                    "                [--players 1|2] [--headless] [--unpaced] [--buttons MASK]\n"
                    "                [--screenshot OUT.ppm] [--capture N OUT.ppm] [--key KEY]\n"
                    "                [--pack LIB.so] [--missed FILE]\n");
    return 2;
}

int main(int argc, char **argv)
{
    ps1_runtime_opts o;
    static char save_dir[1024], key[64];
    int save_set = 0;

    ps1_runtime_opts_init(&o);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--frames") && more)
            o.frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--exe") && more)
            o.exe = argv[++i];
        else if (!strcmp(a, "--save-dir") && more) {
            snprintf(save_dir, sizeof save_dir, "%s", argv[++i]);
            save_set = 1;
        } else if (!strcmp(a, "--bios") && more)
            o.bios = argv[++i];
        else if (!strcmp(a, "--scale") && more)
            o.scale = !strcmp(argv[++i], "integer") ? HOST_SCALE_INTEGER : HOST_SCALE_ASPECT;
        else if (!strcmp(a, "--filter") && more)
            o.filter = !strcmp(argv[++i], "nearest") ? HOST_FILTER_NEAREST : HOST_FILTER_LINEAR;
        else if (!strcmp(a, "--pad") && more)
            o.analog = !strcmp(argv[++i], "analog");
        else if (!strcmp(a, "--players") && more)
            o.players = atoi(argv[++i]) >= 2 ? 2 : 1;
        else if (!strcmp(a, "--headless"))
            o.headless = 1;
        else if (!strcmp(a, "--unpaced"))
            o.unpaced = 1;
        else if (!strcmp(a, "--buttons") && more)
            o.buttons = (uint16_t)strtoul(argv[++i], NULL, 0);
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
        else if (a[0] == '-' || o.game)
            return usage();
        else
            o.game = a;
    }
    if (!o.game)
        return usage();
    if (!o.title_key) {
        int console = 0;
        if (recomp_pack_rom_identify(o.game, &console, key, sizeof key) == 0 && console == RECOMP_CONSOLE_PS1)
            o.title_key = key;
    }
    if (!save_set) {
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        if (xdg && *xdg)
            snprintf(save_dir, sizeof save_dir, "%s/xrecomp/save", xdg);
        else if (home && *home)
            snprintf(save_dir, sizeof save_dir, "%s/.local/share/xrecomp/save", home);
    }
    o.save_dir = save_dir[0] ? save_dir : NULL;

    if (SDL_Init(SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "ps1_play: SDL: %s\n", SDL_GetError());
        return 1;
    }
    int rc = ps1_runtime_run(&o);
    SDL_Quit();
    return rc == 0 ? 0 : 1;
}
