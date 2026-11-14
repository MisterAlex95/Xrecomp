/* gba_runtime.c -- see gba_runtime.h (XBA-146, XBA-157).
 *
 * One thread does everything, frame by frame: pump the window's events (quit, background, pads), read the pads,
 * run one GBA frame, queue its audio, present it, then sleep to the next frame's deadline. Pacing is a clock at the
 * GBA's own 59.7275 Hz with absolute deadlines (no drift), nudged by the audio queue once the core produces sound
 * (so the audio device's clock wins in the long run and the queue neither starves nor grows); vsync is on, so a swap
 * may wait for the display too, and a frame late by more than three periods resets the clock instead of running
 * fast to catch up. While the app is paused (background, the in-game menu: recomp_lifecycle) no frame runs, the save
 * is written and the window keeps pumping. */
#define _GNU_SOURCE /* pread, nanosleep */
#include "gba_runtime.h"

#include "gba_core.h"
#include "gba_pack.h"
#include "host_audio.h"
#include "console_pad.h"
#include "host_pad.h"
#include "host_window.h"
#include "recomp_exit_hook.h"
#include "recomp_lifecycle.h"

#include <SDL.h>

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

#define GBA_ROM_MAX (32u * 1024 * 1024)
#define BIOS_SIZE 16384
#define SAVE_DEBOUNCE_NS 1000000000ull
#define AUDIO_TARGET_FRAMES (GBA_AUDIO_RATE / 20) /* 50 ms queued */

const char *gba_runtime_version(void)
{
    return "gba_runtime " GBA_RUNTIME_VERSION;
}

/* ---- Pure helpers --------------------------------------------------------------------------------------------- */

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t crc = 0xffffffffu;
    while (n--)
        crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static char code_char(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? (char)c : '_';
}

int gba_runtime_key(const uint8_t *rom, size_t size, char *out, size_t out_n)
{
    char code[5] = "____";
    if (!out || out_n < 24)
        return -1;
    if (rom && size >= 0xC0 && rom[0xB2] == 0x96) {
        uint8_t sum = 0;
        for (int i = 0xA0; i <= 0xBC; i++)
            sum = (uint8_t)(sum - rom[i]);
        if ((uint8_t)(sum - 0x19) == rom[0xBD])
            for (int i = 0; i < 4; i++)
                code[i] = code_char(rom[0xAC + i]);
    }
    snprintf(out, out_n, "%s-%08X", code, (unsigned)crc32_of(rom ? rom : (const uint8_t *)"", rom ? size : 0));
    return 0;
}

/* The player's mapping (console_pad.h, RECOMP_GBA_PAD_MAP), loaded when a game starts; the defaults until then. */
static ConsolePadMap s_pad_map;

uint16_t gba_runtime_keys_of(uint32_t t)
{
    static const uint16_t keys[GBA_T_COUNT] = {
        [GBA_T_A] = GBA_KEY_A, [GBA_T_B] = GBA_KEY_B, [GBA_T_L] = GBA_KEY_L, [GBA_T_R] = GBA_KEY_R,
        [GBA_T_START] = GBA_KEY_START, [GBA_T_SELECT] = GBA_KEY_SELECT, [GBA_T_UP] = GBA_KEY_UP,
        [GBA_T_DOWN] = GBA_KEY_DOWN, [GBA_T_LEFT] = GBA_KEY_LEFT, [GBA_T_RIGHT] = GBA_KEY_RIGHT,
    };
    uint16_t k = 0;
    for (int i = 0; i < GBA_T_COUNT; i++)
        if (t & (1u << i))
            k |= keys[i];
    /* Opposite directions at once (a worn stick, two pads): the GBA's own pad cannot, some games misbehave. */
    if ((k & (GBA_KEY_UP | GBA_KEY_DOWN)) == (GBA_KEY_UP | GBA_KEY_DOWN))
        k &= (uint16_t)~(GBA_KEY_UP | GBA_KEY_DOWN);
    if ((k & (GBA_KEY_LEFT | GBA_KEY_RIGHT)) == (GBA_KEY_LEFT | GBA_KEY_RIGHT))
        k &= (uint16_t)~(GBA_KEY_LEFT | GBA_KEY_RIGHT);
    return k;
}

