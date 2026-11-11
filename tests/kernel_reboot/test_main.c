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
 *     descriptor did not survive the exec, and exits with the verdict.
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
#endif

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;
static int restarts;

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

int main(void)
{
    char root[512], save[600], file[700];
    uint8_t page[XBOX_LAUNCH_PAGE_BYTES], back[XBOX_LAUNCH_PAGE_BYTES];
    int64_t times[XBOX_RESTART_MAX], got[XBOX_RESTART_MAX];
    int n = -1;

#if !defined(_WIN32) && !defined(__ANDROID__)
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
    {
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

    remove(file);
    printf("kernel_reboot: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
