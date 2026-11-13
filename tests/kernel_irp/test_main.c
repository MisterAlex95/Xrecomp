/*
 * IRPs in guest memory (XBA-138): IoAllocateIrp, IoInitializeIrp,
 * IoBuildAsynchronousFsdRequest, IofCallDriver, IofCompleteRequest, and
 * NtFsControlFile's FSCTL_DISMOUNT_VOLUME.
 *
 * Driven through the thunk table against a real guest window
 * (xbox_MemoryLayoutInit on tools/conformance/test.xbe), as
 * tests/kernel_objects is: the IRPs come from the guest heap. A "driver" is a
 * DRIVER_OBJECT in guest memory whose dispatch and completion routines are
 * host functions behind fake guest addresses (recomp_lookup). Every call
 * checks esp afterwards (a wrong argument count is a stack leak in the
 * caller: IoAllocateIrp popped 8 bytes for its one argument).
 *
 * The layout checked is the one kernel_bridge.c states (IRP header from
 * nxdk's xboxkrnl.h, IO_STACK_LOCATION from Cxbx-Reloaded + NT x86): the
 * test pins it, it does not prove it against a console.
 *
 * With argv[2] == "off" (RECOMP_IRP_DISPATCH=0) only the A/B switch is
 * checked: IoBuildAsynchronousFsdRequest answers NULL, as before XBA-138.
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

#define DISPATCH_VA 0x00007100u
#define COMPL_VA    0x00007200u

#define IRP_SIZE    0x68u
#define SL_SIZE     0x24u

static int failures;
static char d[256];
static uint32_t B;

#define G32(va) (*(volatile uint32_t *)XBOX_TO_NATIVE(va))
#define G16(va) (*(volatile uint16_t *)XBOX_TO_NATIVE(va))
#define G8(va)  (*(volatile uint8_t  *)XBOX_TO_NATIVE(va))

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-70s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

enum {
    O_THUNK = 0x0000, O_STACK = 0x2000, O_OUT = 0x3000, O_IOSB = 0x3100,
    O_OFF = 0x3200, O_EVENT = 0x3300, O_TIMEOUT = 0x3400, O_DRIVER = 0x3500,
    O_IRPBUF = 0x3600, O_BUF = 0x4000, O_TIB = 0x6000, O_KT = 0x6100,
};
#define V(o) (B + (uint32_t)(o))

enum {
    S_ALLOC, S_BUILD, S_CREATEDEV, S_FREE, S_INIT, S_CALL, S_COMPLETE,
    S_CURTHREAD, S_INITEVENT, S_WAIT1, S_FSCTL, S_COUNT
};
static const uint32_t ord[S_COUNT] = { 59, 60, 65, 72, 73, 86, 87, 104, 108, 159, 200 };

static uint32_t call_regs(int slot, const uint32_t *args, int n, uint32_t ecx, uint32_t edx)
{
    uint32_t sp = V(O_STACK) + 0x800;
    int i;
    G32(sp) = 0xBEEF0000u | (uint32_t)slot;
    for (i = 0; i < n; i++)
        G32(sp + 4 + 4 * (uint32_t)i) = args[i];
    g_esp = sp;
    g_ecx = ecx;
    g_edx = edx;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(G32(V(O_THUNK) + 4 * (uint32_t)slot))();
    return g_eax;
}
#define CALL(slot, ...) do { uint32_t a_[] = { __VA_ARGS__ }; \
    call_regs(slot, a_, (int)(sizeof a_ / sizeof a_[0]), 0, 0); } while (0)
static int esp_ok(int n) { return g_esp == V(O_STACK) + 0x800 + 4 + 4 * (uint32_t)n; }

/* fastcall: ecx, edx; nothing on the stack but the return address. */
static uint32_t fastcall2(int slot, uint32_t ecx, uint32_t edx)
{
    return call_regs(slot, NULL, 0, ecx, edx);
}

/* ── The title's driver ── */
static int dispatches, completions;
static uint32_t disp_dev, disp_irp, disp_sl, disp_sl_dev, disp_major, disp_loc;
static uint32_t compl_dev, compl_irp, compl_ctx, compl_status;
static uint32_t disp_status = 0, disp_info = 0x200, compl_answer = 0;
static int disp_completes = 1;

/* NTSTATUS Dispatch(DeviceObject, Irp), stdcall: what a driver does with a
 * read -- fills IoStatus and completes the IRP through IofCompleteRequest. */
