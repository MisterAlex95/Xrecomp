/* test_grab.c -- host_window_screenshot_bmp of a picture presented as a texture (host_window_present_texture): the
 * texture is remembered, and a screenshot asked from another thread is drawn by the presenting thread into an RGBA8
 * picture of its own and read back, at its next present or pump (the X360 runtime, the GameCube's GX and GPU-YUV
 * paths; the Android report screenshot, cover shot and panel screenshot).
 *
 * host_window.c is compiled into this program (as test_fg_readback.c does) for its GL entry points and state.
 * Exits 77 (skipped) headless or without a GL context. */
#include "../../src/video/host_window.c"

#include <dirent.h>
#include <unistd.h>

static int s_failed;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);       \
            s_failed = 1;                                                        \
        }                                                                        \
    } while (0)

#define XGL_RGBA16F           0x881A
#define XGL_FLOAT             0x1406
#define XGL_RED               0x1903
#define XGL_BLUE              0x1905
#define XGL_TEXTURE_SWIZZLE_R 0x8E42
#define XGL_TEXTURE_SWIZZLE_B 0x8E44

enum { WIN_W = 640, WIN_H = 480 };

static char s_bmp[512], s_ppm[512], s_ppm2[512], s_dir[512];
static const uint8_t TOP[3] = {200, 60, 30}, BOTTOM[3] = {30, 90, 220}, OTHER[3] = {20, 180, 70};

static uint64_t now_ms(void)
{
    return xbox_HostNowNs() / 1000000ull;
}

static int near(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 3; i++)
        if (abs((int)a[i] - (int)b[i]) > 2)
            return 0;
    return 1;
}

/* A texture of w x h, its upper half `top`, its lower half `bottom`, as the picture shows them; top_down 0: its row
 * 0 is the picture's bottom (a GL render). */
static unsigned texture(int w, int h, const uint8_t *top, const uint8_t *bottom, int top_down)
{
    unsigned t = 0;
    uint8_t *px = malloc((size_t)w * h * 4);

    for (int y = 0; y < h; y++) {
        int upper = top_down ? y < h / 2 : y >= h - h / 2;
        for (int x = 0; x < w; x++) {
            uint8_t *o = px + ((size_t)y * w + x) * 4;
            memcpy(o, upper ? top : bottom, 3);
            o[3] = 255;
        }
    }
    s_gl.GenTextures(1, &t);
    s_gl.ActiveTexture(XGL_TEXTURE0);
    s_gl.BindTexture(XGL_TEXTURE_2D, t);
    s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MIN_FILTER, XGL_NEAREST);
    s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MAG_FILTER, XGL_NEAREST);
    s_gl.PixelStorei(XGL_UNPACK_ALIGNMENT, 4);
    s_gl.PixelStorei(XGL_UNPACK_ROW_LENGTH, 0);
    s_gl.TexImage2D(XGL_TEXTURE_2D, 0, (int)XGL_RGBA8, w, h, 0, XGL_RGBA, XGL_UNSIGNED_BYTE, px);
    free(px);
    return t;
}

/* The BMP written: its size, and its pixels as RGB rows from the top. NULL when it is not one of ours. */
static uint8_t *bmp_load(const char *path, int *w, int *h)
{
    uint8_t hdr[54];
    FILE *f = fopen(path, "rb");
    uint8_t *rgb = NULL;

    *w = *h = 0;
    if (!f)
        return NULL;
    if (fread(hdr, 1, sizeof hdr, f) == sizeof hdr && hdr[0] == 'B' && hdr[1] == 'M' && hdr[28] == 24) {
        int32_t bw, bh;
        memcpy(&bw, hdr + 18, 4);
        memcpy(&bh, hdr + 22, 4);
        size_t row = ((size_t)bw * 3 + 3) & ~(size_t)3;
        uint8_t *line = malloc(row);
        rgb = bw > 0 && bh > 0 ? malloc((size_t)bw * bh * 3) : NULL;
        for (int y = bh - 1; rgb && y >= 0; y--) {   /* bottom-up */
            if (fread(line, 1, row, f) != row) {
                free(rgb);
                rgb = NULL;
                break;
            }
            for (int x = 0; x < bw; x++) {
                uint8_t *o = rgb + ((size_t)y * bw + x) * 3;
                o[0] = line[x * 3 + 2];
                o[1] = line[x * 3 + 1];
                o[2] = line[x * 3 + 0];
            }
        }
        free(line);
        if (rgb) {
            *w = bw;
            *h = bh;
        }
    }
    fclose(f);
    unlink(path);
    return rgb;
}

