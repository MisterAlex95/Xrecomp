/* gc_run -- runs a GameCube DOL headless on the spike's core (XBA-225) and dumps the XFB.
 *
 *   gc_run PROG.dol [--frames N] [--png OUT] [--pack LIB.so] [--missed FILE] [--stats] [--peek ADDR[:N]] [--pal]
 *          [--pad PORT:BUTTONS[:SX:SY:CX:CY:L:R]]... [-v]
 *          [--gx] [--gx-hw] [--gx-scale N] [--gx-shaders async|sync|uber|special] [--gx-list FILE] [--gx-copy-ram]
 *          [--gx-programs FILE]
 *   gc_run --gx-warmup LIST[,LIST...] CACHE [--gx-hw]
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
 * also encoded into RAM. --gx-programs: the per-title program binary cache (gx_progcache.h, XBA-264: loaded at
 * the start, rewritten at the end when it grew).
 *
 * --gx-warmup (XBA-264): no program runs; the TEV states of the shader lists (comma separated, missing ones skipped)
 * are compiled on a headless context and their program binaries written to CACHE, as the app's warm-up does after a
 * Prepare. Exit status: 0 done, 3 the driver has no program binaries, 77 no GLES 3 context, 1 CACHE not written.
 *
 * Sound (XBA-263): --wav OUT writes everything the machine played (48 kHz 16-bit stereo, from power-on) as a WAV;
 * the line "audio: ..." sums up the DSP's microcode and what was played.
 *
 * Prepare (XBA-264): --discovered FILE merges the missed addresses and the REL modules seen loaded into FILE (the
 * runtime's discovered.txt format, gc_recomp_write_discovered). `gc_run --extract OUT.gcm IMAGE` writes the sparse
 * GCM tools.prepare reads (header, apploader, FST, main DOL, REL modules; gc_extract.h) out of any image format and
 * prints "key <KEY> dol_sha256 <HEX> rels <N>".
 *
 * Save states (XBA-264): --state-check N:M runs N fields, saves a state (gc_state_save), runs M more fields recording
 * each field's XFB, loads the state and runs the M fields again: every XFB and the state after them must be the same
 * as the first time ("STATE OK ...", else "STATE FAIL ..." and exit 1). With --pack, lifted code runs on both sides
 * (every span is checked again after the load). --rewind C:R [--rewind-mb M]: the rewind round trip
 * (recomp_rewind_check, XBA-194 / XBA-264): --frames fields with pseudo-random pads on port 1, a capture every C
 * fields into a ring capped at M MB (default 64), R steps back, then the fields after it must be the first run's
 * (picture and sound). --cheats FILE: the cheat list (recomp_cheats.h: Action Replay / Gecko codes) applied after
 * every field. Both need src/platform (GC_RUN_PLATFORM; not in a copy of consoles/gc alone). */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_audio.h"
#include "gc_boot.h"
#include "gc_core.h"
#include "gc_extract.h"
#include "gx/gx.h"
#include "gc_png.h"
#include "gc_audio.h"
#ifdef GC_RUN_PLATFORM
#include "recomp_cheats.h"
#include "recomp_rewind.h"
#endif

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

/* FNV-1a 64 of the XFB as the VI scans it (YUV), 0 without one. */
static uint64_t xfb_hash(gc_machine *m)
{
    static uint8_t yuv[GC_XFB_MAX_W * GC_XFB_MAX_H * 2];
    int w, h;
    if (gc_xfb_yuv(m, yuv, &w, &h))
        return 0;
    uint64_t x = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < (size_t)w * h * 2; i++)
        x = (x ^ yuv[i]) * 0x100000001B3ull;
    return x ^ (uint64_t)w << 48 ^ (uint64_t)h << 32;
}

#ifdef GC_RUN_PLATFORM
/* --rewind (XBA-264): pads that change every few fields, the same for a field. */
static void rw_input(void *m, long f)
{
    uint32_t x = (uint32_t)(f / 7) * 2654435761u;
    x ^= x >> 15;
    gc_pad_state p = {true, (uint16_t)(x & 0x1F7F), (uint8_t)(64 + (x >> 8) % 128), (uint8_t)(64 + (x >> 16) % 128),
                      128, 128, 0, 0};
    gc_set_pad(m, 0, &p);
}
static void rw_run(void *m)
{
    gc_run_frame(m);
}
static uint64_t xfb_hash(gc_machine *m);
static uint64_t rw_picture(void *m)
{
    return xfb_hash(m);
}
static uint64_t rw_audio(void *m)
{
    int16_t buf[1024 * 2];
    size_t n;
    uint64_t x = 0xCBF29CE484222325ull;
    while ((n = gc_audio_pull(m, buf, 1024)) > 0)
        for (size_t i = 0; i < 2 * n; i++)
            x = (x ^ (uint16_t)buf[i]) * 0x100000001B3ull;
    return x;
}
static size_t rw_size(void *m)
{
    return gc_state_size_rewind(m);
}
static size_t rw_save(uint8_t *out, size_t cap, void *m)
{
    return gc_state_save_rewind(m, "rewind", out, cap);
}
static int rw_load(const uint8_t *in, size_t n, void *m)
{
    return gc_state_load(m, "rewind", in, n, NULL, 0) == GC_STATE_OK ? 0 : -1;
}
static uint32_t ch_read32(void *m, uint32_t a)
{
    return gc_cheat_read32(m, a);
}
static void ch_write32(void *m, uint32_t a, uint32_t v)
{
    gc_cheat_write32(m, a, v);
}
#endif

