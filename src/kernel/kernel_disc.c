/*
 * kernel_disc.c - D: read straight from a disc image (XBA-126). See kernel_disc.h.
 *
 * The tree is read once at mount, breadth first, into one array of nodes
 * whose children are contiguous and sorted ignoring case, so a lookup is a
 * binary search per component and never touches the image. File reads go to
 * xiso_source_pread at the file's offset: a pread on the image (or the right
 * split part), or a decode through the source's block cache for a compressed
 * one. Nothing is extracted, nothing is written.
 */
#if defined(_WIN32)
typedef int kernel_disc_win32_unused;   /* no image support on Windows (no src/xiso there) */
#else

#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE 1

#include "kernel_disc.h"
#include "xiso.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct xbox_disc_node {
    char *name;
    uint64_t offset;            /* absolute in the image */
    uint32_t size;
    uint32_t first, count;      /* children: s_nodes[first .. first + count) */
    uint8_t is_dir;
};

static xiso_source *s_src;
static xbox_disc_node *s_nodes;
static uint32_t s_count;
static char s_root[PATH_MAX];
static size_t s_root_len;
static uint64_t s_part_off, s_time;

int xbox_disc_mounted(void) { return s_src != NULL; }
const char *xbox_disc_root(void) { return s_root; }
const char *xbox_disc_kind(void) { return s_src ? xiso_source_kind_name(xiso_source_kind(s_src)) : ""; }
uint64_t xbox_disc_time(void) { return s_time; }

int xbox_disc_spec_is_image(const char *spec)
{
    struct stat st;
    if (!spec || !*spec)
        return 0;
    if (strncmp(spec, "fd:", 3) == 0)
        return 1;
    return stat(spec, &st) == 0 && S_ISREG(st.st_mode);
}

static void free_tree(void)
{
    for (uint32_t i = 0; i < s_count; i++)
        free(s_nodes[i].name);
    free(s_nodes);
    s_nodes = NULL;
    s_count = 0;
}

void xbox_disc_unmount(void)
{
    free_tree();
    xiso_source_close(s_src);
    s_src = NULL;
    s_root[0] = 0;
    s_root_len = 0;
}

