/*
 * xbox_state.h - Xbox save states: capture the guest state, the state format (XBA-245) and the same-run
 * restore (XBA-249)
 *
 * Design: docs/technical/xbox-save-states.md. With every guest thread frozen at a kernel boundary
 * (kernel_park.h), a capture writes one blob, "XBXS", of tagged sections:
 *
 *   header   "XBXS", format version, header size, section count, the identity (title ID, SHA-1 of the
 *            XBE's headers, build ID of the binary, toolkit version, RAM / mapped / contiguous sizes,
 *            sizeof(RecompCpuRegs), the mod set), when it was taken, a hash of the header
 *   sections each: u32 tag, u32 version, u64 length, u64 hash of the payload, then the payload
 *
 * All integers little-endian (every host we build for is). A state only loads into the binary that
 * saved it (the build ID; call-site addresses and lifted frames are part of a thread's state), so a
 * section's payload may use this build's layouts (a raw RecompCpuRegs); its version changes when its own
 * format does.
 *
 * Built-in sections (this file's ticket):
 *   CPU   the run it was taken in (XBA-249), then per guest thread its park record (kernel_park.h): the
 *         call it is in (ordinal, guest return address, esp at the call) or the host wait inside lifted code
 *         (site kind and guest VA, XBA-251), whether it returned and its status, the registers
 *         (RecompCpuRegs), the TIB, the KTHREAD, the guest stack bounds and the guest thread id read from the
 *         KTHREAD, and its guest call chain (XBA-247: every frame's return VA, esp and ebp from the frame
 *         shadow stack, and the chain's signature)
 *   MEM   the memory manager's host-side state (xbox_MemStateSave: heap, contiguous window, lazy
 *         reservations, guest_vmem)
 *   RAM   guest memory, range by range (xbox_StateGuestRanges): a bitmap of the pages stored (not all
 *         zero), then those pages. No compressor in the tree (the console runtimes skip zero pages the
 *         same way, n64_state.c); rewind codes XOR deltas on top (recomp_rewind.h)
 *
 * Also built in since XBA-249: KOBJ (kernel_kobj_state.h, saved for this run: XBOX_KOBJ_IN_SESSION), NV2A,
 * APU, USB (xbox_dev_state.h; the capture and the load hold the device threads around every section,
 * xbox_DevStateFreeze / Thaw). Other components add their own sections: a section is a tag, a version, a
 * save callback (runs during the capture, every guest thread frozen), a check callback (validates a payload
 * without restoring anything) and a load callback (puts a checked payload back). xbox_StateRegister, once,
 * before the first capture (an init function of the component).
 *
 * Reading a state checks, in order: the magic and format version, the header hash, the identity
 * against the running title (another title or XBE: EGAME; another build, toolkit, memory size or mod set:
 * EVERSION), then every section: its bounds and hash (ECORRUPT), that it is registered here with the
 * same version (EVERSION), its check callback; and that every registered section is present.
 *
 * Loading (same run, XBA-249: xbox_StateLoad). Every guest thread is frozen, then each must be held at the end
 * of a kernel call (a wait in progress is given until the timeout to end). The state is accepted only when it
 * was taken in this run and its threads are the threads running now, each parked at the same call as in the
 * state (same ordinal, guest return address and esp at the call) through the same call chain (XBA-247: the
 * chain signature of its frame shadow stack, every lifted frame's return address and esp, neither chain
 * unrestorable) with the same ebp in every frame: the host stack below it is then the one the state's thread
 * had (chain_matches in xbox_state.c). A thread at another chain re-enters the state's instead, on its own host
 * thread, through the resume entries (XBA-294: kernel_park.h xbox_ParkReenterThread); when it cannot (the same
 * guest thread and routine, a chain the resume entries can re-enter), ECHAIN with both reasons, and nothing
 * changes. Then, with the
 * devices held: MEM, RAM, KOBJ, NV2A, APU, USB and any other section in registration order, and each thread's
 * registers (the ones it sees when its call returns, or the call made again when the state has the thread
 * inside it). A section that fails part way puts back a capture taken
 * just before (never a partial load).
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

/* CPU v2 (XBA-251): the park site kind (XboxParkRecord.site) in each thread's head. v3 (XBA-249): the run id
 * and the esp at the call. v4 (XBA-247): the registers are a RecompCpuRegs (r_frames at the end) and each
 * thread's call chain follows them. Older versions are refused. */
