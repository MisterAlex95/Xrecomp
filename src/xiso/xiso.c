/* Xbox disc image (XISO / XDVDFS) reader and extractor (XBA-40). See xiso.h.
 *
 * XDVDFS, as far as this file needs it (all little-endian):
 *   volume descriptor, sector 32 of the game partition (2048-byte sectors):
 *     0x000  "MICROSOFT*XBOX*MEDIA"
 *     0x014  u32 root directory sector
 *     0x018  u32 root directory size in bytes
 *     0x01C  FILETIME (unused here); the magic again at 0x7EC (not required)
 *   a directory is a table of `size` bytes at `sector`, holding a binary tree of entries:
 *     0x00  u16 left subtree offset, in DWORDs from the table start (0: none)
 *     0x02  u16 right subtree offset (0: none)
 *     0x04  u32 start sector of the file data or of the subdirectory table
 *     0x08  u32 size in bytes
 *     0x0C  u8  attributes (0x10: directory)
 *     0x0D  u8  name length, then the name (no terminator); entries are DWORD aligned
 *   The root node is at offset 0. A table whose first DWORD is 0xFFFFFFFF (padding) or a size of 0 is an empty
 *   directory. Unused space is padded with 0xFF.
 */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE 1

#include "xiso.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SECTOR_SIZE     2048u
#define DESC_SECTOR     32u
#define ATTR_DIRECTORY  0x10u
#define ENTRY_HEADER    14u

#define BUF_SIZE        (2u << 20)          /* pread / write chunk */
#define REPORT_EVERY    (4ull << 20)        /* progress granularity inside a file */
#define MAX_DEPTH       32                  /* directory nesting (real discs: a handful) */
#define MAX_DIR_BYTES   (16u << 20)         /* one directory table (real discs: a few KB) */
#define MAX_ENTRIES     (1u << 20)          /* files + directories in the whole image */

static const char XISO_MAGIC[20] = { 'M','I','C','R','O','S','O','F','T','*','X','B','O','X','*','M','E','D','I','A' };

/* Game partition bases, probed in order (same list as tools/xiso/xdvdfs.py). */
static const uint64_t BASES[] = { 0x00000000ull, 0x0000FD90ull, 0x00030600ull, 0x0FD90000ull, 0x18300000ull };

typedef struct {
    uint32_t sector, size;
    uint8_t attr;
    char name[256];
} entry_t;

typedef struct {
    xiso_source *src;                       /* not owned */
    uint64_t img_size;                      /* UINT64_MAX when the source cannot tell */
    uint64_t base;
    uint32_t root_sector, root_size;
    uint64_t vol_time;                      /* the descriptor's FILETIME */

    /* directory tables already walked in this pass (open addressing, key sector + 1) */
    uint32_t *seen;
    size_t seen_cap, seen_count;
    uint32_t entries;

    /* probe results */
    uint64_t total;
    int has_xbe;
    uint32_t xbe_sector, xbe_size;

    /* extraction */
    int extracting;
    uint8_t *buf;
    uint64_t done, last_report;
    xiso_progress_fn progress;
    void *user;
    char path[MAX_DEPTH * 257 + 1];         /* current file, relative, for the callback */
} vol_t;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

const char *xiso_strerror(int err)
{
    switch (err) {
    case 0:              return "success";
    case XISO_EIO:       return "error reading the disc image";
    case XISO_ENOTXISO:  return "not an Xbox disc image (no XDVDFS volume found)";
    case XISO_ECORRUPT:  return "the disc image is corrupt or truncated";
    case XISO_ENOSPC:    return "not enough free space for the extracted files";
    case XISO_ECANCELED: return "cancelled";
    case XISO_EWRITE:    return "cannot write the extracted files";
    case XISO_ENOMEM:    return "out of memory";
    case XISO_EINVAL:    return "invalid argument";
    case XISO_ENOENT:    return "no such file in the disc image";
    default:             return "unknown error";
    }
}

/* ── I/O ─────────────────────────────────────────────────────────────────── */

