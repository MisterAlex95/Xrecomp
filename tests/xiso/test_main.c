/* src/xiso (XBA-40): probe and extract synthetic disc images, byte for byte, plus cancel and corrupt images.
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
    test_mutations();
    rm_rf(g_tmp);
    printf(g_fail ? "%d failure(s)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
