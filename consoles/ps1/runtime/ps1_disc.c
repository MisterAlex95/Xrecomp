/* ps1_disc.c -- PlayStation disc images (P-1, XBA-159): raw sectors and tracks, nothing above them.
 *
 * Formats:
 *   .cue    FILE (BINARY / MOTOROLA, one or several files, paths relative to the .cue, case-insensitive fallback),
 *           TRACK nn MODE1/2352 | MODE2/2352 | MODE1/2048 | MODE2/2336 | AUDIO, INDEX 00 / 01, PREGAP (silence not
 *           in the file), POSTGAP (ignored). CATALOG / ISRC / FLAGS / REM / TITLE / PERFORMER / SONGWRITER skipped.
 *   .bin    one data track: raw 2352-byte sectors (sync pattern at 0), or 2048-byte sectors when it is not raw.
 *   .iso    2048-byte sectors: each is returned inside a synthesized Mode 2 Form 1 sector.
 *   .chd    MAME compressed hunks (v5 and older, every codec libchdr has) through the vendored libchdr
 *           (third_party/libchdr) when XBR_PS1_CHD is on; CHT2 / CHTR track metadata; audio is stored big-endian
 *           and swapped back.
 *   fd:N    an open descriptor holding a single-file image (raw .bin, .iso or .chd, recognised by content). The
 *           descriptor is duplicated: the caller keeps its own.
 *
 * Addresses: absolute LBA, MSF 00:02:00 = LBA 0 = track 1's INDEX 01. Track 1's pregap is the 2-second lead-in, not
 * part of the disc: an image that names one (a .cue's PREGAP or INDEX 00 on track 1, a CHD's PREGAP:150 on track 1,
 * stored or not) is read with track 1 at LBA 0 all the same (drop_lead_in).
 * A track covers [begin, end): begin is its INDEX 00 (or the start of its PREGAP), start its INDEX 01. Sectors of a
 * PREGAP not stored in a file read as zeros (audio silence; data tracks get a header and an empty Mode 2 subheader).
 * A sector that is not raw (2048 / 2336 bytes) is returned in a synthesized raw sector: sync, BCD MSF header, mode
 * 2, Form 1 subheader for 2048-byte data (EDC / ECC left zero: the controller never checks them).
 *
 * System area, ISO 9660 and SYSTEM.CNF are not read here (the core and the tools have their own readers). Our own
 * code; the .cue grammar follows the CDRWIN sheet as documented by the cuesheet tools, CHD metadata strings follow
 * MAME's chd.h (as libchdr's chd.h defines them). */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "ps1_devices.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef XBR_PS1_CHD
#include <libchdr/chd.h>
#endif

#define SECTOR 2352
#define MAX_TRACKS 99
#define MAX_FILES 99

enum { SRC_FILE, SRC_CHD };

typedef struct disc_file {
    int fd;
    uint64_t size;
} disc_file;

typedef struct disc_track {
    int audio;
    int mode;           /* 1 or 2 (data), 0 audio */
    int ssize;          /* bytes per stored sector: 2352, 2336 or 2048 */
    int swap;           /* audio stored big-endian (MOTOROLA files, CHD) */
    uint32_t begin;     /* first LBA of the track (INDEX 00 / PREGAP) */
    uint32_t start;     /* INDEX 01 */
    uint32_t end;       /* exclusive */
    uint32_t data_lba;  /* first LBA stored in the file / CHD (begin, or start when the pregap is not stored) */
    int file;           /* index into files[] (SRC_FILE) */
    uint64_t data_off;  /* SRC_FILE: byte offset of data_lba in the file; SRC_CHD: frame number of data_lba */
} disc_track;

