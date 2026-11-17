/* src/xiso (XBA-40): probe and extract synthetic disc images, byte for byte, plus cancel and corrupt images, and
 * the single-file API of the game library (xiso_open_fd / xiso_lookup / xiso_pread / xiso_extract_file_fd), and
 * (XBA-126) split and compressed sources: every format extracted and compared, random reads against the plain image
 * from several threads, xiso_list, broken compressed images.
 *
 *   xiso_test IMAGE_OR_MANIFEST...
 *
 * The files come from make_fixtures.py (run by CMake at configure time) and are found by base name among the
 * arguments, so the device bundle (tools/android/bundle_tests.py) copies them. Contents are regenerated here from
 * each path (make_fixtures.py: FNV-1a seed, xorshift32 stream). Work files go to $TMPDIR (or /tmp).
 */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE 1
#include "xiso.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_argc;
static char **g_argv;
static int g_fail;
static char g_tmp[4096];

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_RC(got, want) do { int g_ = (got), w_ = (want); if (g_ != w_) { \
    fprintf(stderr, "FAIL %s:%d: %s = %d (%s), want %d (%s)\n", __FILE__, __LINE__, #got, g_, xiso_strerror(g_), \
            w_, xiso_strerror(w_)); g_fail++; } } while (0)

static const char *fixture(const char *name)
{
    size_t n = strlen(name);
    for (int i = 1; i < g_argc; i++) {
        size_t l = strlen(g_argv[i]);
        if (l >= n && strcmp(g_argv[i] + l - n, name) == 0 && (l == n || g_argv[i][l - n - 1] == '/'))
            return g_argv[i];
    }
    fprintf(stderr, "missing fixture %s on the command line\n", name);
    exit(1);
}

static int open_ro(const char *p)
{
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror(p);
        exit(1);
    }
    return fd;
}

static char *tmp_path(const char *name)
{
    static char buf[8][4600];
    static int k;
    char *b = buf[k++ & 7];
    snprintf(b, sizeof buf[0], "%s/%s", g_tmp, name);
    return b;
}

static int rm_cb(const char *p, const struct stat *s, int f, struct FTW *w) { (void)s; (void)f; (void)w; remove(p); return 0; }
static void rm_rf(const char *p) { struct stat st; if (lstat(p, &st) == 0) nftw(p, rm_cb, 16, FTW_DEPTH | FTW_PHYS); }
static int exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }

static int g_count;
static int count_cb(const char *p, const struct stat *s, int f, struct FTW *w) { (void)p; (void)s; (void)f; if (w->level > 0) g_count++; return 0; }
static int count_entries(const char *dir) { g_count = 0; if (nftw(dir, count_cb, 16, FTW_PHYS) != 0) return -1; return g_count; }

static uint8_t *read_file(const char *p, size_t *len)
{
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    fstat(fd, &st);
    uint8_t *b = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, b + got, (size_t)st.st_size - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    *len = got;
    return b;
}

static void write_file(const char *p, const uint8_t *b, size_t len)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 || write(fd, b, len) != (ssize_t)len) {
        perror(p);
        exit(1);
    }
    close(fd);
}

/* make_fixtures.py's content stream */
static uint8_t *expected(const char *path, size_t size)
{
    uint32_t x = 0x811C9DC5u;
    for (const char *c = path; *c; c++)
        x = (x ^ (uint8_t)*c) * 0x01000193u;
    if (!x)
        x = 1;
    uint8_t *b = malloc(size + 4);
    for (size_t i = 0; i < size; i += 4) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b[i] = (uint8_t)x; b[i + 1] = (uint8_t)(x >> 8); b[i + 2] = (uint8_t)(x >> 16); b[i + 3] = (uint8_t)(x >> 24);
    }
    return b;
}

/* Compares out_dir with a manifest: every file byte for byte, every directory, and nothing else. Returns the
 * total file size and sets *n_entries. */
static uint64_t verify_tree(const char *out_dir, const char *manifest, int *n_entries)
{
    FILE *m = fopen(manifest, "r");
    char line[1024];
    uint64_t total = 0;
    int n = 0;
    size_t xlen;
    uint8_t *xbe = read_file(fixture("default.xbe.ref"), &xlen);
    CHECK(m && xbe);
    while (m && fgets(line, sizeof line, m)) {
        line[strcspn(line, "\n")] = 0;
        char kind = line[0], path[1024], full[4600];
        unsigned long size = 0;
        if (kind == 'd') {
            snprintf(path, sizeof path, "%s", line + 2);
        } else if (sscanf(line + 2, "%lu %1023s", &size, path) != 2) {
            continue;
        }
        snprintf(full, sizeof full, "%s/%s", out_dir, path);
        n++;
        struct stat st;
        if (lstat(full, &st) != 0) {
            fprintf(stderr, "FAIL missing %s\n", full);
            g_fail++;
            continue;
        }
        if (kind == 'd') {
            CHECK(S_ISDIR(st.st_mode));
            continue;
        }
        total += size;
        size_t len = 0;
        uint8_t *got = read_file(full, &len);
        uint8_t *want = kind == 'x' ? xbe : expected(path, size);
        if (!got || len != size || (kind == 'x' && len != xlen) || memcmp(got, want, size) != 0) {
            fprintf(stderr, "FAIL content of %s (%zu bytes, want %lu)\n", full, len, size);
            g_fail++;
        }
        free(got);
        if (want != xbe)
            free(want);
    }
    if (m)
        fclose(m);
    free(xbe);
    int found = count_entries(out_dir);
    if (found != n) {
        fprintf(stderr, "FAIL %s holds %d entries, the manifest %d\n", out_dir, found, n);
        g_fail++;
    }
    if (n_entries)
        *n_entries = n;
    return total;
}

