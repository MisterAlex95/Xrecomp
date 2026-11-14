/* The host layer alone (XBA-145): links only xbr_host.
 *
 *   host_layer_test core     time, lifecycle, config, storage, pads and audio
 *                            out, no display (SDL's dummy audio driver)
 *   host_layer_test window   a window, a GL texture presented 120 times (or
 *                            for HOST_LAYER_SECONDS), the pads read each frame;
 *                            exits 77 (skipped) when host_window_headless()
 *                            says so (XBR_BUILD_ONLY, no display)
 */
#include "host_audio.h"
#include "host_cpus.h"
#include "host_pad.h"
#include "host_time.h"
#include "host_window.h"
#include "framegen_pacer.h"
#include "recomp_cfg.h"
#include "recomp_lifecycle.h"
#include "recomp_storage.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int s_fail;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail = 1; } } while (0)

static int s_events;
static void on_lifecycle(int paused, void *user)
{
    (void)paused;
    (void)user;
    s_events++;
}

static int run_core(void)
{
    /* Host time: monotonic, and a sleep sleeps. */
    uint64_t t0 = xbox_HostNowNs();
    xbox_HostSleepNs(2000000);
    uint64_t t1 = xbox_HostNowNs();
    CHECK(t1 > t0);
    CHECK(t1 - t0 >= 1000000);

    /* Lifecycle: one listener call per pause and per resume. */
    CHECK(recomp_lifecycle_listen(on_lifecycle, NULL) == 0);
    recomp_lifecycle_pause();
    recomp_lifecycle_pause();
    CHECK(recomp_lifecycle_paused());
    recomp_lifecycle_resume();
    CHECK(!recomp_lifecycle_paused());
    CHECK(s_events == 2);

    /* Config: recomp.cfg into the environment. */
    char dir[] = "/tmp/xbr_host_XXXXXX";
    const char *tmp = getenv("TMPDIR");
    char tmpl[512];
    snprintf(tmpl, sizeof tmpl, "%s/xbr_host_XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    char *base = mkdtemp(tmpl);
    if (!base)
        base = mkdtemp(dir);
    CHECK(base != NULL);
    if (base) {
        char cfg[600];
        snprintf(cfg, sizeof cfg, "%s/recomp.cfg", base);
        FILE *f = fopen(cfg, "w");
        CHECK(f != NULL);
        if (f) {
            fputs("RECOMP_HOST_LAYER_TEST=42\n", f);
            fclose(f);
            unsetenv("RECOMP_HOST_LAYER_TEST");
            CHECK(recomp_cfg_load(cfg) >= 1);
            const char *v = getenv("RECOMP_HOST_LAYER_TEST");
            CHECK(v && !strcmp(v, "42"));
            unlink(cfg);
        }

        /* Storage, console-agnostic: an entry file of our choosing, then none. */
        recomp_storage st;
        char ext[600], gamefile[1200];
        snprintf(ext, sizeof ext, "%s/ext", base);
        unsetenv("RECOMP_GAME_DIR");
        CHECK(recomp_storage_resolve_entry(ext, base, "Game.GBA", &st) == 0);
        snprintf(gamefile, sizeof gamefile, "%s/game.gba", st.game_dir);
        f = fopen(gamefile, "w");
        CHECK(f != NULL);
        if (f)
            fclose(f);
        CHECK(recomp_storage_resolve_entry(ext, base, "Game.GBA", &st) == 1);
        CHECK(strstr(st.xbe, "game.gba") != NULL);
        CHECK(recomp_storage_resolve_entry(ext, base, NULL, &st) == 0);
        CHECK(st.xbe[0] == '\0');
        unlink(gamefile);
        rmdir(st.game_dir);
        rmdir(ext);
        rmdir(st.save_dir);
        rmdir(base);
    }

    /* The shared options (XBA-181): a console's own key wins, then the shared one, then the default. */
    CHECK(host_window_scale_parse("aspect") == HOST_SCALE_ASPECT);
    CHECK(host_window_scale_parse("Integer") == HOST_SCALE_INTEGER);
    CHECK(host_window_scale_parse("stretch") == HOST_SCALE_STRETCH);
    CHECK(host_window_scale_parse("") == -1 && host_window_scale_parse(NULL) == -1);
    CHECK(host_window_scale_parse("fill") == -1);
    CHECK(host_window_filter_parse("nearest") == HOST_FILTER_NEAREST);
    CHECK(host_window_filter_parse("LINEAR") == HOST_FILTER_LINEAR);
    CHECK(host_window_filter_parse("bilinear") == -1);
    unsetenv("RECOMP_SCALE");
    unsetenv("RECOMP_FILTER");
    unsetenv("RECOMP_TEST_SCALE");
    unsetenv("RECOMP_TEST_FILTER");
    CHECK(host_window_env_scale("RECOMP_TEST_SCALE", HOST_SCALE_INTEGER) == HOST_SCALE_INTEGER);
    CHECK(host_window_env_filter("RECOMP_TEST_FILTER", HOST_FILTER_NEAREST) == HOST_FILTER_NEAREST);
    CHECK(host_window_env_filter("RECOMP_TEST_FILTER", HOST_FILTER_LINEAR) == HOST_FILTER_LINEAR);
    setenv("RECOMP_SCALE", "stretch", 1);
    setenv("RECOMP_FILTER", "linear", 1);
    CHECK(host_window_env_scale("RECOMP_TEST_SCALE", HOST_SCALE_ASPECT) == HOST_SCALE_STRETCH);
    CHECK(host_window_env_scale(NULL, HOST_SCALE_ASPECT) == HOST_SCALE_STRETCH);
    CHECK(host_window_env_filter("RECOMP_TEST_FILTER", HOST_FILTER_NEAREST) == HOST_FILTER_LINEAR);
    setenv("RECOMP_TEST_SCALE", "integer", 1);
    setenv("RECOMP_TEST_FILTER", "nearest", 1);
    CHECK(host_window_env_scale("RECOMP_TEST_SCALE", HOST_SCALE_ASPECT) == HOST_SCALE_INTEGER);
    CHECK(host_window_env_filter("RECOMP_TEST_FILTER", HOST_FILTER_LINEAR) == HOST_FILTER_NEAREST);
    setenv("RECOMP_TEST_SCALE", "junk", 1);   /* not a value: the shared key then */
    CHECK(host_window_env_scale("RECOMP_TEST_SCALE", HOST_SCALE_ASPECT) == HOST_SCALE_STRETCH);
    unsetenv("RECOMP_SCALE");
    unsetenv("RECOMP_FILTER");
    unsetenv("RECOMP_TEST_SCALE");
    unsetenv("RECOMP_TEST_FILTER");
    unsetenv("RECOMP_PRESENT_VSYNC");
    CHECK(host_window_env_vsync() == 1);
    setenv("RECOMP_PRESENT_VSYNC", "0", 1);
    CHECK(host_window_env_vsync() == 0);
    setenv("RECOMP_PRESENT_VSYNC", "1", 1);
    CHECK(host_window_env_vsync() == 1);
    unsetenv("RECOMP_PRESENT_VSYNC");
    unsetenv("RECOMP_RUMBLE");
    CHECK(host_pad_rumble_env() == 1);
    setenv("RECOMP_RUMBLE", "0", 1);
    CHECK(host_pad_rumble_env() == 0);
    setenv("RECOMP_RUMBLE", "1", 1);
    CHECK(host_pad_rumble_env() == 1);
    unsetenv("RECOMP_RUMBLE");
    unsetenv("RECOMP_AUDIO_BUF_SAMPLES");
    CHECK(host_audio_env_buffer_samples() == HOST_AUDIO_BUF_SAMPLES_DEFAULT);
    setenv("RECOMP_AUDIO_BUF_SAMPLES", "256", 1);
    CHECK(host_audio_env_buffer_samples() == 256);
    setenv("RECOMP_AUDIO_BUF_SAMPLES", "16", 1);
    CHECK(host_audio_env_buffer_samples() == 128);
    setenv("RECOMP_AUDIO_BUF_SAMPLES", "99999", 1);
    CHECK(host_audio_env_buffer_samples() == 4096);
    unsetenv("RECOMP_AUDIO_BUF_SAMPLES");

    /* Volume: 100 leaves the samples alone, 0 is silence, 50 is a quarter (the square of the fraction). */
    {
        int16_t in[4] = { 32767, -32768, 1000, -1000 }, out[4];
        CHECK(host_audio_volume() == 100);
        host_audio_scale(out, in, 4, 100);
        CHECK(!memcmp(out, in, sizeof in));
        host_audio_scale(out, in, 4, 0);
        CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
        host_audio_scale(out, in, 4, 50);
        CHECK(out[0] == 8191 && out[1] == -8192 && out[2] == 250 && out[3] == -250);
        host_audio_set_volume(150);
        CHECK(host_audio_volume() == 100);
        host_audio_set_volume(-5);
        CHECK(host_audio_volume() == 0);
        host_audio_set_volume(100);
    }

    /* No window: no stats, no screenshot. */
    {
        uint32_t fps = 7;
        uint64_t n = 7;
        CHECK(host_window_stats(&fps, &n) == 0 && fps == 0 && n == 0);
        CHECK(host_window_screenshot_bmp("/nonexistent/x.bmp") == -1);
    }

    /* Fast cores and frame generation link (no pacer thread started). */
    (void)recomp_host_cpus_restore;
    CHECK(!framegen_pacer_active());

    /* Pads without a window: none expected on a CI host, but the calls work. */
    if (host_pad_init()) {
        HostPadState ps;
        host_pad_update();
        unsigned mask = host_pad_mask();
        for (int p = 0; p < HOST_PAD_PORTS; p++)
            CHECK(host_pad_read(p, &ps) == !!(mask & (1u << p)));
        CHECK(host_pad_read(7, &ps) == 0);
        printf("pads: mask 0x%x\n", mask);
        host_pad_shutdown();
    } else {
        printf("pads: no SDL game controller subsystem here\n");
    }

    /* Audio out on whatever SDL_AUDIODRIVER says (the test sets dummy). */
    if (host_audio_open(48000, 2)) {
        int16_t silence[2 * 480] = { 0 };
        CHECK(host_audio_rate() > 0);
        CHECK(host_audio_channels() == 2 || host_audio_channels() == 1);
        CHECK(host_audio_queue(silence, 240));   /* 240 frames fit either channel count */
        recomp_lifecycle_pause();     /* the device pauses with the app */
        recomp_lifecycle_resume();
        host_audio_close();
        CHECK(host_audio_rate() == 0);
    } else {
        printf("audio: no device here\n");
    }
    SDL_Quit();

    printf("%s\n", s_fail ? "host_layer core: FAIL" : "host_layer core: ok");
    return s_fail;
}

static int run_window(void)
{
    enum { W = 320, H = 240 };
    static uint32_t px[W * H];
    double seconds = getenv("HOST_LAYER_SECONDS") ? atof(getenv("HOST_LAYER_SECONDS")) : 0.0;
    uint64_t start, frames = 0;
    unsigned seen_mask = 0;
    uint32_t seen_buttons = 0;
    HostWindowInfo info;

    if (host_window_headless()) {
        printf("host_layer window: headless (XBR_BUILD_ONLY or no display), skipped\n");
        return 77;
    }
    if (!host_window_open("xbr_host test", 640, 480, 1)) {
        fprintf(stderr, "host_layer window: cannot open a window with a GL context\n");
        return 1;
    }
    start = xbox_HostNowNs();
    for (;;) {
        if (!host_window_pump())
            break;
        /* A moving gradient, with a bar per pressed button. */
        uint32_t buttons = 0;
        for (int p = 0; p < HOST_PAD_PORTS; p++) {
            HostPadState ps;
            if (host_pad_read(p, &ps)) {
                seen_mask |= 1u << p;
                buttons |= ps.buttons;
            }
        }
        seen_buttons |= buttons;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint8_t r = (uint8_t)(x + frames), g = (uint8_t)(y * 255 / H), b = 0x40;
                if (y < 16 && (buttons & (1u << (x * 15 / W))))
                    r = g = b = 0xFF;
                px[y * W + x] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u;
            }
        CHECK(host_window_present_rgba(px, W, H, W * 4));
        frames++;
        if (seconds > 0.0) {
            if ((double)(xbox_HostNowNs() - start) / 1e9 >= seconds)
                break;
        } else if (frames >= 120) {
            break;
        }
    }
    host_window_info(&info);
    printf("host_layer window: %llu frames, GL%s %d.%d, %dx%d, vsync %d, pads 0x%x, buttons seen 0x%x\n",
           (unsigned long long)info.frames, info.gles ? " ES" : "", info.gl_major, info.gl_minor, info.drawable_w,
           info.drawable_h, info.vsync, seen_mask, seen_buttons);
    CHECK(info.frames == frames);
    CHECK(host_window_gl_proc("glGetString") != NULL);

    /* Stats and the screenshot of the last picture (XBA-181), from this thread as from any. */
    uint32_t fps = 0;
    uint64_t presented = 0;
    CHECK(host_window_stats(&fps, &presented) == 1);
    CHECK(presented == frames);
    printf("host_layer window: stats %u.%02u fps\n", fps / 100, fps % 100);
    {
        char bmp[512];
        const char *tmp = getenv("TMPDIR");
        snprintf(bmp, sizeof bmp, "%s/xbr_host_shot_%d.bmp", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
        host_window_set_aspect(4, 3);   /* 320x240 is 4:3 already: same size */
        CHECK(host_window_present_rgba(px, W, H, W * 4));
        CHECK(host_window_screenshot_bmp(bmp) == 0);
        FILE *f = fopen(bmp, "rb");
        unsigned char hdr[54] = { 0 };
        CHECK(f && fread(hdr, 1, sizeof hdr, f) == sizeof hdr);
        if (f)
            fclose(f);
        int32_t bw, bh;
        memcpy(&bw, hdr + 18, 4);
        memcpy(&bh, hdr + 22, 4);
        CHECK(hdr[0] == 'B' && hdr[1] == 'M' && bw == W && bh == H && hdr[28] == 24);
        unlink(bmp);
        host_window_set_aspect(0, 0);
    }
    host_window_close();
    CHECK(host_window_stats(&fps, &presented) == 0);
    CHECK(host_window_screenshot_bmp("/tmp/never.bmp") == -1);   /* the picture went with the window */
    SDL_Quit();
    return s_fail;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "core";

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(mode, "core"))
        return run_core();
    if (!strcmp(mode, "window"))
        return run_window();
    fprintf(stderr, "usage: %s core|window\n", argv[0]);
    return 2;
}
