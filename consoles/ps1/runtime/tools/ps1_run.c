/* ps1_run: runs a PlayStation program headless on the core (XBA-162 / XBA-163).
 *
 *   ps1_run [--bios PATH] [--exe X.exe | --disc X.cue] [--frames N] [--result] [--ppm OUT.ppm] [--wav OUT.wav]
 *           [--tty] [--until-tty TEXT] [--pad MASK] [--hash] [--quiet]
 *
 * --bios PATH   a 512 KB BIOS image; default $RECOMP_PS1_BIOS, else build/ps1-bios/openbios.bin (OpenBIOS:
 *               tools/consoles/build_ps1_bios.sh). Never a Sony BIOS dump in the repository.
 * --exe X.exe   a PS-X EXE, fast-booted where the BIOS would enter its shell (a bare FILE argument ending in .exe
 *               means the same; any other bare FILE is a disc)
 * --disc X.cue  a disc image (.cue / .bin / .iso / .chd, needs ps1_disc.c and ps1_cdrom.c); with --exe too, the EXE
 *               boots and the disc is in the drive
 * --frames N    frames to run (default 600); with --result / --until-tty, the most frames to wait
 * --result      read the 512-byte xba_result block of our homebrew (consoles/ps1/tests/homebrew/common/xba_result.h,
 *               physical 0x001F0000) at each VBlank, stop at the verdict; exit 0 on PASS, 1 on FAIL, 2 on timeout
 * --ppm OUT     write the last displayed frame as a binary PPM (black when the display is off)
 * --wav OUT     write the audio of the run (ps1_audio_pull after each frame) as a 44.1 kHz stereo 16-bit WAV
 * --tty         copy the console text (BIOS putchar / puts, the debug UART) to stdout
 * --until-tty T stop once the console text contains T (exit 0), exit 2 if it never does
 * --pad MASK    buttons held on pad 1 for the whole run (PS1_PAD_* bits, 1 = pressed)
 * --hash        print a hash of the RAM and VRAM (FNV-1a 64) and the cycle count
 * --pack LIB    attach a recompiled title pack (ps1_recomp.h: a shared library exporting ps1_recomp_title_table)
 * --lockstep    with --pack: an interpreter-only twin runs alongside, the whole state (CPU, cycles, scheduler, RAM,
 *               scratchpad, GTE, VRAM) compared after every frame; prints "LOCKSTEP OK" or the first difference
 *               (exit 4)
 * --lockstep-cycles N  (debugging) compare every N cycles instead of every frame (1: about every instruction),
 *               and print both PCs at the first difference
 * --missed FILE append the code the interpreter had to run (address, hash, bytes: the discovered file of
 *               ps1_recomp.h) to FILE at exit
 * --stats       print the recompiler's counters (native share needs a pack built with -DPS1_RC_COUNT)
 * Prints the frame rate reached on stderr (unless --quiet). Exit 3 on a usage or file error. */
#define _POSIX_C_SOURCE 199309L /* clock_gettime */
#include "ps1_core.h"
#include "ps1_devices.h"
#include "ps1_recomp.h"

#include <dlfcn.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RESULT_ADDR 0x801F0000u
#define RESULT_MAGIC 0x58424152u
#define RESULT_PASS 0x50415353u
#define RESULT_FAIL 0x4641494Cu

typedef struct tty_state {
    bool echo;
    char tail[256]; /* the last characters, for --until-tty */
    size_t len;
} tty_state;

static void on_tty(void *ctx, char c)
{
    tty_state *t = ctx;
    if (t->echo) {
        fputc(c, stdout);
        if (c == '\n')
            fflush(stdout);
    }
    if (t->len == sizeof t->tail - 1) {
        memmove(t->tail, t->tail + 128, t->len - 128);
        t->len -= 128;
    }
    t->tail[t->len++] = c ? c : ' ';
    t->tail[t->len] = 0;
}

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

static int write_ppm(const char *path, ps1_machine *m)
{
    static uint32_t px[640 * 512];
    int w = 320, h = 240;
    if (ps1_display(m, px, 640, 512, &w, &h) != 0) {
        memset(px, 0, sizeof px);
        if (w <= 0 || h <= 0 || w > 640 || h > 512) {
            w = 320;
            h = 240;
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = {(uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i]};
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f);
}

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, f);
}

