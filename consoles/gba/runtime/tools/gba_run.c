/* gba_run: runs a GBA ROM headless on the core (XBA-152 / XBA-153).
 *
 *   gba_run ROM [--frames N] [--bios PATH] [--bios-boot] [--result] [--ppm OUT.ppm] [--wav OUT.wav] [--keys MASK]
 *           [--save PATH] [--quiet]
 *
 * --frames N    frames to run (default 600); with --result, the most frames to wait for a verdict
 * --bios PATH   a 16 KB BIOS image (e.g. Cult-of-GBA, tools/consoles/build_gba_bios.sh); default: built-in
 * --bios-boot   boot from address 0 through the BIOS instead of the direct boot
 * --result      read the 512-byte xba_result block of our homebrew (consoles/gba/tests/homebrew/common/
 *               xba_result.h) at each VBlank, stop at the verdict; exit 0 on PASS, 1 on FAIL, 2 on timeout
 * --ppm OUT     write the last frame as a binary PPM
 * --wav OUT     write the audio of the run (gba_audio_pull after each frame) as a 48 kHz stereo 16-bit WAV
 * --save PATH   load the save image from PATH when it exists, write it back at the end if the game wrote it
 * --keys MASK   keys held during the whole run (KEYINPUT bit order, 1 = pressed)
 * --reg-result R  after the frames, exit 0 if register rR is 0, 1 if not, 2 if the CPU is not in an idle
 *               loop (jsmolka/gba-tests leave the number of the first failed test in r12 and spin: --reg-result 12)
 * --pack LIB    run with a recompiled title pack (gba_tools.recomp + cc -shared; gba_recomp.h): lifted code where
 *               the table covers the PC, the interpreter elsewhere
 * --missed FILE with --pack: write the cartridge addresses the interpreter had to run (merged with FILE's; the
 *               discovery loop feeds them back to gba_tools.recomp --discovered)
 * --stats       with --pack: print how many cartridge instructions ran natively vs interpreted
 * --lockstep    with --pack: run a second, interpreter-only machine side by side and compare the whole state
 *               (CPU, cycles, memories, frame) after every frame; exit 4 at the first difference
 * --hash        print a hash of the last frame and of the RAM (FNV-1a 64) and the cycle count
 * Prints the frame rate reached on stderr. Exit 3 on a usage or file error. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime */
#include "gba_core.h"
#include "gba_pack.h"
#include "gba_recomp.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RESULT_ADDR 0x0203FE00u
#define RESULT_MAGIC 0x58424152u
#define RESULT_PASS 0x50415353u
#define RESULT_FAIL 0x4641494Cu

