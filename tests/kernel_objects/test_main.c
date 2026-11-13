/*
 * The object namespace and objects with a body in guest memory (XBA-114).
 *
 * Driven through the thunk table against a real guest window
 * (xbox_MemoryLayoutInit on tools/conformance/test.xbe): ObCreateObject
 * allocates from the guest pool. Layouts and argument counts are nxdk's
 * xboxkrnl.h (OBJECT_HEADER, OBJECT_DIRECTORY_INFORMATION, OBJECT_TYPE);
 * every call checks esp after it.
 *
 * Checked:
 *   1. NtOpenDirectoryObject opens \, \??, \Device (also relative to the
 *      root's handle) and refuses a name that is no directory;
 *      NtQueryDirectoryObject lists one entry per call, with its type tag,
 *      Context counting, STATUS_NO_MORE_ENTRIES at the end, RestartScan,
 *      STATUS_BUFFER_TOO_SMALL with the size needed; \?? lists D: and a
 *      link IoCreateSymbolicLink made.
 *   2. ObCreateObject lays out OBJECT_HEADER and the name info;
 *      ObInsertObject names it (listed in its directory, with its type's
 *      pool tag) and opens a handle; a second object of the same name is
 *      refused and deleted; ObOpenObjectByName finds it (and checks the
 *      type); ObReferenceObjectByHandle / ByPointer and ObfDereferenceObject
 *      move PointerCount; closing the handles calls the type's
 *      CloseProcedure with the handle count, the last one takes the name out
 *      and the last reference calls DeleteProcedure. A permanent object
 *      keeps its name after its last handle until ObMakeTemporaryObject.
 *   3. NtQueueApcThread (XBA-133) on the current thread: the routine runs at
 *      the next alertable user-mode wait as ApcRoutine(Arg1, Arg2, Arg3),
 *      which returns STATUS_USER_APC; a handle that is no thread is
 *      STATUS_INVALID_HANDLE. (Waking another thread's wait is the same path
 *      as KeInsertQueueApc's, checked in tests/kernel_io_apc.)
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;
#define XBOX_TO_NATIVE(va) ((void *)((uintptr_t)(va) + g_xbox_mem_offset))

#define CLOSE_VA  0x00007100u     /* "guest" addresses of the type procedures */
#define DELETE_VA 0x00007200u
#define APC_VA    0x00007300u

static int failures;
static char d[256];
static uint32_t B;                /* our guest scratch area */

#define G32(va) (*(volatile uint32_t *)XBOX_TO_NATIVE(va))
#define G16(va) (*(volatile uint16_t *)XBOX_TO_NATIVE(va))

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-70s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

/* Offsets in the scratch area. */
enum {
    O_THUNK = 0x0000, O_STACK = 0x2000, O_HANDLE = 0x3000, O_OUT = 0x3100,
    O_ANSI = 0x3200, O_OA = 0x3300, O_NAME = 0x3400, O_ANSI2 = 0x3500,
    O_NAME2 = 0x3600, O_BUF = 0x4000, O_TYPE = 0x5000, O_TYPE2 = 0x5100,
    O_TIB = 0x6000, O_KT = 0x6100, O_ZERO = 0x6400,
};
#define V(o) (B + (uint32_t)(o))

enum {
    S_CLOSE, S_OPENDIR, S_QUERYDIR, S_SYMLINK, S_CREATE, S_INSERT, S_MKTEMP,
    S_OPENNAME, S_OPENPTR, S_REFPTR, S_REFHANDLE, S_DEREF, S_QAPC, S_DELAY, S_COUNT
};
static const uint32_t ord[S_COUNT] = {
    187, 201, 208, 67, 239, 241, 242, 243, 244, 248, 246, 250, 206, 99,
};

static uint32_t call(int slot, const uint32_t *args, int n)
{
    uint32_t sp = V(O_STACK) + 0x800;
    int i;
    G32(sp) = 0xBEEF0000u | (uint32_t)slot;
    for (i = 0; i < n; i++)
        G32(sp + 4 + 4 * (uint32_t)i) = args[i];
    g_esp = sp;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(G32(V(O_THUNK) + 4 * (uint32_t)slot))();
    return g_eax;
}
#define CALL(slot, ...) do { uint32_t a_[] = { __VA_ARGS__ }; \
    call(slot, a_, (int)(sizeof a_ / sizeof a_[0])); } while (0)
