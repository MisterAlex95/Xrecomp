/* n64_runtime.c -- see n64_runtime.h (XBA-146, XBA-174).
 *
 * The PS1 runtime's shape (consoles/ps1/runtime/ps1_runtime.c): one thread does everything, frame by frame: pump the
 * window's events (quit, background, pads), read the pads, run one N64 frame (to the next VI interrupt), queue its
 * audio, present the VI's picture, then sleep to the next frame's deadline. The frame's length comes from the CPU
 * cycles the core ran (its VI follows the game's timing: NTSC 60 Hz, PAL 50 Hz, one field per interrupt when
 * interlaced), so pacing is the N64's own VI rate; absolute deadlines (no drift), nudged by the audio queue once the
 * AI makes sound, a stall of more than three frames resets the clock. While the app is paused (background, the
 * in-game menu: recomp_lifecycle) no frame runs, the saves are written and the window keeps pumping.
 *
 * Picture: n64_display gives the VI's visible area at framebuffer resolution (320 x 240, 640 x 480, 512 x 288 PAL...)
 * as XRGB; it is shown at 4:3 whatever its size (host_window_set_aspect), as a television did. */
#define _GNU_SOURCE /* pread, nanosleep */
#include "n64_runtime.h"

#include "host_audio.h"
#include "console_pad.h"
#include "host_pad.h"
#include "host_window.h"
#include "n64_core.h"
#include "n64_pack.h"
#include "n64_rdp.h"
#include "recomp_exit_hook.h"
#include "recomp_lifecycle.h"

#include <SDL.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RT_CPU_HZ 93750000ull
#define SAVE_DEBOUNCE_NS 1000000000ull            /* a second after the last write to a save */
#define SAVE_LATEST_NS 5000000000ull              /* and at most five after the first */
#define AUDIO_TARGET_FRAMES (N64_AUDIO_RATE / 20) /* 50 ms queued */
#define FPS_LOG_NS 10000000000ull                 /* a frame-rate line every 10 s (the device check) */
#define STICK_RANGE 80
#define N_SAVES 9 /* n64_save_kind: NONE .. PAK3 */

const char *n64_runtime_version(void)
{
    return "n64_runtime " N64_RUNTIME_VERSION;
}

/* ---- Pure helpers --------------------------------------------------------------------------------------------- */

/* The player's mapping (console_pad.h, RECOMP_N64_PAD_MAP), loaded when a game starts; the defaults until then. */
static ConsolePadMap s_pad_map;

uint16_t n64_runtime_buttons_of(uint32_t t)
{
    static const uint16_t bits[N64_T_COUNT] = {
        [N64_T_A] = N64_BTN_A, [N64_T_B] = N64_BTN_B, [N64_T_Z] = N64_BTN_Z, [N64_T_L] = N64_BTN_L,
        [N64_T_R] = N64_BTN_R, [N64_T_START] = N64_BTN_START, [N64_T_UP] = N64_BTN_DUP, [N64_T_DOWN] = N64_BTN_DDOWN,
        [N64_T_LEFT] = N64_BTN_DLEFT, [N64_T_RIGHT] = N64_BTN_DRIGHT, [N64_T_C_UP] = N64_BTN_CUP,
        [N64_T_C_DOWN] = N64_BTN_CDOWN, [N64_T_C_LEFT] = N64_BTN_CLEFT, [N64_T_C_RIGHT] = N64_BTN_CRIGHT,
    };
    uint16_t k = 0;
    for (int i = 0; i < N64_T_COUNT; i++)
        if (t & (1u << i))
            k |= bits[i];
    /* Opposite directions at once (two pads, a worn stick): a real controller cannot, some games misbehave. */
    const uint16_t pairs[4][2] = {{N64_BTN_DUP, N64_BTN_DDOWN}, {N64_BTN_DLEFT, N64_BTN_DRIGHT},
                                  {N64_BTN_CUP, N64_BTN_CDOWN}, {N64_BTN_CLEFT, N64_BTN_CRIGHT}};
    for (int i = 0; i < 4; i++)
        if ((k & pairs[i][0]) && (k & pairs[i][1]))
            k &= (uint16_t)~(pairs[i][0] | pairs[i][1]);
    return k;
}

uint16_t n64_runtime_pad_buttons(uint32_t b, int rx, int ry, int lt, int rt)
{
    ConsolePadMap m;
    ConsolePadOut o;
    HostPadState st = {.connected = 1, .buttons = b};
    st.axis[HOST_PAD_AXIS_RX] = (int16_t)rx;
    st.axis[HOST_PAD_AXIS_RY] = (int16_t)ry;
    st.axis[HOST_PAD_AXIS_LT] = (int16_t)lt;
    st.axis[HOST_PAD_AXIS_RT] = (int16_t)rt;
    console_pad_defaults(&m, CONSOLE_PAD_N64);
    console_pad_apply(&m, &st, &o);
    return n64_runtime_buttons_of(o.buttons);
}

