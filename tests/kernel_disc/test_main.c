/* kernel_disc (XBA-126): D: served straight from a disc image by the real POSIX file layer (kernel_file.c,
 * kernel_path.c, kernel_disc.c), compared with the same tree as a folder.
 *
 *   kernel_disc_test FIXTURES_DIR      (make_fixtures.py's output, written at configure time)
 *
 * One scenario of guest calls (opens by every device path, reads with and without offsets, positions, sizes,
 * attributes, directory searches with patterns, relative opens, the missing-file statuses and Win32 codes) is run
 * against the folder and against each image (bare, redump layout, CSO v1 and v2, split, split CSO, an fd), and
 * every image's transcript must equal the folder's. Then what only an image does: read-only answers, raw reads of
 * the DVD device (NtReadFile and SCSI READ), the boot's XBE load (xbox_disc_slurp), saves still written, and four
 * threads reading through one compressed image at once. */
#define _GNU_SOURCE
#include "kernel.h"
#include "kernel_disc.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{ (void)level; (void)subsystem; (void)fmt; }
const char *xbox_LookupSymbolicLink(const char *link)
{ (void)link; return NULL; }

extern uint32_t xbox_LastFileError(void);

static int g_fail;
static char g_fx[1024], g_save[1100];

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

/* ── transcript ── */

static char *g_tr;
static size_t g_tr_len, g_tr_cap;

static void say(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (g_tr_len + (size_t)n + 2 > g_tr_cap) {
        g_tr_cap = (g_tr_cap + (size_t)n + 2) * 2;
        g_tr = realloc(g_tr, g_tr_cap);
    }
    memcpy(g_tr + g_tr_len, line, (size_t)n);
    g_tr_len += (size_t)n;
    g_tr[g_tr_len++] = '\n';
    g_tr[g_tr_len] = 0;
}

static uint32_t fnv(const void *p, size_t n, uint32_t h)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x01000193u;
    return h;
}

/* ── guest calls ── */

static NTSTATUS open_at(HANDLE root, const char *path, ACCESS_MASK access, ULONG disp, ULONG options, HANDLE *h)
{
    XBOX_ANSI_STRING name = { (USHORT)strlen(path), (USHORT)(strlen(path) + 1), (PCHAR)path };
    XBOX_OBJECT_ATTRIBUTES oa = { 0 };
    XBOX_IO_STATUS_BLOCK ios = { 0 };
    oa.ObjectName = &name;
    oa.RootDirectory = root;
    oa.Attributes = 0x40;
    *h = NULL;
    return xbox_NtCreateFile(h, access, &oa, &ios, NULL, 0x80, 1, disp, options);
}

static NTSTATUS open_ro(const char *path, ULONG options, HANDLE *h)
{
    return open_at(NULL, path, 0x80000000u /* GENERIC_READ */, XBOX_FILE_OPEN, options, h);
}

static LONGLONG size_of(HANDLE h)
{
    XBOX_FILE_STANDARD_INFORMATION si;
    XBOX_IO_STATUS_BLOCK ios = { 0 };
    if (xbox_NtQueryInformationFile(h, &ios, &si, sizeof si, XboxFileStandardInformation) != 0)
        return -1;
    return si.EndOfFile.QuadPart * 2 + (si.Directory ? 1 : 0);
}

static void read_whole(const char *path, size_t chunk)
{
    HANDLE h;
    NTSTATUS st = open_ro(path, XBOX_FILE_NON_DIRECTORY_FILE, &h);
    say("open %s -> 0x%08X", path, (unsigned)st);
    if (st)
        return;
    uint8_t *buf = malloc(chunk);
    uint32_t hash = 0x811C9DC5u;
    uint64_t total = 0;
    for (;;) {
        XBOX_IO_STATUS_BLOCK ios = { 0 };
        st = xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, (ULONG)chunk, NULL);
        if (st)
            break;
        hash = fnv(buf, ios.Information, hash);
        total += ios.Information;
    }
    say("  read %llu bytes hash %08X, then 0x%08X; size %lld", (unsigned long long)total, hash, (unsigned)st,
        (long long)size_of(h));
    free(buf);
    xbox_NtClose(h);
}