/* The BMP is w x h, its upper quarter line `top`, its lower `bottom` (at the middle column). */
static int bmp_shows(int w, int h, const uint8_t *top, const uint8_t *bottom)
{
    int bw, bh;
    uint8_t *rgb = bmp_load(s_bmp, &bw, &bh);
    int ok = rgb && bw == w && bh == h;

    if (ok) {
        const uint8_t *t = rgb + ((size_t)(bh / 4) * bw + bw / 2) * 3;
        const uint8_t *b = rgb + ((size_t)(bh * 3 / 4) * bw + bw / 2) * 3;
        ok = near(t, top) && near(b, bottom);
        if (!ok)
            fprintf(stderr, "  bmp %dx%d: top %u,%u,%u bottom %u,%u,%u\n", bw, bh, t[0], t[1], t[2], b[0], b[1], b[2]);
    } else {
        fprintf(stderr, "  bmp %dx%d, want %dx%d\n", bw, bh, w, h);
    }
    free(rgb);
    return ok;
}

/* ── A screenshot from another thread ── */
typedef struct {
    pthread_t th;
    int rc, done;   /* done: atomic */
    uint64_t ms;    /* how long the call took */
} Shot;

static void *shot_main(void *arg)
{
    Shot *s = arg;
    uint64_t t0 = now_ms();
    s->rc = host_window_screenshot_bmp(s_bmp);
    s->ms = now_ms() - t0;
    __atomic_store_n(&s->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void shot_start(Shot *s)
{
    memset(s, 0, sizeof *s);
    s->rc = -2;
    pthread_create(&s->th, NULL, shot_main, s);
}

/* The presenting thread meanwhile: present tex again each frame (present 1), or only pump (a paused game), or do
 * nothing (present -1), until the screenshot returns. */
static int shot_while(unsigned tex, int w, int h, int top_down, int present)
{
    Shot s;

    shot_start(&s);
    uint64_t t0 = now_ms();
    while (!__atomic_load_n(&s.done, __ATOMIC_ACQUIRE) && now_ms() - t0 < 5000) {
        if (present > 0)
            CHECK(host_window_present_texture(tex, w, h, top_down));
        else if (present == 0)
            host_window_pump();
        SDL_Delay(4);
    }
    pthread_join(s.th, NULL);
    CHECK(s.ms < 2000);
    return s.rc;
}

static void test_presenting(void)
{
    printf("a screenshot from another thread while the game presents\n");
    unsigned t = texture(320, 240, TOP, BOTTOM, 1);
    CHECK(host_window_present_texture(t, 320, 240, 1));
    CHECK(shot_while(t, 320, 240, 1, 1) == 0);
    CHECK(bmp_shows(320, 240, TOP, BOTTOM));
    CHECK(s_gl.GetError() == 0);
    s_gl.DeleteTextures(1, &t);
}

static void test_paused(void)
{
    printf("a screenshot from another thread while the game is paused (pump only)\n");
    unsigned t = texture(320, 240, TOP, BOTTOM, 1);
    CHECK(host_window_present_texture(t, 320, 240, 1));
    for (int i = 0; i < 3; i++) {   /* again and again: nothing latched, no stall */
        CHECK(shot_while(t, 320, 240, 1, 0) == 0);
        CHECK(bmp_shows(320, 240, TOP, BOTTOM));
    }
    CHECK(s_gl.GetError() == 0);
    /* On the presenting thread itself: read at once. */
    CHECK(host_window_screenshot_bmp(s_bmp) == 0);
    CHECK(bmp_shows(320, 240, TOP, BOTTOM));
    s_gl.DeleteTextures(1, &t);
}

static void test_orientation(void)
{
    printf("orientation\n");
    for (int td = 0; td <= 1; td++) {
        unsigned t = texture(160, 120, TOP, BOTTOM, td);
        CHECK(host_window_present_texture(t, 160, 120, td));
        CHECK(shot_while(t, 160, 120, td, 0) == 0);
        if (!bmp_shows(160, 120, TOP, BOTTOM)) {
            fprintf(stderr, "  top_down %d\n", td);
            s_failed = 1;
        }
        s_gl.DeleteTextures(1, &t);
    }
    CHECK(s_gl.GetError() == 0);
}

/* The X360 frame texture's channel order is its sampler swizzle: the screenshot shows what the window shows. */
static void test_swizzle(void)
{
    printf("swizzled texture\n");
    unsigned t = texture(160, 120, TOP, BOTTOM, 1);
    s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_SWIZZLE_R, XGL_BLUE);
    s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_SWIZZLE_B, XGL_RED);
    if (s_gl.GetError() != 0) {
        printf("  no texture swizzle here, skipped\n");
        s_gl.DeleteTextures(1, &t);
        return;
    }
    const uint8_t top[3] = {TOP[2], TOP[1], TOP[0]}, bottom[3] = {BOTTOM[2], BOTTOM[1], BOTTOM[0]};
    CHECK(host_window_present_texture(t, 160, 120, 1));
    CHECK(shot_while(t, 160, 120, 1, 0) == 0);
    CHECK(bmp_shows(160, 120, top, bottom));
    CHECK(s_gl.GetError() == 0);
    s_gl.DeleteTextures(1, &t);
}

