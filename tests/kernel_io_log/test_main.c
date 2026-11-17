/*
 * The NtReadFile and NtCreateFile bridges do not print a line per call
 * (KRN-5).
 *
 * Both printed and flushed a line on every guest read and every successful
 * NtCreateFile. On Android stderr is a pipe to logcat, so a title streaming
 * in small reads paid a syscall per read on the thread whose stall is audio
 * dry time. Now the first 64 of each print, then only failures and short
 * reads; RECOMP_READ_LOG=1 prints every read (the second CTest entry).
 *
 * Driven through the bridge seam (recomp_lookup_kernel), as
 * tests/kernel_bridge does, with stderr sent to a file and counted.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;

#define THUNK_VA  0x10000u
#define STACK_VA  0x20000u
#define OA_VA     0x40000u
#define NAME_VA   0x40100u
#define PATH_VA   0x40200u
#define HANDLE_VA 0x40500u
#define IOSB_VA   0x40600u
#define OFF_VA    0x40700u
#define BUF_VA    0x50000u

static uint8_t *mem;
static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void call(int slot)
{
    recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 4u * (uint32_t)slot))();
}

static uint32_t guest_create(void)
{
    uint32_t *sp = (uint32_t *)(mem + STACK_VA);
    memset(sp, 0, 64);
    *(uint32_t *)(mem + OA_VA) = 0;
    *(uint32_t *)(mem + OA_VA + 4) = NAME_VA;
    *(uint32_t *)(mem + OA_VA + 8) = 0x40;
    strcpy((char *)mem + PATH_VA, "D:\\data.bin");
    *(uint16_t *)(mem + NAME_VA) = 11;
    *(uint16_t *)(mem + NAME_VA + 2) = 12;
    *(uint32_t *)(mem + NAME_VA + 4) = PATH_VA;
    sp[0] = 0xBEEF0001u; sp[1] = HANDLE_VA; sp[2] = 0x80100080u; sp[3] = OA_VA;
    sp[4] = IOSB_VA; sp[5] = 0; sp[6] = 0x80; sp[7] = 1; sp[8] = XBOX_FILE_OPEN; sp[9] = 0x60;
    g_esp = STACK_VA;
    call(0);
    return g_eax == 0 ? *(uint32_t *)(mem + HANDLE_VA) : 0;
}

static void guest_close(uint32_t h)
{
    uint32_t *sp = (uint32_t *)(mem + STACK_VA);
    sp[0] = 0xBEEF0001u; sp[1] = h;
    g_esp = STACK_VA;
    call(2);
}

static uint32_t guest_read(uint32_t h, uint32_t off, uint32_t len)
{
    uint32_t *sp = (uint32_t *)(mem + STACK_VA);
    memset(sp, 0, 64);
    *(uint32_t *)(mem + OFF_VA) = off;
    *(uint32_t *)(mem + OFF_VA + 4) = 0;
    sp[0] = 0xBEEF0001u; sp[1] = h; sp[5] = IOSB_VA; sp[6] = BUF_VA; sp[7] = len; sp[8] = OFF_VA;
    g_esp = STACK_VA;
    call(1);
    return g_eax;
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (; (hay = strstr(hay, needle)) != NULL; hay += strlen(needle))
        n++;
    return n;
}

int main(void)
{
    const char *all_env = getenv("RECOMP_READ_LOG");
    const int all = all_env && all_env[0] == '1';
    char root[512], file[600], log[700];
    static char buf[1 << 22];
    uint32_t h;
    size_t got;
    FILE *f;
    int i, reads_ok = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(root, sizeof root, "%siolog-%lu-%d", tmp, (unsigned long)GetCurrentProcessId(), all);
        CreateDirectoryA(root, NULL);
        snprintf(file, sizeof file, "%s\\data.bin", root);
    }
#else
    {
        char tmpl[] = "/tmp/iologXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
        snprintf(file, sizeof file, "%s/data.bin", root);
    }
#endif
    f = fopen(file, "wb");
    if (!f) return 2;
    for (i = 0; i < 4096; i++) fputc(i & 0xFF, f);
    fclose(f);

    mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) return 2;
    g_xbox_mem_offset = (ptrdiff_t)mem;
    *(uint32_t *)(mem + THUNK_VA)     = 0x80000000u | 190u;   /* NtCreateFile */
    *(uint32_t *)(mem + THUNK_VA + 4) = 0x80000000u | 219u;   /* NtReadFile */
    *(uint32_t *)(mem + THUNK_VA + 8) = 0x80000000u | 187u;   /* NtClose */
    xbox_path_init(root, root);
    xbox_kernel_set_thunk_address(THUNK_VA, 3);
    xbox_kernel_bridge_init();

    snprintf(log, sizeof log, "%s%sstderr.txt", root,
#if defined(_WIN32)
             "\\"
#else
             "/"
#endif
             );
    if (!freopen(log, "w", stderr)) return 2;

    for (i = 0; i < 100; i++) {
        h = guest_create();
        if (!h) { reads_ok = 0; break; }
        guest_close(h);
    }
    h = guest_create();
    check(h != 0 && reads_ok, "101 guest NtCreateFile calls succeed");
    for (i = 0; i < 2000 && h; i++)
        if (guest_read(h, (uint32_t)(i % 256) * 16u, 16) != 0
                || *(uint32_t *)(mem + IOSB_VA + 4) != 16)
            reads_ok = 0;
    check(reads_ok, "2000 guest reads of 16 bytes succeed");
    guest_read(h, 4090, 16);                         /* short: 6 bytes */
    guest_close(h);

    fflush(stderr);
    f = fopen(log, "r");
    got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[got] = 0;
    if (f) fclose(f);
    {
        int reads = count_of(buf, "[READ] from=");
        int shorts = count_of(buf, "want=16 got=6");
        int opens = count_of(buf, "D:\\data.bin (disp 1 opts");
        char what[128];
        printf("  (%d read lines, %d open lines; RECOMP_READ_LOG %s)\n", reads, opens,
               all ? "=1" : "unset");
        if (all) {
            check(reads == 2001, "RECOMP_READ_LOG=1: every read prints");
        } else {
            snprintf(what, sizeof what, "default: the first 64 reads print, then the short one (%d)", reads);
            check(reads == 65, what);
            snprintf(what, sizeof what, "default: at most 64 NtCreateFile success lines (%d)", opens);
            check(opens == 64, what);
        }
        check(shorts == 1, "the short read is always printed");
    }

    {   /* the save-state directory the bridge made there too */
        char cmd[700];
#if defined(_WIN32)
        snprintf(cmd, sizeof cmd, "cmd /c rmdir /s /q \"%s\"", root);
#else
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
#endif
        fclose(stderr);
        (void)system(cmd);
    }
    VirtualFree(mem, 0, MEM_RELEASE);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
