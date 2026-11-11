/*
 * Burnout 3: Takedown (Xbox, 2004, PAL Fr/De/It), title ID 4541005B -- the
 * title profile. XDK 5849, statically linked D3D8LTCG / DSOUND / XMV / XPP.
 *
 * tools.prepare copies this folder into the title project's src/title/, and
 * the template's main.c calls the two hooks below. Addresses, measurements
 * and runs: NOTES.md here.
 *
 * Switches (environment, or recomp.cfg):
 *   BO3_FENCE=mirror  A/B fallback: segment scan, no software-method hook, and
 *                     D3D's fence mirrored as "GPU always caught up"
 *                     (xbox_Nv2aMirrorFence) instead of the semaphore watch.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#define SETENV_DEFAULT(k, v) setenv((k), (v), 0)
#else
#define SETENV_DEFAULT(k, v) do { if (!getenv(k)) _putenv(k "=" v); } while (0)
#endif

/* D3D8LTCG's device is a static in the D3D section; [0x0035FE48] points at it
 * (0x0035D9A0). device+0x2C: fences submitted, device+0x30: -> the GPU
 * semaphore (contiguous memory), device+0x1DCC: the KEVENT BlockOnTime
 * (sub_00351A70) sleeps on when the GPU is far behind. */
#define BO3_D3D_DEVICE_PTR   0x0035FE48u
#define BO3_D3D_FENCE_COUNT  0x2Cu
#define BO3_D3D_SEMAPHORE    0x30u
#define BO3_D3D_FENCE_EVENT  0x1DCCu

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void nv2a_pb_set_software_method_hook(int (*hook)(uint32_t param));
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern int  xbox_KeSetGuestEvent(uint32_t guest_va);
extern int  xbox_Nv2aMirrorFence(uint32_t device_ptr_va, uint32_t put_off, uint32_t get_ptr_off);

static uint32_t bo3_guest32(uint32_t va)
{
    return *(volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + va);
}

/* D3D software method 5 (NO_OPERATION with value 5): on the console the
 * PGRAPH interrupt handler answers it with KeSetEvent on the fence event.
 * BlockOnTime patches it into the pushbuffer at the fence it waits for when
 * the GPU is more than 0x2000 bytes behind, then waits for ever
 * (KeWaitForSingleObject, no timeout) at 0x00351BB4. D3D built that event
 * itself (no KeInitializeEvent import), hence RECOMP_TITLE_KEVENTS. */
static int bo3_software_method(uint32_t param)
{
    if ((param & 0x1Fu) == 5u) {
        uint32_t dev = bo3_guest32(BO3_D3D_DEVICE_PTR);
        if (dev)
            (void)xbox_KeSetGuestEvent(dev + BO3_D3D_FENCE_EVENT);
        return 1;
    }
    return 0;       /* 8 (PGRAPH RDI, ~80 a second in a race), 9: not serviced */
}

static int bo3_mirror;

void xba_title_configure(void)
{
    const char *fence = getenv("BO3_FENCE");

    bo3_mirror = fence && !strcmp(fence, "mirror");
    /* XDK D3D: the pushbuffer executor draws. The DMA walk keeps DMA_GET and
     * the 0x0310 latch (PGRAPH 0x400B10) honest, which BlockOnTime and its
     * far-behind probe (sub_00351A00) read; under the segment scan the latch
     * never moves and the front end stalls at _FEMain.xmv. */
    SETENV_DEFAULT("RECOMP_PB_EXEC", "1");
    if (!bo3_mirror)
        SETENV_DEFAULT("RECOMP_PB_WALK", "dma");
    /* DirectSound: emulated APU + AC'97 codec ready (without it, a hang after
     * the USB start, no flip). */
    SETENV_DEFAULT("RECOMP_AC97_READY", "1");
    /* 60 Hz vblanks: the XMV player's clock and D3D's swap counters (without
     * them, one flip). */
    SETENV_DEFAULT("RECOMP_VBLANK", "paced");
    /* XPP locks only by raising IRQL (the same USB stack as Fable's), and the
     * title sends a rumble report every frame: the one-CPU DISPATCH gate. */
    SETENV_DEFAULT("RECOMP_DISPATCH_GATE", "1");
    SETENV_DEFAULT("RECOMP_USB", "1");
    SETENV_DEFAULT("RECOMP_KEYBOARD", "1");
    if (!bo3_mirror) {
        SETENV_DEFAULT("RECOMP_TITLE_KEVENTS", "1");
        nv2a_pb_set_software_method_hook(bo3_software_method);
    } else {
        xbox_Nv2aMirrorFence(BO3_D3D_DEVICE_PTR, BO3_D3D_FENCE_COUNT, BO3_D3D_SEMAPHORE);
    }
    /* The whole game is locked to the flip rate: at a 30 fps cap it runs at
     * half speed (98 -> 137 km/h takes 5 s instead of 2.5 s), uncapped it
     * flips 1,450-3,470 times a second. GL back end: cap at 60. Frame
     * generation at 2x on a 60 Hz panel caps at 30 and must stay off; the
     * shell sets it up before this runs, so title.json declares it
     * ("env_force": RECOMP_LSFG=0) rather than a setenv here. */
    SETENV_DEFAULT("RECOMP_GPU_FPS_CAP", "60");
#ifndef __ANDROID__
    /* The software rasteriser has no cap: the flip hold keeps it at 60. */
    if (!getenv("RECOMP_GPU"))
        SETENV_DEFAULT("RECOMP_FLIP_PACING", "1");
#endif
}

#ifndef _WIN32
/* Hand the executor D3D's GPU semaphore (device+0x30) once D3D has allocated
 * it, and again if a device reset moves it. Only a contiguous-window address. */
static void *semaphore_watch(void *unused)
{
    uint32_t last = 0;
    (void)unused;
    for (;;) {
        uint32_t dev = bo3_guest32(BO3_D3D_DEVICE_PTR);
        uint32_t sem = dev ? bo3_guest32(dev + BO3_D3D_SEMAPHORE) : 0;
        if (sem && sem != last && sem >= 0x80000000u && sem < 0x84000000u) {
            nv2a_pb_set_semaphore_target(sem);
            fprintf(stderr, "[D3D] device 0x%08X, GPU semaphore at 0x%08X\n", dev, sem);
            last = sem;
        }
        usleep(1000);
    }
    return NULL;
}

void xba_title_start(const char *game_dir, const char *save_dir)
{
    pthread_t t;
    (void)game_dir; (void)save_dir;
    if (!bo3_mirror && pthread_create(&t, NULL, semaphore_watch, NULL) == 0)
        pthread_detach(t);
}
#else
void xba_title_start(const char *game_dir, const char *save_dir) { (void)game_dir; (void)save_dir; }
#endif