/* Reads exactly len bytes at off. XISO_ECORRUPT when the image ends first. */
static int read_full(vol_t *v, void *dst, size_t len, uint64_t off)
{
    uint8_t *p = dst;
    while (len > 0) {
        long n = xiso_source_pread(v->src, p, len, off);
        if (n < 0)
            return (int)n;
        if (n == 0)
            return XISO_ECORRUPT;
        p += n;
        off += (uint64_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_err(int e)
{
    return (e == ENOSPC
#ifdef EDQUOT
            || e == EDQUOT
#endif
            ) ? XISO_ENOSPC : XISO_EWRITE;
}

static int write_full(int fd, const uint8_t *p, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return write_err(errno);
        }
        if (n == 0)
            return XISO_EWRITE;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* True when [sector * 2048, + size) of the game partition lies inside the image. */
static int in_image(const vol_t *v, uint32_t sector, uint32_t size)
{
    uint64_t start = v->base + (uint64_t)sector * SECTOR_SIZE;   /* < 2^44: no overflow */
    return v->img_size == UINT64_MAX || (start <= v->img_size && size <= v->img_size - start);
}

/* ── Volume ──────────────────────────────────────────────────────────────── */

static int vol_open(vol_t *v, xiso_source *src)
{
    memset(v, 0, sizeof *v);
    v->src = src;
    v->img_size = xiso_source_size(src);      /* UINT64_MAX: a pipe or socket, size unknown */
    for (size_t i = 0; i < sizeof BASES / sizeof BASES[0]; i++) {
        uint8_t desc[SECTOR_SIZE];
        uint64_t off = BASES[i] + (uint64_t)DESC_SECTOR * SECTOR_SIZE;
        if (v->img_size != UINT64_MAX && (off > v->img_size || v->img_size - off < SECTOR_SIZE))
            continue;
        int rc = read_full(v, desc, sizeof desc, off);
        if (rc == XISO_ECORRUPT)
            continue;                         /* past the end of an image of unknown size */
        if (rc)
            return rc;
        if (memcmp(desc, XISO_MAGIC, sizeof XISO_MAGIC) != 0)
            continue;
        v->base = BASES[i];
        v->root_sector = rd32(desc + 0x14);
        v->root_size = rd32(desc + 0x18);
        v->vol_time = (uint64_t)rd32(desc + 0x1C) | (uint64_t)rd32(desc + 0x20) << 32;
        return 0;
    }
    return XISO_ENOTXISO;
}

static void vol_close(vol_t *v)
{
    free(v->seen);
    free(v->buf);
    v->seen = NULL;
    v->buf = NULL;
}

static void seen_reset(vol_t *v)
{
    if (v->seen)
        memset(v->seen, 0, v->seen_cap * sizeof *v->seen);
    v->seen_count = 0;
    v->entries = 0;
}

/* Records a directory table; XISO_ECORRUPT when it was already walked (a cycle, or a shared subtree). */
static int seen_add(vol_t *v, uint32_t sector)
{
    if ((v->seen_count + 1) * 2 > v->seen_cap) {
        size_t cap = v->seen_cap ? v->seen_cap * 2 : 64;
        uint32_t *t = calloc(cap, sizeof *t);
        if (!t)
            return XISO_ENOMEM;
        for (size_t i = 0; i < v->seen_cap; i++) {
            uint32_t k = v->seen[i];
            if (!k)
                continue;
            size_t h = (k * 2654435761u) & (cap - 1);
            while (t[h])
                h = (h + 1) & (cap - 1);
            t[h] = k;
        }
        free(v->seen);
        v->seen = t;
        v->seen_cap = cap;
    }
    uint32_t key = sector + 1;                /* 0 marks a free slot; sector 0xFFFFFFFF is never in range */
    size_t h = (key * 2654435761u) & (v->seen_cap - 1);
    while (v->seen[h]) {
        if (v->seen[h] == key)
            return XISO_ECORRUPT;
        h = (h + 1) & (v->seen_cap - 1);
    }
    v->seen[h] = key;
    v->seen_count++;
    return 0;
}

static int name_ok(const uint8_t *s, unsigned len)
{
    if (len == 0 || (len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'))
        return 0;
    for (unsigned i = 0; i < len; i++)
        if (s[i] < 0x20 || s[i] == 0x7F || s[i] == '/' || s[i] == '\\')
            return 0;
    return 1;
}

static int entry_cmp(const void *a, const void *b)
{
    return strcmp(((const entry_t *)a)->name, ((const entry_t *)b)->name);
}

/* Reads one directory table and returns its entries sorted by name (*out to free). */
static int read_dir(vol_t *v, uint32_t sector, uint32_t size, entry_t **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    if (size == 0)
        return 0;
    if (size > MAX_DIR_BYTES || size < 4 || sector == UINT32_MAX || !in_image(v, sector, size))
        return XISO_ECORRUPT;
    int rc = seen_add(v, sector);
    if (rc)
        return rc;

    size_t ndw = (size + 3) / 4;
    uint8_t *tab = malloc(size);
    uint8_t *mark = calloc((ndw + 7) / 8, 1);
    uint32_t *stack = malloc(ndw * sizeof *stack);
    entry_t *ents = NULL;
    size_t n = 0, cap = 0, sp = 0;
    if (!tab || !mark || !stack) {
        rc = XISO_ENOMEM;
        goto done;
    }
    rc = read_full(v, tab, size, v->base + (uint64_t)sector * SECTOR_SIZE);
    if (rc)
        goto done;
    if (rd32(tab) == 0xFFFFFFFFu)             /* padding only: empty directory */
        goto done;

    /* Each node is pushed at most once (mark), so the walk ends and the stack never overflows. */
    stack[sp++] = 0;
    mark[0] |= 1;
    while (sp > 0) {
        uint32_t off = stack[--sp];
        if (size - off < ENTRY_HEADER) {
            rc = XISO_ECORRUPT;
            goto done;
        }
        const uint8_t *e = tab + off;
        uint16_t l = rd16(e), r = rd16(e + 2);
        unsigned nlen = e[13];
        if ((l == 0xFFFF && r == 0xFFFF) || size - off - ENTRY_HEADER < nlen || !name_ok(e + ENTRY_HEADER, nlen)) {
            rc = XISO_ECORRUPT;
            goto done;
        }
        if (++v->entries > MAX_ENTRIES) {
            rc = XISO_ECORRUPT;
            goto done;
        }
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            entry_t *t = realloc(ents, ncap * sizeof *t);
            if (!t) {
                rc = XISO_ENOMEM;
                goto done;
            }
            ents = t;
            cap = ncap;
        }
        entry_t *d = &ents[n++];
        d->sector = rd32(e + 4);
        d->size = rd32(e + 8);
        d->attr = e[12];
        memcpy(d->name, e + ENTRY_HEADER, nlen);
        d->name[nlen] = 0;

        uint16_t kids[2] = { l, r };
        for (int k = 0; k < 2; k++) {
            if (!kids[k])
                continue;
            uint32_t c = (uint32_t)kids[k] * 4;
            if (c >= size || (mark[kids[k] / 8] & (1u << (kids[k] % 8)))) {
                rc = XISO_ECORRUPT;         /* out of the table, or a node reached twice: a cycle */
                goto done;
            }
            mark[kids[k] / 8] |= (uint8_t)(1u << (kids[k] % 8));
            stack[sp++] = c;
        }
    }
    qsort(ents, n, sizeof *ents, entry_cmp);
    for (size_t i = 1; i < n; i++)
        if (strcmp(ents[i - 1].name, ents[i].name) == 0) {
            rc = XISO_ECORRUPT;             /* the same name twice in one directory */
            goto done;
        }
done:
    free(tab);
    free(mark);
    free(stack);
    if (rc) {
        free(ents);
        return rc;
    }
    *out = ents;
    *count = n;
    return 0;
}

/* ── Walk (probe and extraction share it) ────────────────────────────────── */

static int report(vol_t *v, const char *path)
{
    v->last_report = v->done;
    if (v->progress && v->progress(v->done, v->total, path, v->user))
        return XISO_ECANCELED;
    return 0;
}

static int copy_file(vol_t *v, int dirfd, const entry_t *e)
{
    int rc = report(v, v->path);
    if (rc)
        return rc;
    int fd = openat(dirfd, e->name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0)
        return errno == EEXIST ? XISO_ECORRUPT : write_err(errno);
    uint64_t src = v->base + (uint64_t)e->sector * SECTOR_SIZE;
    uint32_t left = e->size;
    while (left > 0) {
        size_t chunk = left < BUF_SIZE ? left : BUF_SIZE;
        rc = read_full(v, v->buf, chunk, src);
        if (!rc)
            rc = write_full(fd, v->buf, chunk);
        if (rc)
            break;
        src += chunk;
        left -= (uint32_t)chunk;
        v->done += chunk;
        if (v->done - v->last_report >= REPORT_EVERY && (rc = report(v, v->path)) != 0)
            break;
    }
    if (close(fd) != 0 && !rc && errno != EINTR)
        rc = write_err(errno);
    return rc;
}

static int walk(vol_t *v, uint32_t sector, uint32_t size, int depth, int dirfd, size_t plen)
{
    if (depth > MAX_DEPTH)
        return XISO_ECORRUPT;
    entry_t *ents;
    size_t n;
    int rc = read_dir(v, sector, size, &ents, &n);
    if (rc)
        return rc;
    for (size_t i = 0; i < n && !rc; i++) {
        const entry_t *e = &ents[i];
        int is_dir = (e->attr & ATTR_DIRECTORY) != 0;
        if (!is_dir && e->size > 0 && !in_image(v, e->sector, e->size)) {
            rc = XISO_ECORRUPT;
            break;
        }
        size_t nl = strlen(e->name);
        if (plen + 1 + nl >= sizeof v->path) { /* cannot happen with MAX_DEPTH, kept as a guard */
            rc = XISO_ECORRUPT;
            break;
        }
        size_t sub = plen;
        if (plen)
            v->path[sub++] = '/';
        memcpy(v->path + sub, e->name, nl + 1);
        sub += nl;

        if (!v->extracting) {
            if (is_dir) {
                rc = walk(v, e->sector, e->size, depth + 1, -1, sub);
            } else {
                v->total += e->size;
                if (depth == 0 && strcasecmp(e->name, "default.xbe") == 0) {
                    v->has_xbe = 1;
                    v->xbe_sector = e->sector;
                    v->xbe_size = e->size;
                }
            }
        } else if (is_dir) {
            if (mkdirat(dirfd, e->name, 0755) != 0) {
                rc = errno == EEXIST ? XISO_ECORRUPT : write_err(errno);
                break;
            }
            int sfd = openat(dirfd, e->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sfd < 0) {
                rc = write_err(errno);
                break;
            }
            rc = walk(v, e->sector, e->size, depth + 1, sfd, sub);
            close(sfd);
        } else {
            rc = copy_file(v, dirfd, e);
        }
        v->path[plen] = 0;
    }
    free(ents);
    return rc;
}

/* Walks the whole tree once without writing: validates it, sums the sizes, finds default.xbe. */
static int scan(vol_t *v)
{
    seen_reset(v);
    v->extracting = 0;
    v->total = 0;
    v->has_xbe = 0;
    v->path[0] = 0;
    return walk(v, v->root_sector, v->root_size, 0, -1, 0);
}

/* ── Title from default.xbe ──────────────────────────────────────────────── */

static size_t put_utf8(char *dst, size_t room, uint32_t cp)
{
    char b[4];
    size_t n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 0x3F));
                             b[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 0x3F));
           b[2] = (char)(0x80 | (cp >> 6 & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    if (n > room)
        return 0;
    memcpy(dst, b, n);
    return n;
}

/* The certificate's title name: 40 UTF-16LE units at certificate + 0x0C. Leaves "" when anything is off. */
static void read_title(vol_t *v, char *out, size_t cap)
{
    uint8_t hdr[0x178], name[80];
    if (!v->has_xbe || v->xbe_size < sizeof hdr)
        return;
    uint64_t start = v->base + (uint64_t)v->xbe_sector * SECTOR_SIZE;
    if (read_full(v, hdr, sizeof hdr, start) || memcmp(hdr, "XBEH", 4) != 0)
        return;
    uint32_t base_addr = rd32(hdr + 0x104), cert_addr = rd32(hdr + 0x118);
    if (cert_addr < base_addr)
        return;
    uint64_t at = (uint64_t)(cert_addr - base_addr) + 0x0C;
    if (at + sizeof name > v->xbe_size || read_full(v, name, sizeof name, start + at))
        return;
    size_t o = 0;
    for (size_t i = 0; i < 40; i++) {
        uint32_t cp = rd16(name + 2 * i);
        if (cp == 0)
            break;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < 40) {
            uint32_t lo = rd16(name + 2 * (i + 1));
            if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = 0xFFFD;
        }
        size_t w = put_utf8(out + o, cap - 1 - o, cp);
        if (!w)
            break;
        o += w;
    }
    out[o] = 0;
}

/* A source over one fd (not owned) for the fd forms. */
static int fd_source(int fd, xiso_source **out)
{
    if (fd < 0)
        return XISO_EINVAL;
    return xiso_source_open_fds(&fd, 1, 0, out);
}

int xiso_probe_fd(int fd, char *title_out, size_t title_cap, uint64_t *total_bytes, int *has_default_xbe)
{
    xiso_source *src;
    int rc = fd < 0 ? XISO_EINVAL : fd_source(fd, &src);
    if (rc) {
        if (title_out && title_cap)
            title_out[0] = 0;
        if (total_bytes)
            *total_bytes = 0;
        if (has_default_xbe)
            *has_default_xbe = 0;
        return rc;
    }
    rc = xiso_probe_source(src, title_out, title_cap, total_bytes, has_default_xbe);
    xiso_source_close(src);
    return rc;
}

int xiso_probe_source(xiso_source *src, char *title_out, size_t title_cap, uint64_t *total_bytes,
                      int *has_default_xbe)
{
    if (title_out && title_cap)
        title_out[0] = 0;
    if (total_bytes)
        *total_bytes = 0;
    if (has_default_xbe)
        *has_default_xbe = 0;
    if (!src)
        return XISO_EINVAL;
    vol_t *v = malloc(sizeof *v);
    if (!v)
        return XISO_ENOMEM;
    int rc = vol_open(v, src);
    if (!rc)
        rc = scan(v);
    if (!rc) {
        if (title_out && title_cap)
            read_title(v, title_out, title_cap);
        if (total_bytes)
            *total_bytes = v->total;
        if (has_default_xbe)
            *has_default_xbe = v->has_xbe;
    }
    vol_close(v);
    free(v);
    return rc;
}

/* ── Output directory handling ───────────────────────────────────────────── */

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st; (void)flag; (void)ftw;
    remove(path);
    return 0;
}

/* rm -rf, without following symlinks. */
static void rm_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode))
        nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
    else
        unlink(path);
}