struct ps1_disc {
    int kind;
    int ntracks;
    disc_track tracks[MAX_TRACKS];
    int nfiles;
    disc_file files[MAX_FILES];
    uint32_t size;
#ifdef XBR_PS1_CHD
    chd_file *chd;
    FILE *chd_fp;        /* fd: images (libchdr reads through a FILE we own) */
    uint8_t *hunk;
    uint32_t hunk_bytes, frames_per_hunk, hunk_cached;
#endif
};

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Sync, header (BCD MSF of lba + 150, mode) of a raw sector. */
static void make_header(uint8_t *s, uint32_t lba, int mode)
{
    uint32_t a = lba + 150;
    s[0] = 0;
    memset(s + 1, 0xFF, 10);
    s[11] = 0;
    s[12] = bcd(a / 4500);
    s[13] = bcd((a / 75) % 60);
    s[14] = bcd(a % 75);
    s[15] = (uint8_t)mode;
}

static int is_raw_sync(const uint8_t *s)
{
    static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    return memcmp(s, sync, 12) == 0;
}

static int read_full(int fd, void *buf, size_t n, uint64_t off)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
        off += (uint64_t)r;
    }
    return 0;
}

static int add_file(ps1_disc *d, int fd, char *err, size_t errlen)
{
    struct stat st;
    if (fstat(fd, &st) != 0) {
        set_err(err, errlen, "fstat: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (d->nfiles >= MAX_FILES) {
        set_err(err, errlen, "too many files");
        close(fd);
        return -1;
    }
    d->files[d->nfiles].fd = fd;
    d->files[d->nfiles].size = (uint64_t)st.st_size;
    return d->nfiles++;
}

/* ---- single-file images ------------------------------------------------------------------------------------- */

/* One data track over the whole file: raw when its first sector starts with the sync pattern, else 2048-byte. */
static int open_single(ps1_disc *d, int fd, int force_iso, char *err, size_t errlen)
{
    int fi = add_file(d, fd, err, errlen);
    if (fi < 0)
        return -1;
    uint64_t size = d->files[fi].size;
    uint8_t head[16] = {0};
    int raw = 0;
    if (!force_iso && size >= SECTOR && read_full(fd, head, sizeof head, 0) == 0 && is_raw_sync(head))
        raw = 1;
    disc_track *t = &d->tracks[0];
    t->audio = 0;
    t->ssize = raw ? SECTOR : 2048;
    t->mode = raw ? (head[15] == 1 ? 1 : 2) : 2;
    t->begin = t->start = t->data_lba = 0;
    t->end = (uint32_t)(size / (uint64_t)t->ssize);
    t->file = fi;
    t->data_off = 0;
    if (t->end == 0) {
        set_err(err, errlen, "image is empty");
        return -1;
    }
    d->ntracks = 1;
    d->size = t->end;
    return 0;
}

/* ---- .cue --------------------------------------------------------------------------------------------------- */

/* Opens dir/name, or the entry of dir whose name matches it ignoring case (cue sheets made on Windows). */
static int open_relative(const char *cue_path, const char *name, char *err, size_t errlen)
{
    char dir[4096], path[4096 + 512];
    const char *slash = strrchr(cue_path, '/');
    if (name[0] == '/' || !slash) {
        snprintf(dir, sizeof dir, "%s", name[0] == '/' ? "" : ".");
    } else {
        size_t n = (size_t)(slash - cue_path);
        if (n >= sizeof dir)
            n = sizeof dir - 1;
        memcpy(dir, cue_path, n);
        dir[n] = 0;
    }
    if (name[0] == '/')
        snprintf(path, sizeof path, "%s", name);
    else
        snprintf(path, sizeof path, "%s/%s", dir, name);
    int fd = open(path, O_RDONLY);
    if (fd >= 0)
        return fd;
    if (name[0] != '/' && !strchr(name, '/')) {
        DIR *dd = opendir(dir);
        if (dd) {
            struct dirent *e;
            while ((e = readdir(dd))) {
                if (strcasecmp(e->d_name, name) == 0) {
                    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
                    fd = open(path, O_RDONLY);
                    break;
                }
            }
            closedir(dd);
            if (fd >= 0)
                return fd;
        }
    }
    set_err(err, errlen, "cannot open track file %s: %s", path, strerror(errno));
    return -1;
}

/* Next token of a cue line: a quoted string or a run of non-blanks. Returns 0 at the end of the line. */
static int token(const char **p, char *out, size_t outlen)
{
    const char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s || *s == '\r' || *s == '\n')
        return 0;
    size_t n = 0;
    if (*s == '"') {
        s++;
        while (*s && *s != '"' && *s != '\n') {
            if (n + 1 < outlen)
                out[n++] = *s;
            s++;
        }
        if (*s == '"')
            s++;
    } else {
        while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
            if (n + 1 < outlen)
                out[n++] = *s;
            s++;
        }
    }
    out[n] = 0;
    *p = s;
    return 1;
}