/* --state-check N:M (XBA-264). 0, 1 on a difference, 2 when a state cannot be taken. */
static int state_check(gc_machine *m, int n, int fields)
{
    for (int f = 0; f < n; f++)
        if (gc_run_frame(m)) {
            fprintf(stderr, "gc_run: stopped in field %d: %s\n", f, gc_error(m));
            return 1;
        }
    size_t cap = gc_state_size(m) + (1u << 20), len = 0, len2 = 0;
    uint8_t *st = malloc(cap), *st2 = malloc(cap), *st3 = malloc(cap);
    uint64_t *seen = calloc((size_t)fields, sizeof *seen);
    int rc = 2;
    char why[200] = "";
    if (!st || !st2 || !st3 || !seen || !(len = gc_state_save(m, "check", st, cap)))
        goto out;
    for (int f = 0; f < fields; f++) {
        if (gc_run_frame(m))
            goto out;
        seen[f] = xfb_hash(m);
    }
    len2 = gc_state_save(m, "check", st2, cap);
    int lr = gc_state_load(m, "check", st, len, why, sizeof why);
    if (lr != GC_STATE_OK) {
        printf("STATE FAIL load: %d %s\n", lr, why);
        rc = 1;
        goto out;
    }
    rc = 0;
    for (int f = 0; f < fields; f++) {
        if (gc_run_frame(m)) {
            printf("STATE FAIL field %d after the load: the machine stopped: %s\n", n + 1 + f, gc_error(m));
            rc = 1;
            goto out;
        }
        uint64_t hsh = xfb_hash(m);
        if (hsh != seen[f]) {
            printf("STATE FAIL field %d after the load: XFB %016llX, %016llX the first time\n", n + 1 + f,
                   (unsigned long long)hsh, (unsigned long long)seen[f]);
            rc = 1;
        }
    }
    size_t len3 = gc_state_save(m, "check", st3, cap);
    if (len3 != len2 || memcmp(st2, st3, len2) != 0) {
        size_t i = 0;
        while (i < len2 && i < len3 && st2[i] == st3[i])
            i++;
        printf("STATE FAIL the state after %d more fields differs (%zu / %zu bytes, first at %zu)\n", fields, len2,
               len3, i);
        rc = 1;
    }
    if (!rc)
        printf("STATE OK saved at field %d (%zu bytes), %d fields replayed identically (XFB %016llX)\n", n, len,
               fields, (unsigned long long)seen[fields - 1]);
out:
    if (rc == 2)
        printf("STATE FAIL no state taken\n");
    free(st);
    free(st2);
    free(st3);
    free(seen);
    return rc;
}