/* mkdir -p of every parent of path (not path itself). */
static int make_parents(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        int ok = mkdir(path, 0755) == 0 || errno == EEXIST;
        *p = '/';
        if (!ok)
            return write_err(errno);
    }
    return 0;
}

int xiso_extract_fd(int fd, const char *out_dir, xiso_progress_fn progress, void *user)
{
    xiso_source *src;
    if (fd < 0 || !out_dir || !out_dir[0])
        return XISO_EINVAL;
    int rc = fd_source(fd, &src);
    if (rc)
        return rc;
    rc = xiso_extract_source(src, out_dir, progress, user);
    xiso_source_close(src);
    return rc;
}

int xiso_extract_source(xiso_source *src, const char *out_dir, xiso_progress_fn progress, void *user)
{
    if (!src || !out_dir || !out_dir[0])
        return XISO_EINVAL;
    size_t len = strlen(out_dir);
    while (len > 1 && out_dir[len - 1] == '/')
        len--;
    if (len == 1 && out_dir[0] == '/')
        return XISO_EINVAL;

    char *dst = malloc(len + 1), *partial = malloc(len + 9), *old = malloc(len + 5);
    vol_t *v = malloc(sizeof *v);
    int rc = 0, dirfd = -1;
    if (!dst || !partial || !old || !v) {
        free(dst); free(partial); free(old); free(v);
        return XISO_ENOMEM;
    }
    memcpy(dst, out_dir, len);
    dst[len] = 0;
    snprintf(partial, len + 9, "%s.partial", dst);
    snprintf(old, len + 5, "%s.old", dst);

    rc = vol_open(v, src);
    if (!rc)
        rc = scan(v);                         /* validate everything before writing anything */
    if (!rc && !(v->buf = malloc(BUF_SIZE)))
        rc = XISO_ENOMEM;
    if (rc)
        goto out;

    rc = make_parents(partial);
    if (rc)
        goto out;
    rm_tree(partial);
    rm_tree(old);
    if (mkdir(partial, 0755) != 0) {
        rc = write_err(errno);
        goto out;
    }
    dirfd = open(partial, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) {
        rc = write_err(errno);
        goto fail;
    }

    seen_reset(v);
    v->extracting = 1;
    v->progress = progress;
    v->user = user;
    v->done = v->last_report = 0;
    v->path[0] = 0;
    rc = walk(v, v->root_sector, v->root_size, 0, dirfd, 0);
    close(dirfd);
    if (!rc && v->done != v->total)
        rc = XISO_ECORRUPT;                   /* the image changed between the two passes */
    if (!rc)
        rc = report(v, "");
    if (rc)
        goto fail;

    /* Swap: out_dir -> .old, .partial -> out_dir, drop .old. */
    struct stat st;
    int had_old = lstat(dst, &st) == 0;
    if (had_old && rename(dst, old) != 0) {
        rc = write_err(errno);
        goto fail;
    }
    if (rename(partial, dst) != 0) {
        rc = write_err(errno);
        if (had_old)
            rename(old, dst);
        goto fail;
    }
    if (had_old)
        rm_tree(old);
    goto out;

fail:
    rm_tree(partial);
out:
    vol_close(v);
    free(v);
    free(dst);
    free(partial);
    free(old);
    return rc;
}

