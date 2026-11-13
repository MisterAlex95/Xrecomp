/*
 * Kernel I/O beyond a plain read (XBA-114): I/O completion objects.
 *
 * Each export is driven the way a title drives it, through the thunk table,
 * with a buffer standing in for guest memory (tests/kernel_ksync does the
 * same), and checked against nxdk's xboxkrnl.h for the arguments and the
 * structures. Every call checks esp afterwards: a wrong argument count in
 * the stdcall table is a stack leak in the caller.
 *
 * Checked:
 *   1. I/O completion: NtCreateIoCompletion writes a handle; NtSetIoCompletion
 *      queues packets that NtRemoveIoCompletion hands back in order with
 *      their key, context, status and information; NtQueryIoCompletion's
 *      Depth counts them; the handle is signalled while packets are queued;
 *      a zero timeout on an empty object is STATUS_TIMEOUT; a remover on
 *      another thread blocks until a packet is posted; a file joined to the
 *      object (FileCompletionInformation) posts its reads that carry an
 *      ApcContext and no routine, with the file's key; NtClose ends it.
 *   2. Scatter / gather: NtReadFileScatter puts page i of the transfer at
 *      segment i, from ByteOffset; a transfer that runs into the end of the
 *      file is short, not failed; NtWriteFileGather writes page i from
 *      segment i (read back through the scatter); the event is set.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;

recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static uint8_t *mem;
#define M32(va) (*(volatile uint32_t *)(mem + (va)))
#define M16(va) (*(volatile uint16_t *)(mem + (va)))
#define M8(va)  (*(volatile uint8_t  *)(mem + (va)))

static int failures;
static char d[256];

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-70s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

/* ── Guest layout ── */
enum {
    THUNK_VA   = 0x10000,
    STACK_VA   = 0x20000,         /* per thread: STACK_VA + i * 0x2000 */
    TIB_VA     = 0x40000,         /* per thread: TIB_VA + i * 0x100     */
    KT_VA      = 0x50000,         /* per thread: KT_VA + i * 0x200      */
    HANDLE_VA  = 0x60000,         /* out-handles, 4 bytes each */
    IOSB_VA    = 0x60100,
    OUT_VA     = 0x60200,         /* small out-parameters */
    TIMEOUT_VA = 0x60300,         /* LARGE_INTEGER 0 */
    TIMEOUT2_VA = 0x60308,        /* LARGE_INTEGER, set per check */
    OFFSET_VA  = 0x60310,         /* LARGE_INTEGER 0 */
    INFO_VA    = 0x60400,         /* information classes */
    NAME_VA    = 0x61000,         /* path characters */
    ANSI_VA    = 0x61100,
    OA_VA      = 0x61200,
    BUF_VA     = 0x70000,
    SEG_VA     = 0x60500,         /* FILE_SEGMENT_ELEMENT arrays */
    PAGES_VA   = 0x80000,         /* pages for the segments, out of order */
};

#define FILE_BYTES 12288u
static uint8_t pattern(uint32_t i) { return i < 4 ? (uint8_t)"PORT"[i] : (uint8_t)(i * 7u + 3u); }

enum {
    S_CLOSE, S_OPEN, S_READ, S_SETINFO, S_WAIT1,
    S_IOC_CREATE, S_IOC_SET, S_IOC_REMOVE, S_IOC_QUERY,
    S_SCATTER, S_GATHER, S_EVENT,
    S_COUNT
};
static const uint32_t slot_ord[S_COUNT] = {
    187, 202, 219, 226, 233,
    191, 227, 223, 212,
    220, 237, 189,
};

static RECOMP_TLS int t_index;

static uint32_t sp_top(void) { return STACK_VA + (uint32_t)t_index * 0x2000 + 0x1000; }
static uint32_t sp_after(int nargs) { return sp_top() + 4 + 4 * (uint32_t)nargs; }

/* stdcall: a return address and the arguments on this thread's stack. */
static uint32_t call(int slot, const uint32_t *args, int n)
{
    uint32_t sp = sp_top();
    int i;

    M32(sp) = 0xBEEF0000u | (uint32_t)slot;
    for (i = 0; i < n; i++)
        M32(sp + 4 + 4 * i) = args[i];
    g_esp = sp;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(M32(THUNK_VA + 4 * slot))();
    return g_eax;
}

#define CALL(slot, ...) do { uint32_t a_[] = { __VA_ARGS__ }; \
    call(slot, a_, (int)(sizeof a_ / sizeof a_[0])); } while (0)

static int esp_ok(int nargs) { return g_esp == sp_after(nargs); }

