/*
 * kernel_reboot.c - quick reboot into the running title.
 *
 * On a console HalReturnToFirmware(2) reboots without a dashboard round trip,
 * and the launch data page -- persisted contiguous memory -- survives it, so
 * the next image finds it with XGetLaunchInfo. XLaunchNewImage is built on
 * that, and a title that launches its own default.xbe is restarting itself:
 * Fable's Pause -> Quit -> Confirm goes back to its main menu that way. Here
 * the process exited instead, which ended the game.
 *
 * The page survives a host restart through <save dir>/launch_data.bin: the
 * bridge's HalReturnToFirmware writes it and calls the restart handler; kernel
 * init (kernel_data_init) takes it back into export 164 and deletes the file.
 * The file also carries the times of the last restarts, so a title that
 * reboots into itself at once cannot loop: past XBOX_RESTART_MAX restarts in
 * XBOX_RESTART_WINDOW_S seconds the title exits as before.
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif
#endif

#define LAUNCH_FILE_MAGIC   0x444C4258u   /* "XBLD" little-endian */
#define LAUNCH_FILE_VERSION 1u

/* magic, version, ntimes, reserved; times[XBOX_RESTART_MAX]; the page. */
#define LAUNCH_FILE_HEADER  (16 + 8 * XBOX_RESTART_MAX)
#define LAUNCH_FILE_BYTES   (LAUNCH_FILE_HEADER + XBOX_LAUNCH_PAGE_BYTES)

static xbox_restart_handler s_restart_handler;

/* The restarts that led to this process (from the file kernel init took). */
static int64_t s_restart_times[XBOX_RESTART_MAX];
static int     s_restart_count;

void xbox_SetRestartHandler(xbox_restart_handler fn)
{
    s_restart_handler = fn;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

int xbox_launch_data_write(const char *path, const uint8_t *page,
                           const int64_t *times, int ntimes)
{
    uint8_t buf[LAUNCH_FILE_BYTES];
    char tmp[1024];
    FILE *f;
    int i, ok;

    if (!path || !page || ntimes < 0)
        return -1;
    if (ntimes > XBOX_RESTART_MAX) {
        times += ntimes - XBOX_RESTART_MAX;   /* the newest ones */
        ntimes = XBOX_RESTART_MAX;
    }
    memset(buf, 0, sizeof(buf));
    put32(buf + 0, LAUNCH_FILE_MAGIC);
    put32(buf + 4, LAUNCH_FILE_VERSION);
    put32(buf + 8, (uint32_t)ntimes);
    for (i = 0; i < ntimes; i++) {
        put32(buf + 16 + i * 8, (uint32_t)((uint64_t)times[i]));
        put32(buf + 20 + i * 8, (uint32_t)((uint64_t)times[i] >> 32));
    }
    memcpy(buf + LAUNCH_FILE_HEADER, page, XBOX_LAUNCH_PAGE_BYTES);

    /* Written aside and renamed: a half-written file must not be taken for a
     * page by the next start. */
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f)
        return -1;
    ok = fwrite(buf, 1, sizeof(buf), f) == sizeof(buf);
    ok = (fclose(f) == 0) && ok;
    if (ok) {
#if defined(_WIN32)
        ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
        ok = rename(tmp, path) == 0;
#endif
    }
    if (!ok)
        remove(tmp);
    return ok ? 0 : -1;
}

int xbox_launch_data_read(const char *path, uint8_t *page,
                          int64_t *times, int *ntimes)
{
    uint8_t buf[LAUNCH_FILE_BYTES + 1];
    FILE *f;
    size_t n;
    uint32_t count;
    uint32_t i;

    if (!path || !page)
        return -1;
    f = fopen(path, "rb");
    if (!f)
        return -1;
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n != LAUNCH_FILE_BYTES || get32(buf) != LAUNCH_FILE_MAGIC ||
        get32(buf + 4) != LAUNCH_FILE_VERSION)
        return -1;
    count = get32(buf + 8);
    if (count > XBOX_RESTART_MAX)
        return -1;
    for (i = 0; i < count; i++) {
        uint64_t t = (uint64_t)get32(buf + 16 + i * 8) |
                     (uint64_t)get32(buf + 20 + i * 8) << 32;
        if (times)
            times[i] = (int64_t)t;
    }
    if (ntimes)
        *ntimes = (int)count;
    memcpy(page, buf + LAUNCH_FILE_HEADER, XBOX_LAUNCH_PAGE_BYTES);
    return 0;
}

