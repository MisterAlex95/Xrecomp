/* ps1_runtime.c -- see ps1_runtime.h (XBA-146, XBA-168).
 *
 * The GBA runtime's shape (consoles/gba/runtime/gba_runtime.c): one thread does everything, frame by frame: pump the
 * window's events (quit, background, pads), read the pads, run one PS1 frame (to the next VBlank), queue its audio,
 * present the GPU's display, then sleep to the next frame's deadline. The frame's length comes from the CPU cycles
 * the core ran (its scanline scheduler follows the GPU's video mode: NTSC or PAL, interlaced or not), so pacing is the
 * PS1's own field rate; absolute deadlines (no drift), nudged by the audio queue once the SPU makes sound, a stall of
 * more than three frames resets the clock. While the app is paused (background, the in-game menu: recomp_lifecycle) no
 * frame runs, the memory cards are written and the window keeps pumping.
 *
 * Picture: ps1_display gives the display area (256 / 320 / 368 / 512 / 640 x 240 / 480, 15- or 24-bit) as XRGB; it is
 * shown at 4:3 whatever its width (host_window_set_aspect), as a television did. */
#define _GNU_SOURCE /* pread, nanosleep, readlink */
#include "ps1_runtime.h"

#include "host_audio.h"
#include "host_pad.h"
#include "host_window.h"
#include "ps1_core.h"
#include "ps1_devices.h"
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

#define PS1_EXE_MAX (PS1_RAM_SIZE + 0x800u) /* header + at most the whole RAM */
#define SAVE_DEBOUNCE_NS 1000000000ull      /* a second after the last write to a card */
#define SAVE_LATEST_NS 5000000000ull        /* and at most five after the first */
#define AUDIO_TARGET_FRAMES (PS1_AUDIO_RATE / 20) /* 50 ms queued */
#define DISPLAY_MAX_W 640
#define DISPLAY_MAX_H 512

const char *ps1_runtime_version(void)
{
    return "ps1_runtime " PS1_RUNTIME_VERSION;
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

uint16_t ps1_runtime_pad_buttons(uint32_t b, int lx, int ly, int lt, int rt, int analog)
{
    const int dead = 32768 / 3, trig = 16384;
    uint16_t k = 0;
    if (b & HOST_PAD_A) k |= PS1_PAD_CROSS;
    if (b & HOST_PAD_B) k |= PS1_PAD_CIRCLE;
    if (b & HOST_PAD_X) k |= PS1_PAD_SQUARE;
    if (b & HOST_PAD_Y) k |= PS1_PAD_TRIANGLE;
    if (b & HOST_PAD_LSHOULDER) k |= PS1_PAD_L1;
    if (b & HOST_PAD_RSHOULDER) k |= PS1_PAD_R1;
    if (lt > trig) k |= PS1_PAD_L2;
    if (rt > trig) k |= PS1_PAD_R2;
    if (b & HOST_PAD_START) k |= PS1_PAD_START;
    if (b & HOST_PAD_BACK) k |= PS1_PAD_SELECT;
    if (b & HOST_PAD_LSTICK) k |= PS1_PAD_L3;
    if (b & HOST_PAD_RSTICK) k |= PS1_PAD_R3;
    if (b & HOST_PAD_DPAD_UP) k |= PS1_PAD_UP;
    if (b & HOST_PAD_DPAD_DOWN) k |= PS1_PAD_DOWN;
    if (b & HOST_PAD_DPAD_LEFT) k |= PS1_PAD_LEFT;
    if (b & HOST_PAD_DPAD_RIGHT) k |= PS1_PAD_RIGHT;
    if (!analog) {
        if (ly < -dead) k |= PS1_PAD_UP;
        if (ly > dead) k |= PS1_PAD_DOWN;
        if (lx < -dead) k |= PS1_PAD_LEFT;
        if (lx > dead) k |= PS1_PAD_RIGHT;
    }
    /* Opposite directions at once (a worn stick, two pads): a real D-pad cannot, some games misbehave. */
    if ((k & (PS1_PAD_UP | PS1_PAD_DOWN)) == (PS1_PAD_UP | PS1_PAD_DOWN))
        k &= (uint16_t)~(PS1_PAD_UP | PS1_PAD_DOWN);
    if ((k & (PS1_PAD_LEFT | PS1_PAD_RIGHT)) == (PS1_PAD_LEFT | PS1_PAD_RIGHT))
        k &= (uint16_t)~(PS1_PAD_LEFT | PS1_PAD_RIGHT);
    return k;
}

uint8_t ps1_runtime_axis(int v)
{
    if (v < -32768)
        v = -32768;
    if (v > 32767)
        v = 32767;
    return (uint8_t)((v + 32768) >> 8);
}

void ps1_runtime_xrgb_to_rgba(const uint32_t *src, uint8_t *dst, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t c = src[i];
        dst[4 * i + 0] = (uint8_t)(c >> 16);
        dst[4 * i + 1] = (uint8_t)(c >> 8);
        dst[4 * i + 2] = (uint8_t)c;
        dst[4 * i + 3] = 255;
    }
}

