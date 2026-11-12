/*
 * File I/O APCs: where they run, and what they leave on the guest stack.
 *
 * ReadFileEx hands NtReadFile an APC routine. On hardware it runs in the
 * issuing thread's next alertable user-mode wait, which then returns
 * STATUS_USER_APC; a non-alertable or kernel-mode wait leaves it queued. That
 * is the runtime's default since XBA-102. RECOMP_INLINE_APC=1 (the second
 * ctest entry) runs it inline inside NtReadFile, the old default.
 *
 * Everything is driven the way a title drives it: through the thunk table,
 * with a buffer standing in for guest memory, as tests/kernel_bridge does.
 * The APC routine is a C function this test hands back from recomp_lookup for
 * a made-up guest VA.
 *
 * Checked, deferred (the default, no call to xbox_SetDeferredIoApc):
 *   - the APC has not run when NtReadFile returns; esp is exact; the status
 *     block is already written and the event already set, so a title that
 *     polls them or waits on the event without being alertable completes;
 *   - a non-alertable wait and an alertable kernel-mode wait leave it queued;
 *   - alertable user-mode waits run it and return STATUS_USER_APC with esp
 *     exact: KeDelayExecutionThread (SleepEx), NtWaitForSingleObjectEx,
 *     NtWaitForMultipleObjectsEx, KeWaitForSingleObject,
 *     KeWaitForMultipleObjects;
 *   - several queued before the wait all run in it, in request order;
 *   - one queued during the wait (by a completion routine issuing the next
 *     read) runs at the next wait, not in this one;
 *   - a thread that exits with an APC queued takes it with it: another
 *     thread's alertable wait does not run it;
 *   - past the queue (64) a request's APC runs inline rather than being lost;
 *   - xbox_SetDeferredIoApc(0) still selects the inline delivery.
 * Inline (RECOMP_INLINE_APC=1):
 *   - the routine runs during NtReadFile with its context, and esp comes back
 *     exactly past NtReadFile's arguments (deliver_one_apc used to add 12
 *     after a stdcall routine had popped its own 16 bytes);
 *   - a later alertable wait finds nothing queued.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;

#define APC_VA       0x00005000u   /* "guest" address of the APC routine */
#define APC_CONTEXT  0x0000C0DEu
#define CHAIN_CONTEXT 0x0000C4A1u  /* this APC issues one more read */

static uint8_t *mem;
static int apc_runs;
static uint32_t apc_seen[256];     /* contexts, in delivery order */
static uint32_t file_handle;

/* Guest layout. */
enum {
    THUNK_VA = 0x10000,
    STACK_VA = 0x20000,   /* main thread's stack top */
    STACK2_VA = 0x28000,  /* the second thread's */
    NAME_VA  = 0x30000,   /* path characters */
    ANSI_VA  = 0x30100,   /* ANSI_STRING */
    OA_VA    = 0x30200,   /* OBJECT_ATTRIBUTES */
    IOSB_VA  = 0x30300,
    HANDLE_VA = 0x30400,
    EVENT_VA = 0x30408,
    OBJS_VA  = 0x30410,   /* handle array for the multiple-object waits */
    OFFSET_VA = 0x30420,  /* LARGE_INTEGER 0 */
    BUF_VA   = 0x31000,
    TIMEOUT_VA = 0x32000, /* LARGE_INTEGER 0: a regression returns, not hangs */
};

enum { S_OPEN, S_READ, S_WAIT1EX, S_DELAY, S_WAITNEX, S_KEWAIT1, S_KEWAITN,
       S_EVENT, S_COUNT };
static const uint32_t ordinals[S_COUNT] = {
    202, /* NtOpenFile */
    219, /* NtReadFile */
    234, /* NtWaitForSingleObjectEx */
    99,  /* KeDelayExecutionThread */
    235, /* NtWaitForMultipleObjectsEx */
    159, /* KeWaitForSingleObject */
    158, /* KeWaitForMultipleObjects */
    189, /* NtCreateEvent */
};

/* stdcall from the current guest esp: push the arguments and a return
 * address, call the thunk, which pops them all. */
static void call(int slot, const uint32_t *args, int n)
{
    int i;

    g_esp -= 4u * (uint32_t)n;
    for (i = 0; i < n; i++)
        *(uint32_t *)(mem + g_esp + 4u * (uint32_t)i) = args[i];
    g_esp -= 4;
    *(uint32_t *)(mem + g_esp) = 0xBEEF0000u | (uint32_t)slot;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 4 * slot))();
}