static int parse_msf(const char *s, uint32_t *frames)
{
    unsigned m, sec, f;
    if (sscanf(s, "%u:%u:%u", &m, &sec, &f) != 3 || sec >= 60 || f >= 75)
        return -1;
    *frames = (m * 60 + sec) * 75 + f;
    return 0;
}

/* Track 1's INDEX 01 is LBA 0 (MSF 00:02:00) on every CD. A pregap before it (a .cue's PREGAP or INDEX 00 on track 1,
 * a CHD's « PREGAP:150 » on track 1, stored or not) is the lead-in's two seconds, outside the addressable disc: the
 * whole disc moves down so that track 1 starts at 0, and stored lead-in frames are skipped. Without this the PVD
 * (LBA 16) of such an image read 150 sectors early: no title key, no boot. */
static void drop_lead_in(ps1_disc *d)
{
    if (!d->ntracks || !d->tracks[0].start)
        return;
    disc_track *t0 = &d->tracks[0];
    uint32_t shift = t0->start;
    if (t0->data_lba < shift) {
        uint32_t skip = shift - t0->data_lba;
        t0->data_off += d->kind == SRC_CHD ? (uint64_t)skip : (uint64_t)skip * (uint64_t)t0->ssize;
        t0->data_lba = shift;
    }
    t0->begin = shift;
    for (int i = 0; i < d->ntracks; i++) {
        disc_track *t = &d->tracks[i];
        t->begin -= shift;
        t->start -= shift;
        t->data_lba -= shift;
        t->end -= shift;
    }
}

/* A cue track while parsing: positions relative to its file. */
typedef struct cue_track {
    int number, file, audio, mode, ssize, swap;
    int has_idx0, has_idx1;
    uint32_t idx0, idx1, pregap; /* frames in the file; PREGAP length */
} cue_track;

/* Frames of file fi from frame `from` (with sector size ssize) to its end. */
static uint32_t frames_left(const ps1_disc *d, int fi, uint64_t from_off, int ssize)
{
    uint64_t size = d->files[fi].size;
    return size > from_off ? (uint32_t)((size - from_off) / (uint64_t)ssize) : 0;
}