static int esp_ok(int n) { return g_esp == V(O_STACK) + 0x800 + 4 + 4 * (uint32_t)n; }

/* The type procedures: stdcall, so they pop their return address and
 * arguments. */
static int closes, deletes;
static uint32_t last_close_obj, last_close_count, last_delete_obj;

static void close_proc(void)
{
    last_close_obj = G32(g_esp + 4);
    last_close_count = G32(g_esp + 8);
    closes++;
    g_esp += 12;
}

static void delete_proc(void)
{
    last_delete_obj = G32(g_esp + 4);
    deletes++;
    g_esp += 8;
}

static int apc_runs;
static uint32_t apc_args[3];

static void apc_proc(void)       /* ApcRoutine(Arg1, Arg2, Arg3), stdcall */
{
    apc_args[0] = G32(g_esp + 4);
    apc_args[1] = G32(g_esp + 8);
    apc_args[2] = G32(g_esp + 12);
    apc_runs++;
    g_esp += 16;
}

recomp_func_t recomp_lookup(uint32_t va);
recomp_func_t recomp_lookup(uint32_t va)
{
    return va == CLOSE_VA ? close_proc : va == DELETE_VA ? delete_proc
         : va == APC_VA ? apc_proc : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va);
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

/* OBJECT_ATTRIBUTES { RootDirectory, ObjectName, Attributes } naming `name`. */
static uint32_t oa(uint32_t root, const char *name, uint32_t attrs)
{
    strcpy((char *)XBOX_TO_NATIVE(V(O_NAME)), name);
    G16(V(O_ANSI)) = (uint16_t)strlen(name);
    G16(V(O_ANSI) + 2) = (uint16_t)(strlen(name) + 1);
    G32(V(O_ANSI) + 4) = V(O_NAME);
    G32(V(O_OA)) = root;
    G32(V(O_OA) + 4) = V(O_ANSI);
    G32(V(O_OA) + 8) = attrs;
    return V(O_OA);
}

static uint32_t open_dir(uint32_t root, const char *name)
{
    G32(V(O_HANDLE)) = 0;
    CALL(S_OPENDIR, V(O_HANDLE), oa(root, name, 0x40));
    return g_eax == 0 ? G32(V(O_HANDLE)) : 0;
}

/* All the entries of a directory, "name/TAG " each. */
static void list_dir(uint32_t h, char *out, size_t n)
{
    uint32_t restart = 1;
    out[0] = 0;
    for (;;) {
        CALL(S_QUERYDIR, h, V(O_BUF), 0x200, restart, V(O_OUT), V(O_OUT) + 4);
        restart = 0;
        if (g_eax)
            break;
        {
            uint32_t tag = G32(V(O_BUF) + 8);
            size_t l = strlen(out);
            snprintf(out + l, n - l, "%.*s/%.4s ", (int)G16(V(O_BUF)),
                     (const char *)XBOX_TO_NATIVE(G32(V(O_BUF) + 4)), (const char *)&tag);
        }
    }
}

/* ═══════════════════ 1. Directories ═══════════════════ */

