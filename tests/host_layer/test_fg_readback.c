/* test_fg_readback.c -- frame generation's read-back of a GPU picture through pack buffers (GC-003).
 *
 * host_window.c is compiled into this program with frame generation on (the host layer's own build leaves it out), so
 * the test sees the read-back's state and can stand in for the state-load counter and fast-forward. Every picture is
 * a texture that names its frame in two different colours, one per half: a capture of the window tells which frame
 * was shown, right way up or not; black, garbage, a wrong size or a flipped picture does not decode.
 *
 * Exits 77 (skipped) headless or without a GL context. */
#define XBOXRECOMP_LSFG 1
#define recomp_states_loads test_states_loads
#define recomp_ff_active test_ff_active
#include "../../src/video/host_window.c"
#undef recomp_states_loads
#undef recomp_ff_active

#include <dirent.h>
#include <unistd.h>

static unsigned s_test_loads;
static int s_test_ff;
unsigned test_states_loads(void) { return s_test_loads; }
int test_ff_active(void) { return s_test_ff; }

static int s_failed;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);       \
            s_failed = 1;                                                        \
        }                                                                        \
    } while (0)

enum { WIN_W = 640, WIN_H = 480, PIC_W = 160, PIC_H = 120 };

static unsigned s_pic;
static uint8_t s_px[400 * 300 * 4];
static char s_ppm[512], s_bmp[512], s_dir[512];

/* Frame k's colours: the top half, then the bottom half. */
static void colour_top(int k, uint8_t *c)
{
    c[0] = (uint8_t)(40 + k * 2);
    c[1] = 200;
    c[2] = 40;
}

static void colour_bottom(int k, uint8_t *c)
{
    c[0] = 40;
    c[1] = (uint8_t)(40 + k * 2);
    c[2] = 200;
}

static int near(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 3; i++)
        if (abs((int)a[i] - (int)b[i]) > 1)
            return 0;
    return 1;
}

/* Which frame these two samples show, -1 when they show none. */
static int decode(const uint8_t *top, const uint8_t *bottom)
{
    for (int k = 0; k < 100; k++) {
        uint8_t t[3], b[3];
        colour_top(k, t);
        colour_bottom(k, b);
        if (near(top, t) && near(bottom, b))
            return k;
    }
    return -1;
}

/* Frame k into the test's texture. top_down 0: the texture's row 0 is the picture's bottom. */
static void picture(int k, int w, int h, int top_down)
{
    uint8_t t[3], b[3];

    colour_top(k, t);
    colour_bottom(k, b);
    for (int y = 0; y < h; y++) {
        int upper = top_down ? y < h / 2 : y >= h - h / 2;
        for (int x = 0; x < w; x++) {
            uint8_t *o = s_px + ((size_t)y * w + x) * 4;
            memcpy(o, upper ? t : b, 3);
            o[3] = 255;
        }
    }
    s_gl.ActiveTexture(XGL_TEXTURE0);
    s_gl.BindTexture(XGL_TEXTURE_2D, s_pic);
    s_gl.PixelStorei(XGL_UNPACK_ALIGNMENT, 4);
    s_gl.PixelStorei(XGL_UNPACK_ROW_LENGTH, 0);
    s_gl.TexImage2D(XGL_TEXTURE_2D, 0, (int)XGL_RGBA8, w, h, 0, XGL_RGBA, XGL_UNSIGNED_BYTE, s_px);
}

static void texture_make(void)
{
    s_gl.GenTextures(1, &s_pic);
    s_gl.BindTexture(XGL_TEXTURE_2D, s_pic);
    s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MIN_FILTER, XGL_NEAREST);
    s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MAG_FILTER, XGL_NEAREST);
}

/* The frame the finished capture shows; -1 when it shows none, -2 when no capture was written. */
static int capture_frame(void)
{
    FILE *f = fopen(s_ppm, "rb");
    int w = 0, h = 0, max = 0, k = -1;

    if (!f)
        return -2;
    if (fscanf(f, "P6 %d %d %d", &w, &h, &max) == 3 && w == WIN_W && h == WIN_H && fgetc(f) != EOF) {
        long base = ftell(f);
        uint8_t top[3] = {0}, bottom[3] = {0};
        fseek(f, base + (long)(h / 4) * w * 3 + (long)(w / 2) * 3, SEEK_SET);
        if (fread(top, 1, 3, f) == 3) {
            fseek(f, base + (long)(h * 3 / 4) * w * 3 + (long)(w / 2) * 3, SEEK_SET);
            if (fread(bottom, 1, 3, f) == 3)
                k = decode(top, bottom);
        }
    }
    fclose(f);
    unlink(s_ppm);
    return k;
}

