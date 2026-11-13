/* kernel_disc_bench (XBA-126): read throughput of D: through the real file layer, for the same tree as a folder
 * and as disc images (plain, split, CSO). Not a test (no ctest entry): a number for the ticket.
 *
 *   kernel_disc_bench SAVE_DIR GAME_DIR_OR_IMAGE... (each argument: a folder, an image, or fd:N)
 *
 * For each source: every file of D:\bench\ read sequentially in 64 KB reads (as a streaming title does), then
 * 4000 random 4 KB reads per file (as a title seeking in a pack). Prints MB/s and µs per random read. The OS page
 * cache is warm after the first pass of each source (the first pass is not counted), so this measures the cost of
 * the layer itself (pread, the compressed block cache and its decoders), not the storage. */
#define _GNU_SOURCE
#include "kernel.h"
#include "kernel_disc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{ (void)level; (void)subsystem; (void)fmt; }
const char *xbox_LookupSymbolicLink(const char *link)
{ (void)link; return NULL; }

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static HANDLE open_file(const char *path)
{
    XBOX_ANSI_STRING name = { (USHORT)strlen(path), (USHORT)(strlen(path) + 1), (PCHAR)path };
    XBOX_OBJECT_ATTRIBUTES oa = { 0 };
    XBOX_IO_STATUS_BLOCK ios = { 0 };
    HANDLE h = NULL;
    oa.ObjectName = &name;
    if (xbox_NtCreateFile(&h, 0x80000000u, &oa, &ios, NULL, 0x80, 1, XBOX_FILE_OPEN, 0) != 0)
        return NULL;
    return h;
}

static const char *FILES[] = { "D:\\bench\\random1.bin", "D:\\bench\\random2.bin", "D:\\bench\\text1.bin",
                               "D:\\bench\\text2.bin" };

static int pass(double *seq_mbs, double *rand_us)
{
    static uint8_t buf[65536];
    uint64_t bytes = 0;
    double t0 = now();
    for (size_t i = 0; i < sizeof FILES / sizeof FILES[0]; i++) {
        HANDLE h = open_file(FILES[i]);
        if (!h)
            return -1;
        for (;;) {
            XBOX_IO_STATUS_BLOCK ios = { 0 };
            if (xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, sizeof buf, NULL) != 0)
                break;
            bytes += ios.Information;
        }
        xbox_NtClose(h);
    }
    double t1 = now();
    unsigned seed = 1;
    int n = 0;
    for (size_t i = 0; i < sizeof FILES / sizeof FILES[0]; i++) {
        HANDLE h = open_file(FILES[i]);
        XBOX_FILE_STANDARD_INFORMATION si;
        XBOX_IO_STATUS_BLOCK ios = { 0 };
        xbox_NtQueryInformationFile(h, &ios, &si, sizeof si, XboxFileStandardInformation);
        for (int k = 0; k < 4000; k++, n++) {
            seed = seed * 1103515245u + 12345u;
            LARGE_INTEGER off;
            off.QuadPart = (LONGLONG)(((uint64_t)seed << 8) % (uint64_t)(si.EndOfFile.QuadPart - 4096));
            memset(&ios, 0, sizeof ios);
            xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, 4096, &off);
        }
        xbox_NtClose(h);
    }
    double t2 = now();
    *seq_mbs = bytes / 1048576.0 / (t1 - t0);
    *rand_us = (t2 - t1) * 1e6 / n;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: kernel_disc_bench SAVE_DIR SOURCE...\n");
        return 2;
    }
    printf("| source | kind | sequential 64 KB reads | random 4 KB reads |\n|---|---|---|---|\n");
    for (int i = 2; i < argc; i++) {
        double seq = 0, rnd = 0;
        xbox_path_init(argv[i], argv[1]);
        if (pass(&seq, &rnd) != 0) {                      /* warms the page cache */
            printf("| %s | - | cannot read | |\n", argv[i]);
            continue;
        }
        double best_seq = 0, best_rnd = 1e9;
        for (int k = 0; k < 3; k++) {
            pass(&seq, &rnd);
            if (seq > best_seq)
                best_seq = seq;
            if (rnd < best_rnd)
                best_rnd = rnd;
        }
        const char *name = strrchr(argv[i], '/');
        printf("| %s | %s | %.0f MB/s | %.1f µs |\n", name ? name + 1 : argv[i],
               xbox_disc_mounted() ? xbox_disc_kind() : "folder", best_seq, best_rnd);
        fflush(stdout);
    }
    return 0;
}