static void read_at(const char *path, LONGLONG off, ULONG len)
{
    HANDLE h;
    NTSTATUS st = open_ro(path, 0, &h);
    say("open %s -> 0x%08X", path, (unsigned)st);
    if (st)
        return;
    uint8_t *buf = calloc(1, len + 1);
    XBOX_IO_STATUS_BLOCK ios = { 0 };
    LARGE_INTEGER o;
    o.QuadPart = off;
    st = xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, len, &o);
    say("  at %lld: 0x%08X, %lu bytes, hash %08X", (long long)off, (unsigned)st, (unsigned long)ios.Information,
        fnv(buf, ios.Information, 0x811C9DC5u));
    /* The position follows the read; set it, read again without an offset. */
    XBOX_FILE_POSITION_INFORMATION pi;
    memset(&ios, 0, sizeof ios);
    xbox_NtQueryInformationFile(h, &ios, &pi, sizeof pi, XboxFilePositionInformation);
    say("  position %lld", (long long)pi.CurrentByteOffset.QuadPart);
    pi.CurrentByteOffset.QuadPart = 7;
    st = xbox_NtSetInformationFile(h, &ios, &pi, sizeof pi, XboxFilePositionInformation);
    memset(&ios, 0, sizeof ios);
    NTSTATUS st2 = xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, 5, NULL);
    say("  set 7: 0x%08X, read 0x%08X %lu bytes hash %08X", (unsigned)st, (unsigned)st2,
        (unsigned long)ios.Information, fnv(buf, ios.Information, 0x811C9DC5u));
    free(buf);
    xbox_NtClose(h);
}

static void missing(const char *path, ULONG options)
{
    HANDLE h;
    NTSTATUS st = open_ro(path, options, &h);
    say("open %s opts 0x%X -> 0x%08X err %u", path, (unsigned)options, (unsigned)st,
        st ? xbox_LastFileError() : 0u);
    if (!st)
        xbox_NtClose(h);
}

static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Lists a directory with a pattern; the names are sorted (a folder's order is the host's). */
static void list(const char *dir, const char *pattern)
{
    HANDLE h;
    NTSTATUS st = open_ro(dir, XBOX_FILE_DIRECTORY_FILE, &h);
    say("dir %s -> 0x%08X", dir, (unsigned)st);
    if (st)
        return;
    char *names[64];
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {                  /* a second search after NO_MORE_FILES starts again */
        int count = 0;
        for (;;) {
            union { uint64_t a; unsigned char b[600]; } buf;
            XBOX_IO_STATUS_BLOCK ios = { 0 };
            XBOX_ANSI_STRING mask = { (USHORT)strlen(pattern), (USHORT)(strlen(pattern) + 1), (PCHAR)pattern };
            st = xbox_NtQueryDirectoryFile(h, NULL, NULL, NULL, &ios, buf.b, sizeof buf.b,
                                           XboxFileDirectoryInformation, &mask, FALSE);
            if (st)
                break;
            PXBOX_FILE_DIRECTORY_INFORMATION e = (void *)buf.b;
            char line[300];
            /* A directory's size is the host's (a folder) or 0 (an image): not compared. */
            snprintf(line, sizeof line, "%.*s size %lld attr 0x%X", (int)e->FileNameLength, e->FileName,
                     (e->FileAttributes & XBOX_FILE_ATTRIBUTE_DIRECTORY) ? 0LL : (long long)e->EndOfFile.QuadPart,
                     (unsigned)e->FileAttributes);
            if (pass == 0 && n < 64)
                names[n++] = strdup(line);
            count++;
        }
        say("  pass %d: %d entries, then 0x%08X", pass, count, (unsigned)st);
    }
    qsort(names, (size_t)n, sizeof names[0], name_cmp);
    for (int i = 0; i < n; i++) {
        say("  %s", names[i]);
        free(names[i]);
    }
    xbox_NtClose(h);
}

static void attrs(const char *path)
{
    XBOX_ANSI_STRING name = { (USHORT)strlen(path), (USHORT)(strlen(path) + 1), (PCHAR)path };
    XBOX_OBJECT_ATTRIBUTES oa = { 0 };
    XBOX_FILE_NETWORK_OPEN_INFORMATION ni;
    oa.ObjectName = &name;
    memset(&ni, 0, sizeof ni);
    NTSTATUS st = xbox_NtQueryFullAttributesFile(&oa, &ni);
    say("attrs %s -> 0x%08X size %lld attr 0x%X", path, (unsigned)st,
        (ni.FileAttributes & XBOX_FILE_ATTRIBUTE_DIRECTORY) ? 0LL : (long long)ni.EndOfFile.QuadPart,
        (unsigned)ni.FileAttributes);
}