/* ── Single files: open, lookup, pread ───────────────────────────────────── */

struct xiso_image {
    vol_t v;
    xiso_source *own_src;                   /* the fd form's source, closed with the image */
};

int xiso_open_source(xiso_source *src, xiso_image **out)
{
    if (!out)
        return XISO_EINVAL;
    *out = NULL;
    if (!src)
        return XISO_EINVAL;
    xiso_image *img = malloc(sizeof *img);
    if (!img)
        return XISO_ENOMEM;
    int rc = vol_open(&img->v, src);
    if (rc) {
        vol_close(&img->v);
        free(img);
        return rc;
    }
    img->own_src = NULL;
    *out = img;
    return 0;
}

int xiso_open_fd(int fd, xiso_image **out)
{
    xiso_source *src;
    if (!out)
        return XISO_EINVAL;
    *out = NULL;
    int rc = fd_source(fd, &src);
    if (rc)
        return rc;
    rc = xiso_open_source(src, out);
    if (rc) {
        xiso_source_close(src);
        return rc;
    }
    (*out)->own_src = src;
    return 0;
}

void xiso_close(xiso_image *img)
{
    if (!img)
        return;
    vol_close(&img->v);
    xiso_source_close(img->own_src);
    free(img);
}

uint64_t xiso_partition_offset(const xiso_image *img) { return img ? img->v.base : 0; }
uint64_t xiso_volume_time(const xiso_image *img) { return img ? img->v.vol_time : 0; }

