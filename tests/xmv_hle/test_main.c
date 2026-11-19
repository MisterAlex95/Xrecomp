/* The runtime's XMV hook (src/video/xmv_hle.c, XBA-70) on guest memory:
 * the kernel-side note of the movie the title opened, a D3DSurface in the
 * contiguous window, XMVDecoder_GetNextFrame's results, pacing and
 * timestamps (both XDK layouts, nothing written past the field: XBA-316),
 * the pictures written, the skip fallback. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kernel/kernel.h"
#include "kernel/kernel_disc.h"
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
static const char *movie_basic;
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

/* XBA-316: the timestamp field sits in TIME_GUARD bytes of sentinels on
 * each side; the hook may write only the field's own width. */
#define TIME_GUARD 8
#define SENTINEL 0xA5
static void time_fill(uint32_t tim)
{
    memset(host(tim - TIME_GUARD), SENTINEL, TIME_GUARD + 8 + TIME_GUARD);
}

/* 1 if no byte of the guard area outside [tim, tim + width) changed. */
static int time_guard_intact(uint32_t tim, unsigned width)
{
    const uint8_t *p = host(tim - TIME_GUARD);
    unsigned i, n = TIME_GUARD + 8 + TIME_GUARD;
    for (i = 0; i < n; i++)
        if ((i < TIME_GUARD || i >= TIME_GUARD + width) && p[i] != SENTINEL)
            return 0;
    return 1;
}

/* Plays basic.xmv on a new decoder object with the given layout; checks
 * each frame's time against our own player, that XMV_NOFRAME and
 * XMV_ENDOFFILE leave the field alone, and that nothing past the field's
 * width is touched. Returns the frames seen. */
static unsigned play_time_layout(uint32_t decoder, unsigned layout, uint32_t surf, uint32_t res, uint32_t tim)
{
    unsigned width = layout == XMV_TIME_100NS64 ? 8 : 4, frames = 0, calls;
    XmvPlayer *ref = xmv_player_open_file(movie_basic);
    CHECK(ref != NULL, "reference player");
    if (!ref)
        return 0;
    xbox_MovieFileOpened(movie_basic);
    time_fill(tim);
    for (calls = 0; calls < 5000; calls++) {
        uint8_t before[8];
        memcpy(before, host(tim), 8);
        xmv_hle_get_next_frame_ex(decoder, surf, res, tim, layout);
        CHECK(time_guard_intact(tim, width), "layout %u: bytes past the %u-byte field untouched (call %u)", layout,
              width, calls);
        if (rd32(res) == XMV_NEWFRAME) {
            uint32_t pts = 0;
            CHECK(xmv_player_next_frame(ref, &pts) == 1, "reference frame %u", frames);
            if (layout == XMV_TIME_100NS64) {
                uint64_t t;
                memcpy(&t, host(tim), 8);
                CHECK(t == (uint64_t)pts * 10000u, "100 ns layout: frame %u at %llu, expected %u ms", frames,
                      (unsigned long long)t, pts);
            } else {
                CHECK(rd32(tim) == pts, "ms layout: frame %u at %u ms, expected %u", frames, rd32(tim), pts);
            }
            frames++;
        } else {
            CHECK(!memcmp(before, host(tim), 8), "layout %u: result %u leaves the timestamp alone", layout,
                  rd32(res));
            if (rd32(res) == XMV_ENDOFFILE)
                break;
            nap_ms(2);
        }
    }
    xmv_hle_close(decoder);
    xmv_player_close(ref);
    return frames;
}

static void test_time_layouts(uint32_t surf, uint32_t res, uint32_t tim)
{
    CHECK(play_time_layout(0x00D00000u, XMV_TIME_MS32, surf, res, tim) == 12, "ms layout: 12 frames");
    CHECK(play_time_layout(0x00D10000u, XMV_TIME_100NS64, surf, res, tim) == 12, "100 ns layout: 12 frames");
    /* The 4-argument form (hand-written overrides, packs prepared before
     * XBA-316) is the ms one: one DWORD, the high word's sentinels intact. */
    xbox_MovieFileOpened(movie_basic);
    time_fill(tim);
    xmv_hle_get_next_frame(0x00D20000u, surf, res, tim);
    CHECK(rd32(res) == XMV_NEWFRAME && rd32(tim) == 0 && time_guard_intact(tim, 4),
          "xmv_hle_get_next_frame writes the ms DWORD only");
    xmv_hle_close(0x00D20000u);
}