/* The end of a scenario: what the FIFO still holds is shown and gone, and the picture in flight, which the next
 * present would rightly take, is dropped so that it does not count as a frame of the next scenario. */
static void drain(void)
{
    host_window_sleep_until(xbox_HostNowNs() + 150000000ull);
    s_rb.pending = 0;
}

typedef struct {
    int seen;      /* captures that showed a frame */
    int last;      /* the newest frame shown */
} Run;

/* Frames first .. first + n - 1 at the pace of a 30 fps game. Each capture must show one of them, never going back,
 * never ahead of the frame presented. */
static void run_frames(Run *r, int first, int n, int w, int h, int top_down)
{
    r->seen = 0;
    r->last = first - 1;
    unlink(s_ppm);
    s_capture[0] = 0;
    for (int k = first; k < first + n; k++) {
        host_window_pump();
        picture(k, w, h, top_down);
        if (!s_capture[0])
            host_window_capture(s_ppm);
        CHECK(host_window_present_texture(s_pic, w, h, top_down));
        host_window_sleep_until(xbox_HostNowNs() + 33000000ull);
        if (!s_capture[0]) {
            int shown = capture_frame();
            if (shown != -2) {
                if (shown < first || shown > k || shown < r->last)
                    fprintf(stderr, "frame %d: the window shows %d (from %d, last %d)\n", k, shown, first, r->last);
                CHECK(shown >= first && shown <= k && shown >= r->last);
                r->last = shown;
                r->seen++;
            }
        }
    }
    host_window_capture(NULL);
    unlink(s_ppm);
    CHECK(s_gl.GetError() == 0);
}

/* A steady run shows pictures, recent ones, through the pack buffers. */
static void run_steady(int first, int n, int w, int h, int top_down)
{
    Run r;
    uint64_t mapped = s_rb_mapped, direct = s_rb_direct;

    run_frames(&r, first, n, w, h, top_down);
    printf("  %dx%d top_down %d: %d captures, last frame %d of %d; %llu mapped, %llu synchronous\n", w, h, top_down,
           r.seen, r.last, first + n - 1, (unsigned long long)(s_rb_mapped - mapped),
           (unsigned long long)(s_rb_direct - direct));
    CHECK(r.seen >= n / 2);
    CHECK(r.last >= first + n - 6);
    CHECK(s_rb_mapped - mapped >= (uint64_t)n - 2);
    CHECK(s_rb_direct - direct <= 2);
}

static void test_orientation_and_size(void)
{
    printf("orientation and size\n");
    run_steady(0, 24, PIC_W, PIC_H, 1);
    run_steady(30, 24, PIC_W, PIC_H, 0);       /* the flip in the mapping copy */
    run_steady(60, 16, 400, 300, 1);           /* a larger picture: the buffers grow */
    run_steady(80, 16, 80, 60, 0);             /* a smaller one in the larger buffers */
}

/* The size changes on the very next present: what is in flight has the old size and is not taken. */
static void test_size_every_present(void)
{
    static const int sizes[3][2] = {{PIC_W, PIC_H}, {400, 300}, {80, 60}};
    uint64_t direct = s_rb_direct;

    printf("a size change at every present\n");
    for (int k = 0; k < 12; k++) {
        const int *s = sizes[k % 3];
        picture(k, s[0], s[1], 1);
        CHECK(host_window_present_texture(s_pic, s[0], s[1], 1));
        CHECK(s_rb.pending && s_rb.w == s[0] && s_rb.h == s[1]);
        host_window_sleep_until(xbox_HostNowNs() + 33000000ull);
    }
    CHECK(s_rb_direct - direct == 12);
    CHECK(s_gl.GetError() == 0);
}

/* F12 on a present frame generation takes: the picture of this present, not the one in flight. */
static void test_screenshot_is_current(void)
{
    Run r;
    uint8_t hdr[54], top[3] = {0}, bottom[3] = {0};

    printf("screenshot of the current picture\n");
    drain();
    run_frames(&r, 0, 8, PIC_W, PIC_H, 1);
    CHECK(s_rb.pending);
    picture(50, PIC_W, PIC_H, 1);
    host_window_request_screenshot();
    CHECK(host_window_present_texture(s_pic, PIC_W, PIC_H, 1));
    CHECK(host_window_screenshot_bmp(s_bmp) == 0);
    FILE *f = fopen(s_bmp, "rb");
    CHECK(f != NULL);
    if (f) {
        CHECK(fread(hdr, 1, sizeof hdr, f) == sizeof hdr);
        long row = (PIC_W * 3 + 3) & ~3L;
        fseek(f, 54 + row * (PIC_H * 3 / 4) + (PIC_W / 2) * 3, SEEK_SET);   /* bottom-up: the picture's top half */
        CHECK(fread(top, 1, 3, f) == 3);
        fseek(f, 54 + row * (PIC_H / 4) + (PIC_W / 2) * 3, SEEK_SET);
        CHECK(fread(bottom, 1, 3, f) == 3);
        fclose(f);
        uint8_t t[3] = {top[2], top[1], top[0]}, b[3] = {bottom[2], bottom[1], bottom[0]};
        CHECK(decode(t, b) == 50);
    }
    unlink(s_bmp);
    /* The F12 file itself. */
    DIR *d = opendir(s_dir);
    struct dirent *e;
    int files = 0;
    char path[1024];
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", s_dir, e->d_name);
        unlink(path);
        files++;
    }
    if (d)
        closedir(d);
    CHECK(files == 1);
}

