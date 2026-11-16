/*
 * Sega Chihiro mode (XBA-234): RECOMP_CHIHIRO=1 boots our own synthetic XBE on a
 * 128 MB map with the arcade kernel's XboxHardwareInfo, and without it nothing
 * changes from the Xbox.
 *
 *   chihiro_mode_test default|chihiro XBE
 *
 * "chihiro" expects RECOMP_CHIHIRO=1 in the environment (CMakeLists sets it),
 * "default" expects it unset. Checks, each against the other mode's value:
 *
 *   RAM            g_xbox_total_ram and the mapped size: 128 MB / 64 MB
 *   no 64 MB wrap  guest 0x06000000 and 0x02000000 are distinct pages on
 *                  Chihiro; the same address on the Xbox (mirror 1)
 *   mirror stride  guest 0x08070000 aliases 0x00070000 on Chihiro (the wrap is
 *                  at 128 MB), 0x04070000 on the Xbox
 *   heap           a 72 MB allocation fits on Chihiro
 *   kernel         MmQueryStatistics.TotalPhysicalPages 32768 / 16384;
 *                  XboxHardwareInfo.Flags has XBOX_HW_FLAG_ARCADE / is 0
 *   XBE key        a header whose kernel thunk address is encoded with the
 *                  Chihiro key is decoded in Chihiro mode, not on the Xbox
 *
 * The image is tools/conformance/test.xbe, patched in memory: no game code.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "xbox_chihiro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

extern ptrdiff_t g_xbox_mem_offset;

#define MB (1024u * 1024u)
#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define G8(va)  (*(volatile uint8_t  *)((uintptr_t)(va) + g_xbox_mem_offset))

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-58s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

/* Reads one guest byte in a child: an unmapped mirror kills the child, not the
 * test (tests/memory_layout_posix explains why). 1 and *out on success. */
static int peek_in_child(uint32_t va, uint8_t *out)
{
    int fd[2];
    if (pipe(fd) != 0) return 0;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return 0; }
    if (pid == 0) {
        uint8_t v;
        close(fd[0]);
        v = G8(va);
        _exit(write(fd[1], &v, 1) == 1 ? 0 : 1);
    }
    close(fd[1]);
    uint8_t v = 0;
    ssize_t got = read(fd[0], &v, 1);
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (got == 1 && WIFEXITED(st) && WEXITSTATUS(st) == 0) { *out = v; return 1; }
    return 0;
}

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    long len;
    unsigned char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    b = len > 0x1000 ? malloc((size_t)len) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)len;
    return b;
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/chihiroXXXXXX", cmd[300];
    const char *mode = argc > 1 ? argv[1] : "default";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    int chihiro = !strcmp(mode, "chihiro");
    unsigned char *xbe;
    size_t n = 0;
    char detail[128];

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!chihiro && strcmp(mode, "default")) {
        printf("unknown mode '%s'\n", mode);
        return 2;
    }
    if (!mkdtemp(tmpl)) return 2;
    setenv("XDG_DATA_HOME", tmpl, 1);
    xbe = load(path, &n);
    if (!xbe) {
        printf("cannot read %s -- run: python3 tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    /* The kernel thunk address, Chihiro-keyed: an empty table in the headers
     * (the synthetic image's 0x800 is zero), VA 0x00010800. */
    {
        uint32_t raw = (0x00010800u) ^ XBE_THUNK_CHIHIRO_XOR;
        memcpy(xbe + 0x158, &raw, 4);
    }
    printf("%s mode, %s\n", mode, path);

    check(xbox_ChihiroMode() == chihiro, "RECOMP_CHIHIRO matches the mode",
          chihiro ? "RECOMP_CHIHIRO is not set" : "RECOMP_CHIHIRO is set");
    if (!xbox_MemoryLayoutInit(xbe, n)) {
        printf("xbox_MemoryLayoutInit failed\n");
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();

    /* RAM and the map. */
    size_t want = chihiro ? 128u * MB : 64u * MB;
    snprintf(detail, sizeof detail, "%zu MB", g_xbox_total_ram / MB);
    check(g_xbox_total_ram == want, chihiro ? "RAM is 128 MB" : "RAM is 64 MB", detail);
    snprintf(detail, sizeof detail, "%zu MB", xbox_GetMappedSize() / MB);
    check(xbox_GetMappedSize() == want, chihiro ? "mapped size is 128 MB" : "mapped size is 64 MB", detail);

    /* Above 64 MB: its own page on Chihiro, mirror 1 of low memory on the Xbox. */
    G8(0x02000000u) = 0x22;
    G8(0x06000000u) = 0x66;
    if (chihiro)
        check(G8(0x02000000u) == 0x22 && G8(0x06000000u) == 0x66,
              "0x06000000 is RAM of its own, not 0x02000000", "aliased at 64 MB");
    else
        check(G8(0x02000000u) == 0x66, "0x06000000 wraps onto 0x02000000 (26-bit bus)", NULL);

    /* The wrap: at the mapped size. */
    {
        uint8_t got = 0;
        uint32_t m = (uint32_t)want + 0x70000u;
        G8(0x00070000u) = 0x5A;
        snprintf(detail, sizeof detail, "0x%08X %s", m, peek_in_child(m, &got) ? "differs" : "unmapped");
        check(peek_in_child(m, &got) && got == 0x5A,
              chihiro ? "0x08070000 aliases 0x00070000 (wrap at 128 MB)"
                      : "0x04070000 aliases 0x00070000 (wrap at 64 MB)", detail);
    }

    if (chihiro) {
        uint32_t big = xbox_HeapAlloc(72u * MB, 4096);
        snprintf(detail, sizeof detail, "got 0x%08X", big);
        check(big != 0 && big + 72u * MB <= 128u * MB, "a 72 MB heap allocation fits in RAM", detail);
    }

    /* The kernel: what a title reads about the machine. */
    xbox_kernel_init();
    {
        uint32_t tva = 0, tcount = 0;
        xbox_kernel_get_thunk_address(&tva, &tcount);
        snprintf(detail, sizeof detail, "thunk table at 0x%08X", tva);
        if (chihiro)
            check(tva == 0x00010800u, "a Chihiro-keyed thunk address is decoded", detail);
        else
            check(tva != 0x00010800u, "the Chihiro key is not tried on the Xbox", detail);
        xbox_kernel_bridge_init();
    }
    {
        XBOX_MM_STATISTICS st;
        memset(&st, 0, sizeof st);
        st.Length = sizeof st;
        NTSTATUS s = xbox_MmQueryStatistics(&st);
        snprintf(detail, sizeof detail, "%u pages", (unsigned)st.TotalPhysicalPages);
        check(s == STATUS_SUCCESS && st.TotalPhysicalPages == (chihiro ? 32768u : 16384u),
              chihiro ? "MmQueryStatistics: 32768 pages" : "MmQueryStatistics: 16384 pages", detail);
    }
    {
        uint32_t flags = G32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO);
        snprintf(detail, sizeof detail, "Flags 0x%08X", flags);
        check(flags == (chihiro ? XBOX_HW_FLAG_ARCADE : 0u),
              chihiro ? "XboxHardwareInfo.Flags is ARCADE (0x8)" : "XboxHardwareInfo.Flags is 0 (retail)", detail);
    }

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    (void)system(cmd);
    printf("\n%d failure(s)\n", failures);
    fflush(stdout);
    _exit(failures ? 1 : 0);   /* no teardown: the bridge's threads stay up */
}
