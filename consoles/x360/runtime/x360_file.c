/* x360_file.c -- the title's files (XBA-270): the disc (a disc image, XDVDFS / GDFX through src/xiso, or a
 * directory holding its files) and an optional writable data directory, behind the kernel's file calls.
 *
 * Paths come as the kernel sees them (OBJECT_ATTRIBUTES -> ANSI_STRING): "\Device\Cdrom0\dir\file", "game:\dir\file",
 * "d:\...", or through a symbolic link the title made (ObCreateSymbolicLink). "game", "d", "dvd", "cdrom0" are the
 * disc (read only); "cache", "hdd", "e", "devkit", "partition1" the data directory when one is set (else not
 * found). A file handle is a kernel handle on a small guest object (a notification event left signalled: a wait on a
 * file handle, as after an overlapped read, returns at once), so NtClose and the handle table serve files too.
 * Reads are synchronous: the I/O status block and the optional event are set before the call returns.
 *
 * Implemented: NtCreateFile / NtOpenFile (open existing files and directories; create / overwrite in the data
 * directory), NtReadFile, NtWriteFile (data directory), NtQueryInformationFile (basic, standard, position, network
 * open), NtSetInformationFile (position, end of file), NtQueryFullAttributesFile, NtQueryDirectoryFile (one entry per
 * call, "*" / "*.ext" / exact masks), NtQueryVolumeInformationFile (size, attribute), ObCreateSymbolicLink /
 * ObDeleteSymbolicLink. Times are the disc's volume time (0 when unknown). */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "x360_internal.h"
#include "xiso.h"

#define X_STATUS_END_OF_FILE 0xC0000011u
#define X_STATUS_NO_SUCH_FILE 0xC000000Fu
#define X_STATUS_ACCESS_DENIED 0xC0000022u
#define X_STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define X_STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define X_STATUS_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define X_STATUS_NO_MORE_FILES 0x80000006u
#define X_STATUS_INFO_LENGTH_MISMATCH 0xC0000004u
#define X_STATUS_INVALID_INFO_CLASS 0xC0000003u
#define X_STATUS_FILE_IS_A_DIRECTORY 0xC00000BAu
#define X_STATUS_NOT_A_DIRECTORY 0xC0000103u

#define MAX_FILES 256
#define MAX_LINKS 32

enum { DEV_NONE = 0, DEV_DISC, DEV_DATA };

typedef struct {
    int used;
    uint32_t obj;
    int dev, is_dir, writable;
    int fd;                /* host file (disc directory or data directory), -1 for the disc image */
    xiso_stat st;          /* the disc image's entry */
    uint64_t pos, size;
    char path[1600];       /* host path (directories) or image path, for listings */
    /* directory listing */
    char **names;
    uint32_t nnames, next;
} x360_file;

struct x360_fs {
    char disc_dir[1024];
    xiso_source *src;
    xiso_image *img;
    char data_dir[1024];
    x360_file files[MAX_FILES];
    struct {
        char link[64], target[256];
    } links[MAX_LINKS];
};

static struct x360_fs *fs_of(x360_machine *x)
{
    if (!x->fs)
        x->fs = calloc(1, sizeof *x->fs);
    return x->fs;
}

int x360_set_disc(x360_machine *x, const char *path, char *why, size_t n)
{
    struct x360_fs *fs = fs_of(x);
    struct stat sb;
    char fdpath[64];
    if (path && !strncmp(path, "fd:", 3)) { /* a document the app shell opened (XBA-280): its descriptor's file */
        snprintf(fdpath, sizeof fdpath, "/proc/self/fd/%d", atoi(path + 3));
        path = fdpath;
    }
    if (stat(path, &sb) != 0) {
        snprintf(why, n, "%s: %s", path, strerror(errno));
        return -1;
    }
    if (S_ISDIR(sb.st_mode)) {
        snprintf(fs->disc_dir, sizeof fs->disc_dir, "%s", path);
        return 0;
    }
    int rc = xiso_source_open_path(path, &fs->src);
    if (rc == 0)
        rc = xiso_open_source(fs->src, &fs->img);
    if (rc) {
        snprintf(why, n, "%s: %s", path, xiso_strerror(rc));
        return -1;
    }
    return 0;
}

int x360_set_data_dir(x360_machine *x, const char *path)
{
    snprintf(fs_of(x)->data_dir, sizeof fs_of(x)->data_dir, "%s", path);
    return 0;
}