#define XBOX_STATE_CPU_VERSION 5   /* v5 (XBA-250): the verdict for another run after the run id */
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
    XBOX_STATE_ECHAIN = -8,     /* RECOMP_STATE_EREFUSED: the game is not at a point this state can be loaded at
                                 * (a thread parked elsewhere, created or ended since, another run) */
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
    /* Puts a payload xbox_StateCheck accepted back (XBA-249), every guest thread frozen and the device
     * threads held: XBOX_STATE_OK, or an error (the load puts back the state it replaced; a reason through
     * xbox_StateSectionReason). NULL: the section has nothing to restore. */
    int (*load)(const uint8_t *p, size_t n, uint32_t version, void *user);
} XboxStateSection;

/* From a save or load callback: why it failed, for the capture's or the load's reason. */
void xbox_StateSectionReason(const char *fmt, ...);

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
    uint32_t cpu_bytes;      /* sizeof(RecompCpuRegs) */
    uint32_t mod_set;        /* the enabled mods (XBA-286): RECOMP_MOD_SET's first 8 hex digits; 0 no mod */
} XboxStateIdentity;

/* The running title's identity. The build ID is the binary's (ELF NT_GNU_BUILD_ID of the module this code
 * is linked into, the executable or the app's libmain.so; Mach-O LC_UUID), unless set. */
void xbox_StateIdentityGet(XboxStateIdentity *out);
/* Overrides the build ID (n bytes, at most 32; n 0: back to the binary's). Hosts without one, tests. */
void xbox_StateSetBuildId(const uint8_t *id, size_t n);
/* The mod set of a hex mod-set hash (tools/mods/store.py state_mod_set): its first 8 hex digits, big-endian, 1 when
 * they are all 0; 0 for NULL, "" or no hex digits (no mod). xbox_StateIdentityGet reads RECOMP_MOD_SET, which
 * Prepare forces from the enabled mods (docs/technical/mods.md). */
uint32_t xbox_StateModSet(const char *hex);

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

/* A parked thread's guest call chain (XBA-247), from its frame shadow stack (kernel_park.h
 * xbox_ParkFrames) and the call-site table: one frame per lifted call it is inside, from its routine's
 * first call (frame 0) to the call it is parked in (the kernel call's own frame, the last one). */
typedef struct XboxStateFrame {
    uint32_t ret_va;         /* the guest return address of the call (the word at esp) */
    uint32_t esp;            /* guest esp at the callee's entry */
    uint32_t ebp;            /* the caller's ebp at the call (the frame below's when the caller has none) */
    uint32_t func;           /* the lifted function the call site is in (call-site table; 0: unknown) */
    uint32_t flags;          /* its XBOX_CALL_SITE_* */
} XboxStateFrame;

enum {
    XBOX_CHAIN_EMPTY = 0x01,         /* no lifted frame: the routine is not lifted code (a host routine) */
    XBOX_CHAIN_OVERFLOW = 0x02,      /* deeper than RECOMP_FRAME_SLOTS frames: the rest was not kept */
    XBOX_CHAIN_NO_TABLE = 0x04,      /* no call-site table registered (generated code older than XBA-247) */
    XBOX_CHAIN_UNKNOWN_SITE = 0x08,  /* a return address that no call site names */
    XBOX_CHAIN_STACK_SWITCH = 0x10,  /* through a function that switches guest stacks (XBA-101) */
    XBOX_CHAIN_MISMATCH = 0x20,      /* the frames disagree with the park record (count, or the top frame is
                                      * not the kernel call's) */
    XBOX_CHAIN_ALL = 0x3F
};
/* What makes a chain unusable for a restore: an in-session load compares signatures only when neither
 * chain has these. */
#define XBOX_CHAIN_UNRESTORABLE (XBOX_CHAIN_OVERFLOW | XBOX_CHAIN_NO_TABLE | XBOX_CHAIN_UNKNOWN_SITE \
                                 | XBOX_CHAIN_STACK_SWITCH | XBOX_CHAIN_MISMATCH)

typedef struct XboxStateChain {
    uint32_t frames;         /* frames in the chain (at most RECOMP_FRAME_SLOTS) */
    uint32_t base;           /* frames below the thread's routine, not part of its chain */
    uint32_t flags;          /* XBOX_CHAIN_* */
    uint64_t signature;      /* xbox_StateChainSignature of the frames */
} XboxStateChain;