static uint64_t manifest_total(const char *manifest)
{
    FILE *m = fopen(manifest, "r");
    char line[1024];
    uint64_t total = 0;
    unsigned long size;
    while (m && fgets(line, sizeof line, m))
        if (line[0] != 'd' && sscanf(line + 2, "%lu", &size) == 1)
            total += size;
    if (m)
        fclose(m);
    return total;
}

/* ── progress recording / cancelling ── */

typedef struct {
    int calls, cancel_at_call;               /* cancel when calls reaches it (0: never) */
    uint64_t cancel_at_bytes;                /* or when done >= it (0: never) */
    uint64_t last_done, total;
    int monotonic, final_seen, mid_file;
    char last_path[512];
} prog_t;

static int on_progress(uint64_t done, uint64_t total, const char *path, void *user)
{
    prog_t *p = user;
    p->calls++;
    if (done < p->last_done || done > total)
        p->monotonic = 0;
    if (p->calls > 1 && strcmp(path, p->last_path) == 0 && path[0])
        p->mid_file = 1;                     /* a second report inside the same file */
    if (!path[0])
        p->final_seen = (done == total);
    p->last_done = done;
    p->total = total;
    snprintf(p->last_path, sizeof p->last_path, "%s", path);
    if (p->cancel_at_call && p->calls >= p->cancel_at_call)
        return 1;
    if (p->cancel_at_bytes && done >= p->cancel_at_bytes)
        return 1;
    return 0;
}

/* ── tests ── */

static void test_probe(void)
{
    char title[64];
    uint64_t total = 1;
    int xbe = -1;
    int fd = open_ro(fixture("tree.iso"));
    CHECK_RC(xiso_probe_fd(fd, title, sizeof title, &total, &xbe), 0);
    CHECK(strcmp(title, "SYNTH") == 0);
    CHECK(xbe == 1);
    CHECK(total == manifest_total(fixture("tree.manifest")));
    CHECK_RC(xiso_probe_fd(fd, title, 3, NULL, NULL), 0);           /* truncated title */
    CHECK(strcmp(title, "SY") == 0);
    CHECK_RC(xiso_probe_fd(fd, title, 1, NULL, NULL), 0);
    CHECK(title[0] == 0);
    CHECK_RC(xiso_probe_fd(fd, NULL, 0, NULL, NULL), 0);
    close(fd);
    CHECK_RC(xiso_probe_fd(-1, title, sizeof title, &total, &xbe), XISO_EINVAL);
    CHECK_RC(xiso_extract_fd(-1, tmp_path("x"), NULL, NULL), XISO_EINVAL);
    fd = open_ro(fixture("tree.iso"));
    CHECK_RC(xiso_extract_fd(fd, "", NULL, NULL), XISO_EINVAL);
    close(fd);
    printf("probe: ok\n");
}

static void test_extract_tree(void)
{
    char *parent = tmp_path("deep");
    char *out = tmp_path("deep/er/game");
    rm_rf(parent);
    prog_t p = { .monotonic = 1 };
    int fd = open_ro(fixture("tree.iso"));
    CHECK_RC(xiso_extract_fd(fd, out, on_progress, &p), 0);
    close(fd);
    uint64_t total = manifest_total(fixture("tree.manifest"));
    CHECK(verify_tree(out, fixture("tree.manifest"), NULL) == total);
    CHECK(p.monotonic && p.final_seen && p.total == total);
    CHECK(p.mid_file);                       /* big.bin crosses the 4 MB step */
    CHECK(p.calls <= 400);                   /* one per file + a few, not one per chunk */
    CHECK(!exists(tmp_path("deep/er/game.partial")) && !exists(tmp_path("deep/er/game.old")));

    /* Again with another image: out_dir is replaced as a whole. */
    fd = open_ro(fixture("small.iso"));
    char *slash = tmp_path("deep/er/game/");  /* a trailing slash is the same directory */
    CHECK_RC(xiso_extract_fd(fd, slash, NULL, NULL), 0);
    close(fd);
    verify_tree(out, fixture("small.manifest"), NULL);
    CHECK(!exists(tmp_path("deep/er/game.partial")) && !exists(tmp_path("deep/er/game.old")));
    rm_rf(parent);
    printf("extract: ok\n");
}

static void extract_and_verify(const char *image_path, const char *what)
{
    char *out = tmp_path("based");
    rm_rf(out);
    char title[64];
    uint64_t total;
    int xbe;
    int fd = open_ro(image_path);
    CHECK_RC(xiso_probe_fd(fd, title, sizeof title, &total, &xbe), 0);
    CHECK(strcmp(title, "SYNTH") == 0 && xbe == 1 && total == manifest_total(fixture("small.manifest")));
    CHECK_RC(xiso_extract_fd(fd, out, NULL, NULL), 0);
    close(fd);
    verify_tree(out, fixture("small.manifest"), NULL);
    rm_rf(out);
    printf("%s: ok\n", what);
}

