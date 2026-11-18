/* recomp_window_keys -- the desktop window's own keys, the same in every game window (XBA-278): F11 or Alt+Enter
 * toggles borderless fullscreen, F12 writes a screenshot. The console runtimes' window (src/video/host_window.c),
 * the Xbox SDL presenter (src/video/fb_present_sdl.c) and the Xbox Windows presenter (src/video/fb_present.c) handle
 * them before the game sees the key; none of them is a console button or a hotkey of recomp_hotkeys.h (F1-F4, Tab,
 * R, F6), so a game loses nothing. RECOMP_FULLSCREEN=1 starts in fullscreen.
 *
 * Screenshots: a 24-bit BMP of the picture the game showed last, at its own size and shown shape, in
 *   $RECOMP_SCREENSHOT_DIR, else $RECOMP_SAVE_DIR/screenshots (the game's save folder: the Mac launcher and xrecomp
 *   give one), else ./screenshots,
 * named <title>-<yyyymmdd>-<hhmmss>[-N].bmp (the title's letters and digits, else "screenshot").
 *
 * Header only (libc): the Windows presenter, which links no host layer, takes it too. */
#ifndef XBR_RECOMP_WINDOW_KEYS_H
#define XBR_RECOMP_WINDOW_KEYS_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#endif

enum { RECOMP_WK_NONE = 0, RECOMP_WK_FULLSCREEN = 1, RECOMP_WK_SCREENSHOT = 2 };

/* USB HID usages, the values of SDL_Scancode. */
#define RECOMP_WK_HID_RETURN 40
#define RECOMP_WK_HID_F11 68
#define RECOMP_WK_HID_F12 69
#define RECOMP_WK_HID_KP_ENTER 88

/* What a key press (not a repeat) does: usage = its HID usage (SDL scancode), alt = an Alt key is held. */
static inline int recomp_window_key(int usage, int alt)
{
    if (usage == RECOMP_WK_HID_F11 || (alt && (usage == RECOMP_WK_HID_RETURN || usage == RECOMP_WK_HID_KP_ENTER)))
        return RECOMP_WK_FULLSCREEN;
    if (usage == RECOMP_WK_HID_F12 && !alt)
        return RECOMP_WK_SCREENSHOT;
    return RECOMP_WK_NONE;
}

/* RECOMP_FULLSCREEN: "1" / "on" / "yes" / "true". */
static inline int recomp_window_fullscreen_env(void)
{
    const char *v = getenv("RECOMP_FULLSCREEN");
    return v && (!strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "yes") || !strcmp(v, "true"));
}

static inline int recomp_wk_mkdir(const char *path)
{
#ifdef _WIN32
    int rc = _mkdir(path);
#else
    int rc = mkdir(path, 0755);
#endif
    return rc == 0 || errno == EEXIST ? 0 : -1;
}

/* The folder screenshots go to (see above), made with its parents; 0, or -1 when it cannot be made. */
static inline int recomp_screenshot_dir(char *out, size_t n)
{
    const char *d = getenv("RECOMP_SCREENSHOT_DIR"), *save = getenv("RECOMP_SAVE_DIR");
    int w;
    if (d && *d)
        w = snprintf(out, n, "%s", d);
    else if (save && *save)
        w = snprintf(out, n, "%s/screenshots", save);
    else
        w = snprintf(out, n, "screenshots");
    if (w <= 0 || (size_t)w >= n)
        return -1;
    char *first = out + 1;
    if (out[0] && out[1] == ':') /* a Windows drive (C:\): it exists */
        first = out + 2 + (out[2] == '/' || out[2] == '\\');
    for (char *p = first; *p; p++) { /* the parents first */
        if (*p != '/' && *p != '\\')
            continue;
        char c = *p;
        *p = 0;
        int rc = recomp_wk_mkdir(out);
        *p = c;
        if (rc != 0)
            return -1;
    }
    return recomp_wk_mkdir(out);
}

/* A new screenshot's path for title (NULL: "screenshot") at time now: the folder made, a name not taken yet. 0, or
 * -1 when the folder cannot be made or the path does not fit. */
static inline int recomp_screenshot_path(char *out, size_t n, const char *title, time_t now)
{
    char dir[1024], name[48];
    size_t k = 0;
    if (recomp_screenshot_dir(dir, sizeof dir) != 0)
        return -1;
    for (const char *p = title ? title : ""; *p && k + 1 < sizeof name; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            name[k++] = (char)c;
        else if (k && name[k - 1] != '_')
            name[k++] = '_';
    }
    while (k && name[k - 1] == '_')
        k--;
    name[k] = 0;
    if (!k)
        snprintf(name, sizeof name, "screenshot");
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    for (int i = 1; i < 1000; i++) {
        int w = i == 1 ? snprintf(out, n, "%s/%s-%s.bmp", dir, name, stamp)
                       : snprintf(out, n, "%s/%s-%s-%d.bmp", dir, name, stamp, i);
        if (w <= 0 || (size_t)w >= n)
            return -1;
        struct stat st;
        if (stat(out, &st) != 0)
            return 0;
    }
    return -1;
}

#endif /* XBR_RECOMP_WINDOW_KEYS_H */
