/* The GL back end itself (src/gpu/nv2a_gl.c) on a real context: clears,
 * read-backs and presents against a fake guest memory, through the back-end
 * interface the executor calls (nv2a_backend.h). Built twice, once for the
 * desktop GL 3.3 core path and once for the GLES 3.0 path Android runs
 * (NV2A_GL_FORCE_ES, nv2a_gl_api.h); on Linux both run on Mesa's llvmpipe
 * through EGL_PLATFORM=surfaceless. Exit 77 without a GL context.
 *
 * nv2a_gl.c is included, not linked, so the checks can read its statistics.
 *
 * Guest CPU writes into GPU targets (XBA-119): a title that writes a GPU
 * target's memory with the CPU (a movie decoded into the back buffer, a
 * loading bar drawn into the frame) must see those pixels in the GPU's
 * picture, with every other pixel as the GPU drew it:
 *   1. a clean target (read back since its last draw): CPU rectangle, then
 *      a GPU clear elsewhere; the read-back holds both;
 *   2. a dirty target: GPU clear, CPU rectangle, read-back: both (before,
 *      the read-back overwrote the CPU's pixels);
 *   3. host present: the CPU's frame reaches the window's host frame;
 *   4. a 16-bit target at internal scale 2 (the resolve path);
 *   5. a swizzled target;
 *   6. nothing merged while the CPU writes nothing.
 *
 * Depth re-creation (a bug found on the way): a target bigger than the
 * scratch depth (zeta 0, 640x480 at least) makes it grow; depth_free
 * rebinds the framebuffers that had it, and left the last of them bound,
 * so the new depth and the clear went into another target:
 *   7. an 800x600 target cleared after the 64x32 ones is the one cleared,
 *      and the 64x32 target keeps its pixels.
 *
 * Anti-aliased presented surface (XBA-119): a 1280x960 AA 2x2 back buffer
 * presented as is (the window cannot read it through guest memory) comes
 * out as a 640x480 host frame, each pixel the mean of its four samples:
 *   8. one green sample column over red reads half green, half red.
 * Save states (XBA-248), the back end's state hook:
 *   9. a target drawn by the GPU only (never read back), FLUSH, guest memory
 *      captured; more drawn and presented: the reference. The memory
 *      restored, DROP (every GL object forgotten), the same drawn again:
 *      the same pixels. Without the FLUSH the capture misses the GPU's
 *      pixels and the picture differs (the control). The DROP deletes the
 *      cached index buffers too: a name made before it is no buffer after
 *      (it was only forgotten, and leaked up to 32 MB a load).
 * Texture cache (case 10): more distinct textures than the table holds,
 * streamed over frames, retire the old entries; a texture sampled every
 * frame is decoded once (the table used to be dropped whole at 384 keys).
 * Program table (case 11): a full table is dropped and the new pipeline
 * linked (it used to be refused for the rest of the run).
 * Programs off the draw path (case 12, XBA-310), in whichever mode the
 * ctest entry selects (worker by default, RECOMP_GPU_ASYNC_SHADERS=khr / =0):
 * with no wait a new pipeline's batch gets the stand-in (no fragment) until
 * its compile is done, then its own program; with the binary cache in a
 * scratch save dir, the programs linked go to programs.list, and a run
 * whose binaries are gone (a driver update) compiles them ahead from it
 * during its first flips, the shader badge counting "Shaders 1/5" .. done,
 * so the draws that need them find them linked.
 * Also: RECOMP_GPU unset selects the platform's default (gpu_selected).
 * RECOMP_GPU_CPU_WRITES=0 (another ctest entry) checks that the same cases
 * fail without the merge, so the test tells the two apart. */
#include "../../src/gpu/nv2a_gl.c"
#include <unistd.h>