static void test_bases(void)
{
    extract_and_verify(fixture("small_base30600.iso"), "base 0x30600");

    /* Redump layout: the game partition at 0x18300000 behind the video partition, written sparse. */
    size_t len;
    uint8_t *img = read_file(fixture("small.iso"), &len);
    char *red = tmp_path("redump.iso");
    int fd = open(red, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    CHECK(fd >= 0 && img);
    if (fd >= 0 && img && pwrite(fd, img, len, (off_t)0x18300000) == (ssize_t)len) {
        close(fd);
        extract_and_verify(red, "base 0x18300000 (redump)");
    } else {
        if (fd >= 0)
            close(fd);
        fprintf(stderr, "FAIL cannot write the sparse redump image\n");
        g_fail++;
    }
    unlink(red);
    free(img);
}

static void test_cancel(void)
{
    char *out = tmp_path("cancel");
    char *marker = tmp_path("cancel/keep.txt");
    char *partial = tmp_path("cancel.partial");
    rm_rf(out);
    mkdir(out, 0755);
    write_file(marker, (const uint8_t *)"mine", 4);

    struct { int at_call; uint64_t at_bytes; } cases[] = {
        { 1, 0 },                            /* at the first file */
        { 0, 4ull << 20 },                   /* in the middle of big.bin */
        { 0, 0 },                            /* at the final report (cancel_at_call set below) */
    };
    for (int i = 0; i < 3; i++) {
        prog_t p = { .monotonic = 1, .cancel_at_call = cases[i].at_call, .cancel_at_bytes = cases[i].at_bytes };
        if (i == 2) {
            prog_t count = { .monotonic = 1 };
            int fd = open_ro(fixture("small.iso"));
            CHECK_RC(xiso_extract_fd(fd, tmp_path("count"), on_progress, &count), 0);
            close(fd);
            rm_rf(tmp_path("count"));
            p.cancel_at_call = count.calls;  /* the last call is the final one */
        }
        int fd = open_ro(fixture(i == 2 ? "small.iso" : "tree.iso"));
        CHECK_RC(xiso_extract_fd(fd, out, on_progress, &p), XISO_ECANCELED);
        close(fd);
        CHECK(count_entries(out) == 1 && exists(marker));
        CHECK(!exists(partial));
    }
    rm_rf(out);
    printf("cancel: ok\n");
}

static void test_corrupt(void)
{
    struct { const char *name; int want; } cases[] = {
        { "bad_magic.iso", XISO_ENOTXISO },
        { "truncated.iso", XISO_ECORRUPT },
        { "truncated_dir.iso", XISO_ECORRUPT },
        { "sector_range.iso", XISO_ECORRUPT },
        { "self_loop.iso", XISO_ECORRUPT },
        { "dir_cycle.iso", XISO_ECORRUPT },
        { "dotdot.iso", XISO_ECORRUPT },
        { "dot.iso", XISO_ECORRUPT },
        { "slash.iso", XISO_ECORRUPT },
        { "backslash.iso", XISO_ECORRUPT },
        { "duplicate.iso", XISO_ECORRUPT },
    };
    char *out = tmp_path("corrupt");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int fd = open_ro(fixture(cases[i].name));
        char title[64];
        uint64_t total;
        int xbe;
        int rc = xiso_probe_fd(fd, title, sizeof title, &total, &xbe);
        if (rc != cases[i].want) {
            fprintf(stderr, "FAIL probe %s = %d (%s), want %d\n", cases[i].name, rc, xiso_strerror(rc), cases[i].want);
            g_fail++;
        }
        rc = xiso_extract_fd(fd, out, NULL, NULL);
        if (rc != cases[i].want) {
            fprintf(stderr, "FAIL extract %s = %d (%s), want %d\n", cases[i].name, rc, xiso_strerror(rc), cases[i].want);
            g_fail++;
        }
        close(fd);
        CHECK(!exists(out) && !exists(tmp_path("corrupt.partial")));
        CHECK(count_entries(g_tmp) == 0);    /* nothing escaped next to out_dir */
    }
    /* Not an image at all, an empty file, and a pipe (no pread). */
    char *junk = tmp_path("junk.bin");
    uint8_t zeros[70000] = { 0 };
    write_file(junk, zeros, sizeof zeros);
    int fd = open_ro(junk);
    CHECK_RC(xiso_probe_fd(fd, NULL, 0, NULL, NULL), XISO_ENOTXISO);
    close(fd);
    write_file(junk, zeros, 0);
    fd = open_ro(junk);
    CHECK_RC(xiso_probe_fd(fd, NULL, 0, NULL, NULL), XISO_ENOTXISO);
    close(fd);
    unlink(junk);
    int pp[2];
    if (pipe(pp) == 0) {
        close(pp[1]);
        CHECK_RC(xiso_probe_fd(pp[0], NULL, 0, NULL, NULL), XISO_EIO);
        close(pp[0]);
    }
    printf("corrupt images: ok\n");
}

/* ── single files (the game library's scan) ── */

/* Looks path up in img and compares its whole content with want (want_len bytes). */
static void check_file(xiso_image *img, const char *path, const uint8_t *want, size_t want_len)
{
    xiso_stat st;
    int rc = xiso_lookup(img, path, &st);
    if (rc != 0) {
        fprintf(stderr, "FAIL lookup %s = %d (%s)\n", path, rc, xiso_strerror(rc));
        g_fail++;
        return;
    }
    CHECK(!st.is_dir && st.size == want_len);
    uint8_t *got = malloc(want_len + 16);
    int n = xiso_pread(img, &st, got, want_len + 16, 0);   /* asks for more: clamped to the file */
    CHECK(n == (int)want_len);
    if (n == (int)want_len && memcmp(got, want, want_len) != 0) {
        fprintf(stderr, "FAIL content of %s through xiso_pread\n", path);
        g_fail++;
    }
    free(got);
}

static void check_stream(xiso_image *img, const char *lookup_path, const char *real_path, size_t size)
{
    uint8_t *want = expected(real_path, size);
    check_file(img, lookup_path, want, size);
    free(want);
}

