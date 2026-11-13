/* kernel_disc.h - D: read straight from a disc image (XBA-126).
 *
 * When the game directory handed to xbox_path_init is a disc image rather
 * than a folder -- a path to an .iso/.xiso/.cso/.zso/.cci file or one part of
 * a split image, or "fd:N[,M...]" for descriptors the Android shell opened
 * from a content URI -- the image is mounted here instead of being extracted:
 * its directory tree is read once into memory, and the file layer
 * (kernel_file.c, POSIX backend) serves D:\ and \Device\CdRom0\ from it,
 * reading each file at its offset in the image (src/xiso's sources: plain,
 * split, compressed). A folder game directory works as before.
 *
 * The path layer keeps producing host paths under the game directory
 * ("<image>/media/intro.bik"), so logs, relative opens (RootDirectory) and the
 * missing-file report are unchanged; xbox_disc_rel tells the file layer that
 * such a path belongs to the image. Lookups ignore case (ASCII), as the
 * kernel's do. The image is read-only, as the DVD is.
 *
 * Thread-safety: mount and unmount at start-up (single-threaded); every other
 * call may run on any thread. POSIX hosts only: the Win32 backend has no
 * image support (no src/xiso there).
 */
#ifndef XBOX_KERNEL_DISC_H
#define XBOX_KERNEL_DISC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The bare DVD device's synthetic handle (kernel_bridge.c, BRIDGE_CDROM_HANDLE):
 * NtReadFile on it reads the game partition's bytes when an image is mounted. */
#define XBOX_DISC_DEVICE_HANDLE 0xDECD0001u

/* Whether spec names a disc image rather than a folder: "fd:..." or a regular
 * file (after symlinks). Opens nothing. */
int xbox_disc_spec_is_image(const char *spec);

/* Mounts the image spec names (see above). Mounting the spec already mounted
 * is a no-op; another spec replaces it. Returns 0, or a negative XISO_* code
 * (xiso_strerror). RECOMP_DISC_IMAGE=0 refuses every image (-8, EINVAL). */
int xbox_disc_mount(const char *spec);
void xbox_disc_unmount(void);
int xbox_disc_mounted(void);

/* The mounted spec ("" when none), and what holds it: "iso", "cso split", ... */
const char *xbox_disc_root(void);
const char *xbox_disc_kind(void);

/* When host_path is the mounted image's root or under it: the part after
 * "<root>/" ("" for the root itself). NULL otherwise or when nothing is mounted. */
const char *xbox_disc_rel(const char *host_path);

typedef struct xbox_disc_node xbox_disc_node;

/* Looks rel up ('/' or '\\' separators, case-insensitive). NULL when it is not
 * there; then *why is 2 (the last component is missing: ERROR_FILE_NOT_FOUND)
 * or 3 (a directory on the way is missing or a file: ERROR_PATH_NOT_FOUND). */
const xbox_disc_node *xbox_disc_find(const char *rel, int *why);

int xbox_disc_node_is_dir(const xbox_disc_node *n);
uint32_t xbox_disc_node_size(const xbox_disc_node *n);
const char *xbox_disc_node_name(const xbox_disc_node *n);
uint32_t xbox_disc_node_children(const xbox_disc_node *n);
const xbox_disc_node *xbox_disc_node_child(const xbox_disc_node *n, uint32_t i);   /* on-disc order (by name) */

/* Reads up to len bytes of a file at off: the bytes read (0 at or past the
 * end), or a negative XISO_* code. */
long xbox_disc_read(const xbox_disc_node *n, void *buf, size_t len, uint64_t off);

/* The game partition as a device: bytes at off of it (what a raw sector read
 * of the DVD returns). */
long xbox_disc_raw_read(void *buf, size_t len, uint64_t off);
uint64_t xbox_disc_partition_bytes(void);

/* The volume's time stamp (FILETIME), what every entry reports. */
uint64_t xbox_disc_time(void);

/* Reads a whole file named by a host path that goes through an image -- the
 * mounted one, or a regular file met on the way ("game/default.xbe" where
 * game is an image or a link to one; it is then mounted). For the boot's XBE
 * load before xbox_path_init. Returns 0 and a malloc'd buffer, or -1. */
int xbox_disc_slurp(const char *host_path, void **data, size_t *size);

/* SCSI pass-through on the DVD device: READ(10) / READ(12) (sectors of 2048
 * bytes from the game partition) and READ CAPACITY(10). Returns 1 when it
 * answered (data written), 0 when the command is not one of those or nothing
 * is mounted. */
int xbox_disc_scsi(const uint8_t *cdb, uint8_t *data, uint32_t data_len);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_KERNEL_DISC_H */