static void relative(void)
{
    HANDLE dir, h;
    NTSTATUS st = open_ro("D:\\media", XBOX_FILE_DIRECTORY_FILE, &dir);
    say("open D:\\media -> 0x%08X", (unsigned)st);
    if (st)
        return;
    st = open_at(dir, "sub\\leaf.txt", 0x80000000u, XBOX_FILE_OPEN, 0, &h);
    say("  relative sub\\leaf.txt -> 0x%08X size %lld", (unsigned)st, st ? -1LL : (long long)size_of(h));
    if (!st)
        xbox_NtClose(h);
    st = open_at(dir, "nope.txt", 0x80000000u, XBOX_FILE_OPEN, 0, &h);
    say("  relative nope.txt -> 0x%08X", (unsigned)st);
    xbox_NtClose(dir);
}

static void scenario(void)
{
    g_tr_len = 0;
    if (g_tr)
        g_tr[0] = 0;
    read_whole("D:\\default.xbe", 1000);
    read_whole("d:\\MEDIA\\intro.BIK", 65536);
    read_whole("\\Device\\CdRom0\\Data\\big.pak", 300000);
    read_whole("\\??\\D:\\media\\SUB\\leaf.txt", 1);
    read_whole("D:\\media\\empty.bin", 100);
    read_at("D:\\Data\\big.pak", 1024 * 1024 - 100, 4096);         /* crosses a split cut */
    read_at("D:\\media\\outro.bik", 4000, 4096);                    /* clamped at the end */
    read_at("D:\\media\\outro.bik", 9000, 10);                      /* past it */
    missing("D:\\nope.bin", 0);
    missing("D:\\nodir\\x.bin", 0);
    missing("D:\\media\\Intro.bik\\x", 0);                          /* a file used as a directory */
    missing("D:\\media", XBOX_FILE_NON_DIRECTORY_FILE);
    missing("D:\\emptydir", XBOX_FILE_DIRECTORY_FILE);              /* opens */
    list("D:\\", "*");
    list("D:\\media", "*.bik");
    list("D:\\Data\\levels", "L?.LVL");
    list("D:\\emptydir", "*");
    attrs("D:\\media\\Intro.bik");
    attrs("D:\\Data");
    attrs("D:\\missing");
    relative();
}

/* ── image-only behaviour ── */