static void become_thread(int i)
{
    uint32_t tib = TIB_VA + (uint32_t)i * 0x100, kt = KT_VA + (uint32_t)i * 0x200;
    t_index = i;
    memset(mem + tib, 0, 0x40);
    memset(mem + kt, 0, 0x200);
    M32(tib + 0x00) = 0xFFFFFFFFu;
    M32(tib + 0x04) = STACK_VA + (uint32_t)i * 0x2000 + 0x2000;
    M32(tib + 0x08) = STACK_VA + (uint32_t)i * 0x2000;
    M32(tib + 0x28) = kt;
    xbox_InitGuestKthread(kt, 0, M32(tib + 4), M32(tib + 8));
    g_fs_base = tib;
}

static uint32_t open_file(const char *path)
{
    strcpy((char *)mem + NAME_VA, path);
    M16(ANSI_VA)     = (uint16_t)strlen(path);
    M16(ANSI_VA + 2) = (uint16_t)strlen(path) + 1;
    M32(ANSI_VA + 4) = NAME_VA;
    M32(OA_VA)     = 0;
    M32(OA_VA + 4) = ANSI_VA;
    M32(OA_VA + 8) = 0x40;                     /* OBJ_CASE_INSENSITIVE */
    M32(HANDLE_VA) = 0;
    CALL(S_OPEN, HANDLE_VA, 0xC0100000u /* GENERIC_READ|WRITE|SYNCHRONIZE */,
         OA_VA, IOSB_VA, 3, 0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */);
    return g_eax == 0 ? M32(HANDLE_VA) : 0;
}

/* ═══════════════════ 1. I/O completion objects ═══════════════════ */

static uint32_t g_port;
static volatile uint32_t remover_status, remover_key;

static DWORD WINAPI port_remover(LPVOID p)
{
    (void)p;
    become_thread(3);
    CALL(S_IOC_REMOVE, g_port, OUT_VA + 0x40, OUT_VA + 0x44, OUT_VA + 0x48, 0);
    remover_status = g_eax;
    remover_key = M32(OUT_VA + 0x40);
    return 0;
}

