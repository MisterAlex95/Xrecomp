/*
 * Fable: The Lost Chapters (Xbox, 2005), title ID 4D5300D1 -- the title profile.
 *
 * tools.prepare copies this folder into the title project's src/title/, and
 * the template's main.c calls the two hooks below. Everything here was found
 * bringing the title up on macOS arm64 (see NOTES.md in this folder).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void xbox_SetMapSize(size_t bytes);
extern void xbox_SetDeferredIoApc(int on);

#ifndef _WIN32
#include <dirent.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#define SETENV_DEFAULT(k, v) setenv((k), (v), 0)
#else
#define SETENV_DEFAULT(k, v) do { if (!getenv(k)) _putenv(k "=" v); } while (0)
#endif

void xba_title_configure(void)
{
    /* Fable reserves 608 MB of address space for its own allocator and
     * sub-allocates across all of it. With the mapping the size of RAM the
     * reserve was clamped to 38 MB, the allocator walked into the RAM mirrors
     * and two of its strings shared storage. RAM stays 64 MB. */
    xbox_SetMapSize((size_t)1024 * 1024 * 1024);
    /* With the mapping above 64 MB the desktop POSIX layout lost pages of the
     * contiguous window; holding the whole 4 GB window (Android's default)
     * places every view inside one reservation. */
    SETENV_DEFAULT("RECOMP_GUEST_WINDOW", "1");
    /* XDK D3D and DirectSound: the pushbuffer executor, and the emulated APU
     * (DirectSoundCreate waits on the AC'97 codec). */
    SETENV_DEFAULT("RECOMP_PB_EXEC", "1");
    SETENV_DEFAULT("RECOMP_AC97_READY", "1");
    /* More than the default 8 worker threads alive during the front-end load. */
    SETENV_DEFAULT("RECOMP_THREAD_STACKS", "32");
    /* The pad (the template starts the USB model when RECOMP_USB is set); the
     * keyboard stands in for it on a desktop. */
    SETENV_DEFAULT("RECOMP_USB", "1");
    SETENV_DEFAULT("RECOMP_KEYBOARD", "1");
    /* File-I/O completion APCs at the next alertable wait, as the hardware
     * does, not inline inside NtReadFile (kernel_bridge.c, deliver_one_apc).
     * Delivered inline, the loader's completion ran before the read call had
     * returned and its bookkeeping was set up. FABLE_INLINE_APC=1 restores
     * the inline delivery for comparison. */
    if (!getenv("FABLE_INLINE_APC"))
        xbox_SetDeferredIoApc(1);
}

#ifndef _WIN32
extern ptrdiff_t g_xbox_mem_offset;
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars);

/* ── D3D's GPU semaphore ─────────────────────────────────────────────────
 *
 * Fable links the XDK's D3D. Its device pointer lives at 0x00862288 (read by
 * the ring make-space loop sub_00855260), and device+0x30 points at the GPU
 * semaphore D3D polls. Nothing writes it unless the pushbuffer executor is
 * told where it is (docs/technical/d3d8ltcg-device-context.md). The slot does
 * not always hold the device, so only a semaphore in contiguous memory is
 * accepted: handing over a heap address made the executor write into game
 * data. */
#define FABLE_D3D_DEVICE_PTR 0x00862288u

static void *semaphore_watch(void *unused)
{
    uint32_t last = 0;
    (void)unused;
    for (;;) {
        uint32_t dev = *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + FABLE_D3D_DEVICE_PTR);
        uint32_t sem = dev ? *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + dev + 0x30) : 0;
        if (sem && sem != last && sem >= 0x80000000u && sem < 0x84000000u) {
            nv2a_pb_set_semaphore_target(sem);
            fprintf(stderr, "[D3D] device 0x%08X, GPU semaphore at 0x%08X\n", dev, sem);
            last = sem;
        }
        usleep(1000);
    }
    return NULL;
}

/* ── Background cache progress ───────────────────────────────────────────
 *
 * On first run the title decompresses every d:\Data\...\*.biz into
 * z:\data\...\*.big (about 626 MB) while its menus already run. A .biz starts
 * with "biz!" and the decompressed size (u32 LE), so progress is the bytes
 * already in the cache over the sum of those sizes. Shown in the window title
 * and printed (stdout, logcat on Android) whenever the whole percent changes. */
typedef struct { char big[1024]; uint64_t size; } CacheItem;
static CacheItem *s_items;
static int s_n, s_cap;
static char s_game_dir[1024], s_save_dir[1024];

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
                s_items[s_n].size = (uint64_t)h[4] | (uint64_t)h[5] << 8
                                  | (uint64_t)h[6] << 16 | (uint64_t)h[7] << 24;
                s_n++;
            }
            if (f)
                fclose(f);
        }
    }
    closedir(d);
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
    uint64_t total = 0;
    int i, last = -1;
    (void)unused;

    snprintf(data, sizeof data, "%s/Data", s_game_dir);
    snprintf(cache, sizeof cache, "%s/Cache/data", s_save_dir);
    cache_scan(data, "", cache);
    for (i = 0; i < s_n; i++)
        total += s_items[i].size;
    if (!total)
        return NULL;
    for (;;) {
        uint64_t done = 0;
        int pct;
        for (i = 0; i < s_n; i++) {
            struct stat st;
            if (stat(s_items[i].big, &st) == 0)
                done += (uint64_t)st.st_size < s_items[i].size
                      ? (uint64_t)st.st_size : s_items[i].size;
        }
        pct = (int)(done * 100 / total);
        if (pct != last) {
            char t[128];
            printf("[CACHE] background data preparation: %d %% (%llu / %llu MB)\n", pct,
                   (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
            fflush(stdout);
            if (pct < 100)
                snprintf(t, sizeof t, "Fable - preparation des donnees %d %%", pct);
            else
                snprintf(t, sizeof t, "Fable: The Lost Chapters");
            set_title(t);
            last = pct;
        }
        if (pct >= 100)
            return NULL;
        usleep(500 * 1000);
    }
}

void xba_title_start(const char *game_dir, const char *save_dir)
{
    pthread_t t;

    snprintf(s_game_dir, sizeof s_game_dir, "%s", game_dir ? game_dir : "game");
    if (save_dir) {
        snprintf(s_save_dir, sizeof s_save_dir, "%s", save_dir);
    } else {
        /* The kernel's default save directory (kernel_path.c). */
        const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
        if (x && *x)
            snprintf(s_save_dir, sizeof s_save_dir, "%s/xboxrecomp", x);
        else
            snprintf(s_save_dir, sizeof s_save_dir, "%s/.local/share/xboxrecomp", h ? h : ".");
    }
    if (pthread_create(&t, NULL, semaphore_watch, NULL) == 0)
        pthread_detach(t);
    if (pthread_create(&t, NULL, cache_progress, NULL) == 0)
        pthread_detach(t);
}
#else
void xba_title_start(const char *game_dir, const char *save_dir)
{
    (void)game_dir; (void)save_dir;
}
#endif