static void read_with_apc(uint32_t context, uint32_t event)
{
    uint32_t args[8] = { file_handle, event, APC_VA, context, IOSB_VA,
                         BUF_VA, 4, OFFSET_VA };
    call(S_READ, args, 8);
}

/* VOID NTAPI ApcRoutine(PVOID ApcContext, PIO_STATUS_BLOCK, ULONG):
 * stdcall, so it pops its return address and three arguments. */
static void test_apc(void)
{
    uint32_t ctx = *(uint32_t *)(mem + g_esp + 4);

    if (apc_runs < (int)(sizeof apc_seen / sizeof apc_seen[0]))
        apc_seen[apc_runs] = ctx;
    apc_runs++;
    if (ctx == CHAIN_CONTEXT)       /* the next read, as a streamer does */
        read_with_apc(APC_CONTEXT, 0);
    g_esp += 16;
}

recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    return xbox_va == APC_VA ? test_apc : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-66s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static char detail[200];

static void check_esp(uint32_t want, const char *what)
{
    snprintf(detail, sizeof detail, "esp 0x%08X, expected 0x%08X", g_esp, want);
    check(g_esp == want, what, detail);
}

static void check_wait(uint32_t want_status, int want_runs, const char *what)
{
    snprintf(detail, sizeof detail, "status 0x%08X (want 0x%08X), runs %d (want %d)",
             g_eax, want_status, apc_runs, want_runs);
    check(g_eax == want_status && apc_runs == want_runs, what, detail);
}

static uint32_t open_file(void)
{
    const char *path = "\\Device\\CdRom0\\apc.bin";
    uint32_t args[6] = { HANDLE_VA, 0x80100000u /* GENERIC_READ|SYNCHRONIZE */,
                         OA_VA, IOSB_VA, 1 /* FILE_SHARE_READ */,
                         0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */ };

    strcpy((char *)mem + NAME_VA, path);
    *(uint16_t *)(mem + ANSI_VA)     = (uint16_t)strlen(path);
    *(uint16_t *)(mem + ANSI_VA + 2) = (uint16_t)strlen(path) + 1;
    *(uint32_t *)(mem + ANSI_VA + 4) = NAME_VA;
    *(uint32_t *)(mem + OA_VA)     = 0;         /* RootDirectory */
    *(uint32_t *)(mem + OA_VA + 4) = ANSI_VA;   /* ObjectName */
    *(uint32_t *)(mem + OA_VA + 8) = 0x40;      /* OBJ_CASE_INSENSITIVE */
    g_esp = STACK_VA;
    call(S_OPEN, args, 6);
    return g_eax == 0 ? *(uint32_t *)(mem + HANDLE_VA) : 0;
}

static uint32_t create_event(void)
{
    /* NotificationEvent, not signalled: a completion leaves it signalled. */
    uint32_t args[4] = { EVENT_VA, 0, 0, 0 };
    g_esp = STACK_VA;
    call(S_EVENT, args, 4);
    return g_eax == 0 ? *(uint32_t *)(mem + EVENT_VA) : 0;
}

/* The waits, zero timeout. */
static void wait1ex(uint32_t h, uint32_t mode, uint32_t alertable)
{
    uint32_t args[4] = { h, mode, alertable, TIMEOUT_VA };
    call(S_WAIT1EX, args, 4);
}

static void delay_alertable(void)
{
    uint32_t args[3] = { 1 /* UserMode */, 1, TIMEOUT_VA };
    call(S_DELAY, args, 3);
}

static void waitnex(uint32_t h)
{
    uint32_t args[6] = { 1, OBJS_VA, 1 /* WaitAny */, 1 /* UserMode */, 1,
                         TIMEOUT_VA };
    *(uint32_t *)(mem + OBJS_VA) = h;
    call(S_WAITNEX, args, 6);
}

static void kewait1(uint32_t h)
{
    uint32_t args[5] = { h, 0 /* Executive */, 1 /* UserMode */, 1, TIMEOUT_VA };
    call(S_KEWAIT1, args, 5);
}

static void kewaitn(uint32_t h)
{
    uint32_t args[8] = { 1, OBJS_VA, 1 /* WaitAny */, 0, 1 /* UserMode */, 1,
                         TIMEOUT_VA, 0 };
    *(uint32_t *)(mem + OBJS_VA) = h;
    call(S_KEWAITN, args, 8);
}