static int node_cmp(const void *a, const void *b)
{
    const xbox_disc_node *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

/* Breadth first: node i's children are appended as one run, sorted. */
static int build_tree(xiso_image *img)
{
    uint32_t cap = 256;
    s_nodes = calloc(cap, sizeof *s_nodes);
    if (!s_nodes)
        return XISO_ENOMEM;
    s_nodes[0].name = strdup("");
    s_nodes[0].is_dir = 1;
    s_count = 1;
    for (uint32_t i = 0; i < s_count; i++) {
        if (!s_nodes[i].is_dir)
            continue;
        xiso_dirent *d = NULL;
        size_t n = 0;
        xiso_stat st = { s_nodes[i].offset, s_nodes[i].size, 1 };
        int rc = xiso_list(img, i == 0 ? NULL : &st, &d, &n);
        if (rc)
            return rc;
        if (s_count + n > (1u << 20)) {
            free(d);
            return XISO_ECORRUPT;
        }
        while (s_count + n > cap) {
            cap *= 2;
            xbox_disc_node *t = realloc(s_nodes, cap * sizeof *t);
            if (!t) {
                free(d);
                return XISO_ENOMEM;
            }
            s_nodes = t;
        }
        s_nodes[i].first = s_count;
        s_nodes[i].count = (uint32_t)n;
        for (size_t k = 0; k < n; k++) {
            xbox_disc_node *c = &s_nodes[s_count++];
            memset(c, 0, sizeof *c);
            c->name = strdup(d[k].name);
            c->offset = d[k].offset;
            c->size = d[k].size;
            c->is_dir = (uint8_t)d[k].is_dir;
            if (!c->name) {
                free(d);
                return XISO_ENOMEM;
            }
        }
        free(d);
        qsort(s_nodes + s_nodes[i].first, n, sizeof *s_nodes, node_cmp);
    }
    return 0;
}

static int open_spec(const char *spec, xiso_source **src)
{
    if (strncmp(spec, "fd:", 3) == 0) {
        int fds[XISO_SOURCE_MAX_PARTS], n = 0;
        const char *p = spec + 3;
        while (*p && n < XISO_SOURCE_MAX_PARTS) {
            char *end;
            long v = strtol(p, &end, 10);
            if (end == p || v < 0 || v > INT_MAX)
                return XISO_EINVAL;
            fds[n++] = (int)v;
            p = *end == ',' ? end + 1 : end;
            if (*end && *end != ',')
                return XISO_EINVAL;
        }
        if (n == 0 || *p)
            return XISO_EINVAL;
        return xiso_source_open_fds(fds, n, 1, src);
    }
    char real[PATH_MAX];
    /* A link (the desktop's build/game) points at one part: the parts are found by its real name. */
    return xiso_source_open_path(realpath(spec, real) ? real : spec, src);
}

int xbox_disc_mount(const char *spec)
{
    const char *off = getenv("RECOMP_DISC_IMAGE");
    struct timespec t0, t1;
    char root[PATH_MAX];
    size_t len;

    if (!spec || !*spec)
        return XISO_EINVAL;
    if (off && strcmp(off, "0") == 0) {
        fprintf(stderr, "  [DISC] %s: disc images turned off (RECOMP_DISC_IMAGE=0)\n", spec);
        return XISO_EINVAL;
    }
    len = strlen(spec);
    while (len > 1 && spec[len - 1] == '/')
        len--;
    if (len >= sizeof root)
        return XISO_EINVAL;
    memcpy(root, spec, len);
    root[len] = 0;
    if (s_src && strcmp(root, s_root) == 0)
        return 0;                                   /* the boot and xbox_path_init both mount it */

    clock_gettime(CLOCK_MONOTONIC, &t0);
    xiso_source *src = NULL;
    xiso_image *img = NULL;
    int rc = open_spec(root, &src);
    if (!rc)
        rc = xiso_open_source(src, &img);
    if (rc) {
        fprintf(stderr, "  [DISC] %s: cannot read the disc image: %s\n", root, xiso_strerror(rc));
        xiso_source_close(src);
        return rc;
    }
    xbox_disc_unmount();
    s_part_off = xiso_partition_offset(img);
    s_time = xiso_volume_time(img);
    rc = build_tree(img);
    xiso_close(img);
    if (rc) {
        fprintf(stderr, "  [DISC] %s: bad directory tree: %s\n", root, xiso_strerror(rc));
        free_tree();
        xiso_source_close(src);
        return rc;
    }
    s_src = src;
    memcpy(s_root, root, len + 1);
    s_root_len = len;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "  [DISC] mounted %s: %s, %d part(s), %.1f MB, %u entries, tree read in %ld ms\n",
            s_root, xbox_disc_kind(), xiso_source_parts(src), xiso_source_size(src) / 1048576.0, s_count - 1,
            (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
    fflush(stderr);
    return 0;
}

const char *xbox_disc_rel(const char *host_path)
{
    if (!s_src || !host_path || strncmp(host_path, s_root, s_root_len) != 0)
        return NULL;
    const char *r = host_path + s_root_len;
    if (*r != 0 && *r != '/')
        return NULL;
    while (*r == '/')
        r++;
    return r;
}

/* The child of dir named name[0..len), exact spelling first. */
static const xbox_disc_node *child(const xbox_disc_node *dir, const char *name, size_t len)
{
    char key[256];
    if (len >= sizeof key)
        return NULL;
    memcpy(key, name, len);
    key[len] = 0;
    uint32_t lo = 0, hi = dir->count;
    const xbox_disc_node *kids = s_nodes + dir->first;
    while (lo < hi) {                               /* the first that is not below key, ignoring case */
        uint32_t mid = lo + (hi - lo) / 2;
        if (strcasecmp(kids[mid].name, key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    const xbox_disc_node *hit = NULL;
    for (uint32_t i = lo; i < dir->count && strcasecmp(kids[i].name, key) == 0; i++) {
        if (strcmp(kids[i].name, key) == 0)
            return &kids[i];
        if (!hit)
            hit = &kids[i];
    }
    return hit;
}

const xbox_disc_node *xbox_disc_find(const char *rel, int *why)
{
    int dummy;
    if (!why)
        why = &dummy;
    *why = 3;
    if (!s_src || !rel)
        return NULL;
    const xbox_disc_node *n = &s_nodes[0];
    const char *p = rel;
    for (;;) {
        while (*p == '/' || *p == '\\')
            p++;
        if (!*p)
            return n;
        const char *end = p;
        while (*end && *end != '/' && *end != '\\')
            end++;
        if (!n->is_dir) {
            *why = 3;
            return NULL;
        }
        const xbox_disc_node *c = child(n, p, (size_t)(end - p));
        if (!c) {
            const char *rest = end;
            while (*rest == '/' || *rest == '\\')
                rest++;
            *why = *rest ? 3 : 2;
            return NULL;
        }
        n = c;
        p = end;
    }
}

int xbox_disc_node_is_dir(const xbox_disc_node *n) { return n && n->is_dir; }
uint32_t xbox_disc_node_size(const xbox_disc_node *n) { return n && !n->is_dir ? n->size : 0; }
const char *xbox_disc_node_name(const xbox_disc_node *n) { return n ? n->name : ""; }
uint32_t xbox_disc_node_children(const xbox_disc_node *n) { return n && n->is_dir ? n->count : 0; }

const xbox_disc_node *xbox_disc_node_child(const xbox_disc_node *n, uint32_t i)
{
    return n && n->is_dir && i < n->count ? &s_nodes[n->first + i] : NULL;
}

long xbox_disc_read(const xbox_disc_node *n, void *buf, size_t len, uint64_t off)
{
    if (!s_src || !n || n->is_dir)
        return XISO_EINVAL;
    if (off >= n->size)
        return 0;
    if (len > n->size - off)
        len = (size_t)(n->size - off);
    size_t done = 0;
    while (done < len) {
        long r = xiso_source_pread(s_src, (uint8_t *)buf + done, len - done, n->offset + off + done);
        if (r < 0)
            return r;
        if (r == 0)
            return XISO_ECORRUPT;                   /* the image ends inside the file */
        done += (size_t)r;
    }
    return (long)done;
}

uint64_t xbox_disc_partition_bytes(void)
{
    if (!s_src)
        return 0;
    uint64_t size = xiso_source_size(s_src);
    return size > s_part_off ? size - s_part_off : 0;
}

long xbox_disc_raw_read(void *buf, size_t len, uint64_t off)
{
    uint64_t total = xbox_disc_partition_bytes();
    if (!s_src)
        return XISO_EINVAL;
    if (off >= total)
        return 0;
    if (len > total - off)
        len = (size_t)(total - off);
    return xiso_source_pread(s_src, buf, len, s_part_off + off);
}

int xbox_disc_scsi(const uint8_t *cdb, uint8_t *data, uint32_t data_len)
{
    if (!s_src || !cdb || !data)
        return 0;
    if (cdb[0] == 0x25) {                           /* READ CAPACITY(10): last LBA, block length */
        uint64_t blocks = xbox_disc_partition_bytes() / 2048;
        uint32_t last = blocks ? (uint32_t)(blocks - 1) : 0;
        uint8_t r[8] = { (uint8_t)(last >> 24), (uint8_t)(last >> 16), (uint8_t)(last >> 8), (uint8_t)last,
                         0, 0, 0x08, 0x00 };
        memset(data, 0, data_len);
        memcpy(data, r, data_len < 8 ? data_len : 8);
        return 1;
    }
    if (cdb[0] == 0x28 || cdb[0] == 0xA8) {         /* READ(10) / READ(12) */
        uint32_t lba = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 | (uint32_t)cdb[4] << 8 | cdb[5];
        uint32_t count = cdb[0] == 0x28 ? (uint32_t)cdb[7] << 8 | cdb[8]
                       : (uint32_t)cdb[6] << 24 | (uint32_t)cdb[7] << 16 | (uint32_t)cdb[8] << 8 | cdb[9];
        uint64_t want = (uint64_t)count * 2048;
        if (want > data_len)
            want = data_len;
        memset(data, 0, data_len);
        long r = xbox_disc_raw_read(data, (size_t)want, (uint64_t)lba * 2048);
        if (r < 0)
            fprintf(stderr, "  [DISC] READ at sector %u: %s\n", lba, xiso_strerror((int)r));
        return 1;
    }
    return 0;
}

int xbox_disc_slurp(const char *host_path, void **data, size_t *size)
{
    char path[PATH_MAX];
    const char *rel = NULL;
    if (!host_path || !data || !size || strlen(host_path) >= sizeof path)
        return -1;
    *data = NULL;
    *size = 0;
    rel = xbox_disc_rel(host_path);
    if (!rel) {
        /* Not under the mounted image: find an image on the way and mount it. */
        snprintf(path, sizeof path, "%s", host_path);
        for (char *p = path + 1; *p; p++) {
            if (*p != '/' && *p != '\\')
                continue;
            char c = *p;
            *p = 0;
            int image = xbox_disc_spec_is_image(path);
            *p = c;
            if (!image)
                continue;
            *p = 0;
            int rc = xbox_disc_mount(path);
            *p = c;
            if (rc)
                return -1;
            rel = xbox_disc_rel(host_path);
            break;
        }
    }
    if (!rel)
        return -1;
    int why;
    const xbox_disc_node *n = xbox_disc_find(rel, &why);
    if (!n || n->is_dir)
        return -1;
    void *buf = malloc(n->size ? n->size : 1);
    if (!buf)
        return -1;
    if (xbox_disc_read(n, buf, n->size, 0) != (long)n->size) {
        free(buf);
        return -1;
    }
    *data = buf;
    *size = n->size;
    return 0;
}

#endif /* !_WIN32 */