/* An X360 frame of a float format is an RGBA16F texture: sampled, not read as bytes. */
static void test_float(void)
{
    enum { W = 64, H = 48 };
    static float px[W * H * 4];
    unsigned t = 0;

    printf("RGBA16F texture\n");
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t *c = y < H / 2 ? TOP : BOTTOM;
            for (int i = 0; i < 3; i++)
                px[(y * W + x) * 4 + i] = (float)c[i] / 255.0f;
            px[(y * W + x) * 4 + 3] = 1.0f;
        }
    s_gl.GenTextures(1, &t);
    s_gl.BindTexture(XGL_TEXTURE_2D, t);
    s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MIN_FILTER, XGL_NEAREST);
    s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MAG_FILTER, XGL_NEAREST);
    s_gl.TexImage2D(XGL_TEXTURE_2D, 0, XGL_RGBA16F, W, H, 0, XGL_RGBA, XGL_FLOAT, px);
    if (s_gl.GetError() != 0) {
        printf("  no RGBA16F texture here, skipped\n");
        s_gl.DeleteTextures(1, &t);
        return;
    }
    CHECK(host_window_present_texture(t, W, H, 1));
    CHECK(shot_while(t, W, H, 1, 0) == 0);
    CHECK(bmp_shows(W, H, TOP, BOTTOM));
    CHECK(s_gl.GetError() == 0);
    s_gl.DeleteTextures(1, &t);
}

/* What is shown last wins, whichever path showed it. */
static void test_stale(void)
{
    uint8_t a[64 * 48 * 4];

    printf("the picture shown last, from either path\n");
    for (int i = 0; i < 64 * 48; i++) {
        memcpy(a + i * 4, OTHER, 3);
        a[i * 4 + 3] = 255;
    }
    unsigned t = texture(160, 120, TOP, BOTTOM, 1);
    CHECK(host_window_present_rgba(a, 64, 48, 64 * 4));
    CHECK(host_window_present_texture(t, 160, 120, 1));
    CHECK(shot_while(t, 160, 120, 1, 0) == 0);
    CHECK(bmp_shows(160, 120, TOP, BOTTOM));
    CHECK(host_window_present_texture(t, 160, 120, 1));
    CHECK(host_window_present_rgba(a, 64, 48, 64 * 4));
    CHECK(shot_while(t, 160, 120, 1, 0) == 0);
    CHECK(bmp_shows(64, 48, OTHER, OTHER));
    CHECK(s_gl.GetError() == 0);
    s_gl.DeleteTextures(1, &t);
}

