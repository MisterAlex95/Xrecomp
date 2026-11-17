/* gc_run -- runs a GameCube DOL headless on the spike's core (XBA-225) and dumps the XFB.
 *
 *   gc_run PROG.dol [--frames N] [--png OUT] [--pack LIB.so] [--missed FILE] [--stats] [--peek ADDR[:N]] [--pal]
 *          [--pad PORT:BUTTONS[:SX:SY:CX:CY:L:R]]... [-v]
 *          [--gx] [--gx-hw] [--gx-scale N] [--gx-shaders async|sync|uber|special] [--gx-list FILE] [--gx-copy-ram]
 *
 * --frames counts video fields (60 per second, 50 in PAL). --pal: the IPL's PAL state (gc_set_tv_mode). --pad: a
 * standard controller on port PORT (0..3) for the whole run, BUTTONS in hex (GC_PAD_*), then sticks and triggers in
 * decimal (0..255, sticks at rest 128; XBA-259). --png: the framebuffer the video interface scans out after the
 * last field, YUV converted to RGB. --pack: a library built from gc_tools.recomp's C (gc_recomp_title_table); its
 * functions bind to this program's bus symbols. --missed: the text addresses the interpreter had to start at after a
 * jump, one hex per line, for gc_tools.recomp --discovered. Exit status: 0, 1 when the machine stopped (unsupported
 * instruction...), 2 on bad arguments or input.
 *
 * Discs (XBA-261): PROG may be a disc image instead (ISO / GCM, CISO, GCZ, RVZ, WIA: decided by content; --disc
 * forces it): it boots through its apploader with no IPL (gc_boot.h). --disc2 IMAGE --swap-at FIELD swaps discs at
 * that field (the cover opens, then closes on the second disc), for multi-disc sets.
 *
 * GX (XBA-262): --gx draws with the GLES renderer on a headless EGL context (Mesa's llvmpipe unless --gx-hw); exits
 * 77 when there is no GLES 3 context to be had. --gx-scale: internal resolution; --gx-shaders: async (ubershader
 * until the specialised shader is ready), sync, uber (ubershader only), special (specialised only, compiled on first
 * use); --gx-list: the per-title shader list (read, then written back); --gx-copy-ram: EFB copies to textures are
 * also encoded into RAM.
 *
 * Sound (XBA-263): --wav OUT writes everything the machine played (48 kHz 16-bit stereo, from power-on) as a WAV;
 * the line "audio: ..." sums up the DSP's microcode and what was played. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_audio.h"
#include "gc_boot.h"
#include "gc_core.h"
#include "gx/gx.h"
#include "gc_png.h"

/* A 48 kHz 16-bit stereo WAV, its sizes patched at the end. */
static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4;
    uint8_t h[44] = "RIFF____WAVEfmt \x10\0\0\0\x01\0\x02\0________\x04\0\x10\0data____";
    uint32_t v[4] = {36 + data, GC_AUDIO_RATE, GC_AUDIO_RATE * 4, data};
    int at[4] = {4, 24, 28, 40};
    for (int k = 0; k < 4; k++)
        for (int b = 0; b < 4; b++)
            h[at[k] + b] = (uint8_t)(v[k] >> (8 * b));
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

static void wav_drain(gc_machine *m, FILE *f, uint32_t *frames)
{
    static int16_t pcm[4096 * 2];
    size_t n;
    while ((n = gc_audio_pull(m, pcm, 4096)) > 0) {
        if (f) {
            uint8_t le[4096 * 4];
            for (size_t i = 0; i < 2 * n; i++) {
                le[2 * i] = (uint8_t)pcm[i];
                le[2 * i + 1] = (uint8_t)((uint16_t)pcm[i] >> 8);
            }
            fwrite(le, 4, n, f);
            *frames += (uint32_t)n;
        }
    }
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = n > 0 ? malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *size = p ? (size_t)n : 0;
    return p;
}