/* XBA-199: the binding's routing. A movie the runtime decodes exactly is
 * its own; one it would not (or cannot open) goes to the title's own decoder,
 * whose results the watchdog then watches. */
static char fixture_dir[512];
static const char *fixture(const char *name)
{
    static char path[640];
    snprintf(path, sizeof path, "%s/%s.xmv", fixture_dir, name);
    return path;
}

static int route(const char *movie, uint32_t decoder)
{
    xbox_MovieFileOpened(movie);
    return xmv_hle_route(0x00012050u, 90, decoder);
}

/* The title's own GetNextFrame wrote `result`; the binding reports it. */
static void native(uint32_t decoder, uint32_t res, uint32_t result)
{
    wr32(res, result);
    xmv_hle_native_result(decoder, res);
}

static void test_routing(const char *xbe_path, uint32_t surf, uint32_t res, uint32_t tim)
{
    static const char *const delegated[] = { "x8", "wma", "surround", "damaged" };
    XmvPlayer *p;
    XmvProbe pr;
    uint32_t r, polls;
    int i;

    /* The probe, on the player: X8 frames are seen from their picture header. */
    p = xmv_player_open_file(fixture("x8"));
    CHECK(p && xmv_player_probe(p, 1800, 30, &pr) == 0, "probe x8.xmv");
    if (p) {
        uint32_t pts;
        CHECK(pr.x8 == 1 && pr.x8_allowed && pr.frames == 12 && pr.complete && !pr.broken,
              "x8.xmv: one X8 frame of 12 (%u of %u)", pr.x8, pr.frames);
        CHECK(xmv_player_next_frame(p, &pts) == 1 && pts == 0, "the probe rewinds the player");
        xmv_player_close(p);
    }
    p = xmv_player_open_file(fixture("damaged"));
    CHECK(p && xmv_player_probe(p, 1800, 30, &pr) == 0 && pr.damaged == 4 && pr.decoded == 12,
          "damaged.xmv: 4 damaged frames (%u)", p ? pr.damaged : 0);
    xmv_player_close(p);
    p = xmv_player_open_file(fixture("x8_unused"));
    CHECK(p && xmv_player_probe(p, 5, 2, &pr) == 0 && pr.frames == 5 && pr.decoded == 2 && !pr.complete &&
          pr.x8_allowed && !pr.x8, "the probe stops at its limits");
    xmv_player_close(p);

    /* Fully supported: the runtime's. */
    CHECK(route(fixture("x8_unused"), 0x00B00000u) == 1, "X8 allowed but not used: the runtime decodes it");
    xmv_hle_get_next_frame(0x00B00000u, surf, res, tim);
    CHECK(rd32(res) == XMV_NEWFRAME, "and plays it (%u)", rd32(res));
    xmv_hle_close(0x00B00000u);
    CHECK(route(movie_basic, 0x00B00000u) == 1, "basic.xmv: the runtime decodes it");
    xmv_hle_close(0x00B00000u);

    /* Handed to the title's own decoder, for the whole movie. */
    for (i = 0; i < 4; i++) {
        CHECK(route(fixture(delegated[i]), 0x00B10000u) == 0, "%s.xmv: the title's own decoder", delegated[i]);
        CHECK(xmv_hle_route(0x00012050u, 90, 0x00B10000u) == 0, "%s.xmv: still the title's own", delegated[i]);
        native(0x00B10000u, res, XMV_NEWFRAME);
        CHECK(xmv_hle_route(0x00012050u, 90, 0x00B10000u) == 0, "%s.xmv: whole", delegated[i]);
        xmv_hle_close(0x00B10000u);
    }
    CHECK(route(xbe_path, 0x00B20000u) == 0, "a file the runtime cannot open: the title's own decoder");
    xmv_hle_close(0x00B20000u);

    /* RECOMP_XMV: runtime keeps every movie, skip ends it through the hook,
     * native never asks. */
    setenv("RECOMP_XMV", "runtime", 1);
    CHECK(route(fixture("x8"), 0x00B30000u) == 1, "RECOMP_XMV=runtime: the runtime decodes an X8 movie too");
    xmv_hle_get_next_frame(0x00B30000u, surf, res, tim);
    CHECK(rd32(res) == XMV_NEWFRAME, "RECOMP_XMV=runtime plays it (%u)", rd32(res));
    xmv_hle_close(0x00B30000u);
    setenv("RECOMP_XMV", "skip", 1);
    CHECK(route(fixture("x8"), 0x00B30000u) == 1, "RECOMP_XMV=skip: the hook");
    xmv_hle_get_next_frame(0x00B30000u, surf, res, tim);
    CHECK(rd32(res) == XMV_ENDOFFILE, "RECOMP_XMV=skip ends it (%u)", rd32(res));
    xmv_hle_close(0x00B30000u);
    setenv("RECOMP_XMV", "native", 1);
    CHECK(route(movie_basic, 0x00B30000u) == 0, "RECOMP_XMV=native: the title's own, also for a supported movie");
    unsetenv("RECOMP_XMV");

    /* The watchdog: the title's own decoder gives nothing while the title
     * polls; the runtime plays the movie instead. */
    setenv("RECOMP_XMV_WATCHDOG", "0.2", 1);
    CHECK(route(fixture("x8"), 0x00B40000u) == 0, "watchdog: handed over");
    for (polls = 0; polls < 400 && xmv_hle_route(0x00012050u, 90, 0x00B40000u) == 0; polls++) {
        native(0x00B40000u, res, XMV_NOFRAME);
        nap_ms(2);
    }
    CHECK(polls >= 40 && polls < 400, "the watchdog trips after 0.2 s of polls (%u polls)", polls);
    for (r = 0, polls = 0; polls < 200 && r != XMV_NEWFRAME; polls++) {
        xmv_hle_get_next_frame(0x00B40000u, surf, res, tim);
        r = rd32(res);
        nap_ms(2);
    }
    CHECK(r == XMV_NEWFRAME, "then the runtime plays it (%u)", r);
    xmv_hle_close(0x00B40000u);

    /* A hang after frames were shown: the movie ends. */
    CHECK(route(fixture("wma"), 0x00B50000u) == 0, "watchdog after frames: handed over");
    native(0x00B50000u, res, XMV_NEWFRAME);
    for (polls = 0; polls < 400 && xmv_hle_route(0x00012050u, 90, 0x00B50000u) == 0; polls++) {
        native(0x00B50000u, res, XMV_NOFRAME);
        nap_ms(2);
    }
    xmv_hle_get_next_frame(0x00B50000u, surf, res, tim);
    CHECK(polls < 400 && rd32(res) == XMV_ENDOFFILE, "a hang after frames ends the movie (%u)", rd32(res));
    xmv_hle_close(0x00B50000u);

    /* Polls a second apart do not count (a paused movie). */
    CHECK(route(fixture("x8"), 0x00B60000u) == 0, "pause: handed over");
    native(0x00B60000u, res, XMV_NOFRAME);
    nap_ms(1100);
    native(0x00B60000u, res, XMV_NOFRAME);
    CHECK(xmv_hle_route(0x00012050u, 90, 0x00B60000u) == 0, "a pause is not a hang");
    setenv("RECOMP_XMV_WATCHDOG", "0", 1);
    for (polls = 0; polls < 150; polls++) {
        native(0x00B60000u, res, XMV_NOFRAME);
        nap_ms(2);
    }
    CHECK(xmv_hle_route(0x00012050u, 90, 0x00B60000u) == 0, "RECOMP_XMV_WATCHDOG=0: no watchdog");
    xmv_hle_close(0x00B60000u);
    unsetenv("RECOMP_XMV_WATCHDOG");
}

