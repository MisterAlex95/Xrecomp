/* gc_runtime.c -- see gc_runtime.h (XBA-259).
 *
 * One thread does everything, field by field: pump the window's events (quit, background, pads), read the pads into
 * the serial interface, run one VI field, present its XFB, then sleep to the next field's deadline. Pacing is a
 * clock at the field's own length (gc_field_ns: 263 / 262 NTSC lines, 313 / 312 PAL) with absolute deadlines (no
 * drift); vsync is on, so a swap may wait for the display too, and a field late by more than three periods resets the
 * clock instead of running fast to catch up. While the app is paused (background, the in-game menu:
 * recomp_lifecycle) no field runs. Fast-forward (recomp_ff_active: the in-game menu's toggle) runs `speed` fields per
 * shown frame, as many as fit in one display frame at "max".
 *
 * Sound (XBA-263): what the machine played during each field (gc_audio_pull, 48 kHz stereo) goes to host_audio;
 * the clock is nudged by the queue (above twice its 50 ms target: a field 2 % longer, under half of it: 2 % shorter)
 * so the host's audio clock and the guest's never drift apart; fast-forward averages it down (recomp_ff_audio). */
#define _GNU_SOURCE /* pread */
#include "gc_runtime.h"
#include "gc_audio.h"
#include "gc_boot.h"

#include "host_audio.h"

#include "gc_yuv_gl.h"
#include "gx/gx.h"
#include "host_pad.h"
#include "host_perf_hint.h"
#include "host_perf_line.h"
#include "host_window.h"
#include "recomp_exit_hook.h"
#include "recomp_lifecycle.h"
#include "recomp_states.h"
#include "tools/gc_png.h"

#include <SDL.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GC_DOL_MAX (16u * 1024 * 1024)
#define GC_FIELD_NS_NTSC 16666667ull
#define AUDIO_TARGET_FRAMES (GC_AUDIO_RATE / 20) /* 50 ms queued */

const char *gc_runtime_version(void)
{
    return "gc_runtime " GC_RUNTIME_VERSION;
}

void gc_runtime_opts_init(gc_runtime_opts *o)
{
    memset(o, 0, sizeof *o);
    o->tv_mode = -1;
    o->yuv_cpu = -1;
    o->scale = -1;
    o->filter = -1;
}