void n64_runtime_stick(int lx, int ly, int range, int dead, int8_t *x, int8_t *y)
{
    if (range <= 0)
        range = STICK_RANGE;
    if (range > 127)
        range = 127;
    if (dead < 0)
        dead = 32767 * 8 / 100;
    if (dead > 30000)
        dead = 30000;
    double fx = lx, fy = -(double)ly; /* the N64's +y is up */
    double r = sqrt(fx * fx + fy * fy);
    if (r <= dead) {
        *x = *y = 0;
        return;
    }
    double t = (r - dead) / (32767.0 - dead); /* 0..1 past the dead zone */
    if (t > 1.0)
        t = 1.0;
    /* The host stick's round gate to the N64's octagon: the diagonals go 20 % further (0.707 x 1.2 = 0.85 a side). */
    double s2 = 2.0 * fabs(fx) * fabs(fy) / (r * r); /* sin 2θ: 0 on an axis, 1 on a diagonal */
    double mag = t * range * (1.0 + 0.2 * s2);
    double vx = fx / r * mag, vy = fy / r * mag;
    if (vx > range) vx = range;
    if (vx < -range) vx = -range;
    if (vy > range) vy = range;
    if (vy < -range) vy = -range;
    *x = (int8_t)lround(vx);
    *y = (int8_t)lround(vy);
}

void n64_runtime_xrgb_to_rgba(const uint32_t *src, uint8_t *dst, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t c = src[i];
        dst[4 * i + 0] = (uint8_t)(c >> 16);
        dst[4 * i + 1] = (uint8_t)(c >> 8);
        dst[4 * i + 2] = (uint8_t)c;
        dst[4 * i + 3] = 255;
    }
}

static const char *save_name(int kind)
{
    switch (kind) {
    case N64_SAVE_EEPROM4K:
    case N64_SAVE_EEPROM16K: return "game.eep";
    case N64_SAVE_SRAM: return "game.sra";
    case N64_SAVE_FLASH: return "game.fla";
    case N64_SAVE_PAK0: return "pak1.mpk";
    case N64_SAVE_PAK1: return "pak2.mpk";
    case N64_SAVE_PAK2: return "pak3.mpk";
    case N64_SAVE_PAK3: return "pak4.mpk";
    default: return NULL;
    }
}

int n64_runtime_save_path(const char *save_dir, const char *key, int kind, char *out, size_t out_n)
{
    const char *name = save_name(kind);
    if (!save_dir || !*save_dir || !key || !*key || !out || !name)
        return -1;
    int n = snprintf(out, out_n, "%s/n64-%s/%s", save_dir, key, name);
    return n < 0 || (size_t)n >= out_n ? -1 : 0;
}

static char code_char(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? (char)c : '_';
}

int n64_runtime_fallback_key(const uint8_t *rom, size_t n, char *out, size_t out_n)
{
    uint8_t h[0x40];
    if (!rom || n < sizeof h || !out || out_n < 16)
        return -1;
    memcpy(h, rom, sizeof h);
    if (n64_rom_normalize(h, sizeof h) != 0)
        return -1;
    snprintf(out, out_n, "%c%c%c%c-%02X%02X%02X%02X", code_char(h[0x3B]), code_char(h[0x3C]), code_char(h[0x3D]),
             code_char(h[0x3E]), h[0x10], h[0x11], h[0x12], h[0x13]);
    return 0;
}

uint64_t n64_runtime_frame_ns(uint64_t cycles)
{
    uint64_t ns = cycles * 1000000000ull / RT_CPU_HZ;
    const uint64_t lo = 1000000000ull / 75, hi = 1000000000ull / 45;
    return ns < lo ? lo : ns > hi ? hi : ns;
}

int n64_runtime_pak_parse(const char *v)
{
    if (!v)
        return N64_RT_PAK_DEFAULT;
    if (!strcasecmp(v, "controller") || !strcasecmp(v, "pak") || !strcasecmp(v, "mempak") ||
        !strcasecmp(v, "controller_pak"))
        return N64_RT_PAK_CONTROLLER;
    if (!strcasecmp(v, "rumble") || !strcasecmp(v, "rumble_pak"))
        return N64_RT_PAK_RUMBLE;
    if (!strcasecmp(v, "none") || !strcasecmp(v, "off"))
        return N64_RT_PAK_NONE;
    return N64_RT_PAK_DEFAULT;
}

int n64_runtime_frameskip_parse(const char *v)
{
    if (v && v[0] >= '0' && v[0] <= '9' && !v[1])
        return v[0] - '0';
    return N64_RT_FRAMESKIP_AUTO;
}

int n64_runtime_frameskip(int mode, int64_t behind_ns, uint64_t frame_ns, int audio_low, int *run)
{
    int skip;
    if (mode == 0)
        skip = 0;
    else if (mode > 0)
        skip = *run < mode;
    else
        skip = *run < 3 && (behind_ns > (int64_t)(frame_ns / 2) || audio_low);
    *run = skip ? *run + 1 : 0;
    return skip;
}

/* ---- Files ---------------------------------------------------------------------------------------------------- */

static int spec_fd(const char *spec, int *own)
{
    if (strncmp(spec, "fd:", 3) == 0) {
        char *end;
        long v = strtol(spec + 3, &end, 10);
        *own = 0;
        return end == spec + 3 || *end || v < 0 ? -1 : (int)v;
    }
    *own = 1;
    return open(spec, O_RDONLY | O_CLOEXEC);
}

/* A whole file, or "fd:N" read from offset 0 (the descriptor stays open and its offset is not used). NULL when it
 * cannot be read, is empty or is larger than max. */
