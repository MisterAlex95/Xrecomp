/* gba_run: runs a GBA ROM headless on the core (XBA-152 / XBA-153).
 *
 *   gba_run ROM [--frames N] [--bios PATH] [--bios-boot] [--result] [--ppm OUT.ppm] [--keys MASK] [--quiet]
 *
 * --frames N    frames to run (default 600); with --result, the most frames to wait for a verdict
 * --bios PATH   a 16 KB BIOS image (e.g. Cult-of-GBA, tools/consoles/build_gba_bios.sh); default: built-in
 * --bios-boot   boot from address 0 through the BIOS instead of the direct boot
 * --result      read the 512-byte xba_result block of our homebrew (consoles/gba/tests/homebrew/common/
 *               xba_result.h) at each VBlank, stop at the verdict; exit 0 on PASS, 1 on FAIL, 2 on timeout
 * --ppm OUT     write the last frame as a binary PPM
 * --keys MASK   keys held during the whole run (KEYINPUT bit order, 1 = pressed)
 * --reg-result R  after the frames, exit 0 if register rR is 0, 1 if not, 2 if the CPU is not in an idle
 *               loop (jsmolka/gba-tests leave the number of the first failed test in r12 and spin: --reg-result 12)
 * Prints the frame rate reached on stderr. Exit 3 on a usage or file error. */
#define _POSIX_C_SOURCE 199309L /* clock_gettime */
#include "gba_core.h"

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

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
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
    const char *rom_path = NULL, *bios_path = NULL, *ppm = NULL;
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
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc)
            keys = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--reg-result") && i + 1 < argc)
            reg_result = (int)strtol(argv[++i], NULL, 0);
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
                            "[--keys MASK] [--reg-result R] [--quiet]\n");
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
    free(rom);
    free(bios);
    if (!m) {
        fprintf(stderr, "gba_run: bad ROM or BIOS size\n");
        return 3;
    }
    if (bios_boot)
        gba_boot_bios(m);
    gba_set_keys(m, (uint16_t)keys);

    static uint16_t fb[GBA_SCREEN_W * GBA_SCREEN_H];
    uint32_t status = 0;
    long f;
    double t0 = now_s();
    for (f = 0; f < frames; f++) {
        gba_run_frame(m, fb);
        if (result && gba_peek32(m, RESULT_ADDR) == RESULT_MAGIC) {
            status = gba_peek32(m, RESULT_ADDR + 4);
            if (status) {
                f++;
                break;
            }
        }
    }
    double dt = now_s() - t0;
    if (!quiet)
        fprintf(stderr, "gba_run: %ld frames in %.3f s = %.1f frames/s (%.1fx real time)\n", f, dt,
                dt > 0 ? f / dt : 0.0, dt > 0 ? f / dt / 59.7275 : 0.0);
    if (ppm && write_ppm(ppm, fb) != 0)
        fprintf(stderr, "gba_run: cannot write %s\n", ppm);

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
    return rc;
}
