/*
 * xbox_state.h - Xbox save states, part 1: capture the guest state and the state format (XBA-245)
 *
 * Design: docs/technical/xbox-save-states.md. With every guest thread frozen at a kernel boundary
 * (kernel_park.h), a capture writes one blob, "XBXS", of tagged sections:
 *
 *   header   "XBXS", format version, header size, section count, the identity (title ID, SHA-1 of the
 *            XBE's headers, build ID of the binary, toolkit version, RAM / mapped / contiguous sizes,
 *            sizeof(RecompCpu)), when it was taken, a hash of the header
 *   sections each: u32 tag, u32 version, u64 length, u64 hash of the payload, then the payload
 *
 * All integers little-endian (every host we build for is). A state only loads into the binary that
 * saved it (the build ID; call-site addresses and lifted frames are part of a thread's state), so a
 * section's payload may use this build's layouts (a raw RecompCpu); its version changes when its own
 * format does.
 *
 * Built-in sections (this file's ticket):
 *   CPU   per guest thread its park record (kernel_park.h): the call it is in (ordinal, guest return
 *         address) or the host wait inside lifted code (site kind and guest VA, XBA-251), whether it
 *         returned and its status, the RecompCpu, the TIB, the KTHREAD, the guest stack bounds and the
 *         guest thread id read from the KTHREAD
 *   MEM   the memory manager's host-side state (xbox_MemStateSave: heap, contiguous window, lazy
 *         reservations, guest_vmem)
 *   RAM   guest memory, range by range (xbox_StateGuestRanges): a bitmap of the pages stored (not all
 *         zero), then those pages. No compressor in the tree (the console runtimes skip zero pages the
 *         same way, n64_state.c); rewind codes XOR deltas on top (recomp_rewind.h)
 *
 * Other components add their own sections (XBA-246 kernel objects and files, XBA-248 NV2A, APU, USB):
 * a section is a tag, a version, a save callback (runs during the capture, every guest thread frozen)
 * and a check callback (validates a payload without restoring anything). xbox_StateRegister, once,
 * before the first capture (an init function of the component). XBA-249 adds the load callbacks.
 *
 * Reading a state checks, in order: the magic and format version, the header hash, the identity
 * against the running title (another title or XBE: EGAME; another build, toolkit or memory size:
 * EVERSION), then every section: its bounds and hash (ECORRUPT), that it is registered here with the
 * same version (EVERSION), its check callback; and that every registered section is present. Nothing is
 * restored yet (XBA-249).
 *
 * The slot file is recomp_states' (XRSLOT header, console "xbox", game key the title ID in hex,
 * thumbnail), with the XBXS blob as the core blob. POSIX hosts (recomp_states is not built on Windows).
 */
#ifndef XBOX_STATE_H
#define XBOX_STATE_H

#include <stddef.h>
#include <stdint.h>

#include "kernel_park.h"
#include "xbox_memory_layout.h"   /* RecompCpu */

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_STATE_VERSION 1
#define XBOX_STATE_HEADER_BYTES 128u
#define XBOX_STATE_SECTION_HEAD 24u
#define XBOX_STATE_MAX_SECTIONS 32

#define XBOX_STATE_TAG(a, b, c, d) \
    ((uint32_t)(uint8_t)(a) | (uint32_t)(uint8_t)(b) << 8 | (uint32_t)(uint8_t)(c) << 16 | (uint32_t)(uint8_t)(d) << 24)