int xiso_list(xiso_image *img, const xiso_stat *dir, xiso_dirent **out, size_t *count)
{
    if (!img || !out || !count)
        return XISO_EINVAL;
    *out = NULL;
    *count = 0;
    vol_t *v = &img->v;
    uint32_t sector = v->root_sector, size = v->root_size;
    if (dir) {
        if (!dir->is_dir || dir->offset < v->base || (dir->offset - v->base) % SECTOR_SIZE)
            return XISO_EINVAL;
        sector = (uint32_t)((dir->offset - v->base) / SECTOR_SIZE);
        size = dir->size;
    }
    seen_reset(v);
    entry_t *ents;
    size_t n;
    int rc = read_dir(v, sector, size, &ents, &n);
    if (rc)
        return rc;
    xiso_dirent *d = n ? malloc(n * sizeof *d) : NULL;
    if (n && !d) {
        free(ents);
        return XISO_ENOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        int is_dir = (ents[i].attr & ATTR_DIRECTORY) != 0;
        if (ents[i].size > 0 && !in_image(v, ents[i].sector, ents[i].size)) {
            free(ents);
            free(d);
            return XISO_ECORRUPT;
        }
        memcpy(d[i].name, ents[i].name, sizeof d[i].name);
        d[i].offset = v->base + (uint64_t)ents[i].sector * SECTOR_SIZE;
        d[i].size = ents[i].size;
        d[i].is_dir = is_dir;
    }
    free(ents);
    *out = d;
    *count = n;
    return 0;
}