/* The texture deleted after its present: -1 at once, and nothing kept from it (the next one works). */
static void test_deleted(void)
{
    printf("texture deleted after its present\n");
    unsigned t = texture(160, 120, TOP, BOTTOM, 1);
    CHECK(host_window_present_texture(t, 160, 120, 1));
    s_gl.DeleteTextures(1, &t);
    Shot s;
    shot_start(&s);
    uint64_t t0 = now_ms();
    while (!__atomic_load_n(&s.done, __ATOMIC_ACQUIRE) && now_ms() - t0 < 5000) {
        host_window_pump();
        SDL_Delay(4);
    }
    pthread_join(s.th, NULL);
    CHECK(s.rc == -1);
    CHECK(s.ms < 300);
    unlink(s_bmp);
    s_gl.GetError();   /* a present of a deleted texture */
    t = texture(160, 120, OTHER, TOP, 1);
    CHECK(host_window_present_texture(t, 160, 120, 1));
    CHECK(shot_while(t, 160, 120, 1, 0) == 0);
    CHECK(bmp_shows(160, 120, OTHER, TOP));
    CHECK(s_read_failed == 0);
    CHECK(s_gl.GetError() == 0);
    s_gl.DeleteTextures(1, &t);
}

/* Nobody presents or pumps: -1 after the bound, and a later request is served again. */
static void test_no_pump(void)
{
    printf("nothing pumps\n");
    unsigned t = texture(160, 120, TOP, BOTTOM, 1);
    CHECK(host_window_present_texture(t, 160, 120, 1));
    Shot s;
    shot_start(&s);
    pthread_join(s.th, NULL);
    CHECK(s.rc == -1);
    CHECK(s.ms >= 400 && s.ms < 1500);
    CHECK(__atomic_load_n(&s_grab_req, __ATOMIC_RELAXED) == 0);   /* withdrawn */
    unlink(s_bmp);
    CHECK(shot_while(t, 160, 120, 1, 0) == 0);
    CHECK(bmp_shows(160, 120, TOP, BOTTOM));
    s_gl.DeleteTextures(1, &t);
}

/* The PPM host_window_capture wrote, as bytes; NULL when there is none. */
static uint8_t *file_bytes(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;

    *n = 0;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0 && (b = malloc((size_t)len)) && fread(b, 1, (size_t)len, f) == (size_t)len)
        *n = (size_t)len;
    fclose(f);
    unlink(path);
    return b;
}

/* A grab leaves the window's drawing as it was: the next present's drawable is the same, and the renderer's pack
 * state is untouched. */
static void test_window_unchanged(void)
{
    int v = 0;

    printf("the window's picture and state after a grab\n");
    unsigned t = texture(160, 120, TOP, BOTTOM, 0);
    CHECK(host_window_present_texture(t, 160, 120, 0));
    host_window_capture(s_ppm);
    CHECK(host_window_present_texture(t, 160, 120, 0));
    s_gl.PixelStorei(XGL_PACK_ALIGNMENT, 1);
    CHECK(shot_while(t, 160, 120, 0, 0) == 0);
    unlink(s_bmp);
    s_gl.GetIntegerv(XGL_PACK_ALIGNMENT, &v);
    CHECK(v == 1);
    s_gl.PixelStorei(XGL_PACK_ALIGNMENT, 4);
    host_window_capture(s_ppm2);
    CHECK(host_window_present_texture(t, 160, 120, 0));
    size_t na, nb;
    uint8_t *a = file_bytes(s_ppm, &na), *b = file_bytes(s_ppm2, &nb);
    CHECK(a && b && na == nb && na > 0 && memcmp(a, b, na) == 0);
    free(a);
    free(b);
    CHECK(s_gl.GetError() == 0);
    s_gl.DeleteTextures(1, &t);
}

/* The shape it was shown at (a PlayStation-like 640x448 at 4:3), and the 1920 cap on a large picture. */
static void test_aspect_and_cap(void)
{
    printf("aspect and size cap\n");
    unsigned t = texture(640, 448, TOP, BOTTOM, 1);
    host_window_set_aspect(4, 3);
    CHECK(host_window_present_texture(t, 640, 448, 1));
    host_window_set_aspect(0, 0);   /* the shape recorded at the present is the one used */
    CHECK(shot_while(t, 640, 448, 1, 0) == 0);
    CHECK(bmp_shows(640, 480, TOP, BOTTOM));
    s_gl.DeleteTextures(1, &t);
    t = texture(2400, 1200, TOP, BOTTOM, 0);
    CHECK(host_window_present_texture(t, 2400, 1200, 0));
    CHECK(shot_while(t, 2400, 1200, 0, 0) == 0);
    CHECK(bmp_shows(1920, 960, TOP, BOTTOM));
    CHECK(s_gl.GetError() == 0);
    s_gl.DeleteTextures(1, &t);
}