int ps1_runtime_card_path(const char *save_dir, const char *key, int slot, char *out, size_t out_n)
{
    if (!save_dir || !*save_dir || !key || !*key || !out || slot < 0 || slot > 1)
        return -1;
    int n = snprintf(out, out_n, "%s/ps1-%s/card%d.mcr", save_dir, key, slot + 1);
    return n < 0 || (size_t)n >= out_n ? -1 : 0;
}

int ps1_runtime_is_exe(const uint8_t *head, size_t n)
{
    return head && n >= 8 && memcmp(head, "PS-X EXE", 8) == 0;
}

int ps1_runtime_fallback_key(const uint8_t *exe, size_t exe_n, const char *disc, char *out, size_t out_n)
{
    if (!out || out_n < 24)
        return -1;
    if (exe && exe_n) {
        snprintf(out, out_n, "EXE-%08X", (unsigned)crc32_of(exe, exe_n));
        return 0;
    }
    snprintf(out, out_n, "PSX-UNKNOWN");
    char err[128];
    ps1_disc *d = disc && *disc ? ps1_disc_open(disc, err, sizeof err) : NULL;
    if (d) {
        uint8_t sec[2352];
        if (ps1_disc_read_sector(d, 16, sec) == 0) {
            /* Mode 1: user data at 16; Mode 2 Form 1: after the 8-byte subheader, at 24. */
            const uint8_t *user = sec[15] == 1 ? sec + 16 : sec + 24;
            snprintf(out, out_n, "PSX-%08X", (unsigned)crc32_of(user, 2048));
        }
        ps1_disc_close(d);
    }
    return 0;
}