/* ASCII case folding only: XDVDFS names are 8-bit and the kernel compares them that way. */
static int name_eq_nocase(const char *a, const char *b, size_t blen)
{
    size_t i = 0;
    for (; i < blen && a[i]; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z')
            x = (unsigned char)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (unsigned char)(y + 32);
        if (x != y)
            return 0;
    }
    return i == blen && a[i] == 0;
}

int xiso_lookup(xiso_image *img, const char *path, xiso_stat *st)
{
    if (!img || !path || !st)
        return XISO_EINVAL;
    vol_t *v = &img->v;
    seen_reset(v);
    uint32_t sector = v->root_sector, size = v->root_size;
    int is_dir = 1, depth = 0;
    const char *p = path;
    for (;;) {
        while (*p == '/' || *p == '\\')
            p++;
        if (!*p)
            break;
        const char *end = p;
        while (*end && *end != '/' && *end != '\\')
            end++;
        size_t len = (size_t)(end - p);
        if (!is_dir || len > 255)
            return XISO_ENOENT;
        if (++depth > MAX_DEPTH)
            return XISO_ENOENT;   /* deeper than any valid image can be */
        entry_t *ents;
        size_t n;
        int rc = read_dir(v, sector, size, &ents, &n);
        if (rc)
            return rc;
        const entry_t *hit = NULL;
        for (size_t i = 0; i < n; i++) {
            if (strlen(ents[i].name) == len && memcmp(ents[i].name, p, len) == 0) {
                hit = &ents[i];
                break;
            }
            if (!hit && name_eq_nocase(ents[i].name, p, len))
                hit = &ents[i];
        }
        if (!hit) {
            free(ents);
            return XISO_ENOENT;
        }
        sector = hit->sector;
        size = hit->size;
        is_dir = (hit->attr & ATTR_DIRECTORY) != 0;
        free(ents);
        p = end;
    }
    if (size > 0 && !in_image(v, sector, size))
        return XISO_ECORRUPT;
    st->offset = v->base + (uint64_t)sector * SECTOR_SIZE;
    st->size = size;
    st->is_dir = is_dir;
    return 0;
}

