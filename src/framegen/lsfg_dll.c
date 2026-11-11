/* PE resource walker for Lossless.dll (lsfg_dll.h). Written from the PE/COFF specification; libc only. */
#define _FILE_OFFSET_BITS 64
#include "lsfg_dll.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_DIR_ENTRIES 4096u   /* per resource directory: Lossless.dll has a few hundred RCDATA entries */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* True when [off, off + n) lies inside [0, len). */
static int in_range(size_t len, uint64_t off, uint64_t n) { return off <= len && n <= len - off; }

typedef struct Pe {
    const uint8_t *buf;
    size_t len;
    const uint8_t *sections;   /* section table */
    uint32_t nsections;
    uint32_t rsrc_rva, rsrc_size;
    uint64_t rsrc_off;         /* file offset of the resource directory */
    uint32_t rsrc_avail;       /* bytes of the resource section available in the file from rsrc_off */
} Pe;

/* Maps an RVA to a file offset through the section table; *avail gets the raw bytes left in that section. */
static int rva_to_off(const Pe *pe, uint32_t rva, uint64_t *off, uint32_t *avail)
{
    for (uint32_t i = 0; i < pe->nsections; i++) {
        const uint8_t *s = pe->sections + 40u * i;
        uint32_t vsize = rd32(s + 8), va = rd32(s + 12), raw_size = rd32(s + 16), raw_ptr = rd32(s + 20);
        uint32_t span = vsize > raw_size ? vsize : raw_size;
        if (rva < va || (uint64_t)rva >= (uint64_t)va + span)
            continue;
        uint32_t delta = rva - va;
        if (delta >= raw_size)
            return LSFG_DLL_ECORRUPT;   /* in the zero-filled tail: no file data */
        if (!in_range(pe->len, (uint64_t)raw_ptr + delta, 1))
            return LSFG_DLL_ECORRUPT;
        uint64_t end = (uint64_t)raw_ptr + raw_size;
        if (end > pe->len)
            end = pe->len;
        *off = (uint64_t)raw_ptr + delta;
        *avail = (uint32_t)(end - *off);
        return 0;
    }
    return LSFG_DLL_ECORRUPT;
}

static int parse_headers(Pe *pe, const uint8_t *buf, size_t len)
{
    memset(pe, 0, sizeof *pe);
    pe->buf = buf;
    pe->len = len;
    if (len < 0x40 || buf[0] != 'M' || buf[1] != 'Z')
        return LSFG_DLL_ENOTPE;
    uint32_t pe_off = rd32(buf + 0x3c);
    if (!in_range(len, pe_off, 24) || memcmp(buf + pe_off, "PE\0\0", 4) != 0)
        return LSFG_DLL_ENOTPE;
    const uint8_t *coff = buf + pe_off + 4;
    uint32_t nsections = rd16(coff + 2);
    uint32_t opt_size = rd16(coff + 16);
    uint64_t opt_off = (uint64_t)pe_off + 24;
    if (!in_range(len, opt_off, opt_size) || opt_size < 2)
        return LSFG_DLL_ECORRUPT;
    const uint8_t *opt = buf + opt_off;
    uint16_t magic = rd16(opt);
    uint32_t ndir_at, dirs_at;
    if (magic == 0x20b) {          /* PE32+ */
        ndir_at = 108;
        dirs_at = 112;
    } else if (magic == 0x10b) {   /* PE32 */
        ndir_at = 92;
        dirs_at = 96;
    } else {
        return LSFG_DLL_ENOTPE;
    }
    if (opt_size < dirs_at)
        return LSFG_DLL_ECORRUPT;
    uint32_t ndirs = rd32(opt + ndir_at);
    if (ndirs < 3 || opt_size < dirs_at + 3 * 8)
        return LSFG_DLL_ENORSRC;
    pe->rsrc_rva = rd32(opt + dirs_at + 2 * 8);
    pe->rsrc_size = rd32(opt + dirs_at + 2 * 8 + 4);
    uint64_t sec_off = opt_off + opt_size;
    if (nsections == 0 || !in_range(len, sec_off, 40ull * nsections))
        return LSFG_DLL_ECORRUPT;
    pe->sections = buf + sec_off;
    pe->nsections = nsections;
    if (pe->rsrc_rva == 0 || pe->rsrc_size < 16)
        return LSFG_DLL_ENORSRC;
    int rc = rva_to_off(pe, pe->rsrc_rva, &pe->rsrc_off, &pe->rsrc_avail);
    if (rc)
        return rc;
    if (pe->rsrc_avail > pe->rsrc_size)
        pe->rsrc_avail = pe->rsrc_size;
    return 0;
}

/* A resource directory at offset dir (relative to the resource section): *named / *ids entry counts. */
static int read_dir(const Pe *pe, uint32_t dir, uint32_t *named, uint32_t *ids)
{
    if (!in_range(pe->rsrc_avail, dir, 16))
        return LSFG_DLL_ECORRUPT;
    const uint8_t *d = pe->buf + pe->rsrc_off + dir;
    *named = rd16(d + 12);
    *ids = rd16(d + 14);
    if (*named + *ids > MAX_DIR_ENTRIES || !in_range(pe->rsrc_avail, (uint64_t)dir + 16, 8ull * (*named + *ids)))
        return LSFG_DLL_ECORRUPT;
    return 0;
}

static const uint8_t *dir_entry(const Pe *pe, uint32_t dir, uint32_t i)
{
    return pe->buf + pe->rsrc_off + dir + 16 + 8u * i;
}

