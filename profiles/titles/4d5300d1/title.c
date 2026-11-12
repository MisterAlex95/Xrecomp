/*
 * Fable: The Lost Chapters (Xbox, 2005), title ID 4D5300D1 -- the title profile.
 *
 * The title layer: tools.prepare puts it on top of the XDK layers and calls
 * the two hooks below (title.json "hooks") after theirs. Everything here was
 * found bringing the title up on macOS arm64 (see NOTES.md in this folder).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void xbox_SetMapSize(size_t bytes);
extern void xbox_SetDelayOverride(uint32_t ret_va, uint32_t from_ms, uint32_t to_ms);

#ifndef _WIN32
#include <dirent.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ── Aspect ratio ─────────────────────────────────────────────────────────
 *
 * Fable can draw its 3D scenes for a 16:9 screen (overrides.c: the camera's
 * display-size override and the cutscene bars); the presenter then shows
 * those frames stretched to 16:9 and keeps menus and movies at 4:3. The
 * calls are src/video's (aspect_policy.c, declared in video_player.h; Win32
 * has stubs):
 *   xbox_AspectSetting()            RECOMP_ASPECT: 0 auto, 1 4:3, 2 16:9
 *   xbox_FramebufferWindowSetWide() this title's 3D frames are anamorphic 16:9 */
extern int xbox_AspectSetting(void);
extern void xbox_FramebufferWindowSetWide(int on);
extern int g_fable_aspect_wide;     /* overrides.c */

static void fable_aspect_configure(void)
{
    int setting = xbox_AspectSetting();
    int wide;

    /* Auto: wide on Android, where the screen is a 16:9 handheld (the AYN
     * Thor); 4:3 on a desktop, whose window keeps the console's shape. */
#ifdef __ANDROID__
    wide = setting != 1;
#else
    wide = setting == 2;
#endif
    if (!wide)
        return;
    g_fable_aspect_wide = 1;
    xbox_FramebufferWindowSetWide(1);
    fprintf(stderr, "[ASPECT] 16:9 scenes (setting %d)\n", setting);
}

void fable_configure(void)
{
    /* Fable reserves 608 MB of address space for its own allocator and
     * sub-allocates across all of it. With the mapping the size of RAM the
     * reserve was clamped to 38 MB, the allocator walked into the RAM mirrors
     * and two of its strings shared storage. RAM stays 64 MB. With the
     * mapping above 64 MB the desktop POSIX layout lost pages of the
     * contiguous window: title.json holds the whole 4 GB window
     * (RECOMP_GUEST_WINDOW=1, Android's default), which places every view
     * inside one reservation. title.json also asks for more than the default
     * 8 worker threads alive during the front-end load (RECOMP_THREAD_STACKS),
     * and on Android caps the GPU renderer at 30 fps, evenly paced: in
     * Oakvale a frame takes the executor 17-22 ms on an M1 and more on the
     * AYN Thor, so a 60 cap only lets the rate wander between 20 and 40
     * (docs/technical/gpu-backend.md, Android; the launcher's Frame cap wins).
     *
     * The XDK layers (title.json "layers") set the pushbuffer executor, the
     * AC'97 codec, the paced vblanks (the XMV decoder's clock is
     * D3DDevice_GetDisplayFieldStatus, sub_0084F650, which reads the vblank
     * count at device+0x1DE8), the one-CPU DISPATCH gate for XPP
     * (0x0086E480-0x00876C3C), the pad, and service D3D's GPU semaphore and
     * deferred RunPushBuffer (0x00855900; dispatcher sub_00859C40, helper
     * sub_00855860) with the addresses in title.json "params". */
    xbox_SetMapSize((size_t)1024 * 1024 * 1024);
    /* File-I/O completion APCs run at the next alertable wait, as the hardware
     * does: the runtime's default since XBA-102 (inline, the loader's
     * completion ran before the read call had returned and its bookkeeping
     * was set up). RECOMP_INLINE_APC=1 restores the inline delivery. */
    /* The front end (title screen, menus, loading screens) runs
     * do { draw; Sleep(50); } while (!done) in sub_0020BBB5, the two
     * `push 0x32; call Sleep` at 0x0020BD93 and 0x0020C3C5: 20 fps at most
     * by design, with the draw on top -- 7-8 fps on an AYN Thor, where the
     * draw is ~80 ms. The loading itself runs on other threads, which the
     * loop only waits for. FABLE_FRONTEND_SLEEP_MS=<ms> sets the sleep
     * (default 16; 50 is the game's own). */
    {
        const char *e = getenv("FABLE_FRONTEND_SLEEP_MS");
        int ms = e ? atoi(e) : 16;
        if (ms >= 0 && ms < 50) {
            xbox_SetDelayOverride(0x0020BD9Au, 50, (uint32_t)ms);   /* return addresses */
            xbox_SetDelayOverride(0x0020C3CCu, 50, (uint32_t)ms);
        }
    }
    /* Aspect ratio, above. */
    fable_aspect_configure();
}