static void test_directories(void)
{
    char list[512];
    uint32_t root, dev, dos, h;

    root = open_dir(0, "\\");
    check(root != 0 && esp_ok(2), "NtOpenDirectoryObject \\ (2 arguments)", NULL);
    dev = open_dir(0, "\\Device");
    check(dev != 0, "NtOpenDirectoryObject \\Device", NULL);
    h = open_dir(root, "Device");
    check(h == dev, "relative to the root's handle: the same directory", NULL);
    open_dir(0, "\\Nope");
    snprintf(d, sizeof d, "status 0x%08X", g_eax);
    check(g_eax == 0xC0000034u, "a name that is no directory: STATUS_OBJECT_NAME_NOT_FOUND", d);

    list_dir(root, list, sizeof list);
    check(!strcmp(list, "??/Dire Device/Dire "), "\\ lists ?? and Device", list);

    CALL(S_QUERYDIR, dev, V(O_BUF), 0x200, 1, V(O_OUT), V(O_OUT) + 4);
    snprintf(d, sizeof d, "status 0x%08X len %u ctx %u ret %u", g_eax, G16(V(O_BUF)),
             G32(V(O_OUT)), G32(V(O_OUT) + 4));
    check(g_eax == 0 && esp_ok(6) && G16(V(O_BUF)) == 6 && G32(V(O_BUF) + 4) == V(O_BUF) + 12
          && !memcmp(XBOX_TO_NATIVE(V(O_BUF) + 12), "CdRom0", 7) && G32(V(O_OUT)) == 1
          && G32(V(O_OUT) + 4) == 12 + 7,
          "NtQueryDirectoryObject: one entry, name after it, Context 1 (6 arguments)", d);
    CALL(S_QUERYDIR, dev, V(O_BUF), 0x200, 0, V(O_OUT), 0);
    check(g_eax == 0 && G16(V(O_BUF)) == 9 && G32(V(O_OUT)) == 2, "the next call: Harddisk0", NULL);
    CALL(S_QUERYDIR, dev, V(O_BUF), 0x200, 0, V(O_OUT), 0);
    check(g_eax == 0x8000001Au, "then STATUS_NO_MORE_ENTRIES", NULL);
    CALL(S_QUERYDIR, dev, V(O_BUF), 8, 1, V(O_OUT), V(O_OUT) + 4);
    check(g_eax == 0xC0000023u && G32(V(O_OUT) + 4) == 19,
          "too small: STATUS_BUFFER_TOO_SMALL and the size needed", NULL);

    h = open_dir(dev, "Harddisk0");
    list_dir(h, list, sizeof list);
    check(strstr(list, "Partition1/Devi ") != NULL, "\\Device\\Harddisk0 lists the partitions", list);

    /* A drive letter the title links itself. */
    strcpy((char *)XBOX_TO_NATIVE(V(O_NAME2)), "\\??\\Q:");
    G16(V(O_ANSI2)) = 6; G16(V(O_ANSI2) + 2) = 7; G32(V(O_ANSI2) + 4) = V(O_NAME2);
    oa(0, "\\Device\\Harddisk0\\Partition1", 0);
    CALL(S_SYMLINK, V(O_ANSI2), V(O_ANSI));
    dos = open_dir(0, "\\??");
    list_dir(dos, list, sizeof list);
    check(strstr(list, "D:/Symb ") && strstr(list, "Q:/Symb "),
          "\\?? lists D: and the link IoCreateSymbolicLink made", list);
    CALL(S_CLOSE, dev);
    check(g_eax == 0, "NtClose of a directory handle", NULL);
}

/* ═══════════════════ 2. Objects ═══════════════════ */

static uint32_t create(uint32_t type, const char *name, uint32_t attrs, uint32_t size)
{
    G32(V(O_OUT)) = 0;
    CALL(S_CREATE, type, name ? oa(0, name, attrs) : 0, size, V(O_OUT));
    return g_eax == 0 ? G32(V(O_OUT)) : 0;
}

#define PTRS(o)    G32((o) - 0x10)
#define HANDLES(o) G32((o) - 0x0C)