/* The renderer's pack state goes through a present untouched, and does not disturb the read. */
static void test_pack_state(void)
{
    unsigned theirs = 0;
    int v = 0;
    Run r;

    printf("pack state\n");
    drain();
    run_frames(&r, 0, 4, PIC_W, PIC_H, 1);
    void (APIENTRY *gen)(int, unsigned *) = (void (APIENTRY *)(int, unsigned *))SDL_GL_GetProcAddress("glGenBuffers");
    void (APIENTRY *del)(int, const unsigned *) =
        (void (APIENTRY *)(int, const unsigned *))SDL_GL_GetProcAddress("glDeleteBuffers");
    void (APIENTRY *bind)(unsigned, unsigned) =
        (void (APIENTRY *)(unsigned, unsigned))SDL_GL_GetProcAddress("glBindBuffer");
    gen(1, &theirs);
    bind(XGL_PIXEL_PACK_BUFFER, theirs);
    s_gl.PixelStorei(XGL_PACK_ALIGNMENT, 1);
    s_gl.PixelStorei(XGL_PACK_ROW_LENGTH, 7);
    s_gl.PixelStorei(XGL_PACK_SKIP_ROWS, 2);
    s_gl.PixelStorei(XGL_PACK_SKIP_PIXELS, 3);
    uint64_t mapped = s_rb_mapped;
    for (int k = 4; k < 12; k++) {
        picture(k, PIC_W, PIC_H, 1);
        CHECK(host_window_present_texture(s_pic, PIC_W, PIC_H, 1));
        s_gl.GetIntegerv(XGL_PIXEL_PACK_BUFFER_BINDING, &v);
        CHECK(v == (int)theirs);
        s_gl.GetIntegerv(XGL_PACK_ALIGNMENT, &v);
        CHECK(v == 1);
        s_gl.GetIntegerv(XGL_PACK_ROW_LENGTH, &v);
        CHECK(v == 7);
        s_gl.GetIntegerv(XGL_PACK_SKIP_ROWS, &v);
        CHECK(v == 2);
        s_gl.GetIntegerv(XGL_PACK_SKIP_PIXELS, &v);
        CHECK(v == 3);
        host_window_sleep_until(xbox_HostNowNs() + 33000000ull);
    }
    CHECK(s_rb_mapped - mapped >= 7);
    bind(XGL_PIXEL_PACK_BUFFER, 0);
    del(1, &theirs);
    s_gl.PixelStorei(XGL_PACK_ALIGNMENT, 4);
    s_gl.PixelStorei(XGL_PACK_ROW_LENGTH, 0);
    s_gl.PixelStorei(XGL_PACK_SKIP_ROWS, 0);
    s_gl.PixelStorei(XGL_PACK_SKIP_PIXELS, 0);
    CHECK(s_gl.GetError() == 0);
}

/* After event, nothing older than `first` reaches the window, and the next present primed. */
static void after_event(int first)
{
    Run r;
    uint64_t direct = s_rb_direct;

    run_frames(&r, first, 12, PIC_W, PIC_H, 1);
    CHECK(s_rb_direct - direct == 1);
    CHECK(r.seen >= 4 && r.last >= first);
}

/* The buffers deleted so far (s_del_buffers is pointed here). */
static int s_deleted;
static void (APIENTRY *s_real_del)(int, const unsigned *);
static void APIENTRY count_del(int n, const unsigned *names)
{
    s_deleted += n;
    s_real_del(n, names);
}