uint16_t gba_runtime_pad_keys(uint32_t b, int lx, int ly, int lt, int rt)
{
    ConsolePadMap m;
    ConsolePadOut o;
    HostPadState st = {.connected = 1, .buttons = b};
    st.axis[HOST_PAD_AXIS_LX] = (int16_t)lx;
    st.axis[HOST_PAD_AXIS_LY] = (int16_t)ly;
    st.axis[HOST_PAD_AXIS_LT] = (int16_t)lt;
    st.axis[HOST_PAD_AXIS_RT] = (int16_t)rt;
    console_pad_defaults(&m, CONSOLE_PAD_GBA);
    console_pad_apply(&m, &st, &o);
    return gba_runtime_keys_of(o.buttons);
}

void gba_runtime_rgb555_to_rgba(const uint16_t *src, uint8_t *dst, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned c = src[i], r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        dst[4 * i + 0] = (uint8_t)(r << 3 | r >> 2);
        dst[4 * i + 1] = (uint8_t)(g << 3 | g >> 2);
        dst[4 * i + 2] = (uint8_t)(b << 3 | b >> 2);
        dst[4 * i + 3] = 255;
    }
}

int gba_runtime_save_path(const char *save_dir, const char *key, char *out, size_t out_n)
{
    if (!save_dir || !*save_dir || !key || !*key || !out)
        return -1;
    int n = snprintf(out, out_n, "%s/gba-%s/game.sav", save_dir, key);
    return n < 0 || (size_t)n >= out_n ? -1 : 0;
}

/* ---- Files ---------------------------------------------------------------------------------------------------- */

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

static void mkdirs(const char *path)
{
    char tmp[1024];
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
    char tmp[1100], dir[1024];
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

static int write_ppm(const char *path, const uint16_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", GBA_SCREEN_W, GBA_SCREEN_H);
    uint8_t px[4];
    for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; i++) {
        gba_runtime_rgb555_to_rgba(&fb[i], px, 1);
        fwrite(px, 1, 3, f);
    }
    return fclose(f);
}

/* ---- Saves ---------------------------------------------------------------------------------------------------- */

/* The run's machine and its save, shared with the exit hook (the in-game menu's Quit ends the process from the UI
 * thread): s_mu is held while a frame runs and while the save is written. */
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static gba_machine *s_m;
static char s_save_path[1100];
static uint8_t *s_saved;   /* the image as last read or written */
static size_t s_saved_n;
static char s_discovered[1200];   /* gba_runtime_opts.discovered_path of the run */

/* Writes the save when it differs from what is on disk (or was there at start). Called with s_mu held. */
static void save_flush_locked(const char *why)
{
    size_t n = 0;
    const uint8_t *d;
    if (!s_m || !s_save_path[0] || gba_save_type(s_m) == GBA_SAVE_NONE || !(d = gba_save_data(s_m, &n)) || !n)
        return;
    if (s_saved && n == s_saved_n && memcmp(d, s_saved, n) == 0)
        return;
    if (write_file(s_save_path, d, n) != 0) {
        fprintf(stderr, "gba: cannot write the save %s: %s\n", s_save_path, strerror(errno));
        return;
    }
    uint8_t *copy = realloc(s_saved, n);
    if (copy) {
        memcpy(copy, d, n);
        s_saved = copy;
        s_saved_n = n;
    }
    printf("gba: save written (%zu bytes, %s) to %s\n", n, why, s_save_path);
    fflush(stdout);
}

static void save_flush(const char *why)
{
    pthread_mutex_lock(&s_mu);
    save_flush_locked(why);
    pthread_mutex_unlock(&s_mu);
}

static void on_exit_hook(const char *why)
{
    (void)why;
    save_flush("exit");
    /* The in-game menu's Quit ends the process here: the discovery loop's addresses too. */
    pthread_mutex_lock(&s_mu);
    if (s_m && s_discovered[0])
        gba_pack_save_missed(s_m, s_discovered);
    pthread_mutex_unlock(&s_mu);
}

/* The app paused (background, the in-game menu): the save goes to disk at once, from the thread that paused it. On
 * Android SDL parks the thread that pumps events while the app is in the background, so the run loop may not get to
 * it, and the process can be killed there. Registered once (the lifecycle keeps its listeners); no run, no-op. */
static void on_lifecycle(int paused, void *user)
{
    (void)user;
    if (paused)
        save_flush("pause");
}

