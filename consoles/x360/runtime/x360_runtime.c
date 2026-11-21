/* x360_runtime.c -- see x360_runtime.h (XBA-280). The frame loop of gc_runtime.c, for x360_core. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "x360_runtime.h"

#include "host_audio.h"
#include "host_pad.h"
#include "host_perf_hint.h"
#include "host_perf_line.h"
#include "host_shader_progress.h"
#include "host_window.h"
#include "recomp_cheats.h"
#include "recomp_exit_hook.h"
#include "recomp_hotkeys.h"
#include "recomp_lifecycle.h"
#include "recomp_play.h"
#include "recomp_rewind.h"
#include "recomp_states.h"

#include <SDL.h>

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FRAME_100NS 166667ull           /* one 60 Hz frame of the title's virtual clock */
#define FRAME_NS 16666667ull
#define AUDIO_RATE 48000
#define AUDIO_TARGET_FRAMES (AUDIO_RATE / 20) /* 50 ms queued */
#define RENDER_W 1280
#define RENDER_H 720

const char *x360_runtime_version(void)
{
    return "x360_runtime " X360_RUNTIME_VERSION;
}

void x360_runtime_opts_init(x360_runtime_opts *o)
{
    memset(o, 0, sizeof *o);
    o->scale = o->filter = -1;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *base_name(const char *p)
{
    const char *s = p ? strrchr(p, '/') : NULL;
    return s ? s + 1 : p ? p : "?";
}

/* a path or "fd:N" (read from offset 0, left open), at most max bytes */
static uint8_t *read_all(const char *spec, size_t max, size_t *size)
{
    FILE *f = NULL;
    if (!strncmp(spec, "fd:", 3)) {
        int fd = dup(atoi(spec + 3));
        if (fd >= 0) {
            lseek(fd, 0, SEEK_SET);
            f = fdopen(fd, "rb");
        }
    } else {
        f = fopen(spec, "rb");
    }
    if (!f)
        return NULL;
    size_t cap = 1 << 20, n = 0;
    uint8_t *b = malloc(cap);
    while (b) {
        if (n == cap) {
            if (cap >= max) {
                free(b);
                b = NULL;
                break;
            }
            uint8_t *nb = realloc(b, cap * 2);
            if (!nb) {
                free(b);
                b = NULL;
                break;
            }
            b = nb, cap *= 2;
        }
        size_t got = fread(b + n, 1, cap - n, f);
        if (!got)
            break;
        n += got;
    }
    fclose(f);
    if (b && !n) {
        free(b);
        b = NULL;
    }
    *size = n;
    return b;
}

static void mkdirs(const char *dir)
{
    char p[1200];
    snprintf(p, sizeof p, "%s", dir);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

static void wav_header(FILE *f, uint32_t frames)
{
    uint8_t h[44] = "RIFF....WAVEfmt ";
    uint32_t v[] = {36 + frames * 4, 16, 1 | 2u << 16, AUDIO_RATE, AUDIO_RATE * 4, 4 | 16u << 16, 0, frames * 4};
    memcpy(h + 4, &v[0], 4);
    memcpy(h + 16, &v[1], 4);
    memcpy(h + 20, &v[2], 4);
    memcpy(h + 24, &v[3], 4);
    memcpy(h + 28, &v[4], 4);
    memcpy(h + 32, &v[5], 4);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &v[7], 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

/* ---- the run's machine, for the state / exit hooks (other threads) --------------------------------------------- */

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static x360_machine *s_x;
static char s_work[1100], s_key[48], s_state_dir[1200];
static int s_state_loaded;
static unsigned s_saved_missed;
static pthread_t s_machine_thread; /* runs the machine and owns the renderer's GL context */
static int s_machine_thread_set;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void save_missed_locked(void)
{
    if (!s_x || !s_work[0])
        return;
    const uint32_t *m;
    unsigned n = x360_missed(s_x, &m);
    if (!n)
        return;
    char path[1300], tmp[1320];
    snprintf(path, sizeof path, "%s/discovered.txt", s_work);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    /* XBA-282: the file only grows (what earlier runs found stays: the launcher counts it against pack.json's
     * "discovered", and the next Prepare seeds the lifter with all of it) */
    uint32_t *all = malloc(sizeof *all * (n + 65536));
    unsigned na = 0;
    if (!all)
        return;
    FILE *old = fopen(path, "r");
    if (old) {
        char line[64];
        while (na < 65536 && fgets(line, sizeof line, old)) {
            char *end;
            unsigned long v = strtoul(line, &end, 16);
            if (end != line && line[0] != '#')
                all[na++] = (uint32_t)v;
        }
        fclose(old);
    }
    for (unsigned k = 0; k < n; k++)
        all[na++] = m[k];
    qsort(all, na, sizeof *all, cmp_u32);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        free(all);
        return;
    }
    fprintf(f, "# x360_runtime: addresses reached without lifted code (x360_tools.recomp --discovered)\n");
    for (unsigned k = 0; k < na; k++)
        if (!k || all[k] != all[k - 1])
            fprintf(f, "%08X\n", all[k]);
    free(all);
    if (fclose(f) == 0)
        rename(tmp, path);
}

static void on_exit_hook(const char *why)
{
    (void)why;
    pthread_mutex_lock(&s_mu);
    save_missed_locked();
    pthread_mutex_unlock(&s_mu);
}

/* ---- pads ------------------------------------------------------------------------------------------------------ */

static ConsolePadMap s_pad_map;
static recomp_hotkeys_state s_hk_pad[HOST_PAD_PORTS];

/* desktop keys for user 0: arrows, X / Z / C / V = A / B / X / Y, A / S = LB / RB, Q / W = LT / RT, Enter = Start,
 * Backspace = Back */
static void keyboard(x360_pad *p)
{
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    if (!ks)
        return;
    static const struct {
        int sc;
        uint16_t bit;
    } k[] = {{SDL_SCANCODE_X, 0x1000},     {SDL_SCANCODE_Z, 0x2000},    {SDL_SCANCODE_C, 0x4000},
             {SDL_SCANCODE_V, 0x8000},     {SDL_SCANCODE_A, 0x0100},    {SDL_SCANCODE_S, 0x0200},
             {SDL_SCANCODE_RETURN, 0x0010}, {SDL_SCANCODE_BACKSPACE, 0x0020}, {SDL_SCANCODE_UP, 0x0001},
             {SDL_SCANCODE_DOWN, 0x0002},  {SDL_SCANCODE_LEFT, 0x0004}, {SDL_SCANCODE_RIGHT, 0x0008}};
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (ks[k[i].sc])
            p->buttons |= k[i].bit;
    if (ks[SDL_SCANCODE_Q])
        p->left_trigger = 255;
    if (ks[SDL_SCANCODE_W])
        p->right_trigger = 255;
}

static void pads_in(x360_machine *x, const x360_runtime_opts *o, int window, uint32_t *hk_held, uint32_t *hk_pressed)
{
    for (int p = 0; p < 4 && p < HOST_PAD_PORTS; p++) {
        ConsolePadOut out;
        x360_pad st;
        int have = console_pad_read(&s_pad_map, p, &out);
        if (have) {
            HostPadState raw;
            int lt = 0, rt = 0;
            if (host_pad_read(p, &raw)) {
                lt = raw.axis[HOST_PAD_AXIS_LT];
                rt = raw.axis[HOST_PAD_AXIS_RT];
                uint32_t pressed = 0, swallow = 0;
                uint32_t src = console_pad_sources(&raw, s_pad_map.stick_threshold, RECOMP_HK_TRIGGER);
                *hk_held |= recomp_hotkeys_pad_update(recomp_hotkeys_get(), &s_hk_pad[p], src, &pressed, &swallow);
                *hk_pressed |= pressed;
                console_pad_swallow(&s_pad_map, &out, swallow);
            }
            x360_runtime_pad_of(&s_pad_map, &out, lt, rt, &st);
        } else {
            memset(&st, 0, sizeof st);
        }
        if (p == 0) {
            if (window)
                keyboard(&st);
            if (o->pad_force_on) {
                st.buttons |= o->pad_force.buttons;
                if (o->pad_force.left_trigger)
                    st.left_trigger = o->pad_force.left_trigger;
                if (o->pad_force.right_trigger)
                    st.right_trigger = o->pad_force.right_trigger;
                if (o->pad_force.thumb_lx || o->pad_force.thumb_ly)
                    st.thumb_lx = o->pad_force.thumb_lx, st.thumb_ly = o->pad_force.thumb_ly;
            }
            x360_set_pad(x, 0, &st); /* user 0 always connected: the keyboard, the device's own controls */
        } else {
            x360_set_pad(x, p, have ? &st : NULL);
        }
    }
}

/* the title's motor speeds (XamInputSetState) to the host pads, when RECOMP_RUMBLE allows it */
static unsigned s_motor[4][2];
static void rumble(x360_machine *x)
{
    if (!host_pad_rumble_env())
        return;
    for (int p = 0; p < 4 && p < HOST_PAD_PORTS; p++) {
        unsigned l, r;
        x360_pad_motors(x, p, &l, &r);
        if (l || r)
            host_pad_rumble(p, (uint16_t)(l * 257), (uint16_t)(r * 257), 100); /* renewed each frame */
        else if (s_motor[p][0] || s_motor[p][1])
            host_pad_rumble(p, 0, 0, 0);
        s_motor[p][0] = l, s_motor[p][1] = r;
    }
}

/* ---- states, rewind, cheats ------------------------------------------------------------------------------------ */

static int state_save_op(int slot, void *user)
{
    (void)user;
    char path[1300];
    if (recomp_states_slot_path(s_state_dir, slot, path, sizeof path) != 0)
        return RECOMP_STATE_ESLOT;
    static uint16_t thumb[RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
    recomp_state_meta meta;
    memset(&meta, 0, sizeof meta);
    snprintf(meta.console, sizeof meta.console, "x360");
    snprintf(meta.key, sizeof meta.key, "%.47s", s_key);
    pthread_mutex_lock(&s_mu);
    if (!s_x) {
        pthread_mutex_unlock(&s_mu);
        return RECOMP_STATE_ENOGAME;
    }
    /* Lazy resolves (RECOMP_X360_RESOLVE=lazy) into memory first, where the GL context is: on the machine's thread
     * (a hotkey, the automatic slot at quit). Elsewhere (the menu, the automatic save thread) the state goes without
     * the resolves of the last 2 frames: after a load the renderer starts over and the title resolves them again. */
    if (s_machine_thread_set && pthread_equal(pthread_self(), s_machine_thread))
        x360_render_flush(s_x);
    size_t cap = x360_state_size(s_x), n = 0;
    uint8_t *blob = malloc(cap);
    if (blob)
        n = x360_state_save(s_x, s_key, blob, cap);
    x360_stats xs;
    x360_get_stats(s_x, &xs);
    meta.frames = xs.vblanks;
    int w = 0, h = 0;
    const uint8_t *frame = x360_render_frame(s_x, &w, &h); /* read back only off-screen; else a grey thumbnail */
    uint8_t *rgba = NULL;
    if (frame && w > 0 && h > 0 && (rgba = malloc((size_t)w * h * 4)))
        memcpy(rgba, frame, (size_t)w * h * 4);
    pthread_mutex_unlock(&s_mu);
    if (!rgba) {
        w = 16, h = 9;
        rgba = malloc((size_t)w * h * 4);
        if (rgba)
            memset(rgba, 64, (size_t)w * h * 4);
    }
    if (rgba)
        recomp_states_thumb(rgba, w, h, w * 4, 16, 9, thumb, &meta.thumb_w, &meta.thumb_h);
    free(rgba);
    int rc = n ? recomp_states_write(path, &meta, thumb, blob, n) : RECOMP_STATE_EIO;
    free(blob);
    printf("x360: state slot %d %s (%zu bytes) %s\n", slot, rc == 0 ? "saved" : "not saved", n,
           rc == 0 ? path : recomp_states_error(rc));
    fflush(stdout);
    return rc;
}

static int state_load_op(int slot, void *user)
{
    (void)user;
    char path[1300], why[200] = "";
    if (recomp_states_slot_path(s_state_dir, slot, path, sizeof path) != 0)
        return RECOMP_STATE_ESLOT;
    recomp_state_meta meta;
    uint8_t *blob = NULL;
    size_t n = 0;
    int rc = recomp_states_read(path, &meta, NULL, &blob, &n);
    if (rc == RECOMP_STATE_OK && strcmp(meta.console, "x360") != 0)
        rc = RECOMP_STATE_EGAME;
    if (rc == RECOMP_STATE_OK) {
        pthread_mutex_lock(&s_mu);
        if (!s_x) {
            rc = RECOMP_STATE_ENOGAME;
        } else {
            rc = x360_state_load_undo(s_x, s_key, blob, n, why, sizeof why); /* X360_STATE_* = RECOMP_STATE_* */
            if (rc == X360_STATE_OK)
                s_state_loaded = 1;
        }
        pthread_mutex_unlock(&s_mu);
    }
    free(blob);
    if (rc == 0)
        printf("x360: state slot %d loaded (%s)\n", slot, path);
    else
        printf("x360: state slot %d not loaded: %s%s%s\n", slot, recomp_states_error(rc), why[0] ? ": " : "", why);
    fflush(stdout);
    return rc;
}

static size_t rewind_size(void *user)
{
    return x360_state_size((x360_machine *)user);
}

/* Rewind states are taken every frame: no flush of lazy resolves (that would be the read-back they avoid); a state goes
 * without the last 2 frames' resolves, which the title resolves again after the step back (the renderer starts over). */
static size_t rewind_save(uint8_t *out, size_t cap, void *user)
{
    return x360_state_save((x360_machine *)user, s_key, out, cap);
}

static int rewind_load(const uint8_t *in, size_t n, void *user)
{
    return x360_state_load((x360_machine *)user, s_key, in, n, NULL, 0) == X360_STATE_OK ? 0 : -1;
}

static uint32_t cheat_read32(void *x, uint32_t addr)
{
    return x360_cheat_read32(x, addr);
}

static void cheat_write32(void *x, uint32_t addr, uint32_t v)
{
    x360_cheat_write32(x, addr, v);
}

static int env_int(const char *name, int dflt)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : dflt;
}

/* ---- the run --------------------------------------------------------------------------------------------------- */

/* Each step of the start, in the log and for the fault handler: a crash before the first frame says where it was */
static void stage(const char *what)
{
    x360_set_stage(what);
    printf("x360: stage: %s\n", what);
    fflush(stdout);
}

/* The perf line's gpu field (host_perf_config.extra): the renderer's counters over the line's window, per guest frame.
 * The command processor runs inside the emu stage: emu minus gpu is the guest CPU and the runtime. */
typedef struct gpu_perf {
    x360_machine *x;
    x360_render_perf line, run; /* the totals when the line's window and the run started */
    char guest[256];            /* WP-19: the second line of the same window ("x360: perf2 ...", logged after it) */
} gpu_perf;

static void gpu_perf_extra(void *ctx, const struct host_perf_figures *f, int run, char *out, size_t n)
{
    gpu_perf *gp = ctx;
    x360_render_perf now;
    x360_render_perf_get(gp->x, &now);
    const x360_render_perf *since = run ? &gp->run : &gp->line;
    x360_render_perf_format(&now, since, f->guest_frames, "frame", out, n);
    x360_render_perf_format_guest(&now, since, f->guest_frames, f->seconds, "frame", gp->guest, sizeof gp->guest);
    if (!run)
        gp->line = now;
}

int x360_runtime_run(const x360_runtime_opts *o)
{
    if (!o || !o->xex || !o->pack_lib || !*o->pack_lib) {
        fprintf(stderr, "x360: a plain XEX and its pack are needed\n");
        return -1;
    }
    x360_install_fault_handler(NULL); /* the start too: the machine's own faults once it exists */
    stage("read the XEX");
    size_t n = 0;
    uint8_t *xex = read_all(o->xex, 256u << 20, &n);
    if (!xex) {
        fprintf(stderr, "x360: cannot read %s\n", o->xex);
        return -1;
    }
    char key[48], why[256] = "";
    if (o->title_key && *o->title_key)
        snprintf(key, sizeof key, "%s", o->title_key);
    else if (x360_runtime_key(xex, n, key, sizeof key))
        snprintf(key, sizeof key, "X360-UNKNOWN");
    stage("create the machine");
    x360_machine *x = x360_create(xex, n, why, sizeof why);
    free(xex);
    if (!x) {
        fprintf(stderr, "x360: %s does not start: %s\n", base_name(o->xex), why);
        return -1;
    }
    x360_install_fault_handler(x);
    stage("load the pack");
    char libpath[1100];
    snprintf(libpath, sizeof libpath, "%s%s", strchr(o->pack_lib, '/') ? "" : "./", o->pack_lib); /* not a search */
    void *lib = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    x360_pack pk;
    int not_pack = !lib || x360_pack_from_lib(lib, &pk);
    if (!not_pack)
        stage("bind the pack");
    if (not_pack || x360_bind(x, &pk)) {
        fprintf(stderr, "x360: the pack %s %s%s\n", o->pack_lib,
                !lib ? dlerror() : not_pack ? "is not an x360_tools.recomp pack" : "cannot be used: ",
                lib && !not_pack ? x360_error(x) : "");
        x360_destroy(x);
        if (lib)
            dlclose(lib);
        return X360_RUN_PACK_REFUSED;
    }
    if (getenv("RECOMP_X360_INTERP") && !strcmp(getenv("RECOMP_X360_INTERP"), "0"))
        x360_set_interp(x, 0); /* XBA-283: stop at the first missed address, as before */
    stage("open the disc");
    if (o->disc && *o->disc && x360_set_disc(x, o->disc, why, sizeof why))
        fprintf(stderr, "x360: disc %s: %s (running without it)\n", o->disc, why);
    if (o->data_dir && *o->data_dir) {
        mkdirs(o->data_dir);
        x360_set_data_dir(x, o->data_dir);
    }
    snprintf(s_work, sizeof s_work, "%s", o->work_dir ? o->work_dir : "");
    if (s_work[0]) { /* the microcode of every shader met: the next Prepare translates what the cache lacks */
        char sd[1200];
        snprintf(sd, sizeof sd, "%s/shaders", s_work);
        mkdirs(sd);
        x360_set_shader_dump(x, sd);
    }
    char cache[1100];
    if (o->shader_cache && *o->shader_cache) {
        snprintf(cache, sizeof cache, "%s", o->shader_cache);
    } else { /* next to the pack: <pack dir>/shaders */
        const char *slash = strrchr(o->pack_lib, '/');
        snprintf(cache, sizeof cache, "%.*s%sshaders", slash ? (int)(slash - o->pack_lib) : 0, o->pack_lib,
                 slash ? "/" : "");
    }
    printf("x360: %s, title %s, key %s, pack %s\n", x360_runtime_version(), base_name(o->xex), key,
           base_name(o->pack_lib));

    stage("open the window");
    /* the window, or none (headless checks: SDL dummy drivers) */
    int window = !o->headless && !host_window_headless();
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0)
        SDL_InitSubSystem(SDL_INIT_EVENTS);
    int scale = o->scale >= 0 ? o->scale : host_window_env_scale("RECOMP_X360_SCALE", HOST_SCALE_ASPECT);
    int filter = o->filter >= 0 ? o->filter : host_window_env_filter("RECOMP_X360_FILTER", HOST_FILTER_LINEAR);
    host_window_set_scaling(scale, filter);
    host_window_set_aspect(16, 9);
    host_window_shader_env("RECOMP_X360_SHADER");
    if (window && !host_window_open("XRecomp Xbox 360", RENDER_W, RENDER_H, host_window_env_vsync())) {
        fprintf(stderr, "x360: no window, running headless\n");
        window = 0;
    }
    if (!window)
        host_pad_init();
    console_pad_load(&s_pad_map, CONSOLE_PAD_X360, getenv(console_pad_env(CONSOLE_PAD_X360)));
    int upscale = o->upscale > 0 ? o->upscale : env_int("RECOMP_X360_UPSCALE", env_int("RECOMP_UPSCALE", 1));
    upscale = upscale < 1 ? 1 : upscale > 8 ? 8 : upscale;
    x360_title_env(key, stdout);
    stage("start the renderer");
    /* the renderer on the window's context (this thread runs the machine), or off-screen with frames read back */
    int render = x360_set_renderer_ex(x, cache, NULL, 0, RENDER_W, RENDER_H, upscale, window, why, sizeof why) == 0;
    if (!render)
        fprintf(stderr, "x360: no renderer (%s): running without drawing\n", why);
    else if (o->screenshot)
        x360_render_set_readback(x, 1);
    stage("open the audio");
    int audio = host_audio_open(AUDIO_RATE, 2);
    FILE *wav = o->wav_path && *o->wav_path ? fopen(o->wav_path, "w+b") : NULL;
    uint32_t wav_frames = 0;
    if (wav)
        wav_header(wav, 0);
    printf("x360: window %s, renderer %s (internal resolution x%d), audio %s (48 kHz), XMA backend %s, pads %u\n",
           window ? "on" : "off", render ? (window ? "on the window" : "off-screen") : "off",
           render ? x360_render_scale(x) : 0, audio ? "on" : "off (no device)", x360_xma_backend_name(),
           host_pad_mask());

    pthread_mutex_lock(&s_mu);
    s_x = x;
    s_machine_thread = pthread_self(), s_machine_thread_set = 1;
    snprintf(s_key, sizeof s_key, "%s", key);
    pthread_mutex_unlock(&s_mu);
    recomp_exit_hook_set(on_exit_hook);
    if (!o->save_dir || !*o->save_dir ||
        snprintf(s_state_dir, sizeof s_state_dir, "%s/x360-%s", o->save_dir, key) >= (int)sizeof s_state_dir)
        s_state_dir[0] = 0;
    recomp_state_ops sops = {"x360", state_save_op, state_load_op, NULL};
    recomp_states_attach(&sops, s_state_dir);
    printf("x360: save states %s%s, fast-forward x%d%s\n", s_state_dir[0] ? "in " : "off (no save dir)", s_state_dir,
           recomp_ff_speed(), recomp_ff_speed() ? "" : " (max)");
    recomp_rewinder rw;
    recomp_rewinder_init(&rw, "x360", NULL);
    recomp_cheats cheats;
    recomp_cheats_init(&cheats, RECOMP_CHEAT_X360);
    recomp_cheats_load_env(&cheats, "x360");
    recomp_cheat_bus cheat_bus = {x, NULL, NULL, cheat_read32, cheat_write32, 1, NULL, 0, NULL};
    unsigned cheat_polls = 0;
    int resume_on = recomp_states_resume_enabled() && s_state_dir[0];
    if (resume_on) {
        stage("resume from the automatic slot");
        int r = recomp_states_resume();
        printf("x360: resume from the automatic slot: %s\n",
               r == RECOMP_STATE_EEMPTY ? "nothing saved yet" : recomp_states_error(r));
    }
    int gpu_thread = x360_gpu_overlap_wanted();
    x360_gpu_set_worker_hook(x, recomp_host_worker_hook, NULL); /* the worker in the ADPF session, on the fast cores */
    printf("x360: GPU thread %s\n", gpu_thread ? "on (the machine on a worker thread)" : "off (RECOMP_X360_GPU_THREAD=0)");
    stage("first frame");
    recomp_play play;
    recomp_play_init(&play, "x360", "frame", &rw);
    fflush(stdout);

    static int16_t pcm[AUDIO_RATE / 10 * 2];
    recomp_ff_carry ff_carry;
    memset(&ff_carry, 0, sizeof ff_carry);
    uint64_t frames = 0, t0 = now_ns(), next = t0, late_resets = 0;
    int was_paused = 0, rc = 0, stopped = 0, heard = 0;
    recomp_perf_hint_begin((int64_t)FRAME_NS);
    enum { ST_EMU, ST_AUDIO, ST_PRESENT, ST_N };
    host_perf_config hp_cfg = {
        .console = "x360", .unit = "frames/s", .nstages = ST_N,
        .stage = {"emu", "audio", "present"}, .kind = {HOST_PERF_EMU, HOST_PERF_OTHER, HOST_PERF_RENDER},
        .rest = "other", .rest_kind = HOST_PERF_OTHER,
    };
    hp_cfg.frame_ns = FRAME_NS;
    gpu_perf gp = {.x = x};
    x360_render_perf_get(x, &gp.line);
    gp.run = gp.line;
    hp_cfg.extra = gpu_perf_extra, hp_cfg.extra_ctx = &gp;
    host_perf hp;
    host_perf_init(&hp, &hp_cfg, t0);

    for (;;) {
        int running = 1;
        if (window) {
            running = host_window_pump();
        } else {
            SDL_Event ev;
            host_pad_update();
            running = SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) <= 0;
            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        }
        if (!running) {
            printf("x360: quit asked after %llu frames\n", (unsigned long long)frames);
            if (resume_on && frames > 0)
                printf("x360: automatic slot %s\n", recomp_states_error(recomp_states_save(RECOMP_STATE_AUTO)));
            break;
        }
        if (recomp_lifecycle_paused()) {
            if (!was_paused) {
                pthread_mutex_lock(&s_mu);
                save_missed_locked();
                pthread_mutex_unlock(&s_mu);
                printf("x360: paused at frame %llu\n", (unsigned long long)frames);
                fflush(stdout);
                was_paused = 1;
                host_perf_pause(&hp, now_ns());
            }
            SDL_Delay(16);
            continue;
        }
        if (was_paused) {
            printf("x360: resumed\n");
            fflush(stdout);
            was_paused = 0;
            next = now_ns();
            host_perf_resume(&hp, next);
        }
        uint64_t work_t0 = now_ns();
        host_perf_frame(&hp, work_t0, NULL);
        uint32_t hk_held = 0, hk_pressed = 0;
        pads_in(x, o, window, &hk_held, &hk_pressed);
        int nks = 0;
        const Uint8 *ks = window ? SDL_GetKeyboardState(&nks) : NULL;
        recomp_play_frame pf;
        recomp_play_begin(&play, hk_held, hk_pressed, ks, nks, frames, &pf);
        int ff = pf.ff, speed = pf.speed;
        if (pf.ff_changed) {
            memset(&ff_carry, 0, sizeof ff_carry);
            next = now_ns();
        }
        uint64_t burst_end = now_ns() + FRAME_NS * 9 / 10;
        if (pf.rewinding) {
            pthread_mutex_lock(&s_mu);
            if (recomp_rewinder_step(&rw, rewind_load, x) == 0)
                recomp_states_note_load();
            while (x360_audio_pull(x, pcm, sizeof pcm / sizeof pcm[0] / 2) > 0)
                ;
            pthread_mutex_unlock(&s_mu);
            next = now_ns();
        }
        if (++cheat_polls % 30 == 0)
            recomp_cheats_poll(&cheats);
        for (int step = 0; step < pf.steps; step++) {
            uint64_t te = now_ns();
            pthread_mutex_lock(&s_mu);
            /* WP-22 stage 2: the machine on a CPU worker, the command processor and the renderer here, at once */
            int r = gpu_thread ? x360_gpu_overlap(x, x360_run_until, x360_time(x) + FRAME_100NS)
                               : x360_run_until(x, x360_time(x) + FRAME_100NS);
            if ((frames & 255) == 0) { /* XBA-283: runs go on past missed code: keep discovered.txt current */
                const uint32_t *mm;
                unsigned nm = x360_missed(x, &mm);
                if (nm != s_saved_missed) {
                    s_saved_missed = nm;
                    save_missed_locked();
                }
            }
            if (r == X360_FRAME && frames == 0 && step == 0)
                stage("running");
            if (r == X360_FRAME) {
                if (recomp_cheats_pending(&cheats))
                    recomp_cheats_apply(&cheats, &cheat_bus);
                recomp_rewinder_frame(&rw, rewind_size, rewind_save, x);
            }
            if (s_state_loaded) {
                s_state_loaded = 0;
                next = now_ns();
            }
            pthread_mutex_unlock(&s_mu);
            host_perf_stage(&hp, ST_EMU, now_ns() - te);
            if (r != X360_FRAME) {
                if (r == X360_EXITED)
                    printf("x360: the title ended at frame %llu (exit code %u)\n", (unsigned long long)frames,
                           x360_exit_code(x));
                else
                    fprintf(stderr, "x360: the machine stopped at frame %llu: %s\n", (unsigned long long)frames,
                            r == X360_MISSED ? "an address the pack does not cover (discovered.txt for the next "
                                               "Prepare)"
                                             : x360_error(x));
                stopped = 1;
                rc = r == X360_EXITED ? 0 : -1;
                break;
            }
            frames++;
            host_perf_guest(&hp, 1);
            uint64_t ta = now_ns();
            size_t got;
            while ((got = x360_audio_pull(x, pcm, sizeof pcm / sizeof pcm[0] / 2)) > 0) {
                if (wav && fwrite(pcm, 4, got, wav) == got)
                    wav_frames += (uint32_t)got;
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
        rumble(x);
        if (render) {
            x360_render_end_frame(x);
            /* Programs linked this frame (at first use, from the translated shader cache): the shader badge's live
             * count (XBA-314). */
            static uint64_t programs_seen;
            uint64_t programs = x360_render_programs(x);
            for (uint64_t i = programs_seen; i < programs && i < programs_seen + 64; i++)
                host_shader_progress_live();
            programs_seen = programs;
        }
        if (window && !stopped) {
            uint64_t tp = now_ns();
            int w = 0, h = 0;
            unsigned tex = render ? x360_render_texture(x, &w, &h) : 0;
            if (!tex || !host_window_present_texture(tex, w, h, 1)) {
                static const uint8_t black[16 * 9 * 4] = {0};
                host_window_present_rgba(black, 16, 9, 64); /* nothing drawn yet */
            }
            host_perf_stage(&hp, ST_PRESENT, now_ns() - tp);
            host_perf_shown(&hp, 1);
        }
        if (host_perf_log_due(&hp, now_ns()))
            printf("x360: perf2 %s\n", gp.guest), fflush(stdout);
        if (stopped || (o->frames && frames >= o->frames))
            break;
        recomp_perf_hint_frame((int64_t)(now_ns() - work_t0), (int64_t)FRAME_NS);
        if (!o->unpaced && !(ff && !speed)) {
            next += FRAME_NS;
            if (audio && heard) { /* audio-driven: the queue keeps the clock honest (2 %) */
                uint32_t q = host_audio_queued();
                if (q > AUDIO_TARGET_FRAMES * 2)
                    next += FRAME_NS / 50;
                else if (q < AUDIO_TARGET_FRAMES / 2)
                    next -= FRAME_NS / 50;
            }
            uint64_t t = now_ns();
            if (t > next + 3 * FRAME_NS) {
                next = t;
                late_resets++;
            } else if (next > t) {
                host_window_sleep_until(next);
                host_perf_idle(&hp, now_ns() - t);
            }
        }
    }

    recomp_perf_hint_end();
    host_perf_log_run(&hp, now_ns(), NULL);
    printf("x360: perf2 over the run: %s\n", gp.guest), fflush(stdout);
    recomp_states_detach();
    recomp_ff_hold(0);
    recomp_rewinder_free(&rw);
    recomp_cheats_restore(&cheats, &cheat_bus);
    recomp_cheats_free(&cheats);
    s_state_dir[0] = 0;
    if (wav) {
        wav_header(wav, wav_frames);
        fclose(wav);
        printf("x360: %u audio frames written to %s\n", wav_frames, o->wav_path);
    }
    if (audio)
        host_audio_close();
    host_perf_live_clear();
    double secs = (double)(now_ns() - t0) / 1e9;
    x360_stats xs;
    x360_get_stats(x, &xs);
    uint64_t xp, xf, xb;
    unsigned xc;
    x360_xma_get_stats(x, &xp, &xf, &xb, &xc);
    printf("x360: %llu frames in %.1f s (%.2f frames/s), %llu late resets; %llu instructions native, %llu "
           "interpreted, %llu swaps; XMA: %u contexts, %llu packets, %llu frames\n",
           (unsigned long long)frames, secs, secs > 0 ? (double)frames / secs : 0.0, (unsigned long long)late_resets,
           (unsigned long long)xs.native, (unsigned long long)xs.interpreted, (unsigned long long)xs.gpu_swaps, xc,
           (unsigned long long)xp, (unsigned long long)xf);
    x360_render_stats(x, stdout);
    if (o->screenshot) {
        int w = 0, h = 0;
        const uint8_t *f = x360_render_frame(x, &w, &h);
        FILE *ppm = f ? fopen(o->screenshot, "wb") : NULL;
        if (ppm) {
            fprintf(ppm, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; i++)
                fwrite(f + 4 * i, 1, 3, ppm);
            fclose(ppm);
            printf("x360: last frame (%dx%d) written to %s\n", w, h, o->screenshot);
        } else {
            fprintf(stderr, "x360: no frame to write to %s\n", o->screenshot);
            rc = -1;
        }
    }
    fflush(stdout);
    recomp_exit_hook_set(NULL);
    for (int p = 0; p < 4; p++)
        if (s_motor[p][0] || s_motor[p][1]) {
            host_pad_rumble(p, 0, 0, 0);
            s_motor[p][0] = s_motor[p][1] = 0;
        }
    pthread_mutex_lock(&s_mu);
    save_missed_locked();
    s_x = NULL;
    s_machine_thread_set = 0;
    pthread_mutex_unlock(&s_mu);
    x360_destroy(x); /* the renderer first, while the window's context is current */
    if (window)
        host_window_close();
    dlclose(lib);
    return rc;
}

/* ---- the app shell's entry ------------------------------------------------------------------------------------- */

static char s_start_work[1024], s_start_save[1024];

void x360_runtime_set_work_dir(const char *dir)
{
    snprintf(s_start_work, sizeof s_start_work, "%s", dir ? dir : "");
}

void x360_runtime_set_save_dir(const char *dir)
{
    snprintf(s_start_save, sizeof s_start_save, "%s", dir ? dir : "");
}

int x360_runtime_start(const char *xex, const char *pack_lib, const char *title_key)
{
    x360_runtime_opts o;
    x360_runtime_opts_init(&o);
    o.xex = xex;
    o.pack_lib = pack_lib;
    o.title_key = title_key;
    o.disc = getenv("RECOMP_X360_DISC");
    o.shader_cache = getenv("RECOMP_X360_SHADERS");
    o.save_dir = s_start_save[0] ? s_start_save : NULL;
    char work[1200], data[1300];
    if (s_start_work[0] && title_key && *title_key) {
        snprintf(work, sizeof work, "%s/x360-%s", s_start_work, title_key);
        mkdirs(work);
        o.work_dir = work;
        snprintf(data, sizeof data, "%s/data", work); /* cache: / hdd: and save data */
        o.data_dir = data;
    }
    char saved[1300];
    if (s_start_save[0] && title_key && *title_key) {
        /* XBA-282: the title's own saves (hdd:, cache:) live with its states in <save>/x360-<KEY>/data, the folder
         * the app exports, imports and syncs (SaveArchive, CloudSaves); a work/x360-<KEY>/data an earlier build
         * wrote is moved there once (kept where it is when the move fails: another file system) */
        snprintf(saved, sizeof saved, "%s/x360-%s/data", s_start_save, title_key);
        struct stat st;
        int have_new = stat(saved, &st) == 0;
        if (!have_new && o.data_dir && stat(o.data_dir, &st) == 0) {
            char parent[1100];
            snprintf(parent, sizeof parent, "%s/x360-%s", s_start_save, title_key);
            mkdirs(parent);
            if (rename(o.data_dir, saved) == 0) {
                printf("x360: save data moved to %s\n", saved);
                o.data_dir = saved;
            } else {
                fprintf(stderr, "x360: save data stays in %s (cannot move it: %s)\n", o.data_dir, strerror(errno));
            }
        } else {
            o.data_dir = saved;
        }
    }
    const char *frames = getenv("RECOMP_X360_FRAMES");
    if (frames && *frames)
        o.frames = strtoull(frames, NULL, 10);
    printf("x360 runtime ready (%s; title %s, pack %s)\n", x360_runtime_version(), title_key ? title_key : "?",
           pack_lib ? pack_lib : "none");
    fflush(stdout);
    return x360_runtime_run(&o);
}
