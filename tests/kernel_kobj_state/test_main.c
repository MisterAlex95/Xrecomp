/*
 * Kernel objects in a save state (XBA-246): made through the kernel calls a
 * title makes (by ordinal, through the thunk dispatcher, against the guest
 * window of the synthetic XBE), serialised, changed, torn down and made
 * again; their states and handle values checked, and waits on them working.
 *
 *   Objects     a notification event (set), a synchronization event (clear)
 *               and a second handle to it (NtDuplicateObject), a semaphore
 *               (2 of 5), a mutant owned by this thread, a waitable timer
 *               armed 3 s out, an I/O completion port with two packets, a
 *               file read 4 bytes in, a KEVENT and a KTIMER (KE shadows, a
 *               kernel timer), a thread that has ended, a thread that runs.
 *   Refusals    the same objects saved for another run: the running thread
 *               and the owned mutant are named, and no blob comes out.
 *   Restore     after the objects changed (event states flipped, semaphore
 *               released, mutant released, a packet taken, a handle closed,
 *               the file read further, the timer cancelled, a new event
 *               made): every handle value answers with its saved state, the
 *               closed one is back and is the same object as its source, the
 *               new one is gone.
 *   Waits       after the restore: the KTIMER fires, the KEVENT waits and is
 *               set, the ended thread is signalled, the running thread waits
 *               on a restored event and ends, and the wait on it returns.
 *   Format      a truncated blob is refused before anything is torn down; a
 *               record of an unknown type is skipped.
 *   Blocked     a load refuses, changing nothing, while a thread is blocked
 *               in a wait on an event it would tear down.
 */
#include "kernel.h"
#include "kernel_kobj_state.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void (*recomp_func_t)(void);
static void worker(void);
#define WORKER_VA 0x00010000u
recomp_func_t recomp_lookup(uint32_t va) { return va == WORKER_VA ? worker : NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define G8(va)  (*(volatile uint8_t  *)((uintptr_t)(va) + g_xbox_mem_offset))

enum {
    ORD_DELAY = 99, ORD_KEINITEVENT = 108, ORD_KEINITTIMER = 113, ORD_KESETEVENT = 145,
    ORD_KESETTIMER = 149, ORD_KEWAIT = 159, ORD_CANCELTIMER = 185, ORD_CLEAREVENT = 186,
    ORD_NTCLOSE = 187, ORD_CREATEFILE = 190, ORD_CREATEIOC = 191, ORD_CREATEMUTANT = 192,
    ORD_CREATESEM = 193, ORD_CREATETIMER = 194, ORD_DUPLICATE = 197, ORD_QUERYEVENT = 209,
    ORD_QUERYIOC = 212, ORD_QUERYMUTANT = 213, ORD_QUERYSEM = 214, ORD_QUERYTIMER = 216,
    ORD_READFILE = 219, ORD_RELEASEMUTANT = 221, ORD_RELEASESEM = 222, ORD_REMOVEIOC = 223,
    ORD_SETEVENT = 225, ORD_SETIOC = 227, ORD_SETTIMEREX = 229, ORD_WAIT = 233,
    ORD_CREATEEVENT = 189, ORD_PSCREATE = 255, ORD_PSTERM = 258
};
static const uint32_t ORDS[] = {
    ORD_DELAY, ORD_KEINITEVENT, ORD_KEINITTIMER, ORD_KESETEVENT, ORD_KESETTIMER, ORD_KEWAIT,
    ORD_CANCELTIMER, ORD_CLEAREVENT, ORD_NTCLOSE, ORD_CREATEFILE, ORD_CREATEIOC,
    ORD_CREATEMUTANT, ORD_CREATESEM, ORD_CREATETIMER, ORD_DUPLICATE, ORD_QUERYEVENT,
    ORD_QUERYIOC, ORD_QUERYMUTANT, ORD_QUERYSEM, ORD_QUERYTIMER, ORD_READFILE,
    ORD_RELEASEMUTANT, ORD_RELEASESEM, ORD_REMOVEIOC, ORD_SETEVENT, ORD_SETIOC,
    ORD_SETTIMEREX, ORD_WAIT, ORD_CREATEEVENT, ORD_PSCREATE, ORD_PSTERM
};
#define NORDS (sizeof ORDS / sizeof ORDS[0])


static int failures;
static uint32_t g_thunks, g_stack, g_scratch;
static volatile int g_go;               /* the running thread: wait now */
static volatile uint32_t g_go_event;    /* ... on this handle */
static volatile uint32_t g_live_status = 0xFFFFFFFFu;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-66s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static void checkv(int ok, const char *what, uint32_t got)
{
    char d[48];
    snprintf(d, sizeof d, "got 0x%08X", got);
    check(ok, what, d);
}

static recomp_func_t thunk(uint32_t ordinal)
{
    unsigned i;
    for (i = 0; i < NORDS; i++)
        if (ORDS[i] == ordinal)
            return recomp_lookup_kernel(G32(g_thunks + 4 * i));
    return NULL;
}

/* A stdcall kernel call on this thread's guest stack. */
static uint32_t guest_call(uint32_t ordinal, int argc, const uint32_t *argv)
{
    recomp_func_t fn = thunk(ordinal);
    uint32_t sp0 = g_esp, sp;
    int i;

    if (!fn) {
        printf("  ordinal %u did not resolve\n", ordinal);
        failures++;
        return 0xFFFFFFFFu;
    }
    if (!sp0)
        sp0 = g_stack + 0x800;
    sp = (sp0 - 96u) & ~15u;
    G32(sp) = 0x00011000u;              /* return address */
    for (i = 0; i < argc; i++)
        G32(sp + 4 + 4 * (uint32_t)i) = argv[i];
    g_esp = sp;
    g_eax = 0xDEADBEEFu;
    fn();
    g_esp = sp0;
    return g_eax;
}
#define GCALL(ord, ...) guest_call((ord), \
    (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), (uint32_t[]){__VA_ARGS__})