int xiso_pread(xiso_image *img, const xiso_stat *st, void *buf, size_t len, uint64_t off)
{
    if (!img || !st || (!buf && len))
        return XISO_EINVAL;
    if (st->is_dir)
        return XISO_EINVAL;
    if (off >= st->size)
        return 0;
    uint64_t left = st->size - off;
    if (len > left)
        len = (size_t)left;
    if (len > INT32_MAX)
        len = INT32_MAX;
    int rc = read_full(&img->v, buf, len, st->offset + off);
    return rc ? rc : (int)len;
}

/* ── Every XBE of the image (the app's companion folder) ─────────────────── */

typedef struct {
    vol_t *v;
    const char *out_dir;
    uint64_t max_bytes;
    int max_count, count;
    uint8_t *buf;
} xbes_ctx;

/* One XBE entry to out_dir/<v->path>, its directories made on the way (an existing file is replaced). */
static int xbes_copy(xbes_ctx *c, const entry_t *e)
{
    char path[sizeof c->v->path + 4096];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", c->out_dir, c->v->path) >= sizeof path)
        return XISO_EINVAL;
    int rc = make_parents(path);
    if (rc)
        return rc;
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0)
        return write_err(errno);
    uint64_t src = c->v->base + (uint64_t)e->sector * SECTOR_SIZE;
    uint32_t left = e->size;
    while (left > 0 && !rc) {
        size_t chunk = left < BUF_SIZE ? left : BUF_SIZE;
        rc = read_full(c->v, c->buf, chunk, src);
        if (!rc)
            rc = write_full(fd, c->buf, chunk);
        src += chunk;
        left -= (uint32_t)chunk;
    }
    if (close(fd) != 0 && !rc && errno != EINTR)
        rc = write_err(errno);
    if (rc)
        unlink(path);
    return rc;
}

/* Through read_dir, whose seen set is not reset during the walk: a directory table reached twice (a crafted image's
 * cycle, or two entries sharing a subtree) is XISO_ECORRUPT, and MAX_ENTRIES bounds the whole walk, so a crafted
 * tree cannot make it loop or grow exponentially. Directories deeper than XBES_DEPTH are not searched. */
