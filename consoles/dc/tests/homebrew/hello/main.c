/* hello: the Dreamcast spike's test program (XBA-227). KallistiOS boot (no maple, no CD-ROM), console text on the
 * serial port (KOS's dbgio over the SCIF) and on the framebuffer (640x480 RGB565, KOS's own minifont: no BIOS font),
 * an SH-4 check (integer division and 64-bit multiplies through libgcc, FPU single and double precision, FSQRT /
 * FSRRA / FSCA / FIPR / FTRV, the 64-bit FMOV pairs and the XD bank that mat_load uses), a second thread (the
 * scheduler's context switches go through the TMU0 interrupt, banked registers and RTE), VBlanks.
 *
 * Every line the test checks starts with "hello: "; the KOS banner before them is not checked (it holds the
 * toolchain's build date). Expected output: consoles/dc/tests/hello_serial.txt and hello_fb.png. */
#include <kos.h>
#include <dc/fmath.h>
#include <dc/matrix.h>
#include <dc/minifont.h>
#include <stdint.h>
#include <stdio.h>

KOS_INIT_FLAGS(INIT_IRQ | INIT_FS_ALL);

static volatile int32_t vi[4] = {1000003, 97, -77777, 4096};
static volatile float vf[4] = {2.0f, 4.0f, 0.5f, 3.0f};
static volatile double vd[2] = {1.0, 3.0};
static volatile int worker_count;
static volatile int vblanks;

static int line_y = 40;

static void say(const char *s)
{
    printf("hello: %s\n", s);
    minifont_draw_str(vram_s + line_y * 640 + 40, 640, s);
    line_y += 20;
}

static void *worker(void *arg)
{
    int n = (int)(intptr_t)arg;
    for (int i = 0; i < n; i++) {
        worker_count++;
        thd_pass();
    }
    return (void *)(intptr_t)(n * 3);
}

static void on_vblank(uint32_t code, void *data)
{
    (void)code;
    (void)data;
    vblanks++;
}

static int round_i(float f)
{
    return (int)(f < 0 ? f - 0.5f : f + 0.5f);
}