static void test_lookup(void)
{
    size_t xlen;
    uint8_t *xbe = read_file(fixture("default.xbe.ref"), &xlen);
    xiso_image *img = NULL;
    int fd = open_ro(fixture("tree.iso"));
    CHECK_RC(xiso_open_fd(fd, &img), 0);
    if (!img) {
        close(fd);
        free(xbe);
        return;
    }
    check_file(img, "default.xbe", xbe, xlen);
    check_file(img, "DEFAULT.XBE", xbe, xlen);                         /* case-insensitive */
    check_file(img, "/Default.Xbe", xbe, xlen);
    check_stream(img, "ReadMe.TXT", "ReadMe.TXT", 2048);
    check_stream(img, "readme.txt", "ReadMe.TXT", 2048);
    check_stream(img, "media/intro.bik", "media/intro.bik", 5000);
    check_stream(img, "media/sub/deeper/leaf.txt", "media/sub/deeper/leaf.txt", 3);
    check_stream(img, "\\MEDIA\\Sub\\deeper\\LEAF.txt", "media/sub/deeper/leaf.txt", 3);   /* backslashes */
    check_stream(img, "media//sub/\\deeper/leaf.txt/", "media/sub/deeper/leaf.txt", 3);    /* empty components */
    /* a name deep in the 300-entry directory, whose table spans several sectors */
    char name[128];
    snprintf(name, sizeof name, "many/file_%03d_%.*s.dat", 287, 287 % 40, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    check_stream(img, name, name, (287 * 37) % 3000);

    xiso_stat st;
    CHECK_RC(xiso_lookup(img, "", &st), 0);                            /* the root */
    CHECK(st.is_dir);
    CHECK_RC(xiso_lookup(img, "/", &st), 0);
    CHECK(st.is_dir);
    CHECK_RC(xiso_lookup(img, "media", &st), 0);
    CHECK(st.is_dir);
    CHECK_RC(xiso_lookup(img, "Media/SUB/", &st), 0);
    CHECK(st.is_dir);
    CHECK_RC(xiso_lookup(img, "emptydir", &st), 0);
    CHECK(st.is_dir);
    uint8_t b[64];
    CHECK_RC(xiso_pread(img, &st, b, sizeof b, 0), XISO_EINVAL);       /* a directory has no bytes */
    CHECK_RC(xiso_lookup(img, "missing.bin", &st), XISO_ENOENT);
    CHECK_RC(xiso_lookup(img, "media/nope", &st), XISO_ENOENT);
    CHECK_RC(xiso_lookup(img, "emptydir/x", &st), XISO_ENOENT);
    CHECK_RC(xiso_lookup(img, "ReadMe.TXT/inside", &st), XISO_ENOENT);  /* a file used as a directory */
    CHECK_RC(xiso_lookup(img, "default.xb", &st), XISO_ENOENT);        /* no prefix match */
    CHECK_RC(xiso_lookup(img, "default.xbee", &st), XISO_ENOENT);
    CHECK_RC(xiso_lookup(NULL, "a", &st), XISO_EINVAL);
    CHECK_RC(xiso_lookup(img, NULL, &st), XISO_EINVAL);

    /* xiso_pread clamping, in big.bin (> the copy buffer) */
    size_t big = 4 * 1024 * 1024 + 300 * 1024 + 7;
    uint8_t *want = expected("big.bin", big);
    CHECK_RC(xiso_lookup(img, "big.bin", &st), 0);
    CHECK(st.size == big && !st.is_dir);
    uint8_t *got = malloc(4096);
    CHECK(xiso_pread(img, &st, got, 4096, 3 * 1024 * 1024 + 5) == 4096);
    CHECK(memcmp(got, want + 3 * 1024 * 1024 + 5, 4096) == 0);
    CHECK(xiso_pread(img, &st, got, 4096, big - 10) == 10);            /* the tail */
    CHECK(memcmp(got, want + big - 10, 10) == 0);
    CHECK(xiso_pread(img, &st, got, 4096, big) == 0);                  /* at the end */
    CHECK(xiso_pread(img, &st, got, 4096, big + 12345) == 0);          /* past it */
    CHECK(xiso_pread(img, &st, got, 0, 0) == 0);
    CHECK_RC(xiso_lookup(img, "empty.bin", &st), 0);
    CHECK(st.size == 0 && xiso_pread(img, &st, got, 4096, 0) == 0);
    free(got);
    free(want);
    xiso_close(img);
    xiso_close(NULL);
    close(fd);

    CHECK_RC(xiso_open_fd(-1, &img), XISO_EINVAL);
    CHECK(img == NULL);
    fd = open_ro(fixture("bad_magic.iso"));
    CHECK_RC(xiso_open_fd(fd, &img), XISO_ENOTXISO);
    CHECK(img == NULL);
    close(fd);

    /* Other partition bases: 0x30600, and the redump layout (game partition at 0x18300000, sparse). */
    fd = open_ro(fixture("small_base30600.iso"));
    CHECK_RC(xiso_open_fd(fd, &img), 0);
    if (img) {
        check_file(img, "default.xbe", xbe, xlen);
        check_stream(img, "SUB/d2/Z.TXT", "sub/d2/z.txt", 77);
        xiso_close(img);
    }
    close(fd);
    size_t len;
    uint8_t *small = read_file(fixture("small.iso"), &len);
    char *red = tmp_path("redump_lookup.iso");
    fd = open(red, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0 && small && pwrite(fd, small, len, (off_t)0x18300000) == (ssize_t)len) {
        CHECK_RC(xiso_open_fd(fd, &img), 0);
        if (img) {
            check_file(img, "default.xbe", xbe, xlen);
            check_stream(img, "sub/inner.bin", "sub/inner.bin", 4096);
            CHECK_RC(xiso_lookup(img, "sub", &st), 0);
            CHECK(st.is_dir && st.offset >= 0x18300000ull);
            xiso_close(img);
        }
    } else {
        fprintf(stderr, "FAIL cannot write the sparse redump image\n");
        g_fail++;
    }
    if (fd >= 0)
        close(fd);
    unlink(red);
    free(small);
    free(xbe);
    printf("lookup: ok\n");
}

/* Truncated and corrupt images: lookup and pread fail cleanly (never read past the image). */
static void test_lookup_corrupt(void)
{
    static const char *paths[] = { "default.xbe", "a.txt", "b.txt", "c.txt", "dotdot", "slash", "sub/inner.bin",
                                   "sub/d2/z.txt" };
    /* truncated.iso: the image ends inside the last file's data */
    int fd = open_ro(fixture("truncated.iso"));
    xiso_image *img = NULL;
    int corrupt = 0;
    CHECK_RC(xiso_open_fd(fd, &img), 0);
    if (img) {
        uint8_t *buf = malloc(1 << 16);
        for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
            xiso_stat st;
            int rc = xiso_lookup(img, paths[i], &st);
            if (rc == 0)
                rc = xiso_pread(img, &st, buf, 1 << 16, 0) < 0 ? XISO_ECORRUPT : 0;
            CHECK(rc == 0 || rc == XISO_ECORRUPT);
            corrupt += rc == XISO_ECORRUPT;
        }
        free(buf);
        xiso_close(img);
    }
    close(fd);
    CHECK(corrupt == 1);

    struct { const char *name, *path; int want; } cases[] = {
        { "truncated_dir.iso", "a.txt", XISO_ECORRUPT },   /* the root table runs past the end */
        { "self_loop.iso", "a.txt", XISO_ECORRUPT },
        { "sector_range.iso", "", 0 },                     /* the root itself is fine */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fd = open_ro(fixture(cases[i].name));
        img = NULL;
        int rc = xiso_open_fd(fd, &img);
        if (!rc) {
            xiso_stat st;
            rc = xiso_lookup(img, cases[i].path, &st);
            xiso_close(img);
        }
        if (rc != cases[i].want) {
            fprintf(stderr, "FAIL lookup %s in %s = %d (%s), want %d\n", cases[i].path, cases[i].name, rc,
                    xiso_strerror(rc), cases[i].want);
            g_fail++;
        }
        close(fd);
    }
    /* sector_range.iso: some entry points out of the image; whichever it is, lookup or pread says so. */
    fd = open_ro(fixture("sector_range.iso"));
    corrupt = 0;
    if (xiso_open_fd(fd, &img) == 0) {
        uint8_t buf[256];
        for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
            xiso_stat st;
            int rc = xiso_lookup(img, paths[i], &st);
            if (rc == 0 && !st.is_dir)
                rc = xiso_pread(img, &st, buf, sizeof buf, 0) < 0 ? XISO_ECORRUPT : 0;
            CHECK(rc == 0 || rc == XISO_ECORRUPT || rc == XISO_ENOENT);
            corrupt += rc == XISO_ECORRUPT;
        }
        xiso_close(img);
    }
    close(fd);
    CHECK(corrupt >= 1);
    printf("lookup corrupt images: ok\n");
}

