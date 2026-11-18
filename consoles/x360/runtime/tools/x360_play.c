/* x360_play -- plays an Xbox 360 title on a desktop through x360_runtime (window, pads, audio; XBA-280).
 *
 *   x360_play TITLE.xex --pack LIB.so [--disc IMAGE|DIR] [--data DIR] [--shaders DIR] [--work DIR]
 *             [--save-dir DIR] [--frames N] [--headless] [--unpaced] [--screenshot OUT.ppm] [--upscale N]
 *             [--wav OUT.wav] [--pad BUTTONS[:LT:RT]]
 *
 * TITLE.xex: plain (python3 -m tools.xex DEFAULT.XEX --key-file KEY --plain OUT.xex, or what x360_tools.bringup left
 * in its work directory, with the pack and the shader cache). --shaders: the GLSL ES cache (default: shaders/ next to
 * the pack). --work: the shaders met (DIR/shaders, for bringup / Prepare to translate) and the addresses the pack
 * missed (DIR/discovered.txt). --save-dir: states in DIR/x360-<KEY>/ (default $RECOMP_SAVE_DIR, "" off). --upscale:
 * the internal resolution (1-8; $RECOMP_X360_UPSCALE / $RECOMP_UPSCALE). --pad holds user 0's buttons (hex,
 * XINPUT_GAMEPAD_*) and triggers for the whole run (tests). Keys: arrows, X / Z / C / V = A / B / X / Y, A / S =
 * LB / RB, Q / W = LT / RT, Enter = Start, Backspace = Back; Tab fast-forward, F1-F4 the state hotkeys. Exit 0 when
 * the run ended as asked (or the title exited 0), 1 otherwise. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x360_runtime.h"

int main(int argc, char **argv)
{
    x360_runtime_opts o;
    x360_runtime_opts_init(&o);
    int save_set = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--pack") && more)
            o.pack_lib = argv[++i];
        else if (!strcmp(a, "--disc") && more)
            o.disc = argv[++i];
        else if (!strcmp(a, "--data") && more)
            o.data_dir = argv[++i];
        else if (!strcmp(a, "--shaders") && more)
            o.shader_cache = argv[++i];
        else if (!strcmp(a, "--work") && more)
            o.work_dir = argv[++i];
        else if (!strcmp(a, "--save-dir") && more) {
            o.save_dir = argv[++i];
            save_set = 1;
        } else if (!strcmp(a, "--frames") && more)
            o.frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--headless"))
            o.headless = 1;
        else if (!strcmp(a, "--unpaced"))
            o.unpaced = 1;
        else if (!strcmp(a, "--screenshot") && more)
            o.screenshot = argv[++i];
        else if (!strcmp(a, "--upscale") && more)
            o.upscale = atoi(argv[++i]);
        else if (!strcmp(a, "--wav") && more)
            o.wav_path = argv[++i];
        else if (!strcmp(a, "--pad") && more) {
            unsigned b = 0, lt = 0, rt = 0;
            if (sscanf(argv[++i], "%x:%u:%u", &b, &lt, &rt) < 1) {
                fprintf(stderr, "x360_play: --pad BUTTONS[:LT:RT]\n");
                return 2;
            }
            o.pad_force_on = 1;
            o.pad_force.buttons = (uint16_t)b;
            o.pad_force.left_trigger = (uint8_t)lt, o.pad_force.right_trigger = (uint8_t)rt;
        } else if (a[0] != '-' && !o.xex)
            o.xex = a;
        else {
            fprintf(stderr, "usage: x360_play TITLE.xex --pack LIB.so [--disc IMAGE|DIR] [--data DIR] [--shaders DIR] "
                            "[--work DIR] [--save-dir DIR] [--frames N] [--headless] [--unpaced] [--screenshot OUT.ppm] "
                            "[--upscale N] [--wav OUT.wav] [--pad BUTTONS[:LT:RT]]\n");
            return 2;
        }
    }
    if (!save_set)
        o.save_dir = getenv("RECOMP_SAVE_DIR");
    if (!o.xex || !o.pack_lib) {
        fprintf(stderr, "x360_play: a title and its pack (--pack) are needed\n");
        return 2;
    }
    return x360_runtime_run(&o) == 0 ? 0 : 1;
}
