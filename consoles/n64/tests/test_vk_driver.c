/* test_vk_driver.c -- the custom Vulkan driver selection of the N64 RDP (n64_vk_driver.h): the RECOMP_VK_* variables,
 * the trial marker, last_start and <id>.status. Off Android libxba_vkdriver.so does not exist, so every open that gets
 * that far fails as a driver that cannot be opened on a device does: the system-driver path (open_failed). With
 * --wrapper, a fake libxba_vkdriver.so (fake_vkdriver.c, found through LD_LIBRARY_PATH / DYLD_LIBRARY_PATH) opens it:
 * the outcomes the RDP reports afterwards (ok, unsupported, not_loaded). No Vulkan needed. */
#include "n64_vk_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c)                                                                                                        \
    do {                                                                                                                \
        if (!(c)) {                                                                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                \
            fails++;                                                                                                    \
        }                                                                                                               \
    } while (0)

static char root[512], drv[600];

static int read_line(const char *name, char *out, size_t n)
{
    char p[700];
    snprintf(p, sizeof p, "%s/%s", root, name);
    FILE *f = fopen(p, "r");
    if (!f)
        return 0;
    if (!fgets(out, (int)n, f))
        out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return 1;
}

static void write_line(const char *name, const char *text)
{
    char p[700];
    snprintf(p, sizeof p, "%s/%s", root, name);
    FILE *f = fopen(p, "w");
    fputs(text, f);
    fclose(f);
}

static void clear(void)
{
    char p[700];
    snprintf(p, sizeof p, "%s/trying", root);
    remove(p);
    snprintf(p, sizeof p, "%s/last_start", root);
    remove(p);
    snprintf(p, sizeof p, "%s/abc123.status", root);
    remove(p);
    snprintf(p, sizeof p, "%s/myid.status", root);
    remove(p);
}

/* The fake wrapper opens the driver: the marker is written, n64_vk_driver_done gives the outcome. */
static int wrapper_tests(void)
{
    char line[256], lib[700];
    snprintf(lib, sizeof lib, "%s/vulkan.test.so", drv);
    FILE *f = fopen(lib, "w");
    fputs("x", f);
    fclose(f);
    setenv("RECOMP_VK_DRIVER_DIR", drv, 1);
    setenv("RECOMP_VK_DRIVER_LIB", "vulkan.test.so", 1);
    setenv("RECOMP_VK_DRIVER_ID", "myid", 1);
    static const char *outcomes[] = { "ok", "unsupported", "not_loaded" };
    for (int i = 0; i < 3; i++) {
        void *gipa = n64_vk_driver_open();
        CHECK(gipa != NULL);
        if (!gipa)
            break;
        CHECK(read_line("trying", line, sizeof line) && !strcmp(line, "myid"));
        n64_vk_driver_done(outcomes[i]);
        char want[64];
        snprintf(want, sizeof want, "myid %s", outcomes[i]);
        CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, want));
        CHECK(read_line("myid.status", line, sizeof line) && !strcmp(line, outcomes[i]));
        CHECK(!read_line("trying", line, sizeof line));
        n64_vk_driver_done("ok"); /* once only */
        CHECK(read_line("myid.status", line, sizeof line) && !strcmp(line, outcomes[i]));
    }
    /* another driver's start afterwards: its own status file, myid's stays (the app reads both) */
    setenv("RECOMP_VK_DRIVER_ID", "abc123", 1);
    CHECK(n64_vk_driver_open() != NULL);
    n64_vk_driver_done("unsupported");
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "abc123 unsupported"));
    CHECK(read_line("abc123.status", line, sizeof line) && !strcmp(line, "unsupported"));
    CHECK(read_line("myid.status", line, sizeof line) && !strcmp(line, "not_loaded"));
    /* an id that cannot name a file: last_start only */
    setenv("RECOMP_VK_DRIVER_ID", "../x", 1);
    CHECK(n64_vk_driver_open() != NULL);
    n64_vk_driver_done("ok");
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "../x ok"));
    char p[700];
    snprintf(p, sizeof p, "%s/../x.status", root);
    CHECK(access(p, F_OK) != 0);
    clear();
    remove(lib);
    return 0;
}