#define XBES_DEPTH 8
static int xbes_walk(xbes_ctx *c, uint32_t sector, uint32_t size, int depth, size_t plen)
{
    vol_t *v = c->v;
    entry_t *ents;
    size_t n;
    int rc = read_dir(v, sector, size, &ents, &n);
    if (rc)
        return rc;
    for (size_t i = 0; i < n && !rc; i++) {
        const entry_t *e = &ents[i];
        int is_dir = (e->attr & ATTR_DIRECTORY) != 0;
        size_t nl = strlen(e->name);
        if (plen + 1 + nl >= sizeof v->path) {
            rc = XISO_ECORRUPT;
            break;
        }
        size_t sub = plen;
        if (plen)
            v->path[sub++] = '/';
        memcpy(v->path + sub, e->name, nl + 1);
        sub += nl;
        if (is_dir) {
            if (depth < XBES_DEPTH)
                rc = xbes_walk(c, e->sector, e->size, depth + 1, sub);
        } else if (nl >= 4 && strcasecmp(e->name + nl - 4, ".xbe") == 0 && e->size <= c->max_bytes
                   && c->count < c->max_count) {
            if (e->size > 0 && !in_image(v, e->sector, e->size))
                rc = XISO_ECORRUPT;
            else if ((rc = xbes_copy(c, e)) == 0)
                c->count++;
        }
        v->path[plen] = 0;
    }
    free(ents);
    return rc;
}

int xiso_extract_xbes_source(xiso_source *src, const char *out_dir, uint64_t max_bytes, int max_count)
{
    if (!src || !out_dir || !out_dir[0] || max_count < 0)
        return XISO_EINVAL;
    xiso_image *img;
    int rc = xiso_open_source(src, &img);
    if (rc)
        return rc;
    xbes_ctx c = { &img->v, out_dir, max_bytes, max_count, 0, malloc(BUF_SIZE) };
    if (!c.buf) {
        xiso_close(img);
        return XISO_ENOMEM;
    }
    seen_reset(&img->v);
    img->v.path[0] = 0;
    rc = xbes_walk(&c, img->v.root_sector, img->v.root_size, 0, 0);
    free(c.buf);
    xiso_close(img);
    return rc ? rc : c.count;
}

int xiso_extract_file_fd(int fd, const char *path, const char *out_file, uint64_t max_bytes)
{
    xiso_source *src;
    if (fd < 0 || !path || !out_file || !out_file[0])
        return XISO_EINVAL;
    int rc = fd_source(fd, &src);
    if (rc)
        return rc;
    rc = xiso_extract_file_source(src, path, out_file, max_bytes);
    xiso_source_close(src);
    return rc;
}

int xiso_extract_file_source(xiso_source *src, const char *path, const char *out_file, uint64_t max_bytes)
{
    if (!src || !path || !out_file || !out_file[0])
        return XISO_EINVAL;
    xiso_image *img;
    int rc = xiso_open_source(src, &img);
    if (rc)
        return rc;
    xiso_stat st;
    rc = xiso_lookup(img, path, &st);
    if (!rc && st.is_dir)
        rc = XISO_ENOENT;
    if (!rc && st.size > max_bytes)
        rc = XISO_EINVAL;
    uint8_t *buf = NULL;
    if (!rc && !(buf = malloc(BUF_SIZE)))
        rc = XISO_ENOMEM;
    if (rc) {
        xiso_close(img);
        return rc;
    }
    int out = open(out_file, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (out < 0) {
        rc = errno == EEXIST ? XISO_EWRITE : write_err(errno);
        free(buf);
        xiso_close(img);
        return rc;
    }
    uint64_t off = 0;
    while (off < st.size) {
        size_t want = st.size - off < BUF_SIZE ? (size_t)(st.size - off) : BUF_SIZE;
        int n = xiso_pread(img, &st, buf, want, off);
        if (n < 0) {
            rc = n;
            break;
        }
        if (n == 0) {
            rc = XISO_ECORRUPT;
            break;
        }
        rc = write_full(out, buf, (size_t)n);
        if (rc)
            break;
        off += (uint64_t)n;
    }
    if (close(out) != 0 && !rc && errno != EINTR)
        rc = write_err(errno);
    if (rc)
        unlink(out_file);
    free(buf);
    xiso_close(img);
    return rc;
}