/* The signature of a call chain: a 64-bit hash of the frame count and each frame's return VA and esp (not
 * ebp, which is state: a restore writes it back). Two threads parked at the same call sites with the same
 * guest stack depths have the same signature. */
uint64_t xbox_StateChainSignature(const XboxStateFrame *f, uint32_t n);

/* For a section's save callback: thread i's park record and guest-side location. */
typedef struct XboxStateThread {
    int slot, state;
    uint32_t ordinal, ret_va;
    uint32_t call_esp;       /* esp when the call was made (its return address's slot) */
    int returned;
    uint32_t status;
    int dispatch;
    uint32_t tid;            /* host thread id (low 32 bits) */
    uint64_t kernel_calls;
    uint32_t tib, kthread;   /* fs base, the KTHREAD at fs:[0x28] (0 when outside guest memory) */
    uint32_t stack_base, stack_limit;   /* the guest stack: KTHREAD StackBase (+0x1C) / StackLimit (+0x20) */
    uint32_t guest_tid;      /* KTHREAD UniqueThread (+0x12C): the id the title sees */
    RecompCpuRegs cpu;
    uint32_t site;           /* XBOX_PARK_SITE_*: KERNEL (ordinal, ret_va the call's return address) or a
                              * host wait inside lifted code (ordinal 0, ret_va the site's VA), XBA-251 */
    XboxStateChain chain;    /* its call chain (XBA-247); the frames: xbox_StateChain / xbox_StateCpuChain */
} XboxStateThread;
int xbox_StateContextThread(const XboxStateContext *ctx, int i, XboxStateThread *out);

/* A frozen thread's call chain (XBA-247): its frames (at most max into out, which may be NULL) and *info.
 * t is the thread's park record (xbox_StateContextThread, or a thread built from xbox_ParkRecords), which
 * the chain is checked against. Returns the frames in the chain (possibly more than max), or -1 when the
 * thread is gone. Any thread while the guest threads are frozen (xbox_ParkFreeze). */
int xbox_StateChain(const XboxStateThread *t, XboxStateFrame *out, uint32_t max, XboxStateChain *info);

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
/* The run a CPU payload was taken in (xbox_StateRunId), 0 when malformed. */
uint64_t xbox_StateCpuRun(const uint8_t *p, size_t n);
/* The verdict a CPU payload carries for another run (XBA-250): 1 when every thread's chain can be re-entered and
 * the kernel objects made again in a fresh process, 0 with the reason in why, -1 when malformed. */
int xbox_StateCpuAcross(const uint8_t *p, size_t n, char *why, size_t why_n);
/* This run's id: random, fixed for the process. */
uint64_t xbox_StateRunId(void);
/* Thread i's chain frames in a CPU payload (at most max into out): the frame count, or -1. */
int xbox_StateCpuChain(const uint8_t *p, size_t n, int i, XboxStateFrame *out, uint32_t max);
typedef struct XboxStateRamInfo {
    uint32_t ranges, pages, stored;
} XboxStateRamInfo;
int xbox_StateRamInfo(const uint8_t *p, size_t n, XboxStateRamInfo *info);   /* 0 or -1 */
/* The stored copy of guest page `va` (4 KB aligned) in a RAM payload: 1 with *page (NULL: the page was all
 * zero), 0 when no range covers it, -1 when the payload is malformed. */
int xbox_StateRamPage(const uint8_t *p, size_t n, uint32_t va, const uint8_t **page);

/* ---- Loading (same run, XBA-249) ------------------------------------------------------------------------ */

typedef struct XboxStateLoadStats {
    double freeze_ms;        /* until every guest thread was parked */
    double settle_ms;        /* until each was held at the end of its call */
    double backup_ms;        /* the capture taken first, put back if a section fails */
    double load_ms;          /* the sections and the registers */
    int threads;
    int reissued;            /* threads whose call is made again (the state had them inside it) */
    char reason[256];        /* why it was refused or failed, or "" */
    int reentered;           /* threads at another call chain than in the state, re-entering the state's (XBA-294) */
} XboxStateLoadStats;

/* Freezes every guest thread (timeout_ms for the freeze, and again for the waits in progress to end), loads the
 * state (see the top of this file), thaws. Logs one line ("[STATE] load: ..." or "[STATE] load refused: ...").
 * Not from a guest thread. XBOX_STATE_OK, _ECHAIN (with the reason), any code xbox_StateCheck returns,
 * _EFREEZE, _EBUSY, _ENOMEM, _ESECTION. */
