/*
 * XDK Direct3D 8 layer (profiles/xdk/d3d8): what the console's GPU and D3D's
 * interrupt handler do for D3D, which the recompiled title cannot do itself.
 *
 * D3D is linked into each title, so its statics sit at a different address in
 * every XBE: the title layer gives them as params (title.json "params",
 * tools.prepare writes xba_params.h). Without them only the layer's env
 * defaults apply.
 *
 *   XBA_D3D_DEVICE_PTR   the slot holding the device pointer (D3D's g_pDevice)
 *   XBA_D3D_DEVICE       the device itself (a static in D3D's section), for
 *                        the RunPushBuffer methods
 *   XBA_D3D_SW_METHODS   the software methods to service, bit n = method n:
 *                        0x20 the fence event (5), 0x7000 RunPushBuffer (0xC-0xE)
 *   XBA_D3D_DMA_WALK     1: walk the pushbuffer as the DMA engine does
 *                        (RECOMP_PB_WALK=dma)
 *
 * The offsets inside the device below were read on XDK 5849 (Burnout 3,
 * D3D8LTCG) and on Fable's D3D; check them before giving a title of another
 * build these params (docs/technical/d3d8ltcg-device-context.md).
 *
 * Switch: XBA_D3D_FENCE=mirror is the A/B fallback Burnout 3 was brought up
 * with: no software methods, no semaphore watch, no DMA walk, and D3D's fence
 * mirrored as "GPU always caught up" (xbox_Nv2aMirrorFence).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xba_params.h"

#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#define SETENV_DEFAULT(k, v) setenv((k), (v), 0)
#else
#define SETENV_DEFAULT(k, v) do { if (!getenv(k)) _putenv_s((k), (v)); } while (0)
#endif

#ifndef XBA_D3D_SW_METHODS
#define XBA_D3D_SW_METHODS 0u
#endif

#define D3D_FENCE_COUNT  0x2Cu      /* fences submitted */
#define D3D_SEMAPHORE    0x30u      /* -> the GPU semaphore (contiguous memory) */
#define D3D_RPB_PENDING  0x50u      /* RunPushBuffer calls waiting for the GPU */
#define D3D_RPB_NODES    0x264u     /* RunPushBuffer's node table, 20 bytes a node */
#define D3D_FENCE_EVENT  0x1DCCu    /* the KEVENT BlockOnTime sleeps on */

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void nv2a_pb_set_software_method_hook(int (*hook)(uint32_t param));
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern int  xbox_KeSetGuestEvent(uint32_t guest_va);
extern int  xbox_Nv2aMirrorFence(uint32_t device_ptr_va, uint32_t put_off, uint32_t get_ptr_off);

#define G32(a) (*(volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + (uint32_t)(a)))

static int d3d_mirror;

#ifdef XBA_D3D_DEVICE
/* ── RunPushBuffer's deferred return JUMP (software methods 0xC, 0xD, 0xE) ──
 *
 * D3DDevice_RunPushBuffer JUMPs from the ring into a recorded pushbuffer whose
 * last word is a JUMP back. If the GPU has not yet passed the pushbuffer's
 * previous use, that return JUMP cannot be patched yet: D3D puts a software
 * method before the entering JUMP and lets its interrupt handler patch it when
 * the GPU gets there. Unserviced, the walk follows the old return target and
 * runs earlier commands again (Fable: the visibility reports it waits on
 * in-world never come).
 *
 *   0xC  node = payload | 0x80000000 (in the ring), one pending run fewer
 *   0xD  node = device + 0x264 + payload * 20 (D3D's table), one pending run fewer
 *   0xE  node = payload | 0x80000000, recording: the count is left alone
 *
 * A node is { return target | flags, next, slot, fixups, base }. As the
 * handler does, for each node of the chain: *slot = JUMP(target); with flag
 * bit 29, copy each fixup { size, offset, bytes } to base + offset until size
 * 0xFFFFFFFF; next = -1; with bit 31, clear bit 30.
 *
 * Runs on the executor thread, while the main thread may be in RunPushBuffer
 * bumping the same pending count. */