static uint8_t *read_all(const char *spec, size_t max, size_t *size)
{
    int own, fd = spec_fd(spec, &own);
    *size = 0;
    if (fd < 0)
        return NULL;
    size_t cap = 1 << 16, n = 0;
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

static void mkdirs(const char *path)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

/* Writes path through path.tmp + rename, so a kill mid-write leaves the old save. */
static int write_file(const char *path, const uint8_t *data, size_t n)
{
    char tmp[1300], dir[1200];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdirs(dir);
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return -1;
    int ok = fwrite(data, 1, n, f) == n;
    ok &= fflush(f) == 0;
    ok &= fsync(fileno(f)) == 0;
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int write_ppm(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = {(uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i]};
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f);
}

/* ---- Saves ---------------------------------------------------------------------------------------------------- */

/* The run's machine and its saves, shared with the exit hook (the in-game menu's Quit ends the process from the UI
 * thread) and the lifecycle listener: s_mu is held while a frame runs and while a save is written. */
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static n64_machine *s_m;
static char s_save_dir[1024], s_key[64];
static uint8_t *s_saved[N_SAVES]; /* each image as last read or written (or as the run started), by kind */
static char s_discovered[1200];   /* n64_runtime_opts.discovered_path of the run */

static void save_path(int kind, char *out, size_t n)
{
    if (n64_runtime_save_path(s_save_dir, s_key, kind, out, n) != 0)
        out[0] = 0;
}

/* Writes the cartridge's save (once its kind is known) and each controller pak that differs from what is on disk
 * (or was in the machine at start). Called with s_mu held. */
static void saves_flush_locked(const char *why)
{
    if (!s_m || !s_save_dir[0])
        return;
    int kinds[5] = {n64_save_kind_detected(s_m), N64_SAVE_PAK0, N64_SAVE_PAK1, N64_SAVE_PAK2, N64_SAVE_PAK3};
    for (int i = 0; i < 5; i++) {
        int kind = kinds[i];
        size_t n = 0;
        const uint8_t *d = kind != N64_SAVE_NONE ? n64_save_data(s_m, (n64_save_kind)kind, &n) : NULL;
        if (!d || !n)
            continue;
        if (s_saved[kind] && memcmp(d, s_saved[kind], n) == 0)
            continue;
        char path[1200];
        save_path(kind, path, sizeof path);
        if (!path[0])
            continue;
        if (write_file(path, d, n) != 0) {
            fprintf(stderr, "n64: cannot write the save %s: %s\n", path, strerror(errno));
            continue;
        }
        if (!s_saved[kind])
            s_saved[kind] = malloc(n);
        if (s_saved[kind])
            memcpy(s_saved[kind], d, n);
        printf("n64: save written (%zu bytes, %s) to %s\n", n, why, path);
    }
    fflush(stdout);
}

static void saves_flush(const char *why)
{
    pthread_mutex_lock(&s_mu);
    saves_flush_locked(why);
    pthread_mutex_unlock(&s_mu);
}

static void on_exit_hook(const char *why)
{
    (void)why;
    saves_flush("exit");
    /* The in-game menu's Quit ends the process here: the discovery loop's code too (XBA-174). */
    pthread_mutex_lock(&s_mu);
    if (s_m && s_discovered[0])
        n64_pack_save_missed(s_m, s_discovered);
    pthread_mutex_unlock(&s_mu);
}

/* The app paused (background, the in-game menu): the saves go to disk at once, from the thread that paused it (on
 * Android SDL parks the thread that pumps events in the background, and the process can be killed there). Registered
 * once; no run, no-op. */
static void on_lifecycle(int paused, void *user)
{
    (void)user;
    if (paused)
        saves_flush("pause");
}

static void snapshot(n64_machine *m, int kind)
{
    size_t n = 0;
    const uint8_t *d = n64_save_data(m, (n64_save_kind)kind, &n);
    free(s_saved[kind]);
    s_saved[kind] = NULL;
    if (d && n && (s_saved[kind] = malloc(n)))
        memcpy(s_saved[kind], d, n);
    n64_save_take_dirty(m, (n64_save_kind)kind);
}

/* Loads the cartridge save found on disk (game.eep / game.sra / game.fla: the one the header's save type names
 * first, else the first there) and the controller paks. An .eep's size picks 4K (512 B) or 16K (2 KB) unless the
 * kind is known already; then a 2 KB file for a 4K game (some emulators always write 2 KB) gives its first 512 bytes,
 * a 512-byte one for a 16K game is padded with 0xFF. Called with s_mu held. */
static void saves_load(n64_machine *m)
{
    char path[1200];
    int known = n64_save_kind_detected(m);
    const int order[3] = {N64_SAVE_EEPROM4K, N64_SAVE_SRAM, N64_SAVE_FLASH};
    int loaded = 0;
    for (int pass = 0; pass < 2 && !loaded && s_save_dir[0]; pass++) {
        for (int i = 0; i < 3 && !loaded; i++) {
            int kind = order[i];
            int is_eep = kind == N64_SAVE_EEPROM4K;
            int matches = known == kind || (is_eep && known == N64_SAVE_EEPROM16K);
            if (pass == 0 ? !matches : known != N64_SAVE_NONE)
                continue; /* first the header's kind; the others only while it is unknown */
            save_path(kind, path, sizeof path);
            size_t n = 0;
            uint8_t *d = path[0] ? read_all(path, 128 * 1024, &n) : NULL;
            if (!d)
                continue;
            int want = kind;
            if (is_eep)
                want = known == N64_SAVE_EEPROM16K || (known == N64_SAVE_NONE && n == 2048) ? N64_SAVE_EEPROM16K
                                                                                              : N64_SAVE_EEPROM4K;
            size_t need = n64_save_size((n64_save_kind)want);
            uint8_t *img = malloc(need);
            if (img) {
                memset(img, 0xFF, need);
                memcpy(img, d, n < need ? n : need);
            }
            if (img && (n == need || is_eep) && n64_save_load(m, (n64_save_kind)want, img, need) == 0) {
                printf("n64: save loaded (%zu bytes, %s) from %s\n", need,
                       want == N64_SAVE_EEPROM4K ? "EEPROM 4K" : want == N64_SAVE_EEPROM16K ? "EEPROM 16K"
                       : want == N64_SAVE_SRAM ? "SRAM" : "FlashRAM", path);
                loaded = 1;
            } else {
                fprintf(stderr, "n64: %s is not a %s image (%zu bytes), ignored (kept until the game saves)\n", path,
                        is_eep ? "512 B / 2 KB EEPROM" : kind == N64_SAVE_SRAM ? "32 KB SRAM" : "128 KB FlashRAM", n);
            }
            free(img);
            free(d);
        }
    }
    if (!loaded && s_save_dir[0])
        printf("n64: no cartridge save yet (%s/n64-%s/)\n", s_save_dir, s_key);
    for (int p = 0; p < 4; p++) {
        int kind = N64_SAVE_PAK0 + p;
        save_path(kind, path, sizeof path);
        size_t n = 0;
        uint8_t *d = path[0] ? read_all(path, 64 * 1024, &n) : NULL;
        if (d && n == n64_save_size((n64_save_kind)kind) && n64_save_load(m, (n64_save_kind)kind, d, n) == 0)
            printf("n64: controller pak %d loaded from %s\n", p + 1, path);
        else if (d)
            fprintf(stderr, "n64: %s is not a 32 KB controller pak image (%zu bytes), ignored\n", path, n);
        free(d);
    }
    /* What is on disk now (or the fresh images): written once they change. */
    for (int kind = N64_SAVE_EEPROM4K; kind < N_SAVES; kind++)
        snapshot(m, kind);
}

static void saves_clear(void)
{
    for (int k = 0; k < N_SAVES; k++) {
        free(s_saved[k]);
        s_saved[k] = NULL;
    }
}

/* ---- The run -------------------------------------------------------------------------------------------------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t t)
{
    uint64_t now;
    while ((now = now_ns()) < t) {
        uint64_t d = t - now;
        struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
        nanosleep(&ts, NULL);
    }
}

static int env_choice(const char *name, const char *one, int def)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return def;
    return strcasecmp(v, one) == 0;
}

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : def;
}

#ifndef __ANDROID__
/* Desktop keys: arrows = stick, X = A, C = B, Z = Z, Enter = Start, Q / E = L / R, I / K / J / L = C up / down /
 * left / right, T / G / F / H = D-pad up / down / left / right. */
static uint16_t keyboard_buttons(int *kx, int *ky)
{
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint16_t k = 0;
    *kx = *ky = 0;
    if (!ks)
        return 0;
    if (ks[SDL_SCANCODE_X]) k |= N64_BTN_A;
    if (ks[SDL_SCANCODE_C]) k |= N64_BTN_B;
    if (ks[SDL_SCANCODE_Z]) k |= N64_BTN_Z;
    if (ks[SDL_SCANCODE_RETURN]) k |= N64_BTN_START;
    if (ks[SDL_SCANCODE_Q]) k |= N64_BTN_L;
    if (ks[SDL_SCANCODE_E]) k |= N64_BTN_R;
    if (ks[SDL_SCANCODE_I]) k |= N64_BTN_CUP;
    if (ks[SDL_SCANCODE_K]) k |= N64_BTN_CDOWN;
    if (ks[SDL_SCANCODE_J]) k |= N64_BTN_CLEFT;
    if (ks[SDL_SCANCODE_L]) k |= N64_BTN_CRIGHT;
    if (ks[SDL_SCANCODE_T]) k |= N64_BTN_DUP;
    if (ks[SDL_SCANCODE_G]) k |= N64_BTN_DDOWN;
    if (ks[SDL_SCANCODE_F]) k |= N64_BTN_DLEFT;
    if (ks[SDL_SCANCODE_H]) k |= N64_BTN_DRIGHT;
    if (ks[SDL_SCANCODE_LEFT]) *kx -= 32767;
    if (ks[SDL_SCANCODE_RIGHT]) *kx += 32767;
    if (ks[SDL_SCANCODE_UP]) *ky -= 32767;
    if (ks[SDL_SCANCODE_DOWN]) *ky += 32767;
    return k;
}
#endif

typedef struct pad_in {
    int any; /* a host pad drives this controller */
    uint16_t buttons;
    int8_t x, y;
} pad_in;

/* The host pads for N64 controller `port`, through the player's mapping (the on-screen pad joins host pad 1,
 * console_pad_read; the mapping's range / deadzone, when set, over the runtime's): with one player every pad drives
 * controller 1 (buttons merged, the most deflected stick wins); with more, host pad N drives controller N. */
static void pads_read(int port, int players, int range, int dead, pad_in *out)
{
    memset(out, 0, sizeof *out);
    if (!s_pad_map.console)
        console_pad_defaults(&s_pad_map, CONSOLE_PAD_N64);
    if (s_pad_map.range > 0)
        range = s_pad_map.range;
    if (s_pad_map.deadzone >= 0)
        dead = 32767 * s_pad_map.deadzone / 100;
    for (int p = 0; p < HOST_PAD_PORTS; p++) {
        if (players >= 2 ? p != port : port != 0)
            continue;
        ConsolePadOut o;
        if (!console_pad_read(&s_pad_map, p, &o))
            continue;
        out->any = 1;
        out->buttons |= n64_runtime_buttons_of(o.buttons);
        int8_t x, y;
        n64_runtime_stick(o.stick_x[0], o.stick_y[0], range, dead, &x, &y);
        if (abs(x) + abs(y) > abs(out->x) + abs(out->y)) {
            out->x = x;
            out->y = y;
        }
    }
}

/* The rumble pak's motor to the host pads (host_pad_rumble, both motors): with one player every pad (they all drive
 * controller 1), with more host pad N for controller N. Refreshed every frame while on (so a lost update cannot leave
 * a pad buzzing for long), stopped once when off. */
static void rumble_drive(n64_machine *m, int players, int sent[4])
{
    for (int port = 0; port < (players >= 2 ? players : 1); port++) {
        int on = n64_rumble(m, port);
        if (!on && !sent[port])
            continue;
        for (int p = 0; p < HOST_PAD_PORTS; p++)
            if (players >= 2 ? p == port : 1)
                host_pad_rumble(p, on ? 0xC000 : 0, on ? 0x8000 : 0, on ? 100 : 0);
        sent[port] = on;
    }
}

static void on_tty(void *ctx, char c)
{
    static char line[256];
    static size_t n;
    (void)ctx;
    if (c != '\n' && n < sizeof line - 1) {
        line[n++] = c;
        return;
    }
    line[n] = 0;
    printf("n64: tty: %s\n", line);
    fflush(stdout);
    n = 0;
}

void n64_runtime_opts_init(n64_runtime_opts *o)
{
    memset(o, 0, sizeof *o);
    o->scale = -1;
    o->filter = -1;
    o->pak = N64_RT_PAK_DEFAULT;
    o->players = -1;
    o->rdp_threads = -1;
    o->force_pal = -1;
    o->tty = -1;
    o->frameskip = N64_RT_FRAMESKIP_ENV;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static const char *pak_name(int pak)
{
    return pak == N64_RT_PAK_RUMBLE ? "rumble pak" : pak == N64_RT_PAK_NONE ? "no pak" : "controller pak";
}

static const char *save_kind_name(int kind)
{
    switch (kind) {
    case N64_SAVE_EEPROM4K: return "EEPROM 4K";
    case N64_SAVE_EEPROM16K: return "EEPROM 16K";
    case N64_SAVE_SRAM: return "SRAM";
    case N64_SAVE_FLASH: return "FlashRAM";
    default: return "none yet";
    }
}

int n64_runtime_run(const n64_runtime_opts *o)
{
    if (!o || !o->rom || !*o->rom) {
        fprintf(stderr, "n64: no ROM\n");
        return -1;
    }
    size_t rom_n = 0;
    uint8_t *rom = read_all(o->rom, N64_ROM_MAX, &rom_n);
    if (!rom) {
        fprintf(stderr, "n64: cannot read the ROM %s (missing, empty, over 64 MB or no permission)\n", o->rom);
        return -1;
    }
    char key[64];
    if (o->title_key && *o->title_key)
        snprintf(key, sizeof key, "%s", o->title_key);
    else if (n64_runtime_fallback_key(rom, rom_n, key, sizeof key) != 0)
        snprintf(key, sizeof key, "N64-UNKNOWN");

    n64_options mo = {.force_pal = o->force_pal >= 0 ? o->force_pal != 0 : -1, .save_kind = N64_SAVE_NONE};
    /* A pack's source identity (n64_pack.h): the SHA-256 of the ROM in big-endian order, computed only when there is
     * a pack to check (the ROM is in memory already). */
    char src_sha[65] = "";
    double sha_ms = 0;
    if (o->pack_lib && *o->pack_lib) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        n64_pack_rom_sha256(rom, rom_n, src_sha);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        sha_ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    }
    char err[256] = "";
    n64_machine *m = n64_create(rom, rom_n, &mo, err, sizeof err);
    free(rom);
    if (!m) {
        fprintf(stderr, "n64: the machine cannot start: %s\n", err[0] ? err : "?");
        return -1;
    }
    const n64_rom_info *info = n64_info(m);
    const char *order = info->order == N64_ROM_V64 ? "v64" : info->order == N64_ROM_N64 ? "n64" : "z64";
    int pal = o->force_pal >= 0 ? o->force_pal != 0 : info->pal;
    printf("n64: %s, ROM %s (%s, \"%s\", %s, %zu KB), key %s, CIC %s, %s\n", n64_runtime_version(), base_name(o->rom),
           order, info->title, info->game_code, rom_n / 1024, key, n64_cic_name(info->cic),
           pal ? "PAL (50 Hz)" : "NTSC (60 Hz)");
    if (o->pack_lib && *o->pack_lib)
        printf("n64: ROM identity %.16s... (%.0f ms)\n", src_sha, sha_ms);
    n64_pack pack;
    n64_pack_load(m, o->pack_lib, key, src_sha, &pack); /* the interpreter whatever happens (logged) */

    /* The RDP's scanline-band threads: 2 by default on Android (the phone's big cores), the core's elsewhere. */
    int threads = o->rdp_threads > 0 ? o->rdp_threads : env_int("RECOMP_N64_RDP_THREADS", 0);
#ifdef __ANDROID__
    if (threads <= 0 && !getenv("XBR_N64_RDP_THREADS"))
        threads = 2;
#endif
    struct n64_rdp *rdp = n64_get_rdp(m);
    int rdp_threads = 1;
    if (rdp) /* without a choice, the count n64_rdp_create took from $XBR_N64_RDP_THREADS (or 1) */
        rdp_threads = n64_rdp_set_threads(rdp, threads > 0 ? threads : env_int("XBR_N64_RDP_THREADS", 1));

    int pak = o->pak != N64_RT_PAK_DEFAULT ? o->pak : n64_runtime_pak_parse(getenv("RECOMP_N64_PAK"));
    if (pak == N64_RT_PAK_DEFAULT)
        pak = N64_RT_PAK_CONTROLLER;
    int players = o->players >= 1 ? o->players : env_int("RECOMP_N64_PLAYERS", 1);
    players = players < 1 ? 1 : players > 4 ? 4 : players;
    int range = env_int("RECOMP_N64_STICK_RANGE", STICK_RANGE);
    int dead_pct = env_int("RECOMP_N64_DEADZONE", 8);
    int dead = dead_pct >= 0 && dead_pct <= 90 ? 32767 * dead_pct / 100 : -1;
    n64_accessory acc = pak == N64_RT_PAK_RUMBLE ? N64_ACC_RUMBLE_PAK
                        : pak == N64_RT_PAK_NONE ? N64_ACC_NONE : N64_ACC_CONTROLLER_PAK;
    for (int port = 0; port < 4; port++) {
        n64_set_accessory(m, port, acc);
        n64_set_controller(m, port, port < players, 0, 0, 0);
    }
    int fskip = o->frameskip != N64_RT_FRAMESKIP_ENV ? o->frameskip
                                                      : n64_runtime_frameskip_parse(getenv("RECOMP_N64_FRAMESKIP"));
    if (fskip > N64_RT_FRAMESKIP_MAX)
        fskip = N64_RT_FRAMESKIP_MAX;
    if (fskip < N64_RT_FRAMESKIP_AUTO)
        fskip = N64_RT_FRAMESKIP_AUTO;
    if (fskip == N64_RT_FRAMESKIP_AUTO)
        printf("n64: frame skip auto (when late or the audio runs low, at most 3 in a row)\n");
    else
        printf("n64: frame skip %d\n", fskip);
    int tty = o->tty >= 0 ? o->tty : env_choice("RECOMP_N64_TTY", "1", 0);
    if (tty)
        n64_set_tty(m, on_tty, NULL);

    pthread_mutex_lock(&s_mu);
    s_m = m;
    snprintf(s_save_dir, sizeof s_save_dir, "%s", o->save_dir ? o->save_dir : "");
    snprintf(s_key, sizeof s_key, "%s", key);
    saves_load(m);
    snprintf(s_discovered, sizeof s_discovered, "%s", o->discovered_path ? o->discovered_path : "");
    pthread_mutex_unlock(&s_mu);
    recomp_exit_hook_set(on_exit_hook);
    static int listening;
    if (!listening && recomp_lifecycle_listen(on_lifecycle, NULL) == 0)
        listening = 1;

    int window = !o->headless && !host_window_headless();
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0)
        SDL_InitSubSystem(SDL_INIT_EVENTS);
    int scale = o->scale >= 0 ? o->scale
                              : env_choice("RECOMP_N64_SCALE", "integer", 0) ? HOST_SCALE_INTEGER : HOST_SCALE_ASPECT;
    int filter = o->filter >= 0 ? o->filter
                                : env_choice("RECOMP_N64_FILTER", "nearest", 0) ? HOST_FILTER_NEAREST
                                                                                 : HOST_FILTER_LINEAR;
    host_window_set_scaling(scale, filter);
    host_window_set_aspect(4, 3);
    if (window) {
        char title[300];
        snprintf(title, sizeof title, "XRecomp N64 - %s", info->title[0] ? info->title : base_name(o->rom));
        if (!host_window_open(title, 320 * 3, 240 * 3, 1)) {
            fprintf(stderr, "n64: no window, running headless\n");
            window = 0;
        }
    }
    if (!window)
        host_pad_init();
    console_pad_load(&s_pad_map, CONSOLE_PAD_N64, getenv(console_pad_env(CONSOLE_PAD_N64)));
    int audio = host_audio_open(N64_AUDIO_RATE, 2);
    printf("n64: window %s (4:3, %s, %s), audio %s, pads %u, %s%s, RDP %s, threads %d\n", window ? "on" : "off",
           scale == HOST_SCALE_INTEGER ? "integer" : "aspect", filter == HOST_FILTER_NEAREST ? "nearest" : "linear",
           audio ? "on" : "off", host_pad_mask(), pak_name(pak), players >= 2 ? ", several players" : "",
           n64_rdp_backend_name(rdp), rdp_threads);
    fflush(stdout);

    static uint32_t xrgb[N64_SCREEN_MAX_W * N64_SCREEN_MAX_H];
    static uint8_t rgba[N64_SCREEN_MAX_W * N64_SCREEN_MAX_H * 4];
    static int16_t pcm[N64_AUDIO_RATE / 10 * 2];
    int dw = 320, dh = 240, last_w = 0, last_h = 0, display_on = -1, last_kind = N64_SAVE_NONE;
    uint64_t frames = 0, next = now_ns(), t0 = next, late_resets = 0, dirty_at = 0, dirty_first = 0;
    uint64_t fps_t = t0, fps_frames = 0, last_frame_ns = 0;
    int was_paused = 0, heard = 0, rc = 0;
    int rumble_sent[4] = {0, 0, 0, 0};
    int64_t behind = 0; /* how late this frame starts (pacing below) */
    int skip_run = 0;
    uint64_t skipped = 0, skipped_log = 0;

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
            printf("n64: quit asked after %llu frames\n", (unsigned long long)frames);
            break;
        }
        if (recomp_lifecycle_paused()) {
            if (!was_paused) {
                printf("n64: paused at frame %llu\n", (unsigned long long)frames);
                fflush(stdout);
                saves_flush("pause");
                rumble_sent[0] = rumble_sent[1] = rumble_sent[2] = rumble_sent[3] = 1; /* stopped below on resume */
                for (int p = 0; p < HOST_PAD_PORTS; p++)
                    host_pad_rumble(p, 0, 0, 0);
                dirty_at = dirty_first = 0;
                was_paused = 1;
            }
            SDL_Delay(16);
            continue;
        }
        if (was_paused) {
            printf("n64: resumed\n");
            fflush(stdout);
            was_paused = 0;
            next = now_ns(); /* the time away is not caught up */
            fps_t = next;
            fps_frames = 0;
        }

        pad_in pads[4];
        for (int port = 0; port < players; port++)
            pads_read(port, players, range, dead, &pads[port]);
        pads[0].buttons |= o->buttons;