int xbox_StateLoad(const uint8_t *blob, size_t n, uint32_t timeout_ms, XboxStateLoadStats *st);
/* The same with the freeze already held by the caller (it stays held: the threads run on at the caller's
 * xbox_ParkThaw). */
int xbox_StateLoadFrozen(const uint8_t *blob, size_t n, uint32_t settle_ms, XboxStateLoadStats *st);
/* Reads a slot file (xbox_StateReadSlot) and loads it. */
int xbox_StateLoadSlot(const char *path, uint32_t timeout_ms, XboxStateLoadStats *st);

/* ---- Another run (XBA-250: resume entries) -------------------------------------------------------------- */

/* 1 when the blob is this title's and build's and its threads can be re-entered in a fresh process (the verdict
 * the capture wrote: every chain through sites with resume entries, kernel objects that can be made again), else
 * 0 with the reason. */
int xbox_StateResumable(const uint8_t *blob, size_t n, char *why, size_t why_n);
/* Loads a state into a process where the game has not started (no guest thread yet; before the title's entry
 * point): one host thread per saved guest thread, with its guest stack, TIB and KTHREAD, then MEM, RAM, KOBJ
 * (across runs: running threads are these), NV2A, APU, USB; then each thread re-enters its call chain through the
 * lifted code's resume entries (xbox_resume.h) and its kernel call or host wait. Logs « [STATE] resume: ... ».
 * XBOX_STATE_OK; _ECHAIN (not resumable, with the reason; or the game already running); any code xbox_StateCheck
 * returns; _ESECTION when a section failed part way (the game cannot go on then). */
int xbox_StateResume(const uint8_t *blob, size_t n, XboxStateLoadStats *st);
int xbox_StateResumeSlot(const char *path, XboxStateLoadStats *st);
/* 1 once a resume let its threads go (this process runs a resumed state). */
int xbox_StateResumed(void);
/* The process main after a resume: waits until the resumed main guest thread ends (0 at once when there is none). */
int xbox_StateResumeWait(void);
/* At start, before the title's entry point (templates/new-game/src/main.c): with RECOMP_STATE_RESUME on and an
 * automatic slot that can be resumed, resumes it (xbox_StateResume) and returns 1: the caller waits
 * (xbox_StateResumeWait) instead of calling the entry point. 0: start the game as usual (the reason logged). */
int xbox_StateResumeAtBoot(void);
/* Save states can survive the run for this title (« Resume where I left off », the automatic slot): the generated
 * code has resume entries (a call-site table) and states are available now; else 0 with the reason. */
int xbox_StateResumeAvailable(char *why, size_t why_n);

/* ---- The runtime service (xbox_state_service.c) --------------------------------------------------------- */

/* Attaches the running title to recomp_states (console "xbox", slots in <save dir>/xbox-<TITLE ID>), so the
 * in-game menu's Save state / Load state, the quick save / load chords and the slot hotkeys reach
 * xbox_StateSaveSlot / xbox_StateLoadSlot; starts the rewind thread (RECOMP_REWIND, recomp_rewind.h).
 * Called by xbox_kernel_bridge_init (after xbox_path_init in a title; a host that sets the paths later calls it
 * again: the attach only). */
void xbox_StateServiceStart(void);
/* 1 when save states can be offered for the running title, else 0 with the reason in why (no guest
 * thread yet, a title that switches stacks, untracked threads, no save folder). */
int  xbox_StateAvailable(char *why, size_t why_n);
/* From the pad's hotkeys (usb_gamepad.c, the controller thread, which a capture holds): the actions pressed
 * (recomp_hotkeys.h bits) run on the service's thread; held: the rewind chord is held now. */
void xbox_StateServiceHotkeys(uint32_t pressed, int rewind_held);
/* Rewind arms on the first pad report (a title playing) or this call (tests): captures start then. */
void xbox_StateRewindArm(void);
typedef struct XboxRewindStats {
    int on;                  /* rewind is on (settings, and not turned off by a failure) */
    uint64_t captures, steps, failed;
    double capture_ms_avg, capture_ms_max, step_ms_avg;
    size_t ring_bytes, ring_count, blob_bytes;
    char off_reason[160];    /* why it turned itself off, or "" */
} XboxRewindStats;
void xbox_StateRewindStats(XboxRewindStats *out);

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
