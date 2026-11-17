/* gc_extract.c -- what Prepare needs from a GameCube disc, out of any image format (XBA-264): see gc_extract.h. */
#define _GNU_SOURCE
#include "gc_extract.h"

#include "gc_pack.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && !strcasecmp(s + n - k, suffix);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Copies len bytes at off from the disc to the same offset of fd. */
static int copy_range(gc_disc *d, int fd, uint64_t off, uint64_t len, char *why, size_t why_n)
{
    uint8_t buf[1 << 16];
    while (len) {
        size_t k = len < sizeof buf ? (size_t)len : sizeof buf;
        if (gc_disc_read(d, off, buf, k)) {
            snprintf(why, why_n, "read at 0x%llX: %s", (unsigned long long)off, gc_disc_error(d));
            return -1;
        }
        for (size_t done = 0; done < k;) {
            ssize_t w = pwrite(fd, buf + done, k - done, (off_t)(off + done));
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                snprintf(why, why_n, "write: %s", strerror(errno));
                return -1;
            }
            done += (size_t)w;
        }
        off += k;
        len -= k;
    }
    return 0;
}

int gc_extract_sparse(gc_disc *d, const char *out_path, gc_extract_info *info, char *why, size_t why_n)
{
    gc_disc_header h;
    gc_fst fst = {0};
    gc_extract_info local;
    if (!info)
        info = &local;
    memset(info, 0, sizeof *info);
    if (gc_disc_header_read(d, &h)) {
        snprintf(why, why_n, "no GameCube header: %s", gc_disc_error(d));
        return -1;
    }
    uint32_t dol_n = gc_disc_dol_size(d);
    if (!dol_n || dol_n > 16u << 20) {
        snprintf(why, why_n, "main DOL of %u bytes", dol_n);
        return -1;
    }
    if (gc_disc_fst(d, &fst)) {
        snprintf(why, why_n, "FST: %s", gc_disc_error(d));
        return -1;
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        snprintf(why, why_n, "%s: %s", tmp, strerror(errno));
        gc_fst_free(&fst);
        return -1;
    }
    uint64_t size = gc_disc_size(d);
    int rc = 0;
    if (size && ftruncate(fd, (off_t)size) != 0) /* the rest stays holes: a few MB on disk for a 1.4 GB image */
        rc = -1, snprintf(why, why_n, "ftruncate: %s", strerror(errno));
    uint8_t app[0x20];
    uint64_t app_end = 0x2440;
    if (!rc && gc_disc_read(d, 0x2440, app, sizeof app) == 0)
        app_end = 0x2460ull + be32(app + 0x14) + be32(app + 0x18);
    if (app_end > 0x2460 + (4u << 20))
        app_end = 0x2440;
    /* boot.bin, bi2.bin, the apploader, the FST, the main DOL */
    if (!rc)
        rc = copy_range(d, fd, 0, app_end, why, why_n);
    if (!rc)
        rc = copy_range(d, fd, h.fst_off, h.fst_size, why, why_n);
    if (!rc)
        rc = copy_range(d, fd, h.dol_off, dol_n, why, why_n);
    info->dol_size = dol_n;
    /* the REL modules (and their names table, framework.str / *.str: tiny) */
    for (size_t i = 0; !rc && i < fst.n; i++) {
        const gc_fst_entry *e = &fst.e[i];
        if (e->dir || !(ends_with_ci(e->path, ".rel") || ends_with_ci(e->path, ".str")))
            continue;
        if (e->size > 32u << 20)
            continue;
        rc = copy_range(d, fd, e->offset, e->size, why, why_n);
        if (!rc && ends_with_ci(e->path, ".rel")) {
            info->rels++;
            info->rel_bytes += e->size;
        }
    }
    if (fsync(fd) != 0 && !rc)
        rc = -1, snprintf(why, why_n, "fsync: %s", strerror(errno));
    if (close(fd) != 0 && !rc)
        rc = -1, snprintf(why, why_n, "close: %s", strerror(errno));
    gc_fst_free(&fst);
    if (!rc && rename(tmp, out_path) != 0)
        rc = -1, snprintf(why, why_n, "rename: %s", strerror(errno));
    if (rc)
        unlink(tmp);
    return rc;
}

int gc_extract_dol_sha256(gc_disc *d, char hex[65])
{
    hex[0] = 0;
    gc_disc_header h;
    uint32_t n = gc_disc_dol_size(d);
    if (!n || n > 16u << 20 || gc_disc_header_read(d, &h))
        return -1;
    uint8_t *dol = malloc(n);
    if (!dol)
        return -1;
    int rc = gc_disc_read(d, h.dol_off, dol, n);
    if (!rc) {
        uint8_t sum[32];
        gc_pack_sha256(dol, n, sum);
        gc_pack_hex(sum, hex);
    }
    free(dol);
    return rc ? -1 : 0;
}
