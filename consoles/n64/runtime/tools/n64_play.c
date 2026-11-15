/* n64_play: plays a Nintendo 64 ROM on a desktop (Linux, macOS) through the runtime the Android app uses
 * (n64_runtime_run: host_window, host_pad, host_audio, saves, pacing). XBA-174; a title runs here before it is
 * debugged on Android.
 *
 *   n64_play ROM [--frames N] [--save-dir DIR] [--scale aspect|integer|stretch] [--filter nearest|linear]
 *                [--pak controller|rumble|none] [--players 1..4] [--rdp-threads N] [--pal | --ntsc] [--tty]
 *                [--headless] [--unpaced] [--buttons MASK] [--screenshot OUT.ppm] [--capture N OUT.ppm] [--key KEY]
 *                [--pack LIB.so] [--missed FILE] [--frameskip auto|0..9]
 *
 * ROM               a .z64 / .v64 / .n64 file (any byte order); no BIOS needed (the PIF is HLE)
 * --frames N        stop after N frames (VI interrupts; default: until the window is closed)
 * --save-dir DIR    saves in DIR/n64-<KEY>/ (game.eep / game.sra / game.fla, pak1.mpk..; default
 *                   $RECOMP_SAVE_DIR, else $XDG_DATA_HOME/xrecomp/save, else ~/.local/share/xrecomp/save); ""
 *                   turns them off
 * --scale, --filter how the 4:3 picture fills the window (default aspect, linear; $RECOMP_N64_SCALE / _FILTER, else
 *                   $RECOMP_SCALE / $RECOMP_FILTER); --scale stretch fills the whole window
 * --pak             the accessory in the controllers (default controller pak; $RECOMP_N64_PAK)
 * --players N       host pad N -> controller N (default: every pad drives controller 1)
 * --rdp-threads N   RDP scanline-band threads (default $RECOMP_N64_RDP_THREADS, else $XBR_N64_RDP_THREADS, else 1)
 * --pal / --ntsc    force the TV type (default: the ROM's region)
 * --tty             print the game's IS-Viewer debug text ("n64: tty: ..." lines)
 * --frameskip M     auto (default, $RECOMP_N64_FRAMESKIP): skip the picture of frames the host is late for; 0 never;
 *                   N: show one frame in N + 1 (the frames still run whole, audio included)
 * --headless        no window (also with SDL_VIDEODRIVER=dummy or no DISPLAY): frames, pads, audio, saves only
 * --unpaced         no pacing: as fast as the host goes
 * --buttons MASK    N64 buttons held the whole run on controller 1 (N64_BTN_* bits, 1 = pressed)
 * --screenshot OUT  the last displayed picture at its own size as a PPM
 * --capture N OUT   the window's whole drawable at frame N, read back before the swap (the real present path)
 * --key KEY         the title key (default: read from the ROM as the app does, recomp_pack_rom_identify)
 * --pack LIB.so     a title pack library (python3 -m tools.prepare ROM: $XDG_DATA_HOME/xrecomp/packs/n64-<KEY>/,
 *                   or tools/consoles/n64_recomp_build.py): lifted code, interpreter elsewhere; with a pack.json
 *                   beside it, n64_pack_check first (n64_pack.h); a pack that is not this game's runs interpreted
 * --missed FILE     with --pack: at exit, add the code the interpreter had to run (address, hash, bytes) to FILE (the
 *                   discovery loop: the Prepare's <work>/discovered.txt, or n64_tools.recomp --discovered FILE)
 * Keyboard: arrows = stick, X = A, C = B, Z = Z, Enter = Start, Q / E = L / R, I / K / J / L = C buttons,
 * T / G / F / H = D-pad; any game controller (Xbox layout).
 * Save states (XBA-183): F1-F4 save slots 1-4, Shift+F1-F4 load them (<save dir>/n64-<KEY>/state<N>.st); Tab held
 * fast-forwards (RECOMP_FF_SPEED: 2..16 times, default 4, or "max"); on a pad, Back + right trigger held does the same,
 * Back + right / left shoulder quick saves / loads; RECOMP_STATE_RESUME=1 resumes from the automatic slot, saved when
 * the window closes.
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
    fprintf(stderr, "usage: n64_play ROM [--frames N] [--save-dir DIR] [--scale aspect|integer|stretch]\n"
                    "                [--filter nearest|linear] [--edges] [--pak controller|rumble|none] [--players 1..4]\n"
                    "                [--rdp-threads N] [--pal | --ntsc] [--tty] [--headless] [--unpaced]\n"
                    "                [--buttons MASK] [--screenshot OUT.ppm] [--capture N OUT.ppm] [--key KEY]\n"
                    "                [--pack LIB.so] [--missed FILE] [--frameskip auto|0..9]\n");
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
        } else if (!strcmp(a, "--scale") && more) {
            o.scale = host_window_scale_parse(argv[++i]);
            if (o.scale < 0)
                return usage();
        } else if (!strcmp(a, "--filter") && more) {
            o.filter = host_window_filter_parse(argv[++i]);
            if (o.filter < 0)
                return usage();
        }
        else if (!strcmp(a, "--edges"))
            o.crop_edges = 0; /* show the VI's guard band (RECOMP_N64_CROP_EDGES=0) */
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
        else if (!strcmp(a, "--frameskip") && more)
            o.frameskip = n64_runtime_frameskip_parse(argv[++i]);
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
        fprintf(stderr, "n64_play: SDL: %s\n", SDL_GetError());
        return 1;
    }
    int rc = n64_runtime_run(&o);
    SDL_Quit();
    return rc == 0 ? 0 : 1;
}