static void test_extract_file(void)
{
    size_t xlen, len;
    uint8_t *xbe = read_file(fixture("default.xbe.ref"), &xlen);
    char *out = tmp_path("one.xbe");
    unlink(out);
    int fd = open_ro(fixture("tree.iso"));
    CHECK_RC(xiso_extract_file_fd(fd, "Default.XBE", out, 32u << 20), 0);
    uint8_t *got = read_file(out, &len);
    CHECK(got && len == xlen && memcmp(got, xbe, xlen) == 0);
    free(got);
    /* O_EXCL: an existing file is an error and stays as it was */
    CHECK_RC(xiso_extract_file_fd(fd, "ReadMe.TXT", out, 32u << 20), XISO_EWRITE);
    got = read_file(out, &len);
    CHECK(got && len == xlen && memcmp(got, xbe, xlen) == 0);
    free(got);
    unlink(out);
    /* larger than max_bytes, a directory, a missing file: nothing written */
    CHECK_RC(xiso_extract_file_fd(fd, "default.xbe", out, xlen - 1), XISO_EINVAL);
    CHECK(!exists(out));
    CHECK_RC(xiso_extract_file_fd(fd, "default.xbe", out, xlen), 0);     /* exactly max_bytes is fine */
    unlink(out);
    CHECK_RC(xiso_extract_file_fd(fd, "media", out, 32u << 20), XISO_ENOENT);
    CHECK(!exists(out));
    CHECK_RC(xiso_extract_file_fd(fd, "nothing.here", out, 32u << 20), XISO_ENOENT);
    CHECK(!exists(out));
    /* a multi-chunk file */
    size_t big = 4 * 1024 * 1024 + 300 * 1024 + 7;
    CHECK_RC(xiso_extract_file_fd(fd, "big.bin", out, 32u << 20), 0);
    got = read_file(out, &len);
    uint8_t *want = expected("big.bin", big);
    CHECK(got && len == big && memcmp(got, want, big) == 0);
    free(got);
    free(want);
    unlink(out);
    CHECK_RC(xiso_extract_file_fd(fd, "empty.bin", out, 0), 0);          /* an empty file */
    CHECK(exists(out));
    unlink(out);
    CHECK_RC(xiso_extract_file_fd(fd, "a", "", 1), XISO_EINVAL);
    close(fd);
    CHECK_RC(xiso_extract_file_fd(-1, "default.xbe", out, 1), XISO_EINVAL);
    /* not an image; a truncated one leaves no partial file */
    fd = open_ro(fixture("bad_magic.iso"));
    CHECK_RC(xiso_extract_file_fd(fd, "default.xbe", out, 32u << 20), XISO_ENOTXISO);
    close(fd);
    CHECK(!exists(out));
    fd = open_ro(fixture("truncated_dir.iso"));
    CHECK_RC(xiso_extract_file_fd(fd, "default.xbe", out, 32u << 20), XISO_ECORRUPT);
    close(fd);
    CHECK(!exists(out));
    free(xbe);
    printf("extract one file: ok\n");
}