/* Scratch guest memory, one 16-byte cell per use. */
static uint32_t cell(int i) { return g_scratch + 16u * (uint32_t)i; }

static uint32_t rel_ms(int i, int64_t ms)   /* a relative LARGE_INTEGER */
{
    int64_t v = -ms * 10000;
    G32(cell(i)) = (uint32_t)v;
    G32(cell(i) + 4) = (uint32_t)((uint64_t)v >> 32);
    return cell(i);
}

enum { K_EXIT, K_LIVE, K_BLOCK };
static volatile uint32_t g_block_status = 0xFFFFFFFFu;

static void worker(void)
{
    int kind = (int)G32(g_esp + 4);
    if (kind == K_EXIT)
        return;                             /* ends at once, status 0 */
    if (kind == K_BLOCK) {                  /* blocked in a wait on an event */
        g_block_status = GCALL(ORD_WAIT, g_go_event, 0, rel_ms(202, 5000));
        return;
    }
    while (!g_go)
        GCALL(ORD_DELAY, 0, 0, rel_ms(200, 5));
    g_live_status = GCALL(ORD_WAIT, g_go_event, 0, rel_ms(201, 5000));
}

static uint32_t spawn(uint32_t hva, int kind)
{
    uint32_t argv[10] = { hva, 0, 0, 0, 0, (uint32_t)kind, 0, 0, 0, WORKER_VA };
    recomp_func_t fn = thunk(ORD_PSCREATE);
    uint32_t sp = g_stack + 0x400;
    unsigned k;

    G32(sp) = 0xBEEF0001u;
    for (k = 0; k < 10; k++)
        G32(sp + 4 + 4 * k) = argv[k];
    g_esp = sp;
    fn();
    g_esp = 0;
    return g_eax;
}

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    b = malloc((size_t)len);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)len;
    return b;
}

/* Handles live in guest memory: the title's variables. */
enum {
    H_EVN, H_EVS, H_EVDUP, H_SEM, H_MUT, H_TMR, H_PORT, H_FILE, H_EXIT, H_LIVE, H_GO, H_NEW, H_BLOCK,
    H_COUNT
};
static uint32_t g_hva;
#define HVA(i) (g_hva + 4u * (uint32_t)(i))
#define TOK(i) G32(HVA(i))