#define MEM_BYTES (16u << 20)
#define FB32   0x00100000u          /* 64x32 A8R8G8B8, pitch 256 */
#define FB16   0x00200000u          /* 640x480 R5G6B5 (frame class: scaled) */
#define FBSWZ  0x00400000u          /* 32x32 swizzled A8R8G8B8 */
#define FBHOST 0x00500000u          /* 64x32 A8R8G8B8, host present */
#define FBAA   0x00600000u          /* 1280x960 A8R8G8B8, AA 2x2 */
#define FBBIG  0x00B00000u          /* 800x600 A8R8G8B8 */
#define FBST   0x00E00000u          /* 64x32 A8R8G8B8, save states */

static uint8_t *s_mem;
static int s_fail, s_cpu_on = 1;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; \
} } while (0)

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
static const char *s_save_dir;      /* case 12: a scratch save dir */
const char *xbox_path_save_dir_utf8(void) { return s_save_dir; }
void xbox_HostThreadFloat(const char *role) { (void)role; }   /* the shader worker's placement */
void recomp_host_cpus_untrack(void) {}
/* Textures: LIN_A8R8G8B8 only, read as is; decodes counted per address. */
#define TEX_LIVE 0x00F00000u        /* 4x4, sampled every frame (case 10) */
static uint32_t s_live_decodes;
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out)
{
    if (tex->color != 0x12)
        return 0;
    memcpy(argb_out, s_mem + tex->offset, (size_t)tex->width * tex->height * 4u);
    s_live_decodes += tex->offset == TEX_LIVE;
    return 1;
}
uint32_t nv2a_backend_texture_bytes(const Nv2aTexture *tex)
{
    return tex->color == 0x12 ? tex->width * tex->height * 4u : 0;
}
int nv2a_backend_texture_level(const Nv2aTexture *tex, uint32_t level, Nv2aTexture *out)
{ (void)tex; (void)level; (void)out; return 0; }

static const Nv2aBackend *s_be;
static int (*s_present)(const Nv2aPresentSurface *, int, Nv2aPresentSurface *);
void nv2a_backend_register(const Nv2aBackend *b) { s_be = b; if (!b) s_present = NULL; }
void nv2a_backend_register_with_present(const Nv2aBackend *b,
                                        int (*present)(const Nv2aPresentSurface *, int,
                                                       Nv2aPresentSurface *))
{
    s_be = b;
    s_present = present;
}

static Nv2aRenderState s_rs;

static Nv2aSurface surf(uint32_t va, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp,
                        uint32_t format)
{
    Nv2aSurface s;
    memset(&s, 0, sizeof s);
    s.color_va = va;
    s.width = w;
    s.height = h;
    s.pitch = pitch;
    s.bytes_per_pixel = bpp;
    s.aa_sx = s.aa_sy = 1;
    s.format = format;
    s.color_valid = 1;
    return s;
}

static void clear(const Nv2aSurface *s, uint32_t argb)
{
    s_be->clear(s, &s_rs, 0xF0, argb, 0);
}

static Nv2aSurface clip(Nv2aSurface s, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    s.clip_x = x;
    s.clip_y = y;
    s.width = w;
    s.height = h;
    return s;
}

/* A flip's present (sync: read back into guest memory now), then flip. */
static int present(const Nv2aSurface *s, int sync, Nv2aPresentSurface *ready)
{
    Nv2aPresentSurface now, tmp;
    int r;
    memset(&now, 0, sizeof now);
    now.va = s->color_va;
    now.pitch = s->pitch;
    now.bpp = s->bytes_per_pixel;
    now.format = s->format;
    now.w = s->width;
    now.h = s->height;
    r = s_present(&now, sync, ready ? ready : &tmp);
    return r;
}

static uint32_t px32(uint32_t va, uint32_t pitch, uint32_t x, uint32_t y)
{
    uint32_t v;
    memcpy(&v, s_mem + va + (size_t)y * pitch + x * 4u, 4);
    return v;
}
static void put32(uint32_t va, uint32_t pitch, uint32_t x, uint32_t y, uint32_t v)
{
    memcpy(s_mem + va + (size_t)y * pitch + x * 4u, &v, 4);
}
static uint16_t px16(uint32_t va, uint32_t pitch, uint32_t x, uint32_t y)
{
    uint16_t v;
    memcpy(&v, s_mem + va + (size_t)y * pitch + x * 2u, 2);
    return v;
}
static void rect32(uint32_t va, uint32_t pitch, uint32_t x0, uint32_t y0, uint32_t x1,
                   uint32_t y1, uint32_t v)
{
    uint32_t x, y;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            put32(va, pitch, x, y, v);
}