/* xiso_extract_xbes_source: the app's companion folder (every XBE, nothing else); a crafted directory cycle is
 * refused, not walked. */
static int xbes_from(const char *image, const char *out_dir, uint64_t max_bytes, int max_count)
{
    xiso_source *src = NULL;
    int rc = xiso_source_open_path(fixture(image), &src);
    if (rc)
        return rc;
    rc = xiso_extract_xbes_source(src, out_dir, max_bytes, max_count);
    xiso_source_close(src);
    return rc;
}

static void test_extract_xbes(void)
{
    size_t xlen, len;
    uint8_t *xbe = read_file(fixture("default.xbe.ref"), &xlen);
    char *out = tmp_path("xbes");
    char path[4200];
    rm_rf(out);
    mkdir(out, 0755);
    CHECK_RC(xbes_from("tree.iso", out, 32u << 20, 16), 1);
    snprintf(path, sizeof path, "%s/default.xbe", out);
    uint8_t *got = read_file(path, &len);
    CHECK(got && len == xlen && memcmp(got, xbe, xlen) == 0);
    free(got);
    CHECK(count_entries(out) == 1);                                       /* nothing else */
    CHECK_RC(xbes_from("tree.iso", out, 32u << 20, 16), 1);               /* again: the file is replaced */
    CHECK_RC(xbes_from("tree.iso", out, xlen - 1, 16), 0);                /* larger than max_bytes: passed over */
    CHECK_RC(xbes_from("tree.iso", out, 32u << 20, 0), 0);                /* max_count */
    /* A subdirectory pointing back at the root: the walk used to go round it (up to its depth limit, once per
     * directory entry on the way, so a table of N such entries cost N^depth reads); a table reached twice is now
     * corrupt. The self-referencing tree node: corrupt too. */
    rm_rf(out);
    mkdir(out, 0755);
    CHECK_RC(xbes_from("dir_cycle.iso", out, 32u << 20, 16), XISO_ECORRUPT);
    CHECK_RC(xbes_from("self_loop.iso", out, 32u << 20, 16), XISO_ECORRUPT);
    CHECK_RC(xbes_from("bad_magic.iso", out, 32u << 20, 16), XISO_ENOTXISO);
    CHECK_RC(xiso_extract_xbes_source(NULL, out, 1, 1), XISO_EINVAL);
    rm_rf(out);
    free(xbe);
    printf("extract every XBE: ok\n");
}

/* Random byte flips in the descriptor and directory tables, and random truncations: no crash, no write outside. */
static void test_mutations(void)
{
    size_t len;
    uint8_t *orig = read_file(fixture("small.iso"), &len);
    uint8_t *img = malloc(len);
    char *path = tmp_path("mut.iso");
    char *box = tmp_path("box");
    char *out = tmp_path("box/out");
    uint32_t r = 12345;
    int ok = 0, errs = 0;
    rm_rf(box);
    mkdir(box, 0755);
    for (int it = 0; it < 1000; it++) {
        memcpy(img, orig, len);
        size_t n = len;
        int flips = 1 + (int)(r % 8);
        for (int k = 0; k < flips; k++) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            /* the descriptor's root fields, or the used start of one of the four directory tables (sectors 33-36) */
            size_t at = (r & 0x700) == 0 ? 32 * 2048 + 0x14 + (r >> 12) % 8
                                         : (33 + (r >> 12) % 4) * 2048 + (r >> 16) % 192;
            if (at < len) {
                r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                img[at] = (it & 1) ? (uint8_t)r : (uint8_t)(img[at] ^ (1u << (r % 8)));
            }
        }
        if (it % 10 == 9) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            n = r % len;
        }
        write_file(path, img, n);
        int fd = open_ro(path);
        xiso_image *img = NULL;
        int lrc = xiso_open_fd(fd, &img);
        if (!lrc) {
            xiso_stat st;
            uint8_t b[512];
            lrc = xiso_lookup(img, "SUB\\d2/z.txt", &st);
            if (!lrc && !st.is_dir) {
                int n = xiso_pread(img, &st, b, sizeof b, 0);
                lrc = n < 0 ? n : 0;
            }
            xiso_close(img);
        }
        CHECK(lrc == 0 || lrc == XISO_ECORRUPT || lrc == XISO_ENOTXISO || lrc == XISO_ENOENT);
        int rc = xiso_probe_fd(fd, NULL, 0, NULL, NULL);
        if (rc == 0) {
            rc = xiso_extract_fd(fd, out, NULL, NULL);
            rm_rf(out);
        }
        close(fd);
        rc == 0 ? ok++ : errs++;
        CHECK(rc == 0 || rc == XISO_ECORRUPT || rc == XISO_ENOTXISO);
        CHECK(count_entries(box) == 0);      /* nothing left beside out: no .partial, no escaped file */
    }
    unlink(path);
    rm_rf(box);
    free(img);
    free(orig);
    printf("mutations: ok (%d extracted, %d rejected)\n", ok, errs);
}