/* F12 on a texture present: the file is this present's picture, through the grab. */
static void test_f12(void)
{
    printf("F12 on a texture present\n");
    unsigned t = texture(160, 120, TOP, BOTTOM, 1);
    s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_SWIZZLE_R, XGL_BLUE);
    s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_SWIZZLE_B, XGL_RED);
    int swz = s_gl.GetError() == 0;
    host_window_request_screenshot();
    CHECK(host_window_present_texture(t, 160, 120, 1));
    CHECK(__atomic_load_n(&s_shot_req, __ATOMIC_RELAXED) == 0);
    /* The F12 file itself: this picture. */
    DIR *d = opendir(s_dir);
    struct dirent *e;
    int files = 0;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char path[1100];
        snprintf(path, sizeof path, "%s/%s", s_dir, e->d_name);
        rename(path, s_bmp);
        files++;
    }
    if (d)
        closedir(d);
    CHECK(files == 1);
    const uint8_t top[3] = {TOP[2], TOP[1], TOP[0]}, bottom[3] = {BOTTOM[2], BOTTOM[1], BOTTOM[0]};
    CHECK(bmp_shows(160, 120, swz ? top : TOP, swz ? bottom : BOTTOM));
    /* The kept picture is the grab: read it back from here. */
    CHECK(host_window_screenshot_bmp(s_bmp) == 0);
    CHECK(bmp_shows(160, 120, swz ? top : TOP, swz ? bottom : BOTTOM));
    s_gl.DeleteTextures(1, &t);
}

/* host_window_close releases a screenshot waiting for a presenting thread that will not come. */
static void test_close_releases(void)
{
    printf("close releases a waiting screenshot\n");
    unsigned t = texture(160, 120, TOP, BOTTOM, 1);
    CHECK(host_window_present_texture(t, 160, 120, 1));
    s_gl.DeleteTextures(1, &t);
    Shot s;
    shot_start(&s);
    while (!__atomic_load_n(&s_grab_req, __ATOMIC_ACQUIRE))
        SDL_Delay(1);
    uint64_t t0 = now_ms();
    host_window_close();
    pthread_join(s.th, NULL);
    CHECK(s.rc == -1);
    CHECK(now_ms() - t0 < 300);
    unlink(s_bmp);
    CHECK(host_window_screenshot_bmp(s_bmp) == -1);   /* closed: nothing shown */
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    const char *dir = tmp && tmp[0] ? tmp : "/tmp";

    if (host_window_headless()) {
        printf("grab: headless, skipped\n");
        return 77;
    }
    snprintf(s_bmp, sizeof s_bmp, "%s/xbr_grab_%d.bmp", dir, (int)getpid());
    snprintf(s_ppm, sizeof s_ppm, "%s/xbr_grab_%d_a.ppm", dir, (int)getpid());
    snprintf(s_ppm2, sizeof s_ppm2, "%s/xbr_grab_%d_b.ppm", dir, (int)getpid());
    snprintf(s_dir, sizeof s_dir, "%s/xbr_grab_%d", dir, (int)getpid());
    setenv("RECOMP_SCREENSHOT_DIR", s_dir, 1);
    if (!host_window_open("grab", WIN_W, WIN_H, 0)) {
        printf("grab: no window with a GL context, skipped\n");
        return 77;
    }
    CHECK(host_window_screenshot_bmp(s_bmp) == -1);   /* nothing shown yet */
    test_presenting();
    test_paused();
    test_orientation();
    test_swizzle();
    test_float();
    test_stale();
    test_deleted();
    test_no_pump();
    test_window_unchanged();
    test_aspect_and_cap();
    test_f12();
    test_close_releases();
    rmdir(s_dir);
    SDL_Quit();
    printf("grab: %s\n", s_failed ? "FAILED" : "ok");
    return s_failed;
}
