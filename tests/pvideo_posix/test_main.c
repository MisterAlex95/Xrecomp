/* The PVIDEO overlay on POSIX hosts (src/video/pvideo_posix.c).
 *
 * The register block is plain memory here, as it is in guest memory on a
 * POSIX host: nv2a_hook_init is pointed at it, and guest physical memory is
 * a buffer (xbox_PvideoTestMemory). Headless, so the window takes nothing;
 * the composition is read back with xbox_PvideoPicture.
 *
 *   - vblank_consumes_bank: one vblank's scan-out (xbox_PvideoStep) draws a
 *     pending YUY2 bank and clears its BUFFER bit, and only its bit.
 *   - stop_without_vblank: STOP empties BUFFER from the ack pass
 *     (xbox_Nv2aPvideoStop) with no vblank at all, which is what
 *     D3DDevice_EnableOverlay(FALSE) spins on; the next scan-out sees the
 *     overlay went down.
 *   - flip_under_overlay: a frame presented while the overlay is up becomes
 *     the picture under it; the colour key lets the film through only where
 *     that frame holds the key.
 *   - bounds: a bank outside guest memory is not read, but handed back.
 * Also prints what composing a full 640x480 bank costs on this host. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "nv2a/nv2a_mmio_hook.h"
#include "nv2a/nv2a_regs.h"
#include "video/pvideo_posix.h"
#include "video/video_player.h"
#include "video/pvideo_scanout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int s_failures;

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); s_failures++; } } while (0)

#define FW 640u
#define FH 480u
#define BANK_PHYS 0x1000u
#define BANK_BYTES (FW * 2u * FH)

static volatile uint32_t s_block[0x1000 / 4];   /* the PVIDEO registers */
static uint8_t s_phys[BANK_PHYS + 2u * BANK_BYTES];
static uint32_t s_frame[FW * FH];

#define REG(r) s_block[(r) / 4]
#define BREG(r, bank) s_block[((r) + (bank) * 4u) / 4]

/* A full-screen 640x480 YUY2 bank at s_phys + phys: white (Y 235) on rows
 * above `split`, black (Y 16) below (registers only when it would not fit). */
static void setup_bank(unsigned bank, uint32_t phys, unsigned split, int keyed)
{
    unsigned x, y;
    for (y = 0; y < FH && phys + BANK_BYTES <= sizeof(s_phys); y++)
        for (x = 0; x < FW; x += 2) {
            uint8_t *p = s_phys + phys + (size_t)y * FW * 2u + x * 2u;
            p[0] = p[2] = y < split ? 235 : 16;
            p[1] = p[3] = 128;
        }
    BREG(NV_PVIDEO_BASE, bank) = 0;
    BREG(NV_PVIDEO_OFFSET, bank) = phys;
    BREG(NV_PVIDEO_LIMIT, bank) = 0x03FFFFFFu;
    BREG(NV_PVIDEO_FORMAT, bank) = (NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8 << 16)
                                 | (keyed ? NV_PVIDEO_FORMAT_DISPLAY : 0) | (FW * 2u);
    BREG(NV_PVIDEO_SIZE_IN, bank) = (FH << 16) | FW;
    BREG(NV_PVIDEO_POINT_IN, bank) = 0;
    BREG(NV_PVIDEO_DS_DX, bank) = NV_PVIDEO_DIN_DOUT_UNITY;
    BREG(NV_PVIDEO_DT_DY, bank) = NV_PVIDEO_DIN_DOUT_UNITY;
    BREG(NV_PVIDEO_POINT_OUT, bank) = 0;
    BREG(NV_PVIDEO_SIZE_OUT, bank) = (FH << 16) | FW;
}

static uint32_t pic(unsigned x, unsigned y)
{
    return xbox_PvideoPicture(NULL)[y * PVIDEO_W + x];
}

