/*
 * Quick reboot into the running title (kernel_reboot.c).
 *
 * Fable's Pause -> Quit -> Confirm calls XLaunchNewImage on its own
 * default.xbe and HalReturnToFirmware(2). The bridge writes the launch data
 * page to <save dir>/launch_data.bin and restarts the process; the next
 * kernel init takes the page back and deletes the file. Covered here:
 *   - the file round trip, byte for byte, with the restart times;
 *   - xbox_launch_data_take: reads <save dir>/launch_data.bin once and
 *     deletes it (a second take finds nothing), and deletes a corrupt file;
 *   - the launch-path match ("\Device\CdRom0;default.xbe" is
 *     XeImageFileName, another XBE or device is not);
 *   - the cap: XBOX_RESTART_MAX restarts in XBOX_RESTART_WINDOW_S seconds;
 *   - POSIX desktop: the default handler, end to end. A forked child opens a
 *     file and asks for a quick reboot; the process that execv starts is
 *     this test again, which takes the page, checks it and that the file
 *     descriptor did not survive the exec, and exits with the verdict;
 *   - the title leaving by itself (K-2): xbox_exit_reason's wording, and on
 *     POSIX desktop xbox_title_exit from a forked child: the exit handler gets
 *     the reason, the process then ends with status 0.
 * Otherwise nothing is restarted: the handler is one that counts.
 */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#endif

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;
static int restarts;
static char handed[256];

#if !defined(_WIN32) && !defined(__ANDROID__)
static int exit_fd = -1;

/* The exit handler of the forked child: the reason, down the pipe. */
static void record_exit(const char *why)
{
    if (exit_fd >= 0 && write(exit_fd, why, strlen(why)) < 0)
        _exit(31);
}
#endif

static void count_image(const char *target)
{
    snprintf(handed, sizeof handed, "%s", target);
}

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* A restart that "fails": counts and returns, so xbox_quick_reboot goes on
 * to its exit path and the test keeps running. */
static void fake_restart(void)
{
    restarts++;
}

static void fill_page(uint8_t *page, uint8_t seed)
{
    int i;

    memset(page, 0, XBOX_LAUNCH_PAGE_BYTES);
    page[0] = 0;                                  /* LDT_TITLE */
    page[4] = 0xD1; page[5] = 0x00; page[6] = 0x53; page[7] = 0x4D;
    strcpy((char *)page + 8, "\\Device\\CdRom0;default.xbe");
    for (i = 0; i < 3072; i++)
        page[0x400 + i] = (uint8_t)(i * 7 + seed);
}

#if !defined(_WIN32) && !defined(__ANDROID__)
/* The re-executed child (XBR_REBOOT_CHILD set): 0 when the page came back
 * and the descriptor opened before the exec is gone. */
static int reexec_child(void)
{
    uint8_t page[XBOX_LAUNCH_PAGE_BYTES], back[XBOX_LAUNCH_PAGE_BYTES];
    const char *fd_s = getenv("XBR_REBOOT_FD");
    int fd = fd_s ? atoi(fd_s) : -1;

    xbox_path_init(getenv("XBR_REBOOT_ROOT"), getenv("XBR_REBOOT_SAVE"));
    fill_page(page, 9);
    if (xbox_launch_data_take(back) != 0)
        return 10;
    if (memcmp(page, back, sizeof(page)) != 0)
        return 11;
    if (fd < 0 || fcntl(fd, F_GETFD) != -1)
        return 12;                       /* still open after the exec */
    return 0;
}
#endif

#if defined(__linux__) && !defined(__ANDROID__)
/* Whether this process can exec its own binary, as the default restart
 * handler does (execv /proc/self/exe). Under qemu-user it cannot: the exec
 * reaches the host kernel with the arm64 binary and fails with ENOEXEC
 * unless binfmt_misc is set up, a property of the emulator, not of the
 * restart. 1 = it can. */