static int launch_file_path(char *out, size_t n)
{
    const char *dir = xbox_path_save_dir_utf8();

    if (!dir || !dir[0])
        return -1;
    snprintf(out, n, "%s/%s", dir, XBOX_LAUNCH_DATA_FILE);
    return 0;
}

int xbox_launch_data_take(uint8_t *page)
{
    char path[1024];
    int rc;

    if (launch_file_path(path, sizeof(path)) != 0)
        return -1;
    s_restart_count = 0;
    rc = xbox_launch_data_read(path, page, s_restart_times, &s_restart_count);
    /* Deleted either way: the page is one-shot, as on the console, and a file
     * that cannot be read must not stand in front of every later start. */
    if (remove(path) == 0 && rc != 0)
        fprintf(stderr, "  [KERNEL] %s unreadable, deleted\n", path);
    if (rc != 0)
        s_restart_count = 0;
    return rc;
}

/* "\Device\CdRom0;default.xbe" names "\Device\CdRom0\default.xbe". A path
 * without ';' is compared as it is. */
int xbox_launch_path_is_image(const char *launch_path, const char *image_path)
{
    char joined[600];
    const char *semi;
    size_t i;

    if (!launch_path || !image_path || !launch_path[0])
        return 0;
    semi = strchr(launch_path, ';');
    if (semi) {
        size_t dir = (size_t)(semi - launch_path);
        while (dir > 0 && launch_path[dir - 1] == '\\')
            dir--;
        snprintf(joined, sizeof(joined), "%.*s\\%s", (int)dir, launch_path,
                 semi + 1);
    } else {
        snprintf(joined, sizeof(joined), "%s", launch_path);
    }
    for (i = 0; joined[i] && image_path[i]; i++) {
        char a = joined[i], b = image_path[i];
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return joined[i] == 0 && image_path[i] == 0;
}

int xbox_restart_recent(const int64_t *times, int n, int64_t now)
{
    int i, recent = 0;

    for (i = 0; i < n; i++)
        if (times[i] <= now && now - times[i] < XBOX_RESTART_WINDOW_S)
            recent++;
    return recent;
}

/* ---- The desktop default -------------------------------------------------- */

#if defined(_WIN32)

static void default_restart(void)
{
    WCHAR exe[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    LPWSTR cmd;

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return;
    cmd = _wcsdup(GetCommandLineW());   /* CreateProcessW may write to it */
    if (!cmd)
        return;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    /* No inherited handles: the partition images and caches close when this
     * process ends, right below, long before the new one's kernel opens them. */
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "  [KERNEL] restart: CreateProcess failed (%lu)\n",
                (unsigned long)GetLastError());
        free(cmd);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    fflush(NULL);
    ExitProcess(0);
}

#elif defined(__ANDROID__)

/* No sensible default: the app process cannot exec itself. The shell sets
 * the handler (android/app/src/main/cpp/shell_main.c). */
static void default_restart(void)
{
    fprintf(stderr, "  [KERNEL] restart: no restart handler on this host\n");
}

#else

/* Everything past stderr closes on exec: partition images, caches, the
 * window's and the audio device's descriptors. Data written through them is
 * already the kernel's (write(2), no user-space buffer), so the new process
 * reads it back. */
static void close_on_exec_all(void)
{
    struct rlimit rl;
    long max = 4096;
    int fd;

    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        max = (long)rl.rlim_cur;
    if (max > 65536)
        max = 65536;
    for (fd = 3; fd < max; fd++) {
        int fl = fcntl(fd, F_GETFD);
        if (fl >= 0 && !(fl & FD_CLOEXEC))
            fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }
}

#if defined(__APPLE__)
static void default_restart(void)
{
    char exe[4096];
    uint32_t size = sizeof(exe);
    char **argv = *_NSGetArgv();
    sigset_t none;

    if (_NSGetExecutablePath(exe, &size) != 0)
        return;
    close_on_exec_all();
    /* The calling thread's mask is the new process's mask. */
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, NULL);
    fflush(NULL);
    execv(exe, argv);
    fprintf(stderr, "  [KERNEL] restart: execv %s: %s\n", exe, strerror(errno));
}
#else
static void default_restart(void)
{
    char *args[256];
    static char cmdline[65536];
    size_t n = 0, i;
    int argc = 0;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    sigset_t none;

    if (!f)
        return;
    n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
    fclose(f);
    cmdline[n] = 0;
    for (i = 0; i < n && argc < 255; i += strlen(cmdline + i) + 1)
        args[argc++] = cmdline + i;
    args[argc] = NULL;
    if (!argc)
        return;
    close_on_exec_all();
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, NULL);
    fflush(NULL);
    execv("/proc/self/exe", args);
    fprintf(stderr, "  [KERNEL] restart: execv /proc/self/exe: %s\n",
            strerror(errno));
}
#endif