void x360_fs_free(x360_machine *x)
{
    struct x360_fs *fs = x->fs;
    if (!fs)
        return;
    for (int k = 0; k < MAX_FILES; k++) {
        if (fs->files[k].used && fs->files[k].fd >= 0)
            close(fs->files[k].fd);
        for (uint32_t j = 0; j < fs->files[k].nnames; j++)
            free(fs->files[k].names[j]);
        free(fs->files[k].names);
    }
    xiso_close(fs->img);
    xiso_source_close(fs->src);
    free(fs);
    x->fs = NULL;
}

/* ---- paths ---- */

static int ieq(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
        a++, b++;
    return !*a && !*b;
}

static int dev_of(const char *name)
{
    static const char *disc[] = {"game", "d", "dvd", "cdrom0", "cdrom", "gamedisc"};
    static const char *data[] = {"cache", "hdd", "e", "devkit", "partition1", "harddisk0", "z"};
    for (size_t k = 0; k < sizeof disc / sizeof disc[0]; k++)
        if (ieq(name, disc[k]))
            return DEV_DISC;
    for (size_t k = 0; k < sizeof data / sizeof data[0]; k++)
        if (ieq(name, data[k]))
            return DEV_DATA;
    return DEV_NONE;
}

/* Splits a kernel path into a device and a path relative to it ('/' separated). */
static int resolve(struct x360_fs *fs, const char *in, char *rel, size_t n, int depth)
{
    char p[512];
    snprintf(p, sizeof p, "%s", in);
    for (char *q = p; *q; q++)
        if (*q == '/')
            *q = '\\';
    const char *s = p;
    if (!strncmp(s, "\\??\\", 4))
        s += 4;
    /* symbolic links first */
    for (int k = 0; k < MAX_LINKS && depth < 4; k++) {
        size_t l = strlen(fs->links[k].link);
        if (l && !strncasecmp(s, fs->links[k].link, l) && (s[l] == '\\' || s[l] == 0)) {
            char t[512];
            snprintf(t, sizeof t, "%s%s", fs->links[k].target, s + l);
            return resolve(fs, t, rel, n, depth + 1);
        }
    }
    char dev[64] = "";
    const char *colon = strchr(s, ':');
    if (colon && colon - s < 63) {
        memcpy(dev, s, (size_t)(colon - s));
        dev[colon - s] = 0;
        s = colon + 1;
    } else if (!strncasecmp(s, "\\Device\\", 8)) {
        s += 8;
        const char *e = strchr(s, '\\');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l > 63)
            return DEV_NONE;
        memcpy(dev, s, l);
        dev[l] = 0;
        s += l;
        if (ieq(dev, "harddisk0")) { /* \Device\Harddisk0\Partition1 */
            const char *e2 = strchr(s + 1, '\\');
            s = e2 ? e2 : s + strlen(s);
        }
    } else {
        return DEV_NONE;
    }
    int d = dev_of(dev);
    size_t o = 0;
    for (; *s && o + 1 < n; s++) {
        if (*s == '\\') {
            if (o && rel[o - 1] != '/')
                rel[o++] = '/';
        } else
            rel[o++] = *s;
    }
    while (o && rel[o - 1] == '/')
        o--;
    rel[o] = 0;
    if (strstr(rel, "..")) /* never outside the disc / data directory */
        return DEV_NONE;
    return d;
}

static int path_of_attributes(x360_machine *x, uint32_t attrs, char *out, size_t n)
{
    if (!attrs)
        return -1;
    uint32_t root = x360_r32(x, attrs), name = x360_r32(x, attrs + 4);
    (void)root;
    if (!name)
        return -1;
    uint16_t len = x360_r16(x, name);
    uint32_t buf = x360_r32(x, name + 4);
    if (len >= n)
        len = (uint16_t)(n - 1);
    for (uint16_t k = 0; k < len; k++)
        out[k] = (char)x360_r8(x, buf + k);
    out[len] = 0;
    return 0;
}

/* ---- lookups ---- */

typedef struct {
    int found, is_dir;
    uint64_t size;
    xiso_stat st;
    char host[1600];
} lookup_t;