static void test_io_completion(uint32_t file)
{
    uint32_t port, r;

    become_thread(0);
    M32(HANDLE_VA) = 0;
    CALL(S_IOC_CREATE, HANDLE_VA, 0x1F0003u, 0, 0);
    port = M32(HANDLE_VA);
    snprintf(d, sizeof d, "status 0x%08X handle 0x%08X", g_eax, port);
    check(g_eax == 0 && port != 0 && esp_ok(4), "NtCreateIoCompletion writes a handle (4 arguments)", d);
    if (!port)
        return;

    CALL(S_IOC_QUERY, port, OUT_VA);
    check(g_eax == 0 && M32(OUT_VA) == 0 && esp_ok(2),
          "NtQueryIoCompletion: Depth 0 when empty (2 arguments)", NULL);
    CALL(S_WAIT1, port, 0, TIMEOUT_VA);
    check(g_eax == 0x102u, "an empty object's handle is not signalled", NULL);

    CALL(S_IOC_SET, port, 0x11, 0x22, 0, 0x33);
    check(g_eax == 0 && esp_ok(5), "NtSetIoCompletion queues a packet (5 arguments)", NULL);
    CALL(S_IOC_SET, port, 0x44, 0x55, 0xC0000011u, 0x66);
    CALL(S_IOC_QUERY, port, OUT_VA);
    check(M32(OUT_VA) == 2, "Depth counts two packets", NULL);
    CALL(S_WAIT1, port, 0, TIMEOUT_VA);
    check(g_eax == 0, "the handle is signalled while packets are queued", NULL);

    memset(mem + OUT_VA, 0xEE, 16);
    CALL(S_IOC_REMOVE, port, OUT_VA, OUT_VA + 4, IOSB_VA, TIMEOUT_VA);
    snprintf(d, sizeof d, "status 0x%08X key 0x%X ctx 0x%X iosb 0x%X/0x%X", g_eax,
             M32(OUT_VA), M32(OUT_VA + 4), M32(IOSB_VA), M32(IOSB_VA + 4));
    check(g_eax == 0 && M32(OUT_VA) == 0x11 && M32(OUT_VA + 4) == 0x22
          && M32(IOSB_VA) == 0 && M32(IOSB_VA + 4) == 0x33 && esp_ok(5),
          "NtRemoveIoCompletion: the first packet, key/context/status/info", d);
    CALL(S_IOC_REMOVE, port, OUT_VA, OUT_VA + 4, IOSB_VA, TIMEOUT_VA);
    check(g_eax == 0 && M32(OUT_VA) == 0x44 && M32(IOSB_VA) == 0xC0000011u
          && M32(IOSB_VA + 4) == 0x66, "then the second, an error status as posted", NULL);
    CALL(S_IOC_REMOVE, port, OUT_VA, OUT_VA + 4, IOSB_VA, TIMEOUT_VA);
    check(g_eax == 0x102u, "empty, zero timeout: STATUS_TIMEOUT", NULL);
    CALL(S_WAIT1, port, 0, TIMEOUT_VA);
    check(g_eax == 0x102u, "and the handle is no longer signalled", NULL);
    *(int64_t *)(mem + TIMEOUT2_VA) = -300000;   /* 30 ms */
    CALL(S_IOC_REMOVE, port, OUT_VA, OUT_VA + 4, IOSB_VA, TIMEOUT2_VA);
    check(g_eax == 0x102u, "a 30 ms timeout runs out", NULL);

    {   /* A remover on another thread, woken by a post. */
        HANDLE th;
        g_port = port;
        remover_status = 0xFFFFFFFFu;
        th = CreateThread(NULL, 0, port_remover, NULL, 0, NULL);
        Sleep(50);
        check(remover_status == 0xFFFFFFFFu, "a remover on another thread blocks while empty", NULL);
        CALL(S_IOC_SET, port, 0x77, 0, 0, 0);
        r = WaitForSingleObject(th, 5000);
        CloseHandle(th);
        snprintf(d, sizeof d, "wait %u status 0x%X key 0x%X", r, remover_status, remover_key);
        check(r == 0 && remover_status == 0 && remover_key == 0x77,
              "a post wakes it with the packet", d);
    }

    /* A file joined to the object. */
    if (file) {
        M32(INFO_VA) = port;
        M32(INFO_VA + 4) = 0xABCD;
        CALL(S_SETINFO, file, IOSB_VA, INFO_VA, 8, 30 /* FileCompletionInformation */);
        check(g_eax == 0 && esp_ok(5), "FileCompletionInformation joins the file to the object", NULL);
        CALL(S_READ, file, 0, 0, 0x5150 /* ApcContext */, IOSB_VA, BUF_VA, 4, OFFSET_VA);
        check(g_eax == 0 && memcmp(mem + BUF_VA, "PORT", 4) == 0, "a read of the file", NULL);
        CALL(S_IOC_REMOVE, port, OUT_VA, OUT_VA + 4, IOSB_VA, TIMEOUT_VA);
        snprintf(d, sizeof d, "status 0x%08X key 0x%X ctx 0x%X info %u", g_eax,
                 M32(OUT_VA), M32(OUT_VA + 4), M32(IOSB_VA + 4));
        check(g_eax == 0 && M32(OUT_VA) == 0xABCD && M32(OUT_VA + 4) == 0x5150
              && M32(IOSB_VA) == 0 && M32(IOSB_VA + 4) == 4,
              "posts its completion: the file's key, the read's context and count", d);
        CALL(S_READ, file, 0, 0, 0, IOSB_VA, BUF_VA, 4, OFFSET_VA);
        CALL(S_IOC_QUERY, port, OUT_VA);
        check(M32(OUT_VA) == 0, "a read with no ApcContext posts nothing", NULL);
        *(int64_t *)(mem + TIMEOUT2_VA) = 0x100000;   /* past the end */
        CALL(S_READ, file, 0, 0, 0x5151, IOSB_VA, BUF_VA, 4, TIMEOUT2_VA);
        CALL(S_IOC_QUERY, port, OUT_VA);
        check(g_eax == 0 && M32(OUT_VA) == 0,
              "a read that fails synchronously posts nothing", NULL);
    }

    CALL(S_CLOSE, port);
    CALL(S_IOC_SET, port, 1, 2, 3, 4);
    check(g_eax == 0xC0000008u, "after NtClose the handle is no object: STATUS_INVALID_HANDLE", NULL);
}

/* ═══════════════════ 2. Scatter / gather ═══════════════════ */