static void read_only(void)
{
    HANDLE h;
    /* The one deliberate difference with a folder: a file opened as a directory fails (the folder backend opens
     * the file's directory instead, a rule meant for the partition device images). */
    CHECK(open_ro("D:\\media\\outro.bik", XBOX_FILE_DIRECTORY_FILE, &h) == STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(open_at(NULL, "D:\\media\\outro.bik", 0xC0000000u, XBOX_FILE_OPEN, 0, &h) == STATUS_ACCESS_DENIED);
    CHECK(xbox_LastFileError() == 5);
    CHECK(open_at(NULL, "D:\\new.bin", 0xC0000000u, XBOX_FILE_CREATE, 0, &h) == STATUS_ACCESS_DENIED);
    CHECK(open_at(NULL, "D:\\new.bin", 0x80000000u, XBOX_FILE_OPEN_IF, 0, &h) == STATUS_ACCESS_DENIED);
    CHECK(open_at(NULL, "D:\\media\\outro.bik", 0x80000000u, XBOX_FILE_OVERWRITE_IF, 0, &h) == STATUS_ACCESS_DENIED);
    CHECK(open_at(NULL, "D:\\media\\outro.bik", 0x80000000u, XBOX_FILE_OPEN_IF, 0, &h) == 0);   /* exists: opens */
    if (h) {
        XBOX_IO_STATUS_BLOCK ios = { 0 };
        XBOX_FILE_END_OF_FILE_INFORMATION eof = { { { 0 } } };
        CHECK(xbox_NtWriteFile(h, NULL, NULL, NULL, &ios, "x", 1, NULL) == STATUS_ACCESS_DENIED);
        CHECK(xbox_NtSetInformationFile(h, &ios, &eof, sizeof eof, XboxFileEndOfFileInformation)
              == STATUS_ACCESS_DENIED);
        XBOX_FILE_FS_SIZE_INFORMATION fs;
        CHECK(xbox_NtQueryVolumeInformationFile(h, &ios, &fs, sizeof fs, XboxFileFsSizeInformation) == 0);
        CHECK(fs.AvailableAllocationUnits.QuadPart == 0 && fs.TotalAllocationUnits.QuadPart > 0);
        xbox_NtClose(h);
    }
    XBOX_ANSI_STRING name = { 16, 17, "D:\\media\\outro.bik" };
    name.Length = (USHORT)strlen(name.Buffer);
    XBOX_OBJECT_ATTRIBUTES oa = { 0 };
    oa.ObjectName = &name;
    CHECK(xbox_NtDeleteFile(&oa) == STATUS_ACCESS_DENIED);
    /* The saves are not on the disc: T:\ writes as before. */
    CHECK(open_at(NULL, "T:\\save.bin", 0xC0000000u, XBOX_FILE_OVERWRITE_IF, 0, &h) == 0);
    if (h) {
        XBOX_IO_STATUS_BLOCK ios = { 0 };
        CHECK(xbox_NtWriteFile(h, NULL, NULL, NULL, &ios, "save", 4, NULL) == 0 && ios.Information == 4);
        xbox_NtClose(h);
    }
}

static void device_reads(void)
{
    uint8_t buf[4096];
    XBOX_IO_STATUS_BLOCK ios = { 0 };
    LARGE_INTEGER o;
    o.QuadPart = 32 * 2048;                                         /* the volume descriptor */
    memset(buf, 0, sizeof buf);
    CHECK(xbox_NtReadFile((HANDLE)(uintptr_t)XBOX_DISC_DEVICE_HANDLE, NULL, NULL, NULL, &ios, buf, 2048, &o) == 0);
    CHECK(ios.Information == 2048 && memcmp(buf, "MICROSOFT*XBOX*MEDIA", 20) == 0);
    uint8_t cdb[16] = { 0x28, 0, 0, 0, 0, 32, 0, 0, 1 };           /* READ(10) sector 32, one sector */
    memset(buf, 0, sizeof buf);
    CHECK(xbox_disc_scsi(cdb, buf, 2048) == 1 && memcmp(buf, "MICROSOFT*XBOX*MEDIA", 20) == 0);
    uint8_t cap[16] = { 0x25 };
    CHECK(xbox_disc_scsi(cap, buf, 8) == 1);
    uint32_t last = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3];
    CHECK((uint64_t)(last + 1) * 2048 <= xbox_disc_partition_bytes() && buf[6] == 0x08);
    uint8_t other[16] = { 0x12 };                                   /* INQUIRY: not ours */
    CHECK(xbox_disc_scsi(other, buf, 36) == 0);
}

static void slurp(const char *game)
{
    char path[1200];
    void *data = NULL;
    size_t size = 0;
    snprintf(path, sizeof path, "%s/default.xbe", game);
    CHECK(xbox_disc_slurp(path, &data, &size) == 0 && size > 0x200 && memcmp(data, "XBEH", 4) == 0);
    free(data);
    snprintf(path, sizeof path, "%s/missing.xbe", game);
    CHECK(xbox_disc_slurp(path, &data, &size) == -1);
}

typedef struct { const char *path; uint32_t want; int bad; } job_t;

static void *reader(void *arg)
{
    job_t *j = arg;
    for (int k = 0; k < 20; k++) {
        HANDLE h;
        if (open_ro(j->path, 0, &h)) {
            j->bad++;
            continue;
        }
        uint8_t buf[70000];
        uint32_t hash = 0x811C9DC5u;
        for (;;) {
            XBOX_IO_STATUS_BLOCK ios = { 0 };
            if (xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, sizeof buf, NULL))
                break;
            hash = fnv(buf, ios.Information, hash);
        }
        if (hash != j->want)
            j->bad++;
        xbox_NtClose(h);
    }
    return NULL;
}

static uint32_t hash_host(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t buf[65536];
    size_t n;
    uint32_t h = 0x811C9DC5u;
    while (f && (n = fread(buf, 1, sizeof buf, f)) > 0)
        h = fnv(buf, n, h);
    if (f)
        fclose(f);
    return h;
}