static int do_lookup(struct x360_fs *fs, int dev, const char *rel, lookup_t *l)
{
    memset(l, 0, sizeof *l);
    if (dev == DEV_DISC && fs->img) {
        if (xiso_lookup(fs->img, rel, &l->st) == 0) {
            l->found = 1;
            l->is_dir = l->st.is_dir;
            l->size = l->st.size;
        }
        return 0;
    }
    const char *base = dev == DEV_DISC ? fs->disc_dir : dev == DEV_DATA ? fs->data_dir : "";
    if (!base[0])
        return -1;
    snprintf(l->host, sizeof l->host, "%s/%s", base, rel);
    struct stat sb;
    if (stat(l->host, &sb) == 0) {
        l->found = 1;
        l->is_dir = S_ISDIR(sb.st_mode);
        l->size = (uint64_t)sb.st_size;
        return 0;
    }
    /* the disc's names are case-insensitive: try each component case-insensitively */
    char built[1100];
    snprintf(built, sizeof built, "%s", base);
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", rel);
    for (char *comp = strtok(tmp, "/"); comp; comp = strtok(NULL, "/")) {
        DIR *d = opendir(built);
        if (!d)
            return 0;
        struct dirent *e;
        int ok = 0;
        while ((e = readdir(d)))
            if (ieq(e->d_name, comp)) {
                size_t bl = strlen(built);
                snprintf(built + bl, sizeof built - bl, "/%s", e->d_name);
                ok = 1;
                break;
            }
        closedir(d);
        if (!ok)
            return 0;
    }
    snprintf(l->host, sizeof l->host, "%s", built);
    if (stat(l->host, &sb) == 0) {
        l->found = 1;
        l->is_dir = S_ISDIR(sb.st_mode);
        l->size = (uint64_t)sb.st_size;
    }
    return 0;
}

static x360_file *file_of(x360_machine *x, uint32_t handle)
{
    struct x360_fs *fs = x->fs;
    uint32_t o = x360_handle_obj(x, handle);
    if (!fs || !o)
        return NULL;
    for (int k = 0; k < MAX_FILES; k++)
        if (fs->files[k].used && fs->files[k].obj == o)
            return &fs->files[k];
    return NULL;
}

/* called by NtClose: release the file behind a handle (when it is one) */
void x360_file_close(x360_machine *x, uint32_t handle)
{
    x360_file *f = file_of(x, handle);
    if (!f)
        return;
    if (f->fd >= 0)
        close(f->fd);
    for (uint32_t j = 0; j < f->nnames; j++)
        free(f->names[j]);
    free(f->names);
    memset(f, 0, sizeof *f);
    f->fd = -1;
}

static void io_status(x360_machine *x, uint32_t iosb, uint32_t status, uint32_t info)
{
    if (iosb) {
        x360_w32(x, iosb, status);
        x360_w32(x, iosb + 4, info);
    }
}

#define FILE_SUPERSEDE 0
#define FILE_OPEN 1
#define FILE_CREATE 2
#define FILE_OPEN_IF 3
#define FILE_OVERWRITE 4
#define FILE_OVERWRITE_IF 5
#define FILE_DIRECTORY_FILE 0x01
#define FILE_NON_DIRECTORY_FILE 0x40