/* ── sources: split parts, compressed images (XBA-126) ── */

static void source_tree_ok(const char *first_part, const char *manifest, int want_kind, int want_parts)
{
    xiso_source *src = NULL;
    CHECK_RC(xiso_source_open_path(first_part, &src), 0);
    if (!src)
        return;
    if (xiso_source_kind(src) != want_kind || xiso_source_parts(src) != want_parts) {
        fprintf(stderr, "FAIL %s: kind %s parts %d, want %s parts %d\n", first_part,
                xiso_source_kind_name(xiso_source_kind(src)), xiso_source_parts(src),
                xiso_source_kind_name(want_kind), want_parts);
        g_fail++;
    }
    char title[64];
    uint64_t total = 0;
    int xbe = 0;
    CHECK_RC(xiso_probe_source(src, title, sizeof title, &total, &xbe), 0);
    CHECK(strcmp(title, "SYNTH") == 0 && xbe == 1 && total == manifest_total(manifest));
    char *out = tmp_path("src_out");
    rm_rf(out);
    CHECK_RC(xiso_extract_source(src, out, NULL, NULL), 0);
    verify_tree(out, manifest, NULL);
    rm_rf(out);
    xiso_source_close(src);
}

typedef struct {
    xiso_source *src;
    const uint8_t *plain;
    size_t len;
    unsigned seed;
    int bad;
} reader_t;

static void *reader(void *arg)
{
    reader_t *r = arg;
    uint8_t *buf = malloc(70000);
    for (int i = 0; i < 400 && buf; i++) {
        r->seed = r->seed * 1103515245u + 12345u;
        uint64_t off = (r->seed >> 4) % (r->len + 100);
        r->seed = r->seed * 1103515245u + 12345u;
        size_t n = (r->seed >> 8) % 70000;
        long got = xiso_source_pread(r->src, buf, n, off);
        size_t want = off >= r->len ? 0 : (r->len - off < n ? r->len - off : n);
        if (got != (long)want || (want && memcmp(buf, r->plain + off, want) != 0))
            r->bad++;
    }
    free(buf);
    return NULL;
}