#define XBOX_STATE_TAG_CPU  XBOX_STATE_TAG('C', 'P', 'U', ' ')
#define XBOX_STATE_TAG_MEM  XBOX_STATE_TAG('M', 'E', 'M', ' ')
#define XBOX_STATE_TAG_RAM  XBOX_STATE_TAG('R', 'A', 'M', ' ')
/* Reserved for the sibling tickets (the design's names). */
#define XBOX_STATE_TAG_KOBJ XBOX_STATE_TAG('K', 'O', 'B', 'J')   /* XBA-246 */
#define XBOX_STATE_TAG_FILE XBOX_STATE_TAG('F', 'I', 'L', 'E')   /* XBA-246 */
#define XBOX_STATE_TAG_NV2A XBOX_STATE_TAG('N', 'V', '2', 'A')   /* XBA-248 */
#define XBOX_STATE_TAG_APU  XBOX_STATE_TAG('A', 'P', 'U', ' ')   /* XBA-248 */
#define XBOX_STATE_TAG_USB  XBOX_STATE_TAG('U', 'S', 'B', ' ')   /* XBA-248 */

/* CPU v2 (XBA-251): the park site kind (XboxParkRecord.site) in each thread's head; v1 is refused. */
#define XBOX_STATE_CPU_VERSION 2
#define XBOX_STATE_RAM_VERSION 1
/* MEM: XBOX_MEM_STATE_VERSION (xbox_memory_layout.h) */

/* Results. The first ones are recomp_states.h's RECOMP_STATE_* (the menu shows a message per code). */
enum {
    XBOX_STATE_OK = 0,
    XBOX_STATE_EGAME = -3,      /* another title or another XBE */
    XBOX_STATE_EVERSION = -4,   /* another format, build, toolkit, memory size or section version */
    XBOX_STATE_EIO = -5,
    XBOX_STATE_EEMPTY = -2,     /* no slot file */
    XBOX_STATE_ECORRUPT = -7,   /* not a state, truncated, or a hash does not match */
    XBOX_STATE_EFREEZE = -20,   /* a guest thread did not reach a kernel call in time */
    XBOX_STATE_EBUSY = -21,     /* not now: a thread at DISPATCH_LEVEL, untracked threads, the dispatch
                                 * gate held, a capture already running, or called from a guest thread */
    XBOX_STATE_ENOMEM = -22,
    XBOX_STATE_ESECTION = -23,  /* a section's save callback failed */
    XBOX_STATE_EUNSTABLE = -24, /* kernel calls kept returning during every attempt */
};
const char *xbox_StateError(int code);

/* ---- Writing and reading payloads ----------------------------------------------------------------------- */

typedef struct XboxStateWriter {
    uint8_t *buf;
    size_t n, cap;
    int failed;              /* out of memory: everything after is dropped */
} XboxStateWriter;

/* n bytes at the end of the blob, to fill in (NULL when out of memory). The pointer is valid until the
 * next call on w: keep offsets (w->n), not pointers, across calls. */
uint8_t *xbox_StateReserve(XboxStateWriter *w, size_t n);
void xbox_StatePut(XboxStateWriter *w, const void *p, size_t n);
void xbox_StatePut32(XboxStateWriter *w, uint32_t v);
void xbox_StatePut64(XboxStateWriter *w, uint64_t v);

typedef struct XboxStateReader {
    const uint8_t *p;
    size_t n, at;
    int bad;                 /* read past the end */
} XboxStateReader;

uint32_t xbox_StateGet32(XboxStateReader *r);
uint64_t xbox_StateGet64(XboxStateReader *r);
/* n bytes, or NULL (and r->bad) when fewer are left. */
const uint8_t *xbox_StateGet(XboxStateReader *r, size_t n);

/* The hash sections and the header carry (64 bits, four lanes, not cryptographic). */
uint64_t xbox_StateHash(const void *p, size_t n);

/* ---- Sections ------------------------------------------------------------------------------------------- */

/* What a section's save callback can see: the frozen threads' records (kernel_park.h). */
typedef struct XboxStateContext {
    const XboxParkRecord *records;   /* xbox_StateContextThread reads them with their guest-side location */
    int threads;
} XboxStateContext;