#define W 96
/* XBA-309: the game data read in place from a disc image (Prepare's default
 * for an ISO). The movie the title opens is then a path inside the image,
 * not a host file: the hook reads it through the disc layer. Before, it could
 * not open it, and every movie of such a title went to the title's own
 * decoder ("not an XMV file the runtime can play"). */
static void test_disc_image(const char *iso, uint32_t surf, uint32_t res, uint32_t tim)
{
    char path[1024];
    FILE *f;
    uint32_t frames = 0, calls;
    int rc = xbox_disc_mount(iso);
    CHECK(rc == 0, "mount %s (%d)", iso, rc);
    if (rc)
        return;
    snprintf(path, sizeof path, "%s/movie/basic.xmv", iso);
    f = fopen(path, "rb");
    CHECK(!f, "%s is not a host file", path);
    if (f)
        fclose(f);
    CHECK(route(path, 0x00C00000u) == 1, "a movie inside the disc image: the runtime decodes it");
    for (calls = 0; calls < 5000; calls++) {
        xmv_hle_get_next_frame(0x00C00000u, surf, res, tim);
        if (rd32(res) == XMV_ENDOFFILE)
            break;
        if (rd32(res) == XMV_NEWFRAME)
            frames++;
        else
            nap_ms(2);
    }
    CHECK(frames == 12, "the disc image's movie: 12 frames delivered (%u)", frames);
    xmv_hle_close(0x00C00000u);
    /* A name the image does not hold: not playable, handed over as before. */
    snprintf(path, sizeof path, "%s/movie/missing.xmv", iso);
    CHECK(route(path, 0x00C10000u) == 0, "a movie missing from the image: the title's own decoder");
    xmv_hle_close(0x00C10000u);
    xbox_disc_unmount();
}

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
    snprintf(fixture_dir, sizeof fixture_dir, "%s", argc > 3 ? argv[3] : "tests/xmv_hle/fixtures");
    movie_basic = movie;
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
    tim = xbox_ContiguousAlloc(TIME_GUARD + 8 + TIME_GUARD, 16);
    CHECK(surf && pix && res && tim, "guest allocations");
    tim += TIME_GUARD;
    memset(host(pix), 0x5A, PITCH * H);
    wr32(surf + 0x04, pix - XBOX_CONTIG_BASE);            /* physical address */
    wr32(surf + 0x0C, 0x1Eu << 8);                         /* LIN_X8R8G8B8 */
    wr32(surf + 0x10, (128u - 1) | ((H - 1u) << 12) | ((PITCH / 64u - 1) << 24));

    /* No movie opened yet: the decoder object ends at once. */
    r = xmv_hle_get_next_frame(0x00A00000u, surf, res, tim);
    CHECK(r == 0 && rd32(res) == XMV_ENDOFFILE, "no movie opened: XMV_ENDOFFILE (%u)", rd32(res));

    /* The title opens the movie; a new decoder plays it. */
    xbox_MovieFileOpened(movie);
    time_fill(tim);
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
            CHECK(time_guard_intact(tim, 4), "frame %u: one DWORD written, nothing past it", frames);
            CHECK(frames == 0 || t > last_t, "frame %u at %u after %u", frames, t, last_t);
            last_t = t;
            /* The same frame from a player of our own, converted the same way. */
            CHECK(xmv_player_next_frame(ref, &pts) == 1 && pts == t, "frame %u time %u (%u ms)", frames, t, pts);
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

    wr32(surf + 0x0C, 0x1Eu << 8);                         /* LIN_X8R8G8B8 again */
    test_time_layouts(surf, res, tim);
    test_routing(xbe_path, surf, res, tim);
    if (argc > 4)
        test_disc_image(argv[4], surf, res, tim);
    else
        printf("xmv_hle: no disc image fixture (python3 missing at configure time): disc test skipped\n");

    xmv_player_close(ref);
    free(expect);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("xmv_hle: %u frames, %u paced polls, all passed\n", frames, nof);
    return 0;
}