static int open_cue(ps1_disc *d, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        set_err(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    static const size_t max_tracks = MAX_TRACKS;
    cue_track ct[MAX_TRACKS];
    int n = 0, cur_file = -1, cur_swap = 0;
    char line[1024], kw[64], a[512], b[64];
    int rc = 0, lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        const char *p = line;
        if (lineno == 1 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
            p += 3; /* UTF-8 BOM */
        if (!token(&p, kw, sizeof kw))
            continue;
        for (char *k = kw; *k; k++)
            *k = (char)toupper((unsigned char)*k);
        if (strcmp(kw, "FILE") == 0) {
            if (!token(&p, a, sizeof a)) {
                set_err(err, errlen, "%s:%d: FILE without a name", path, lineno);
                rc = -1;
                break;
            }
            if (!token(&p, b, sizeof b))
                strcpy(b, "BINARY");
            for (char *k = b; *k; k++)
                *k = (char)toupper((unsigned char)*k);
            if (strcmp(b, "BINARY") != 0 && strcmp(b, "MOTOROLA") != 0) {
                set_err(err, errlen, "%s:%d: file type %s is not supported (BINARY / MOTOROLA only)", path, lineno, b);
                rc = -1;
                break;
            }
            cur_swap = strcmp(b, "MOTOROLA") == 0;
            int fd = open_relative(path, a, err, errlen);
            if (fd < 0 || (cur_file = add_file(d, fd, err, errlen)) < 0) {
                rc = -1;
                break;
            }
        } else if (strcmp(kw, "TRACK") == 0) {
            if (cur_file < 0 || (size_t)n >= max_tracks || !token(&p, a, sizeof a) || !token(&p, b, sizeof b)) {
                set_err(err, errlen, "%s:%d: bad TRACK", path, lineno);
                rc = -1;
                break;
            }
            cue_track *t = &ct[n++];
            memset(t, 0, sizeof *t);
            t->number = atoi(a);
            t->file = cur_file;
            t->swap = cur_swap;
            for (char *k = b; *k; k++)
                *k = (char)toupper((unsigned char)*k);
            if (strcmp(b, "AUDIO") == 0) {
                t->audio = 1, t->ssize = SECTOR;
            } else if (strcmp(b, "MODE1/2352") == 0) {
                t->mode = 1, t->ssize = SECTOR;
            } else if (strcmp(b, "MODE2/2352") == 0) {
                t->mode = 2, t->ssize = SECTOR;
            } else if (strcmp(b, "MODE1/2048") == 0 || strcmp(b, "MODE2/2048") == 0) {
                t->mode = 2, t->ssize = 2048;
            } else if (strcmp(b, "MODE2/2336") == 0) {
                t->mode = 2, t->ssize = 2336;
            } else {
                set_err(err, errlen, "%s:%d: track mode %s is not supported", path, lineno, b);
                rc = -1;
                break;
            }
        } else if (strcmp(kw, "INDEX") == 0) {
            uint32_t fr;
            if (!n || !token(&p, a, sizeof a) || !token(&p, b, sizeof b) || parse_msf(b, &fr) != 0) {
                set_err(err, errlen, "%s:%d: bad INDEX", path, lineno);
                rc = -1;
                break;
            }
            int idx = atoi(a);
            if (idx == 0)
                ct[n - 1].has_idx0 = 1, ct[n - 1].idx0 = fr;
            else if (idx == 1)
                ct[n - 1].has_idx1 = 1, ct[n - 1].idx1 = fr;
            /* INDEX 02.. are subdivisions of the track: nothing to do */
        } else if (strcmp(kw, "PREGAP") == 0) {
            uint32_t fr;
            if (!n || !token(&p, a, sizeof a) || parse_msf(a, &fr) != 0) {
                set_err(err, errlen, "%s:%d: bad PREGAP", path, lineno);
                rc = -1;
                break;
            }
            ct[n - 1].pregap = fr;
        }
        /* everything else (REM, CATALOG, CDTEXTFILE, TITLE, PERFORMER, SONGWRITER, ISRC, FLAGS, POSTGAP) is skipped */
    }
    fclose(f);
    if (rc)
        return rc;
    if (!n) {
        set_err(err, errlen, "%s: no track", path);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (!ct[i].has_idx1) {
            set_err(err, errlen, "%s: track %d has no INDEX 01", path, ct[i].number);
            return -1;
        }
    }

    /* Absolute positions. file_base: LBA of frame 0 of the current file; gaps: PREGAP frames inserted so far. A
     * file's frame f is at byte offset off(f), sector sizes changing at each track's first stored frame. */
    uint32_t file_base = 0, gaps = 0;
    uint64_t tr_off = 0; /* byte offset of the first stored frame of the previous track of the same file */
    uint32_t tr_frame = 0; /* its frame number in the file */
    int tr_ssize = SECTOR;
    for (int i = 0; i < n; i++) {
        cue_track *c = &ct[i];
        disc_track *t = &d->tracks[i];
        if (i > 0 && c->file != ct[i - 1].file) {
            /* the previous file ends: its frame count moves the base */
            uint32_t prev_frames = tr_frame + frames_left(d, ct[i - 1].file, tr_off, tr_ssize);
            file_base += prev_frames;
            tr_off = 0, tr_frame = 0;
        } else if (i == 0) {
            tr_off = 0, tr_frame = 0;
        }
        uint32_t first = c->has_idx0 ? c->idx0 : c->idx1; /* first stored frame of this track */
        if (first < tr_frame || c->idx1 < first) {
            set_err(err, errlen, "%s: track %d indexes go backwards", path, c->number);
            return -1;
        }
        tr_off += (uint64_t)(first - tr_frame) * (uint64_t)tr_ssize;
        tr_frame = first;
        tr_ssize = c->ssize;
        gaps += c->pregap;
        t->audio = c->audio;
        t->mode = c->mode;
        t->ssize = c->ssize;
        t->swap = c->audio && c->swap;
        t->file = c->file;
        t->start = file_base + gaps + c->idx1;
        t->data_lba = file_base + gaps + first;
        t->begin = t->data_lba - c->pregap;
        t->data_off = tr_off;
    }
    for (int i = 0; i < n; i++) {
        disc_track *t = &d->tracks[i];
        if (i + 1 < n && d->tracks[i + 1].file == t->file) {
            t->end = d->tracks[i + 1].begin;
        } else {
            t->end = t->data_lba + frames_left(d, t->file, t->data_off, t->ssize);
            if (i + 1 < n && t->end > d->tracks[i + 1].begin)
                t->end = d->tracks[i + 1].begin;
        }
        if (t->end < t->start) {
            set_err(err, errlen, "%s: track %d is past the end of its file", path, ct[i].number);
            return -1;
        }
    }
    d->ntracks = n;
    drop_lead_in(d);
    d->size = d->tracks[n - 1].end;
    return 0;
}

/* ---- .chd --------------------------------------------------------------------------------------------------- */

#ifdef XBR_PS1_CHD
#define CHD_FRAME (2352 + 96)

static int open_chd(ps1_disc *d, const char *path, int fd, char *err, size_t errlen)
{
    chd_error e;
    if (fd >= 0) {
        int nfd = dup(fd);
        d->chd_fp = nfd >= 0 ? fdopen(nfd, "rb") : NULL;
        if (!d->chd_fp) {
            if (nfd >= 0)
                close(nfd);
            set_err(err, errlen, "fd: %s", strerror(errno));
            return -1;
        }
        e = chd_open_file(d->chd_fp, CHD_OPEN_READ, NULL, &d->chd);
    } else {
        e = chd_open(path, CHD_OPEN_READ, NULL, &d->chd);
    }
    if (e != CHDERR_NONE) {
        d->chd = NULL;
        set_err(err, errlen, "chd: %s", chd_error_string(e));
        return -1;
    }
    const chd_header *h = chd_get_header(d->chd);
    if (!h || h->hunkbytes < CHD_FRAME || h->hunkbytes % CHD_FRAME) {
        set_err(err, errlen, "chd: not a CD image (hunk of %u bytes)", h ? h->hunkbytes : 0);
        return -1;
    }
    d->hunk_bytes = h->hunkbytes;
    d->frames_per_hunk = h->hunkbytes / CHD_FRAME;
    d->hunk = malloc(d->hunk_bytes);
    d->hunk_cached = UINT32_MAX;
    if (!d->hunk) {
        set_err(err, errlen, "out of memory");
        return -1;
    }
    uint32_t lba = 0;
    uint64_t frame = 0;
    for (int i = 0; i < MAX_TRACKS; i++) {
        char meta[512], type[32] = "", sub[32] = "", pgtype[32] = "", pgsub[32] = "";
        uint32_t len = 0, tag = 0;
        uint8_t flags;
        int num = 0, frames = 0, pregap = 0, postgap = 0;
        memset(meta, 0, sizeof meta);
        if (chd_get_metadata(d->chd, CDROM_TRACK_METADATA2_TAG, (uint32_t)i, meta, sizeof meta - 1, &len, &tag,
                             &flags) == CHDERR_NONE) {
            if (sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &num, type, sub, &frames, &pregap, pgtype, pgsub,
                       &postgap) < 4) {
                set_err(err, errlen, "chd: bad track metadata '%s'", meta);
                return -1;
            }
        } else if (chd_get_metadata(d->chd, CDROM_TRACK_METADATA_TAG, (uint32_t)i, meta, sizeof meta - 1, &len, &tag,
                                    &flags) == CHDERR_NONE) {
            if (sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &num, type, sub, &frames) != 4) {
                set_err(err, errlen, "chd: bad track metadata '%s'", meta);
                return -1;
            }
        } else {
            break;
        }
        disc_track *t = &d->tracks[i];
        memset(t, 0, sizeof *t);
        if (strcmp(type, "AUDIO") == 0) {
            t->audio = 1, t->ssize = SECTOR, t->swap = 1;
        } else if (strcmp(type, "MODE1_RAW") == 0 || strcmp(type, "MODE2_RAW") == 0) {
            t->mode = type[4] == '1' ? 1 : 2, t->ssize = SECTOR;
        } else if (strcmp(type, "MODE1") == 0 || strcmp(type, "MODE2_FORM1") == 0) {
            t->mode = 2, t->ssize = 2048;
        } else if (strcmp(type, "MODE2") == 0 || strcmp(type, "MODE2_FORM_MIX") == 0) {
            t->mode = 2, t->ssize = 2336;
        } else {
            set_err(err, errlen, "chd: track type %s is not supported", type);
            return -1;
        }
        int pg_stored = pgtype[0] == 'V';
        t->begin = lba;
        t->start = lba + (uint32_t)pregap;
        t->data_lba = pg_stored ? t->begin : t->start;
        t->data_off = frame;
        uint32_t stored = (uint32_t)frames; /* FRAMES includes a stored pregap */
        t->end = t->data_lba + stored;
        lba = t->end + (uint32_t)postgap;
        frame += ((uint64_t)stored + 3) & ~(uint64_t)3; /* CD_TRACK_PADDING */
        d->ntracks = i + 1;
    }
    if (!d->ntracks) {
        set_err(err, errlen, "chd: no CD track metadata");
        return -1;
    }
    drop_lead_in(d);
    d->size = d->tracks[d->ntracks - 1].end;
    return 0;
}