static void save_load(gba_machine *m)
{
    size_t n = 0;
    uint8_t *d = s_save_path[0] ? read_all(s_save_path, 1u << 20, &n) : NULL;
    if (d) {
        if (gba_save_load(m, d, n) == 0)
            printf("gba: save loaded (%zu bytes) from %s\n", n, s_save_path);
        else
            fprintf(stderr, "gba: the save %s (%zu bytes) does not fit this game's save memory, not loaded\n",
                    s_save_path, n);
        free(d);
    } else if (s_save_path[0]) {
        printf("gba: no save yet (%s)\n", s_save_path);
    }
    /* What is on disk now (or the blank memory): written again only once it changes. */
    const uint8_t *cur = gba_save_data(m, &n);
    free(s_saved);
    s_saved = NULL;
    s_saved_n = 0;
    if (cur && n && (s_saved = malloc(n))) {
        memcpy(s_saved, cur, n);
        s_saved_n = n;
    }
}

/* ---- The run -------------------------------------------------------------------------------------------------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Relative sleeps to an absolute deadline (macOS has no clock_nanosleep); the deadline itself never drifts. */
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

/* Desktop keys: arrows, X / Z = A / B, A / S = L / R, Enter = Start, Backspace or right Shift = Select. */
static uint16_t keyboard_keys(void)
{
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint16_t k = 0;
    if (!ks)
        return 0;
    if (ks[SDL_SCANCODE_X]) k |= GBA_KEY_A;
    if (ks[SDL_SCANCODE_Z]) k |= GBA_KEY_B;
    if (ks[SDL_SCANCODE_A]) k |= GBA_KEY_L;
    if (ks[SDL_SCANCODE_S]) k |= GBA_KEY_R;
    if (ks[SDL_SCANCODE_RETURN]) k |= GBA_KEY_START;
    if (ks[SDL_SCANCODE_BACKSPACE] || ks[SDL_SCANCODE_RSHIFT]) k |= GBA_KEY_SELECT;
    if (ks[SDL_SCANCODE_UP]) k |= GBA_KEY_UP;
    if (ks[SDL_SCANCODE_DOWN]) k |= GBA_KEY_DOWN;
    if (ks[SDL_SCANCODE_LEFT]) k |= GBA_KEY_LEFT;
    if (ks[SDL_SCANCODE_RIGHT]) k |= GBA_KEY_RIGHT;
    return k;
}

/* Every connected pad drives the one GBA (a handheld's own controls and an external pad both work), through the
 * player's mapping; the on-screen pad joins pad 1 (console_pad_read). */
static uint16_t pads_keys(void)
{
    uint16_t k = 0;
    if (!s_pad_map.console)
        console_pad_defaults(&s_pad_map, CONSOLE_PAD_GBA);
    for (int p = 0; p < HOST_PAD_PORTS; p++) {
        ConsolePadOut o;
        if (console_pad_read(&s_pad_map, p, &o))
            k |= gba_runtime_keys_of(o.buttons);
    }
    return k;
}