#ifndef _WIN32
extern void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars);

/* ── Background cache progress ───────────────────────────────────────────
 *
 * On first run the title decompresses d:\Data\...\*.biz into z:\data\...\*.big
 * while its menus already run. A .biz starts with "biz!" and the decompressed
 * size (u32 LE), so progress is the bytes already in the cache over the sum of
 * those sizes. Shown in the window title and printed (stdout, logcat on
 * Android) whenever the whole percent changes.
 *
 * The sum is over the files the game creates, not every .biz on the disc: the
 * disc carries Data\lang\<language> for each language and the game unpacks
 * one (English and French on the PAL disc: 626 MB of .biz, 554 MB unpacked),
 * so counting them all left the line at 88 % forever. A language's files
 * join the total once the game has created one of them. The cache also does
 * not keep the disc's case (Data\Misc becomes data\misc), so the .big is
 * looked up case-insensitively, which matters on Android's file systems.
 *
 * Polling stops at 100 %, or once the unpacked size has not moved for 30 s
 * (a cache that was already complete, or one the game stopped writing). */
typedef struct {
    char big[1024];     /* expected path, disc case */
    char found[1024];   /* the path that exists, once it does */
    uint64_t size;
    int lang;           /* under Data/lang/<language>/ */
} CacheItem;
static CacheItem *s_items;
static int s_n, s_cap;
static char s_game_dir[1024];

/* Z: on the host: <user root>/Cache/4d5300d1 (kernel_path.c). */
extern const char *xbox_path_utility_dir_utf8(void);

static void cache_scan(const char *dir, const char *rel, const char *cache_root)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        char path[1024], sub[1024];
        size_t n = strlen(e->d_name);
        struct stat st;
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            cache_scan(path, sub, cache_root);
        } else if (n > 4 && !strcasecmp(e->d_name + n - 4, ".biz")) {
            FILE *f = fopen(path, "rb");
            unsigned char h[8];
            if (f && fread(h, 1, 8, f) == 8 && !memcmp(h, "biz!", 4)) {
                if (s_n == s_cap) {
                    CacheItem *grown;
                    s_cap = s_cap ? s_cap * 2 : 32;
                    grown = realloc(s_items, sizeof *s_items * (size_t)s_cap);
                    if (!grown) {
                        fclose(f);
                        break;
                    }
                    s_items = grown;
                }
                snprintf(s_items[s_n].big, sizeof s_items[0].big, "%s/%.*s.big",
                         cache_root, (int)(strlen(sub) - 4), sub);
                s_items[s_n].found[0] = 0;
                s_items[s_n].size = (uint64_t)h[4] | (uint64_t)h[5] << 8
                                  | (uint64_t)h[6] << 16 | (uint64_t)h[7] << 24;
                s_items[s_n].lang = !strncasecmp(sub, "lang/", 5);
                s_n++;
            }
            if (f)
                fclose(f);
        }
    }
    closedir(d);
}

/* Find `rel` (a path under `root`, '/'-separated) whatever the case of each
 * component. Returns 1 and fills `out` when it exists. */
static int find_nocase(const char *root, const char *rel, char *out, size_t cap)
{
    char cur[1024], part[256];
    snprintf(cur, sizeof cur, "%s", root);
    while (*rel) {
        size_t n = strcspn(rel, "/");
        DIR *d;
        struct dirent *e;
        int hit = 0;
        if (n >= sizeof part)
            return 0;
        memcpy(part, rel, n);
        part[n] = 0;
        rel += n;
        while (*rel == '/')
            rel++;
        if (!n)
            continue;
        if (!(d = opendir(cur)))
            return 0;
        while ((e = readdir(d)) != NULL) {
            if (!strcasecmp(e->d_name, part)) {
                size_t l = strlen(cur);
                snprintf(cur + l, sizeof cur - l, "/%s", e->d_name);
                hit = 1;
                break;
            }
        }
        closedir(d);
        if (!hit)
            return 0;
    }
    snprintf(out, cap, "%s", cur);
    return 1;
}

