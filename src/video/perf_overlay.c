/* Performance overlay (perf_overlay.h). Text in a built-in 3x5 pixel font,
 * scaled to the output, on a translucent box in the top-left corner; the
 * figures come from /proc and /sys where the host has them (Linux, Android)
 * and getrusage elsewhere, refreshed twice a second. */
#include "perf_overlay.h"
#include "host_perf_line.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

/* 3x5 glyphs, one octal digit per row (top first), bit 2 = left column. */
static unsigned glyph(int c)
{
    static const struct { char c; unsigned g; } t[] = {
        {'0', 075557}, {'1', 026227}, {'2', 071747}, {'3', 071717}, {'4', 055711},
        {'5', 074717}, {'6', 074757}, {'7', 071122}, {'8', 075757}, {'9', 075717},
        {'A', 025755}, {'B', 065656}, {'C', 034443}, {'D', 065556}, {'E', 074647},
        {'F', 074644}, {'G', 034553}, {'H', 055755}, {'I', 072227}, {'J', 011152},
        {'K', 055655}, {'L', 044447}, {'M', 057755}, {'N', 065555}, {'O', 025552},
        {'P', 065644}, {'Q', 025563}, {'R', 065655}, {'S', 034216}, {'T', 072222},
        {'U', 055557}, {'V', 055552}, {'W', 055775}, {'X', 055255}, {'Y', 055222},
        {'Z', 071247}, {'.', 000002}, {':', 002020}, {'/', 011244}, {'%', 051245},
        {'-', 000700}, {'+', 002720}, {'(', 012221}, {')', 042224}, {'=', 007070},
        {'|', 022222}, {',', 000024},
    };
    size_t i;
    c = toupper((unsigned char)c);
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].c == c)
            return t[i].g;
    return 0;
}

#define MAX_LINES 11
#define LINE_CHARS 48

static char s_lines[MAX_LINES][LINE_CHARS];
static int s_nlines;
static SDL_Texture *s_tex;
static int s_tex_w, s_tex_h, s_dirty_tex = 1;
/* The text drawn (perf_overlay_pixels): rebuilt when the text changes. */
static uint32_t *s_px;
static int s_px_w, s_px_h, s_dirty_px = 1;
static unsigned s_px_serial;

static uint64_t s_last_ms, s_last_flips;
static double s_last_cpu_s;
static double s_fps;
static int s_fg_state;
static uint32_t s_fg_real, s_fg_shown, s_fg_gen_us;

void perf_overlay_set_framegen(int state, uint32_t real_fps_centi, uint32_t shown_fps_centi,
                               uint32_t gen_us)
{
    s_fg_state = state;
    s_fg_real = real_fps_centi;
    s_fg_shown = shown_fps_centi;
    s_fg_gen_us = gen_us;
}

int perf_overlay_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_OVERLAY");
        v = e && *e && strcmp(e, "0") != 0;   /* off unless asked for */
    }
    return v;
}

static long read_long(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;
    if (f) {
        if (fscanf(f, "%ld", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

/* Resident set of this process, MB; -1 when unknown. */
static long rss_mb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = -1, res = -1;
    if (f) {
        if (fscanf(f, "%ld %ld", &pages, &res) != 2)
            res = -1;
        fclose(f);
    }
    if (res >= 0)
        return res * (sysconf(_SC_PAGESIZE) / 1024) / 1024;
    {
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) == 0)
#ifdef __APPLE__
            return (long)(ru.ru_maxrss / (1024 * 1024));   /* bytes, peak */
#else
            return (long)(ru.ru_maxrss / 1024);            /* KB, peak */
#endif
    }
    return -1;
}

/* System memory in use / total, MB (MemTotal - MemAvailable). */
static int sys_mem_mb(long *used, long *total)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char k[64];
    long v, tot = -1, avail = -1;
    if (!f)
        return -1;
    while (fscanf(f, "%63s %ld kB", k, &v) == 2) {
        if (!strcmp(k, "MemTotal:"))
            tot = v;
        else if (!strcmp(k, "MemAvailable:"))
            avail = v;
    }
    fclose(f);
    if (tot <= 0 || avail < 0)
        return -1;
    *used = (tot - avail) / 1024;
    *total = tot / 1024;
    return 0;
}