static uint32_t open_file(x360_machine *x, uint32_t handle_ptr, uint32_t attrs, uint32_t iosb, uint32_t disposition,
                          uint32_t options, uint32_t access)
{
    struct x360_fs *fs = fs_of(x);
    char kpath[512], rel[512];
    if (path_of_attributes(x, attrs, kpath, sizeof kpath))
        return X_STATUS_INVALID_PARAMETER;
    int dev = resolve(fs, kpath, rel, sizeof rel, 0);
    if (x->verbose)
        x360_logf(x, "x360: open \"%s\" -> %s \"%s\"\n", kpath,
                  dev == DEV_DISC ? "disc" : dev == DEV_DATA ? "data" : "nothing", rel);
    if (dev == DEV_NONE || (dev == DEV_DATA && !fs->data_dir[0]) ||
        (dev == DEV_DISC && !fs->img && !fs->disc_dir[0])) {
        io_status(x, iosb, X_STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return X_STATUS_OBJECT_PATH_NOT_FOUND;
    }
    lookup_t l;
    do_lookup(fs, dev, rel, &l);
    int creating = 0;
    if (!l.found) {
        if (dev != DEV_DATA || disposition == FILE_OPEN || disposition == FILE_OVERWRITE) {
            io_status(x, iosb, X_STATUS_OBJECT_NAME_NOT_FOUND, 0);
            return X_STATUS_OBJECT_NAME_NOT_FOUND;
        }
        creating = 1;
        snprintf(l.host, sizeof l.host, "%s/%s", fs->data_dir, rel);
        l.is_dir = (options & FILE_DIRECTORY_FILE) != 0;
    } else if (disposition == FILE_CREATE) {
        io_status(x, iosb, X_STATUS_OBJECT_NAME_COLLISION, 0);
        return X_STATUS_OBJECT_NAME_COLLISION;
    }
    if ((options & FILE_DIRECTORY_FILE) && !l.is_dir) {
        io_status(x, iosb, X_STATUS_NOT_A_DIRECTORY, 0);
        return X_STATUS_NOT_A_DIRECTORY;
    }
    if ((options & FILE_NON_DIRECTORY_FILE) && l.is_dir) {
        io_status(x, iosb, X_STATUS_FILE_IS_A_DIRECTORY, 0);
        return X_STATUS_FILE_IS_A_DIRECTORY;
    }
    int k = 0;
    while (k < MAX_FILES && fs->files[k].used)
        k++;
    if (k == MAX_FILES)
        return X_STATUS_NO_MEMORY;
    x360_file *f = &fs->files[k];
    memset(f, 0, sizeof *f);
    f->fd = -1;
    f->dev = dev;
    f->is_dir = l.is_dir;
    f->size = l.size;
    f->st = l.st;
    snprintf(f->path, sizeof f->path, "%s", dev == DEV_DISC && fs->img ? rel : l.host);
    if (!(dev == DEV_DISC && fs->img) && !l.is_dir) {
        int wr = dev == DEV_DATA && (access & 0x40000002u); /* GENERIC_WRITE / FILE_WRITE_DATA */
        int flags = wr ? O_RDWR : O_RDONLY;
        if (dev == DEV_DATA && (creating || disposition == FILE_SUPERSEDE || disposition == FILE_OVERWRITE ||
                                disposition == FILE_OVERWRITE_IF))
            flags = O_RDWR | O_CREAT | O_TRUNC;
        f->fd = open(l.host, flags | O_CLOEXEC, 0644);
        if (f->fd < 0) {
            io_status(x, iosb, X_STATUS_ACCESS_DENIED, 0);
            return X_STATUS_ACCESS_DENIED;
        }
        f->writable = (flags & O_ACCMODE) == O_RDWR;
        if (flags & O_TRUNC)
            f->size = 0;
    } else if (creating && l.is_dir) {
        mkdir(l.host, 0755);
    }
    f->obj = x360_kalloc(x, 16);
    x360_w8(x, f->obj, X_OBJ_EVENT_NOTIFICATION);
    x360_w32(x, f->obj + 4, 1);
    uint32_t h = x360_handle_new(x, f->obj);
    if (!h)
        return X_STATUS_NO_MEMORY;
    f->used = 1;
    x360_w32(x, handle_ptr, h);
    io_status(x, iosb, X_STATUS_SUCCESS, creating ? 2 /* FILE_CREATED */ : 1 /* FILE_OPENED */);
    return X_STATUS_SUCCESS;
}

/* ---- the kernel calls (x360_kernel.c dispatches to these) ---- */

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

void x360_k_NtCreateFile(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (handle*, access, attributes, iosb, allocation size*, file attributes, share, disposition, options) */
    uint32_t options = (uint32_t)x360_stack_arg(x, c, 8);
    RET(open_file(x, A(0), A(2), A(3), A(7), options, A(1)));
}

void x360_k_NtOpenFile(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (handle*, access, attributes, iosb, options): no share access argument, unlike Windows' NtOpenFile */
    RET(open_file(x, A(0), A(2), A(3), FILE_OPEN, A(4), A(1)));
}

static uint64_t file_offset(x360_machine *x, x360_file *f, uint32_t off_ptr)
{
    if (!off_ptr)
        return f->pos;
    uint64_t v = x360_r64(x, off_ptr);
    return v == 0xFFFFFFFFFFFFFFFEull ? f->pos : v; /* FILE_USE_FILE_POINTER_POSITION */
}

static void complete(x360_machine *x, uint32_t event, uint32_t iosb, uint32_t status, uint32_t info)
{
    io_status(x, iosb, status, info);
    if (event) {
        uint32_t o = x360_handle_obj(x, event);
        if (o)
            x360_w32(x, o + 4, 1);
    }
}

void x360_k_NtReadFile(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (handle, event, apc routine, apc context, iosb, buffer, length, byte offset*) */
    x360_file *f = file_of(x, A(0));
    if (!f) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (f->is_dir) {
        complete(x, A(1), A(4), X_STATUS_FILE_IS_A_DIRECTORY, 0);
        RET(X_STATUS_FILE_IS_A_DIRECTORY);
        return;
    }
    uint64_t off = file_offset(x, f, A(7));
    uint32_t len = A(6), buf = A(5);
    long got;
    if (off >= f->size) {
        complete(x, A(1), A(4), X_STATUS_END_OF_FILE, 0);
        RET(X_STATUS_END_OF_FILE);
        return;
    }
    if (len > f->size - off)
        len = (uint32_t)(f->size - off);
    if (f->fd >= 0)
        got = pread(f->fd, x->base + buf, len, (off_t)off);
    else
        got = xiso_pread(x->fs->img, &f->st, x->base + buf, len, off);
    if (got < 0) {
        complete(x, A(1), A(4), X_STATUS_UNSUCCESSFUL, 0);
        RET(X_STATUS_UNSUCCESSFUL);
        return;
    }
    f->pos = off + (uint64_t)got;
    complete(x, A(1), A(4), X_STATUS_SUCCESS, (uint32_t)got);
    RET(X_STATUS_SUCCESS);
}

void x360_k_NtWriteFile(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_file *f = file_of(x, A(0));
    if (!f) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (!f->writable || f->fd < 0) {
        complete(x, A(1), A(4), X_STATUS_ACCESS_DENIED, 0);
        RET(X_STATUS_ACCESS_DENIED);
        return;
    }
    uint64_t off = file_offset(x, f, A(7));
    long put = pwrite(f->fd, x->base + A(5), A(6), (off_t)off);
    if (put < 0) {
        complete(x, A(1), A(4), X_STATUS_UNSUCCESSFUL, 0);
        RET(X_STATUS_UNSUCCESSFUL);
        return;
    }
    f->pos = off + (uint64_t)put;
    if (f->pos > f->size)
        f->size = f->pos;
    complete(x, A(1), A(4), X_STATUS_SUCCESS, (uint32_t)put);
    RET(X_STATUS_SUCCESS);
}

static uint64_t ftime(x360_machine *x)
{
    return x->fs && x->fs->img ? xiso_volume_time(x->fs->img) : 0;
}

/* FILE_NETWORK_OPEN_INFORMATION: four times, allocation size, end of file, attributes */
static void network_info(x360_machine *x, uint32_t p, uint64_t size, int is_dir)
{
    for (int k = 0; k < 4; k++)
        x360_w64(x, p + 8 * (uint32_t)k, ftime(x));
    x360_w64(x, p + 32, (size + 2047) & ~2047ull);
    x360_w64(x, p + 40, size);
    x360_w32(x, p + 48, is_dir ? 0x10 : 0x80);
    x360_w32(x, p + 52, 0);
}

void x360_k_NtQueryInformationFile(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (handle, iosb, info, length, class) */
    x360_file *f = file_of(x, A(0));
    uint32_t p = A(2), len = A(3), cls = A(4), n = 0;
    if (!f) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    switch (cls) {
    case 4: /* FileBasicInformation */
        n = 40;
        if (len < n)
            break;
        for (int k = 0; k < 4; k++)
            x360_w64(x, p + 8 * (uint32_t)k, ftime(x));
        x360_w32(x, p + 32, f->is_dir ? 0x10 : 0x80);
        x360_w32(x, p + 36, 0);
        break;
    case 5: /* FileStandardInformation */
        n = 24;
        if (len < n)
            break;
        x360_w64(x, p, (f->size + 2047) & ~2047ull);
        x360_w64(x, p + 8, f->size);
        x360_w32(x, p + 16, 1);
        x360_w8(x, p + 20, 0);
        x360_w8(x, p + 21, (uint32_t)f->is_dir);
        break;
    case 14: /* FilePositionInformation */
        n = 8;
        if (len >= n)
            x360_w64(x, p, f->pos);
        break;
    case 34: /* FileNetworkOpenInformation */
        n = 56;
        if (len >= n)
            network_info(x, p, f->size, f->is_dir);
        break;
    default:
        io_status(x, A(1), X_STATUS_INVALID_INFO_CLASS, 0);
        RET(X_STATUS_INVALID_INFO_CLASS);
        return;
    }
    if (len < n) {
        io_status(x, A(1), X_STATUS_INFO_LENGTH_MISMATCH, 0);
        RET(X_STATUS_INFO_LENGTH_MISMATCH);
        return;
    }
    io_status(x, A(1), X_STATUS_SUCCESS, n);
    RET(X_STATUS_SUCCESS);
}

void x360_k_NtSetInformationFile(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_file *f = file_of(x, A(0));
    uint32_t p = A(2), cls = A(4);
    if (!f) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (cls == 14) {
        f->pos = x360_r64(x, p);
    } else if (cls == 20 && f->writable && f->fd >= 0) { /* FileEndOfFileInformation */
        uint64_t sz = x360_r64(x, p);
        if (ftruncate(f->fd, (off_t)sz) == 0)
            f->size = sz;
    } else if (cls != 4) { /* basic information (times, attributes): accepted, not kept */
        io_status(x, A(1), X_STATUS_INVALID_INFO_CLASS, 0);
        RET(X_STATUS_INVALID_INFO_CLASS);
        return;
    }
    io_status(x, A(1), X_STATUS_SUCCESS, 0);
    RET(X_STATUS_SUCCESS);
}

void x360_k_NtQueryFullAttributesFile(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    struct x360_fs *fs = fs_of(x);
    char kpath[512], rel[512];
    if (path_of_attributes(x, A(0), kpath, sizeof kpath)) {
        RET(X_STATUS_INVALID_PARAMETER);
        return;
    }
    int dev = resolve(fs, kpath, rel, sizeof rel, 0);
    lookup_t l;
    if (dev == DEV_NONE || do_lookup(fs, dev, rel, &l) || !l.found) {
        RET(X_STATUS_OBJECT_NAME_NOT_FOUND);
        return;
    }
    network_info(x, A(1), l.size, l.is_dir);
    RET(X_STATUS_SUCCESS);
}

static int match(const char *mask, const char *name)
{
    if (!mask[0] || !strcmp(mask, "*") || !strcmp(mask, "*.*"))
        return 1;
    if (mask[0] == '*') {
        size_t lm = strlen(mask + 1), ln = strlen(name);
        return ln >= lm && ieq(name + ln - lm, mask + 1);
    }
    return ieq(mask, name);
}

static int load_listing(x360_machine *x, x360_file *f)
{
    if (f->names)
        return 0;
    struct x360_fs *fs = x->fs;
    if (f->fd < 0 && fs->img) {
        xiso_dirent *ents;
        size_t count;
        xiso_stat st = f->st;
        if (xiso_list(fs->img, f->path[0] ? &st : NULL, &ents, &count))
            return -1;
        f->names = calloc(count ? count : 1, sizeof *f->names);
        for (size_t k = 0; k < count; k++)
            f->names[f->nnames++] = strdup(ents[k].name);
        free(ents);
        return 0;
    }
    DIR *d = opendir(f->path);
    if (!d)
        return -1;
    uint32_t cap = 16;
    f->names = malloc(cap * sizeof *f->names);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (f->nnames == cap)
            f->names = realloc(f->names, (cap *= 2) * sizeof *f->names);
        f->names[f->nnames++] = strdup(e->d_name);
    }
    closedir(d);
    /* sorted, like the disc's listing */
    for (uint32_t i = 1; i < f->nnames; i++)
        for (uint32_t j = i; j > 0 && strcmp(f->names[j - 1], f->names[j]) > 0; j--) {
            char *tmp = f->names[j];
            f->names[j] = f->names[j - 1];
            f->names[j - 1] = tmp;
        }
    return 0;
}