/* A second guest thread: reads with an APC and exits without waiting. */
static DWORD WINAPI reader_thread(LPVOID param)
{
    (void)param;
    g_esp = STACK2_VA;
    read_with_apc(0x7777u, 0);
    return 0;
}

static void run_deferred(uint32_t ev)
{
    int i, ok;

    /* Before any wait. */
    apc_runs = 0;
    memset(mem + IOSB_VA, 0xEE, 8);
    memset(mem + BUF_VA, 0, 4);
    g_esp = STACK_VA;
    read_with_apc(APC_CONTEXT, ev);
    check(apc_runs == 0, "deferred by default: the APC has not run after NtReadFile",
          NULL);
    check_esp(STACK_VA, "deferred: esp is exactly past NtReadFile's arguments");
    check(memcmp(mem + BUF_VA, "APC!", 4) == 0
          && *(uint32_t *)(mem + IOSB_VA) == 0 && *(uint32_t *)(mem + IOSB_VA + 4) == 4,
          "deferred: the data and the status block are there at once (polling)",
          NULL);

    wait1ex(ev, 1 /* UserMode */, 0 /* not alertable */);
    check_wait(0, 0, "non-alertable wait on the event: signalled, APC still queued");
    wait1ex(ev, 0 /* KernelMode */, 1);
    check_wait(0, 0, "alertable kernel-mode wait: signalled, APC still queued");

    delay_alertable();
    check_wait(0xC0u, 1, "alertable KeDelayExecutionThread runs it: STATUS_USER_APC");
    check(apc_seen[0] == APC_CONTEXT, "the APC sees its context", NULL);
    check_esp(STACK_VA, "esp is exactly past the delay's arguments");
    delay_alertable();
    check(g_eax != 0xC0u && apc_runs == 1, "the next alertable wait finds nothing queued",
          NULL);

    /* Each alertable user-mode wait. */
    apc_runs = 0;
    read_with_apc(APC_CONTEXT, 0);
    wait1ex(file_handle, 1, 1);
    check_wait(0xC0u, 1, "alertable NtWaitForSingleObjectEx runs it");
    check_esp(STACK_VA, "esp is exactly past NtWaitForSingleObjectEx's arguments");

    apc_runs = 0;
    read_with_apc(APC_CONTEXT, 0);
    waitnex(ev);
    check_wait(0xC0u, 1, "alertable NtWaitForMultipleObjectsEx runs it (signalled object)");
    check_esp(STACK_VA, "esp is exactly past NtWaitForMultipleObjectsEx's arguments");

    apc_runs = 0;
    read_with_apc(APC_CONTEXT, 0);
    kewait1(ev);
    check_wait(0xC0u, 1, "alertable user-mode KeWaitForSingleObject runs it");
    check_esp(STACK_VA, "esp is exactly past KeWaitForSingleObject's arguments");

    apc_runs = 0;
    read_with_apc(APC_CONTEXT, 0);
    kewaitn(ev);
    check_wait(0xC0u, 1, "alertable user-mode KeWaitForMultipleObjects runs it");
    check_esp(STACK_VA, "esp is exactly past KeWaitForMultipleObjects's arguments");

    /* Several queued before one wait: all run, in request order. */
    apc_runs = 0;
    for (i = 0; i < 3; i++)
        read_with_apc(0x100u + (uint32_t)i, 0);
    delay_alertable();
    check_wait(0xC0u, 3, "three queued before the wait all run in it");
    check(apc_seen[0] == 0x100u && apc_seen[1] == 0x101u && apc_seen[2] == 0x102u,
          "in request order", NULL);

    /* Queued during the wait: its completion issues the next read. */
    apc_runs = 0;
    read_with_apc(CHAIN_CONTEXT, 0);
    delay_alertable();
    check_wait(0xC0u, 1, "one queued during the wait is not run by that wait");
    check_esp(STACK_VA, "esp is exact after a completion that issued a read");
    delay_alertable();
    check_wait(0xC0u, 2, "the next alertable wait runs it");
    check(apc_seen[1] == APC_CONTEXT, "with its own context", NULL);

    /* A thread that exits with an APC queued. */
    {
        HANDLE th = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);
        apc_runs = 0;
        check(th != NULL, "second thread started", NULL);
        if (th) {
            WaitForSingleObject(th, INFINITE);
            CloseHandle(th);
        }
        g_esp = STACK_VA;
        delay_alertable();
        snprintf(detail, sizeof detail, "status 0x%08X, runs %d", g_eax, apc_runs);
        check(apc_runs == 0 && g_eax != 0xC0u,
              "an exited thread's queued APC runs nowhere else", detail);
    }

    /* Past the queue: inline, not dropped. */
    apc_runs = 0;
    for (i = 0; i < 65; i++)
        read_with_apc(0x200u + (uint32_t)i, 0);
    snprintf(detail, sizeof detail, "runs %d", apc_runs);
    check(apc_runs == 1 && apc_seen[0] == 0x200u + 64,
          "the 65th request without a wait runs its APC inline", detail);
    delay_alertable();
    ok = apc_runs == 65;
    for (i = 0; ok && i < 64; i++)
        ok = apc_seen[1 + i] == 0x200u + (uint32_t)i;
    snprintf(detail, sizeof detail, "runs %d", apc_runs);
    check(ok && g_eax == 0xC0u, "the wait then runs the 64 queued, in order", detail);
    check_esp(STACK_VA, "esp is exact after 65 requests and a wait");

    /* The call still selects inline. */
    xbox_SetDeferredIoApc(0);
    apc_runs = 0;
    read_with_apc(APC_CONTEXT, 0);
    check(apc_runs == 1, "xbox_SetDeferredIoApc(0): the APC runs inline", NULL);
    xbox_SetDeferredIoApc(1);
}