int main(int argc, char **argv)
{
    const char *dol_path = NULL, *png = NULL, *pack = NULL, *missed = NULL, *disc2 = NULL, *wav = NULL;
    const char *discovered = NULL, *extract = NULL;
    int frames = 60, stats = 0, verbose = 0, npeek = 0, force_disc = 0, swap_at = -1, use_gx = 0, gx_hw = 0;
    gx_gl_options gxo = {0, 1, 0, NULL, NULL};
    const char *warm_lists = NULL, *warm_cache = NULL;
    const char *peek[16];
    int pal = 0, state_n = -1, state_m = 0, rewind_every = 0, rewind_back = 0;
    double rewind_mb = 64;
    const char *cheats_path = NULL;
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
        else if (!strcmp(argv[i], "--discovered") && i + 1 < argc)
            discovered = argv[++i];
        else if (!strcmp(argv[i], "--extract") && i + 1 < argc)
            extract = argv[++i];
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
        else if (!strcmp(argv[i], "--gx-programs") && i + 1 < argc)
            gxo.program_cache = argv[++i];
        else if (!strcmp(argv[i], "--gx-warmup") && i + 2 < argc)
            warm_lists = argv[i + 1], warm_cache = argv[i + 2], i += 2;
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
        else if (!strcmp(argv[i], "--rewind") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d:%d", &rewind_every, &rewind_back) != 2 || rewind_every < 1 || rewind_back < 1) {
                fprintf(stderr, "gc_run: --rewind C:R (a capture every C fields, R steps back)\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--rewind-mb") && i + 1 < argc)
            rewind_mb = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--cheats") && i + 1 < argc)
            cheats_path = argv[++i];
        else if (!strcmp(argv[i], "--state-check") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d:%d", &state_n, &state_m) != 2 || state_n < 0 || state_m < 1) {
                fprintf(stderr, "gc_run: --state-check N:M (N >= 0 fields before the save, M >= 1 after)\n");
                return 2;
            }
        }
        else if (argv[i][0] != '-' && !dol_path)
            dol_path = argv[i];
        else {
            fprintf(stderr, "usage: gc_run PROG.dol|DISC [--disc IMAGE] [--disc2 IMAGE --swap-at FIELD] [--frames N]"
                            " [--png OUT] [--pack LIB] [--missed FILE] [--stats] [--peek ADDR[:N]] [--pal]"
                            " [--pad PORT:BUTTONS[:SX:SY:CX:CY:L:R]] [--wav OUT] [-v] [--gx] [--gx-hw] [--gx-scale N]"
                            " [--gx-shaders MODE] [--gx-list FILE] [--gx-copy-ram] [--gx-programs FILE]"
                            " [--discovered FILE] [--state-check N:M] [--rewind C:R [--rewind-mb M]] [--cheats FILE]\n"
                            "       gc_run --gx-warmup LIST[,LIST...] CACHE [--gx-hw]\n"
                            "       gc_run --extract OUT.gcm IMAGE\n");
            return 2;
        }
    }
    if (warm_lists) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s", warm_lists);
        const char *lists[16];
        size_t n = 0;
        for (char *t = strtok(buf, ","); t && n < 16; t = strtok(NULL, ","))
            lists[n++] = t;
        gx_warmup_result r;
        int wrc = gx_warmup_headless(lists, n, warm_cache, !gx_hw, &r);
        printf("gx warm-up: rc %d, %d states, %d cached, %d compiled, %d failed\n", wrc, r.states, r.cached,
               r.compiled, r.failed);
        return wrc == 0 ? 0 : wrc == 1 ? 3 : wrc == 2 ? 77 : 1;
    }
    if (!dol_path) {
        fprintf(stderr, "gc_run: no DOL\n");
        return 2;
    }
    if (extract) {
        char ewhy[300], key[48] = "?", hex[65];
        gc_extract_info info;
        gc_disc *d = gc_disc_open(dol_path, ewhy, sizeof ewhy);
        if (!d || gc_extract_sparse(d, extract, &info, ewhy, sizeof ewhy)) {
            fprintf(stderr, "gc_run: --extract %s: %s\n", dol_path, ewhy);
            if (d)
                gc_disc_close(d);
            return 2;
        }
        gc_disc_title_key(d, key, sizeof key);
        gc_extract_dol_sha256(d, hex);
        printf("key %s dol_sha256 %s rels %u (%llu bytes)\n", key, hex, info.rels,
               (unsigned long long)info.rel_bytes);
        gc_disc_close(d);
        return 0;
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
    if (state_n >= 0) {
        rc = state_check(m, state_n, state_m);
        frames = 0;
    }
#ifdef GC_RUN_PLATFORM
    if (rewind_every > 0) {
        recomp_rewind_machine mc = {m, rw_input, rw_run, rw_picture, rw_audio, rw_size, rw_save, rw_load};
        int r = recomp_rewind_check("gc", &mc, frames, rewind_every, rewind_back,
                                    (size_t)((rewind_mb > 0 ? rewind_mb : 1) * 1048576.0));
        rc = r ? 1 : 0;
        frames = 0;
    }
    recomp_cheats cheats;
    recomp_cheats_init(&cheats, RECOMP_CHEAT_GC);
    recomp_cheat_bus cheat_bus = {m, NULL, NULL, ch_read32, ch_write32, 1, NULL, 0, NULL};
    if (cheats_path && recomp_cheats_load(&cheats, cheats_path) < 0)
        rc = 2, frames = 0;
#else
    if (rewind_every > 0 || cheats_path) {
        fprintf(stderr, "gc_run: --rewind / --cheats need src/platform (built without GC_RUN_PLATFORM)\n");
        rc = 2;
        frames = 0;
    }
#endif
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
#ifdef GC_RUN_PLATFORM
        if (cheats.n)
            recomp_cheats_apply(&cheats, &cheat_bus);
#endif
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
    if (discovered) {
        int added = gc_recomp_write_discovered(m, discovered);
        if (added < 0)
            fprintf(stderr, "gc_run: cannot write %s\n", discovered);
        else
            printf("discovered +%d (%zu missed, %zu REL records)\n", added, gc_recomp_missed(m, NULL, 0),
                   gc_recomp_rel_records(m, NULL, 0));
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