#endif

void xbox_quick_reboot(const uint8_t *page)
{
    int64_t now = (int64_t)time(NULL);
    int64_t times[XBOX_RESTART_MAX + 1];
    int n = 0, i;
    char path[1024];

    /* The restarts still inside the window, oldest first, then this one. */
    for (i = 0; i < s_restart_count; i++)
        if (s_restart_times[i] <= now &&
            now - s_restart_times[i] < XBOX_RESTART_WINDOW_S)
            times[n++] = s_restart_times[i];
    if (n >= XBOX_RESTART_MAX) {
        fprintf(stderr, "  [KERNEL] quick reboot refused: %d restarts in the last"
                        " %d s, exiting instead\n", n, XBOX_RESTART_WINDOW_S);
        fflush(stderr);
        return;
    }
    times[n++] = now;

    if (launch_file_path(path, sizeof(path)) != 0 ||
        xbox_launch_data_write(path, page, times, n) != 0) {
        fprintf(stderr, "  [KERNEL] quick reboot: cannot write the launch data"
                        " (%s), exiting instead\n", path);
        fflush(stderr);
        return;
    }
    fprintf(stderr, "  [KERNEL] quick reboot: launch data in %s, restarting the"
                    " title (%d in the last %d s)\n", path, n,
            XBOX_RESTART_WINDOW_S);
    fflush(stdout);
    fflush(stderr);

    (s_restart_handler ? s_restart_handler : default_restart)();

    /* Still here: the restart did not happen. No page for a later start. */
    remove(path);
    fprintf(stderr, "  [KERNEL] quick reboot failed, exiting instead\n");
    fflush(stderr);
}

/* ---- Another image of the disc (XBA-82, desktop) ------------------------- */

/* The disc-relative path of a launch path, lowercase, with '/':
 * "\Device\CdRom0\game.xbe", "D:\game.xbe", "\??\D:\x\y.xbe" and
 * "\Device\CdRom0;game.xbe" give "game.xbe" / "x/y.xbe". */
/* The disc-relative part of p (one of the prefixes below stripped), lowercase,
 * with '/', appended to out at *o. */