static int chd_frame(ps1_disc *d, uint64_t frame, uint8_t *out, size_t bytes)
{
    uint32_t hunk = (uint32_t)(frame / d->frames_per_hunk);
    if (hunk != d->hunk_cached) {
        if (chd_read(d->chd, hunk, d->hunk) != CHDERR_NONE) {
            d->hunk_cached = UINT32_MAX;
            return -1;
        }
        d->hunk_cached = hunk;
    }
    memcpy(out, d->hunk + (frame % d->frames_per_hunk) * CHD_FRAME, bytes);
    return 0;
}
#endif

/* ---- public ------------------------------------------------------------------------------------------------- */

static int has_ext(const char *path, const char *ext)
{
    size_t n = strlen(path), e = strlen(ext);
    return n > e && strcasecmp(path + n - e, ext) == 0;
}

ps1_disc *ps1_disc_open(const char *path, char *err, size_t errlen)
{
    if (!path) {
        set_err(err, errlen, "no path");
        return NULL;
    }
    ps1_disc *d = calloc(1, sizeof *d);
    if (!d) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    int rc;
    if (strncmp(path, "fd:", 3) == 0) {
        char *endp;
        long n = strtol(path + 3, &endp, 10);
        if (endp == path + 3 || *endp || n < 0) {
            set_err(err, errlen, "bad descriptor '%s'", path);
            free(d);
            return NULL;
        }
        uint8_t magic[8] = {0};
        int is_chd = read_full((int)n, magic, 8, 0) == 0 && memcmp(magic, "MComprHD", 8) == 0;
        if (is_chd) {
#ifdef XBR_PS1_CHD
            d->kind = SRC_CHD;
            rc = open_chd(d, NULL, (int)n, err, errlen);
#else
            set_err(err, errlen, "CHD support is not built in (XBR_PS1_CHD)");
            rc = -1;
#endif
        } else {
            int fd = dup((int)n);
            rc = fd < 0 ? (set_err(err, errlen, "dup: %s", strerror(errno)), -1) : open_single(d, fd, 0, err, errlen);
        }
    } else if (has_ext(path, ".cue")) {
        rc = open_cue(d, path, err, errlen);
    } else if (has_ext(path, ".chd")) {
#ifdef XBR_PS1_CHD
        d->kind = SRC_CHD;
        rc = open_chd(d, path, -1, err, errlen);
#else
        set_err(err, errlen, "CHD support is not built in (XBR_PS1_CHD)");
        rc = -1;
#endif
    } else {
        int fd = open(path, O_RDONLY);
        uint8_t magic[8] = {0};
        if (fd < 0) {
            set_err(err, errlen, "cannot open %s: %s", path, strerror(errno));
            rc = -1;
        } else if (read_full(fd, magic, 8, 0) == 0 && memcmp(magic, "MComprHD", 8) == 0) {
            /* a CHD under another name: by content, as for fd: */
            close(fd);
#ifdef XBR_PS1_CHD
            d->kind = SRC_CHD;
            rc = open_chd(d, path, -1, err, errlen);
#else
            set_err(err, errlen, "CHD support is not built in (XBR_PS1_CHD)");
            rc = -1;
#endif
        } else {
            rc = open_single(d, fd, has_ext(path, ".iso"), err, errlen);
        }
    }
    if (rc) {
        ps1_disc_close(d);
        return NULL;
    }
    return d;
}