static int self_exec_works(void)
{
    int status = -1;
    pid_t pid;

    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        setenv("XBR_REBOOT_PROBE", "1", 1);
        execl("/proc/self/exe", "kernel_reboot_probe", (char *)NULL);
        _exit(errno == ENOEXEC ? 21 : 22);
    }
    if (pid > 0)
        waitpid(pid, &status, 0);
    if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 21)
        return 0;
    return 1;   /* anything else: run the check and let it say what fails */
}
#else
static int self_exec_works(void) { return 1; }
#endif

int main(void)
{
    char root[512], save[600], file[700];
    uint8_t page[XBOX_LAUNCH_PAGE_BYTES], back[XBOX_LAUNCH_PAGE_BYTES];
    int64_t times[XBOX_RESTART_MAX], got[XBOX_RESTART_MAX];
    int n = -1;

#if !defined(_WIN32) && !defined(__ANDROID__)
    if (getenv("XBR_REBOOT_PROBE"))
        return 0;
    if (getenv("XBR_REBOOT_CHILD"))
        return reexec_child();
#endif

#if defined(_WIN32)
    snprintf(root, sizeof(root), "%s\\xbr_reboot_%d",
             getenv("TEMP") ? getenv("TEMP") : ".", (int)GetCurrentProcessId());
    _mkdir(root);
    snprintf(save, sizeof(save), "%s\\save", root);
#else
    snprintf(root, sizeof(root), "%s/xbr_reboot_%d",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    mkdir(root, 0755);
    snprintf(save, sizeof(save), "%s/save", root);
#endif
    xbox_path_init(root, save);
    snprintf(file, sizeof(file), "%s/%s", xbox_path_save_dir_utf8(),
             XBOX_LAUNCH_DATA_FILE);
    printf("kernel_reboot: save dir %s\n", xbox_path_save_dir_utf8());

    /* Round trip. */
    fill_page(page, 3);
    times[0] = 1700000000; times[1] = 1700000010; times[2] = 0x123456789LL;
    check(xbox_launch_data_write(file, page, times, 3) == 0, "write");
    check(xbox_launch_data_read(file, back, got, &n) == 0, "read");
    check(memcmp(page, back, sizeof(page)) == 0, "page identical, 4096 bytes");
    check(n == 3 && got[0] == times[0] && got[1] == times[1] &&
          got[2] == times[2], "restart times identical (64-bit)");

    /* take: once, then gone. */
    memset(back, 0, sizeof(back));
    check(xbox_launch_data_take(back) == 0, "take finds the page");
    check(memcmp(page, back, sizeof(page)) == 0, "take returns the page");
    check(!exists(file), "take deletes the file");
    check(xbox_launch_data_take(back) != 0, "second take finds nothing");

    /* A corrupt file is not a page, and does not stay. */
    {
        FILE *f = fopen(file, "wb");
        if (f) { fputs("not a launch page", f); fclose(f); }
    }
    check(xbox_launch_data_take(back) != 0, "truncated file refused");
    check(!exists(file), "truncated file deleted");

    /* Paths. */
    check(xbox_launch_path_is_image("\\Device\\CdRom0;default.xbe",
                                    "\\Device\\CdRom0\\default.xbe"),
          "\\Device\\CdRom0;default.xbe is the running image");
    check(xbox_launch_path_is_image("\\device\\cdrom0\\;DEFAULT.XBE",
                                    "\\Device\\CdRom0\\default.xbe"),
          "case and a trailing backslash do not matter");
    check(!xbox_launch_path_is_image("\\Device\\CdRom0;other.xbe",
                                     "\\Device\\CdRom0\\default.xbe"),
          "another XBE on the disc is not");
    check(!xbox_launch_path_is_image("\\Device\\Harddisk0\\Partition2;xboxdash.xbe",
                                     "\\Device\\CdRom0\\default.xbe"),
          "the dashboard is not");
    check(!xbox_launch_path_is_image("", "\\Device\\CdRom0\\default.xbe"),
          "an empty path is not");

    /* XBA-82: the running image's name, and a hand-over through the image
     * handler (Android's), whatever the host. */
    check(strcmp(xbox_image_file_name(), "\\Device\\CdRom0\\default.xbe") == 0,
          "no RECOMP_IMAGE: the image is default.xbe");
#ifdef _WIN32
    _putenv_s("RECOMP_IMAGE", "sub/game.xbe");
#else
    setenv("RECOMP_IMAGE", "sub/game.xbe", 1);
#endif
    check(strcmp(xbox_image_file_name(), "\\Device\\CdRom0\\sub\\game.xbe") == 0,
          "RECOMP_IMAGE=sub/game.xbe names \\Device\\CdRom0\\sub\\game.xbe");
    check(!xbox_launch_path_is_image("\\Device\\CdRom0;default.xbe", xbox_image_file_name()),
          "from that image, default.xbe is another image");
#ifdef _WIN32
    _putenv_s("RECOMP_IMAGE", "../x.xbe");
#else
    setenv("RECOMP_IMAGE", "../x.xbe", 1);
#endif
    check(strcmp(xbox_image_file_name(), "\\Device\\CdRom0\\default.xbe") == 0,
          "a path with .. is refused");
    {
        char map[700];
        FILE *f;
        snprintf(map, sizeof map, "%s/images.map", root);
        f = fopen(map, "w");
        if (f) { fputs("default.xbe\tdefault.xbe\nSub/Game.xbe\tSub/Game.xbe\n", f); fclose(f); }
#ifdef _WIN32
        _putenv_s("RECOMP_IMAGE_MAP", map);
#else
        setenv("RECOMP_IMAGE_MAP", map, 1);
#endif
        fill_page(page, 9);
        check(xbox_launch_image("\\Device\\CdRom0;nothere.xbe", page) != 0 && !handed[0],
              "an image not in the map: no hand-over");
        xbox_SetImageHandler(count_image);
        check(xbox_launch_image("\\Device\\CdRom0\\sub;game.xbe", page) != 0,
              "the handler returned: the launch reports failure");
        check(strcmp(handed, "Sub/Game.xbe") == 0, "the handler got the map's target (any case)");
        check(!exists(file), "and the launch data is not left behind");
        xbox_SetImageHandler(NULL);
        remove(map);
    }

    /* The window. */
    times[0] = 1000; times[1] = 1020; times[2] = 1029;
    check(xbox_restart_recent(times, 3, 1029) == 3, "three restarts within 30 s");
    check(xbox_restart_recent(times, 3, 1031) == 2, "the oldest leaves the window");
    check(xbox_restart_recent(times, 3, 2000) == 0, "none after a long run");

    /* xbox_quick_reboot with a handler that returns: one call, and the file
     * it wrote is removed again (no page for a later, unrelated start). */
    xbox_SetRestartHandler(fake_restart);
    xbox_quick_reboot(page);
    check(restarts == 1, "quick reboot calls the handler");
    check(!exists(file), "a failed restart leaves no launch data behind");

    /* The cap: a page carrying XBOX_RESTART_MAX recent restarts is refused
     * without calling the handler. */
    {
        int64_t now = (int64_t)time(NULL);
        times[0] = now - 5; times[1] = now - 3; times[2] = now - 1;
        xbox_launch_data_write(file, page, times, 3);
        xbox_launch_data_take(back);          /* as kernel init does */
        restarts = 0;
        xbox_quick_reboot(page);
        check(restarts == 0, "refused past 3 restarts in 30 s");

        times[0] = now - 100; times[1] = now - 3; times[2] = now - 1;
        xbox_launch_data_write(file, page, times, 3);
        xbox_launch_data_take(back);
        xbox_quick_reboot(page);
        check(restarts == 1, "allowed when one is older than 30 s");
    }
    xbox_SetRestartHandler(NULL);

#if !defined(_WIN32) && !defined(__ANDROID__)
    /* The default handler: execv of this binary, from a forked child. */
    if (!self_exec_works()) {
        printf("  %-66s SKIP\n", "default handler re-executes (this host cannot exec itself: qemu-user?)");
    } else {
        char fdbuf[16];
        int status = -1;
        pid_t pid;

        fflush(NULL);
        pid = fork();
        if (pid == 0) {
            int fd = open(file, O_WRONLY | O_CREAT, 0644);   /* like a partition image */
            snprintf(fdbuf, sizeof(fdbuf), "%d", fd);
            setenv("XBR_REBOOT_CHILD", "1", 1);
            setenv("XBR_REBOOT_FD", fdbuf, 1);
            setenv("XBR_REBOOT_ROOT", root, 1);
            setenv("XBR_REBOOT_SAVE", save, 1);
            close(fd);
            fd = open(save, O_RDONLY);                       /* stays open */
            snprintf(fdbuf, sizeof(fdbuf), "%d", fd);
            setenv("XBR_REBOOT_FD", fdbuf, 1);
            remove(file);
            fill_page(page, 9);
            xbox_quick_reboot(page);
            _exit(20);                                       /* not restarted */
        }
        if (pid > 0)
            waitpid(pid, &status, 0);
        check(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "default handler re-executes with the page, fds closed");
        if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
            printf("    child status 0x%X\n", status);
        check(!exists(file), "the restarted process took the file");
    }
#endif

    /* The title leaving by itself: the reason the launcher shows. */
    {
        char why[640];

        xbox_exit_reason(why, sizeof why, 0, 2, "", 0);
        check(strcmp(why, "the game returned to the dashboard (routine 2)") == 0,
              "reason: dashboard, empty launch path");
        xbox_exit_reason(why, sizeof why, 0, 2, "", 3);
        check(strcmp(why, "the game returned to the dashboard (routine 2,"
                          " dashboard reason 3)") == 0,
              "reason: dashboard with XapiBootToDash's reason");
        xbox_exit_reason(why, sizeof why, 0, 1, NULL, 7);
        check(strcmp(why, "the game returned to the dashboard (routine 1)") == 0,
              "reason: no launch data page");
        xbox_exit_reason(why, sizeof why, 0, 2, "\\Device\\CdRom0;other.xbe", 0);
        check(strcmp(why, "the game asked to start \\Device\\CdRom0;other.xbe,"
                          " which this build cannot start (routine 2)") == 0,
              "reason: another image this build cannot start");
        xbox_exit_reason(why, sizeof why, 1, 0, NULL, 0);
        check(strcmp(why, "the game shut the console down") == 0,
              "reason: HalInitiateShutdown");
    }

#if !defined(_WIN32) && !defined(__ANDROID__)
    /* xbox_title_exit: the handler first, with the reason; then the process
     * ends with status 0 although the handler returned. */
    {
        int pfd[2], status = -1;
        pid_t pid = -1;
        char got[700];
        ssize_t n = 0;

        fflush(NULL);
        if (pipe(pfd) == 0) {
            pid = fork();
            if (pid == 0) {
                close(pfd[0]);
                exit_fd = pfd[1];
                xbox_SetExitHandler(record_exit);
                xbox_title_exit(2, "the game returned to the dashboard (routine 2)");
                _exit(30);                                   /* returned: wrong */
            }
            close(pfd[1]);
            if (pid > 0) {
                ssize_t r;
                while (n < (ssize_t)sizeof(got) - 1 &&
                       (r = read(pfd[0], got + n, sizeof(got) - 1 - n)) > 0)
                    n += r;
                waitpid(pid, &status, 0);
            }
            close(pfd[0]);
        }
        got[n > 0 ? n : 0] = 0;
        check(strcmp(got, "the game returned to the dashboard (routine 2)") == 0,
              "title exit: the handler gets the reason");
        check(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "title exit: the process then ends with status 0");
    }
#endif

    remove(file);
    printf("kernel_reboot: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