static void rel_append(const char *p, size_t len, char *out, size_t n, size_t *o)
{
    static const char *const prefixes[] = { "\\device\\cdrom0", "\\??\\d:", "d:", "\\??" };
    size_t i;

    for (i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        size_t l = strlen(prefixes[i]), k;
        for (k = 0; k < l && k < len && (p[k] | 0x20) == (prefixes[i][k] | 0x20); k++)
            ;
        if (k == l && (len == l || p[l] == '\\' || p[l] == '/')) {
            p += l;
            len -= l;
            break;
        }
    }
    while (len && (*p == '\\' || *p == '/')) {
        p++;
        len--;
    }
    for (; len && *o + 1 < n; p++, len--) {
        char c = *p == '\\' ? '/' : *p;
        out[(*o)++] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    out[*o] = 0;
}

void xbox_launch_rel_path(const char *launch_path, char *out, size_t n)
{
    const char *p = launch_path ? launch_path : "";
    const char *semi = strchr(p, ';');
    size_t o = 0;

    if (!n)
        return;
    out[0] = 0;
    if (semi) {                 /* "<directory>;<file>", as XLaunchNewImage writes it */
        rel_append(p, (size_t)(semi - p), out, n, &o);
        while (o && out[o - 1] == '/')
            out[--o] = 0;
        if (o && o + 1 < n) {
            out[o++] = '/';
            out[o] = 0;
        }
        p = semi + 1;
    }
    rel_append(p, strlen(p), out, n, &o);
}

static xbox_image_handler s_image_handler;

void xbox_SetImageHandler(xbox_image_handler fn)
{
    s_image_handler = fn;
}

/* The map line for launch_path: its target (executable, or what the image
 * handler takes) in out. 0 when found. */
static int launch_map_lookup(const char *launch_path, char *out, size_t n)
{
    const char *map = getenv("RECOMP_IMAGE_MAP");
    char want[600], line[2048];
    FILE *f;
    int found = -1;

    if (!map || !*map || !(f = fopen(map, "r")))
        return -1;
    xbox_launch_rel_path(launch_path, want, sizeof want);
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t'), rel[600];
        size_t l;
        if (!tab)
            continue;
        *tab = 0;
        xbox_launch_rel_path(line, rel, sizeof rel);
        if (strcmp(rel, want))
            continue;
        snprintf(out, n, "%s", tab + 1);
        l = strlen(out);
        while (l && (out[l - 1] == '\n' || out[l - 1] == '\r'))
            out[--l] = 0;
        found = l ? 0 : -1;
        break;
    }
    fclose(f);
    return found;
}

int xbox_launch_image(const char *launch_path, const uint8_t *page)
{
    char target[1536], lpath[1024];
    int64_t now = (int64_t)time(NULL);

    if (launch_map_lookup(launch_path, target, sizeof target) != 0)
        return -1;
#if defined(_WIN32) || defined(__ANDROID__)
    if (!s_image_handler)
        return -1;              /* no exec hand-over on these hosts */
#else
    if (!s_image_handler && access(target, X_OK) != 0)
        return -1;
#endif
    if (launch_file_path(lpath, sizeof lpath) != 0 || xbox_launch_data_write(lpath, page, &now, 1) != 0) {
        fprintf(stderr, "  [KERNEL] launch of '%s': cannot write the launch data, exiting instead\n", launch_path);
        return -1;
    }
    if (s_image_handler) {
        fprintf(stderr, "  [KERNEL] launch of '%s': handing over to %s (image handler)\n", launch_path, target);
        fflush(NULL);
        s_image_handler(target);
        fprintf(stderr, "  [KERNEL] launch of '%s': the image handler returned\n", launch_path);
        remove(lpath);
        return -1;
    }
#if !defined(_WIN32) && !defined(__ANDROID__)
    fprintf(stderr, "  [KERNEL] launch of '%s': handing over to %s (RECOMP_IMAGE_MAP)\n", launch_path, target);
    {
        char *args[2];
        sigset_t none;
        args[0] = target;
        args[1] = NULL;
        close_on_exec_all();
        sigemptyset(&none);
        pthread_sigmask(SIG_SETMASK, &none, NULL);
        fflush(NULL);
        execv(target, args);
        fprintf(stderr, "  [KERNEL] launch of '%s': execv %s: %s\n", launch_path, target, strerror(errno));
    }
#endif
    remove(lpath);
    return -1;
}

/* XeImageFileName's text: "\Device\CdRom0\" + $RECOMP_IMAGE ('/' as '\'),
 * or default.xbe. At most 63 bytes: the guest's copy has 64
 * (KDATA_XE_IMAGE_BUF). */
const char *xbox_image_file_name(void)
{
    static char name[64];
    const char *rel = getenv("RECOMP_IMAGE");
    size_t o, i;

    if (!rel || !*rel || strlen(rel) > 47 || strstr(rel, ".."))
        rel = "default.xbe";
    while (*rel == '/' || *rel == '\\')
        rel++;
    o = (size_t)snprintf(name, sizeof name, "\\Device\\CdRom0\\");
    for (i = 0; rel[i] && o + 1 < sizeof name; i++)
        name[o++] = rel[i] == '/' ? '\\' : rel[i];
    name[o] = 0;
    return name;
}