static void dispatch_proc(void)
{
    uint32_t esp = g_esp, ret;
    disp_dev = G32(esp + 4);
    disp_irp = G32(esp + 8);
    disp_loc = G8(disp_irp + 0x19);
    disp_sl = G32(disp_irp + 0x5C);
    disp_sl_dev = G32(disp_sl + 0x14);
    disp_major = G8(disp_sl);
    dispatches++;
    G32(disp_irp + 0x10) = disp_status;
    G32(disp_irp + 0x14) = disp_info;
    if (disp_completes) {
        /* The driver's own call, on the driver's stack below ours. */
        uint32_t sp = esp - 0x100;
        G32(sp) = 0xC0FFEEu;
        g_esp = sp;
        g_ecx = disp_irp;
        g_edx = 0;
        recomp_lookup_kernel(G32(V(O_THUNK) + 4 * S_COMPLETE))();
    }
    ret = disp_status;
    g_esp = esp + 12;
    g_eax = ret;
}

/* NTSTATUS Completion(DeviceObject, Irp, Context), stdcall. */
static void compl_proc(void)
{
    compl_dev = G32(g_esp + 4);
    compl_irp = G32(g_esp + 8);
    compl_ctx = G32(g_esp + 12);
    compl_status = G32(compl_irp + 0x10);
    completions++;
    g_esp += 16;
    g_eax = compl_answer;
}

recomp_func_t recomp_lookup(uint32_t va);
recomp_func_t recomp_lookup(uint32_t va)
{
    return va == DISPATCH_VA ? dispatch_proc : va == COMPL_VA ? compl_proc : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va);
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

static void become_thread(void)
{
    memset(XBOX_TO_NATIVE(V(O_TIB)), 0, 0x40);
    memset(XBOX_TO_NATIVE(V(O_KT)), 0, 0x200);
    G32(V(O_TIB)) = 0xFFFFFFFFu;
    G32(V(O_TIB) + 0x28) = V(O_KT);
    xbox_InitGuestKthread(V(O_KT), 0, V(O_STACK) + 0x1000, V(O_STACK));
    g_fs_base = V(O_TIB);
}

/* IoSetCompletionRoutine, the XDK's inline: on the next stack location. */
static void set_completion(uint32_t irp, uint32_t ctx, uint8_t control)
{
    uint32_t next = G32(irp + 0x5C) - SL_SIZE;
    G32(next + 0x1C) = COMPL_VA;
    G32(next + 0x20) = ctx;
    G8(next + 0x03) = control;
}

static int event_signalled(uint32_t ev)
{
    CALL(S_WAIT1, ev, 0, 0, 0, V(O_TIMEOUT));
    return g_eax == 0;
}

static void test_alloc_init(void)
{
    uint32_t irp;

    CALL(S_ALLOC, 3);
    irp = g_eax;
    snprintf(d, sizeof d, "irp 0x%08X esp %+d", irp, (int)(g_esp - (V(O_STACK) + 0x800 + 8)));
    check(irp != 0 && esp_ok(1), "IoAllocateIrp(3) answers an IRP and pops one argument", d);
    if (!irp)
        return;
    snprintf(d, sizeof d, "type %u size 0x%X count %u loc %u cur 0x%08X list %08X/%08X",
             G16(irp), G16(irp + 2), G8(irp + 0x18), G8(irp + 0x19), G32(irp + 0x5C),
             G32(irp + 8), G32(irp + 12));
    check(G16(irp) == 6 && G16(irp + 2) == IRP_SIZE + 3 * SL_SIZE
          && G8(irp + 0x18) == 3 && G8(irp + 0x19) == 4
          && G32(irp + 0x5C) == irp + IRP_SIZE + 3 * SL_SIZE
          && G32(irp + 8) == irp + 8 && G32(irp + 12) == irp + 8
          && G32(irp + 0x10) == 0 && G32(irp + 0x1C) == 0,
          "initialised: IO_TYPE_IRP, Size, StackCount, CurrentLocation past the end", d);
    CALL(S_FREE, irp);
    check(esp_ok(1), "IoFreeIrp (1 argument)", NULL);

    memset(XBOX_TO_NATIVE(V(O_IRPBUF)), 0xCC, 0x200);
    CALL(S_INIT, V(O_IRPBUF), IRP_SIZE + 2 * SL_SIZE, 2);
    irp = V(O_IRPBUF);
    check(esp_ok(3) && G16(irp) == 6 && G16(irp + 2) == IRP_SIZE + 2 * SL_SIZE
          && G8(irp + 0x18) == 2 && G8(irp + 0x19) == 3
          && G32(irp + 0x5C) == irp + IRP_SIZE + 2 * SL_SIZE
          && G32(irp + 0x30) == 0 && G8(irp + IRP_SIZE + 2 * SL_SIZE) == 0xCC,
          "IoInitializeIrp on a caller's buffer: same fields, nothing past PacketSize", NULL);
}