int main(int argc, char **argv)
{
    const int wrapper = argc > 1 && !strcmp(argv[1], "--wrapper");
    char line[256];
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/n64vkdXXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }
    char base[520];
    snprintf(base, sizeof base, "%s", root);
    snprintf(root, sizeof root, "%s/vk_drivers", base); /* the marker and last_start live here */
    mkdir(root, 0700);
    snprintf(drv, sizeof drv, "%s/abc123", root);
    mkdir(drv, 0700);
    unsetenv("RECOMP_VK_DRIVER");
    unsetenv("RECOMP_VK_DRIVER_DIR");
    unsetenv("RECOMP_VK_DRIVER_LIB");
    unsetenv("RECOMP_VK_DRIVER_ID");
    unsetenv("RECOMP_VK_HOOK_DIR");

    if (wrapper) {
        wrapper_tests();
        rmdir(drv);
        rmdir(root);
        rmdir(base);
        if (fails) {
            fprintf(stderr, "n64_vk_driver --wrapper: %d failure(s)\n", fails);
            return 1;
        }
        printf("n64_vk_driver --wrapper: ok\n");
        return 0;
    }

    /* nothing selected: no open, no file */
    CHECK(!n64_vk_driver_selected());
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(!read_line("last_start", line, sizeof line));

    /* selected, forced back to the system driver */
    setenv("RECOMP_VK_DRIVER_DIR", drv, 1); /* no trailing '/': added */
    setenv("RECOMP_VK_DRIVER_LIB", "vulkan.test.so", 1);
    CHECK(n64_vk_driver_selected());
    setenv("RECOMP_VK_DRIVER", "system", 1);
    CHECK(!n64_vk_driver_selected());
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(!read_line("last_start", line, sizeof line));
    unsetenv("RECOMP_VK_DRIVER");

    /* the library is missing: open_failed, id from the directory's name; its own status file too */
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(!strcmp(n64_vk_driver_id(), "abc123"));
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "abc123 open_failed"));
    CHECK(read_line("abc123.status", line, sizeof line) && !strcmp(line, "open_failed"));
    CHECK(!read_line("trying", line, sizeof line));
    clear();

    /* a library file name with a '/' is refused */
    setenv("RECOMP_VK_DRIVER_LIB", "../x.so", 1);
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "abc123 open_failed"));
    setenv("RECOMP_VK_DRIVER_LIB", "vulkan.test.so", 1);
    clear();

    /* the library is there, libxba_vkdriver.so is not (desktop): open_failed, the marker written then removed */
    char lib[700];
    snprintf(lib, sizeof lib, "%s/vulkan.test.so", drv);
    FILE *f = fopen(lib, "w");
    fputs("not an ELF", f);
    fclose(f);
    setenv("RECOMP_VK_DRIVER_ID", "myid", 1);
    char slash[700];
    snprintf(slash, sizeof slash, "%s/", drv);
    setenv("RECOMP_VK_DRIVER_DIR", slash, 1); /* with the trailing '/' */
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(!strcmp(n64_vk_driver_id(), "myid"));
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "myid open_failed"));
    CHECK(!read_line("trying", line, sizeof line));
    clear();

    /* a marker for another driver: ignored and removed */
    write_line("trying", "other\n");
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "myid open_failed"));
    CHECK(!read_line("trying", line, sizeof line));
    clear();

    /* the marker of a previous start of this driver (it crashed): skipped, the marker stays for the app */
    write_line("trying", "myid\n");
    CHECK(n64_vk_driver_open() == NULL);
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "myid crashed"));
    CHECK(read_line("myid.status", line, sizeof line) && !strcmp(line, "crashed"));
    CHECK(read_line("trying", line, sizeof line) && !strcmp(line, "myid"));
    n64_vk_driver_done("ok"); /* nothing was opened: no effect */
    CHECK(read_line("trying", line, sizeof line) && !strcmp(line, "myid"));
    CHECK(read_line("last_start", line, sizeof line) && !strcmp(line, "myid crashed"));
    clear();

    /* outside a vk_drivers directory: no marker, no last_start */
    char other[700], olib[760];
    snprintf(other, sizeof other, "%s/elsewhere", base);
    mkdir(other, 0700);
    snprintf(olib, sizeof olib, "%s/vulkan.test.so", other);
    f = fopen(olib, "w");
    fputs("x", f);
    fclose(f);
    setenv("RECOMP_VK_DRIVER_DIR", other, 1);
    CHECK(n64_vk_driver_open() == NULL);
    char p2[760];
    snprintf(p2, sizeof p2, "%s/last_start", base);
    CHECK(access(p2, F_OK) != 0);
    snprintf(p2, sizeof p2, "%s/trying", base);
    CHECK(access(p2, F_OK) != 0);
    remove(olib);
    rmdir(other);

    remove(lib);
    rmdir(drv);
    rmdir(root);
    rmdir(base);
    if (fails) {
        fprintf(stderr, "n64_vk_driver: %d failure(s)\n", fails);
        return 1;
    }
    printf("n64_vk_driver: ok\n");
    return 0;
}