void x360_k_NtQueryDirectoryFile(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (handle, event, apc routine, apc context, iosb, info, length, file name*, restart scan) */
    x360_file *f = file_of(x, A(0));
    uint32_t p = A(5), len = A(6), mask_str = A(7), restart = (uint32_t)x360_stack_arg(x, c, 8) & 0xFF;
    if (!f || !f->is_dir) {
        RET(f ? X_STATUS_NOT_A_DIRECTORY : X_STATUS_INVALID_HANDLE);
        return;
    }
    if (load_listing(x, f)) {
        complete(x, A(1), A(4), X_STATUS_UNSUCCESSFUL, 0);
        RET(X_STATUS_UNSUCCESSFUL);
        return;
    }
    char mask[256] = "";
    if (mask_str) {
        uint16_t ml = x360_r16(x, mask_str);
        uint32_t mb = x360_r32(x, mask_str + 4);
        for (uint16_t k = 0; k < ml && k < 255; k++)
            mask[k] = (char)x360_r8(x, mb + k);
        mask[ml < 255 ? ml : 255] = 0;
    }
    if (restart)
        f->next = 0;
    while (f->next < f->nnames && !match(mask, f->names[f->next]))
        f->next++;
    if (f->next >= f->nnames) {
        complete(x, A(1), A(4), X_STATUS_NO_MORE_FILES, 0);
        RET(X_STATUS_NO_MORE_FILES);
        return;
    }
    const char *name = f->names[f->next];
    uint32_t nl = (uint32_t)strlen(name), need = 0x40 + nl;
    if (len < need) {
        complete(x, A(1), A(4), X_STATUS_INFO_LENGTH_MISMATCH, 0);
        RET(X_STATUS_INFO_LENGTH_MISMATCH);
        return;
    }
    char child[1900];
    lookup_t l;
    snprintf(child, sizeof child, "%s%s%s", f->path, f->path[0] ? "/" : "", name);
    if (f->fd < 0 && x->fs->img)
        do_lookup(x->fs, DEV_DISC, child, &l);
    else {
        struct stat sb;
        memset(&l, 0, sizeof l);
        if (stat(child, &sb) == 0) {
            l.size = (uint64_t)sb.st_size;
            l.is_dir = S_ISDIR(sb.st_mode);
        }
    }
    /* FILE_DIRECTORY_INFORMATION (names in 8-bit characters on the console) */
    x360_w32(x, p, 0);
    x360_w32(x, p + 4, f->next);
    for (int k = 0; k < 4; k++)
        x360_w64(x, p + 8 + 8 * (uint32_t)k, ftime(x));
    x360_w64(x, p + 40, l.size);
    x360_w64(x, p + 48, (l.size + 2047) & ~2047ull);
    x360_w32(x, p + 56, l.is_dir ? 0x10 : 0x80);
    x360_w32(x, p + 60, nl);
    memcpy(x->base + p + 0x40, name, nl);
    f->next++;
    complete(x, A(1), A(4), X_STATUS_SUCCESS, need);
    RET(X_STATUS_SUCCESS);
}