static uint8_t *load_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static int write_ppm(const char *path, const uint16_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", GBA_SCREEN_W, GBA_SCREEN_H);
    for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; i++) {
        uint16_t c = fb[i];
        uint8_t rgb[3] = {(uint8_t)((c & 31) << 3 | (c & 31) >> 2), (uint8_t)(((c >> 5) & 31) << 3 | ((c >> 5) & 31) >> 2),
                          (uint8_t)(((c >> 10) & 31) << 3 | ((c >> 10) & 31) >> 2)};
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f);
}

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* WAV header for `frames` 16-bit stereo frames at GBA_AUDIO_RATE. */
static void wav_header(uint8_t h[44], uint32_t frames)
{
    uint32_t bytes = frames * 4;
    memcpy(h, "RIFF", 4);
    put_le(h + 4, 36 + bytes, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 1, 2); /* PCM */
    put_le(h + 22, 2, 2);
    put_le(h + 24, GBA_AUDIO_RATE, 4);
    put_le(h + 28, GBA_AUDIO_RATE * 4, 4);
    put_le(h + 32, 4, 2);
    put_le(h + 34, 16, 2);
    memcpy(h + 36, "data", 4);
    put_le(h + 40, bytes, 4);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t fnv1a(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

static void fourcc(uint32_t v, char out[5])
{
    for (int i = 0; i < 4; i++) {
        char c = (char)(v >> (24 - 8 * i));
        out[i] = (c >= 32 && c < 127) ? c : '?';
    }
    out[4] = 0;
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *bios_path = NULL, *ppm = NULL, *wav = NULL, *save = NULL;
    const char *pack = NULL, *missed = NULL;
    int stats = 0, lockstep = 0, hash = 0;
    long frames = 600;
    int result = 0, bios_boot = 0, quiet = 0, reg_result = -1;
    unsigned keys = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--bios") && i + 1 < argc)
            bios_path = argv[++i];
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc)
            ppm = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc)
            wav = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc)
            save = argv[++i];
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc)
            keys = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--reg-result") && i + 1 < argc)
            reg_result = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--pack") && i + 1 < argc)
            pack = argv[++i];
        else if (!strcmp(argv[i], "--missed") && i + 1 < argc)
            missed = argv[++i];
        else if (!strcmp(argv[i], "--stats"))
            stats = 1;
        else if (!strcmp(argv[i], "--lockstep"))
            lockstep = 1;
        else if (!strcmp(argv[i], "--hash"))
            hash = 1;
        else if (!strcmp(argv[i], "--result"))
            result = 1;
        else if (!strcmp(argv[i], "--bios-boot"))
            bios_boot = 1;
        else if (!strcmp(argv[i], "--quiet"))
            quiet = 1;
        else if (argv[i][0] != '-' && !rom_path)
            rom_path = argv[i];
        else {
            fprintf(stderr, "usage: gba_run ROM [--frames N] [--bios PATH] [--bios-boot] [--result] [--ppm OUT] "
                            "[--wav OUT] [--save PATH] [--keys MASK] [--reg-result R] [--pack LIB] [--missed FILE] [--stats] "
                            "[--lockstep] [--hash] [--quiet]\n");
            return 3;
        }
    }
    if (!rom_path || frames <= 0) {
        fprintf(stderr, "gba_run: no ROM given\n");
        return 3;
    }

    size_t rom_size = 0, bios_size = 0;
    uint8_t *rom = load_file(rom_path, &rom_size), *bios = NULL;
    if (!rom) {
        fprintf(stderr, "gba_run: cannot read %s\n", rom_path);
        return 3;
    }
    if (bios_path && !(bios = load_file(bios_path, &bios_size))) {
        fprintf(stderr, "gba_run: cannot read %s\n", bios_path);
        return 3;
    }
    gba_machine *m = gba_create(rom, rom_size, bios, bios_size);
    gba_machine *ref = lockstep && pack ? gba_create(rom, rom_size, bios, bios_size) : NULL;
    char rom_sha[65];
    uint8_t sum[32];
    gba_pack_sha256(rom, rom_size, sum);
    gba_pack_hex(sum, rom_sha);
    free(rom);
    free(bios);
    if (!m || (lockstep && pack && !ref)) {
        fprintf(stderr, "gba_run: bad ROM or BIOS size\n");
        return 3;
    }
    gba_pack pk = {0};
    if (pack) {
        char why[256];
        if (gba_pack_open(pack, rom_sha, &pk, why, sizeof why) != 0 || gba_attach_recomp(m, pk.table) != 0) {
            fprintf(stderr, "gba_run: pack %s: %s\n", pack, why);
            return 3;
        }
    }
    if (bios_boot)
        gba_boot_bios(m);
    gba_set_keys(m, (uint16_t)keys);
    if (ref) {
        if (bios_boot)
            gba_boot_bios(ref);
        gba_set_keys(ref, (uint16_t)keys);
    }
    if (save) {
        size_t n = 0;
        uint8_t *img = load_file(save, &n);
        if (img && gba_save_load(m, img, n) != 0)
            fprintf(stderr, "gba_run: %s does not fit the save kind %d, ignored\n", save, (int)gba_save_type(m));
        free(img);
    }

    FILE *wf = NULL;
    uint32_t wav_frames = 0;
    if (wav) {
        uint8_t h[44] = {0};
        if (!(wf = fopen(wav, "wb")) || fwrite(h, 1, sizeof h, wf) != sizeof h) {
            fprintf(stderr, "gba_run: cannot write %s\n", wav);
            return 3;
        }
    }

    static uint16_t fb[GBA_SCREEN_W * GBA_SCREEN_H];
    static int16_t pcm[2 * 4096];
    uint32_t status = 0;
    long f;
    double t0 = now_s();
    static uint16_t ref_fb[GBA_SCREEN_W * GBA_SCREEN_H];
    int diverged = 0;
    for (f = 0; f < frames; f++) {
        gba_run_frame(m, fb);
        if (ref) {
            char why[160] = "";
            static int16_t sink[2 * 4096];
            gba_run_frame(ref, ref_fb);
            while (gba_audio_pull(ref, sink, 4096) > 0) /* keeps both audio rings alike */
                ;
            int d = gba_recomp_compare(ref, m, why, sizeof why);
            if (!d && memcmp(fb, ref_fb, sizeof fb) != 0) {
                snprintf(why, sizeof why, "framebuffer");
                d = 1;
            }
            if (d) {
                printf("LOCKSTEP DIVERGED at frame %ld: %s (interpreter vs recompiled)\n", f + 1, why);
                diverged = 1;
                f++;
                break;
            }
        }
        if (wf) {
            size_t n;
            while ((n = gba_audio_pull(m, pcm, 4096)) > 0) {
                uint8_t le[4 * 4096];
                for (size_t i = 0; i < 2 * n; i++)
                    put_le(le + 2 * i, (uint16_t)pcm[i], 2);
                fwrite(le, 4, n, wf);
                wav_frames += (uint32_t)n;
            }
        }
        if (result && gba_peek32(m, RESULT_ADDR) == RESULT_MAGIC) {
            status = gba_peek32(m, RESULT_ADDR + 4);
            if (status) {
                f++;
                break;
            }
        }
    }
    double dt = now_s() - t0;
    if (ref && !diverged)
        printf("LOCKSTEP OK: %ld frames identical (interpreter vs recompiled)\n", f);
    if (!quiet)
        fprintf(stderr, "gba_run: %ld frames in %.3f s = %.1f frames/s (%.1fx real time)\n", f, dt,
                dt > 0 ? f / dt : 0.0, dt > 0 ? f / dt / 59.7275 : 0.0);
    if (ppm && write_ppm(ppm, fb) != 0)
        fprintf(stderr, "gba_run: cannot write %s\n", ppm);
    if (wf) {
        uint8_t h[44];
        wav_header(h, wav_frames);
        if (fseek(wf, 0, SEEK_SET) != 0 || fwrite(h, 1, sizeof h, wf) != sizeof h || fclose(wf) != 0)
            fprintf(stderr, "gba_run: cannot write %s\n", wav);
        else if (!quiet)
            fprintf(stderr, "gba_run: %s: %u frames of audio (%.2f s)\n", wav, wav_frames,
                    (double)wav_frames / GBA_AUDIO_RATE);
    }
    if (save && gba_save_take_dirty(m)) {
        size_t n = 0;
        const uint8_t *img = gba_save_data(m, &n);
        FILE *sf = img ? fopen(save, "wb") : NULL;
        if (!sf || fwrite(img, 1, n, sf) != n || fclose(sf) != 0)
            fprintf(stderr, "gba_run: cannot write %s\n", save);
    }

    if (hash) {
        uint64_t h = fnv1a(0xCBF29CE484222325ull, fb, sizeof fb);
        uint64_t r = 0xCBF29CE484222325ull;
        for (uint32_t a = 0x02000000; a < 0x02040000; a += 4) {
            uint32_t v = gba_peek32(m, a);
            r = fnv1a(r, &v, 4);
        }
        for (uint32_t a = 0x03000000; a < 0x03008000; a += 4) {
            uint32_t v = gba_peek32(m, a);
            r = fnv1a(r, &v, 4);
        }
        printf("frame-hash %016llx ram-hash %016llx cycles %llu\n", (unsigned long long)h, (unsigned long long)r,
               (unsigned long long)gba_cycles(m));
    }
    if (pack && stats) {
        gba_recomp_stats st;
        gba_recomp_get_stats(m, &st);
        uint64_t tot = st.native + st.interp_rom;
        printf("recomp: native %llu, interpreted cartridge %llu (fallbacks %llu): %.2f%% native; %llu calls, "
               "%llu slow lookups, %zu missed\n",
               (unsigned long long)st.native, (unsigned long long)st.interp_rom, (unsigned long long)st.fallbacks,
               tot ? 100.0 * (double)st.native / (double)tot : 0.0, (unsigned long long)st.calls,
               (unsigned long long)st.slow_lookups, gba_recomp_missed(m, NULL, 0));
    }
    if (pack && missed) {
        size_t n = gba_recomp_missed(m, NULL, 0);
        uint32_t *v = malloc((n ? n : 1) * sizeof *v);
        if (!v || gba_pack_write_discovered(missed, v, gba_recomp_missed(m, v, n), NULL) != 0)
            fprintf(stderr, "gba_run: cannot write %s\n", missed);
        free(v);
    }

    int rc = 0;
    if (result) {
        char id[5];
        fourcc(gba_peek32(m, RESULT_ADDR + 8), id);
        uint32_t detail = gba_peek32(m, RESULT_ADDR + 12), count = gba_peek32(m, RESULT_ADDR + 20);
        const char *verdict = status == RESULT_PASS ? "PASS" : status == RESULT_FAIL ? "FAIL" : "TIMEOUT";
        printf("%s %s detail=%u checksum=0x%08X count=%u frames=%u stage=%u (after %ld frames, pc=0x%08X)\n", id,
               verdict, detail, gba_peek32(m, RESULT_ADDR + 16), count, gba_peek32(m, RESULT_ADDR + 24),
               gba_peek32(m, RESULT_ADDR + 28), f, gba_reg(m, 15));
        if (count > 120)
            count = 120;
        if (!quiet || status != RESULT_PASS)
            for (uint32_t i = 0; i < count; i++)
                printf("  table[%u] = 0x%08X%s\n", i, gba_peek32(m, RESULT_ADDR + 32 + 4 * i),
                       detail == i + 1 ? "   <- first failing check" : "");
        rc = status == RESULT_PASS ? 0 : status == RESULT_FAIL ? 1 : 2;
    }
    if (reg_result >= 0 && reg_result < 15) {
        /* The verdict counts only once the program sits in its final `b .` loop (same PC a frame later). */
        uint32_t v = gba_reg(m, reg_result), pc = gba_reg(m, 15);
        gba_run_frame(m, NULL);
        bool idle = gba_reg(m, 15) == pc && gba_reg(m, reg_result) == v;
        printf("r%d = %u (%s), pc=0x%08X\n", reg_result, v,
               !idle ? "TIMEOUT: not in an idle loop" : v ? "FAIL: first failed test" : "PASS", pc);
        rc = !idle ? 2 : v ? 1 : rc;
    }
    gba_destroy(m);
    gba_destroy(ref);
    gba_pack_close(&pk);
    return diverged ? 4 : rc;
}