static uint32_t make_device(void)
{
    uint32_t dev;

    memset(XBOX_TO_NATIVE(V(O_DRIVER)), 0, 0x48);
    G32(V(O_DRIVER) + 0x0C + 4 * 2) = DISPATCH_VA;   /* MajorFunction[IRP_MJ_READ] */
    G32(V(O_OUT)) = 0;
    CALL(S_CREATEDEV, V(O_DRIVER), 0x10, 0, 0x07 /* FILE_DEVICE_DISK */, 0, V(O_OUT));
    dev = G32(V(O_OUT));
    snprintf(d, sizeof d, "status 0x%08X dev 0x%08X StackSize byte %u, 0x1D %u",
             g_eax, dev, dev ? G8(dev + 0x1E) : 0, dev ? G8(dev + 0x1D) : 0);
    check(g_eax == 0 && dev && esp_ok(6) && G8(dev + 0x1E) == 1 && G8(dev + 0x1D) == 0
          && G32(dev + 8) == V(O_DRIVER),
          "IoCreateDevice: StackSize 1 at 0x1E (nxdk), DriverObject kept", d);
    return dev;
}

static void test_build_and_call(uint32_t dev)
{
    uint32_t irp, sl, thread, r;

    G8(dev + 0x1E) = 2;                       /* a device two drivers deep */
    *(uint64_t *)XBOX_TO_NATIVE(V(O_OFF)) = 0x0000123456789A00ull;
    CALL(S_CURTHREAD, 0);
    thread = g_eax;

    memset(XBOX_TO_NATIVE(V(O_IOSB)), 0xEE, 8);
    CALL(S_BUILD, 2 /* IRP_MJ_READ */, dev, V(O_BUF), 0x200, V(O_OFF), V(O_IOSB));
    irp = g_eax;
    check(irp != 0 && esp_ok(6), "IoBuildAsynchronousFsdRequest answers an IRP (6 arguments)", NULL);
    if (!irp)
        return;
    sl = G32(irp + 0x5C) - SL_SIZE;
    snprintf(d, sizeof d, "count %u loc %u major %u len 0x%X off %08X%08X buf 0x%08X iosb 0x%08X thread 0x%08X/0x%08X",
             G8(irp + 0x18), G8(irp + 0x19), G8(sl), G32(sl + 4), G32(sl + 0x10), G32(sl + 0x0C),
             G32(irp + 0x30), G32(irp + 0x1C), G32(irp + 0x50), thread);
    check(G8(irp + 0x18) == 2 && G8(irp + 0x19) == 3 && G8(sl) == 2
          && G32(sl + 4) == 0x200 && G32(sl + 0x0C) == 0x56789A00u && G32(sl + 0x10) == 0x1234u
          && G32(irp + 0x30) == V(O_BUF) && G32(irp + 0x1C) == V(O_IOSB)
          && G32(irp + 0x50) == thread && thread != 0,
          "next stack location: IRP_MJ_READ, Length, ByteOffset; UserBuffer, UserIosb, Thread", d);

    /* Completion routine on the driver's location; it lets completion go on. */
    CALL(S_INITEVENT, V(O_EVENT), 0 /* NotificationEvent */, 0);
    G32(irp + 0x20) = V(O_EVENT);             /* UserEvent */
    set_completion(irp, 0x5150, 0xE0);
    dispatches = completions = 0;
    disp_status = 0; disp_info = 0x200; compl_answer = 0; disp_completes = 1;
    r = fastcall2(S_CALL, dev, irp);
    snprintf(d, sizeof d, "r 0x%08X dispatches %d dev 0x%08X irp 0x%08X loc %u sl 0x%08X/0x%08X sl.dev 0x%08X major %u",
             r, dispatches, disp_dev, disp_irp, disp_loc, disp_sl, sl, disp_sl_dev, disp_major);
    check(r == 0 && esp_ok(0) && dispatches == 1 && disp_dev == dev && disp_irp == irp
          && disp_loc == 2 && disp_sl == sl && disp_sl_dev == dev && disp_major == 2,
          "IofCallDriver: the driver's MajorFunction[READ](dev, irp) on its location", d);
    snprintf(d, sizeof d, "completions %d dev 0x%08X irp 0x%08X ctx 0x%X status 0x%X iosb %08X/%X",
             completions, compl_dev, compl_irp, compl_ctx, compl_status,
             G32(V(O_IOSB)), G32(V(O_IOSB) + 4));
    check(completions == 1 && compl_dev == 0 && compl_irp == irp && compl_ctx == 0x5150
          && compl_status == 0 && G32(V(O_IOSB)) == 0 && G32(V(O_IOSB) + 4) == 0x200,
          "IofCompleteRequest: completion routine (top: no device), then UserIosb", d);
    check(event_signalled(V(O_EVENT)), "and UserEvent is set", NULL);
    CALL(S_FREE, irp);

    /* STATUS_MORE_PROCESSING_REQUIRED: the routine keeps the IRP. */
    CALL(S_BUILD, 2, dev, V(O_BUF), 0x10, 0, V(O_IOSB));
    irp = g_eax;
    sl = G32(irp + 0x5C) - SL_SIZE;
    check(irp && G32(sl + 0x0C) == 0 && G32(sl + 0x10) == 0, "no StartingOffset: ByteOffset 0", NULL);
    memset(XBOX_TO_NATIVE(V(O_IOSB)), 0xEE, 8);
    set_completion(irp, 7, 0xE0);
    completions = 0; compl_answer = 0xC0000016u;
    fastcall2(S_CALL, dev, irp);
    check(completions == 1 && G32(V(O_IOSB)) == 0xEEEEEEEEu,
          "MORE_PROCESSING_REQUIRED from the routine: UserIosb left alone", NULL);
    compl_answer = 0;
    CALL(S_FREE, irp);

    /* A major the driver has no dispatch for: completed with an error. */
    CALL(S_BUILD, 3 /* IRP_MJ_WRITE */, dev, V(O_BUF), 0x10, 0, V(O_IOSB));
    irp = g_eax;
    set_completion(irp, 9, 0x80 /* on error only */);
    dispatches = completions = 0;
    r = fastcall2(S_CALL, dev, irp);
    snprintf(d, sizeof d, "r 0x%08X dispatches %d completions %d status 0x%X iosb 0x%08X",
             r, dispatches, completions, compl_status, G32(V(O_IOSB)));
    check(r == 0xC0000010u && dispatches == 0 && completions == 1
          && compl_status == 0xC0000010u && G32(V(O_IOSB)) == 0xC0000010u,
          "no dispatch: STATUS_INVALID_DEVICE_REQUEST, through the completion", d);
    CALL(S_FREE, irp);

    /* A success with an error-only routine does not call it. */
    CALL(S_BUILD, 2, dev, V(O_BUF), 0x10, 0, V(O_IOSB));
    irp = g_eax;
    set_completion(irp, 9, 0x80);
    completions = 0;
    fastcall2(S_CALL, dev, irp);
    check(completions == 0 && G32(V(O_IOSB)) == 0, "success with an error-only routine: not called", NULL);
    CALL(S_FREE, irp);

    CALL(S_BUILD, 2, 0, V(O_BUF), 0x10, 0, V(O_IOSB));
    check(g_eax == 0 && esp_ok(6), "no device: NULL", NULL);
}