int main(int argc, char **argv)
{
    const char *dol_path = NULL, *png = NULL, *pack = NULL, *missed = NULL, *disc2 = NULL, *wav = NULL;
    int frames = 60, stats = 0, verbose = 0, npeek = 0, force_disc = 0, swap_at = -1, use_gx = 0, gx_hw = 0;
    gx_gl_options gxo = {0, 1, 0, NULL};
    const char *peek[16];
    int pal = 0;
    gc_pad_state pads[4];
    memset(pads, 0, sizeof pads);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc)
            png = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc)
            wav = argv[++i];
        else if (!strcmp(argv[i], "--pack") && i + 1 < argc)
            pack = argv[++i];
        else if (!strcmp(argv[i], "--missed") && i + 1 < argc)
            missed = argv[++i];
        else if (!strcmp(argv[i], "--peek") && i + 1 < argc && npeek < 16)
            peek[npeek++] = argv[++i];
        else if (!strcmp(argv[i], "--pal"))
            pal = 1;
        else if (!strcmp(argv[i], "--pad") && i + 1 < argc) {
            unsigned port = 4, b = 0, v[6] = {128, 128, 128, 128, 0, 0};
            int n = sscanf(argv[++i], "%u:%x:%u:%u:%u:%u:%u:%u", &port, &b, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
            if (n < 2 || port > 3) {
                fprintf(stderr, "gc_run: --pad PORT:BUTTONS[:SX:SY:CX:CY:L:R]\n");
                return 2;
            }
            pads[port] = (gc_pad_state){true, (uint16_t)b, (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3],
                                        (uint8_t)v[4], (uint8_t)v[5]};
        } else if (!strcmp(argv[i], "--stats"))
            stats = 1;
        else if (!strcmp(argv[i], "--gx"))
            use_gx = 1;
        else if (!strcmp(argv[i], "--gx-hw"))
            use_gx = 1, gx_hw = 1;
        else if (!strcmp(argv[i], "--gx-scale") && i + 1 < argc)
            gxo.scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gx-list") && i + 1 < argc)
            gxo.shader_list = argv[++i];
        else if (!strcmp(argv[i], "--gx-copy-ram"))
            gxo.efb_copy_ram = 1;
        else if (!strcmp(argv[i], "--gx-shaders") && i + 1 < argc) {
            const char *v = argv[++i];
            gxo.async_shaders = !strcmp(v, "sync") ? 0 : !strcmp(v, "uber") ? -1 : !strcmp(v, "special") ? 2 : 1;
        }
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc && !dol_path)
            force_disc = 1, dol_path = argv[++i];
        else if (!strcmp(argv[i], "--disc2") && i + 1 < argc)
            disc2 = argv[++i];
        else if (!strcmp(argv[i], "--swap-at") && i + 1 < argc)
            swap_at = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v"))
            verbose++;
        else if (argv[i][0] != '-' && !dol_path)
            dol_path = argv[i];
        else {
            fprintf(stderr, "usage: gc_run PROG.dol|DISC [--disc IMAGE] [--disc2 IMAGE --swap-at FIELD] [--frames N]"
                            " [--png OUT] [--pack LIB] [--missed FILE] [--stats] [--peek ADDR[:N]] [--pal]"
                            " [--pad PORT:BUTTONS[:SX:SY:CX:CY:L:R]] [--wav OUT] [-v] [--gx] [--gx-hw] [--gx-scale N]"
                            " [--gx-shaders MODE] [--gx-list FILE] [--gx-copy-ram]\n");
            return 2;
        }
    }
    if (!dol_path) {
        fprintf(stderr, "gc_run: no DOL\n");
        return 2;
    }
    char why[160];
    gc_machine *m = NULL;
    gc_disc *disc = gc_disc_open(dol_path, why, sizeof why);
    if (disc) {
        m = gc_create_disc(disc, why, sizeof why);
        if (!m)
            gc_disc_close(disc);
    } else if (force_disc) {
        fprintf(stderr, "gc_run: %s: %s\n", dol_path, why);
        return 2;
    } else {
        size_t size;
        uint8_t *dol = read_file(dol_path, &size);
        if (!dol) {
            fprintf(stderr, "gc_run: cannot read %s\n", dol_path);
            return 2;
        }
        m = gc_create(dol, size, why, sizeof why);
        free(dol);
    }
    if (!m) {
        fprintf(stderr, "gc_run: %s: %s\n", dol_path, why);
        return 2;
    }
    gc_set_verbose(m, verbose);
    if (use_gx) {
        char gwhy[160] = "";
        if (gx_egl_headless_create(!gx_hw, gwhy, sizeof gwhy) || gx_gl_attach(gc_gx(m), &gxo)) {
            fprintf(stderr, "gc_run: no GLES 3 renderer (%s): skipped\n", gwhy[0] ? gwhy : "see above");
            gc_destroy(m);
            return 77;
        }
    }
    if (pal)
        gc_set_tv_mode(m, GC_TV_PAL);
    for (int p = 0; p < 4; p++)
        gc_set_pad(m, p, &pads[p]);
    void *lib = NULL;
    if (pack) {
        lib = dlopen(pack, RTLD_NOW | RTLD_LOCAL);
        gc_recomp_title_table_fn get = lib ? (gc_recomp_title_table_fn)dlsym(lib, GC_RECOMP_ENTRY_SYMBOL) : NULL;
        if (!get || gc_attach_recomp(m, get())) {
            fprintf(stderr, "gc_run: pack %s: %s\n", pack, lib ? "no table or ABI mismatch" : dlerror());
            gc_destroy(m);
            return 2;
        }
    }
    int rc = 0;
    FILE *wf = NULL;
    uint32_t wav_frames = 0;
    if (wav) {
        wf = fopen(wav, "wb");
        if (!wf) {
            fprintf(stderr, "gc_run: cannot write %s\n", wav);
            gc_destroy(m);
            return 2;
        }
        wav_header(wf, 0);
    }
    for (int f = 0; f < frames; f++) {
        if (disc2 && f == swap_at) {
            gc_disc *d2 = gc_disc_open(disc2, why, sizeof why);
            if (!d2) {
                fprintf(stderr, "gc_run: %s: %s\n", disc2, why);
                rc = 2;
                break;
            }
            gc_disc_insert(m, d2);
        }
        if (gc_run_frame(m)) {
            fprintf(stderr, "gc_run: stopped in field %d: %s\n", f, gc_error(m));
            gc_dump_state(m, stderr);
            rc = 1;
            break;
        }
        wav_drain(m, wf, &wav_frames);
    }
    printf("frames %llu\n", (unsigned long long)gc_frames(m));
    {
        gc_audio_info ai;
        gc_audio_get_info(m, &ai);
        printf("audio: ucode %s (%08X), %u booted (%u unknown), %llu mails in / %llu out, %llu command lists, "
               "%llu DMA blocks, %llu stream frames, %llu frames out\n",
               ai.ucode, ai.ucode_crc, ai.ucodes_booted, ai.unknown_ucodes, (unsigned long long)ai.mails_to_dsp,
               (unsigned long long)ai.mails_from_dsp, (unsigned long long)ai.cmd_lists,
               (unsigned long long)ai.dma_blocks, (unsigned long long)ai.stream_frames,
               (unsigned long long)ai.frames_out);
    }
    if (wf) {
        wav_header(wf, wav_frames);
        fclose(wf);
        printf("wav %u frames\n", wav_frames);
    }
    if (verbose)
        gc_dump_state(m, stdout);
    if (stats) {
        gc_recomp_stats s;
        gc_recomp_get_stats(m, &s);
        printf("stats: native %llu interpreted %llu calls %llu span_checks %llu\n", (unsigned long long)s.native,
               (unsigned long long)s.interp, (unsigned long long)s.calls, (unsigned long long)s.span_checks);
        gx_stats g;
        gx_get_stats(gc_gx(m), &g);
        printf("gx: fifo %llu bytes, draws %llu, vertices %llu, batches %llu (uber %llu, specialised %llu), "
               "efb copies %llu, xfb copies %llu, textures decoded %llu (hits %llu), shaders ready %llu pending %llu, "
               "peeks %llu pokes %llu, unknown commands %llu\n",
               (unsigned long long)g.fifo_bytes, (unsigned long long)g.draws, (unsigned long long)g.vertices,
               (unsigned long long)g.batches, (unsigned long long)g.uber_draws, (unsigned long long)g.special_draws,
               (unsigned long long)g.efb_copies, (unsigned long long)g.xfb_copies, (unsigned long long)g.tex_decodes,
               (unsigned long long)g.tex_hits, (unsigned long long)g.shaders_ready,
               (unsigned long long)g.shaders_pending, (unsigned long long)g.peeks, (unsigned long long)g.pokes,
               (unsigned long long)g.unknown_cmds);
    }
    for (int i = 0; i < npeek; i++) { /* --peek ADDR[:WORDS]: memory words at the end (debugging) */
        unsigned long a = strtoul(peek[i], NULL, 16), n = 1;
        const char *c = strchr(peek[i], ':');
        if (c)
            n = strtoul(c + 1, NULL, 0);
        for (unsigned long k = 0; k < n; k++)
            printf("%s%08lX", k % 8 ? " " : k ? "\n" : "peek ", (unsigned long)gc_peek32(m, (uint32_t)(a + 4 * k)));
        printf("\n");
    }
    if (missed) {
        size_t n = gc_recomp_missed(m, NULL, 0);
        uint32_t *list = malloc((n ? n : 1) * sizeof *list);
        FILE *f = fopen(missed, "w");
        if (list && f) {
            gc_recomp_missed(m, list, n);
            fprintf(f, "# gc_run --missed: text addresses the interpreter started at after a jump\n");
            for (size_t i = 0; i < n; i++)
                fprintf(f, "%08X\n", list[i]);
            printf("missed %zu\n", n);
        }
        if (f)
            fclose(f);
        free(list);
    }
    if (png) {
        static uint8_t rgb[GC_XFB_MAX_W * GC_XFB_MAX_H * 3];
        int w, h;
        if (gc_xfb_rgb(m, rgb, &w, &h)) {
            fprintf(stderr, "gc_run: the video interface has no framebuffer\n");
            rc = rc ? rc : 1;
        } else if (write_png(png, rgb, w, h)) {
            fprintf(stderr, "gc_run: cannot write %s\n", png);
            rc = 2;
        } else {
            printf("xfb %dx%d\n", w, h);
        }
    }
    gc_destroy(m);
    if (use_gx)
        gx_egl_headless_destroy();
    if (lib)
        dlclose(lib);
    return rc;
}
