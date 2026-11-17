/*
 * Positioned NtReadFile / NtWriteFile from two threads on one handle (KRN-1).
 *
 * With a ByteOffset the POSIX backend did lseek() then read(): two calls on
 * the file position every user of the handle shares. Two threads reading one
 * pack file at explicit offsets got each other's bytes, with a success
 * status. The file here holds, at every 4-byte-aligned offset, that offset;
 * two threads read and check random blocks through one handle, then write
 * their own blocks at explicit offsets and the file is checked afterwards.
 *
 * POSIX only: the Win32 backend passes an OVERLAPPED offset (skipped, 77).
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#if defined(_WIN32)
int main(void) { return 77; }
#else
#include <pthread.h>
#include <unistd.h>

#define FILE_WORDS (1u << 20)          /* 4 MB */
#define BLOCK      4096u
#define ITERS      20000

static HANDLE g_file;
static volatile int g_bad_reads, g_bad_status;

static NTSTATUS open_file(const char *path, ULONG disposition, HANDLE *out)
{
    XBOX_ANSI_STRING s;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    s.Length = (USHORT)strlen(path);
    s.MaximumLength = (USHORT)(s.Length + 1);
    s.Buffer = (PCHAR)path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &s;
    oa.Attributes = 0x40;
    *out = NULL;
    return xbox_NtCreateFile(out, 0xC0100080u, &oa, &ios, NULL, 0, 0, disposition, 0x20);
}

static void *reader(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg;
    uint32_t buf[BLOCK / 4];
    int i;
    for (i = 0; i < ITERS; i++) {
        uint32_t word = (uint32_t)(rand_r(&seed) % (FILE_WORDS - BLOCK / 4));
        LARGE_INTEGER off;
        XBOX_IO_STATUS_BLOCK ios;
        NTSTATUS st;
        unsigned k;
        off.QuadPart = (LONGLONG)word * 4;
        st = xbox_NtReadFile(g_file, NULL, NULL, NULL, &ios, buf, BLOCK, &off);
        if (st != 0 || ios.Information != BLOCK) { g_bad_status++; continue; }
        for (k = 0; k < BLOCK / 4; k++)
            if (buf[k] != (word + k) * 4) { g_bad_reads++; break; }
    }
    return NULL;
}

/* Thread t writes the blocks whose index is t mod 2, each filled with
 * the complement of its offsets, in random order. */
static void *writer(void *arg)
{
    unsigned t = (unsigned)(uintptr_t)arg, seed = t + 7;
    uint32_t buf[BLOCK / 4];
    unsigned nblocks = FILE_WORDS * 4 / BLOCK, i;
    for (i = 0; i < ITERS; i++) {
        unsigned b = (unsigned)(rand_r(&seed) % (nblocks / 2)) * 2 + t, k;
        LARGE_INTEGER off;
        XBOX_IO_STATUS_BLOCK ios;
        off.QuadPart = (LONGLONG)b * BLOCK;
        for (k = 0; k < BLOCK / 4; k++)
            buf[k] = ~(uint32_t)(b * BLOCK + k * 4);
        if (xbox_NtWriteFile(g_file, NULL, NULL, NULL, &ios, buf, BLOCK, &off) != 0
                || ios.Information != BLOCK)
            g_bad_status++;
    }
    return NULL;
}

int main(void)
{
    char tmpl[] = "/tmp/filepreadXXXXXX", host[600];
    uint32_t *data;
    FILE *f;
    pthread_t th[2];
    unsigned i;
    int failures = 0, bad = 0;
    NTSTATUS st;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!mkdtemp(tmpl)) return 2;
    snprintf(host, sizeof host, "%s/pack.bin", tmpl);
    data = malloc(FILE_WORDS * 4);
    if (!data) return 2;
    for (i = 0; i < FILE_WORDS; i++) data[i] = i * 4;
    f = fopen(host, "wb");
    if (!f || fwrite(data, 4, FILE_WORDS, f) != FILE_WORDS) return 2;
    fclose(f);
    xbox_path_init(tmpl, tmpl);

    st = open_file("D:\\pack.bin", XBOX_FILE_OPEN, &g_file);
    printf("  open D:\\pack.bin                                   %s\n", st == 0 ? "PASS" : "FAIL");
    if (st != 0) return 1;

    pthread_create(&th[0], NULL, reader, (void *)1);
    pthread_create(&th[1], NULL, reader, (void *)2);
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    printf("  two readers, %d positioned reads each: %d wrong, %d failed  %s\n",
           ITERS, g_bad_reads, g_bad_status, g_bad_reads || g_bad_status ? "FAIL" : "PASS");
    failures += g_bad_reads || g_bad_status;

    {   /* a read without an offset continues after the last positioned one */
        LARGE_INTEGER off;
        XBOX_IO_STATUS_BLOCK ios;
        uint32_t w[2] = {0, 0};
        off.QuadPart = 400;
        xbox_NtReadFile(g_file, NULL, NULL, NULL, &ios, w, 4, &off);
        xbox_NtReadFile(g_file, NULL, NULL, NULL, &ios, w + 1, 4, NULL);
        printf("  a read without an offset follows a positioned read  %s\n",
               w[0] == 400 && w[1] == 404 ? "PASS" : "FAIL");
        failures += !(w[0] == 400 && w[1] == 404);
    }

    g_bad_status = 0;
    pthread_create(&th[0], NULL, writer, (void *)0);
    pthread_create(&th[1], NULL, writer, (void *)1);
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    xbox_NtClose(g_file);

    f = fopen(host, "rb");
    if (!f || fread(data, 4, FILE_WORDS, f) != FILE_WORDS) return 2;
    fclose(f);
    for (i = 0; i < FILE_WORDS; i++)
        if (data[i] != i * 4 && data[i] != ~(i * 4)) bad++;
    printf("  two writers, %d positioned writes each: %d words misplaced, %d failed  %s\n",
           ITERS, bad, g_bad_status, bad || g_bad_status ? "FAIL" : "PASS");
    failures += bad || g_bad_status;

    unlink(host);
    rmdir(tmpl);
    free(data);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
#endif