/* Random reads (crossing blocks, parts, the end) compared with the plain image, from four threads at once. */
static void source_reads_ok(const char *path, const char *plain_path)
{
    size_t len;
    uint8_t *plain = read_file(plain_path, &len);
    xiso_source *src = NULL;
    CHECK_RC(xiso_source_open_path(path, &src), 0);
    if (!src || !plain) {
        free(plain);
        xiso_source_close(src);
        return;
    }
    CHECK(xiso_source_size(src) == len);
    pthread_t t[4];
    reader_t r[4];
    for (int i = 0; i < 4; i++) {
        r[i] = (reader_t){ src, plain, len, 77u + (unsigned)i * 1000u, 0 };
        pthread_create(&t[i], NULL, reader, &r[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(t[i], NULL);
        if (r[i].bad) {
            fprintf(stderr, "FAIL %s: %d reads differ from the plain image (thread %d)\n", path, r[i].bad, i);
            g_fail++;
        }
    }
    xiso_source_close(src);
    free(plain);
}

static void test_sources(void)
{
    const char *sm = fixture("small.manifest"), *tm = fixture("tree.manifest");
    source_tree_ok(fixture("small.iso"), sm, XISO_KIND_PLAIN, 1);
    source_tree_ok(fixture("small_v1.cso"), sm, XISO_KIND_CSO, 1);
    source_tree_ok(fixture("small_v2.cso"), sm, XISO_KIND_CSO, 1);
    source_tree_ok(fixture("small_v2f.cso"), sm, XISO_KIND_CSO, 1);
    source_tree_ok(fixture("small_v2m.cso"), sm, XISO_KIND_CSO, 1);
    source_tree_ok(fixture("small_b16k.cso"), sm, XISO_KIND_CSO, 1);
    source_tree_ok(fixture("small.zso"), sm, XISO_KIND_ZSO, 1);
    source_tree_ok(fixture("small.cci"), sm, XISO_KIND_CCI, 1);
    source_tree_ok(fixture("tree_v1.cso"), tm, XISO_KIND_CSO, 1);
    source_tree_ok(fixture("tree_split.1.iso"), tm, XISO_KIND_PLAIN | XISO_KIND_SPLIT, 3);
    source_tree_ok(fixture("tree_split.2.iso"), tm, XISO_KIND_PLAIN | XISO_KIND_SPLIT, 3);   /* any part */
    source_tree_ok(fixture("small_x.iso.001"), sm, XISO_KIND_PLAIN | XISO_KIND_SPLIT, 6);
    source_tree_ok(fixture("small_p.part3.iso"), sm, XISO_KIND_PLAIN | XISO_KIND_SPLIT, 4);
    source_tree_ok(fixture("small_cs.1.cso"), sm, XISO_KIND_CSO | XISO_KIND_SPLIT, 2);

    /* The fd forms read a compressed file too (the app's import and library scan). */
    int fd = open_ro(fixture("small_v2.cso"));
    char title[64];
    CHECK_RC(xiso_probe_fd(fd, title, sizeof title, NULL, NULL), 0);
    CHECK(strcmp(title, "SYNTH") == 0);
    close(fd);

    /* Split parts handed over as fds (the app's picker gives no paths). */
    int fds[3] = { open_ro(fixture("tree_split.1.iso")), open_ro(fixture("tree_split.2.iso")),
                   open_ro(fixture("tree_split.3.iso")) };
    xiso_source *src = NULL;
    CHECK_RC(xiso_source_open_fds(fds, 3, 1, &src), 0);
    if (src) {
        xiso_image *img = NULL;
        CHECK_RC(xiso_open_source(src, &img), 0);
        if (img) {
            check_stream(img, "big.bin", "big.bin", 4 * 1024 * 1024 + 300 * 1024 + 7);   /* crosses both cuts */
            xiso_close(img);
        }
        xiso_source_close(src);                          /* closes the three fds (own) */
    }

    source_reads_ok(fixture("tree_v1.cso"), fixture("tree.iso"));
    source_reads_ok(fixture("tree_split.3.iso"), fixture("tree.iso"));
    source_reads_ok(fixture("small_v2m.cso"), fixture("small.iso"));
    source_reads_ok(fixture("small.cci"), fixture("small.iso"));
    source_reads_ok(fixture("small_cs.2.cso"), fixture("small.iso"));

    /* Split names. */
    char (*parts)[1024] = malloc(sizeof(*parts) * XISO_SOURCE_MAX_PARTS);
    CHECK(xiso_split_parts(fixture("small.iso"), parts, XISO_SOURCE_MAX_PARTS) == 1);
    CHECK(xiso_split_parts(fixture("small_x.iso.004"), parts, XISO_SOURCE_MAX_PARTS) == 6);
    CHECK(strstr(parts[0], "small_x.iso.001") && strstr(parts[5], "small_x.iso.006"));
    CHECK(xiso_split_parts(tmp_path("nope.1.iso"), parts, XISO_SOURCE_MAX_PARTS) == 0);
    {   /* a lone "x.1.iso" is no split; "x.2.iso" without "x.1.iso" neither */
        char *one = tmp_path("lone.1.iso"), *two = tmp_path("gap.2.iso"), *three = tmp_path("gap.3.iso");
        write_file(one, (const uint8_t *)"x", 1);
        write_file(two, (const uint8_t *)"x", 1);
        write_file(three, (const uint8_t *)"x", 1);
        CHECK(xiso_split_parts(one, parts, XISO_SOURCE_MAX_PARTS) == 1);
        CHECK(xiso_split_parts(two, parts, XISO_SOURCE_MAX_PARTS) == 1 && strcmp(parts[0], two) == 0);
        unlink(one); unlink(two); unlink(three);
    }
    free(parts);

    /* xiso_list: the root and a subdirectory, sorted, with sizes and kinds. */
    CHECK_RC(xiso_source_open_path(fixture("small_v2.cso"), &src), 0);
    if (src) {
        xiso_image *img = NULL;
        CHECK_RC(xiso_open_source(src, &img), 0);
        if (img) {
            xiso_dirent *d = NULL;
            size_t n = 0;
            CHECK_RC(xiso_list(img, NULL, &d, &n), 0);
            CHECK(n == 8);                               /* small: default.xbe, 5 files, sub, void */
            int saw_sub = 0;
            for (size_t i = 0; i < n; i++) {
                if (i)
                    CHECK(strcmp(d[i - 1].name, d[i].name) < 0);
                if (!strcmp(d[i].name, "sub"))
                    saw_sub = d[i].is_dir;
                if (!strcmp(d[i].name, "b.txt"))
                    CHECK(d[i].size == 2049 && !d[i].is_dir);
            }
            CHECK(saw_sub);
            free(d);
            xiso_stat st;
            CHECK_RC(xiso_lookup(img, "sub", &st), 0);
            CHECK_RC(xiso_list(img, &st, &d, &n), 0);
            CHECK(n == 2);
            free(d);
            CHECK_RC(xiso_lookup(img, "void", &st), 0);
            CHECK_RC(xiso_list(img, &st, &d, &n), 0);
            CHECK(n == 0);
            free(d);
            CHECK_RC(xiso_lookup(img, "a.txt", &st), 0);
            CHECK_RC(xiso_list(img, &st, &d, &n), XISO_EINVAL);   /* a file */
            CHECK(xiso_partition_offset(img) == 0);
            xiso_close(img);
        }
        xiso_source_close(src);
    }

    /* Broken compressed images: the read that needs the bad block fails, nothing else does. */
    CHECK_RC(xiso_source_open_path(fixture("short.cso"), &src), XISO_ECORRUPT);
    CHECK(src == NULL);
    CHECK_RC(xiso_source_open_path(fixture("bad_block.zso"), &src), 0);
    if (src) {
        uint8_t b[4096];
        CHECK(xiso_source_pread(src, b, sizeof b, 0) == XISO_ECORRUPT);           /* block 0 is broken */
        CHECK(xiso_source_pread(src, b, 2048, 16 * 2048) == 2048);               /* block 16 is fine */
        xiso_source_close(src);
    }
    printf("sources: ok\n");
}

int main(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;
    const char *t = getenv("TMPDIR");
    snprintf(g_tmp, sizeof g_tmp, "%s/xiso_test.XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(g_tmp)) {
        perror(g_tmp);
        return 1;
    }
    test_probe();
    test_extract_tree();
    test_bases();
    test_cancel();
    test_corrupt();
    test_lookup();
    test_lookup_corrupt();
    test_extract_file();
    test_extract_xbes();
    test_mutations();
    test_sources();
    rm_rf(g_tmp);
    printf(g_fail ? "%d failure(s)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
