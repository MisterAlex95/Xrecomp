/* The runtime's XMV hook (src/video/xmv_hle.c, XBA-70) on guest memory:
 * the kernel-side note of the movie the title opened, a D3DSurface in the
 * contiguous window, XMVDecoder_GetNextFrame's results, pacing and
 * timestamps, the pictures written, the skip fallback. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kernel/kernel.h"
#include "kernel/xbox_memory_layout.h"
#include "video/xmv_hle.h"
#include "video/xmv/xmv_player.h"
#include "video/xmv/xmv_surface.h"

/* Provided by the generated title; this harness has none. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static uint8_t *host(uint32_t va) { return (uint8_t *)(uintptr_t)va + xbox_GetMemoryOffset(); }
static uint32_t rd32(uint32_t va) { uint32_t v; memcpy(&v, host(va), 4); return v; }
static void wr32(uint32_t va, uint32_t v) { memcpy(host(va), &v, 4); }

static void nap_ms(int ms)
{
    struct timespec ts = { 0, ms * 1000000L };
    nanosleep(&ts, NULL);
}

#define W 96
#define H 64
#define PITCH 512                        /* a 128-pixel LIN_X8R8G8B8 surface */

int main(int argc, char **argv)
{
    const char *xbe_path = argc > 1 ? argv[1] : "tools/conformance/test.xbe";
    const char *movie = argc > 2 ? argv[2] : "tests/xmv_decode/fixtures/basic.xmv";
    size_t xbe_len = 0;
    unsigned char *xbe;
    uint32_t surf, pix, res, tim, last_t = 0, frames = 0, nof = 0, calls = 0, r;
    XmvPlayer *ref;
    uint8_t *expect;

    setvbuf(stdout, NULL, _IONBF, 0);
    xbe = load_file(xbe_path, &xbe_len);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, xbe_len)) {
        printf("cannot set up guest memory from %s\n", xbe_path);
        return 2;
    }
    ref = xmv_player_open_file(movie);
    if (!ref) {
        printf("cannot open %s\n", movie);
        return 2;
    }

    /* A D3DSurface (Common, Data, Lock, Format, Size) and its pixels. */
    surf = xbox_ContiguousAlloc(0x40, 16);
    pix = xbox_ContiguousAlloc(PITCH * H, 4096);
    res = xbox_ContiguousAlloc(16, 16);
    CHECK(surf && pix && res, "guest allocations");
    tim = res + 8;
    memset(host(pix), 0x5A, PITCH * H);
    wr32(surf + 0x04, pix - XBOX_CONTIG_BASE);            /* physical address */
    wr32(surf + 0x0C, 0x1Eu << 8);                         /* LIN_X8R8G8B8 */
    wr32(surf + 0x10, (128u - 1) | ((H - 1u) << 12) | ((PITCH / 64u - 1) << 24));

    /* No movie opened yet: the decoder object ends at once. */
    r = xmv_hle_get_next_frame(0x00A00000u, surf, res, tim);
    CHECK(r == 0 && rd32(res) == XMV_ENDOFFILE, "no movie opened: XMV_ENDOFFILE (%u)", rd32(res));

    /* The title opens the movie; a new decoder plays it. */
    xbox_MovieFileOpened(movie);
    expect = (uint8_t *)malloc(PITCH * H);
    for (calls = 0; calls < 5000; calls++) {
        r = xmv_hle_get_next_frame(0x00A10000u, surf, res, tim);
        CHECK(r == 0, "S_OK");
        if (rd32(res) == XMV_ENDOFFILE)
            break;
        if (rd32(res) == XMV_NEWFRAME) {
            uint32_t t = rd32(tim), pts;
            const uint8_t *pl[3];
            int st[3];
            CHECK(rd32(tim + 4) == 0, "the timestamp's high word");
            CHECK(frames == 0 || t > last_t, "frame %u at %u after %u", frames, t, last_t);
            last_t = t;
            /* The same frame from a player of our own, converted the same way. */
            CHECK(xmv_player_next_frame(ref, &pts) == 1 && pts * 10000u == t, "frame %u time %u (%u ms)", frames,
                  t, pts);
            pl[0] = xmv_player_plane(ref, 0, &st[0]);
            pl[1] = xmv_player_plane(ref, 1, &st[1]);
            pl[2] = xmv_player_plane(ref, 2, &st[2]);
            memset(expect, 0x5A, PITCH * H);
            xmv_surface_write(pl, st, W, H, 0x1E, 128, H, PITCH, expect, PITCH * H);
            CHECK(!memcmp(expect, host(pix), PITCH * H), "frame %u: the surface holds the picture", frames);
            frames++;
        } else {
            CHECK(rd32(res) == XMV_NOFRAME, "result %u", rd32(res));
            nof++;
            nap_ms(2);
        }
    }
    CHECK(frames == 12, "12 frames delivered, %u", frames);
    CHECK(nof > 0, "the movie was paced (%u XMV_NOFRAME answers)", nof);
    CHECK(rd32(res) == XMV_ENDOFFILE, "the movie ends");
    r = xmv_hle_get_next_frame(0x00A10000u, surf, res, tim);
    CHECK(rd32(res) == XMV_ENDOFFILE, "and stays ended");

    /* A new open after the end: the same decoder object plays it again. */
    xbox_MovieFileOpened(movie);
    xmv_hle_get_next_frame(0x00A10000u, surf, res, tim);
    CHECK(rd32(res) == XMV_NEWFRAME, "a new open restarts the decoder object (%u)", rd32(res));
    xmv_hle_close(0x00A10000u);

    /* RECOMP_XMV=skip: every movie ends at once. */
    setenv("RECOMP_XMV", "skip", 1);
    xbox_MovieFileOpened(movie);
    xmv_hle_get_next_frame(0x00A20000u, surf, res, tim);
    CHECK(rd32(res) == XMV_ENDOFFILE, "RECOMP_XMV=skip ends the movie (%u)", rd32(res));
    unsetenv("RECOMP_XMV");

    /* A file that is not XMV: skipped too. */
    xbox_MovieFileOpened(xbe_path);
    xmv_hle_get_next_frame(0x00A30000u, surf, res, 0);
    CHECK(rd32(res) == XMV_ENDOFFILE, "a non-XMV file is skipped (%u)", rd32(res));

    /* A surface format the hook does not write: frames are still reported. */
    xbox_MovieFileOpened(movie);
    wr32(surf + 0x0C, 0x0Cu << 8);                         /* DXT1 */
    xmv_hle_get_next_frame(0x00A40000u, surf, res, tim);
    CHECK(rd32(res) == XMV_NEWFRAME, "an unknown surface format still advances the movie (%u)", rd32(res));
    xmv_hle_close(0x00A40000u);

    /* The binding tools.prepare generates (XBA-199) asks which decoder runs:
     * the runtime's by default and with skip, the title's own with native. */
    CHECK(xmv_hle_bound(0, 0x00012050u, 90) == 1, "bound: the runtime's decoder by default");
    setenv("RECOMP_XMV", "auto", 1);
    CHECK(xmv_hle_bound(1, 0x00012080u, 90) == 1, "RECOMP_XMV=auto: the runtime's decoder");
    setenv("RECOMP_XMV", "skip", 1);
    CHECK(xmv_hle_bound(0, 0x00012050u, 90) == 1, "RECOMP_XMV=skip: the runtime's hook (which ends movies)");
    setenv("RECOMP_XMV", "native", 1);
    CHECK(xmv_hle_bound(0, 0x00012050u, 90) == 0, "RECOMP_XMV=native: the title's own decoder");
    unsetenv("RECOMP_XMV");

    xmv_player_close(ref);
    free(expect);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("xmv_hle: %u frames, %u paced polls, all passed\n", frames, nof);
    return 0;
}