/* Every pixel of the 64x32 surface at va: blue inside [bx0,bx1)x[by0,by1),
 * green inside [gx0,gx1) (all rows), red elsewhere. Returns the number of
 * pixels that are not. */
static uint32_t count_bad(uint32_t va, uint32_t bx0, uint32_t by0, uint32_t bx1, uint32_t by1,
                          uint32_t gx0, uint32_t gx1)
{
    uint32_t x, y, bad = 0;
    for (y = 0; y < 32; y++)
        for (x = 0; x < 64; x++) {
            uint32_t want = 0xFFFF0000u, got = px32(va, 256, x, y);
            if (x >= gx0 && x < gx1)
                want = 0xFF00FF00u;
            else if (x >= bx0 && x < bx1 && y >= by0 && y < by1)
                want = 0xFF0000FFu;
            bad += got != want;
        }
    return bad;
}

static void flip(void) { s_be->flip(); }

/* Case 12: the files of a scratch directory, deleted. */
static void rm_files(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d)
        return;
    while ((de = readdir(d)) != NULL)
        if (de->d_name[0] != '.') {
            char f[1500];
            snprintf(f, sizeof f, "%s/%s", dir, de->d_name);
            remove(f);
        }
    closedir(d);
}

int main(void)
{
    const char *e = getenv("RECOMP_GPU_CPU_WRITES");
    Nv2aSurface s32 = surf(FB32, 64, 32, 256, 4, 0x0108);
    uint32_t bad, up0;

    s_cpu_on = !(e && strcmp(e, "0") == 0);
    s_mem = (uint8_t *)calloc(1, MEM_BYTES);
    /* Which renderer (XBA-119): RECOMP_GPU unset means the platform's
     * default -- the GL back end on Android, the CPU rasteriser elsewhere;
     * "0" is always the CPU rasteriser, anything else the GL back end. */
    CHECK(gpu_selected(NULL, 1) && !gpu_selected(NULL, 0) && gpu_selected("", 1));
    CHECK(!gpu_selected("0", 1) && gpu_selected("1", 0) && gpu_selected("1", 1));
    unsetenv("RECOMP_GPU");
    CHECK(nv2a_gl_install_from_env() == NV2A_GL_DEFAULT_ON);
    CHECK(!s_be == !NV2A_GL_DEFAULT_ON);
    setenv("RECOMP_GPU", "1", 1);
    setenv("RECOMP_GPU_SCALE", "2", 1);
    setenv("RECOMP_GPU_PRESENT", "host", 1);
    nv2a_gl_set_display_size(1280, 960);
    if (!nv2a_gl_install_from_env() || !s_be || !s_present) {
        fprintf(stderr, "back end not registered\n");
        return 1;
    }
    s_rs.color_mask = 0x01010101u;
    clear(&s32, 0xFFFF0000u);
    if (!s_be || s_init <= 0) {
        puts("no GL context: GL back-end checks skipped");
        return 77;
    }
    printf("GL: %s | %s (%s path)\n", (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION), NV2A_GL_ES ? "GLES" : "desktop GL");

    /* 1. Clean target: read back, CPU rectangle, GPU clear elsewhere. The
     * rectangle's three rows go up in one upload. */
    present(&s32, 1, NULL);
    flip();
    CHECK(count_bad(FB32, 0, 0, 0, 0, 0, 0) == 0);
    up0 = s_cpu_uploads;
    rect32(FB32, 256, 10, 5, 20, 8, 0xFF0000FFu);
    {
        Nv2aSurface g = clip(s32, 40, 0, 10, 32);
        clear(&g, 0xFF00FF00u);
    }
    present(&s32, 1, NULL);
    flip();
    bad = count_bad(FB32, 10, 5, 20, 8, 40, 50);
    printf("1. clean target, CPU rectangle then GPU clear: %u pixels wrong\n", bad);
    CHECK(s_cpu_on ? bad == 0 : bad == 30);
    CHECK(s_cpu_uploads - up0 == (s_cpu_on ? 1u : 0u));

    /* 2. Dirty target: GPU clear, CPU rectangle, read-back. */
    clear(&s32, 0xFFFF0000u);
    rect32(FB32, 256, 2, 20, 9, 30, 0xFF0000FFu);
    present(&s32, 1, NULL);
    flip();
    bad = count_bad(FB32, 2, 20, 9, 30, 0, 0);
    printf("2. dirty target, CPU rectangle before the read-back: %u pixels wrong\n", bad);
    CHECK(s_cpu_on ? bad == 0 : bad == 70);

    /* 3. Host present: the GPU's red frame, then a frame the CPU wrote
     * whole (a movie). The host frame (read at flip N, shown at N+1) must
     * end up the CPU's picture. */
    {
        Nv2aSurface h = surf(FBHOST, 64, 32, 256, 4, 0x0108);
        Nv2aPresentSurface ready;
        uint32_t i, shown = 0;
        clear(&h, 0xFFFF0000u);
        for (i = 0; i < 2; i++) {
            present(&h, 0, &ready);
            flip();
        }
        up0 = s_cpu_uploads;
        rect32(FBHOST, 256, 0, 0, 64, 32, 0xFF204080u);
        for (i = 0; i < 3; i++) {
            int ok;
            memset(&ready, 0, sizeof ready);
            ok = present(&h, 0, &ready);
            if (ok && ready.host_px) {
                const uint8_t *p = (const uint8_t *)ready.host_px
                                 + (size_t)(ready.host_h / 2) * ready.host_pitch
                                 + (ready.host_w / 2) * 4u;
                /* RGBX bytes (GLES) or 0xXXRRGGBB words (desktop). */
                shown = ready.host_format == NV2A_HOST_FORMAT_RGBX8888
                      ? ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2])
                      : ((uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
            } else if (ok) {
                shown = px32(ready.va, ready.pitch, 32, 16) & 0xFFFFFFu;  /* guest memory */
            }
            flip();
        }
        printf("3. host present of a CPU-written frame: shows 0x%06X\n", shown);
        CHECK(s_cpu_on ? shown == 0x204080u : shown != 0x204080u);
        /* The whole frame changed: one upload, not one a row. */
        printf("3. whole frame merged in %u upload(s)\n", s_cpu_uploads - up0);
        CHECK(s_cpu_uploads - up0 == (s_cpu_on ? 1u : 0u));
    }

    /* 4. 640x480 R5G6B5: the frame class, drawn at scale 2. */
    {
        Nv2aSurface f = surf(FB16, 640, 480, 1280, 2, 0x0103);
        uint32_t x, y, nbad = 0;
        clear(&f, 0xFFFF0000u);
        present(&f, 1, NULL);
        flip();
        CHECK(px16(FB16, 1280, 0, 0) == 0xF800u);
        for (y = 100; y < 110; y++)
            for (x = 200; x < 300; x++) {
                uint16_t b = 0x001F;
                memcpy(s_mem + FB16 + (size_t)y * 1280 + x * 2u, &b, 2);
            }
        {
            Nv2aSurface g = clip(f, 600, 0, 40, 480);
            clear(&g, 0xFF00FF00u);
        }
        present(&f, 1, NULL);
        flip();
        for (y = 0; y < 480; y++)
            for (x = 0; x < 640; x++) {
                uint16_t want = x >= 600 ? 0x07E0 : (y >= 100 && y < 110 && x >= 200 && x < 300)
                                                    ? 0x001F : 0xF800;
                nbad += px16(FB16, 1280, x, y) != want;
            }
        printf("4. scaled 16-bit target: %u pixels wrong (scale %.2f)\n", nbad,
               (double)s_scale);
        CHECK(s_scale == 2.0f);
        CHECK(s_cpu_on ? nbad == 0 : nbad == 1000);
    }

    /* 5. Swizzled 32x32 target. */
    {
        Nv2aSurface z = surf(FBSWZ, 32, 32, 128, 4, 0x0208 | 5u << 16 | 5u << 24);
        uint32_t mx, my, v, i;
        xbox_swizzle_masks(32, 32, &mx, &my);
        clear(&z, 0xFFFF0000u);
        present(&z, 1, NULL);
        flip();
        v = 0xFF0000FFu;
        for (i = 0; i < 4; i++)
            memcpy(s_mem + FBSWZ + (size_t)(swizzle_deposit(3 + i, mx)
                                            | swizzle_deposit(4, my)) * 4, &v, 4);
        {
            Nv2aSurface g = clip(z, 20, 0, 4, 32);
            clear(&g, 0xFF00FF00u);
        }
        present(&z, 1, NULL);
        flip();
        memcpy(&v, s_mem + FBSWZ + (size_t)(swizzle_deposit(5, mx) | swizzle_deposit(4, my)) * 4, 4);
        printf("5. swizzled target: CPU pixel reads 0x%08X\n", v);
        CHECK(s_cpu_on ? v == 0xFF0000FFu : v == 0xFFFF0000u);
        memcpy(&v, s_mem + FBSWZ + (size_t)(swizzle_deposit(21, mx) | swizzle_deposit(9, my)) * 4, 4);
        CHECK(v == 0xFF00FF00u);
    }

    /* 6. Frames with no CPU write merge nothing. */
    {
        uint32_t before = s_st.cpu_merges, i;
        for (i = 0; i < 20; i++) {
            Nv2aSurface g = clip(s32, i, 0, 1, 32);
            clear(&g, 0xFF00FF00u);
            present(&s32, i & 1, NULL);
            flip();
        }
        printf("6. 20 frames without CPU writes: %u merges\n", s_st.cpu_merges - before);
        CHECK(s_st.cpu_merges == before);
    }

    /* 7. Scratch depth growth. */
    {
        Nv2aSurface b = surf(FBBIG, 800, 600, 3200, 4, 0x0108);
        uint32_t before, after, big;
        present(&s32, 1, NULL);
        flip();
        before = px32(FB32, 256, 63, 31);
        clear(&b, 0xFF0000FFu);
        present(&b, 1, NULL);
        flip();
        big = px32(FBBIG, 3200, 700, 550);
        {
            Nv2aSurface g = clip(s32, 0, 0, 1, 1);  /* make it dirty: read back again */
            clear(&g, 0xFF00FF00u);
        }
        present(&s32, 1, NULL);
        flip();
        after = px32(FB32, 256, 63, 31);
        printf("7. 800x600 target after 64x32 ones: reads 0x%08X; the 64x32 target 0x%08X"
               " -> 0x%08X\n", big, before, after);
        CHECK(big == 0xFF0000FFu);
        CHECK(after == before);
    }

    /* 8. AA 2x2 back buffer, presented. At scale 1 (the frame class would
     * otherwise draw it at 2, and the host frame would keep every sample). */
    {
        Nv2aSurface a = surf(FBAA, 1280, 960, 5120, 4, 0x2108);
        Nv2aPresentSurface ready;
        uint32_t i, w = 0, h = 0, c0 = 0, c1 = 0;
        float keep = s_scale_cfg.scale;
        s_scale_cfg.scale = 1.0f;
        a.aa_sx = a.aa_sy = 2;
        clear(&a, 0xFFFF0000u);
        {
            Nv2aSurface g = clip(a, 0, 0, 1, 960);
            clear(&g, 0xFF00FF00u);
        }
        for (i = 0; i < 2; i++) {
            memset(&ready, 0, sizeof ready);
            if (present(&a, 0, &ready) && ready.host_px) {
                const uint8_t *p = (const uint8_t *)ready.host_px
                                 + (size_t)(ready.host_h / 2) * ready.host_pitch;
                int rgba = ready.host_format == NV2A_HOST_FORMAT_RGBX8888;
                w = ready.host_w;
                h = ready.host_h;
                c0 = rgba ? ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2])
                          : ((uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
                p += 4;
                c1 = rgba ? ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2])
                          : ((uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
            }
            flip();
        }
        printf("8. AA 2x2 1280x960 presented: host frame %ux%u, pixel 0 0x%06X, pixel 1 0x%06X\n",
               w, h, c0, c1);
        CHECK(w == 640 && h == 480);
        CHECK((c0 >> 16) >= 0x7E && (c0 >> 16) <= 0x81 && ((c0 >> 8) & 0xFF) >= 0x7E
              && ((c0 >> 8) & 0xFF) <= 0x81 && (c0 & 0xFF) == 0);
        CHECK(c1 == 0xFF0000u);
        s_scale_cfg.scale = keep;
    }

    /* 9. Save states: FLUSH before the capture, DROP after the restore. */
    {
        Nv2aSurface st = surf(FBST, 64, 32, 256, 4, 0x0108);
        static uint8_t snap[MEM_BYTES];
        uint8_t ref[64 * 32 * 4], got[64 * 32 * 4];
        uint32_t drops = s_state_drops, flushes = s_state_flushes, diff, i;
        int pass;

        for (pass = 0; pass < 2; pass++) {       /* 0: with the FLUSH, 1: without */
            memset(s_mem + FBST, 0, sizeof ref);
            clear(&st, 0xFFFF0000u);
            present(&st, 1, NULL);               /* red in guest memory: clean */
            flip();
            {
                Nv2aSurface g = clip(st, 8, 0, 6, 32);
                clear(&g, 0xFF00FF00u);          /* green on the GPU only */
            }
            if (pass == 0)
                s_backend.state(NV2A_BACKEND_STATE_FLUSH);
            memcpy(snap, s_mem, MEM_BYTES);      /* the RAM section */
            {
                Nv2aSurface g = clip(st, 30, 10, 20, 12);
                clear(&g, 0xFF0000FFu);
            }
            present(&st, 1, NULL);
            flip();
            memcpy(ref, s_mem + FBST, sizeof ref);   /* uninterrupted */

            memset(s_mem, 0x77, MEM_BYTES);
            memcpy(s_mem, snap, MEM_BYTES);
            {
                static const uint16_t idx[6] = { 0, 1, 2, 2, 1, 3 };
                GLuint ib = ib_get(idx, 6);
                CHECK(ib != 0 && glIsBuffer(ib));
                s_backend.state(NV2A_BACKEND_STATE_DROP);
                /* Before anything can make a new GL name that reuses it. */
                CHECK(!glIsBuffer(ib));
                CHECK(s_nib == 0 && s_ib_bytes == 0);
            }
            {
                Nv2aSurface g = clip(st, 30, 10, 20, 12);
                clear(&g, 0xFF0000FFu);
            }
            present(&st, 1, NULL);
            flip();
            memcpy(got, s_mem + FBST, sizeof got);
            for (diff = 0, i = 0; i < 64 * 32; i++)
                diff += memcmp(ref + i * 4, got + i * 4, 4) != 0;
            printf("9. save state %s the flush: %u of 2048 pixels differ after the restore"
                   " (green at x=10 reads 0x%08X)\n", pass ? "without" : "with", diff,
                   px32(FBST, 256, 10, 5));
            if (pass == 0) {
                CHECK(diff == 0);
                CHECK(px32(FBST, 256, 10, 5) == 0xFF00FF00u);
                CHECK(px32(FBST, 256, 35, 15) == 0xFF0000FFu);
            } else {
                CHECK(diff == 6 * 32);           /* the green column is lost */
            }
        }
        CHECK(s_state_flushes == flushes + 1 && s_state_drops == drops + 2);
    }

    /* 10. Texture cache eviction: 400 new textures a frame for 6 frames
     * (2400 keys, more than the table holds) and one texture sampled every
     * frame. The cache retires the old keys instead of dropping everything:
     * the live texture is decoded once, and nothing is dropped whole. */
    {
        Nv2aTexture t;
        uint32_t f, i, drops = s_tex_drops, evictions = s_tex_evictions, nul = 0;
        memset(&t, 0, sizeof t);
        t.width = t.height = 4;
        t.pitch = 16;
        t.color = 0x12;
        t.levels = 1;
        memset(s_mem + TEX_LIVE, 0x5A, 0x40000);
        s_live_decodes = 0;
        for (f = 0; f < 6; f++) {
            t.offset = TEX_LIVE;
            nul += tex_get(&t, 0, 0) == NULL;
            for (i = 0; i < 400; i++) {
                t.offset = TEX_LIVE + 0x100u + (f * 400u + i) * 64u;
                nul += tex_get(&t, 1, 1) == NULL;
            }
            t.offset = TEX_LIVE;            /* again after the stream: still there */
            nul += tex_get(&t, 0, 0) == NULL;
            flip();
        }
        printf("10. texture cache, 2400 keys over 6 frames: live texture decoded %u times,"
               " %u evictions, %u full drops\n", s_live_decodes,
               s_tex_evictions - evictions, s_tex_drops - drops);
        CHECK(nul == 0);
        CHECK(s_live_decodes == 1);
        CHECK(s_tex_evictions > evictions);
        CHECK(s_tex_drops == drops);
        /* A live set too big for the table still falls back to the drop. */
        for (i = 0; i < NV_GL_MAX_TEX; i++) {
            t.offset = TEX_LIVE + 0x100u + i * 64u;
            nul += tex_get(&t, 0, 2) == NULL;
        }
        CHECK(nul == 0 && s_tex_drops == drops + 1);
        flip();
    }

    /* 11. A full program table is dropped, not refused: every slot taken
     * (failed entries, as a long session leaves them), a new pipeline still
     * gets a linked program. */
    {
        Nv2aGlShaderKey key;
        GlProg *p;
        uint32_t i, drops = s_prog_drops;
        memset(&key, 0, sizeof key);
        p = prog_get(&key, NULL);
        CHECK(p != NULL && p->prog != 0);
        for (i = 0; i < NV_GL_MAX_PROG; i++)
            if (!s_prog[i].prog && !s_prog[i].failed) {
                s_prog[i].failed = 1;
                s_prog[i].hash = 0xDEAD0000u + i;
                s_nprog++;
            }
        key.final0 = 0x12345678u;           /* a pipeline not in the table */
        p = prog_get(&key, NULL);
        printf("11. program table full: new pipeline %s, %u drop(s)\n",
               p && p->prog ? "linked" : "refused", s_prog_drops - drops);
        CHECK(p != NULL && p->prog != 0);
        CHECK(s_prog_drops == drops + 1 && s_nprog == 1);
    }

    /* 12. Programs compiled off the draw path, the program list. */
    {
        Nv2aGlShaderKey key;
        GlProg *p;
        char dir[] = "/tmp/nv2a-gl-plist-XXXXXX", path[1200], badge[64];
        uint32_t i, k, saved_wait = s_wait_max_us, standins = 0;
        int flips = 0, binaries;
        memset(&key, 0, sizeof key);
        /* No wait: the first draw of a new pipeline gets the stand-in while its
         * compile runs (a driver may finish a KHR compile at once: then its
         * own program), every later one too until it is done. */
        s_wait_max_us = 0;
        key.final0 = 0x0BADF00Du;
        p = prog_get(&key, NULL);
        CHECK(p != NULL);
        if (s_async && p == &s_standin) {
            CHECK(p->prog && p->cloc && p->cloc[0] == -1 && p->u_vp == -1);
            for (i = 0; i < 4000 && (p = prog_get(&key, NULL)) == &s_standin; i++)
                usleep(1000);
        }
        CHECK(p != NULL && p != &s_standin && p->prog != 0 && !p->pending);
        s_wait_max_us = saved_wait;

        /* The cache in a scratch dir: five programs linked, listed. */
        CHECK(mkdtemp(dir) != NULL);
        s_save_dir = dir;
        gl_reset_objects(0);
        CHECK(ensure_init());
        binaries = s_pc_on;
        for (k = 0; k < 5; k++) {
            key.final0 = 0x5EED0000u + k;
            p = prog_get(&key, NULL);
            standins += p == &s_standin;
            CHECK(p != NULL && p->prog != 0);
        }
        flip();                     /* the list's records written */
        snprintf(path, sizeof path, "%s/shadercache/00000000/programs.list", dir);
        printf("12. shaders %s, binary cache %s, %u stand-ins with a full wait; list %s\n",
               s_async == NV_ASYNC_WORKER ? "on a worker" : s_async == NV_ASYNC_KHR ? "KHR parallel" : "at the draw",
               binaries ? "on" : "off (no binary format)", standins, access(path, R_OK) == 0 ? "written" : "absent");
        CHECK(standins == 0);
        if (binaries) {
            DIR *d;
            struct dirent *de;
            CHECK(access(path, R_OK) == 0 && s_npl_known == 5);
            /* A broken record at the end (a run killed while appending): cut off. */
            {
                FILE *f = fopen(path, "ab");
                if (f) {
                    fwrite("P\x01\x02", 3, 1, f);
                    fclose(f);
                }
            }
            /* A driver update: the binaries gone, the list kept. */
            snprintf(path, sizeof path, "%s/shadercache/00000000", dir);
            if ((d = opendir(path)) != NULL) {
                while ((de = readdir(d)) != NULL)
                    if (strstr(de->d_name, ".bin")) {
                        char f[1500];
                        snprintf(f, sizeof f, "%s/%s", path, de->d_name);
                        remove(f);
                    }
                closedir(d);
            }
            gl_reset_objects(0);
            CHECK(ensure_init());
            host_shader_progress_text(badge, sizeof badge);
            printf("    fresh run: %d programs to compile ahead, badge \"%s\"\n", s_npl_todo, badge);
            if (s_async) {
                CHECK(s_npl_todo == 5 && strcmp(badge, "Shaders 1/5") == 0);
                /* The first list program claimed by a draw while it compiles (no
                 * wait), never asked for again: the flip installs it, the badge
                 * still closes. */
                flip();
                s_wait_max_us = 0;
                key.final0 = 0x5EED0000u;
                p = prog_get(&key, NULL);
                s_wait_max_us = saved_wait;
                printf("    a list program drawn while compiling: %s\n",
                       p == &s_standin ? "stand-in" : p && p->prog ? "ready" : "refused");
                CHECK(p != NULL);
                while (s_pl_todo && flips < 4000) {
                    flip();
                    flips++;
                    usleep(1000);
                }
                host_shader_progress_text(badge, sizeof badge);
                printf("    warm-up done after %d flips: %d programs warm, %d in flight, badge \"%s\"\n",
                       flips, s_nwarm, s_ahead_inflight, badge);
                CHECK(!s_pl_todo && s_nwarm == 4 && s_ahead_inflight == 0 && badge[0] == 0);
                for (k = 0; k < 5; k++) {
                    uint32_t loads = s_st.cache_loads, compiles = s_st.compiles;
                    key.final0 = 0x5EED0000u + k;
                    p = prog_get(&key, NULL);
                    CHECK(p != NULL && p != &s_standin && p->prog != 0 && !p->pending);
                    /* the first in the table already, the others warm: no compile */
                    CHECK(s_st.compiles == compiles && s_st.cache_loads == loads + (k ? 1u : 0u));
                }
                CHECK(s_nwarm == 0);
            } else {
                CHECK(s_npl_todo == 0 && badge[0] == 0);   /* compiled at the draw: nothing ahead */
            }
        }
        gl_reset_objects(0);
        s_save_dir = NULL;
        snprintf(path, sizeof path, "%s/shadercache/00000000", dir);
        rm_files(path);
        rmdir(path);
        snprintf(path, sizeof path, "%s/shadercache", dir);
        rmdir(path);
        rmdir(dir);
        CHECK(ensure_init());
    }

    CHECK(glGetError() == GL_NO_ERROR);
    if (!s_fail)
        printf("GL back end (%s, guest CPU writes %s): ok\n", NV2A_GL_ES ? "GLES" : "GL",
               s_cpu_on ? "on" : "off");
    return s_fail;
}
