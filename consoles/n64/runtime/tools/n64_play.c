/* n64_play: plays a Nintendo 64 ROM on a desktop (Linux, macOS) through the runtime the Android app uses
 * (n64_runtime_run: host_window, host_pad, host_audio, saves, pacing). XBA-174; a title runs here before it is
 * debugged on Android.
 *
 *   n64_play ROM [--frames N] [--save-dir DIR] [--scale aspect|integer] [--filter nearest|linear]
 *                [--pak controller|rumble|none] [--players 1..4] [--rdp-threads N] [--pal | --ntsc] [--tty]
 *                [--headless] [--unpaced] [--buttons MASK] [--screenshot OUT.ppm] [--capture N OUT.ppm] [--key KEY]
 *
 * ROM               a .z64 / .v64 / .n64 file (any byte order); no BIOS needed (the PIF is HLE)
 * --frames N        stop after N frames (VI interrupts; default: until the window is closed)
 * --save-dir DIR    saves in DIR/n64-<KEY>/ (game.eep / game.sra / game.fla, pak1.mpk..; default
 *                   $XDG_DATA_HOME/xrecomp/save, else ~/.local/share/xrecomp/save); "" turns them off
 * --scale, --filter how the 4:3 picture fills the window (default aspect, linear; $RECOMP_N64_SCALE / _FILTER)
 * --pak             the accessory in the controllers (default controller pak; $RECOMP_N64_PAK)
 * --players N       host pad N -> controller N (default: every pad drives controller 1)
 * --rdp-threads N   RDP scanline-band threads (default $RECOMP_N64_RDP_THREADS, else $XBR_N64_RDP_THREADS, else 1)
 * --pal / --ntsc    force the TV type (default: the ROM's region)
 * --tty             print the game's IS-Viewer debug text ("n64: tty: ..." lines)
 * --headless        no window (also with SDL_VIDEODRIVER=dummy or no DISPLAY): frames, pads, audio, saves only
 * --unpaced         no pacing: as fast as the host goes
 * --buttons MASK    N64 buttons held the whole run on controller 1 (N64_BTN_* bits, 1 = pressed)
 * --screenshot OUT  the last displayed picture at its own size as a PPM
 * --capture N OUT   the window's whole drawable at frame N, read back before the swap (the real present path)
 * --key KEY         the title key (default: read from the ROM as the app does, recomp_pack_rom_identify)
 * Keyboard: arrows = stick, X = A, C = B, Z = Z, Enter = Start, Q / E = L / R, I / K / J / L = C buttons,
 * T / G / F / H = D-pad; any game controller (Xbox layout).
 * Exit 0, 1 when the game cannot run, 2 on a usage error. */
#include "host_window.h"
#include "n64_runtime.h"
#include "recomp_pack.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr, "usage: n64_play ROM [--frames N] [--save-dir DIR] [--scale aspect|integer]\n"
                    "                [--filter nearest|linear] [--pak controller|rumble|none] [--players 1..4]\n"
                    "                [--rdp-threads N] [--pal | --ntsc] [--tty] [--headless] [--unpaced]\n"
                    "                [--buttons MASK] [--screenshot OUT.ppm] [--capture N OUT.ppm] [--key KEY]\n");
    return 2;
}

int main(int argc, char **argv)
{
    n64_runtime_opts o;
    static char save_dir[1024], key[64];
    int save_set = 0;

    n64_runtime_opts_init(&o);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--frames") && more)
            o.frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--save-dir") && more) {
            snprintf(save_dir, sizeof save_dir, "%s", argv[++i]);
            save_set = 1;
        } else if (!strcmp(a, "--scale") && more)
            o.scale = !strcmp(argv[++i], "integer") ? HOST_SCALE_INTEGER : HOST_SCALE_ASPECT;
        else if (!strcmp(a, "--filter") && more)
            o.filter = !strcmp(argv[++i], "nearest") ? HOST_FILTER_NEAREST : HOST_FILTER_LINEAR;
        else if (!strcmp(a, "--pak") && more) {
            o.pak = n64_runtime_pak_parse(argv[++i]);
            if (o.pak == N64_RT_PAK_DEFAULT)
                return usage();
        } else if (!strcmp(a, "--players") && more)
            o.players = atoi(argv[++i]);
        else if (!strcmp(a, "--rdp-threads") && more)
            o.rdp_threads = atoi(argv[++i]);
        else if (!strcmp(a, "--pal"))
            o.force_pal = 1;
        else if (!strcmp(a, "--ntsc"))
            o.force_pal = 0;
        else if (!strcmp(a, "--tty"))
            o.tty = 1;
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
        else if (a[0] == '-' || o.rom)
            return usage();
        else
            o.rom = a;
    }
    if (!o.rom)
        return usage();
    if (!o.title_key) {
        int console = 0;
        if (recomp_pack_rom_identify(o.rom, &console, key, sizeof key) == 0 && console == RECOMP_CONSOLE_N64)
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
        fprintf(stderr, "n64_play: SDL: %s\n", SDL_GetError());
        return 1;
    }
    int rc = n64_runtime_run(&o);
    SDL_Quit();
    return rc == 0 ? 0 : 1;
}
