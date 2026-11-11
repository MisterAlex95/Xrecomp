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
extern void xbox_SetDelayOverride(uint32_t ret_va, uint32_t from_ms, uint32_t to_ms);

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

/* ── D3D's deferred RunPushBuffer (software methods 0xC, 0xD, 0xE) ─────────
 *
 * XDK D3DDevice_RunPushBuffer (0x00855900) JUMPs from the ring into a
 * recorded pushbuffer whose last word is a JUMP back. If the GPU has not yet
 * passed the pushbuffer's previous use, that return JUMP cannot be patched
 * yet: D3D puts a software method before the entering JUMP and lets its
 * interrupt handler patch it when the GPU gets there (dispatcher
 * sub_00859C40, helper sub_00855860). Unserviced, the walk follows the old
 * return target and runs earlier commands again, and the visibility reports
 * Fable waits on in-world (sub_0012F7F0) never come.
 *
 *   0xC  node = payload | 0x80000000 (in the ring), one pending run fewer
 *   0xD  node = 0x00860044 + payload * 20 (D3D's table), one pending run fewer
 *   0xE  node = payload | 0x80000000, recording: the count is left alone
 *
 * A node is { return target | flags, next, slot, fixups, base }. As
 * sub_00855860 does, for each node of the chain: *slot = JUMP(target);
 * with flag bit 29, copy each fixup { size, offset, bytes } to base + offset
 * until size 0xFFFFFFFF; next = -1; with bit 31, clear bit 30. The handler's
 * final cache flush (sub_008556D0) has nothing to flush here.
 *
 * Runs on the executor thread (nv2a_pb_set_software_method_hook), while the
 * main thread may be in RunPushBuffer bumping the same pending count. */
extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void nv2a_pb_set_software_method_hook(int (*hook)(uint32_t param));

#define FABLE_RPB_NODES   0x00860044u   /* device (0x0085FDE0) + 0x264 */
#define FABLE_RPB_PENDING 0x0085FE30u   /* device + 0x50 */

static int fable_software_method(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t type = param & 0x1Fu, payload = param >> 5, node;
    int guard;
#define G32(a) (*(volatile uint32_t *)(mem + (uint32_t)(a)))

    switch (type) {
    case 0xC: node = payload | 0x80000000u; break;
    case 0xD: node = FABLE_RPB_NODES + payload * 20u; break;
    case 0xE: node = payload | 0x80000000u; break;
    default:  return 0;             /* the rest: not serviced */
    }
    if (type != 0xE) {
#if defined(__GNUC__) || defined(__clang__)
        __atomic_fetch_sub((uint32_t *)(mem + FABLE_RPB_PENDING), 1u, __ATOMIC_SEQ_CST);
#else
        G32(FABLE_RPB_PENDING) -= 1u;
#endif
    }
    for (guard = 0; guard < 4096; guard++) {
        uint32_t target = G32(node), next;

        G32(G32(node + 8)) = (target & 0x0FFFFFFFu) + 1u;      /* JUMP target */
        if (target & 0x20000000u) {
            uint32_t base = G32(node + 16), fix = G32(node + 12), size;
            while ((size = G32(fix)) != 0xFFFFFFFFu) {
                memcpy(mem + (uint32_t)(base + G32(fix + 4)), mem + fix + 8, size);
                fix += 8u + size;
            }
        }
        next = G32(node + 4);
        G32(node + 4) = 0xFFFFFFFFu;
        if (target & 0x80000000u)
            G32(node) = target & 0xBFFFFFFFu;
        if (next == 0 || next == 0xFFFFFFFFu)
            break;
        node = next;
    }
#undef G32
    return 1;
}

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
    /* Vblank interrupts on the 60 Hz schedule. The XMV decoder's clock is
     * D3DDevice_GetDisplayFieldStatus (sub_0084F650), which reads the vblank
     * count D3D's vblank handler keeps at device+0x1DE8; with no vblanks it
     * never moved, the decoder delivered the first frame and then waited for
     * ever (the movies were skipped with an override until now). The plain
     * ~50 Hz tick plays them slower than the game expects: paced. */
    SETENV_DEFAULT("RECOMP_VBLANK", "paced");
#ifdef __ANDROID__
    /* GPU renderer on Android: 30 fps, evenly paced. In Oakvale a frame
     * takes the executor 17-22 ms on an M1 and more on the AYN Thor, so a
     * 60 cap only lets the rate wander between 20 and 40; menus stay
     * smooth at 30 and the device stays cooler. Only read with
     * RECOMP_GPU=1, which is not this profile's default yet (see
     * docs/technical/gpu-backend.md, Android). The launcher's Frame cap
     * setting (recomp.cfg) wins. */
    SETENV_DEFAULT("RECOMP_GPU_FPS_CAP", "30");
#endif
    /* The one-CPU DISPATCH gate (kernel_hal.c). XAPI's USB stack (XPP,
     * 0x0086E480-0x00876C3C) locks only by raising to DISPATCH_LEVEL.
     * XInputSetState programs a control transfer ~30 times a second
     * (sub_008766DD takes 4 TDs from the free list at 0x9A614C) while the
     * USB DPC (sub_00875EA1, on the timer thread) frees TDs and re-arms the
     * pad's interrupt URB. Without the gate the two run side by side, the TD
     * pool and the ED queues get corrupted and within a minute the DPC loops
     * for ever in sub_00875A48: pad input dead, movies stalled. The gate
     * needs the paced vblank loop above. RECOMP_DISPATCH_GATE=0 to compare. */
    SETENV_DEFAULT("RECOMP_DISPATCH_GATE", "1");
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
    /* D3D's deferred RunPushBuffer, above. */
    nv2a_pb_set_software_method_hook(fable_software_method);
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
    snprintf(cache, sizeof cache, "%s/Cache/data", s_save_dir);
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