/* Follows a name-level entry down to its first language leaf: data RVA and size. */
static int leaf_of(const Pe *pe, uint32_t name_target, LsfgDllResource *res, const uint8_t **data)
{
    if (!(name_target & 0x80000000u))
        return LSFG_DLL_ECORRUPT;   /* the name level must point at a language directory */
    uint32_t lang_dir = name_target & 0x7fffffffu, named, ids;
    int rc = read_dir(pe, lang_dir, &named, &ids);
    if (rc)
        return rc;
    if (named + ids == 0)
        return LSFG_DLL_ECORRUPT;
    const uint8_t *e = dir_entry(pe, lang_dir, 0);
    uint32_t lang = rd32(e), target = rd32(e + 4);
    if (target & 0x80000000u)
        return LSFG_DLL_ECORRUPT;   /* a fourth level: not a valid resource tree */
    if (!in_range(pe->rsrc_avail, target, 16))
        return LSFG_DLL_ECORRUPT;
    const uint8_t *de = pe->buf + pe->rsrc_off + target;
    uint32_t rva = rd32(de), size = rd32(de + 4);
    uint64_t off;
    uint32_t avail;
    rc = rva_to_off(pe, rva, &off, &avail);
    if (rc)
        return rc;
    if (size > avail)
        return LSFG_DLL_ECORRUPT;
    res->lang = lang & 0x7fffffffu;
    res->size = size;
    res->file_offset = (uint32_t)off;
    *data = pe->buf + off;
    return 0;
}

/* The RT_RCDATA name directory (offset relative to the resource section). */
static int rcdata_dir(const Pe *pe, uint32_t *out)
{
    uint32_t named, ids;
    int rc = read_dir(pe, 0, &named, &ids);
    if (rc)
        return rc;
    for (uint32_t i = named; i < named + ids; i++) {
        const uint8_t *e = dir_entry(pe, 0, i);
        uint32_t type = rd32(e), target = rd32(e + 4);
        if (type != LSFG_DLL_RT_RCDATA)
            continue;
        if (!(target & 0x80000000u) || (target & 0x7fffffffu) == 0)
            return LSFG_DLL_ECORRUPT;   /* must be a subdirectory, and not the root again */
        *out = target & 0x7fffffffu;
        return 0;
    }
    return LSFG_DLL_ENORSRC;
}

int lsfg_dll_walk(const uint8_t *buf, size_t len, lsfg_dll_visit_fn fn, void *user)
{
    if (!buf || !fn)
        return LSFG_DLL_EINVAL;
    Pe pe;
    int rc = parse_headers(&pe, buf, len);
    if (rc)
        return rc;
    uint32_t dir, named, ids;
    if ((rc = rcdata_dir(&pe, &dir)) != 0 || (rc = read_dir(&pe, dir, &named, &ids)) != 0)
        return rc;
    if (ids == 0)
        return LSFG_DLL_ENORSRC;
    for (uint32_t i = named; i < named + ids; i++) {
        const uint8_t *e = dir_entry(&pe, dir, i);
        LsfgDllResource res = { .id = rd32(e) & 0x7fffffffu };
        const uint8_t *data;
        if ((rc = leaf_of(&pe, rd32(e + 4), &res, &data)) != 0)
            return rc;
        if ((rc = fn(&res, data, user)) != 0)
            return rc;
    }
    return 0;
}

struct find_ctx {
    uint32_t id;
    LsfgDllResource res;
    const uint8_t *data;
};

static int find_cb(const LsfgDllResource *res, const uint8_t *data, void *user)
{
    struct find_ctx *c = user;
    if (res->id != c->id)
        return 0;
    c->res = *res;
    c->data = data;
    return 1;
}

int lsfg_dll_find(const uint8_t *buf, size_t len, uint32_t id, LsfgDllResource *res, const uint8_t **data)
{
    struct find_ctx c = { .id = id };
    int rc = lsfg_dll_walk(buf, len, find_cb, &c);
    if (rc < 0)
        return rc;
    if (rc == 0)
        return LSFG_DLL_ENORSRC;
    if (res)
        *res = c.res;
    if (data)
        *data = c.data;
    return 0;
}

int lsfg_dll_read_fd(int fd, uint8_t **buf, size_t *len)
{
    if (fd < 0 || !buf || !len)
        return LSFG_DLL_EINVAL;
    *buf = NULL;
    *len = 0;
    struct stat st;
    if (fstat(fd, &st) != 0)
        return LSFG_DLL_EIO;
    if (st.st_size < 0)
        return LSFG_DLL_EIO;
    if ((uint64_t)st.st_size > LSFG_DLL_MAX_SIZE)
        return LSFG_DLL_ETOOBIG;
    size_t n = (size_t)st.st_size;
    uint8_t *b = malloc(n ? n : 1);
    if (!b)
        return LSFG_DLL_ENOMEM;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, b + got, n - got, (off_t)got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            free(b);
            return LSFG_DLL_EIO;
        }
        got += (size_t)r;
    }
    *buf = b;
    *len = n;
    return 0;
}

int lsfg_dll_read_path(const char *path, uint8_t **buf, size_t *len)
{
    if (!path)
        return LSFG_DLL_EINVAL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return LSFG_DLL_EIO;
    int rc = lsfg_dll_read_fd(fd, buf, len);
    close(fd);
    return rc;
}

const char *lsfg_dll_strerror(int err)
{
    switch (err) {
    case 0: return "ok";
    case LSFG_DLL_EIO: return "cannot read the file";
    case LSFG_DLL_ENOTPE: return "not a Windows DLL (PE) file";
    case LSFG_DLL_ECORRUPT: return "damaged or truncated DLL";
    case LSFG_DLL_ENORSRC: return "the DLL has no shader resources";
    case LSFG_DLL_ENOMEM: return "out of memory";
    case LSFG_DLL_EINVAL: return "invalid argument";
    case LSFG_DLL_ETOOBIG: return "file too large";
    default: return "unknown error";
    }
}
