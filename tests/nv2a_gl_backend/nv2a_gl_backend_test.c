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
 * RECOMP_GPU_CPU_WRITES=0 (another ctest entry) checks that the same cases
 * fail without the merge, so the test tells the two apart. */
#include "../../src/gpu/nv2a_gl.c"

#define MEM_BYTES (8u << 20)
#define FB32   0x00100000u          /* 64x32 A8R8G8B8, pitch 256 */
#define FB16   0x00200000u          /* 640x480 R5G6B5 (frame class: scaled) */
#define FBSWZ  0x00400000u          /* 32x32 swizzled A8R8G8B8 */
#define FBHOST 0x00500000u          /* 64x32 A8R8G8B8, host present */

static uint8_t *s_mem;
static int s_fail, s_cpu_on = 1;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; \
} } while (0)

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
const char *xbox_path_save_dir_utf8(void) { return NULL; }
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out)
{ (void)tex; (void)argb_out; return 0; }
uint32_t nv2a_backend_texture_bytes(const Nv2aTexture *tex) { (void)tex; return 0; }

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

int main(void)
{
    const char *e = getenv("RECOMP_GPU_CPU_WRITES");
    Nv2aSurface s32 = surf(FB32, 64, 32, 256, 4, 0x0108);
    uint32_t bad;

    s_cpu_on = !(e && strcmp(e, "0") == 0);
    s_mem = (uint8_t *)calloc(1, MEM_BYTES);
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

    /* 1. Clean target: read back, CPU rectangle, GPU clear elsewhere. */
    present(&s32, 1, NULL);
    flip();
    CHECK(count_bad(FB32, 0, 0, 0, 0, 0, 0) == 0);
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

    CHECK(glGetError() == GL_NO_ERROR);
    if (!s_fail)
        printf("GL back end (%s, guest CPU writes %s): ok\n", NV2A_GL_ES ? "GLES" : "GL",
               s_cpu_on ? "on" : "off");
    return s_fail;
}