static double cpu_seconds(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return 0;
    return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6
         + (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

/* Hottest CPU thermal zone, degrees C; -1000 when none is readable (newer
 * Android versions hide most zones from apps). */
static double cpu_temp_c(void)
{
    static int zones[32], nz = -1;
    double best = -1000;
    int i;
    if (nz < 0) {
        nz = 0;
        for (i = 0; i < 128 && nz < 32; i++) {
            char p[96], type[64] = "";
            FILE *f;
            snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/type", i);
            f = fopen(p, "r");
            if (!f)
                continue;
            if (fscanf(f, "%63s", type) == 1 && (strstr(type, "cpu") || strstr(type, "CPU")))
                zones[nz++] = i;
            fclose(f);
        }
    }
    for (i = 0; i < nz; i++) {
        char p[96];
        long v;
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", zones[i]);
        v = read_long(p);
        if (v > 0) {
            double c = v > 1000 ? v / 1000.0 : (double)v;
            if (c > best)
                best = c;
        }
    }
    return best;
}

/* One line of the overlay, cut at LINE_CHARS. */
__attribute__((format(printf, 2, 3))) static void put_line(char *line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, LINE_CHARS, fmt, ap);
    va_end(ap);
}

int perf_overlay_tick(uint64_t now_ms, uint64_t flips, uint32_t draws, const char *status)
{
    char old[MAX_LINES][LINE_CHARS];
    int n = 0, i;
    long used, total, rss, bat, bat_t;
    double cpu = cpu_seconds(), temp;

    if (!perf_overlay_enabled())
        return 0;
    memcpy(old, s_lines, sizeof old);
    if (s_last_ms && now_ms > s_last_ms) {
        double dt = (double)(now_ms - s_last_ms) / 1000.0;
        s_fps = (double)(flips - s_last_flips) / dt;
        if (draws == PERF_OVERLAY_NO_DRAWS)
            snprintf(s_lines[n++], LINE_CHARS, "FPS %.1f  %.1f MS", s_fps, s_fps > 0.05 ? 1000.0 / s_fps : 0.0);
        else
            snprintf(s_lines[n++], LINE_CHARS, "FPS %.1f  %.1f MS  DRAWS %u", s_fps,
                     s_fps > 0.05 ? 1000.0 / s_fps : 0.0, (unsigned)draws);
        snprintf(s_lines[n++], LINE_CHARS, "CPU %.0f %%", (cpu - s_last_cpu_s) / dt * 100.0);
    } else {
        snprintf(s_lines[n++], LINE_CHARS, "FPS --");
    }
    s_last_ms = now_ms;
    s_last_flips = flips;
    s_last_cpu_s = cpu;

    /* The performance line's fields, the same on every console (host_perf_line.h): the game's rate against its full
     * one, the work of a frame and its emulation / drawing split, the worst frame and the slow ones, pictures shown;
     * over the last half second. */
    {
        host_perf_figures f;
        if (host_perf_live_get(now_ms * 1000000ull, &f)) {
            const char *unit = f.cfg.unit ? f.cfg.unit : "fps";
            if (f.full_hz > 0)
                put_line(s_lines[n++], "SPEED %.0f %%  %.1f/%.1f %s", f.speed_pct, f.guest_fps,
                         f.full_hz, unit);
            else
                put_line(s_lines[n++], "GAME %.1f %s", f.guest_fps, unit);
            put_line(s_lines[n++], "FRAME %.1f  EMU %.1f  DRAW %.1f MS", f.busy_ms, f.emu_ms,
                     f.render_ms);
            put_line(s_lines[n++], "WORST %.0f MS  SLOW %llu/%llu  SHOWN %.0f", f.worst_ms,
                     (unsigned long long)f.over_1, (unsigned long long)f.host_frames, f.shown_fps);
        }
    }

    if (s_fg_state == 1)
        snprintf(s_lines[n++], LINE_CHARS, "LSFG %u.%u / %u.%u FPS  GEN %u.%u MS",
                 s_fg_real / 100, s_fg_real % 100 / 10, s_fg_shown / 100, s_fg_shown % 100 / 10,
                 s_fg_gen_us / 1000, s_fg_gen_us % 1000 / 100);
    else if (s_fg_state == 2)
        snprintf(s_lines[n++], LINE_CHARS, "LSFG OFF");
    else if (s_fg_state == 3)
        snprintf(s_lines[n++], LINE_CHARS, "LSFG WAITING");

    rss = rss_mb();
    if (sys_mem_mb(&used, &total) == 0)
        snprintf(s_lines[n++], LINE_CHARS, "RAM %ld MB  SYS %.1f/%.1f GB", rss,
                 used / 1024.0, total / 1024.0);
    else if (rss >= 0)
        snprintf(s_lines[n++], LINE_CHARS, "RAM %ld MB", rss);

    temp = cpu_temp_c();
    bat = read_long("/sys/class/power_supply/battery/capacity");
    bat_t = read_long("/sys/class/power_supply/battery/temp");
    if (bat >= 0 && temp > -999)
        snprintf(s_lines[n++], LINE_CHARS, "BAT %ld %%  %.1f C  CPU %.0f C", bat,
                 bat_t > 0 ? bat_t / 10.0 : 0.0, temp);
    else if (bat >= 0)
        snprintf(s_lines[n++], LINE_CHARS, "BAT %ld %%  %.1f C", bat, bat_t > 0 ? bat_t / 10.0 : 0.0);
    else if (temp > -999)
        snprintf(s_lines[n++], LINE_CHARS, "CPU %.0f C", temp);

    if (status && *status)
        snprintf(s_lines[n++], LINE_CHARS, "%s", status);
    for (i = n; i < MAX_LINES; i++)
        s_lines[i][0] = 0;
    s_nlines = n;
    if (memcmp(old, s_lines, sizeof old) != 0) {
        s_dirty_tex = 1;
        s_dirty_px = 1;
        return 1;
    }
    return 0;
}

void perf_overlay_text(char *out, size_t n)
{
    size_t o = 0;
    int i;
    if (!n)
        return;
    out[0] = 0;
    for (i = 0; i < s_nlines && o < n; i++) {
        int k = snprintf(out + o, n - o, "%s%s", i ? "\n" : "", s_lines[i]);
        if (k < 0)
            break;
        o += (size_t)k;
    }
}

void perf_overlay_reset(void)
{
    if (s_tex)
        SDL_DestroyTexture(s_tex);
    s_tex = NULL;
    s_dirty_tex = 1;
}

/* The text at 1 pixel per font pixel, white on a translucent black box
 * (0xAARRGGBB words), kept in s_px until the text changes. */
static int build_pixels(void)
{
    int cols = 0, i, w, h, pad = 2;
    uint32_t *px;

    if (!s_dirty_px && s_px)
        return 1;
    for (i = 0; i < s_nlines; i++) {
        int l = (int)strlen(s_lines[i]);
        if (l > cols)
            cols = l;
    }
    w = cols * 4 - 1 + 2 * pad;
    h = s_nlines * 7 - 2 + 2 * pad;
    if (w <= 2 * pad || h <= 2 * pad)
        return 0;
    px = (uint32_t *)realloc(s_px, (size_t)w * (size_t)h * sizeof *px);
    if (!px)
        return 0;
    s_px = px;
    s_px_w = w;
    s_px_h = h;
    s_dirty_px = 0;
    s_px_serial++;
    for (i = 0; i < w * h; i++)
        px[i] = 0xA0000000u;
    for (i = 0; i < s_nlines; i++) {
        const char *s = s_lines[i];
        int c, x0 = pad, y0 = pad + i * 7;
        uint32_t color = i == 0 ? 0xFF7CFC9Au : 0xFFFFFFFFu;   /* FPS line in green */
        for (c = 0; s[c]; c++, x0 += 4) {
            unsigned g = glyph(s[c]);
            int r, b;
            for (r = 0; r < 5; r++)
                for (b = 0; b < 3; b++)
                    if (g >> ((4 - r) * 3 + (2 - b)) & 1)
                        px[(y0 + r) * w + x0 + b] = color;
        }
    }
    return 1;
}

const uint32_t *perf_overlay_pixels(int *w, int *h, unsigned *serial)
{
    if (!perf_overlay_enabled() || !s_nlines || !build_pixels())
        return NULL;
    *w = s_px_w;
    *h = s_px_h;
    *serial = s_px_serial;
    return s_px;
}

/* The pixels into an SDL texture (the Xbox window). */
static void build_texture(SDL_Renderer *ren)
{
    if (!build_pixels())
        return;
    if (!s_tex || s_px_w != s_tex_w || s_px_h != s_tex_h) {
        if (s_tex)
            SDL_DestroyTexture(s_tex);
        s_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, s_px_w, s_px_h);
        if (!s_tex)
            return;
        SDL_SetTextureBlendMode(s_tex, SDL_BLENDMODE_BLEND);
        s_tex_w = s_px_w;
        s_tex_h = s_px_h;
    }
    SDL_UpdateTexture(s_tex, NULL, s_px, s_px_w * 4);
    s_dirty_tex = 0;
}

void perf_overlay_draw(SDL_Renderer *ren)
{
    int ow = 0, oh = 0, lw = 0, lh = 0;
    float k;
    SDL_FRect dst;

    if (!perf_overlay_enabled() || !s_nlines)
        return;
    if (s_dirty_tex || !s_tex)
        build_texture(ren);
    if (!s_tex)
        return;
    /* About oh/320 output pixels per font pixel (glyphs ~17 px tall at
     * 1080p), in the renderer's own units: the window draws in the game's
     * 640x480 logical size, scaled to the screen. */
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    SDL_RenderGetLogicalSize(ren, &lw, &lh);
    k = lh > 0 ? (float)lh / 320.0f : (float)oh / 320.0f;
    if (k < 1.0f)
        k = 1.0f;
    dst.x = k;
    dst.y = k;
    dst.w = (float)s_tex_w * k;
    dst.h = (float)s_tex_h * k;
    SDL_RenderCopyF(ren, s_tex, NULL, &dst);
}