static void vblank_consumes_bank(void)
{
    int up = 0;

    setup_bank(0, BANK_PHYS, 100, 0);
    REG(NV_PVIDEO_BUFFER) = NV_PVIDEO_BUFFER_0_USE;
    CHECK(xbox_PvideoStep() == 1, "a pending bank was not shown");
    CHECK(REG(NV_PVIDEO_BUFFER) == 0, "BUFFER %08X after the vblank", REG(NV_PVIDEO_BUFFER));
    (void)xbox_PvideoPicture(&up);
    CHECK(up, "the overlay is not up after a frame");
    CHECK(pic(5, 5) == 0xFFFFFFFFu && pic(600, 99) == 0xFFFFFFFFu,
          "white rows came out %08X %08X", pic(5, 5), pic(600, 99));
    CHECK(pic(5, 100) == 0xFF000000u && pic(639, 479) == 0xFF000000u,
          "black rows came out %08X %08X", pic(5, 100), pic(639, 479));

    /* Nothing pending: nothing new is shown, the picture stays. */
    CHECK(xbox_PvideoStep() == 0, "a vblank with nothing pending showed a frame");
    (void)xbox_PvideoPicture(&up);
    CHECK(up && pic(5, 5) == 0xFFFFFFFFu, "the last frame did not stay on screen");

    /* Bank 1 pending while the title has just set bank 0 again: clearing
     * bank 1 must leave bank 0's bit. Bank 0 is consumed in the same pass
     * here (both are pending), so set it after by hand to check the mask. */
    setup_bank(1, BANK_PHYS + BANK_BYTES, 300, 0);
    REG(NV_PVIDEO_BUFFER) = NV_PVIDEO_BUFFER_1_USE;
    CHECK(xbox_PvideoStep() == 1, "bank 1 was not shown");
    CHECK(REG(NV_PVIDEO_BUFFER) == 0, "BUFFER %08X after bank 1", REG(NV_PVIDEO_BUFFER));
    CHECK(pic(5, 299) == 0xFFFFFFFFu && pic(5, 300) == 0xFF000000u,
          "bank 1's picture came out %08X %08X", pic(5, 299), pic(5, 300));
    REG(NV_PVIDEO_BUFFER) = NV_PVIDEO_BUFFER_0_USE | NV_PVIDEO_BUFFER_1_USE;
    nv2a_hook_pvideo_consume(1);
    CHECK(REG(NV_PVIDEO_BUFFER) == NV_PVIDEO_BUFFER_0_USE,
          "consuming bank 1 left BUFFER %08X", REG(NV_PVIDEO_BUFFER));
    REG(NV_PVIDEO_BUFFER) = 0;
}

static void stop_without_vblank(void)
{
    uint32_t stops = xbox_Nv2aPvideoStops();
    int up = 1;

    REG(NV_PVIDEO_BUFFER) = NV_PVIDEO_BUFFER_0_USE | NV_PVIDEO_BUFFER_1_USE;
    CHECK(!xbox_Nv2aPvideoStop(s_block), "acted without STOP");
    CHECK(REG(NV_PVIDEO_BUFFER) != 0, "BUFFER cleared without STOP");
    REG(NV_PVIDEO_STOP) = 1;
    CHECK(xbox_Nv2aPvideoStop(s_block), "STOP not answered");
    CHECK(REG(NV_PVIDEO_BUFFER) == 0, "BUFFER %08X after STOP", REG(NV_PVIDEO_BUFFER));
    CHECK(REG(NV_PVIDEO_STOP) == 0, "STOP still set");
    CHECK(xbox_Nv2aPvideoStops() == stops + 1, "stop count %u, was %u",
          xbox_Nv2aPvideoStops(), stops);
    CHECK(xbox_PvideoStep() == 0, "a frame after STOP");
    (void)xbox_PvideoPicture(&up);
    CHECK(!up, "the overlay is still up after STOP");
    /* Down: a flip is the window's again. */
    CHECK(xbox_FramebufferWindowPresentHost(s_frame, FW, FH, FW * 4u, XBOX_FB_HOST_BGRX8888) == 0,
          "headless window took a frame");
}

/* EnableOverlay(FALSE) with nothing pending: no STOP, bank 0 released
 * (SIZE_IN = 0xFFFFFFFF). The next flip is the window's, not composed. */