static void run_inline(void)
{
    apc_runs = 0;
    g_esp = STACK_VA;
    read_with_apc(APC_CONTEXT, 0);
    check(g_eax == 0 && memcmp(mem + BUF_VA, "APC!", 4) == 0,
          "inline: NtReadFile reads the file", NULL);
    check(apc_runs == 1 && apc_seen[0] == APC_CONTEXT,
          "inline: the APC runs during NtReadFile, with its context", NULL);
    check_esp(STACK_VA, "inline: esp is exactly past NtReadFile's arguments");
    delay_alertable();
    check(g_eax != 0xC0u && apc_runs == 1,
          "inline: a later alertable wait finds nothing queued", NULL);
}

int main(void)
{
    char dir[MAX_PATH], file[MAX_PATH];
    const char *inl = getenv("RECOMP_INLINE_APC");
    FILE *f;
    uint32_t ev;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);

    mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT,
                       PAGE_READWRITE);
    if (!mem) {
        printf("guest memory stand-in not allocated\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;

    /* A host directory standing in for the disc, holding one file. */
    GetTempPathA(sizeof dir, dir);
    strcat(dir, inl ? "xr_io_apc_inline" : "xr_io_apc");
    CreateDirectoryA(dir, NULL);
    /* A forward slash, which Win32 takes as readily as a backslash. fopen is
     * the C library, not the shim, so nothing normalises a separator for it:
     * with a backslash the file landed beside the directory as
     * "xr_io_apc\\apc.bin" rather than in it, and the open under test then
     * correctly found nothing. One spelling for every caller here. */
    snprintf(file, sizeof file, "%s/apc.bin", dir);
    f = fopen(file, "wb");
    if (!f) {
        printf("cannot create %s\n", file);
        return 1;
    }
    fwrite("APC!", 1, 4, f);
    fclose(f);

    for (i = 0; i < S_COUNT; i++)
        *(uint32_t *)(mem + THUNK_VA + 4 * i) = 0x80000000u | ordinals[i];
    memset(mem + TIMEOUT_VA, 0, 8);
    memset(mem + OFFSET_VA, 0, 8);
    xbox_kernel_set_thunk_address(THUNK_VA, S_COUNT);
    xbox_kernel_init();
    xbox_path_init(dir, dir);
    xbox_kernel_bridge_init();

    file_handle = open_file();
    snprintf(detail, sizeof detail, "status 0x%08X", g_eax);
    check(file_handle != 0, "NtOpenFile opens the stand-in disc file", detail);
    ev = create_event();
    snprintf(detail, sizeof detail, "status 0x%08X", g_eax);
    check(ev != 0, "NtCreateEvent", detail);
    if (!file_handle || !ev)
        goto done;

    printf("%s\n", inl ? "RECOMP_INLINE_APC set: inline delivery"
                       : "default: deferred delivery");
    if (inl)
        run_inline();
    else
        run_deferred(ev);

done:
    DeleteFileA(file);
    RemoveDirectoryA(dir);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
