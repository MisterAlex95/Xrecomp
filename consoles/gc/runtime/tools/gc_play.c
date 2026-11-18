/* gc_play -- plays a GameCube DOL on a desktop through gc_runtime (window, pads; XBA-259).
 *
 *   gc_play PROG.dol [--frames N] [--headless] [--unpaced] [--screenshot OUT.png] [--capture FIELD OUT.ppm]
 *           [--pal] [--cpu-yuv] [--pack LIB.so] [--pad BUTTONS[:SX:SY:CX:CY:L:R]] [--wav OUT.wav]
 *           [--save-dir DIR] [--card-size 59|251|1019] [--card-b] [--no-card] [--missed FILE]
 *
 * --pad holds port 1's buttons (hex, GC_PAD_*) and sets its sticks and triggers (0..255, sticks at rest 128) for the
 * whole run, on top of the real pads (tests). --wav: everything played, also as a 48 kHz WAV (XBA-263). --save-dir:
 * the memory cards' folder, DIR/gc-<KEY>/cardA.raw (XBA-264); --card-size for a new card; --card-b a card in slot B
 * too; --no-card slot A empty. Without --save-dir: $RECOMP_SAVE_DIR (xrecomp's, the macOS launcher's), else no
 * memory card and no save state; --save-dir "" turns them off. --missed FILE: at exit, the addresses the interpreter
 * had to start at (gc_tools.recomp --discovered, `xrecomp play` keeps them; XBA-274), with gx_shaders.txt and
 * gx_programs.bin next to it. Exit 0, 1 when the program cannot start or the machine stopped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_runtime.h"

int main(int argc, char **argv)
{
    gc_runtime_opts o;
    gc_runtime_opts_init(&o);
    int save_set = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            o.frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--headless"))
            o.headless = 1;
        else if (!strcmp(argv[i], "--unpaced"))
            o.unpaced = 1;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
            o.screenshot = argv[++i];
        else if (!strcmp(argv[i], "--save-dir") && i + 1 < argc) {
            o.save_dir = argv[++i];
            save_set = 1;
        } else if (!strcmp(argv[i], "--missed") && i + 1 < argc)
            o.discovered_path = argv[++i];
        else if (!strcmp(argv[i], "--card-size") && i + 1 < argc)
            o.card_blocks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--card-b"))
            o.card_b = 1;
        else if (!strcmp(argv[i], "--no-card"))
            o.card_a = 0;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc)
            o.wav_path = argv[++i];
        else if (!strcmp(argv[i], "--capture") && i + 2 < argc) {
            o.capture_frame = strtoull(argv[++i], NULL, 10);
            o.capture_path = argv[++i];
        } else if (!strcmp(argv[i], "--pal"))
            o.tv_mode = GC_TV_PAL;
        else if (!strcmp(argv[i], "--cpu-yuv"))
            o.yuv_cpu = 1;
        else if (!strcmp(argv[i], "--pack") && i + 1 < argc)
            o.pack_lib = argv[++i];
        else if (!strcmp(argv[i], "--pad") && i + 1 < argc) {
            unsigned b = 0, v[6] = {128, 128, 128, 128, 0, 0};
            if (sscanf(argv[++i], "%x:%u:%u:%u:%u:%u:%u", &b, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) < 1) {
                fprintf(stderr, "gc_play: --pad BUTTONS[:SX:SY:CX:CY:L:R]\n");
                return 2;
            }
            o.pad_force_on = 1;
            o.pad_force = (gc_pad_state){true, (uint16_t)b, (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2],
                                         (uint8_t)v[3], (uint8_t)v[4], (uint8_t)v[5]};
        } else if (argv[i][0] != '-' && !o.dol)
            o.dol = argv[i];
        else {
            fprintf(stderr, "usage: gc_play PROG.dol [--frames N] [--headless] [--unpaced] [--screenshot OUT.png]"
                            " [--capture FIELD OUT.ppm] [--pal] [--cpu-yuv] [--pack LIB.so]"
                            " [--pad BUTTONS[:SX:SY:CX:CY:L:R]] [--wav OUT.wav] [--save-dir DIR]"
                            " [--card-size 59|251|1019] [--card-b] [--no-card] [--missed FILE]\n");
            return 2;
        }
    }
    if (!save_set) {
        const char *env = getenv("RECOMP_SAVE_DIR");
        o.save_dir = env && *env ? env : NULL;
    } else if (o.save_dir && !*o.save_dir) {
        o.save_dir = NULL;
    }
    if (!o.dol) {
        fprintf(stderr, "gc_play: no DOL\n");
        return 2;
    }
    return gc_runtime_run(&o) == 0 ? 0 : 1;
}