#ifndef __ANDROID__ /* a device's own controls may also come as keys (arrows for its D-pad): not the stick there */
        if (window) {
            int kx, ky;
            pads[0].buttons |= keyboard_buttons(&kx, &ky);
            if (kx || ky)
                n64_runtime_stick(kx, ky, range, 0, &pads[0].x, &pads[0].y);
        }
#endif
        pthread_mutex_lock(&s_mu);
        for (int port = 0; port < players; port++)
            n64_set_controller(m, port, 1, pads[port].buttons, pads[port].x, pads[port].y);
        uint64_t c0 = n64_cycles(m);
        n64_run_frame(m);
        uint64_t frame_ns = n64_runtime_frame_ns(n64_cycles(m) - c0);
        int kind = n64_save_kind_detected(m);
        int cart_dirty = kind != N64_SAVE_NONE && n64_save_take_dirty(m, (n64_save_kind)kind);
        if (kind != last_kind && kind != N64_SAVE_NONE && !cart_dirty && !s_saved[kind])
            snapshot(m, kind); /* detected at its first access: written once the game changes it */
        int dirty = cart_dirty;
        for (int p = 0; p < 4; p++)
            dirty |= n64_save_take_dirty(m, (n64_save_kind)(N64_SAVE_PAK0 + p));
        /* Frame skip: the picture of a frame the host is late for is not made nor shown (the frame itself ran). */
        int audio_low = audio && heard && !o->unpaced && host_audio_queued() < AUDIO_TARGET_FRAMES / 4;
        int skip = n64_runtime_frameskip(o->unpaced && fskip < 0 ? 0 : fskip, o->unpaced ? 0 : behind,
                                         frame_ns, audio_low, &skip_run);
        if ((o->frames && frames + 1 >= o->frames) || (o->capture_path && o->capture_frame == frames + 1))
            skip = 0; /* the last frame of a bounded run is the screenshot; a capture is a capture */
        int on = skip ? display_on > 0 : n64_display(m, xrgb, N64_SCREEN_MAX_W, N64_SCREEN_MAX_H, &dw, &dh) == 0;
        skipped += (uint64_t)skip;
        rumble_drive(m, players, rumble_sent);
        pthread_mutex_unlock(&s_mu);
        frames++;
        fps_frames++;

        if (kind != last_kind) {
            printf("n64: cartridge save type %s at frame %llu\n", save_kind_name(kind), (unsigned long long)frames);
            last_kind = kind;
        }
        if (frames == 2 || (frames > 2 && (frame_ns > last_frame_ns + 500000 || last_frame_ns > frame_ns + 500000))) {
            printf("n64: VI rate %.2f Hz at frame %llu\n", 1e9 / (double)frame_ns, (unsigned long long)frames);
            last_frame_ns = frame_ns;
        }
        if (skip) {
            /* keep the last picture and its size */
        } else if (!on || dw <= 0 || dh <= 0 || dw > N64_SCREEN_MAX_W || dh > N64_SCREEN_MAX_H) {
            dw = last_w ? last_w : 320;
            dh = last_h ? last_h : 240;
            memset(xrgb, 0, (size_t)dw * (size_t)dh * sizeof xrgb[0]);
        } else if (dw != last_w || dh != last_h) {
            printf("n64: display %d x %d at frame %llu\n", dw, dh, (unsigned long long)frames);
            last_w = dw;
            last_h = dh;
        }
        if (on != display_on) {
            if (display_on >= 0 || !on)
                printf("n64: display %s at frame %llu\n", on ? "on" : "off", (unsigned long long)frames);
            display_on = on;
        }

        /* Sound: everything the frame made (about 800 sample frames at 60 Hz). */
        size_t got;
        while ((got = n64_audio_pull(m, pcm, sizeof pcm / sizeof pcm[0] / 2)) > 0) {
            heard = 1;
            if (audio)
                host_audio_queue(pcm, (uint32_t)got);
        }

        uint64_t t = now_ns();
        if (dirty) {
            dirty_at = t;
            if (!dirty_first)
                dirty_first = t;
        }
        if (dirty_at && (t - dirty_at >= SAVE_DEBOUNCE_NS || t - dirty_first >= SAVE_LATEST_NS)) {
            saves_flush("written by the game");
            dirty_at = dirty_first = 0;
        }
        if (t - fps_t >= FPS_LOG_NS) {
            printf("n64: %.1f fps over the last %.0f s (VI %.1f Hz), %llu frames skipped, %llu late resets so far\n",
                   (double)fps_frames * 1e9 / (double)(t - fps_t), (double)(t - fps_t) / 1e9, 1e9 / (double)frame_ns,
                   (unsigned long long)(skipped - skipped_log), (unsigned long long)late_resets);
            skipped_log = skipped;
            fflush(stdout);
            fps_t = t;
            fps_frames = 0;
        }

        if (window && !skip) {
            if (o->capture_path && o->capture_frame == frames)
                host_window_capture(o->capture_path);
            n64_runtime_xrgb_to_rgba(xrgb, rgba, (size_t)dw * (size_t)dh);
            host_window_present_rgba(rgba, dw, dh, dw * 4);
        }
        if (o->frames && frames >= o->frames)
            break;

        if (!o->unpaced) {
            next += frame_ns;
            /* Audio-driven: a queue above its target slows the clock a little, below it speeds it up (±2%). */
            if (audio && heard) {
                uint32_t q = host_audio_queued();
                if (q > AUDIO_TARGET_FRAMES * 2)
                    next += frame_ns / 50;
                else if (q < AUDIO_TARGET_FRAMES / 2)
                    next -= frame_ns / 50;
            }
            t = now_ns();
            behind = (int64_t)(t - next);
            if (t > next + 3 * frame_ns) {
                next = t; /* a stall (a slow host, the debugger): no burst to catch up */
                late_resets++;
            } else if (next > t) {
                sleep_until(next);
            }
        }
    }

    double secs = (double)(now_ns() - t0) / 1e9;
    printf("n64: %llu frames in %.1f s (%.2f fps), %llu skipped, %llu late resets\n", (unsigned long long)frames, secs,
           secs > 0 ? (double)frames / secs : 0.0, (unsigned long long)skipped, (unsigned long long)late_resets);
    saves_flush("exit");
    if (o->screenshot) {
        if (write_ppm(o->screenshot, xrgb, dw, dh) == 0)
            printf("n64: last frame (%d x %d) written to %s\n", dw, dh, o->screenshot);
        else
            rc = -1;
    }
    fflush(stdout);
    if (rumble_sent[0] || rumble_sent[1] || rumble_sent[2] || rumble_sent[3])
        for (int p = 0; p < HOST_PAD_PORTS; p++)
            host_pad_rumble(p, 0, 0, 0);
    recomp_exit_hook_set(NULL);
    if (audio)
        host_audio_close();
    if (window)
        host_window_close();
    host_window_set_aspect(0, 0);
    pthread_mutex_lock(&s_mu);
    s_m = NULL;
    s_save_dir[0] = 0;
    saves_clear();
    if (s_discovered[0])
        n64_pack_save_missed(m, s_discovered);
    s_discovered[0] = 0;
    pthread_mutex_unlock(&s_mu);
    n64_destroy(m);
    n64_pack_close(&pack);
    return rc;
}