typedef struct XboxStateSection {
    uint32_t tag;            /* XBOX_STATE_TAG(...), not 0 */
    uint32_t version;        /* of this section's payload */
    const char *name;        /* for logs (kept as a pointer: a string literal) */
    /* Writes the payload during a capture, every guest thread frozen. XBOX_STATE_OK, or an error (the
     * capture fails with XBOX_STATE_ESECTION). A capture may be attempted more than once. */
    int (*save)(XboxStateWriter *w, const XboxStateContext *ctx, void *user);
    /* Validates a payload of `version` without restoring it: XBOX_STATE_OK, _ECORRUPT or _EVERSION. NULL:
     * any payload is accepted. */
    int (*check)(const uint8_t *p, size_t n, uint32_t version, void *user);
    void *user;
} XboxStateSection;

/* Adds a section (or replaces the one with the same tag), written after the ones registered before it
 * (CPU, MEM, RAM come first). 0, or -1 (tag 0, no save callback, table full). Any thread; not during a
 * capture. */
int  xbox_StateRegister(const XboxStateSection *s);
void xbox_StateUnregister(uint32_t tag);
/* The registered section with that tag, or NULL. */
const XboxStateSection *xbox_StateFindSection(uint32_t tag);

/* ---- Identity ------------------------------------------------------------------------------------------- */

typedef struct XboxStateIdentity {
    uint32_t title_id;
    uint8_t xbe_sha1[20];    /* xbox_XbeHeaderSha1 */
    uint8_t build_id_len;    /* 0: unknown */
    uint8_t build_id[32];
    char toolkit[16];        /* "0.14.0" */
    uint32_t ram_size, mapped_size, contig_size;
    uint32_t cpu_bytes;      /* sizeof(RecompCpu) */
} XboxStateIdentity;

/* The running title's identity. The build ID is the binary's (ELF NT_GNU_BUILD_ID of the module this code
 * is linked into, the executable or the app's libmain.so; Mach-O LC_UUID), unless set. */
void xbox_StateIdentityGet(XboxStateIdentity *out);
/* Overrides the build ID (n bytes, at most 32; n 0: back to the binary's). Hosts without one, tests. */
void xbox_StateSetBuildId(const uint8_t *id, size_t n);

/* ---- Capture -------------------------------------------------------------------------------------------- */

typedef struct XboxStateStats {
    double freeze_ms;        /* until every guest thread was parked */
    double settle_ms;        /* until the non-blocking calls in progress had returned */
    double capture_ms;       /* the sections, all attempts */
    int attempts;            /* 1 when no call returned during the first */
    int threads;
    uint32_t ram_ranges, ram_pages, ram_stored, ram_changed;   /* changed: since the previous capture */
    size_t ram_bytes, mem_bytes, cpu_bytes, blob_bytes;
    char reason[192];        /* why it failed, or "" */
} XboxStateStats;

/* Freezes every guest thread (xbox_ParkFreeze, timeout_ms), captures, thaws. *blob is malloc'ed (free()).
 * Logs one line ("[STATE] capture: ..."). Not from a guest thread. XBOX_STATE_*. */
int xbox_StateCapture(uint32_t timeout_ms, uint8_t **blob, size_t *blob_n, XboxStateStats *st);
/* The same with the freeze already held by the caller (xbox_ParkFreeze returned 1; it stays held). */
int xbox_StateCaptureFrozen(uint8_t **blob, size_t *blob_n, XboxStateStats *st);

/* For a section's save callback: thread i's park record and guest-side location. */
typedef struct XboxStateThread {
    int slot, state;
    uint32_t ordinal, ret_va;
    int returned;
    uint32_t status;
    int dispatch;
    uint32_t tid;            /* host thread id (low 32 bits) */
    uint64_t kernel_calls;
    uint32_t tib, kthread;   /* fs base, the KTHREAD at fs:[0x28] (0 when outside guest memory) */
    uint32_t stack_base, stack_limit;   /* the guest stack: KTHREAD StackBase (+0x1C) / StackLimit (+0x20) */
    uint32_t guest_tid;      /* KTHREAD UniqueThread (+0x12C): the id the title sees */
    RecompCpu cpu;
    uint32_t site;           /* XBOX_PARK_SITE_*: KERNEL (ordinal, ret_va the call's return address) or a
                              * host wait inside lifted code (ordinal 0, ret_va the site's VA), XBA-251 */
} XboxStateThread;
int xbox_StateContextThread(const XboxStateContext *ctx, int i, XboxStateThread *out);