static void test_objects(void)
{
    uint32_t type = V(O_TYPE), obj, obj2, h, h2, p;
    char list[512];

    memset(XBOX_TO_NATIVE(type), 0, 0x1C);
    G32(type + 0x08) = CLOSE_VA;
    G32(type + 0x0C) = DELETE_VA;
    G32(type + 0x18) = 0x74736554u;          /* 'Test' */
    memset(XBOX_TO_NATIVE(V(O_TYPE2)), 0, 0x1C);

    obj = create(type, "\\Device\\Widget", 0x40, 0x20);
    snprintf(d, sizeof d, "status 0x%08X obj 0x%08X", g_eax, obj);
    check(obj && esp_ok(4), "ObCreateObject returns a body (4 arguments)", d);
    if (!obj)
        return;
    snprintf(d, sizeof d, "ptrs %u handles %u type 0x%X flags %u name %u@0x%X",
             PTRS(obj), HANDLES(obj), G32(obj - 8), G32(obj - 4), G16(obj - 0x18), G32(obj - 0x14));
    check(PTRS(obj) == 1 && HANDLES(obj) == 0 && G32(obj - 8) == type && G32(obj - 4) == 1
          && G16(obj - 0x18) == 14
          && !memcmp(XBOX_TO_NATIVE(G32(obj - 0x14)), "\\Device\\Widget", 14),
          "OBJECT_HEADER and name info as the console lays them out", d);

    G32(V(O_HANDLE)) = 0;
    CALL(S_INSERT, obj, oa(0, "\\Device\\Widget", 0x40), 0, V(O_HANDLE));
    h = G32(V(O_HANDLE));
    check(g_eax == 0 && h && HANDLES(obj) == 1 && PTRS(obj) == 1 && esp_ok(4),
          "ObInsertObject opens a handle (4 arguments)", NULL);
    {
        uint32_t dev = open_dir(0, "\\Device");
        list_dir(dev, list, sizeof list);
        check(strstr(list, "Widget/Test ") != NULL, "its name is listed in \\Device with the type's tag", list);
    }

    obj2 = create(type, "\\Device\\Widget", 0x40, 0x10);
    deletes = 0;
    CALL(S_INSERT, obj2, oa(0, "\\Device\\Widget", 0x40), 0, V(O_HANDLE));
    check(g_eax == 0xC0000035u && deletes == 1 && last_delete_obj == obj2,
          "the same name again: STATUS_OBJECT_NAME_COLLISION, that object deleted", NULL);

    G32(V(O_HANDLE)) = 0;
    CALL(S_OPENNAME, oa(0, "\\Device\\Widget", 0x40), type, 0, V(O_HANDLE));
    h2 = G32(V(O_HANDLE));
    check(g_eax == 0 && h2 && h2 != h && HANDLES(obj) == 2 && PTRS(obj) == 2 && esp_ok(4),
          "ObOpenObjectByName: a second handle", NULL);
    CALL(S_OPENNAME, oa(0, "\\Device\\Widget", 0x40), V(O_TYPE2), 0, V(O_HANDLE));
    check(g_eax == 0xC0000024u, "of another type: STATUS_OBJECT_TYPE_MISMATCH", NULL);

    G32(V(O_OUT)) = 0;
    CALL(S_REFHANDLE, h2, type, V(O_OUT));
    p = G32(V(O_OUT));
    check(g_eax == 0 && p == obj && PTRS(obj) == 3, "ObReferenceObjectByHandle: the body, one more reference", NULL);
    g_ecx = obj;
    call(S_DEREF, NULL, 0);
    check(PTRS(obj) == 2, "ObfDereferenceObject: one less", NULL);
    CALL(S_REFPTR, obj, type);
    check(g_eax == 0 && PTRS(obj) == 3 && esp_ok(2), "ObReferenceObjectByPointer (2 arguments)", NULL);
    g_ecx = obj;
    call(S_DEREF, NULL, 0);
    G32(V(O_HANDLE)) = 0;
    CALL(S_OPENPTR, obj, type, V(O_HANDLE));
    check(g_eax == 0 && G32(V(O_HANDLE)) && HANDLES(obj) == 3 && esp_ok(3),
          "ObOpenObjectByPointer: a third handle (3 arguments)", NULL);
    CALL(S_CLOSE, G32(V(O_HANDLE)));

    closes = deletes = 0;
    CALL(S_CLOSE, h2);
    snprintf(d, sizeof d, "closes %d obj 0x%X count %u handles %u", closes, last_close_obj,
             last_close_count, HANDLES(obj));
    check(closes == 1 && last_close_obj == obj && last_close_count == 2 && HANDLES(obj) == 1
          && deletes == 0, "closing a handle calls CloseProcedure with the handle count", d);
    CALL(S_CLOSE, h);
    check(closes == 2 && last_close_count == 1 && deletes == 1 && last_delete_obj == obj,
          "the last handle: CloseProcedure, then DeleteProcedure (no reference left)", NULL);
    CALL(S_OPENNAME, oa(0, "\\Device\\Widget", 0x40), 0, 0, V(O_HANDLE));
    check(g_eax == 0xC0000034u, "and the name is gone", NULL);

    /* A permanent object, kept alive by a bias. */
    obj = create(type, "\\Device\\Keep", 0x50 /* OBJ_PERMANENT */, 0x10);
    CALL(S_INSERT, obj, oa(0, "\\Device\\Keep", 0x50), 1, V(O_HANDLE));
    h = G32(V(O_HANDLE));
    check(g_eax == 0 && PTRS(obj) == 2, "ObInsertObject adds ObjectPointerBias", NULL);
    CALL(S_CLOSE, h);
    CALL(S_OPENNAME, oa(0, "\\Device\\Keep", 0x40), 0, 0, V(O_HANDLE));
    check(g_eax == 0, "a permanent object keeps its name after its last handle", NULL);
    CALL(S_CLOSE, G32(V(O_HANDLE)));
    CALL(S_MKTEMP, obj);
    CALL(S_OPENNAME, oa(0, "\\Device\\Keep", 0x40), 0, 0, V(O_HANDLE));
    check(g_eax == 0xC0000034u && esp_ok(4), "ObMakeTemporaryObject takes the name out", NULL);
    deletes = 0;
    g_ecx = obj;
    call(S_DEREF, NULL, 0);
    check(deletes == 1, "the last reference deletes it", NULL);
}

