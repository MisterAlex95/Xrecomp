/* Xbox disc image (XISO / XDVDFS) reader and extractor (XBA-40).
 *
 * The Android app lets the player pick a disc image on the phone and extracts
 * it once into the game folder, since the runtime reads plain files with POSIX
 * calls. This library does the reading and the extraction. It depends only on
 * libc and takes an open fd, so the image can come from a path or from
 * Android's ContentResolver (ParcelFileDescriptor). It only needs pread on
 * that fd, never lseek for positioning.
 *
 * Supported images:
 *   - a bare game partition (extract-xiso / xdvdfs output): descriptor at sector 32, offset 0x10000;
 *   - a full disc dump with the video partition first (redump): the game partition starts at 0x18300000.
 *     The other known bases (0xFD90, 0x30600, 0xFD90000) are probed too, the same list as tools/xiso/xdvdfs.py.
 *
 * Directories are walked through their binary tree (left / right offsets), the
 * way the Xbox kernel looks names up, so a file the tree cannot reach could not
 * be opened by the game either. Every offset and size is checked against the
 * image size. Cycles (a node reached twice, a directory listed twice) and
 * unreasonable depth or entry counts make the image ECORRUPT. So do names that
 * are empty, ".", "..", or contain '/', '\\' or control characters, and
 * duplicate names in one directory. Nothing is ever written outside out_dir.
 *
 * Thread-safety: no global state. Two calls may run at the same time on
 * different fds. The progress callback runs on the calling thread.
 *
 * Split and compressed images (XBA-126): see « Sources » at the end. Every
 * call has a source form; the fd forms read one plain file, as before.
 */
#ifndef XISO_H
#define XISO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes: always negative, 0 is success. */
#define XISO_EIO        (-1)  /* reading the image failed (I/O error) */
#define XISO_ENOTXISO   (-2)  /* no XDVDFS volume at any known partition base */
#define XISO_ECORRUPT   (-3)  /* inconsistent volume: truncated image, out-of-range offsets, cycles, bad names */
#define XISO_ENOSPC     (-4)  /* no space left (or quota exceeded) on the output device */
#define XISO_ECANCELED  (-5)  /* the progress callback asked to stop */
#define XISO_EWRITE     (-6)  /* creating, writing or renaming the output failed (not a space problem) */
#define XISO_ENOMEM     (-7)  /* out of memory */
#define XISO_EINVAL     (-8)  /* bad argument (negative fd, empty out_dir, ...) */
#define XISO_ENOENT     (-9)  /* xiso_lookup: no such file or directory in the image */

/* Extraction progress. done_bytes / total_bytes count file contents only.
 * Called when each file starts (current_path is that file, relative to the
 * image root, with '/' separators), every 4 MB within a file, and once at the
 * end with done_bytes == total_bytes and current_path "". Return nonzero to
 * cancel. */
typedef int (*xiso_progress_fn)(uint64_t done_bytes, uint64_t total_bytes,
                                const char *current_path, void *user);

/* Reads the volume from an open fd and checks the whole directory tree.
 * title_out gets default.xbe's certificate title (UTF-8, NUL-terminated,
 * truncated to title_cap - 1 bytes on a character boundary), or "" when there
 * is none. total_bytes gets the sum of the file sizes. has_default_xbe gets 1
 * when the root holds a default.xbe file (name compared case-insensitively).
 * Every output pointer may be NULL. Returns 0 or a negative XISO_* code. */
int xiso_probe_fd(int fd, char *title_out, size_t title_cap, uint64_t *total_bytes, int *has_default_xbe);

/* Extracts every file into out_dir, keeping the tree. The files go into the
 * sibling out_dir + ".partial" first (missing parents are created, a stale
 * .partial from an earlier crash is removed). Only once everything is written
 * does it replace out_dir: an existing out_dir is renamed to out_dir + ".old",
 * .partial is renamed to out_dir, and .old is deleted. So out_dir must hold
 * nothing but a previous extraction. A cancelled or failed run leaves out_dir
 * untouched and removes .partial.
 * Returns 0, XISO_ECANCELED when the callback cancelled, or another negative
 * XISO_* code. progress may be NULL. */
int xiso_extract_fd(int fd, const char *out_dir, xiso_progress_fn progress, void *user);

/* ── Reading single files without extracting (the game library's scan) ──
 *
 * xiso_open_fd reads only the volume descriptor (no tree walk), so opening a
 * multi-GB image costs one sector read. xiso_lookup then reads only the
 * directory tables on the way to one path, with the same checks as the full
 * walk (bounds, cycles, names). The fd is not owned: the caller keeps it open
 * until xiso_close and closes it afterwards. An xiso_image is used by one
 * thread at a time. */
typedef struct xiso_image xiso_image;

typedef struct {
    uint64_t offset;   /* absolute byte offset of the data (or directory table) in the image */
    uint32_t size;     /* bytes */
    int is_dir;
} xiso_stat;

/* Returns 0 and *out, or XISO_EINVAL, XISO_ENOMEM, XISO_EIO, XISO_ENOTXISO. */
int xiso_open_fd(int fd, xiso_image **out);

/* Frees img (NULL is fine). Does not close the fd. */
void xiso_close(xiso_image *img);

/* Looks a path up, relative to the image root: components separated by '/'
 * or '\\' (empty components ignored, so "" and "/" are the root), names
 * compared case-insensitively (ASCII, as the kernel does; an exact match wins
 * over a case-insensitive one). Returns 0 and *st, XISO_ENOENT when a component
 * is missing or a file is used as a directory, XISO_ECORRUPT for a bad table,
 * or another negative XISO_* code. */