/* A 48 kHz 16-bit stereo WAV of what was played (tests): the header, rewritten with the sizes at the end. */
static void wav_write_header(FILE *f, uint32_t frames)
{
    uint8_t h[44] = "RIFF____WAVEfmt \x10\0\0\0\x01\0\x02\0________\x04\0\x10\0data____";
    uint32_t v[4] = {36 + frames * 4, GC_AUDIO_RATE, GC_AUDIO_RATE * 4, frames * 4};
    const int at[4] = {4, 24, 28, 40};
    for (int k = 0; k < 4; k++)
        for (int b = 0; b < 4; b++)
            h[at[k] + b] = (uint8_t)(v[k] >> (8 * b));
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

static uint32_t wav_write_frames(FILE *f, const int16_t *lr, size_t n)
{
    for (size_t i = 0; i < 2 * n; i++) {
        uint8_t le[2] = {(uint8_t)lr[i], (uint8_t)((uint16_t)lr[i] >> 8)};
        fwrite(le, 1, 2, f);
    }
    return (uint32_t)n;
}

/* A whole file, or "fd:N" read from offset 0 (the descriptor stays open and its offset is not used). */
static uint8_t *read_all(const char *spec, size_t max, size_t *size)
{
    int fd, own = 1;
    if (strncmp(spec, "fd:", 3) == 0) {
        char *end;
        long v = strtol(spec + 3, &end, 10);
        if (end == spec + 3 || *end || v < 0)
            return NULL;
        fd = (int)v;
        own = 0;
    } else {
        fd = open(spec, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0)
        return NULL;
    size_t cap = 1 << 20, n = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        if (n == cap) {
            if (cap >= max + 1)
                break;
            uint8_t *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2;
        }
        ssize_t r = pread(fd, buf + n, cap - n, (off_t)n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            free(buf);
            buf = NULL;
        }
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    if (own)
        close(fd);
    if (buf && (n == 0 || n > max)) {
        free(buf);
        buf = NULL;
    }
    *size = n;
    return buf;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* ---- The discovered addresses (the next Prepare's discovery loop) ------------------------------------------------ */

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static gc_machine *s_m;           /* the run's machine, for the exit hook (the in-game menu's Quit) */
static char s_discovered[1200];

static void mkdirs_for(const char *path)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
}

/* Under s_mu: one hex address per line (gc_run --missed's format). */
static void save_missed_locked(void)
{
    if (!s_m || !s_discovered[0])
        return;
    size_t n = gc_recomp_missed(s_m, NULL, 0);
    if (!n)
        return;
    uint32_t *list = malloc(n * sizeof *list);
    if (!list)
        return;
    gc_recomp_missed(s_m, list, n);
    mkdirs_for(s_discovered);
    FILE *f = fopen(s_discovered, "w");
    if (f) {
        fprintf(f, "# gc_runtime: text addresses the interpreter started at after a jump\n");
        for (size_t i = 0; i < n; i++)
            fprintf(f, "%08X\n", list[i]);
        fclose(f);
        printf("gc: %zu address(es) the interpreter started at written to %s\n", n, s_discovered);
    }
    free(list);
}

static void on_exit_hook(const char *why)
{
    (void)why;
    pthread_mutex_lock(&s_mu);
    save_missed_locked();
    if (s_m)
        gx_save_shader_list(gc_gx(s_m)); /* the GX states met (XBA-262) */
    pthread_mutex_unlock(&s_mu);
}

/* ---- Pads ------------------------------------------------------------------------------------------------------- */

static ConsolePadMap s_pad_map;

/* Desktop keys for port 1: arrows, X / Z / C / V = A / B / X / Y, A / S = L / R, D = Z, Enter = Start. */
static uint16_t keyboard_buttons(void)
{
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint16_t b = 0;
    if (!ks)
        return 0;
    if (ks[SDL_SCANCODE_X]) b |= GC_PAD_A;
    if (ks[SDL_SCANCODE_Z]) b |= GC_PAD_B;
    if (ks[SDL_SCANCODE_C]) b |= GC_PAD_X;
    if (ks[SDL_SCANCODE_V]) b |= GC_PAD_Y;
    if (ks[SDL_SCANCODE_A]) b |= GC_PAD_L;
    if (ks[SDL_SCANCODE_S]) b |= GC_PAD_R;
    if (ks[SDL_SCANCODE_D]) b |= GC_PAD_Z;
    if (ks[SDL_SCANCODE_RETURN]) b |= GC_PAD_START;
    if (ks[SDL_SCANCODE_UP]) b |= GC_PAD_UP;
    if (ks[SDL_SCANCODE_DOWN]) b |= GC_PAD_DOWN;
    if (ks[SDL_SCANCODE_LEFT]) b |= GC_PAD_LEFT;
    if (ks[SDL_SCANCODE_RIGHT]) b |= GC_PAD_RIGHT;
    return b;
}

/* Host pad n (and the on-screen pad on port 1) is the controller in port n; port 1 is always plugged in, so a game
 * that waits for a controller starts with the device's own controls or the keyboard. */
static void pads_to_si(gc_machine *m, const gc_runtime_opts *o, int window)
{
    for (int p = 0; p < 4 && p < HOST_PAD_PORTS; p++) {
        ConsolePadOut out;
        gc_pad_state st;
        int have = console_pad_read(&s_pad_map, p, &out);
        if (have) {
            HostPadState raw;
            int lt = 0, rt = 0;
            if (host_pad_read(p, &raw)) {
                lt = raw.axis[HOST_PAD_AXIS_LT];
                rt = raw.axis[HOST_PAD_AXIS_RT];
            }
            gc_runtime_pad_of(&s_pad_map, &out, lt, rt, &st);
        } else {
            memset(&st, 0, sizeof st);
            st.stick_x = st.stick_y = st.cstick_x = st.cstick_y = 128;
            st.connected = p == 0;
        }
        if (p == 0) {
            if (window)
                st.buttons |= keyboard_buttons();
            if (o->pad_force_on) {
                st.buttons |= o->pad_force.buttons;
                st.stick_x = o->pad_force.stick_x;
                st.stick_y = o->pad_force.stick_y;
                st.cstick_x = o->pad_force.cstick_x;
                st.cstick_y = o->pad_force.cstick_y;
                st.trigger_l = o->pad_force.trigger_l;
                st.trigger_r = o->pad_force.trigger_r;
            }
        }
        gc_set_pad(m, p, &st);
    }
}

/* The game's motor commands to the host pads' rumble (when RECOMP_RUMBLE allows it): a short pulse renewed each
 * field while the motor runs, stopped when it stops. */
static int s_motor[4];
static void rumble(gc_machine *m)
{
    if (!host_pad_rumble_env())
        return;
    for (int p = 0; p < 4 && p < HOST_PAD_PORTS; p++) {
        int on = gc_pad_motor(m, p) == 1;
        if (on)
            host_pad_rumble(p, 0xC000, 0xC000, 100);
        else if (s_motor[p])
            host_pad_rumble(p, 0, 0, 0);
        s_motor[p] = on;
    }
}

/* ---- The run ---------------------------------------------------------------------------------------------------- */

static void display(const gc_runtime_opts *o, int *scale, int *filter)
{
    /* RECOMP_GC_SCALE / _FILTER, else the shared RECOMP_SCALE / RECOMP_FILTER (XBA-181): a 640-pixel picture on a
     * phone's screen looks best smoothed. */
    *scale = o->scale >= 0 ? o->scale : host_window_env_scale("RECOMP_GC_SCALE", HOST_SCALE_ASPECT);
    *filter = o->filter >= 0 ? o->filter : host_window_env_filter("RECOMP_GC_FILTER", HOST_FILTER_LINEAR);
}

int gc_runtime_run(const gc_runtime_opts *o)
{
    size_t dol_n = 0;
    char key[48], why[160];
    /* A disc image (XBA-261: ISO / GCM, CISO, GCZ, RVZ, WIA, by content) boots through its apploader, no IPL. */
    gc_disc *disc = o && o->dol ? gc_disc_open(o->dol, why, sizeof why) : NULL;
    uint8_t *dol = NULL;
    gc_machine *m = NULL;
    bool disc_pal = false;
    if (disc) {
        uint8_t hdr[0x20];
        if (gc_disc_read(disc, 0, hdr, sizeof hdr) == 0)
            disc_pal = hdr[3] == 'P';
        if (o->title_key && *o->title_key)
            snprintf(key, sizeof key, "%s", o->title_key);
        else if (gc_disc_title_key(disc, key, sizeof key))
            snprintf(key, sizeof key, "?");
        printf("gc: disc %s (%s image), key %s\n", base_name(o->dol), gc_disc_format(disc), key);
        m = gc_create_disc(disc, why, sizeof why);
        if (!m) {
            gc_disc_close(disc);
            fprintf(stderr, "gc: %s does not boot: %s\n", base_name(o->dol), why);
            return -1;
        }
        goto started;
    }
    dol = o && o->dol ? read_all(o->dol, GC_DOL_MAX, &dol_n) : NULL;
    if (!dol) {
        fprintf(stderr, "gc: cannot read the program %s (missing, empty or over 16 MB)\n",
                o && o->dol ? o->dol : "(none)");
        return -1;
    }
    if (o->title_key && *o->title_key)
        snprintf(key, sizeof key, "%s", o->title_key);
    else
        gc_runtime_key(dol, dol_n, key, sizeof key);
    m = gc_create(dol, dol_n, why, sizeof why);
    free(dol);
    if (!m) {
        fprintf(stderr, "gc: %s is no DOL the machine can start: %s\n", base_name(o->dol), why);
        return -1;
    }
started:;
    /* TV: the option, RECOMP_GC_TV, else a European disc's PAL (gc_create_disc set it), else NTSC */
    const char *tv_env = getenv("RECOMP_GC_TV");
    int tv = o->tv_mode >= 0 ? o->tv_mode
             : tv_env && *tv_env ? (!strcasecmp(tv_env, "pal") ? GC_TV_PAL : GC_TV_NTSC)
             : disc_pal ? GC_TV_PAL : GC_TV_NTSC;
    if (tv != (disc_pal ? GC_TV_PAL : GC_TV_NTSC))
        gc_set_tv_mode(m, tv);
    if (disc)
        printf("gc: %s, disc %s, key %s, TV %s\n", gc_runtime_version(), base_name(o->dol), key,
               tv == GC_TV_PAL ? "PAL (50 Hz)" : "NTSC (60 Hz)");
    else
        printf("gc: %s, program %s (%zu KB), key %s, TV %s\n", gc_runtime_version(), base_name(o->dol), dol_n / 1024,
               key, tv == GC_TV_PAL ? "PAL (50 Hz)" : "NTSC (60 Hz)");

    /* A pack: lifted code where memory still holds what it was lifted from, the interpreter elsewhere. */
    void *lib = NULL;
    if (o->pack_lib && *o->pack_lib) {
        lib = dlopen(o->pack_lib, RTLD_NOW | RTLD_LOCAL);
        gc_recomp_title_table_fn get = lib ? (gc_recomp_title_table_fn)dlsym(lib, GC_RECOMP_ENTRY_SYMBOL) : NULL;
        const gc_recomp_table *t = get ? get() : NULL;
        if (t && gc_attach_recomp(m, t) == 0) {
            printf("PACK_LOADED gc %s (%u resume points, %u spans)\n", key, t->count, t->nspans);
        } else {
            printf("PACK_STALE gc %s (%s), running interpreted\n", key,
                   !lib ? dlerror() : !t ? "no gc_recomp_title_table" : "recompiler ABI differs");
            if (lib)
                dlclose(lib);
            lib = NULL;
        }
    }
    pthread_mutex_lock(&s_mu);
    s_m = m;
    snprintf(s_discovered, sizeof s_discovered, "%s", o->discovered_path ? o->discovered_path : "");
    pthread_mutex_unlock(&s_mu);
    recomp_exit_hook_set(on_exit_hook);

    /* The window, or none (headless checks: SDL dummy drivers). */
    int window = !o->headless && !host_window_headless();
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0)
        SDL_InitSubSystem(SDL_INIT_EVENTS);
    int scale, filter;
    display(o, &scale, &filter);
    host_window_set_scaling(scale, filter);
    host_window_set_aspect(4, 3); /* NTSC 640 x 480 and PAL 640 x 576 alike: a 4:3 TV */
    host_window_shader_env("RECOMP_GC_SHADER");
    if (window && !host_window_open("XRecomp GameCube", 640 * 2, 480 * 2, host_window_env_vsync())) {
        fprintf(stderr, "gc: no window, running headless\n");
        window = 0;
    }
    if (!window)
        host_pad_init();
    console_pad_load(&s_pad_map, CONSOLE_PAD_GC, getenv(console_pad_env(CONSOLE_PAD_GC)));
    const char *yuv_env = getenv("RECOMP_GC_YUV");
    int yuv_cpu = o->yuv_cpu >= 0 ? o->yuv_cpu : yuv_env && !strcasecmp(yuv_env, "cpu");
    gc_yuv_gl *gpu = window && !yuv_cpu ? gc_yuv_gl_create(filter) : NULL;
    /* GX (XBA-262): the GLES renderer draws on the window's context (this thread runs the fields); its copies to the
     * XFB also land in RAM, and the window shows GX's own picture of the XFB the VI scans (at the internal
     * resolution, RECOMP_GC_UPSCALE). The TEV / vertex states met go to gx_shaders.txt next to the discovered
     * addresses, read back and compiled ahead at the next start. RECOMP_GC_GX=0: GX parses but does not draw. */
    int gx_on = 0;
    const char *gx_env = getenv("RECOMP_GC_GX");
    if (window && !(gx_env && !strcmp(gx_env, "0"))) {
        static char gx_list[1200];
        gx_list[0] = 0;
        if (o->discovered_path && *o->discovered_path) {
            const char *slash = strrchr(o->discovered_path, '/');
            int dir = slash ? (int)(slash - o->discovered_path) : 0;
            snprintf(gx_list, sizeof gx_list, "%.*s%sgx_shaders.txt", dir, o->discovered_path, slash ? "/" : "");
        }
        gx_gl_options go = {0, 1, 0, gx_list[0] ? gx_list : NULL};
        gx_on = gx_gl_attach(gc_gx(m), &go) == 0;
    }
    int audio = host_audio_open(GC_AUDIO_RATE, 2);
    FILE *wav = NULL;
    uint32_t wav_frames = 0;
    if (o->wav_path && *o->wav_path) {
        wav = fopen(o->wav_path, "wb");
        if (wav)
            wav_write_header(wav, 0);
        else
            fprintf(stderr, "gc: cannot write %s\n", o->wav_path);
    }
    printf("gc: window %s, XFB to RGB on the %s, audio %s (48 kHz), pads %u\n", window ? "on" : "off",
           gpu ? "GPU" : "CPU", audio ? "on" : "off (no device)", host_pad_mask());
    fflush(stdout);

    static uint8_t yuv[GC_XFB_MAX_W * GC_XFB_MAX_H * 2];
    static uint8_t rgb[GC_XFB_MAX_W * GC_XFB_MAX_H * 3];
    static uint8_t rgba[GC_XFB_MAX_W * GC_XFB_MAX_H * 4];
    uint64_t frames = 0, t0 = now_ns(), next = t0, late_resets = 0;
    int was_paused = 0, rc = 0, was_ff = 0, stopped = 0, heard = 0;
    static int16_t pcm[GC_AUDIO_RATE / 10 * 2];
    recomp_ff_carry ff_carry;
    memset(&ff_carry, 0, sizeof ff_carry);
    uint64_t field_ns = gc_field_ns(m);
    recomp_perf_hint_begin((int64_t)field_ns); /* the frame loop, one session (no worker threads) */
    /* The performance line every console logs every 10 s and at the end (host_perf_line.h, XBA-243). */
    enum { ST_EMU, ST_AUDIO, ST_PRESENT, ST_N };
    host_perf_config hp_cfg = {
        .console = "gc", .unit = "VI/s", .nstages = ST_N,
        .stage = {"emu", "audio", "present"}, .kind = {HOST_PERF_EMU, HOST_PERF_OTHER, HOST_PERF_RENDER},
        .rest = "other", .rest_kind = HOST_PERF_OTHER,
    };
    hp_cfg.frame_ns = tv == GC_TV_PAL ? 20000000ull : GC_FIELD_NS_NTSC;
    host_perf hp;
    host_perf_init(&hp, &hp_cfg, t0);

    for (;;) {
        int running = 1;
        if (window) {
            running = host_window_pump();
        } else {
            SDL_Event ev;
            host_pad_update();
            running = SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) <= 0; /* SIGINT, a test */
            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        }
        if (!running) {
            printf("gc: quit asked after %llu fields\n", (unsigned long long)frames);
            break;
        }
        if (recomp_lifecycle_paused()) {
            if (!was_paused) {
                printf("gc: paused at field %llu\n", (unsigned long long)frames);
                fflush(stdout);
                was_paused = 1;
                host_perf_pause(&hp, now_ns());
            }
            SDL_Delay(16);
            continue;
        }
        if (was_paused) {
            printf("gc: resumed\n");
            fflush(stdout);
            was_paused = 0;
            next = now_ns(); /* the time away is not caught up */
            host_perf_resume(&hp, next);
        }
        uint64_t work_t0 = now_ns(); /* the frame's work, for the performance hint (the sleep after it is not) */
        host_perf_frame(&hp, work_t0, NULL);

        int ff = recomp_ff_active(), speed = recomp_ff_speed();
        if (ff != was_ff) {
            printf("gc: fast-forward %s at field %llu\n", ff ? (speed ? "on" : "on (max)") : "off",
                   (unsigned long long)frames);
            fflush(stdout);
            was_ff = ff;
            next = now_ns();
        }
        int steps = ff ? (speed ? speed : 64) : 1;
        uint64_t burst_end = now_ns() + field_ns * 9 / 10;
        pads_to_si(m, o, window);
        for (int step = 0; step < steps; step++) {
            uint64_t te = now_ns();
            pthread_mutex_lock(&s_mu);
            field_ns = gc_field_ns(m);
            int r = gc_run_frame(m);
            pthread_mutex_unlock(&s_mu);
            host_perf_stage(&hp, ST_EMU, now_ns() - te);
            if (r) {
                fprintf(stderr, "gc: the machine stopped at field %llu: %s\n", (unsigned long long)frames,
                        gc_error(m));
                gc_dump_state(m, stderr);
                stopped = 1;
                break;
            }
            frames++;
            host_perf_guest(&hp, 1);

            /* Sound: what the field played (800 frames at 60 Hz); averaged down while fast-forwarding. */
            uint64_t ta = now_ns();
            size_t got;
            while ((got = gc_audio_pull(m, pcm, sizeof pcm / sizeof pcm[0] / 2)) > 0) {
                if (wav)
                    wav_frames += wav_write_frames(wav, pcm, got);
                for (size_t i = 0; i < 2 * got && !heard; i++)
                    heard = pcm[i] != 0;
                if (ff)
                    got = speed ? recomp_ff_audio(pcm, got, speed, &ff_carry) : 0;
                if (audio && got)
                    host_audio_queue(pcm, (uint32_t)got);
            }
            host_perf_stage(&hp, ST_AUDIO, now_ns() - ta);
            if (o->frames && frames >= o->frames)
                break;
            if (ff && !speed && now_ns() >= burst_end)
                break;
        }
        rumble(m);

        if (window && !stopped) {
            uint64_t tp = now_ns();
            int w, h;
            if (o->capture_path && o->capture_frame == frames)
                host_window_capture(o->capture_path);
            uint32_t xfb_at = gx_on ? gc_xfb_addr(m) : 0;
            unsigned gx_tex = xfb_at ? gx_xfb_texture(gc_gx(m), xfb_at, &w, &h) : 0;
            if (gx_tex && host_window_present_texture(gx_tex, w, h, 1)) {
                /* GX's picture of the XFB */
            } else if (gpu && gc_xfb_yuv(m, yuv, &w, &h) == 0) {
                if (!gc_yuv_gl_present(gpu, yuv, w, h)) {
                    fprintf(stderr, "gc: GPU YUV present failed, converting on the CPU from now on\n");
                    gc_yuv_gl_destroy(gpu);
                    gpu = NULL;
                }
            } else if (!gpu && gc_xfb_rgb(m, rgb, &w, &h) == 0) {
                for (int i = 0; i < w * h; i++) {
                    rgba[4 * i] = rgb[3 * i];
                    rgba[4 * i + 1] = rgb[3 * i + 1];
                    rgba[4 * i + 2] = rgb[3 * i + 2];
                    rgba[4 * i + 3] = 255;
                }
                host_window_present_rgba(rgba, w, h, w * 4);
            } else {
                static const uint8_t black[4 * 4 * 4] = {0};
                host_window_present_rgba(black, 4, 3, 16); /* the VI shows nothing yet */
            }
            host_perf_stage(&hp, ST_PRESENT, now_ns() - tp);
            host_perf_shown(&hp, 1);
        }
        host_perf_log_due(&hp, now_ns());
        if (stopped) {
            rc = -1;
            break;
        }
        if (o->frames && frames >= o->frames)
            break;
        recomp_perf_hint_frame((int64_t)(now_ns() - work_t0), (int64_t)field_ns);

        if (!o->unpaced && !(ff && !speed)) {
            next += field_ns;
            if (audio && heard) { /* audio-driven: the queue keeps the clock honest (±2 %) */
                uint32_t q = host_audio_queued();
                if (q > AUDIO_TARGET_FRAMES * 2)
                    next += field_ns / 50;
                else if (q < AUDIO_TARGET_FRAMES / 2)
                    next -= field_ns / 50;
            }
            uint64_t t = now_ns();
            if (t > next + 3 * field_ns) {
                next = t; /* a stall (a slow host, the debugger): no burst to catch up */
                late_resets++;
            } else if (next > t) {
                host_window_sleep_until(next);
                host_perf_idle(&hp, now_ns() - t);
            }
        }
    }

    recomp_perf_hint_end();
    host_perf_log_run(&hp, now_ns(), NULL);
    {
        gc_audio_info ai;
        gc_audio_get_info(m, &ai);
        printf("gc: audio: ucode %s (%08X), %u started (%u unknown), %llu command lists, %llu DMA blocks, "
               "%llu stream frames, %llu frames out, %llu dropped\n",
               ai.ucode, ai.ucode_crc, ai.ucodes_booted, ai.unknown_ucodes, (unsigned long long)ai.cmd_lists,
               (unsigned long long)ai.dma_blocks, (unsigned long long)ai.stream_frames,
               (unsigned long long)ai.frames_out, (unsigned long long)ai.overruns);
    }
    if (wav) {
        wav_write_header(wav, wav_frames);
        fclose(wav);
        printf("gc: %u audio frames written to %s\n", wav_frames, o->wav_path);
    }
    if (audio)
        host_audio_close();
    host_perf_live_clear();
    double secs = (double)(now_ns() - t0) / 1e9;
    gc_recomp_stats rs;
    gc_recomp_get_stats(m, &rs);
    printf("gc: %llu fields in %.1f s (%.2f fields/s), %llu late resets; %llu instructions interpreted, %llu lifted "
           "calls\n",
           (unsigned long long)frames, secs, secs > 0 ? (double)frames / secs : 0.0, (unsigned long long)late_resets,
           (unsigned long long)rs.interp, (unsigned long long)rs.calls);
    if (o->screenshot) {
        int w, h;
        if (gc_xfb_rgb(m, rgb, &w, &h) == 0 && write_png(o->screenshot, rgb, w, h) == 0)
            printf("gc: last field's XFB (%dx%d) written to %s\n", w, h, o->screenshot);
        else {
            fprintf(stderr, "gc: no XFB to write to %s\n", o->screenshot);
            rc = -1;
        }
    }
    fflush(stdout);
    recomp_exit_hook_set(NULL);
    for (int p = 0; p < 4; p++)
        if (s_motor[p]) {
            host_pad_rumble(p, 0, 0, 0);
            s_motor[p] = 0;
        }
    gc_yuv_gl_destroy(gpu);
    if (gx_on)
        gx_gl_detach(gc_gx(m)); /* while the window's context is current; writes the shader list */
    if (window)
        host_window_close();
    pthread_mutex_lock(&s_mu);
    save_missed_locked();
    s_m = NULL;
    s_discovered[0] = 0;
    pthread_mutex_unlock(&s_mu);
    gc_destroy(m);
    if (lib)
        dlclose(lib);
    return rc;
}

/* ---- The app shell's entry -------------------------------------------------------------------------------------- */

static char s_start_work_dir[1024];

void gc_runtime_set_work_dir(const char *dir)
{
    snprintf(s_start_work_dir, sizeof s_start_work_dir, "%s", dir ? dir : "");
}

int gc_runtime_start(const char *dol, const char *pack_lib, const char *title_key)
{
    gc_runtime_opts o;
    gc_runtime_opts_init(&o);
    o.dol = dol;
    o.title_key = title_key;
    o.pack_lib = pack_lib;
    char discovered[1200];
    if (s_start_work_dir[0] && title_key && *title_key) {
        snprintf(discovered, sizeof discovered, "%s/gc-%s/discovered.txt", s_start_work_dir, title_key);
        o.discovered_path = discovered;
    }
    const char *frames = getenv("RECOMP_GC_FRAMES"); /* a bounded run (checks) */
    if (frames && *frames)
        o.frames = strtoull(frames, NULL, 10);
    printf("gc runtime ready (%s; title %s, pack %s, program %s)\n", gc_runtime_version(), title_key ? title_key : "?",
           pack_lib && *pack_lib ? pack_lib : "none (interpreter)", dol ? dol : "?");
    fflush(stdout);
    return gc_runtime_run(&o);
}