static void release_without_stop(void)
{
    int up = 0;

    setup_bank(0, BANK_PHYS, FH, 0);
    REG(NV_PVIDEO_BUFFER) = NV_PVIDEO_BUFFER_0_USE;
    CHECK(xbox_PvideoStep() == 1, "the bank was not shown");
    BREG(NV_PVIDEO_SIZE_IN, 0) = 0xFFFFFFFFu;
    memset(s_frame, 0x33, sizeof(s_frame));
    (void)xbox_FramebufferWindowPresentHost(s_frame, FW, FH, FW * 4u, XBOX_FB_HOST_BGRX8888);
    (void)xbox_PvideoPicture(&up);
    CHECK(!up, "the overlay is still up after the release");
    CHECK(pic(5, 5) == 0xFFFFFFFFu, "the flip after the release was composed: %08X", pic(5, 5));
    /* The same seen by the vblank instead of a flip. */
    setup_bank(0, BANK_PHYS, FH, 0);
    REG(NV_PVIDEO_BUFFER) = NV_PVIDEO_BUFFER_0_USE;
    CHECK(xbox_PvideoStep() == 1, "the bank was not shown again");
    BREG(NV_PVIDEO_SIZE_IN, 0) = 0xFFFFFFFFu;
    CHECK(xbox_PvideoStep() == 0, "a frame after the release");
    (void)xbox_PvideoPicture(&up);
    CHECK(!up, "the vblank did not take the released overlay down");
}

static void flip_under_overlay(void)
{
    const uint32_t key = 0x00FF00FFu;
    unsigned x, y;
    int up = 0;

    setup_bank(0, BANK_PHYS, FH, 1);                  /* all white, keyed */
    REG(NV_PVIDEO_COLOR_KEY) = key;
    REG(NV_PVIDEO_BUFFER) = NV_PVIDEO_BUFFER_0_USE;
    /* No frame to put under it (headless, never flipped): the screen is
     * taken to hold the key, so the film shows everywhere. */
    CHECK(xbox_PvideoStep() == 1, "the keyed bank was not shown");
    CHECK(pic(320, 240) == 0xFFFFFFFFu, "keyed film over nothing came out %08X", pic(320, 240));

    /* The title flips: left half the key, right half grey. */
    for (y = 0; y < FH; y++)
        for (x = 0; x < FW; x++)
            s_frame[y * FW + x] = x < FW / 2 ? key : 0x00808080u;
    (void)xbox_FramebufferWindowPresentHost(s_frame, FW, FH, FW * 4u, XBOX_FB_HOST_BGRX8888);
    (void)xbox_PvideoPicture(&up);
    CHECK(up, "the flip took the overlay down");
    CHECK(pic(10, 10) == 0xFFFFFFFFu && pic(319, 479) == 0xFFFFFFFFu,
          "the film did not show through the key: %08X %08X", pic(10, 10), pic(319, 479));
    CHECK(pic(320, 10) == 0xFF808080u && pic(639, 479) == 0xFF808080u,
          "the frame was not kept outside the key: %08X %08X", pic(320, 10), pic(639, 479));

    /* A host frame at twice the size (a GPU back end's own) is resampled. */
    {
        static uint32_t big[2 * FW * 2 * FH];
        for (y = 0; y < 2 * FH; y++)
            for (x = 0; x < 2 * FW; x++)
                big[y * 2 * FW + x] = y < FH ? key : 0x00404040u;
        (void)xbox_FramebufferWindowPresentHost(big, 2 * FW, 2 * FH, 2 * FW * 4u,
                                                XBOX_FB_HOST_BGRX8888);
        CHECK(pic(600, 239) == 0xFFFFFFFFu && pic(5, 240) == 0xFF404040u,
              "a 1280x960 frame under the overlay came out %08X %08X", pic(600, 239), pic(5, 240));
    }
    REG(NV_PVIDEO_STOP) = 1;
    (void)xbox_Nv2aPvideoStop(s_block);
    (void)xbox_PvideoStep();
}

static void bounds(void)
{
    int up = 1;

    setup_bank(0, (uint32_t)sizeof(s_phys) - BANK_BYTES / 2, FH, 0);   /* runs off the end */
    REG(NV_PVIDEO_BUFFER) = NV_PVIDEO_BUFFER_0_USE;
    CHECK(xbox_PvideoStep() == 0, "a bank outside guest memory was shown");
    CHECK(REG(NV_PVIDEO_BUFFER) == 0, "a bank outside guest memory was not handed back");
    (void)xbox_PvideoPicture(&up);
    CHECK(!up, "the overlay came up from a bank it could not read");

    /* A format it cannot draw: handed back too, so the title does not wait
     * forever, and nothing shown. */
    setup_bank(0, BANK_PHYS, FH, 0);
    BREG(NV_PVIDEO_FORMAT, 0) &= ~NV_PVIDEO_FORMAT_COLOR;
    REG(NV_PVIDEO_BUFFER) = NV_PVIDEO_BUFFER_0_USE;
    CHECK(xbox_PvideoStep() == 0, "a non-YUY2 bank was shown");
    CHECK(REG(NV_PVIDEO_BUFFER) == 0, "a refused bank was not handed back");
}