static void test_scatter_gather(uint32_t file)
{
    static const uint32_t page_of[3] = { 2, 0, 5 };   /* segment i -> page */
    uint32_t i, ev, bad = 0;

    become_thread(0);
    CALL(S_EVENT, HANDLE_VA, 0, 0 /* notification */, 0);
    ev = M32(HANDLE_VA);
    for (i = 0; i < 3; i++)
        M32(SEG_VA + 4 * i) = PAGES_VA + page_of[i] * 0x1000;
    memset(mem + PAGES_VA, 0, 6 * 0x1000);
    M32(OFFSET_VA) = 0; M32(OFFSET_VA + 4) = 0;
    CALL(S_SCATTER, file, ev, 0, 0, IOSB_VA, SEG_VA, 10000, OFFSET_VA);
    for (i = 0; i < 10000; i++)
        if (M8(PAGES_VA + page_of[i / 0x1000] * 0x1000 + i % 0x1000) != pattern(i))
            bad++;
    snprintf(d, sizeof d, "status 0x%08X info %u bad %u", g_eax, M32(IOSB_VA + 4), bad);
    check(g_eax == 0 && M32(IOSB_VA) == 0 && M32(IOSB_VA + 4) == 10000 && !bad && esp_ok(8),
          "NtReadFileScatter: page i at segment i, 10000 bytes (8 arguments)", d);
    check(M8(PAGES_VA + 5 * 0x1000 + (10000 - 8192)) == 0,
          "nothing past Length in the last segment", NULL);
    CALL(S_WAIT1, ev, 0, TIMEOUT_VA);
    check(g_eax == 0, "the event is set", NULL);

    M32(TIMEOUT2_VA) = 8192; M32(TIMEOUT2_VA + 4) = 0;
    CALL(S_SCATTER, file, 0, 0, 0, IOSB_VA, SEG_VA, 8192, TIMEOUT2_VA);
    snprintf(d, sizeof d, "status 0x%08X info %u", g_eax, M32(IOSB_VA + 4));
    check(g_eax == 0 && M32(IOSB_VA + 4) == FILE_BYTES - 8192,
          "into the end of the file: short (4096), not failed", d);

    for (i = 0; i < 0x1000; i++) {
        M8(PAGES_VA + 3 * 0x1000 + i) = (uint8_t)(0xA0 ^ i);
        M8(PAGES_VA + 1 * 0x1000 + i) = (uint8_t)(0x5C ^ i);
    }
    M32(SEG_VA + 0x20) = PAGES_VA + 3 * 0x1000;
    M32(SEG_VA + 0x24) = PAGES_VA + 1 * 0x1000;
    M32(TIMEOUT2_VA) = 4096;
    CALL(S_GATHER, file, 0, 0, 0, IOSB_VA, SEG_VA + 0x20, 8192, TIMEOUT2_VA);
    snprintf(d, sizeof d, "status 0x%08X info %u", g_eax, M32(IOSB_VA + 4));
    check(g_eax == 0 && M32(IOSB_VA + 4) == 8192 && esp_ok(8),
          "NtWriteFileGather writes two pages (8 arguments)", d);
    memset(mem + PAGES_VA, 0, 6 * 0x1000);
    CALL(S_SCATTER, file, 0, 0, 0, IOSB_VA, SEG_VA, FILE_BYTES, OFFSET_VA);
    for (bad = 0, i = 0; i < FILE_BYTES; i++) {
        uint8_t want = i < 4096 ? pattern(i) : i < 8192 ? (uint8_t)(0xA0 ^ (i - 4096))
                                                       : (uint8_t)(0x5C ^ (i - 8192));
        if (M8(PAGES_VA + page_of[i / 0x1000] * 0x1000 + i % 0x1000) != want)
            bad++;
    }
    snprintf(d, sizeof d, "bad %u", bad);
    check(g_eax == 0 && !bad, "read back: page i of the gather came from segment i", d);
    CALL(S_CLOSE, ev);
}

int main(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    uint32_t file;
    FILE *f;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        printf("guest memory stand-in not allocated\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;

    /* A host directory standing in for the disc, holding one file. */
    GetTempPathA(sizeof dir, dir);
    snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "xr_io_ports_%u",
             (unsigned)GetCurrentProcessId());
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s/port.bin", dir);
    f = fopen(path, "wb");
    if (!f) {
        printf("cannot create %s\n", path);
        return 1;
    }
    for (i = 0; i < (int)FILE_BYTES; i++)
        fputc(pattern((uint32_t)i), f);
    fclose(f);

    for (i = 0; i < S_COUNT; i++)
        M32(THUNK_VA + 4 * i) = 0x80000000u | slot_ord[i];
    xbox_kernel_set_thunk_address(THUNK_VA, S_COUNT);
    xbox_kernel_init();
    xbox_path_init(dir, dir);
    xbox_kernel_bridge_init();

    become_thread(0);
    file = open_file("\\Device\\CdRom0\\port.bin");
    snprintf(d, sizeof d, "status 0x%08X", g_eax);
    check(file != 0, "NtOpenFile opens the stand-in disc file", d);

    test_io_completion(file);
    if (file)
        test_scatter_gather(file);

    if (file)
        CALL(S_CLOSE, file);
    DeleteFileA(path);
    RemoveDirectoryA(dir);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
