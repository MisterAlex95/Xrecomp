/* n64_run: runs a Nintendo 64 ROM headless on the core (XBA-169 / 171 / 172).
 *
 *   n64_run ROM [--frames N] [--result] [--ppm OUT.ppm] [--wav OUT.wav] [--tty] [--pad MASK] [--pal | --ntsc]
 *           [--save PATH] [--hash] [--info] [--regs] [--quiet]
 *           [--pack LIB.so [--lockstep] [--missed FILE] [--stats]]
 *
 * ROM           a .z64 / .v64 / .n64 file (any byte order)
 * --frames N    frames (VI interrupts) to run (default 600); with --result, the most frames to wait
 * --result      read the 512-byte xba_result block of our homebrew (consoles/n64/tests/homebrew/common/xba_result.h,
 *               physical 0x00300000, big-endian) after each frame, stop at the verdict; exit 0 on PASS, 1 on FAIL,
 *               2 on timeout
 * --ppm OUT     write the last frame the VI shows as a binary PPM (black when blank)
 * --wav OUT     write the audio of the run (n64_audio_pull after each frame) as a 48 kHz stereo 16-bit WAV
 * --tty         copy the IS-Viewer debug text (libdragon debugf, osSyncPrintf) to stdout
 * --pad MASK    buttons held on controller 1 for the whole run (N64_BTN_* bits)
 * --pal/--ntsc  force the TV type (default: the ROM's region)
 * --save PATH   cartridge save file: loaded at start if it exists (its size picks the kind), written at exit when the
 *               game wrote it
 * --hash        print a hash of the RDRAM (FNV-1a 64), the instruction and cycle counts
 * --info        print the ROM header and the detected CIC
 * --regs        print the CPU state at the end (PC, COP0 Status / Cause / EPC / BadVAddr, GPRs)
 * --pack LIB    a recompiled title pack (n64_recomp.h: libtitle.so built by n64_tools.recomp /
 *               tools/consoles/n64_recomp_build.py): its lifted code runs natively, the interpreter runs the rest
 * --lockstep    with --pack: an interpreter-only twin runs alongside; after every frame the whole state (CPU, COP0, TLB,
 *               FPU, time, scheduler, MI, RDRAM, RSP memory) and the picture are compared. Prints "LOCKSTEP OK" or
 *               the first difference ("LOCKSTEP FAIL"), exit 1 then
 * --lockstep-cycles N  compare every N CPU cycles instead of every frame (then --frames counts these steps), and
 *               --lockstep-from C runs C cycles before the first comparison (to close in on a divergence)
 * --missed FILE with --pack: append the code the interpreter had to run to FILE (the discovery loop's input)
 * --stats       with --pack: calls, fallbacks, lookups, re-hashes, and the native share of the instructions (packs
 *               built with -DN64_RC_COUNT)
 * Prints the frame rate reached on stderr (unless --quiet). Exit 3 on a usage or file error. */
#define _POSIX_C_SOURCE 199309L /* clock_gettime */
#include "n64_core.h"
#include "n64_recomp.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RESULT_ADDR 0xA0300000u
#define RESULT_MAGIC 0x58424152u
#define RESULT_PASS 0x50415353u
#define RESULT_FAIL 0x4641494Cu