int main(void)
{
    char buf[96];
    uint32_t fail = 0;

    vid_set_mode(DM_640x480, PM_RGB565);
    vid_clear(0, 0, 64);
    say("Hello, Dreamcast! (XRecomp SH-4 spike)");

    /* 0: 32-bit division (libgcc: DIV0S / DIV1 loops) */
    int32_t q = vi[0] / vi[1], r = vi[0] % vi[1];
    uint32_t uq = (uint32_t)vi[2] / (uint32_t)vi[1];
    if (q != 10309 || r != 30 || uq != 44277211u)
        fail |= 1u << 0;
    /* 1: 64-bit multiplies (DMULS.L / DMULU.L) and a 64-bit division */
    int64_t p = (int64_t)vi[2] * vi[0];
    uint64_t up = (uint64_t)(uint32_t)vi[2] * (uint32_t)vi[0];
    if (p != -77777233331ll || up != 4294902403668557ull || p / vi[3] != -18988582)
        fail |= 1u << 1;
    /* 2: single precision: FSQRT, FDIV, FTRC, FLOAT */
    float s2 = fsqrt(vf[0]);
    int fs = round_i(s2 * 10000.0f), fd = round_i(vf[3] / vf[1] * 1000.0f), fl = round_i((float)vi[1] * vf[2]);
    if (fs != 14142 || fd != 750 || fl != 49)
        fail |= 1u << 2;
    /* 3: FSRRA and FSCA (approximations on hardware: rounded to 3 digits) */
    int rs = round_i(frsqrt(vf[1]) * 1000.0f);
    float fs90, fc90;
    fsincos(vf[0] * 45.0f, &fs90, &fc90); /* degrees: FSCA at 0x4000 */
    int sn = round_i(fs90 * 1000.0f), cs = round_i(fc90 * 1000.0f);
    int s30 = round_i(fsin(F_PI / 6.0f) * 1000.0f);
    if (rs != 500 || sn != 1000 || cs != 0 || s30 != 500)
        fail |= 1u << 3;
    /* 4: FIPR */
    int dot = round_i(fipr(1.0f, 2.0f, 3.0f, 4.0f, vf[0] + 3.0f, 6.0f, 7.0f, 8.0f));
    if (dot != 70)
        fail |= 1u << 4;
    /* 5: FTRV through mat_load (FSCHG + 64-bit FMOV into XD, FRCHG) and mat_transform (x / w, y / w, 1 / w) */
    static matrix_t m __attribute__((aligned(32))) = {
        {2.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 3.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 4.0f, 0.0f}, {10.0f, 20.0f, 30.0f, 1.0f}};
    static vector_t in[2] __attribute__((aligned(32))) = {{1.0f, 1.0f, 1.0f, 1.0f}, {-1.0f, 2.0f, 0.5f, 1.0f}};
    static vector_t out[2] __attribute__((aligned(32)));
    mat_load(&m);
    mat_transform(in, out, 2, sizeof(vector_t));
    int t0 = round_i(out[0].x) * 10000 + round_i(out[0].y) * 100 + round_i(out[0].z);
    int t1 = round_i(out[1].x) * 10000 + round_i(out[1].y) * 100 + round_i(out[1].z);
    if (t0 != 122301 || t1 != 82601)
        fail |= 1u << 5;
    /* 6: double precision (FPSCR.PR toggled by GCC around double code with -m4-single) */
    double third = vd[0] / vd[1];
    int dthird = (int)(third * 1e9);
    if (dthird != 333333333)
        fail |= 1u << 6;

    snprintf(buf, sizeof buf, "div %ld %ld %lu", (long)q, (long)r, (unsigned long)uq);
    say(buf);
    snprintf(buf, sizeof buf, "mul64 %lld %llu", (long long)p, (unsigned long long)up);
    say(buf);
    snprintf(buf, sizeof buf, "fpu %d %d %d rsqrt %d sin %d cos %d sin30 %d", fs, fd, fl, rs, sn, cs, s30);
    say(buf);
    snprintf(buf, sizeof buf, "fipr %d ftrv %d %d double %d", dot, t0, t1, dthird);
    say(buf);
    if (fail)
        snprintf(buf, sizeof buf, "SH-4: FAIL %04lx", (unsigned long)fail);
    else
        snprintf(buf, sizeof buf, "SH-4: ok");
    say(buf);

    /* 7: a second thread (context switches) */
    kthread_t *t = thd_create(0, worker, (void *)(intptr_t)5);
    void *ret = NULL;
    for (int i = 0; i < 5; i++)
        thd_pass();
    thd_join(t, &ret);
    snprintf(buf, sizeof buf, "thread %d ret %d", worker_count, (int)(intptr_t)ret);
    say(buf);

    /* 8: 30 VBlank interrupts (Holly, IRL) against the millisecond timer (TMU): 500 ms at 60 Hz */
    int h = vblank_handler_add(on_vblank, NULL);
    int v0 = vblanks;
    while (vblanks == v0)
        ;
    uint64_t ms0 = timer_ms_gettime64();
    v0 = vblanks;
    while (vblanks - v0 < 30)
        thd_pass();
    uint64_t ms = timer_ms_gettime64() - ms0;
    vblank_handler_remove(h);
    snprintf(buf, sizeof buf, "vblank irq 30, timer %s", ms >= 450 && ms <= 550 ? "ok" : "off");
    say(buf);
    if (ms < 450 || ms > 550)
        printf("hello: (timer said %lu ms)\n", (unsigned long)ms);
    say("done");
    /* One more second (the idle thread, thd_sleep), then back to the BIOS (exit code 0). */
    thd_sleep(1000);
    return 0;
}
