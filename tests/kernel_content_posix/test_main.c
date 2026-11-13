/* Downloadable content and title updates on the save side (XBA-124).
 *
 * On the console XAPI mounts T: on \Device\Harddisk0\Partition1\TDATA\<title
 * id>; content packages are folders under T:\$c and a title update is
 * T:\$u\default.xbe. XAPI's XFindFirstContent enumerates T:\$c through
 * NtOpenFile + NtQueryDirectoryFile, both lifted into the title. Here the
 * TDATA rule maps that to <user root>/TDATA/<title id>. What is pinned:
 *
 *   1. no $c folder (the case of every player: nothing is installed): the
 *      open fails with a not-found status, which XAPI turns into
 *      INVALID_HANDLE_VALUE / ERROR_PATH_NOT_FOUND -- "no content", cleanly;
 *   2. an empty $c: the open succeeds and the first query says no file,
 *      never a success with an empty entry;
 *   3. a package folder the owner put there is listed (what XContent would
 *      then open is the title's own business, not tested here);
 *   4. T:\$u\default.xbe opens like any file: the runtime never launches it
 *      (the disc's default.xbe runs; tools.triage reports the update).
 * No title, no runtime init: the real path and file backends. */
#define _GNU_SOURCE
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

#include <stdarg.h>
static char g_said[2048];          /* every log line, for the start-up report check */
void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{
    char line[700];
    va_list ap;
    (void)level; (void)subsystem;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strlen(g_said) + strlen(line) + 2 < sizeof g_said) {
        strcat(g_said, line);
        strcat(g_said, "\n");
    }
}
extern void xbox_path_set_title_id(uint32_t title_id);

/* XAPI's mount of T: (IoCreateSymbolicLink at start-up). */
const char *xbox_LookupSymbolicLink(const char *link)
{
    if (link && !strcmp(link, "\\??\\T:"))
        return "\\Device\\Harddisk0\\Partition1\\TDATA\\58424124";
    return NULL;
}

static int failures;
static void check(int ok, const char *name)
{
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static NTSTATUS open_guest(const char *path, ULONG options, HANDLE *h)
{
    XBOX_ANSI_STRING name = {(USHORT)strlen(path), (USHORT)(strlen(path) + 1), (PCHAR)path};
    XBOX_OBJECT_ATTRIBUTES oa = {0};
    XBOX_IO_STATUS_BLOCK ios = {0};
    oa.ObjectName = &name; oa.Attributes = 0x40; *h = NULL;
    return xbox_NtCreateFile(h, 0x80000000u, &oa, &ios, NULL, 0x80, 0, XBOX_FILE_OPEN, options);
}

/* The first entry of a "*" query: its status, its name in name[]. */
static NTSTATUS first_entry(HANDLE h, char *name, size_t n)
{
    union { uint64_t alignment; unsigned char bytes[512]; } buf = {0};
    XBOX_IO_STATUS_BLOCK ios = {0};
    XBOX_ANSI_STRING mask = {1, 2, "*"};
    NTSTATUS st = xbox_NtQueryDirectoryFile(h, NULL, NULL, NULL, &ios, buf.bytes, sizeof buf.bytes,
                                            XboxFileDirectoryInformation, &mask, TRUE);
    PXBOX_FILE_DIRECTORY_INFORMATION e = (void *)buf.bytes;
    name[0] = 0;
    if (st == STATUS_SUCCESS && e->FileNameLength < n) {
        memcpy(name, e->FileName, e->FileNameLength);
        name[e->FileNameLength] = 0;
    }
    return st;
}

static int not_found(NTSTATUS st)
{
    return st == STATUS_OBJECT_NAME_NOT_FOUND || st == STATUS_OBJECT_PATH_NOT_FOUND;
}

int main(void)
{
    char root[] = "/tmp/xbox-content-XXXXXX", p[600], name[256];
    HANDLE h;
    NTSTATUS st;

    if (!mkdtemp(root))
        return 2;
    xbox_path_init(root, root);

    st = open_guest("T:\\$c", XBOX_FILE_DIRECTORY_FILE, &h);
    printf("open T:\\$c with nothing installed: 0x%08X\n", (unsigned)st);
    check(not_found(st) && !h, "no $c folder: the open says not found, no handle");
    st = open_guest("T:\\$u\\default.xbe", 0, &h);
    check(not_found(st) && !h, "no update: T:\\$u\\default.xbe is not found");

    snprintf(p, sizeof p, "%s/TDATA", root);
    mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/TDATA/58424124", root);
    mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/TDATA/58424124/$c", root);
    if (mkdir(p, 0700) != 0)
        return 2;
    st = open_guest("T:\\$c", XBOX_FILE_DIRECTORY_FILE, &h);
    check(st == STATUS_SUCCESS && h, "an empty $c opens");
    if (h) {
        st = first_entry(h, name, sizeof name);
        printf("first entry of an empty $c: 0x%08X\n", (unsigned)st);
        check(st == STATUS_NO_SUCH_FILE || st == STATUS_NO_MORE_FILES, "an empty $c lists nothing");
        xbox_NtClose(h);
    }

    snprintf(p, sizeof p, "%s/TDATA/58424124/$c/0000000000000001", root);
    check(mkdir(p, 0700) == 0, "a content package folder made");
    st = open_guest("T:\\$c", XBOX_FILE_DIRECTORY_FILE, &h);
    if (h) {
        st = first_entry(h, name, sizeof name);
        check(st == STATUS_SUCCESS && !strcmp(name, "0000000000000001"), "the package is listed");
        xbox_NtClose(h);
    } else {
        check(0, "the package is listed");
    }

    snprintf(p, sizeof p, "%s/TDATA/58424124/$u", root);
    check(mkdir(p, 0700) == 0, "an update folder made");
    snprintf(p, sizeof p, "%s/TDATA/58424124/$u/default.xbe", root);
    {
        FILE *f = fopen(p, "wb");
        if (!f)
            return 2;
        fputs("XBEH", f);
        fclose(f);
    }
    st = open_guest("T:\\$u\\default.xbe", 0, &h);
    check(st == STATUS_SUCCESS && h, "T:\\$u\\default.xbe opens as a file (never launched by the runtime)");
    if (h)
        xbox_NtClose(h);

    /* 5. At start-up (path init, after the layout set the title ID) the
     * update and the packages are reported, not silently ignored. */
    g_said[0] = 0;
    xbox_path_set_title_id(0x58424124u);
    xbox_path_init(root, root);
    check(strstr(g_said, "a title update for 58424124, not used") != NULL, "the update is reported at start-up");
    check(strstr(g_said, "1 content package(s)") != NULL, "the content packages are counted at start-up");
    g_said[0] = 0;
    xbox_path_set_title_id(0x58424125u);
    xbox_path_init(root, root);
    check(strstr(g_said, "title update") == NULL, "another title's update is not this one's");

    {
        char cmd[700];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
        if (system(cmd) != 0)
            fprintf(stderr, "could not remove %s\n", root);
    }
    return failures ? 1 : 0;
}