static void d3d_run_pushbuffer_method(uint32_t type, uint32_t payload)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t node = type == 0xDu ? XBA_D3D_DEVICE + D3D_RPB_NODES + payload * 20u : payload | 0x80000000u;
    int guard;

    if (type != 0xEu) {
#if defined(__GNUC__) || defined(__clang__)
        __atomic_fetch_sub((uint32_t *)(mem + XBA_D3D_DEVICE + D3D_RPB_PENDING), 1u, __ATOMIC_SEQ_CST);
#else
        G32(XBA_D3D_DEVICE + D3D_RPB_PENDING) -= 1u;
#endif
    }
    for (guard = 0; guard < 4096; guard++) {
        uint32_t target = G32(node), next;

        G32(G32(node + 8)) = (target & 0x0FFFFFFFu) + 1u;      /* JUMP target */
        if (target & 0x20000000u) {
            uint32_t base = G32(node + 16), fix = G32(node + 12), size;
            while ((size = G32(fix)) != 0xFFFFFFFFu) {
                memcpy(mem + (uint32_t)(base + G32(fix + 4)), mem + fix + 8, size);
                fix += 8u + size;
            }
        }
        next = G32(node + 4);
        G32(node + 4) = 0xFFFFFFFFu;
        if (target & 0x80000000u)
            G32(node) = target & 0xBFFFFFFFu;
        if (next == 0 || next == 0xFFFFFFFFu)
            break;
        node = next;
    }
}
#endif

/* The software methods (NO_OPERATION with a value) the console's PGRAPH
 * interrupt handler answers; 1 when serviced. */
static int d3d_software_method(uint32_t param)
{
    uint32_t type = param & 0x1Fu;

    if (!(XBA_D3D_SW_METHODS & (1u << type)))
        return 0;
#ifdef XBA_D3D_DEVICE_PTR
    /* 5: KeSetEvent on the fence event. BlockOnTime patches it into the
     * pushbuffer at the fence it waits for when the GPU is more than 0x2000
     * bytes behind, then waits without a timeout. D3D built that event itself
     * (no KeInitializeEvent import), hence RECOMP_TITLE_KEVENTS. */
    if (type == 5u) {
        uint32_t dev = G32(XBA_D3D_DEVICE_PTR);
        if (dev)
            (void)xbox_KeSetGuestEvent(dev + D3D_FENCE_EVENT);
        return 1;
    }
#endif
#ifdef XBA_D3D_DEVICE
    if (type >= 0xCu && type <= 0xEu) {
        d3d_run_pushbuffer_method(type, param >> 5);
        return 1;
    }
#endif
    return 0;
}

void xba_d3d8_configure(void)
{
    const char *fence = getenv("XBA_D3D_FENCE");

    d3d_mirror = fence && !strcmp(fence, "mirror");
#ifndef XBA_D3D_DEVICE_PTR
    fprintf(stderr, "[D3D] profile layer xdk/d3d8: the title profile gives no XBA_D3D_DEVICE_PTR, so D3D's GPU "
                    "semaphore is not watched\n");
#endif
    if (d3d_mirror) {
#ifdef XBA_D3D_DEVICE_PTR
        xbox_Nv2aMirrorFence(XBA_D3D_DEVICE_PTR, D3D_FENCE_COUNT, D3D_SEMAPHORE);
#endif
        return;
    }
#if defined(XBA_D3D_DMA_WALK) && XBA_D3D_DMA_WALK
    /* D3D's fence wait reads DMA_GET and the 0x0310 latch (PGRAPH 0x400B10),
     * which only the DMA walk keeps honest. */
    SETENV_DEFAULT("RECOMP_PB_WALK", "dma");
#endif
    if (XBA_D3D_SW_METHODS & (1u << 5))
        SETENV_DEFAULT("RECOMP_TITLE_KEVENTS", "1");
    if (XBA_D3D_SW_METHODS)
        nv2a_pb_set_software_method_hook(d3d_software_method);
}

#if !defined(_WIN32) && defined(XBA_D3D_DEVICE_PTR)
/* Hand the executor D3D's GPU semaphore (device + 0x30) once D3D has
 * allocated it, and again if a device reset moves it. The slot does not
 * always hold the device (Fable), so only a semaphore in contiguous memory is
 * accepted: handing over a heap address made the executor write into game data. */
static void *d3d_semaphore_watch(void *unused)
{
    uint32_t last = 0;
    (void)unused;
    for (;;) {
        uint32_t dev = G32(XBA_D3D_DEVICE_PTR);
        uint32_t sem = dev ? G32(dev + D3D_SEMAPHORE) : 0;
        if (sem && sem != last && sem >= 0x80000000u && sem < 0x84000000u) {
            nv2a_pb_set_semaphore_target(sem);
            fprintf(stderr, "[D3D] device 0x%08X, GPU semaphore at 0x%08X\n", dev, sem);
            last = sem;
        }
        usleep(1000);
    }
    return NULL;
}

void xba_d3d8_start(const char *game_dir, const char *save_dir)
{
    pthread_t t;
    (void)game_dir; (void)save_dir;
    if (!d3d_mirror && pthread_create(&t, NULL, d3d_semaphore_watch, NULL) == 0)
        pthread_detach(t);
}
#else
void xba_d3d8_start(const char *game_dir, const char *save_dir) { (void)game_dir; (void)save_dir; }
#endif