static void test_events(void)
{
    Run r;

    printf("state load, pause, fast-forward, a new context, frame generation off and on\n");
    drain();
    run_frames(&r, 0, 8, PIC_W, PIC_H, 1);
    CHECK(s_rb.pending);
    s_test_loads++;
    after_event(10);

    CHECK(s_rb.pending);
    s_real_del = s_del_buffers;
    s_del_buffers = count_del;
    s_deleted = 0;
    recomp_lifecycle_pause();
    recomp_lifecycle_resume();
    after_event(30);
    CHECK(s_deleted == 2);   /* made again after the pause */
    CHECK(s_rb.pbo[0] != 0);
    s_del_buffers = s_real_del;

    CHECK(s_rb.pending);
    s_test_ff = 1;
    for (int k = 42; k < 46; k++) {
        picture(k, PIC_W, PIC_H, 1);
        CHECK(host_window_present_texture(s_pic, PIC_W, PIC_H, 1));
        CHECK(!s_rb.pending);
    }
    s_test_ff = 0;
    drain();
    after_event(50);

    /* A picture from another path between two GPU pictures. */
    CHECK(s_rb.pending);
    picture(63, PIC_W, PIC_H, 1);
    CHECK(host_window_present_rgba(s_px, PIC_W, PIC_H, PIC_W * 4));
    CHECK(!s_rb.pending);
    after_event(63);

    /* SDL replaced the context: the names are forgotten, never deleted or used again. */
    unsigned old_pbo = s_rb.pbo[0];
    CHECK(old_pbo != 0);
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_RENDER_DEVICE_RESET;
    SDL_PushEvent(&ev);
    host_window_pump();
    CHECK(__atomic_load_n(&s_read_lost, __ATOMIC_ACQUIRE) == 1);
    drain();
    after_event(78);
    CHECK(s_rb.pbo[0] != 0 && s_rb.pbo[0] != old_pbo && s_read_fb != 0);   /* made again, the old ones never reused */

    /* Frame generation off at run time: the buffers go; on again: they come back. */
    drain();
    s_fg = 0;
    picture(83, PIC_W, PIC_H, 1);
    CHECK(host_window_present_texture(s_pic, PIC_W, PIC_H, 1));
    CHECK(s_rb.pbo[0] == 0 && s_rb.pbo[1] == 0 && !s_rb.pending);
    s_fg = 1;
    after_event(84);
    CHECK(s_rb.pbo[0] != 0);
}

/* Without the entry points everything still works, synchronously. */
static void test_no_pack_buffers(void)
{
    Run r;
    uint64_t mapped = s_rb_mapped;

    printf("no pack buffers\n");
    drain();
    rb_release();
    s_rb.loaded = -1;
    run_frames(&r, 0, 12, PIC_W, PIC_H, 1);
    CHECK(s_rb_mapped == mapped);
    CHECK(s_rb.pbo[0] == 0);
    CHECK(r.seen >= 4);
    s_rb.loaded = 0;
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");

    if (host_window_headless()) {
        printf("fg_readback: headless, skipped\n");
        return 77;
    }
    setenv("RECOMP_LSFG", "1", 1);
    setenv("RECOMP_LSFG_FAKE", "1", 1);
    setenv("RECOMP_LSFG_DISPLAY_HZ", "60", 1);
    snprintf(s_ppm, sizeof s_ppm, "%s/xbr_fg_readback_%d.ppm", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
    snprintf(s_bmp, sizeof s_bmp, "%s/xbr_fg_readback_%d.bmp", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
    snprintf(s_dir, sizeof s_dir, "%s/xbr_fg_readback_%d", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
    setenv("RECOMP_SCREENSHOT_DIR", s_dir, 1);
    if (!host_window_open("fg readback", WIN_W, WIN_H, 0)) {
        printf("fg_readback: no window with a GL context, skipped\n");
        return 77;
    }
    if (!s_fg) {
        printf("fg_readback: frame generation did not start, skipped\n");
        host_window_close();
        return 77;
    }
    texture_make();
    test_orientation_and_size();
    test_size_every_present();
    test_screenshot_is_current();
    test_pack_state();
    test_events();
    test_no_pack_buffers();

    /* Closed and opened again: nothing of the old context is used. */
    printf("close and open again\n");
    drain();
    run_steady(0, 12, PIC_W, PIC_H, 1);
    CHECK(s_rb.pbo[0] != 0);
    s_gl.DeleteTextures(1, &s_pic);
    host_window_close();
    CHECK(s_rb.pbo[0] == 0 && s_rb.pbo[1] == 0 && !s_rb.pending && !s_rb.loaded);
    CHECK(s_read_fb == 0 && s_read_px == NULL && s_read_cap == 0);
    CHECK(host_window_open("fg readback", WIN_W, WIN_H, 0));
    if (s_win && s_fg) {
        texture_make();
        run_steady(20, 12, PIC_W, PIC_H, 0);
        s_gl.DeleteTextures(1, &s_pic);
    }
    host_window_close();
    rmdir(s_dir);
    SDL_Quit();
    printf("fg_readback: %s\n", s_failed ? "FAILED" : "ok");
    return s_failed;
}