void gba_runtime_opts_init(gba_runtime_opts *o)
{
    memset(o, 0, sizeof *o);
    o->scale = -1;
    o->filter = -1;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int gba_runtime_run(const gba_runtime_opts *o)
{
    size_t rom_n = 0, bios_n = 0;
    uint8_t *rom = o && o->rom ? read_all(o->rom, GBA_ROM_MAX, &rom_n) : NULL;
    uint8_t *bios = NULL;
    char key[32];

    if (!rom) {
        fprintf(stderr, "gba: cannot read the ROM %s (missing, empty or over 32 MB)\n", o && o->rom ? o->rom : "(none)");
        return -1;
    }
    if (o->title_key && *o->title_key)
        snprintf(key, sizeof key, "%s", o->title_key);
    else
        gba_runtime_key(rom, rom_n, key, sizeof key);
    const char *bios_path = o->bios ? o->bios : getenv("RECOMP_GBA_BIOS");
    if (bios_path && *bios_path) {
        bios = read_all(bios_path, BIOS_SIZE, &bios_n);
        if (!bios || bios_n != BIOS_SIZE) {
            fprintf(stderr, "gba: BIOS %s is not a 16 KB image, using the built-in BIOS\n", bios_path);
            free(bios);
            bios = NULL;
            bios_n = 0;
        }
    }
    char rom_sha[65] = "";
    if (o->pack_lib && *o->pack_lib) {
        uint8_t sum[32];
        gba_pack_sha256(rom, rom_n, sum);
        gba_pack_hex(sum, rom_sha);
    }
    gba_machine *m = gba_create(rom, rom_n, bios, bios_n);
    free(bios);
    free(rom);
    if (!m) {
        fprintf(stderr, "gba: the machine cannot start (ROM of %zu bytes)\n", rom_n);
        return -1;
    }
    printf("gba: %s, game %s (%zu KB), key %s, BIOS %s\n", gba_runtime_version(), base_name(o->rom), rom_n / 1024,
           key, bios_n ? bios_path : "built-in");
    gba_pack pack;
    gba_pack_load(m, o->pack_lib, key, rom_sha, &pack);   /* the interpreter whatever happens (logged) */

    pthread_mutex_lock(&s_mu);
    s_m = m;
    if (gba_runtime_save_path(o->save_dir, key, s_save_path, sizeof s_save_path) != 0)
        s_save_path[0] = 0;
    save_load(m);
    snprintf(s_discovered, sizeof s_discovered, "%s", o->discovered_path ? o->discovered_path : "");
    pthread_mutex_unlock(&s_mu);
    recomp_exit_hook_set(on_exit_hook);
    static int listening;
    if (!listening && recomp_lifecycle_listen(on_lifecycle, NULL) == 0)
        listening = 1;

    /* The window, or none (headless checks: SDL dummy drivers). */
    int window = !o->headless && !host_window_headless();
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0)
        SDL_InitSubSystem(SDL_INIT_EVENTS);
    int scale = o->scale >= 0 ? o->scale
                              : env_choice("RECOMP_GBA_SCALE", "integer", 0) ? HOST_SCALE_INTEGER : HOST_SCALE_ASPECT;
    int filter = o->filter >= 0 ? o->filter
                                : env_choice("RECOMP_GBA_FILTER", "linear", 0) ? HOST_FILTER_LINEAR
                                                                                 : HOST_FILTER_NEAREST;
    host_window_set_scaling(scale, filter);
    if (window) {
        char title[300];
        snprintf(title, sizeof title, "XRecomp GBA - %s", base_name(o->rom));
        if (!host_window_open(title, GBA_SCREEN_W * 4, GBA_SCREEN_H * 4, 1)) {
            fprintf(stderr, "gba: no window, running headless\n");
            window = 0;
        }
    }
    if (!window)
        host_pad_init();
    console_pad_load(&s_pad_map, CONSOLE_PAD_GBA, getenv(console_pad_env(CONSOLE_PAD_GBA)));
    int audio = host_audio_open(GBA_AUDIO_RATE, 2);
    printf("gba: window %s (%s, %s), audio %s, pads %u\n", window ? "on" : "off",
           scale == HOST_SCALE_INTEGER ? "integer" : "aspect", filter == HOST_FILTER_NEAREST ? "nearest" : "linear",
           audio ? "on" : "off", host_pad_mask());
    fflush(stdout);

    static uint16_t fb[GBA_SCREEN_W * GBA_SCREEN_H];
    static uint8_t rgba[GBA_SCREEN_W * GBA_SCREEN_H * 4];
    static int16_t pcm[GBA_AUDIO_RATE / 10 * 2];
    uint64_t frames = 0, next = now_ns(), dirty_at = 0, t0 = next, late_resets = 0;
    int was_paused = 0, heard = 0, rc = 0;

    for (;;) {
        int running = 1;
        if (window) {
            running = host_window_pump();
        } else {
            SDL_Event ev;
            host_pad_update();
            running = SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) <= 0;   /* SIGINT, a test */
            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        }
        if (!running) {
            printf("gba: quit asked after %llu frames\n", (unsigned long long)frames);
            break;
        }
        if (recomp_lifecycle_paused()) {
            if (!was_paused) {
                printf("gba: paused at frame %llu\n", (unsigned long long)frames);
                fflush(stdout);
                save_flush("pause");
                dirty_at = 0;
                was_paused = 1;
            }
            SDL_Delay(16);
            continue;
        }
        if (was_paused) {
            printf("gba: resumed\n");
            fflush(stdout);
            was_paused = 0;
            next = now_ns();   /* the time away is not caught up */
        }

        uint16_t keys = (uint16_t)(o->keys | pads_keys() | (window ? keyboard_keys() : 0));
        pthread_mutex_lock(&s_mu);
        gba_set_keys(m, keys);
        gba_run_frame(m, fb);
        int dirty = gba_save_take_dirty(m);
        pthread_mutex_unlock(&s_mu);
        frames++;

        /* Sound: everything the frame made (about 803 sample frames). */
        size_t got;
        while ((got = gba_audio_pull(m, pcm, sizeof pcm / sizeof pcm[0] / 2)) > 0) {
            heard = 1;
            if (audio)
                host_audio_queue(pcm, (uint32_t)got);
        }

        uint64_t t = now_ns();
        if (dirty && !dirty_at)
            dirty_at = t;
        if (dirty_at && t - dirty_at >= SAVE_DEBOUNCE_NS) {
            save_flush("written by the game");
            dirty_at = 0;
        }

        if (window) {
            if (o->capture_path && o->capture_frame == frames)
                host_window_capture(o->capture_path);
            gba_runtime_rgb555_to_rgba(fb, rgba, GBA_SCREEN_W * GBA_SCREEN_H);
            host_window_present_rgba(rgba, GBA_SCREEN_W, GBA_SCREEN_H, GBA_SCREEN_W * 4);
        }
        if (o->frames && frames >= o->frames)
            break;

        if (!o->unpaced) {
            next += GBA_FRAME_NS;
            /* Audio-driven: a queue above its target slows the clock a little, below it speeds it up (±2%). */
            if (audio && heard) {
                uint32_t q = host_audio_queued();
                if (q > AUDIO_TARGET_FRAMES * 2)
                    next += GBA_FRAME_NS / 50;
                else if (q < AUDIO_TARGET_FRAMES / 2)
                    next -= GBA_FRAME_NS / 50;
            }
            t = now_ns();
            if (t > next + 3 * GBA_FRAME_NS) {
                next = t;   /* a stall (a slow host, the debugger): no burst to catch up */
                late_resets++;
            } else if (next > t) {
                sleep_until(next);
            }
        }
    }

    double secs = (double)(now_ns() - t0) / 1e9;
    printf("gba: %llu frames in %.1f s (%.2f fps), %llu late resets\n", (unsigned long long)frames, secs,
           secs > 0 ? (double)frames / secs : 0.0, (unsigned long long)late_resets);
    save_flush("exit");
    if (o->screenshot) {
        if (write_ppm(o->screenshot, fb) == 0)
            printf("gba: last frame written to %s\n", o->screenshot);
        else
            rc = -1;
    }
    fflush(stdout);
    recomp_exit_hook_set(NULL);
    if (audio)
        host_audio_close();
    if (window)
        host_window_close();
    pthread_mutex_lock(&s_mu);
    s_m = NULL;
    s_save_path[0] = 0;
    free(s_saved);
    s_saved = NULL;
    s_saved_n = 0;
    pthread_mutex_unlock(&s_mu);
    if (s_discovered[0])
        gba_pack_save_missed(m, s_discovered);
    s_discovered[0] = 0;
    gba_destroy(m);
    gba_pack_close(&pack);
    return rc;
}