static void set_title(const char *ascii)
{
    uint16_t w[128];
    int i;
    for (i = 0; i < 127 && ascii[i]; i++)
        w[i] = (uint8_t)ascii[i];
    w[i] = 0;
    xbox_FramebufferWindowSetTitle(w, 127);
}

static void *cache_progress(void *unused)
{
    char data[1100], cache[1100];
    uint64_t last_done = (uint64_t)-1;
    unsigned still = 0;                 /* polls without the size moving */
    int i, last = -1, have_lang = 0;
    (void)unused;

    snprintf(data, sizeof data, "%s/Data", s_game_dir);
    snprintf(cache, sizeof cache, "%s/data", xbox_path_utility_dir_utf8());
    cache_scan(data, "", cache);
    for (i = 0; i < s_n; i++)
        have_lang |= s_items[i].lang;
    if (!s_n)
        return NULL;
    for (;;) {
        uint64_t done = 0, total = 0;
        int pct, lang_started = 0;
        struct stat st;

        for (i = 0; i < s_n; i++) {
            CacheItem *it = &s_items[i];
            if (!it->found[0])
                find_nocase(cache, it->big + strlen(cache), it->found,
                            sizeof it->found);
            if (it->found[0] && it->lang)
                lang_started = 1;
        }
        for (i = 0; i < s_n; i++) {
            CacheItem *it = &s_items[i];
            /* A language counts once the game has started on it: the one it
             * picked, not the others on the disc. */
            if (it->lang) {
                char dir[1024];
                const char *slash;
                int j, picked = 0;
                if (!lang_started)
                    continue;
                /* Same Data/lang/<language>/ directory as a created file? */
                slash = strchr(it->big + strlen(cache) + 6, '/');
                snprintf(dir, sizeof dir, "%.*s", slash
                         ? (int)(slash - it->big) : 0, it->big);
                for (j = 0; j < s_n && !picked; j++)
                    picked = s_items[j].lang && s_items[j].found[0]
                          && !strncasecmp(s_items[j].big, dir, strlen(dir))
                          && s_items[j].big[strlen(dir)] == '/';
                if (!picked)
                    continue;
            }
            total += it->size;
            if (it->found[0] && stat(it->found, &st) == 0)
                done += (uint64_t)st.st_size < it->size
                      ? (uint64_t)st.st_size : it->size;
        }
        pct = total ? (int)(done * 100 / total) : 0;
        if (pct != last) {
            char t[128];
            printf("[CACHE] background data preparation: %d %% (%llu / %llu MB)\n", pct,
                   (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
            fflush(stdout);
            if (pct < 100)
                snprintf(t, sizeof t, "Fable - preparing game data %d %%", pct);
            else
                snprintf(t, sizeof t, "Fable: The Lost Chapters");
            set_title(t);
            last = pct;
        }
        /* Done, unless the language files have not started yet. */
        if (pct >= 100 && (lang_started || !have_lang))
            return NULL;
        still = done == last_done ? still + 1 : 0;
        last_done = done;
        if (still >= 60) {              /* 30 s at 0.5 s */
            printf("[CACHE] no progress for 30 s; stopped at %d %% (%llu / %llu MB)\n",
                   pct, (unsigned long long)(done >> 20),
                   (unsigned long long)(total >> 20));
            fflush(stdout);
            if (pct < 100)
                set_title("Fable: The Lost Chapters");
            return NULL;
        }
        usleep(500 * 1000);
    }
}

void fable_start(const char *game_dir, const char *save_dir)
{
    pthread_t t;

    (void)save_dir;                     /* Z: is the kernel's to place */
    snprintf(s_game_dir, sizeof s_game_dir, "%s", game_dir ? game_dir : "game");
    if (pthread_create(&t, NULL, cache_progress, NULL) == 0)
        pthread_detach(t);
}
#else
void fable_start(const char *game_dir, const char *save_dir)
{
    (void)game_dir; (void)save_dir;
}
#endif