static uint32_t query2(uint32_t ord, uint32_t h, uint32_t *a, uint32_t *b)
{
    uint32_t out = cell(100), st;
    G32(out) = G32(out + 4) = G32(out + 8) = 0xCCCCCCCCu;
    st = GCALL(ord, h, out);
    *a = G32(out);
    if (b) *b = G32(out + 4);
    return st;
}

static uint32_t open_file(uint32_t hva, const char *xbox_path)
{
    uint32_t str = cell(110), oa = cell(112), ios = cell(114), buf = cell(120);
    size_t n = strlen(xbox_path);
    memcpy((void *)((uintptr_t)buf + g_xbox_mem_offset), xbox_path, n + 1);
    G32(str) = (uint32_t)n | ((uint32_t)(n + 1) << 16);
    G32(str + 4) = buf;
    G32(oa) = 0;
    G32(oa + 4) = str;
    G32(oa + 8) = 0x40;                     /* OBJ_CASE_INSENSITIVE */
    /* GENERIC_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | NON_DIRECTORY */
    return GCALL(ORD_CREATEFILE, hva, 0x80000000u, oa, ios, 0, 0x80, 1, 1, 0x60);
}

static uint32_t read4(uint32_t h, char out[5])
{
    uint32_t ios = cell(130), buf = cell(132), st;
    st = GCALL(ORD_READFILE, h, 0, 0, 0, ios, buf, 4, 0);
    memcpy(out, (void *)((uintptr_t)buf + g_xbox_mem_offset), 4);
    out[4] = 0;
    return st;
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/kkobjXXXXXX", path[300], cmd[300], s4[5];
    size_t len = 0, blob_len = 0;
    unsigned char *xbe;
    void *blob = NULL;
    uint32_t kev, ktm, a, b, st, i;
    XboxKobjRefusals why;
    char err[200];

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!mkdtemp(tmpl)) return 2;
    setenv("XDG_DATA_HOME", tmpl, 1);
    xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &len);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, len)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }
    g_thunks  = xbox_HeapAlloc(0x1000, 4096);
    g_stack   = xbox_HeapAlloc(0x1000, 4096);
    g_scratch = xbox_HeapAlloc(0x1000, 4096);
    g_hva     = xbox_HeapAlloc(0x100, 4096);
    kev       = xbox_HeapAlloc(0x40, 16);
    ktm       = xbox_HeapAlloc(0x40, 16);
    for (i = 0; i < NORDS; i++)
        G32(g_thunks + 4 * i) = 0x80000000u | ORDS[i];
    G32(g_thunks + 4 * NORDS) = 0;
    xbox_kernel_set_thunk_address(g_thunks, NORDS + 1);
    xbox_kernel_bridge_init();
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);
    xbox_GuestCpuPart();
    xbox_path_init(tmpl, tmpl);
    snprintf(path, sizeof path, "%s/kobj.bin", tmpl);
    {
        FILE *f = fopen(path, "wb");
        if (!f) return 2;
        fputs("0123456789ABCDEF", f);
        fclose(f);
    }

    printf("Objects\n");
    checkv(GCALL(ORD_CREATEEVENT, HVA(H_EVN), 0, 0 /* notification */, 1) == 0,
           "notification event, set", TOK(H_EVN));
    checkv(GCALL(ORD_CREATEEVENT, HVA(H_EVS), 0, 1 /* synchronization */, 0) == 0,
           "synchronization event, clear", TOK(H_EVS));
    checkv(GCALL(ORD_DUPLICATE, TOK(H_EVS), HVA(H_EVDUP), 2) == 0
           && TOK(H_EVDUP) != TOK(H_EVS), "a second handle to it", TOK(H_EVDUP));
    checkv(GCALL(ORD_CREATEEVENT, HVA(H_GO), 0, 1, 0) == 0, "the running thread's event",
           TOK(H_GO));
    checkv(GCALL(ORD_CREATESEM, HVA(H_SEM), 0, 2, 5) == 0, "semaphore 2 of 5", TOK(H_SEM));
    checkv(GCALL(ORD_CREATEMUTANT, HVA(H_MUT), 0, 1) == 0, "mutant owned by this thread",
           TOK(H_MUT));
    checkv(GCALL(ORD_CREATETIMER, HVA(H_TMR), 0, 1) == 0, "waitable timer", TOK(H_TMR));
    checkv(GCALL(ORD_SETTIMEREX, TOK(H_TMR), rel_ms(1, 3000), 0, 0, 0, 0, 0, 0) == 0,
           "  armed 3 s out", 0);
    checkv(GCALL(ORD_CREATEIOC, HVA(H_PORT), 0, 0, 0) == 0, "I/O completion port", TOK(H_PORT));
    GCALL(ORD_SETIOC, TOK(H_PORT), 0x11, 0x22, 0, 7);
    GCALL(ORD_SETIOC, TOK(H_PORT), 0x33, 0x44, 0, 9);
    st = open_file(HVA(H_FILE), "D:\\kobj.bin");
    checkv(st == 0 && TOK(H_FILE), "file D:\\kobj.bin", st);
    st = read4(TOK(H_FILE), s4);
    check(st == 0 && !strcmp(s4, "0123"), "  read 4 bytes in", s4);
    GCALL(ORD_KEINITEVENT, kev, 0 /* notification */, 0);
    GCALL(ORD_KEINITTIMER, ktm, 0);
    GCALL(ORD_KESETTIMER, ktm, (uint32_t)(-4000000LL), 0xFFFFFFFFu, 0);
    check(1, "KEVENT and KTIMER initialised, KTIMER set 400 ms out", NULL);
    spawn(HVA(H_EXIT), K_EXIT);
    st = GCALL(ORD_WAIT, TOK(H_EXIT), 0, rel_ms(3, 3000));
    checkv(st == 0, "a thread that has ended", st);
    spawn(HVA(H_LIVE), K_LIVE);
    g_go_event = TOK(H_GO);
    usleep(20000);
    st = GCALL(ORD_WAIT, TOK(H_LIVE), 0, rel_ms(4, 0));
    checkv(st == STATUS_TIMEOUT, "a thread that runs", st);

    printf("Refusals\n");
    st = (uint32_t)xbox_KobjCheck(XBOX_KOBJ_ACROSS_RUNS, &why);
    {
        int thread = 0, mutant = 0, k;
        for (k = 0; k < why.count && k < XBOX_KOBJ_REFUSALS_MAX; k++) {
            printf("    %s 0x%08X / 0x%08X: %s\n", why.r[k].what, why.r[k].token,
                   why.r[k].guest_va, why.r[k].reason);
            if (!strcmp(why.r[k].what, "thread")) thread = 1;
            if (!strcmp(why.r[k].what, "mutant") && why.r[k].token == TOK(H_MUT)) mutant = 1;
        }
        check(st > 0 && thread && mutant,
              "across runs: the running thread and the owned mutant are named", NULL);
    }
    check(!xbox_KobjSave(&blob, &blob_len, XBOX_KOBJ_ACROSS_RUNS, NULL) && !blob,
          "  and no blob comes out", NULL);

    printf("Save\n");
    st = (uint32_t)xbox_KobjSave(&blob, &blob_len, XBOX_KOBJ_IN_SESSION, &why);
    {
        char d[160];
        snprintf(d, sizeof d, "%d refusal(s): %s", why.count, why.count ? why.r[0].reason : "");
        check(st == 1 && blob && blob_len > 8, "in session: saved", d);
    }
    printf("  blob: %zu bytes\n", blob_len);

    printf("Change everything\n");
    GCALL(ORD_CLEAREVENT, TOK(H_EVN));
    GCALL(ORD_SETEVENT, TOK(H_EVS), 0);
    GCALL(ORD_RELEASESEM, TOK(H_SEM), 3, 0);
    GCALL(ORD_RELEASEMUTANT, TOK(H_MUT), 0);
    GCALL(ORD_REMOVEIOC, TOK(H_PORT), cell(5), cell(6), cell(7), rel_ms(8, 0));
    GCALL(ORD_CREATEEVENT, HVA(H_NEW), 0, 0, 1);   /* before the close: a slot of its own */
    GCALL(ORD_NTCLOSE, TOK(H_EVDUP));
    GCALL(ORD_CANCELTIMER, TOK(H_TMR), 0);
    read4(TOK(H_FILE), s4);
    checkv(GCALL(ORD_QUERYEVENT, TOK(H_EVDUP), cell(100)) == STATUS_INVALID_HANDLE,
           "the closed handle is gone", TOK(H_EVDUP));

    printf("Format\n");
    st = (uint32_t)xbox_KobjLoad(blob, blob_len - 3, XBOX_KOBJ_IN_SESSION, err, sizeof err);
    check(!st, "a truncated blob is refused", err);
    checkv(query2(ORD_QUERYEVENT, TOK(H_NEW), &a, &b) == 0,
           "  before anything is torn down (a newer handle still answers)", TOK(H_NEW));
    {
        /* An unknown record before END: skipped. */
        unsigned char *ext = malloc(blob_len + 12);
        memcpy(ext, blob, blob_len - 8);
        memcpy(ext + blob_len - 8, "\x39\x30\x01\x00\x04\x00\x00\x00\xAA\xBB\xCC\xDD", 12);
        memcpy(ext + blob_len + 4, (unsigned char *)blob + blob_len - 8, 8);
        st = (uint32_t)xbox_KobjLoad(ext, blob_len + 12, XBOX_KOBJ_IN_SESSION, err, sizeof err);
        check(st == 1, "a record of an unknown type is skipped (restore 1)", err);
        free(ext);
    }
    /* Change some again, then the real restore. */
    GCALL(ORD_SETEVENT, TOK(H_EVS), 0);
    GCALL(ORD_RELEASESEM, TOK(H_SEM), 1, 0);
    GCALL(ORD_CREATEEVENT, HVA(H_NEW), 0, 0, 1);

    printf("Restore\n");
    st = (uint32_t)xbox_KobjLoad(blob, blob_len, XBOX_KOBJ_IN_SESSION, err, sizeof err);
    check(st == 1, "loaded", err);
    st = query2(ORD_QUERYEVENT, TOK(H_EVN), &a, &b);
    check(st == 0 && a == 0 && b == 1, "notification event: same handle, set", NULL);
    st = query2(ORD_QUERYEVENT, TOK(H_EVS), &a, &b);
    check(st == 0 && a == 1 && b == 0, "synchronization event: same handle, clear", NULL);
    st = query2(ORD_QUERYSEM, TOK(H_SEM), &a, &b);
    check(st == 0 && a == 2 && b == 5, "semaphore: 2 of 5", NULL);
    st = query2(ORD_QUERYMUTANT, TOK(H_MUT), &a, &b);
    check(st == 0 && (int32_t)a == 0 && (b & 0xFF) == 1, "mutant: owned by this thread, once",
          NULL);
    checkv(GCALL(ORD_RELEASEMUTANT, TOK(H_MUT), 0) == 0, "  and this thread can release it", 0);
    st = query2(ORD_QUERYTIMER, TOK(H_TMR), &a, &b);
    {
        uint64_t left = (uint64_t)a | ((uint64_t)b << 32);
        uint8_t state = G8(cell(100) + 8);
        char d[64];
        snprintf(d, sizeof d, "%llu ms left, state %u", (unsigned long long)(left / 10000), state);
        check(st == 0 && left > 15000000ull && left <= 30000000ull && !state,
              "waitable timer: armed again, about 3 s left", d);
    }
    st = query2(ORD_QUERYIOC, TOK(H_PORT), &a, NULL);
    check(st == 0 && a == 2, "port: two packets", NULL);
    st = GCALL(ORD_REMOVEIOC, TOK(H_PORT), cell(5), cell(6), cell(7), rel_ms(8, 0));
    check(st == 0 && G32(cell(5)) == 0x11 && G32(cell(6)) == 0x22 && G32(cell(7) + 4) == 7,
          "  the first one first", NULL);
    st = GCALL(ORD_REMOVEIOC, TOK(H_PORT), cell(5), cell(6), cell(7), rel_ms(8, 0));
    check(st == 0 && G32(cell(5)) == 0x33 && G32(cell(7) + 4) == 9, "  then the second", NULL);
    st = read4(TOK(H_FILE), s4);
    check(st == 0 && !strcmp(s4, "4567"), "file: same handle, at offset 4", s4);
    st = query2(ORD_QUERYEVENT, TOK(H_EVDUP), &a, &b);
    check(st == 0 && b == 0, "the closed second handle is back", NULL);
    GCALL(ORD_SETEVENT, TOK(H_EVDUP), 0);
    st = query2(ORD_QUERYEVENT, TOK(H_EVS), &a, &b);
    check(st == 0 && b == 1, "  and is the same object as its source", NULL);
    checkv(GCALL(ORD_QUERYEVENT, TOK(H_NEW), cell(100)) == STATUS_INVALID_HANDLE,
           "the handle made after the save is gone", TOK(H_NEW));
    check(xbox_KobjCheck(XBOX_KOBJ_IN_SESSION, NULL) == 0, "a restored kernel saves again",
          NULL);

    printf("Waits\n");
    st = GCALL(ORD_KEWAIT, ktm, 0, 0, 0, rel_ms(9, 3000));
    checkv(st == 0, "the KTIMER fires", st);
    st = GCALL(ORD_KEWAIT, kev, 0, 0, 0, rel_ms(10, 0));
    checkv(st == STATUS_TIMEOUT, "the KEVENT is clear", st);
    GCALL(ORD_KESETEVENT, kev, 0, 0);
    st = GCALL(ORD_KEWAIT, kev, 0, 0, 0, rel_ms(10, 0));
    checkv(st == 0, "  and set", st);
    st = GCALL(ORD_WAIT, TOK(H_EXIT), 0, rel_ms(11, 0));
    checkv(st == 0, "the ended thread is signalled", st);
    st = GCALL(ORD_WAIT, TOK(H_LIVE), 0, rel_ms(12, 0));
    checkv(st == STATUS_TIMEOUT, "the running thread still runs", st);
    g_go = 1;
    usleep(20000);
    GCALL(ORD_SETEVENT, TOK(H_GO), 0);
    st = GCALL(ORD_WAIT, TOK(H_LIVE), 0, rel_ms(13, 5000));
    checkv(st == 0, "it waits on a restored event, wakes and ends", st);
    checkv(g_live_status == 0, "  its wait returned STATUS_SUCCESS", g_live_status);

    printf("A thread blocked on an event\n");
    spawn(HVA(H_BLOCK), K_BLOCK);
    usleep(50000);
    st = (uint32_t)xbox_KobjLoad(blob, blob_len, XBOX_KOBJ_IN_SESSION, err, sizeof err);
    check(!st && strstr(err, "blocked"), "a load refuses while a thread waits on an event", err);
    checkv(query2(ORD_QUERYEVENT, TOK(H_EVN), &a, &b) == 0,
           "  and tears nothing down", TOK(H_EVN));
    GCALL(ORD_SETEVENT, TOK(H_GO), 0);
    st = GCALL(ORD_WAIT, TOK(H_BLOCK), 0, rel_ms(14, 5000));
    check(st == 0 && g_block_status == 0, "  the thread's wait completes as before", NULL);

    printf("Reset\n");
    xbox_KobjReset();
    checkv(GCALL(ORD_QUERYEVENT, TOK(H_EVN), cell(100)) == STATUS_INVALID_HANDLE,
           "every handle is gone", TOK(H_EVN));
    st = (uint32_t)xbox_KobjLoad(blob, blob_len, XBOX_KOBJ_IN_SESSION, err, sizeof err);
    check(!st && strstr(err, "not running"),
          "a state whose running thread has ended does not load in session", err);

    free(blob);
    free(xbe);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    (void)system(cmd);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