static void threads(void)
{
    char a[1200], b[1200], c[1200];
    snprintf(a, sizeof a, "%s/game/Data/big.pak", g_fx);
    snprintf(b, sizeof b, "%s/game/media/Intro.bik", g_fx);
    snprintf(c, sizeof c, "%s/game/Data/levels/l2.lvl", g_fx);
    job_t jobs[4] = { { "D:\\Data\\big.pak", hash_host(a), 0 }, { "D:\\media\\intro.bik", hash_host(b), 0 },
                      { "D:\\data\\LEVELS\\l2.lvl", hash_host(c), 0 }, { "D:\\Data\\big.pak", hash_host(a), 0 } };
    pthread_t t[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, reader, &jobs[i]);
    for (int i = 0; i < 4; i++) {
        pthread_join(t[i], NULL);
        if (jobs[i].bad) {
            fprintf(stderr, "FAIL thread %d: %d reads of %s differ\n", i, jobs[i].bad, jobs[i].path);
            g_fail++;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: kernel_disc_test FIXTURES_DIR\n");
        return 2;
    }
    if (!realpath(argv[1], g_fx)) {
        perror(argv[1]);
        return 2;
    }
    char tmpl[] = "/tmp/kernel_disc_save.XXXXXX";
    if (!mkdtemp(tmpl))
        return 2;
    snprintf(g_save, sizeof g_save, "%s", tmpl);

    char folder[1200];
    snprintf(folder, sizeof folder, "%s/game", g_fx);
    xbox_path_init(folder, g_save);
    CHECK(!xbox_disc_mounted());
    scenario();
    char *want = strdup(g_tr ? g_tr : "");
    printf("folder transcript: %zu bytes\n", strlen(want));

    static const char *images[] = { "game.iso", "game_redump.iso", "game_v1.cso", "game_v2.cso", "game_split.1.iso",
                                    "game_split.3.iso", "game_cs.2.cso", "fd" };
    for (size_t i = 0; i < sizeof images / sizeof images[0]; i++) {
        char spec[1200];
        int fd = -1;
        if (!strcmp(images[i], "fd")) {
            char p[1200];
            snprintf(p, sizeof p, "%s/game_v2.cso", g_fx);
            fd = open(p, O_RDONLY);
            snprintf(spec, sizeof spec, "fd:%d", fd);
        } else {
            snprintf(spec, sizeof spec, "%s/%s", g_fx, images[i]);
        }
        xbox_path_init(spec, g_save);
        if (!xbox_disc_mounted()) {
            fprintf(stderr, "FAIL %s: not mounted\n", spec);
            g_fail++;
            continue;
        }
        scenario();
        if (strcmp(want, g_tr) != 0) {
            fprintf(stderr, "FAIL %s: transcript differs from the folder's\n--- folder\n%s--- image\n%s", images[i],
                    want, g_tr);
            g_fail++;
        } else {
            printf("%s (%s): same as the folder\n", images[i], xbox_disc_kind());
        }
        read_only();
        device_reads();
        if (fd < 0)
            slurp(spec);
        if (!strcmp(images[i], "game_v2.cso"))
            threads();
    }

    /* The boot's XBE load before xbox_path_init: an image met on the way is mounted. */
    xbox_disc_unmount();
    {
        char link[1200], target[1200], path[1300];
        void *data = NULL;
        size_t size = 0;
        snprintf(link, sizeof link, "%s/game_link", g_save);
        snprintf(target, sizeof target, "%s/game_split.2.iso", g_fx);
        CHECK(symlink(target, link) == 0);                          /* build/game -> one part of a split */
        snprintf(path, sizeof path, "%s/default.xbe", link);
        CHECK(xbox_disc_slurp(path, &data, &size) == 0 && size > 0 && memcmp(data, "XBEH", 4) == 0);
        CHECK(xbox_disc_mounted() && strcmp(xbox_disc_kind(), "iso split") == 0);
        free(data);
        unlink(link);
    }
    {   /* RECOMP_DISC_IMAGE=0 turns images off */
        char spec[1200];
        snprintf(spec, sizeof spec, "%s/game.iso", g_fx);
        xbox_disc_unmount();
        setenv("RECOMP_DISC_IMAGE", "0", 1);
        CHECK(xbox_disc_mount(spec) != 0 && !xbox_disc_mounted());
        unsetenv("RECOMP_DISC_IMAGE");
        CHECK(xbox_disc_mount(spec) == 0);
    }

    free(want);
    free(g_tr);
    char cmd[1300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_save);
    if (system(cmd) != 0) { /* best effort */ }
    printf(g_fail ? "%d failure(s)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