/* ---- The app shell's entry ------------------------------------------------------------------------------------ */

static char s_start_save_dir[1024];
static char s_start_work_dir[1024];

void gba_runtime_set_work_dir(const char *dir)
{
    snprintf(s_start_work_dir, sizeof s_start_work_dir, "%s", dir ? dir : "");
}

void gba_runtime_set_save_dir(const char *dir)
{
    snprintf(s_start_save_dir, sizeof s_start_save_dir, "%s", dir ? dir : "");
}

int gba_runtime_start(const char *rom, const char *pack_lib, const char *title_key)
{
    gba_runtime_opts o;
    gba_runtime_opts_init(&o);
    o.rom = rom;
    o.title_key = title_key;
    o.save_dir = s_start_save_dir[0] ? s_start_save_dir : NULL;
    o.pack_lib = pack_lib;
    char discovered[1200];
    if (s_start_work_dir[0] && title_key && *title_key) {
        snprintf(discovered, sizeof discovered, "%s/gba-%s/discovered.txt", s_start_work_dir, title_key);
        o.discovered_path = discovered;
    }
    const char *frames = getenv("RECOMP_GBA_FRAMES");   /* a bounded run (checks) */
    if (frames && *frames)
        o.frames = strtoull(frames, NULL, 10);
    printf("gba runtime ready (%s; title %s, pack %s, game %s)\n", gba_runtime_version(), title_key ? title_key : "?",
           pack_lib && *pack_lib ? pack_lib : "none (interpreter)", rom ? rom : "?");
    fflush(stdout);
    return gba_runtime_run(&o);
}