uint64_t ps1_runtime_frame_ns(uint64_t cycles)
{
    uint64_t ns = cycles * 1000000000ull / PS1_CPU_HZ;
    const uint64_t lo = 1000000000ull / 75, hi = 1000000000ull / 45;
    return ns < lo ? lo : ns > hi ? hi : ns;
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

/* A whole file, or "fd:N" read from offset 0 (the descriptor stays open and its offset is not used). */
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

/* The first bytes of a file or "fd:N" (from offset 0). Returns how many were read, or -1. */
static ssize_t read_head(const char *spec, uint8_t *out, size_t n)
{
    int own, fd = spec_fd(spec, &own);
    if (fd < 0)
        return -1;
    ssize_t r;
    do
        r = pread(fd, out, n, 0);
    while (r < 0 && errno == EINTR);
    if (own)
        close(fd);
    return r;
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

/* Writes path through path.tmp + rename, so a kill mid-write leaves the old card. */
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

/* ---- BIOS ----------------------------------------------------------------------------------------------------- */

static char s_start_bios[1024];

static uint8_t *bios_try(const char *path, size_t *n)
{
    uint8_t *b = path && *path ? read_all(path, PS1_BIOS_SIZE, n) : NULL;
    if (b && *n != PS1_BIOS_SIZE) {
        free(b);
        b = NULL;
    }
    return b;
}

/* The BIOS image (ps1_runtime_opts.bios); where it came from in `from`. */
static uint8_t *bios_load(const char *explicit_path, char *from, size_t from_n)
{
    size_t n = 0;
    uint8_t *b;
    const char *env = getenv("RECOMP_PS1_BIOS");
    const char *first[] = {explicit_path, env && *env ? env : NULL, s_start_bios[0] ? s_start_bios : NULL};
    for (int i = 0; i < 3; i++) {
        if (!first[i])
            continue;
        if ((b = bios_try(first[i], &n))) {
            snprintf(from, from_n, "%s", first[i]);
            return b;
        }
        fprintf(stderr, "ps1: BIOS %s is not a 512 KB image\n", first[i]);
        if (i == 0)
            return NULL; /* asked for that one */
    }
    /* The app's asset (SDL reads the APK's assets for a relative path on Android; on a desktop, relative to the
     * working directory). */
    SDL_RWops *rw = SDL_RWFromFile("ps1-bios/openbios.bin", "rb");
    if (rw) {
        Sint64 size = SDL_RWsize(rw);
        b = size == PS1_BIOS_SIZE ? malloc(PS1_BIOS_SIZE) : NULL;
        if (b && SDL_RWread(rw, b, 1, PS1_BIOS_SIZE) != PS1_BIOS_SIZE) {
            free(b);
            b = NULL;
        }
        SDL_RWclose(rw);
        if (b) {
            snprintf(from, from_n, "asset ps1-bios/openbios.bin");
            return b;
        }
    }
    /* A desktop build tree: build/ps1-bios/openbios.bin (tools/consoles/build_ps1_bios.sh) in the working directory,
     * or beside an ancestor of the executable (build/ps1/ps1_runtime/ps1_play -> build/ps1-bios). */
    char path[1200], dir[1024];
    if ((b = bios_try("build/ps1-bios/openbios.bin", &n))) {
        snprintf(from, from_n, "build/ps1-bios/openbios.bin");
        return b;
    }
    ssize_t len = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if (len > 0) {
        dir[len] = 0;
        char *slash;
        while ((slash = strrchr(dir, '/')) && slash != dir) {
            *slash = 0;
            const char *rel[] = {"ps1-bios/openbios.bin", "build/ps1-bios/openbios.bin"};
            for (int i = 0; i < 2; i++) {
                snprintf(path, sizeof path, "%s/%s", dir, rel[i]);
                if ((b = bios_try(path, &n))) {
                    snprintf(from, from_n, "%s", path);
                    return b;
                }
            }
        }
    }
    return NULL;
}

void ps1_runtime_set_bios(const char *path)
{
    snprintf(s_start_bios, sizeof s_start_bios, "%s", path ? path : "");
}

/* ---- Memory cards --------------------------------------------------------------------------------------------- */

/* The run's machine and its cards, shared with the exit hook (the in-game menu's Quit ends the process from the UI
 * thread) and the lifecycle listener: s_mu is held while a frame runs and while a card is written. */
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static ps1_machine *s_m;
static char s_card_path[2][1100];
static uint8_t s_saved[2][PS1_CARD_SIZE]; /* each card as last read or written */

/* Writes each card that differs from what is on disk (or was there at start). Called with s_mu held. */
static void cards_flush_locked(const char *why)
{
    for (int slot = 0; slot < 2; slot++) {
        size_t n = 0;
        const uint8_t *d;
        if (!s_m || !s_card_path[slot][0] || !(d = ps1_card_data(s_m, slot, &n)) || n != PS1_CARD_SIZE)
            continue;
        if (memcmp(d, s_saved[slot], n) == 0)
            continue;
        if (write_file(s_card_path[slot], d, n) != 0) {
            fprintf(stderr, "ps1: cannot write the memory card %s: %s\n", s_card_path[slot], strerror(errno));
            continue;
        }
        memcpy(s_saved[slot], d, n);
        printf("ps1: memory card %d written (%s) to %s\n", slot + 1, why, s_card_path[slot]);
    }
    fflush(stdout);
}

static void cards_flush(const char *why)
{
    pthread_mutex_lock(&s_mu);
    cards_flush_locked(why);
    pthread_mutex_unlock(&s_mu);
}

static void on_exit_hook(const char *why)
{
    (void)why;
    cards_flush("exit");
}

/* The app paused (background, the in-game menu): the cards go to disk at once, from the thread that paused it (on
 * Android SDL parks the thread that pumps events in the background, and the process can be killed there). Registered
 * once; no run, no-op. */
static void on_lifecycle(int paused, void *user)
{
    (void)user;
    if (paused)
        cards_flush("pause");
}

static void cards_load(ps1_machine *m)
{
    for (int slot = 0; slot < 2; slot++) {
        size_t n = 0;
        uint8_t *d = s_card_path[slot][0] ? read_all(s_card_path[slot], PS1_CARD_SIZE, &n) : NULL;
        if (d && n == PS1_CARD_SIZE && ps1_card_load(m, slot, d, n) == 0)
            printf("ps1: memory card %d loaded from %s\n", slot + 1, s_card_path[slot]);
        else if (d || (s_card_path[slot][0] && access(s_card_path[slot], F_OK) == 0))
            fprintf(stderr, "ps1: %s is not a 128 KB memory card image, a blank card is in slot %d (the file is "
                            "kept until the game writes the card)\n", s_card_path[slot], slot + 1);
        else if (s_card_path[slot][0] && slot == 0)
            printf("ps1: no memory card yet (%s), a blank one is in slot 1\n", s_card_path[slot]);
        free(d);
        const uint8_t *cur = ps1_card_data(m, slot, &n);
        if (cur && n == PS1_CARD_SIZE)
            memcpy(s_saved[slot], cur, n); /* what is on disk now (or the blank card): written once it changes */
        ps1_card_take_dirty(m, slot);
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

/* Desktop keys: arrows, X = Cross, C = Circle, Z = Square, S = Triangle, Q / E = L1 / R1, 1 / 3 = L2 / R2,
 * Enter = Start, Backspace or right Shift = Select. */
static uint16_t keyboard_buttons(void)
{
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint16_t k = 0;
    if (!ks)
        return 0;
    if (ks[SDL_SCANCODE_X]) k |= PS1_PAD_CROSS;
    if (ks[SDL_SCANCODE_C]) k |= PS1_PAD_CIRCLE;
    if (ks[SDL_SCANCODE_Z]) k |= PS1_PAD_SQUARE;
    if (ks[SDL_SCANCODE_S]) k |= PS1_PAD_TRIANGLE;
    if (ks[SDL_SCANCODE_Q]) k |= PS1_PAD_L1;
    if (ks[SDL_SCANCODE_E]) k |= PS1_PAD_R1;
    if (ks[SDL_SCANCODE_1]) k |= PS1_PAD_L2;
    if (ks[SDL_SCANCODE_3]) k |= PS1_PAD_R2;
    if (ks[SDL_SCANCODE_RETURN]) k |= PS1_PAD_START;
    if (ks[SDL_SCANCODE_BACKSPACE] || ks[SDL_SCANCODE_RSHIFT]) k |= PS1_PAD_SELECT;
    if (ks[SDL_SCANCODE_UP]) k |= PS1_PAD_UP;
    if (ks[SDL_SCANCODE_DOWN]) k |= PS1_PAD_DOWN;
    if (ks[SDL_SCANCODE_LEFT]) k |= PS1_PAD_LEFT;
    if (ks[SDL_SCANCODE_RIGHT]) k |= PS1_PAD_RIGHT;
    return k;
}

typedef struct pad_in {
    uint16_t buttons;
    uint8_t lx, ly, rx, ry;
    int toggle; /* Back + L3 + R3 held */
} pad_in;

static int deflection(uint8_t v)
{
    return v > 0x80 ? v - 0x80 : 0x80 - v;
}

/* The host pads for PS1 port `port`: with one player every pad drives port 0 (buttons merged, the most deflected
 * stick wins); with two, host pad N drives port N. */
static void pads_read(int port, int players, int analog, pad_in *out)
{
    memset(out, 0, sizeof *out);
    out->lx = out->ly = out->rx = out->ry = 0x80;
    for (int p = 0; p < HOST_PAD_PORTS; p++) {
        if (players >= 2 ? p != port : port != 0)
            continue;
        HostPadState st;
        if (!host_pad_read(p, &st))
            continue;
        const uint32_t chord = HOST_PAD_BACK | HOST_PAD_LSTICK | HOST_PAD_RSTICK;
        if ((st.buttons & chord) == chord)
            out->toggle = 1;
        out->buttons |= ps1_runtime_pad_buttons(st.buttons, st.axis[HOST_PAD_AXIS_LX], st.axis[HOST_PAD_AXIS_LY],
                                                st.axis[HOST_PAD_AXIS_LT], st.axis[HOST_PAD_AXIS_RT], analog);
        uint8_t lx = ps1_runtime_axis(st.axis[HOST_PAD_AXIS_LX]), ly = ps1_runtime_axis(st.axis[HOST_PAD_AXIS_LY]);
        uint8_t rx = ps1_runtime_axis(st.axis[HOST_PAD_AXIS_RX]), ry = ps1_runtime_axis(st.axis[HOST_PAD_AXIS_RY]);
        if (deflection(lx) + deflection(ly) > deflection(out->lx) + deflection(out->ly)) {
            out->lx = lx;
            out->ly = ly;
        }
        if (deflection(rx) + deflection(ry) > deflection(out->rx) + deflection(out->ry)) {
            out->rx = rx;
            out->ry = ry;
        }
    }
}

void ps1_runtime_opts_init(ps1_runtime_opts *o)
{
    memset(o, 0, sizeof *o);
    o->scale = -1;
    o->filter = -1;
    o->analog = -1;
    o->players = -1;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static const char *pad_name(int analog)
{
    return analog ? "analog (DualShock)" : "digital";
}

int ps1_runtime_run(const ps1_runtime_opts *o)
{
    if (!o || !o->game || !*o->game) {
        fprintf(stderr, "ps1: no game\n");
        return -1;
    }
    /* An EXE (fast boot), or a disc the BIOS boots. */
    uint8_t head[8] = {0};
    ssize_t hn = read_head(o->game, head, sizeof head);
    if (hn <= 0) {
        fprintf(stderr, "ps1: cannot read the game %s (missing, empty or no permission)\n", o->game);
        return -1;
    }
    uint8_t *exe = NULL;
    size_t exe_n = 0;
    const char *disc = NULL, *exe_path = NULL;
    if (ps1_runtime_is_exe(head, (size_t)hn)) {
        exe_path = o->game;
    } else {
        disc = o->game;
        exe_path = o->exe && *o->exe ? o->exe : NULL;
    }
    if (exe_path && !(exe = read_all(exe_path, PS1_EXE_MAX, &exe_n))) {
        fprintf(stderr, "ps1: cannot read the EXE %s (missing, empty or over 2 MB)\n", exe_path);
        return -1;
    }
    char bios_from[1200] = "";
    uint8_t *bios = bios_load(o->bios, bios_from, sizeof bios_from);
    if (!bios) {
        fprintf(stderr, "ps1: no BIOS image (OpenBIOS: tools/consoles/build_ps1_bios.sh, then --bios or "
                        "RECOMP_PS1_BIOS)\n");
        free(exe);
        return -1;
    }
    char key[64];
    if (o->title_key && *o->title_key)
        snprintf(key, sizeof key, "%s", o->title_key);
    else
        ps1_runtime_fallback_key(exe_path == o->game ? exe : NULL, exe_path == o->game ? exe_n : 0, disc, key,
                                 sizeof key);
    char err[256] = "";
    ps1_machine *m = ps1_create(bios, PS1_BIOS_SIZE, disc, exe, exe_n, err, sizeof err);
    free(bios);
    free(exe);
    if (!m) {
        fprintf(stderr, "ps1: the machine cannot start: %s\n", err[0] ? err : "?");
        return -1;
    }
    printf("ps1: %s, game %s (%s), key %s, BIOS %s\n", ps1_runtime_version(), base_name(o->game),
           exe_path == o->game ? "EXE, fast boot" : exe_path ? "disc + EXE fast boot" : "disc, BIOS boot", key,
           bios_from);
    if (o->pack_lib && *o->pack_lib)
        printf("ps1: pack %s ignored (no PS1 recompiled code path yet), the interpreter runs the game\n", o->pack_lib);

    int analog = o->analog >= 0 ? o->analog != 0 : env_choice("RECOMP_PS1_PAD", "analog", 0);
    int players = o->players >= 1 ? o->players : 0;
    if (!players) {
        const char *v = getenv("RECOMP_PS1_PLAYERS");
        players = v && atoi(v) >= 2 ? 2 : 1;
    }
    players = players >= 2 ? 2 : 1;

    pthread_mutex_lock(&s_mu);
    s_m = m;
    for (int slot = 0; slot < 2; slot++)
        if (ps1_runtime_card_path(o->save_dir, key, slot, s_card_path[slot], sizeof s_card_path[slot]) != 0)
            s_card_path[slot][0] = 0;
    cards_load(m);
    pthread_mutex_unlock(&s_mu);
    recomp_exit_hook_set(on_exit_hook);
    static int listening;
    if (!listening && recomp_lifecycle_listen(on_lifecycle, NULL) == 0)
        listening = 1;

    int window = !o->headless && !host_window_headless();
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0)
        SDL_InitSubSystem(SDL_INIT_EVENTS);
    int scale = o->scale >= 0 ? o->scale
                              : env_choice("RECOMP_PS1_SCALE", "integer", 0) ? HOST_SCALE_INTEGER : HOST_SCALE_ASPECT;
    int filter = o->filter >= 0 ? o->filter
                                : env_choice("RECOMP_PS1_FILTER", "nearest", 0) ? HOST_FILTER_NEAREST
                                                                                 : HOST_FILTER_LINEAR;
    host_window_set_scaling(scale, filter);
    host_window_set_aspect(4, 3);
    if (window) {
        char title[300];
        snprintf(title, sizeof title, "XRecomp PS1 - %s", base_name(o->game));
        if (!host_window_open(title, 320 * 3, 240 * 3, 1)) {
            fprintf(stderr, "ps1: no window, running headless\n");
            window = 0;
        }
    }
    if (!window)
        host_pad_init();
    int audio = host_audio_open(PS1_AUDIO_RATE, 2);
    printf("ps1: window %s (4:3, %s, %s), audio %s, pads %u, port 1 %s%s\n", window ? "on" : "off",
           scale == HOST_SCALE_INTEGER ? "integer" : "aspect", filter == HOST_FILTER_NEAREST ? "nearest" : "linear",
           audio ? "on" : "off", host_pad_mask(), pad_name(analog), players >= 2 ? ", two players" : "");
    fflush(stdout);

    static uint32_t xrgb[DISPLAY_MAX_W * DISPLAY_MAX_H];
    static uint8_t rgba[DISPLAY_MAX_W * DISPLAY_MAX_H * 4];
    static int16_t pcm[PS1_AUDIO_RATE / 10 * 2];
    int dw = 320, dh = 240, last_w = 0, last_h = 0, last_pal = -1, last_int = -1, display_on = -1;
    uint64_t frames = 0, next = now_ns(), t0 = next, late_resets = 0, dirty_at = 0, dirty_first = 0;
    int was_paused = 0, heard = 0, rc = 0, toggle_held = 0;
    ps1_gpu *gpu = ps1_get_gpu(m);

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
            printf("ps1: quit asked after %llu frames\n", (unsigned long long)frames);
            break;
        }
        if (recomp_lifecycle_paused()) {
            if (!was_paused) {
                printf("ps1: paused at frame %llu\n", (unsigned long long)frames);
                fflush(stdout);
                cards_flush("pause");
                dirty_at = dirty_first = 0;
                was_paused = 1;
            }
            SDL_Delay(16);
            continue;
        }
        if (was_paused) {
            printf("ps1: resumed\n");
            fflush(stdout);
            was_paused = 0;
            next = now_ns(); /* the time away is not caught up */
        }

        pad_in p1, p2;
        pads_read(0, players, analog, &p1);
        pads_read(1, players, analog, &p2);
        if (p1.toggle && !toggle_held) {
            analog = !analog;
            printf("ps1: port 1 is now %s\n", pad_name(analog));
            fflush(stdout);
        }
        toggle_held = p1.toggle;
        if (p1.toggle)
            p1.buttons &= (uint16_t)~(PS1_PAD_SELECT | PS1_PAD_L3 | PS1_PAD_R3);
        p1.buttons |= o->buttons;
        if (window)
            p1.buttons |= keyboard_buttons();
        ps1_pad_kind kind = analog ? PS1_PAD_ANALOG : PS1_PAD_DIGITAL;
        pthread_mutex_lock(&s_mu);
        ps1_set_pad(m, 0, kind, p1.buttons, p1.lx, p1.ly, p1.rx, p1.ry);
        if (players >= 2)
            ps1_set_pad(m, 1, kind, p2.buttons, p2.lx, p2.ly, p2.rx, p2.ry);
        uint64_t c0 = ps1_cycles(m);
        ps1_run_frame(m);
        uint64_t frame_ns = ps1_runtime_frame_ns(ps1_cycles(m) - c0);
        int dirty = ps1_card_take_dirty(m, 0) | ps1_card_take_dirty(m, 1);
        int on = ps1_display(m, xrgb, DISPLAY_MAX_W, DISPLAY_MAX_H, &dw, &dh) == 0;
        pthread_mutex_unlock(&s_mu);
        frames++;

        if (gpu) {
            ps1_gpu_timing tm;
            ps1_gpu_get_timing(gpu, &tm);
            if (tm.pal != last_pal || tm.interlaced != last_int) {
                printf("ps1: video %s%s at frame %llu\n", tm.pal ? "PAL (50 Hz)" : "NTSC (59.94 Hz)",
                       tm.interlaced ? ", interlaced" : "", (unsigned long long)frames);
                last_pal = tm.pal;
                last_int = tm.interlaced;
            }
        }
        if (!on || dw <= 0 || dh <= 0 || dw > DISPLAY_MAX_W || dh > DISPLAY_MAX_H) {
            dw = last_w ? last_w : 320;
            dh = last_h ? last_h : 240;
            memset(xrgb, 0, (size_t)dw * (size_t)dh * sizeof xrgb[0]);
        } else if (dw != last_w || dh != last_h) {
            printf("ps1: display %d x %d at frame %llu\n", dw, dh, (unsigned long long)frames);
            last_w = dw;
            last_h = dh;
        }
        if (on != display_on) {
            if (display_on >= 0 || !on)
                printf("ps1: display %s at frame %llu\n", on ? "on" : "off", (unsigned long long)frames);
            display_on = on;
        }

        /* Sound: everything the frame made (about 735 sample frames). */
        size_t got;
        while ((got = ps1_audio_pull(m, pcm, sizeof pcm / sizeof pcm[0] / 2)) > 0) {
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
            cards_flush("written by the game");
            dirty_at = dirty_first = 0;
        }

        if (window) {
            if (o->capture_path && o->capture_frame == frames)
                host_window_capture(o->capture_path);
            ps1_runtime_xrgb_to_rgba(xrgb, rgba, (size_t)dw * (size_t)dh);
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
            if (t > next + 3 * frame_ns) {
                next = t; /* a stall (a slow host, the debugger): no burst to catch up */
                late_resets++;
            } else if (next > t) {
                sleep_until(next);
            }
        }
    }

    double secs = (double)(now_ns() - t0) / 1e9;
    printf("ps1: %llu frames in %.1f s (%.2f fps), %llu late resets\n", (unsigned long long)frames, secs,
           secs > 0 ? (double)frames / secs : 0.0, (unsigned long long)late_resets);
    cards_flush("exit");
    if (o->screenshot) {
        if (write_ppm(o->screenshot, xrgb, dw, dh) == 0)
            printf("ps1: last frame (%d x %d) written to %s\n", dw, dh, o->screenshot);
        else
            rc = -1;
    }
    fflush(stdout);
    recomp_exit_hook_set(NULL);
    if (audio)
        host_audio_close();
    if (window)
        host_window_close();
    host_window_set_aspect(0, 0);
    pthread_mutex_lock(&s_mu);
    s_m = NULL;
    s_card_path[0][0] = s_card_path[1][0] = 0;
    pthread_mutex_unlock(&s_mu);
    ps1_destroy(m);
    return rc;
}

/* ---- The app shell's entry ------------------------------------------------------------------------------------ */

static char s_start_save_dir[1024];

void ps1_runtime_set_save_dir(const char *dir)
{
    snprintf(s_start_save_dir, sizeof s_start_save_dir, "%s", dir ? dir : "");
}

int ps1_runtime_start(const char *rom, const char *pack_lib, const char *title_key)
{
    ps1_runtime_opts o;
    ps1_runtime_opts_init(&o);
    o.game = rom;
    o.title_key = title_key;
    o.save_dir = s_start_save_dir[0] ? s_start_save_dir : NULL;
    o.pack_lib = pack_lib;
    const char *frames = getenv("RECOMP_PS1_FRAMES"); /* a bounded run (checks) */
    if (frames && *frames)
        o.frames = strtoull(frames, NULL, 10);
    printf("ps1 runtime ready (%s; title %s, pack %s, game %s)\n", ps1_runtime_version(), title_key ? title_key : "?",
           pack_lib && *pack_lib ? pack_lib : "none (interpreter)", rom ? rom : "?");
    fflush(stdout);
    return ps1_runtime_run(&o);
}