int xiso_lookup(xiso_image *img, const char *path, xiso_stat *st);

/* Reads up to len bytes of a file found by xiso_lookup, from byte off of the
 * file. Clamped to the file: returns the number of bytes read (0 at or past
 * the end, at most INT32_MAX per call), XISO_EINVAL for a directory, or
 * XISO_ECORRUPT when the image ends before the file does. */
int xiso_pread(xiso_image *img, const xiso_stat *st, void *buf, size_t len, uint64_t off);

/* Copies one file of the image to out_file, created with O_EXCL (an existing
 * out_file is XISO_EWRITE and is left alone). A file larger than max_bytes is
 * XISO_EINVAL, a directory XISO_ENOENT. On any failure out_file is removed.
 * Returns 0 or a negative XISO_* code. */
int xiso_extract_file_fd(int fd, const char *path, const char *out_file, uint64_t max_bytes);

/* A static English message for an XISO_* code (or "success" for 0). */
const char *xiso_strerror(int err);

/* ── Sources: split parts and compressed images (XBA-126, xiso_source.c) ──
 *
 * A source is the byte stream of a disc image, whatever holds it:
 *   - one file (fd or path);
 *   - a FAT32 split: parts concatenated in order. By name (xiso_source_open_path): "Game.1.iso" / "Game.2.iso",
 *     "Game.part1.iso", "Game.iso.1" / "Game.iso.001", and the same with .xiso / .cso / .ciso / .zso / .cci;
 *     any part may be named, the lowest number found is the first.
 *   - a compressed image, recognised by its magic (never by its name), over one file or the split parts:
 *     CSO ("CISO", version 0/1 deflate, version 2 deflate or LZ4, the Xbox tools' LZ4 layout too), ZSO ("ZISO",
 *     LZ4) and CCI ("CCIM", LZ4). Blocks are decoded on demand into a small cache (self-contained inflate and LZ4
 *     decoders: no zlib). CHD, 7z and zip are not read.
 * xiso_source_pread is thread-safe (the runtime's threads read the disc at once); the cache has its own lock.
 * Every other xiso_* call taking a source reads it through xiso_source_pread. */
typedef struct xiso_source xiso_source;

#define XISO_SOURCE_MAX_PARTS 16

/* What a source is, for logs and the app: XISO_KIND_* (the low byte) | XISO_KIND_SPLIT when it has several parts. */
#define XISO_KIND_PLAIN   1
#define XISO_KIND_CSO     2
#define XISO_KIND_ZSO     3
#define XISO_KIND_CCI     4
#define XISO_KIND_SPLIT   0x100

/* One file or the split parts in order (n <= XISO_SOURCE_MAX_PARTS). own: close the fds in xiso_source_close.
 * Returns 0 and *out, or XISO_EINVAL, XISO_ENOMEM, XISO_EIO, XISO_ECORRUPT (a compressed header or index that does
 * not hold together). Does not look for an XDVDFS volume: xiso_open_source does. */
int xiso_source_open_fds(const int *fds, int n, int own, xiso_source **out);

/* Opens path read-only and, when its name is one of a split's parts, every other part (see above). */
int xiso_source_open_path(const char *path, xiso_source **out);

/* The split parts path belongs to, in order, into parts[i] (each cap bytes). Returns their number: 1 when path is
 * not a split part name or its siblings are missing (parts[0] = path), 0 when path does not exist, or a negative
 * XISO_* code (a name too long). */
int xiso_split_parts(const char *path, char (*parts)[1024], int max);

/* Reads len bytes at off of the image (decompressed). Returns the bytes read: len, fewer only at the end of the
 * image, or a negative XISO_* code (XISO_EIO, XISO_ECORRUPT for a block that does not decode). */
long xiso_source_pread(xiso_source *s, void *buf, size_t len, uint64_t off);

uint64_t xiso_source_size(const xiso_source *s);          /* the image's size (decompressed) */
uint64_t xiso_source_stored_size(const xiso_source *s);   /* bytes on the storage: the sum of the parts */
int xiso_source_kind(const xiso_source *s);               /* XISO_KIND_* */
int xiso_source_parts(const xiso_source *s);
const char *xiso_source_kind_name(int kind);              /* "iso", "cso", "zso", "cci" (+ " split") */
void xiso_source_close(xiso_source *s);                   /* NULL is fine */

/* The fd-based calls above, on a source (which is not owned). */
int xiso_open_source(xiso_source *src, xiso_image **out);
int xiso_probe_source(xiso_source *src, char *title_out, size_t title_cap, uint64_t *total_bytes,
                      int *has_default_xbe);
int xiso_extract_source(xiso_source *src, const char *out_dir, xiso_progress_fn progress, void *user);
int xiso_extract_file_source(xiso_source *src, const char *path, const char *out_file, uint64_t max_bytes);

/* One directory's entries (xiso_lookup gave dir, or dir NULL for the root), sorted by name (strcmp). *out is
 * malloc'd (free it), *count entries. Same checks as xiso_lookup. */
typedef struct {
    char name[256];
    uint64_t offset;   /* absolute byte offset of the data (or directory table) in the image */
    uint32_t size;
    int is_dir;
} xiso_dirent;
int xiso_list(xiso_image *img, const xiso_stat *dir, xiso_dirent **out, size_t *count);

/* The game partition: its byte offset in the image and the volume's FILETIME (100 ns since 1601, 0 when unset). */
uint64_t xiso_partition_offset(const xiso_image *img);
uint64_t xiso_volume_time(const xiso_image *img);

#ifdef __cplusplus
}
#endif

#endif /* XISO_H */