static void test_fsctl(void)
{
    memset(XBOX_TO_NATIVE(V(O_IOSB)), 0xEE, 8);
    CALL(S_FSCTL, 0, 0, 0, 0, V(O_IOSB), 0x00090020u, 0, 0, 0, 0);
    check(g_eax == 0 && esp_ok(10) && G32(V(O_IOSB)) == 0,
          "NtFsControlFile(FSCTL_DISMOUNT_VOLUME): success (10 arguments)", NULL);
    CALL(S_FSCTL, 0, 0, 0, 0, V(O_IOSB), 0x00090000u, 0, 0, 0, 0);
    check(g_eax == 0xC00000BBu && G32(V(O_IOSB)) == 0xC00000BBu,
          "another FSCTL: STATUS_NOT_SUPPORTED", NULL);
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
    become_thread();
    *(int64_t *)XBOX_TO_NATIVE(V(O_TIMEOUT)) = 0;

    if (argc > 2 && !strcmp(argv[2], "off")) {
        uint32_t dev = make_device();
        CALL(S_BUILD, 2, dev, V(O_BUF), 0x10, 0, V(O_IOSB));
        check(g_eax == 0 && esp_ok(6), "RECOMP_IRP_DISPATCH=0: IoBuildAsynchronousFsdRequest answers NULL", NULL);
    } else {
        test_alloc_init();
        test_build_and_call(make_device());
        test_fsctl();
    }

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