void x360_k_NtQueryVolumeInformationFile(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_file *f = file_of(x, A(0));
    uint32_t p = A(2), len = A(3), cls = A(4);
    if (!f) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (cls == 3 && len >= 24) { /* FileFsSizeInformation */
        x360_w64(x, p, 0x100000);
        x360_w64(x, p + 8, f->dev == DEV_DATA ? 0x80000 : 0);
        x360_w32(x, p + 16, 1);
        x360_w32(x, p + 20, 2048);
        io_status(x, A(1), X_STATUS_SUCCESS, 24);
        RET(X_STATUS_SUCCESS);
    } else if (cls == 5 && len >= 16) { /* FileFsAttributeInformation: attributes, max name length, name */
        const char *n = f->dev == DEV_DISC ? "GDFX" : "FATX";
        x360_w32(x, p, 0);
        x360_w32(x, p + 4, 255);
        x360_w32(x, p + 8, 4);
        memcpy(x->base + p + 12, n, 4);
        io_status(x, A(1), X_STATUS_SUCCESS, 16);
        RET(X_STATUS_SUCCESS);
    } else {
        io_status(x, A(1), X_STATUS_INVALID_INFO_CLASS, 0);
        RET(X_STATUS_INVALID_INFO_CLASS);
    }
}

/* save states (x360_state.c): open files by device, path and position (opened again at load), symbolic links */
void x360_fs_state(x360_machine *x, void *s, void (*u32)(void *, uint32_t *), void (*u64)(void *, uint64_t *),
                   void (*i32)(void *, int *), void (*bytes)(void *, void *, size_t), int load)
{
    struct x360_fs *fs = fs_of(x);
    for (int k = 0; k < MAX_FILES; k++) {
        x360_file *f = &fs->files[k];
        int used = f->used;
        i32(s, &used);
        if (load) {
            if (f->used && f->fd >= 0)
                close(f->fd);
            for (uint32_t j = 0; j < f->nnames; j++)
                free(f->names[j]);
            free(f->names);
            memset(f, 0, sizeof *f);
            f->fd = -1;
            f->used = used;
        }
        if (!used)
            continue;
        uint32_t off_lo = (uint32_t)f->st.offset, off_hi = (uint32_t)(f->st.offset >> 32), stsize = f->st.size;
        int stdir = f->st.is_dir;
        u32(s, &f->obj);
        i32(s, &f->dev);
        i32(s, &f->is_dir);
        i32(s, &f->writable);
        u64(s, &f->pos);
        u64(s, &f->size);
        u32(s, &off_lo);
        u32(s, &off_hi);
        u32(s, &stsize);
        i32(s, &stdir);
        u32(s, &f->next);
        bytes(s, f->path, sizeof f->path);
        if (load) {
            f->path[sizeof f->path - 1] = 0;
            f->st.offset = (uint64_t)off_hi << 32 | off_lo;
            f->st.size = stsize;
            f->st.is_dir = stdir;
            if (!f->is_dir && !(f->dev == DEV_DISC && fs->img))
                f->fd = open(f->path, (f->writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
        }
    }
    for (int k = 0; k < MAX_LINKS; k++) {
        bytes(s, fs->links[k].link, sizeof fs->links[k].link);
        bytes(s, fs->links[k].target, sizeof fs->links[k].target);
    }
}

/* XamContentCreateEx: content <name> is the directory content/<name> of the data directory, mounted on <root>:
 * 1 when it was created, 0 when it existed, -1 without a data directory */
int x360_set_content_root(x360_machine *x, const char *root, const char *name)
{
    struct x360_fs *fs = fs_of(x);
    if (!fs->data_dir[0] || !root[0] || !name[0])
        return -1;
    char path[1200];
    snprintf(path, sizeof path, "%s/content", fs->data_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/content/%s", fs->data_dir, name);
    int created = mkdir(path, 0755) == 0;
    if (!created && errno != EEXIST)
        return -1;
    char link[64], target[256];
    snprintf(link, sizeof link, "%s:", root);
    snprintf(target, sizeof target, "\\Device\\Harddisk0\\Partition1\\content\\%s", name);
    for (int k = 0; k < MAX_LINKS; k++)
        if (!fs->links[k].link[0] || !strcasecmp(fs->links[k].link, link)) {
            snprintf(fs->links[k].link, sizeof fs->links[k].link, "%s", link);
            snprintf(fs->links[k].target, sizeof fs->links[k].target, "%s", target);
            return created;
        }
    return -1;
}

static void ansi(x360_machine *x, uint32_t s, char *out, size_t n)
{
    uint16_t l = s ? x360_r16(x, s) : 0;
    uint32_t b = s ? x360_r32(x, s + 4) : 0;
    if (l >= n)
        l = (uint16_t)(n - 1);
    for (uint16_t k = 0; k < l; k++)
        out[k] = (char)x360_r8(x, b + k);
    out[l] = 0;
}

void x360_k_ObCreateSymbolicLink(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    struct x360_fs *fs = fs_of(x);
    char link[64], target[256];
    ansi(x, A(0), link, sizeof link);
    ansi(x, A(1), target, sizeof target);
    const char *l = !strncmp(link, "\\??\\", 4) ? link + 4 : link;
    for (int k = 0; k < MAX_LINKS; k++)
        if (!fs->links[k].link[0] || !strcasecmp(fs->links[k].link, l)) {
            snprintf(fs->links[k].link, sizeof fs->links[k].link, "%s", l);
            snprintf(fs->links[k].target, sizeof fs->links[k].target, "%s", target);
            RET(X_STATUS_SUCCESS);
            return;
        }
    RET(X_STATUS_NO_MEMORY);
}

void x360_k_ObDeleteSymbolicLink(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    struct x360_fs *fs = fs_of(x);
    char link[64];
    ansi(x, A(0), link, sizeof link);
    const char *l = !strncmp(link, "\\??\\", 4) ? link + 4 : link;
    for (int k = 0; k < MAX_LINKS; k++)
        if (!strcasecmp(fs->links[k].link, l))
            fs->links[k].link[0] = 0;
    RET(X_STATUS_SUCCESS);
}