/* ---- Reading -------------------------------------------------------------------------------------------- */

typedef struct XboxStateInfo {
    uint32_t version;
    XboxStateIdentity id;
    int64_t saved_ms;
    int sections;
    struct { uint32_t tag, version; uint64_t length; } section[XBOX_STATE_MAX_SECTIONS];
    char reason[192];        /* why it was refused, or "" */
} XboxStateInfo;

/* The header alone (magic, version, header hash) into info: XBOX_STATE_OK, _ECORRUPT, _EVERSION. No
 * identity comparison: the menu lists any slot. */
int xbox_StateParseHeader(const uint8_t *blob, size_t n, XboxStateInfo *info);
/* Everything (see the top of this file) against the running title. info may be NULL. */
int xbox_StateCheck(const uint8_t *blob, size_t n, XboxStateInfo *info);
/* A section's payload: XBOX_STATE_OK with *p, *len, *version, or _EEMPTY when the blob has none. Assumes
 * a blob xbox_StateCheck accepted (bounds are still checked). */
int xbox_StateSection(const uint8_t *blob, size_t n, uint32_t tag, const uint8_t **p, size_t *len,
                      uint32_t *version);

/* The built-in sections' payloads, parsed (tests, XBA-249). */
int xbox_StateCpuThreads(const uint8_t *p, size_t n, XboxStateThread *out, int max);   /* count or -1 */
typedef struct XboxStateRamInfo {
    uint32_t ranges, pages, stored;
} XboxStateRamInfo;
int xbox_StateRamInfo(const uint8_t *p, size_t n, XboxStateRamInfo *info);   /* 0 or -1 */
/* The stored copy of guest page `va` (4 KB aligned) in a RAM payload: 1 with *page (NULL: the page was all
 * zero), 0 when no range covers it, -1 when the payload is malformed. */
int xbox_StateRamPage(const uint8_t *p, size_t n, uint32_t va, const uint8_t **page);

/* ---- Slot files ----------------------------------------------------------------------------------------- */

/* Writes a slot file (recomp_states_write): console "xbox", key the title ID ("%08X"), the thumbnail
 * (tw x th RGB565, may be NULL), the blob. XBOX_STATE_OK or _EIO. */
int xbox_StateWriteSlot(const char *path, const uint8_t *blob, size_t n, const uint16_t *thumb, int tw, int th);
/* Captures (xbox_StateCapture) and writes a slot file. */
int xbox_StateSaveSlot(const char *path, uint32_t timeout_ms, const uint16_t *thumb, int tw, int th,
                       XboxStateStats *st);
/* Reads a slot file and checks it (xbox_StateCheck); *blob (malloc'ed) may be NULL to only check. */
int xbox_StateReadSlot(const char *path, uint8_t **blob, size_t *n, XboxStateInfo *info);

/* The measurement probe (opt-in diagnostics): RECOMP_STATE_PROBE=<seconds> starts a host thread that takes a
 * capture every that many seconds (the « [STATE] capture: » line: time, blob size, pages changed); with
 * RECOMP_STATE_PROBE_DIR=<dir> it also writes <dir>/probe.st and reads it back ("[STATE] probe: ...").
 * Called once by xbox_kernel_bridge_init; does nothing without the variable. */
void xbox_StateProbeStart(void);

/* Tests: forget the previous capture's page hashes and the registered sections other than the built-ins. */
void xbox_StateReset(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_STATE_H */
