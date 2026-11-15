/* ps1_play: plays a PlayStation disc or EXE on a desktop (Linux, macOS) through the runtime the Android app uses
 * (ps1_runtime_run: host_window, host_pad, host_audio, memory cards, pacing). XBA-168; a title runs here before it is
 * debugged on Android.
 *
 *   ps1_play DISC|EXE [--exe X.exe] [--frames N] [--save-dir DIR] [--bios PATH] [--scale aspect|integer|stretch]
 *                     [--filter nearest|linear] [--upscale 1|2|4] [--pad digital|analog] [--players 1|2] [--headless] [--unpaced]
 *                     [--buttons MASK] [--screenshot OUT.ppm] [--capture N OUT.ppm] [--key KEY]
 *                     [--pack LIB.so] [--missed FILE] [--no-fast-boot] [--swap FRAME:DISC]
 *
 * DISC              a .cue (with its tracks), .bin, .iso or .chd: the BIOS boots it (SYSTEM.CNF); EXE: a PS-X EXE,
 *                   fast-booted where the BIOS would enter its shell
 * --exe X.exe       with a DISC: fast-boot this EXE with the disc in the drive
 * --frames N        stop after N frames (default: until the window is closed)
 * --save-dir DIR    memory cards in DIR/ps1-<KEY>/card1.mcr, card2.mcr (default $RECOMP_SAVE_DIR, else
 *                   $XDG_DATA_HOME/xrecomp/save, else ~/.local/share/xrecomp/save); "" turns them off
 * --bios PATH       the 512 KB BIOS (default $RECOMP_PS1_BIOS, else build/ps1-bios/openbios.bin from the working
 *                   directory or beside an ancestor of this program: tools/consoles/build_ps1_bios.sh; none: the
 *                   HLE BIOS); "hle": the HLE BIOS
 * --scale, --filter how the 4:3 picture fills the window (default aspect, linear; $RECOMP_PS1_SCALE / _FILTER, else
 *                   $RECOMP_SCALE / $RECOMP_FILTER); --scale stretch fills the whole window
 * --upscale N       internal resolution: the GPU draws N times the PS1's (default $RECOMP_PS1_UPSCALE, else
 *                   $RECOMP_UPSCALE, else 1); the picture (and --screenshot) is N times wider and higher
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
 * --no-fast-boot    a disc boots through the BIOS's shell (OpenBIOS's logo, about five seconds; default: skipped;
 *                   $RECOMP_PS1_FASTBOOT=0 the same)
 * --swap FRAME:DISC at frame FRAME, change the disc to DISC (the lid opens, a second later it closes on DISC)
 * DISC may be a .m3u playlist (multi-disc games): its first disc plays, F6 changes to the next one.
 * Keyboard: arrows, X = Cross, C = Circle, Z = Square, S = Triangle, Q / E = L1 / R1, 1 / 3 = L2 / R2,
 * Enter = Start, Backspace = Select; any game controller (Xbox layout).
 * Save states (XBA-182): F1-F4 save slots 1-4, Shift+F1-F4 load them (<save dir>/ps1-<KEY>/state<N>.st); Tab held
 * fast-forwards (RECOMP_FF_SPEED: 2..16 times, default 4, or "max"); on a pad, Back + right trigger held does the same.
 * Rewind (XBA-194): R held goes back in time (Back + left trigger on a pad); RECOMP_REWIND=0 turns it off.
 * Exit 0, 1 when the game cannot run, 2 on a usage error. */
#include "host_window.h"
#include "ps1_runtime.h"
#include "ps1_identify.h"
#include "recomp_pack.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int usage(void)
{
    fprintf(stderr, "usage: ps1_play DISC|EXE [--exe X.exe] [--frames N] [--save-dir DIR] [--bios PATH]\n"
                    "                [--scale aspect|integer|stretch] [--filter nearest|linear] [--upscale 1|2|4]\n"
                    "                [--pad digital|analog] [--players 1|2] [--headless] [--unpaced] [--buttons MASK]\n"
                    "                [--screenshot OUT.ppm] [--capture N OUT.ppm] [--key KEY]\n"
                    "                [--pack LIB.so] [--missed FILE] [--no-fast-boot] [--swap FRAME:DISC]\n");
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
        else if (!strcmp(a, "--scale") && more) {
            o.scale = host_window_scale_parse(argv[++i]);
            if (o.scale < 0)
                return usage();
        } else if (!strcmp(a, "--filter") && more) {
            o.filter = host_window_filter_parse(argv[++i]);
            if (o.filter < 0)
                return usage();
        }
        else if (!strcmp(a, "--upscale") && more) {
            o.upscale = atoi(argv[++i]);
            if (o.upscale < 1 || o.upscale > 8)
                return usage();
        } else if (!strcmp(a, "--pad") && more)
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
        else if (!strcmp(a, "--no-fast-boot"))
            o.fast_boot = 0;
        else if (!strcmp(a, "--swap") && more) {
            char *colon = NULL;
            o.swap_frame = strtoull(argv[++i], &colon, 10);
            if (!colon || *colon != ':' || !colon[1] || !o.swap_frame)
                return usage();
            o.swap_disc = colon + 1;
        } else if (a[0] == '-' || o.game)
            return usage();
        else
            o.game = a;
    }
    if (!o.game)
        return usage();
    if (!o.title_key) {
        int console = 0;
        ps1_identify_install(); /* a .chd too */
        static char first[1][PS1_RUNTIME_PATH_MAX];
        const char *id = o.game;
        size_t gl = strlen(o.game);
        if (gl > 4 && !strcasecmp(o.game + gl - 4, ".m3u") && ps1_runtime_m3u(o.game, first, 1) == 1)
            id = first[0]; /* a playlist: its first disc's key */
        if (recomp_pack_rom_identify(id, &console, key, sizeof key) == 0 && console == RECOMP_CONSOLE_PS1)
            o.title_key = key;
    }
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
        fprintf(stderr, "ps1_play: SDL: %s\n", SDL_GetError());
        return 1;
    }
    int rc = ps1_runtime_run(&o);
    SDL_Quit();
    return rc == 0 ? 0 : 1;
}