/* ═══════════════════ 3. NtQueueApcThread ═══════════════════ */

static void test_queue_apc(void)
{
    /* A TIB whose fs:[0x28] names this thread's KTHREAD, as the loader
     * builds them. */
    memset(XBOX_TO_NATIVE(V(O_TIB)), 0, 0x40);
    memset(XBOX_TO_NATIVE(V(O_KT)), 0, 0x200);
    G32(V(O_TIB)) = 0xFFFFFFFFu;
    G32(V(O_TIB) + 0x28) = V(O_KT);
    xbox_InitGuestKthread(V(O_KT), 0, V(O_STACK) + 0x1000, V(O_STACK));
    g_fs_base = V(O_TIB);
    G32(V(O_ZERO)) = 0; G32(V(O_ZERO) + 4) = 0;

    apc_runs = 0;
    CALL(S_QAPC, 0xFFFFFFFEu /* NtCurrentThread */, APC_VA, 0x11, 0x22, 0x33);
    check(g_eax == 0 && apc_runs == 0 && esp_ok(5),
          "NtQueueApcThread queues, nothing runs yet (5 arguments)", NULL);
    CALL(S_DELAY, 1 /* UserMode */, 1, V(O_ZERO));
    snprintf(d, sizeof d, "status 0x%08X runs %d args 0x%X 0x%X 0x%X", g_eax, apc_runs,
             apc_args[0], apc_args[1], apc_args[2]);
    check(g_eax == 0xC0u && apc_runs == 1 && apc_args[0] == 0x11 && apc_args[1] == 0x22
          && apc_args[2] == 0x33, "the next alertable wait runs ApcRoutine(1, 2, 3)", d);
    CALL(S_DELAY, 1, 1, V(O_ZERO));
    check(g_eax != 0xC0u && apc_runs == 1, "once", NULL);
    CALL(S_QAPC, 0x12345678u, APC_VA, 0, 0, 0);
    check(g_eax == 0xC0000008u, "a handle that is no thread: STATUS_INVALID_HANDLE", NULL);
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

int main(int argc, char **argv)
{
    size_t len = 0;
    unsigned char *xbe;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &len);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, len)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }
    B = xbox_HeapAlloc(0x8000, 4096);
    for (i = 0; i < S_COUNT; i++)
        G32(V(O_THUNK) + 4 * (uint32_t)i) = 0x80000000u | ord[i];
    xbox_kernel_set_thunk_address(V(O_THUNK), S_COUNT);
    xbox_kernel_bridge_init();

    test_directories();
    test_objects();
    test_queue_apc();

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