/* pvideo_scanout's 1:1 path against the per-pixel formula, on noise, keyed
 * and placed so the output is cropped at an odd column. */
static void fast_path_matches(void)
{
    static uint32_t regs[0x1000 / 4], dst[64 * 16], ref[64 * 16];
    static uint8_t img[40 * 2 * 12];
    const uint32_t key = 0x00123456u;
    unsigned i, x, y, seed = 12345;

    for (i = 0; i < sizeof(img); i++) {
        seed = seed * 1103515245u + 12345u;
        img[i] = (uint8_t)(seed >> 16);
    }
    regs[NV_PVIDEO_BUFFER / 4] = NV_PVIDEO_BUFFER_0_USE;
    regs[NV_PVIDEO_FORMAT / 4] = (NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8 << 16)
                               | NV_PVIDEO_FORMAT_DISPLAY | 80u;
    regs[NV_PVIDEO_SIZE_IN / 4] = (12u << 16) | 40u;
    regs[NV_PVIDEO_SIZE_OUT / 4] = (12u << 16) | 40u;
    regs[NV_PVIDEO_POINT_OUT / 4] = (7u << 16) | 33u;      /* 31 columns fit */
    regs[NV_PVIDEO_DS_DX / 4] = NV_PVIDEO_DIN_DOUT_UNITY;
    regs[NV_PVIDEO_DT_DY / 4] = NV_PVIDEO_DIN_DOUT_UNITY;
    regs[NV_PVIDEO_LIMIT / 4] = 0xFFFFFFFFu;
    regs[NV_PVIDEO_COLOR_KEY / 4] = key;
    for (i = 0; i < 64 * 16; i++)
        dst[i] = ref[i] = (i % 3) ? key : 0x00ABCDEFu;
    CHECK(pvideo_scanout(regs, img, sizeof(img), dst, 64, 16, 0), "1:1 bank refused");
    for (y = 7; y < 16; y++)
        for (x = 33; x < 64; x++) {
            const uint8_t *pair = img + (y - 7) * 80u + ((x - 33) & ~1u) * 2u;
            int c = pair[((x - 33) & 1u) * 2] - 16, u = pair[1] - 128, v = pair[3] - 128;
            uint32_t *p = &ref[y * 64 + x];
            if ((*p & 0xffffffu) != key)
                continue;
            *p = 0xff000000u | (pvideo_clamp((298*c + 409*v + 128) >> 8) << 16) |
                 (pvideo_clamp((298*c - 100*u - 208*v + 128) >> 8) << 8) |
                 pvideo_clamp((298*c + 516*u + 128) >> 8);
        }
    CHECK(memcmp(dst, ref, sizeof(dst)) == 0, "the 1:1 path differs from the per-pixel formula");
}

static void cost(void)
{
    struct timespec t0, t1;
    unsigned i, n = 120;
    double ms;

    setup_bank(0, BANK_PHYS, 240, 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < n; i++) {
        REG(NV_PVIDEO_BUFFER) = NV_PVIDEO_BUFFER_0_USE;
        (void)xbox_PvideoStep();
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / n;
    printf("  scan-out of a 640x480 bank (copy, compose, publish): %.3f ms/frame\n", ms);
    REG(NV_PVIDEO_STOP) = 1;
    (void)xbox_Nv2aPvideoStop(s_block);
    (void)xbox_PvideoStep();
}

int main(void)
{
    /* The block where nv2a_hook_init expects the guest's 0xFD008000. */
    nv2a_hook_init((ptrdiff_t)((uintptr_t)s_block - 0xFD008000u));
    xbox_PvideoTestMemory(s_phys, sizeof(s_phys));

    vblank_consumes_bank();
    stop_without_vblank();
    release_without_stop();
    flip_under_overlay();
    bounds();
    fast_path_matches();
    cost();

    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    puts("PASS: banks consumed at vblank, STOP answered without one, flips composed "
         "under the overlay, out-of-range banks not read.");
    return 0;
}