static void put16(FILE *f, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    fwrite(b, 1, 2, f);
}

static uint64_t fnv64(const uint8_t *p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int usage(void)
{
    fprintf(stderr, "usage: ps1_run [--bios PATH] [--exe X.exe | --disc X.cue] [--frames N] [--result] [--ppm OUT]\n"
                    "               [--wav OUT] [--tty] [--until-tty TEXT] [--pad MASK] [--hash] [--quiet]\n"
                    "               [--pack LIB.so [--lockstep]] [--missed FILE] [--stats]\n");
    return 3;
}

int main(int argc, char **argv)
{
    const char *bios_path = getenv("RECOMP_PS1_BIOS"), *exe_path = NULL, *disc_path = NULL;
    const char *ppm = NULL, *wav = NULL, *until = NULL, *pack = NULL, *missed = NULL;
    bool lockstep = false, stats = false;
    long lock_cycles = 0;
    long frames = 600;
    bool result = false, quiet = false, hash = false;
    tty_state tty = {0};
    uint16_t pad = 0;

    if (!bios_path || !*bios_path)
        bios_path = "build/ps1-bios/openbios.bin";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--bios") && more)
            bios_path = argv[++i];
        else if (!strcmp(a, "--exe") && more)
            exe_path = argv[++i];
        else if (!strcmp(a, "--disc") && more)
            disc_path = argv[++i];
        else if (!strcmp(a, "--frames") && more)
            frames = strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--result"))
            result = true;
        else if (!strcmp(a, "--ppm") && more)
            ppm = argv[++i];
        else if (!strcmp(a, "--wav") && more)
            wav = argv[++i];
        else if (!strcmp(a, "--tty"))
            tty.echo = true;
        else if (!strcmp(a, "--until-tty") && more)
            until = argv[++i];
        else if (!strcmp(a, "--pad") && more)
            pad = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--hash"))
            hash = true;
        else if (!strcmp(a, "--pack") && more)
            pack = argv[++i];
        else if (!strcmp(a, "--lockstep"))
            lockstep = true;
        else if (!strcmp(a, "--lockstep-cycles") && more)
            lockstep = true, lock_cycles = strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--missed") && more)
            missed = argv[++i];
        else if (!strcmp(a, "--stats"))
            stats = true;
        else if (!strcmp(a, "--quiet"))
            quiet = true;
        else if (a[0] != '-') {
            size_t n = strlen(a);
            if (n > 4 && (!strcmp(a + n - 4, ".exe") || !strcmp(a + n - 4, ".EXE")))
                exe_path = a;
            else
                disc_path = a;
        } else
            return usage();
    }
    if (frames <= 0)
        return usage();

    size_t bios_size = 0, exe_size = 0;
    uint8_t *bios = load_file(bios_path, &bios_size), *exe = NULL;
    if (!bios) {
        fprintf(stderr, "ps1_run: cannot read the BIOS %s (build OpenBIOS: tools/consoles/build_ps1_bios.sh)\n",
                bios_path);
        return 3;
    }
    if (exe_path && !(exe = load_file(exe_path, &exe_size))) {
        fprintf(stderr, "ps1_run: cannot read %s\n", exe_path);
        return 3;
    }
    char err[256] = "";
    ps1_machine *m = ps1_create(bios, bios_size, disc_path, exe, exe_size, err, sizeof err);
    ps1_machine *twin = NULL;
    if (m && pack && lockstep)
        twin = ps1_create(bios, bios_size, disc_path, exe, exe_size, err, sizeof err);
    free(bios);
    free(exe);
    if (!m || (pack && lockstep && !twin)) {
        fprintf(stderr, "ps1_run: %s\n", err);
        return 3;
    }
    void *lib = NULL;
    if (pack) {
        lib = dlopen(pack, RTLD_NOW | RTLD_LOCAL);
        ps1_recomp_title_table_fn get = lib ? (ps1_recomp_title_table_fn)dlsym(lib, PS1_RECOMP_ENTRY_SYMBOL) : NULL;
        if (!get || ps1_attach_recomp(m, get()) != 0) {
            fprintf(stderr, "ps1_run: cannot load the pack %s: %s\n", pack, lib ? "bad table" : dlerror());
            return 3;
        }
        if (!quiet)
            fprintf(stderr, "ps1_run: pack %s, %u functions\n", pack, get()->count);
    } else if (missed) {
        fprintf(stderr, "ps1_run: --missed needs --pack (lift the EXE first: ps1_tools.recomp)\n");
        return 3;
    }
    if (twin)
        ps1_set_pad(twin, 0, PS1_PAD_DIGITAL, pad, 0x80, 0x80, 0x80, 0x80);
    ps1_set_tty(m, on_tty, &tty);
    ps1_set_pad(m, 0, PS1_PAD_DIGITAL, pad, 0x80, 0x80, 0x80, 0x80);

    FILE *wf = NULL;
    uint32_t wav_frames = 0;
    if (wav) {
        wf = fopen(wav, "wb");
        if (!wf) {
            fprintf(stderr, "ps1_run: cannot write %s\n", wav);
            ps1_destroy(m);
            return 3;
        }
        fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, wf);
        put32(wf, 16);
        put16(wf, 1);
        put16(wf, 2);
        put32(wf, PS1_AUDIO_RATE);
        put32(wf, PS1_AUDIO_RATE * 4);
        put16(wf, 4);
        put16(wf, 16);
        fwrite("data\0\0\0\0", 1, 8, wf);
    }

    int rc = result || until ? 2 : 0;
    double t0 = now_s();
    long f;
    long diverged = -1;
    char why[256] = "";
    for (f = 0; f < frames; f++) {
        if (twin && lock_cycles > 0) {
            uint64_t end = ps1_cycles(m) + 33868800 / 60;
            while (ps1_cycles(m) < end) {
                uint32_t pa = ps1_pc(twin), pb = ps1_pc(m);
                ps1_run_cycles(m, (uint64_t)lock_cycles);
                while (ps1_cycles(twin) < ps1_cycles(m))
                    ps1_run_cycles(twin, ps1_cycles(m) - ps1_cycles(twin));
                if (ps1_recomp_compare(twin, m, why, sizeof why) != 0) {
                    printf("LOCKSTEP DIFF at cycle %llu: %s; before: interpreter pc 0x%08X, recompiled pc 0x%08X; "
                           "after: 0x%08X, 0x%08X\n", (unsigned long long)ps1_cycles(m), why, pa, pb, ps1_pc(twin),
                           ps1_pc(m));
                    break;
                }
            }
            if (why[0]) {
                diverged = f;
                f++;
                break;
            }
            continue;
        }
        ps1_run_frame(m);
        if (twin) {
            ps1_run_frame(twin);
            if (ps1_recomp_compare(twin, m, why, sizeof why) != 0) {
                diverged = f;
                f++;
                break;
            }
        }
        if (wf) {
            int16_t buf[2048 * 2];
            size_t n;
            while ((n = ps1_audio_pull(m, buf, 2048)) > 0) {
                for (size_t i = 0; i < n * 2; i++)
                    put16(wf, (uint16_t)buf[i]);
                wav_frames += (uint32_t)n;
            }
        }
        if (result && ps1_peek32(m, RESULT_ADDR) == RESULT_MAGIC) {
            uint32_t st = ps1_peek32(m, RESULT_ADDR + 4);
            if (st == RESULT_PASS || st == RESULT_FAIL) {
                rc = st == RESULT_PASS ? 0 : 1;
                f++;
                break;
            }
        }
        if (until && strstr(tty.tail, until)) {
            rc = 0;
            f++;
            break;
        }
    }
    double dt = now_s() - t0;
    if (twin) {
        if (diverged >= 0) {
            printf("LOCKSTEP DIFF at frame %ld: %s (interpreter vs recompiled)\n", diverged, why);
            rc = 4;
        } else {
            printf("LOCKSTEP OK: %ld frames identical to the interpreter\n", f);
        }
    }
    if (stats && pack) {
        ps1_recomp_stats st;
        ps1_recomp_get_stats(m, &st);
        double total = (double)st.native + (double)st.interp_ram;
        printf("recomp: %.2f%% native (%llu native, %llu interpreted RAM instructions, %llu handed over), %llu calls, "
               "%llu slow lookups, %llu rehashes, %llu stale, %llu interrupts\n",
               total > 0 ? 100.0 * (double)st.native / total : 0.0, (unsigned long long)st.native,
               (unsigned long long)st.interp_ram, (unsigned long long)st.fallbacks, (unsigned long long)st.calls,
               (unsigned long long)st.slow_lookups, (unsigned long long)st.rehashes, (unsigned long long)st.stale,
               (unsigned long long)st.interrupts);
    }
    if (missed) {
        int n = ps1_recomp_write_missed(m, missed);
        if (n < 0)
            fprintf(stderr, "ps1_run: cannot write %s\n", missed);
        else if (stats || !quiet)
            printf("missed: %d new record(s) -> %s\n", n, missed);
    }

    if (result) {
        uint32_t magic = ps1_peek32(m, RESULT_ADDR), st = ps1_peek32(m, RESULT_ADDR + 4);
        uint32_t id = ps1_peek32(m, RESULT_ADDR + 8), detail = ps1_peek32(m, RESULT_ADDR + 12);
        uint32_t count = ps1_peek32(m, RESULT_ADDR + 20);
        char fourcc[5] = {(char)(id >> 24), (char)(id >> 16), (char)(id >> 8), (char)id, 0};
        for (int i = 0; i < 4; i++)
            if (fourcc[i] < 32 || fourcc[i] > 126)
                fourcc[i] = '?';
        printf("result: %s test=%s detail=%u checksum=%08X count=%u frames=%u stage=%u (%ld frames run)\n",
               magic != RESULT_MAGIC ? "NOBLOCK" : st == RESULT_PASS ? "PASS" : st == RESULT_FAIL ? "FAIL" : "TIMEOUT",
               fourcc, detail, ps1_peek32(m, RESULT_ADDR + 16), count, ps1_peek32(m, RESULT_ADDR + 24),
               ps1_peek32(m, RESULT_ADDR + 28), f);
        if (count > 120)
            count = 120;
        for (uint32_t i = 0; i < count && (st == RESULT_FAIL || !quiet); i++)
            printf("  table[%u] = 0x%08X\n", i, ps1_peek32(m, RESULT_ADDR + 32 + 4 * i));
    }
    if (until && rc != 0)
        printf("until-tty: \"%s\" not seen in %ld frames\n", until, f);
    if (ppm && write_ppm(ppm, m) != 0)
        fprintf(stderr, "ps1_run: cannot write %s\n", ppm);
    if (wf) {
        fseek(wf, 4, SEEK_SET);
        put32(wf, 36 + wav_frames * 4);
        fseek(wf, 40, SEEK_SET);
        put32(wf, wav_frames * 4);
        fclose(wf);
    }
    if (hash) {
        uint64_t h = fnv64(ps1_ram(m), PS1_RAM_SIZE, 0xCBF29CE484222325ull);
        const uint16_t *vram = ps1_gpu_vram(ps1_get_gpu(m));
        uint64_t hv = vram ? fnv64((const uint8_t *)vram, 1024 * 512 * 2, 0xCBF29CE484222325ull) : 0;
        printf("hash: ram=%016llx vram=%016llx cycles=%llu\n", (unsigned long long)h, (unsigned long long)hv,
               (unsigned long long)ps1_cycles(m));
    }
    if (!quiet)
        fprintf(stderr, "ps1_run: %ld frames in %.2f s (%.1f fps, %.1f MHz emulated)\n", f, dt, dt > 0 ? f / dt : 0.0,
                dt > 0 ? (double)ps1_cycles(m) / dt / 1e6 : 0.0);
    ps1_destroy(m);
    if (twin)
        ps1_destroy(twin);
    if (lib)
        dlclose(lib);
    return rc;
}