void ps1_disc_close(ps1_disc *d)
{
    if (!d)
        return;
    for (int i = 0; i < d->nfiles; i++)
        close(d->files[i].fd);
#ifdef XBR_PS1_CHD
    if (d->chd)
        chd_close(d->chd);
    if (d->chd_fp)
        fclose(d->chd_fp);
    free(d->hunk);
#endif
    free(d);
}

int ps1_disc_read_sector(ps1_disc *d, uint32_t lba, uint8_t out[2352])
{
    if (!d || lba >= d->size)
        return -1;
    const disc_track *t = NULL;
    for (int i = 0; i < d->ntracks; i++) {
        if (lba >= d->tracks[i].begin && lba < d->tracks[i].end) {
            t = &d->tracks[i];
            break;
        }
    }
    if (!t || lba < t->data_lba) {
        /* a gap no file stores (PREGAP / POSTGAP) */
        memset(out, 0, SECTOR);
        if (t && !t->audio)
            make_header(out, lba, 2);
        return 0;
    }
    uint64_t rel = lba - t->data_lba;
    uint8_t *dst = out;
    size_t bytes = (size_t)t->ssize;
    if (t->ssize == 2048) {
        memset(out, 0, SECTOR);
        make_header(out, lba, 2);
        out[18] = out[22] = 0x08; /* subheader: data, Form 1 */
        dst = out + 24;
    } else if (t->ssize == 2336) {
        memset(out, 0, SECTOR);
        make_header(out, lba, 2);
        dst = out + 16;
    }
    int rc;
#ifdef XBR_PS1_CHD
    if (d->kind == SRC_CHD)
        rc = chd_frame(d, t->data_off + rel, dst, bytes);
    else
#endif
        rc = read_full(d->files[t->file].fd, dst, bytes, t->data_off + rel * (uint64_t)t->ssize);
    if (rc)
        return -1;
    if (t->swap) {
        for (size_t i = 0; i + 1 < SECTOR; i += 2) {
            uint8_t x = out[i];
            out[i] = out[i + 1];
            out[i + 1] = x;
        }
    }
    return 0;
}

int ps1_disc_track_count(ps1_disc *d) { return d ? d->ntracks : 0; }

int ps1_disc_track(ps1_disc *d, int t, uint32_t *start_lba, int *audio)
{
    if (!d || t < 1 || t > d->ntracks)
        return -1;
    if (start_lba)
        *start_lba = d->tracks[t - 1].start;
    if (audio)
        *audio = d->tracks[t - 1].audio;
    return 0;
}

uint32_t ps1_disc_size(ps1_disc *d) { return d ? d->size : 0; }

int ps1_disc_track_begin(ps1_disc *d, int t, uint32_t *begin_lba)
{
    if (!d || t < 1 || t > d->ntracks)
        return -1;
    if (begin_lba)
        *begin_lba = d->tracks[t - 1].begin;
    return 0;
}