static void on_tty(void *ctx, char c)
{
    (void)ctx;
    fputc(c, stdout);
    if (c == '\n')
        fflush(stdout);
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

static int write_ppm(const char *path, n64_machine *m)
{
    static uint32_t px[N64_SCREEN_MAX_W * N64_SCREEN_MAX_H];
    int w = 320, h = 240;
    if (n64_display(m, px, N64_SCREEN_MAX_W, N64_SCREEN_MAX_H, &w, &h) != 0)
        memset(px, 0, sizeof px);
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

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int usage(void)
{
    fprintf(stderr, "usage: n64_run ROM [--frames N] [--result] [--ppm OUT] [--wav OUT] [--tty] [--pad MASK]\n"
                    "               [--pal | --ntsc] [--save PATH] [--hash] [--info] [--regs] [--quiet]\n"
                    "               [--pack LIB.so [--lockstep] [--missed FILE] [--stats]]\n");
    return 3;
}

static n64_save_kind kind_of_size(size_t n)
{
    switch (n) {
    case 512: return N64_SAVE_EEPROM4K;
    case 2048: return N64_SAVE_EEPROM16K;
    case 0x8000: return N64_SAVE_SRAM;
    case 0x20000: return N64_SAVE_FLASH;
    default: return N64_SAVE_NONE;
    }
}

static uint64_t display_hash(n64_machine *m)
{
    static uint32_t px[N64_SCREEN_MAX_W * N64_SCREEN_MAX_H];
    int w = 0, h = 0;
    if (n64_display(m, px, N64_SCREEN_MAX_W, N64_SCREEN_MAX_H, &w, &h) != 0)
        return 0;
    uint64_t hv = 0xCBF29CE484222325ull;
    for (int i = 0; i < w * h; i++) {
        hv ^= px[i];
        hv *= 0x100000001B3ull;
    }
    return hv ^ ((uint64_t)w << 32 | (uint32_t)h);
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *ppm = NULL, *wav = NULL, *save = NULL, *pack = NULL, *missed = NULL;
    long frames = 600;
    bool result = false, quiet = false, hash = false, tty = false, info = false, regs = false;
    bool lockstep = false, stats = false;
    long long lock_cycles = 0, lock_from = 0;
    uint16_t pad = 0;
    n64_options opts = {.force_pal = -1};

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--frames") && more)
            frames = strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--result"))
            result = true;
        else if (!strcmp(a, "--ppm") && more)
            ppm = argv[++i];
        else if (!strcmp(a, "--wav") && more)
            wav = argv[++i];
        else if (!strcmp(a, "--tty"))
            tty = true;
        else if (!strcmp(a, "--pad") && more)
            pad = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--pal"))
            opts.force_pal = 1;
        else if (!strcmp(a, "--ntsc"))
            opts.force_pal = 0;
        else if (!strcmp(a, "--save") && more)
            save = argv[++i];
        else if (!strcmp(a, "--hash"))
            hash = true;
        else if (!strcmp(a, "--info"))
            info = true;
        else if (!strcmp(a, "--regs"))
            regs = true;
        else if (!strcmp(a, "--quiet"))
            quiet = true;
        else if (!strcmp(a, "--pack") && more)
            pack = argv[++i];
        else if (!strcmp(a, "--missed") && more)
            missed = argv[++i];
        else if (!strcmp(a, "--lockstep"))
            lockstep = true;
        else if (!strcmp(a, "--lockstep-cycles") && more)
            lock_cycles = strtoll(argv[++i], NULL, 0);
        else if (!strcmp(a, "--lockstep-from") && more)
            lock_from = strtoll(argv[++i], NULL, 0);
        else if (!strcmp(a, "--stats"))
            stats = true;
        else if (a[0] != '-' && !rom_path)
            rom_path = a;
        else
            return usage();
    }
    if (!rom_path || frames <= 0)
        return usage();

    size_t rom_size = 0;
    uint8_t *rom = load_file(rom_path, &rom_size);
    if (!rom) {
        fprintf(stderr, "n64_run: cannot read %s\n", rom_path);
        return 3;
    }
    size_t save_size = 0;
    uint8_t *save_data = save ? load_file(save, &save_size) : NULL;
    if (save_data)
        opts.save_kind = kind_of_size(save_size);
    char err[256] = "";
    n64_machine *m = n64_create(rom, rom_size, &opts, err, sizeof err);
    n64_machine *twin = NULL;
    if (m && pack && lockstep)
        twin = n64_create(rom, rom_size, &opts, err, sizeof err);
    free(rom);
    if (!m || (pack && lockstep && !twin)) {
        fprintf(stderr, "n64_run: %s\n", err);
        free(save_data);
        n64_destroy(m);
        return 3;
    }
    if (save_data) {
        if (n64_save_load(m, opts.save_kind, save_data, save_size) != 0)
            fprintf(stderr, "n64_run: %s: not a save image (%zu bytes)\n", save, save_size);
        if (twin)
            n64_save_load(twin, opts.save_kind, save_data, save_size);
        free(save_data);
    }
    void *lib = NULL;
    if (pack) {
        lib = dlopen(pack, RTLD_NOW | RTLD_LOCAL);
        n64_recomp_title_table_fn get = lib ? (n64_recomp_title_table_fn)dlsym(lib, N64_RECOMP_ENTRY_SYMBOL) : NULL;
        const n64_recomp_table *t = get ? get() : NULL;
        if (!t || n64_attach_recomp(m, t) != 0) {
            fprintf(stderr, "n64_run: %s: %s\n", pack, !lib ? dlerror() : !t ? "no n64_recomp_title_table" : "ABI");
            n64_destroy(m);
            n64_destroy(twin);
            return 3;
        }
        if (!quiet)
            fprintf(stderr, "n64_run: pack %s: %u functions, boot 0x%08X\n", pack, t->count, t->boot_pc);
    }
    if (info) {
        const n64_rom_info *ri = n64_info(m);
        printf("rom: \"%s\" code %s version %u region '%c' (%s) crc %08X %08X boot 0x%08X cic %s (ipl3 crc32 %08X)\n",
               ri->title, ri->game_code, ri->version, ri->region ? ri->region : '?', ri->pal ? "PAL" : "NTSC",
               ri->crc1, ri->crc2, ri->boot_address, n64_cic_name(ri->cic), ri->ipl3_crc);
    }
    if (tty)
        n64_set_tty(m, on_tty, NULL);
    n64_set_controller(m, 0, 1, pad, 0, 0);
    if (twin)
        n64_set_controller(twin, 0, 1, pad, 0, 0);
    bool lock_bad = false;

    FILE *wf = NULL;
    uint32_t wav_frames = 0;
    if (wav) {
        wf = fopen(wav, "wb");
        if (!wf) {
            fprintf(stderr, "n64_run: cannot write %s\n", wav);
            n64_destroy(m);
            return 3;
        }
        fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, wf);
        put32(wf, 16);
        put16(wf, 1);
        put16(wf, 2);
        put32(wf, N64_AUDIO_RATE);
        put32(wf, N64_AUDIO_RATE * 4);
        put16(wf, 4);
        put16(wf, 16);
        fwrite("data\0\0\0\0", 1, 8, wf);
    }

    int rc = result ? 2 : 0;
    double t0 = now_s();
    long f;
    if (twin && lock_from > 0) {
        n64_run_cycles(m, (uint64_t)lock_from);
        n64_run_cycles(twin, (uint64_t)lock_from);
    }
    for (f = 0; f < frames; f++) {
        if (twin && lock_cycles > 0)
            n64_run_cycles(m, (uint64_t)lock_cycles);
        else
            n64_run_frame(m);
        if (twin) {
            if (lock_cycles > 0)
                n64_run_cycles(twin, (uint64_t)lock_cycles);
            else
                n64_run_frame(twin);
            char why[160] = "";
            int d = n64_recomp_compare(m, twin, why, sizeof why);
            if (!d && display_hash(m) != display_hash(twin)) {
                d = 1;
                snprintf(why, sizeof why, "the picture");
            }
            if (d) {
                printf("LOCKSTEP FAIL at frame %ld: %s (recompiled vs interpreter; pc 0x%016llX vs 0x%016llX, "
                       "instructions %llu vs %llu, cycles %llu vs %llu)\n",
                       f + 1, why, (unsigned long long)n64_pc(m), (unsigned long long)n64_pc(twin),
                       (unsigned long long)n64_instructions(m), (unsigned long long)n64_instructions(twin),
                       (unsigned long long)n64_cycles(m), (unsigned long long)n64_cycles(twin));
                lock_bad = true;
                f++;
                break;
            }
        }
        if (wf) {
            int16_t buf[2048 * 2];
            size_t n;
            while ((n = n64_audio_pull(m, buf, 2048)) > 0) {
                for (size_t i = 0; i < n * 2; i++)
                    put16(wf, (uint16_t)buf[i]);
                wav_frames += (uint32_t)n;
            }
        }
        if (result && n64_peek32(m, RESULT_ADDR) == RESULT_MAGIC) {
            uint32_t st = n64_peek32(m, RESULT_ADDR + 4);
            if (st == RESULT_PASS || st == RESULT_FAIL) {
                rc = st == RESULT_PASS ? 0 : 1;
                f++;
                break;
            }
        }
    }
    double dt = now_s() - t0;
    if (twin && !lock_bad)
        printf("LOCKSTEP OK (%ld frames, the whole state identical after each)\n", f);
    if (pack && missed) {
        int n = n64_recomp_write_missed(m, missed);
        if (n < 0)
            fprintf(stderr, "n64_run: cannot write %s\n", missed);
        else if (!quiet)
            fprintf(stderr, "n64_run: %d new missed records -> %s\n", n, missed);
    }
    if (pack && stats) {
        n64_recomp_stats st;
        n64_recomp_get_stats(m, &st);
        uint64_t total = n64_instructions(m);
        printf("recomp: %llu calls, %llu interpreted RDRAM instructions (%llu fallbacks), %llu slow lookups, "
               "%llu re-hashes (%llu stale), %llu interrupts\n",
               (unsigned long long)st.calls, (unsigned long long)st.interp_ram, (unsigned long long)st.fallbacks,
               (unsigned long long)st.slow_lookups, (unsigned long long)st.rehashes, (unsigned long long)st.stale,
               (unsigned long long)st.interrupts);
        if (st.native)
            printf("recomp: %.2f%% native (%llu of %llu instructions)\n", total ? 100.0 * st.native / total : 0.0,
                   (unsigned long long)st.native, (unsigned long long)total);
    }

    if (result) {
        uint32_t magic = n64_peek32(m, RESULT_ADDR), st = n64_peek32(m, RESULT_ADDR + 4);
        uint32_t id = n64_peek32(m, RESULT_ADDR + 8), detail = n64_peek32(m, RESULT_ADDR + 12);
        uint32_t count = n64_peek32(m, RESULT_ADDR + 20);
        char fourcc[5] = {(char)(id >> 24), (char)(id >> 16), (char)(id >> 8), (char)id, 0};
        for (int i = 0; i < 4; i++)
            if (fourcc[i] < 32 || fourcc[i] > 126)
                fourcc[i] = '?';
        printf("result: %s test=%s detail=%u checksum=%08X count=%u frames=%u stage=%u (%ld frames run)\n",
               magic != RESULT_MAGIC ? "NOBLOCK" : st == RESULT_PASS ? "PASS" : st == RESULT_FAIL ? "FAIL" : "TIMEOUT",
               fourcc, detail, n64_peek32(m, RESULT_ADDR + 16), count, n64_peek32(m, RESULT_ADDR + 24),
               n64_peek32(m, RESULT_ADDR + 28), f);
        if (count > 120)
            count = 120;
        for (uint32_t i = 0; i < count && (st == RESULT_FAIL || !quiet); i++)
            printf("  table[%u] = 0x%08X\n", i, n64_peek32(m, RESULT_ADDR + 32 + 4 * i));
        if (magic != RESULT_MAGIC || (st != RESULT_PASS && st != RESULT_FAIL))
            printf("  pc 0x%016llX, %llu instructions\n", (unsigned long long)n64_pc(m),
                   (unsigned long long)n64_instructions(m));
    }
    if (ppm && write_ppm(ppm, m) != 0)
        fprintf(stderr, "n64_run: cannot write %s\n", ppm);
    if (wf) {
        fseek(wf, 4, SEEK_SET);
        put32(wf, 36 + wav_frames * 4);
        fseek(wf, 40, SEEK_SET);
        put32(wf, wav_frames * 4);
        fclose(wf);
    }
    if (save && n64_save_take_dirty(m, n64_save_kind_detected(m))) {
        size_t n = 0;
        const uint8_t *d = n64_save_data(m, n64_save_kind_detected(m), &n);
        FILE *sf = d ? fopen(save, "wb") : NULL;
        if (!sf || fwrite(d, 1, n, sf) != n)
            fprintf(stderr, "n64_run: cannot write %s\n", save);
        if (sf)
            fclose(sf);
    }
    if (regs) {
        printf("pc %016llX status %08llX cause %08llX epc %016llX badvaddr %016llX count %08llX\n",
               (unsigned long long)n64_pc(m), (unsigned long long)n64_cop0(m, 12), (unsigned long long)n64_cop0(m, 13),
               (unsigned long long)n64_cop0(m, 14), (unsigned long long)n64_cop0(m, 8),
               (unsigned long long)n64_cop0(m, 9));
        for (int r = 0; r < 32; r += 4)
            printf("r%-2d %016llX %016llX %016llX %016llX\n", r, (unsigned long long)n64_reg(m, r),
                   (unsigned long long)n64_reg(m, r + 1), (unsigned long long)n64_reg(m, r + 2),
                   (unsigned long long)n64_reg(m, r + 3));
    }
    if (hash) {
        uint64_t h = 0xCBF29CE484222325ull;
        const uint8_t *p = n64_rdram(m);
        for (uint32_t i = 0; i < n64_rdram_size(m); i++) {
            h ^= p[i];
            h *= 0x100000001B3ull;
        }
        printf("hash: rdram=%016llx instructions=%llu cycles=%llu\n", (unsigned long long)h,
               (unsigned long long)n64_instructions(m), (unsigned long long)n64_cycles(m));
    }
    if (!quiet)
        fprintf(stderr, "n64_run: %ld frames in %.2f s (%.1f fps, %.1f MIPS, %.1f MHz emulated)\n", f, dt,
                dt > 0 ? f / dt : 0.0, dt > 0 ? (double)n64_instructions(m) / dt / 1e6 : 0.0,
                dt > 0 ? (double)n64_cycles(m) / dt / 1e6 : 0.0);
    n64_destroy(m);
    n64_destroy(twin);
    /* lib stays loaded until exit (no dlclose: nothing may call into it after the machines are gone anyway) */
    (void)lib;
    return lock_bad ? 1 : rc;
}