/* ---- The app shell's entry ------------------------------------------------------------------------------------ */

static char s_start_save_dir[1024];
static char s_start_work_dir[1024];

void n64_runtime_set_work_dir(const char *dir)
{
    snprintf(s_start_work_dir, sizeof s_start_work_dir, "%s", dir ? dir : "");
}

void n64_runtime_set_save_dir(const char *dir)
{
    snprintf(s_start_save_dir, sizeof s_start_save_dir, "%s", dir ? dir : "");
}

int n64_runtime_start(const char *rom, const char *pack_lib, const char *title_key)
{
    n64_runtime_opts o;
    n64_runtime_opts_init(&o);
    o.rom = rom;
    o.title_key = title_key;
    o.save_dir = s_start_save_dir[0] ? s_start_save_dir : NULL;
    o.pack_lib = pack_lib;
    char discovered[1200];
    if (s_start_work_dir[0] && title_key && *title_key) {
        snprintf(discovered, sizeof discovered, "%s/n64-%s/discovered.txt", s_start_work_dir, title_key);
        o.discovered_path = discovered;
    }
    const char *frames = getenv("RECOMP_N64_FRAMES"); /* a bounded run (checks) */
    if (frames && *frames)
        o.frames = strtoull(frames, NULL, 10);
    printf("n64 runtime ready (%s; title %s, pack %s, game %s)\n", n64_runtime_version(), title_key ? title_key : "?",
           pack_lib && *pack_lib ? pack_lib : "none (interpreter)", rom ? rom : "?");
    fflush(stdout);
    return n64_runtime_run(&o);
}
