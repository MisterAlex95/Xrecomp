/* x360_internal.h -- the runtime's private state (XBA-270). Everything a title can observe lives in guest memory or
 * in these plain structs (no host pointers but the reservation's base), so a save state can be added the way the
 * other consoles do it: serialise x360_machine minus `base` / `fn_tab`, plus the guest pages in use. */
#ifndef X360_INTERNAL_H
#define X360_INTERNAL_H

#ifndef XE_RUNTIME
#define XE_RUNTIME 1
#endif
#include <stdarg.h>
#include <stdio.h>

#include "x360_core.h"
#include "xenon_rt.h"

enum {
#define X360_EXPORT(lib, ord, name, kind) X360_##lib##_##name = ord,
#include "x360_exports.inc"
#undef X360_EXPORT
};

enum { X360_LIB_XBOXKRNL = 0, X360_LIB_XAM = 1, X360_LIB_OTHER = 2 };

#define X360_MAX_THREADS 64
#define X360_MAX_MISSED 4096   /* addresses one run records (x360_missed) */
#define X360_INTERP_RET_BITS 12 /* x360_interp.c: the return-address set holds up to 2048 */
#define X360_MAX_HANDLES 4096
#define X360_MAX_WAIT 64
#define X360_MAX_TIMERS 64
#define X360_TLS_SLOTS 64
#define X360_THREAD_EXIT 0xBCBCBCBCu /* a thread's first LR: returning there ends the thread */
#define X360_SEH_RETURN 0xBCBCBCB8u  /* an exception handler's LR: returning there goes back to x360_seh.c */
#define X360_SEH_DEPTH 3             /* exceptions raised inside handlers, nested */
#define X360_SLICE 4096              /* blocks per time slice */
#define X360_EPOCH 0x01D7000000000000ull /* system time at boot (100 ns since 1601): fixed, runs are reproducible */

/* NTSTATUS */
#define X_STATUS_SUCCESS 0x00000000u
#define X_STATUS_TIMEOUT 0x00000102u
#define X_STATUS_ABANDONED_WAIT_0 0x00000080u
#define X_STATUS_UNSUCCESSFUL 0xC0000001u
#define X_STATUS_NOT_IMPLEMENTED 0xC0000002u
#define X_STATUS_INVALID_HANDLE 0xC0000008u
#define X_STATUS_INVALID_PARAMETER 0xC000000Du
#define X_STATUS_NO_MEMORY 0xC0000017u
#define X_STATUS_CONFLICTING_ADDRESSES 0xC0000018u
#define X_STATUS_MUTANT_NOT_OWNED 0xC0000046u
#define X_STATUS_SEMAPHORE_LIMIT_EXCEEDED 0xC0000047u
#define X_STATUS_NOT_FOUND 0xC0000225u

/* dispatcher object types (X_DISPATCH_HEADER.type) */
enum { X_OBJ_EVENT_NOTIFICATION = 0, X_OBJ_EVENT_SYNC = 1, X_OBJ_MUTANT = 2, X_OBJ_SEMAPHORE = 5, X_OBJ_THREAD = 6,
       X_OBJ_TIMER_NOTIFICATION = 8, X_OBJ_TIMER_SYNC = 9 };

typedef struct x360_heap {
    const char *name;
    uint32_t base, size, page; /* guest range and page size */
    uint32_t pages;
    uint32_t *len;             /* per page: allocation length in pages at its first page, 0 elsewhere / free */
    uint8_t *used;
    int physical;              /* backed by the physical memfd (always RW), not by mprotect */
} x360_heap;

enum { TH_FREE = 0, TH_READY, TH_WAIT, TH_SUSPENDED, TH_DONE };
enum { WAIT_OBJECTS = 0, WAIT_CRITSEC, WAIT_DELAY };

/* x360_seh.c (XBA-282): one exception being dispatched on a thread; plain data (save states carry it) */
typedef struct x360_seh_ctx {
    uint64_t gpr[32];
    double fpr[32];
    uint64_t lr, ctr;
    uint32_t pc, cr, xer;
} x360_seh_ctx;

typedef struct x360_seh_level {
    int phase, wait;
    uint32_t scope;                 /* __C_specific_handler: the scope record being looked at */
    x360_seh_ctx exc, cur, next;    /* at the raise; the frame looked at; its caller */
    uint32_t rec, area, establisher;
    uint32_t fstart, flen, fprolog, handler, hdata;
    uint32_t target_frame, target_ip, retval;
} x360_seh_level;

typedef struct x360_seh {
    int depth;
    x360_seh_level lv[X360_SEH_DEPTH];
} x360_seh;

typedef struct x360_thread {
    xe_cpu cpu;
    int state;
    int suspend;
    uint32_t id, handle;
    uint32_t kthread, pcr, tls_static, tls_slots;
    uint32_t stack_base, stack_limit;
    int wait_kind, wait_all, wait_count;
    uint32_t wait_obj[X360_MAX_WAIT];
    uint64_t wait_deadline; /* virtual time, 100 ns, when wait_timed */
    int wait_timed;
    uint32_t exit_code;
    int interrupt; /* runs a graphics interrupt callback (x360_gpu.c) */
    int interp;    /* XBA-283: its time slice ran out inside interpreted code (not state: re-entry is not a miss) */
    x360_seh seh;  /* XBA-282: the exception being dispatched, if any */
} x360_thread;

typedef struct x360_handle {
    uint32_t obj;  /* guest dispatcher object */
    int used;
} x360_handle;

typedef struct x360_timer {
    uint32_t obj;
    uint64_t due;     /* 0: not armed */
    uint32_t period;  /* ms, 0: one shot */
} x360_timer;

typedef struct x360_audio_client {
    uint32_t callback, arg;  /* XAudioRegisterRenderDriverClient's routine and argument */
    uint64_t next;           /* virtual time of the next frame request */
    int thread;              /* its guest context (x360_callback_run), -1 before the first */
    uint32_t frames_requested, frames_submitted;
} x360_audio_client;

typedef struct x360_pad_slot {
    int connected;
    uint32_t packet; /* X_INPUT_STATE's packet number: changes with the state */
    x360_pad state;
} x360_pad_slot;

/* one export's HLE: the dispatch tables of x360_kernel.c, x360_more.c, x360_xma.c and the files of X360_HLE_TABLES */
typedef void (*x360_hle_fn)(x360_machine *x, struct x360_thread *t, xe_cpu *c);
typedef struct x360_hle_row {
    uint8_t lib;
    uint16_t ordinal;
    x360_hle_fn fn;
} x360_hle_row;
#define X360_HK(name, fn) {X360_LIB_XBOXKRNL, X360_xboxkrnl_##name, fn}
#define X360_HX(name, fn) {X360_LIB_XAM, X360_xam_##name, fn}
/* the runtime files with a table of their own: const x360_hle_row x360_tab_<group>[], unsigned x360_tab_<group>_n */
#define X360_HLE_TABLES(T) T(kdpc) T(rtl) T(crypt2) T(vd) T(xam2)
#define X360_DECLARE_TABLE(group) extern const x360_hle_row x360_tab_##group[]; extern const unsigned x360_tab_##group##_n;
X360_HLE_TABLES(X360_DECLARE_TABLE)

typedef struct x360_thunk {
    uint32_t addr;
    uint16_t ordinal;
    uint8_t lib;
} x360_thunk;

struct x360_machine {
    xe_machine m;       /* m.mem = base; m.cur = the running thread's context */
    uint8_t *base;      /* the 4 GB reservation */
    int memfd;
    x360_heap heap_v4k, heap_v64k, heap_stack, heap_kernel, heap_phys;
    uint32_t kalloc_cur, kalloc_end;

    uint32_t image_base, image_size, entry, stack_size, image_crc;
    uint32_t xex_header;      /* the XEX header copied to guest memory (RtlImageXexHeaderField) */
    uint32_t module;          /* LDR_DATA_TABLE_ENTRY-like block for XexExecutableModuleHandle */
    uint32_t tls_raw, tls_raw_size, tls_data_size;
    uint32_t timestamp_bundle;          /* KeTimeStampBundle's storage (updated as virtual time runs) */
    int strict_imports;                 /* stop at the first import without an HLE */
    uint64_t stop_at;                   /* x360_run's instruction budget (rc_native to stop at) */
    uint64_t time_limit;                /* x360_set_time_limit: virtual 100 ns units, 0 = none */
    uint64_t run_until;                 /* x360_run_until's deadline (virtual 100 ns), 0 = none */
    uint64_t rng;                       /* XeCryptRandom's generator (state: runs repeat, states resume it) */
    uint32_t title_id, execution_info; /* the execution info header (guest copy), its title id */

    x360_thunk *thunks;
    unsigned nthunks;
    uint32_t thunk_lo, thunk_hi;
    uint32_t vars[2][4096];   /* guest address of each variable export, by library and ordinal (0: none) */

    xe_rc_fn *fn_tab;         /* lifted code by (pc - code_lo) / 4 */
    const uint32_t *helpers;  /* the pack's save / restore helper entries (x360_pack), sorted */
    unsigned nhelpers;
    int pack_bound;           /* x360_bind succeeded (an empty pack has no fn_tab) */
    unsigned pack_features;   /* the pack's xe_rc_features (bit 0: atomic reservations) */
    struct x360_host *host;   /* XBA-283: guest threads on host threads (x360_set_host_threads), NULL: cooperative */
    uint64_t host_t0;         /* host mode: the host clock (100 ns) at virtual time 0 */
    uint32_t code_lo, code_hi;
    uint32_t pdata_va, pdata_n; /* x360_seh.c: the image's .pdata (found at the first exception) */
    int pdata_scanned;
    uint32_t cspec_thunk;     /* the title's thunk of xboxkrnl's __C_specific_handler (done by x360_seh.c), or 0 */

    x360_thread threads[X360_MAX_THREADS];
    int nthreads, cur;
    uint32_t next_thread_id;
    x360_handle handles[X360_MAX_HANDLES];
    x360_timer timers[X360_MAX_TIMERS];
    x360_audio_client audio[8];
    x360_pad_slot pads[4];
    FILE *audio_out;      /* --audio: the submitted render driver frames as WAV (not state) */
    uint32_t audio_bytes;
    x360_pad_event *pad_script; /* a host's timed pad states (not state: the host gives it again) */
    unsigned pad_script_n, pad_script_at;
    uint64_t tls_used;        /* KeTlsAlloc slots */
    uint64_t time_skip;       /* virtual 100 ns skipped while every thread waited */

    int status;
    uint32_t exit_code;
    char error[256];
    uint32_t missed[X360_MAX_MISSED];
    unsigned nmissed;
    int stop_at_miss;         /* x360_set_interp(x, 0): stop at the first missed address (before XBA-283) */
    uint32_t interp_ret[1u << X360_INTERP_RET_BITS]; /* x360_interp.c: return addresses of interpreted calls */
    unsigned interp_nret;
    uint8_t unimpl_seen[2][4096];

    struct x360_gpu *gpu;     /* x360_gpu.c: the command processor, interrupts */
    struct x360_xma *xma;     /* x360_xma.c: XMA contexts, the audio mix, the motors */
    struct x360_fs *fs;       /* x360_file.c: the disc, the data directory, open files, symbolic links */
    FILE *dbg, *log;
    int verbose;
    x360_stats st;
    x360_hle_fn hle[2][4096]; /* every export with an HLE, by library and ordinal (x360_kernel_init; not state) */
    int hle_conflict;         /* two tables gave one export different functions: the machine is not created */
};

/* guest memory (big-endian) */
static inline uint8_t *x360_p(x360_machine *x, uint32_t a) { return x->base + a; }
static inline uint32_t x360_r32(x360_machine *x, uint32_t a) { return xe_lwz(&x->m, a); }
static inline uint16_t x360_r16(x360_machine *x, uint32_t a) { return (uint16_t)xe_lhz(&x->m, a); }
static inline uint8_t x360_r8(x360_machine *x, uint32_t a) { return (uint8_t)xe_lbz(&x->m, a); }
static inline uint64_t x360_r64(x360_machine *x, uint32_t a) { return xe_ld(&x->m, a); }
static inline void x360_w32(x360_machine *x, uint32_t a, uint32_t v) { xe_stw(&x->m, a, v); }
static inline void x360_w16(x360_machine *x, uint32_t a, uint32_t v) { xe_sth(&x->m, a, v); }
static inline void x360_w8(x360_machine *x, uint32_t a, uint32_t v) { xe_stb(&x->m, a, v); }
static inline void x360_w64(x360_machine *x, uint32_t a, uint64_t v) { xe_std(&x->m, a, v); }

/* x360_mem.c */
int x360_mem_init(x360_machine *x, char *why, size_t n);
void x360_mem_free(x360_machine *x);
uint32_t x360_heap_alloc(x360_machine *x, x360_heap *h, uint32_t size, uint32_t align);
int x360_heap_alloc_at(x360_machine *x, x360_heap *h, uint32_t addr, uint32_t size);
uint32_t x360_heap_free(x360_machine *x, x360_heap *h, uint32_t addr);
uint32_t x360_heap_size_of(x360_heap *h, uint32_t addr);
int x360_heap_all_used(x360_heap *h, uint32_t addr, uint32_t size);
x360_heap *x360_heap_of(x360_machine *x, uint32_t addr);
int x360_commit(x360_machine *x, uint32_t addr, uint32_t size);
uint32_t x360_kalloc(x360_machine *x, uint32_t size); /* zeroed kernel memory, 16-byte aligned */

/* x360_load.c */
int x360_load(x360_machine *x, const uint8_t *xex, size_t size, char *why, size_t n);
uint32_t x360_crc32(const uint8_t *p, size_t n);

/* x360_thread.c */
x360_thread *x360_thread_create(x360_machine *x, uint32_t stack_size, uint32_t entry, uint32_t r3, uint32_t r4,
                                int suspended);
x360_thread *x360_cur(x360_machine *x);
int x360_thread_restart(x360_machine *x, x360_thread *t, uint32_t entry, uint32_t r3, uint32_t r4);
/* run entry(r3, r4) on a callback context kept in *slot (made once, reused): 0, or -1 while it still runs */
int x360_callback_run(x360_machine *x, int *slot, uint32_t entry, uint32_t r3, uint32_t r4);
x360_thread *x360_thread_by_kthread(x360_machine *x, uint32_t kthread);
const x360_thunk *x360_thunk_at(x360_machine *x, uint32_t pc);

/* x360_seh.c (XBA-282) */
void x360_seh_raise(x360_machine *x, x360_thread *t, uint32_t rec);
void x360_seh_unwind(x360_machine *x, x360_thread *t, uint32_t frame, uint32_t ip, uint32_t rec, uint32_t retval);
void x360_seh_return(x360_machine *x, x360_thread *t);
void x360_seh_bind(x360_machine *x);
int x360_seh_code_pc(x360_machine *x, uint32_t pc);
void x360_seh_write_context(x360_machine *x, uint32_t a, const x360_seh_ctx *k);
void x360_thread_exit(x360_machine *x, x360_thread *t, uint32_t code);
uint32_t x360_handle_new(x360_machine *x, uint32_t obj);
uint32_t x360_handle_obj(x360_machine *x, uint32_t handle);
int x360_handle_close(x360_machine *x, uint32_t handle);
uint64_t x360_now(x360_machine *x);
/* start a wait for thread t (objects / delay); timeout_ptr: guest pointer to a 64-bit timeout, 0 = infinite */
void x360_wait(x360_machine *x, x360_thread *t, int kind, const uint32_t *objs, int count, int wait_all,
               uint32_t timeout_ptr);
void x360_fail(x360_machine *x, const char *fmt, ...);
void x360_logf(x360_machine *x, const char *fmt, ...);
void x360_timer_set(x360_machine *x, uint32_t obj, uint64_t due, uint32_t period);
void x360_timer_cancel(x360_machine *x, uint32_t obj);
int x360_wait_check(x360_machine *x, x360_thread *t);
void x360_timers_fire(x360_machine *x);
void x360_install_fault_handler(x360_machine *x);
x360_machine *x360_create_bare(char *why, size_t n); /* tests: memory, kernel and a main thread, no XEX */

/* x360_kernel.c */
void x360_hle_call(x360_machine *x, const x360_thunk *t);
int x360_kernel_init(x360_machine *x);
const char *x360_export_name(int lib, unsigned ordinal);
uint64_t x360_stack_arg(x360_machine *x, xe_cpu *c, int n);
/* the dispatch table as the runtime answers imports: the function behind an export, NULL without an HLE */
x360_hle_fn x360_hle_lookup(const x360_machine *x, int lib, unsigned ordinal);
void x360_hle_add(x360_machine *x, const x360_hle_row *rows, size_t n);
/* x360_more.c: the rest of the surface a shipped title imports */
void x360_more_register(x360_machine *x);
void x360_audio_poll(x360_machine *x);
void x360_audio_close(x360_machine *x);
uint64_t x360_audio_next_event(x360_machine *x);
int x360_set_content_root(x360_machine *x, const char *root, const char *name);

/* x360_file.c: the file calls (x360_kernel.c's table points at them) */
void x360_file_close(x360_machine *x, uint32_t handle);
void x360_fs_free(x360_machine *x);
#define X360_FILE_CALLS(X)                                                                                             \
    X(NtCreateFile) X(NtOpenFile) X(NtReadFile) X(NtWriteFile) X(NtQueryInformationFile) X(NtSetInformationFile)      \
    X(NtQueryFullAttributesFile) X(NtQueryDirectoryFile) X(NtQueryVolumeInformationFile) X(ObCreateSymbolicLink)      \
    X(ObDeleteSymbolicLink)
#define X360_DECLARE_FILE_CALL(name) void x360_k_##name(x360_machine *x, x360_thread *t, xe_cpu *c);
X360_FILE_CALLS(X360_DECLARE_FILE_CALL)

/* x360_xam.c: xam.xex, its own names; X360_XAM_ALIASES maps more exports onto them */
#define X360_XAM_CALLS(X)                                                                                              \
    X(XamGetSystemVersion) X(XGetAVPack) X(XGetGameRegion) X(XGetLanguage) X(XamGetCurrentTitleId)                   \
    X(XamGetExecutionId) X(XamLoaderGetLaunchDataSize) X(XamLoaderGetLaunchData) X(XamLoaderSetLaunchData)           \
    X(XamLoaderLaunchTitle) X(XamAlloc) X(XamFree) X(XamUserGetSigninState) X(XamUserGetXUID) X(XamUserGetName)       \
    X(XamUserGetSigninInfo) X(XamUserCheckPrivilege) X(XamUserIsOnlineEnabled) X(XamInputGetCapabilities)            \
    X(XamEnableInactivityProcessing) X(XamNotifyCreateListener) X(XNotifyGetNext) X(XamShowSigninUI)                 \
    X(XamFeatureEnabled) X(XamResetInactivity)
#define X360_XAM_ALIASES(Y) Y(XamUserGetGamerTag, XamUserGetName)
/* x360_input.c: the controllers */
#define X360_INPUT_CALLS(X)                                                                                            \
    X(XamInputGetState) X(XamInputSetState) X(XamInputGetCapabilitiesEx) X(XamInputGetKeystroke)                     \
    X(XamInputGetKeystrokeEx)
/* x360_gpu.c: the Vd* calls */
#define X360_GPU_CALLS(X)                                                                                              \
    X(VdInitializeRingBuffer) X(VdEnableRingBufferRPtrWriteBack) X(VdSetGraphicsInterruptCallback) X(VdSwap)         \
    X(VdGetSystemCommandBuffer) X(VdInitializeEngines) X(VdIsHSIOTrainingSucceeded) X(VdGetCurrentDisplayGamma)       \
    X(VdPersistDisplay)
#define X360_GPU_ALIASES(Y)                                                                                            \
    Y(VdShutdownEngines, VdNop0) Y(VdRetrainEDRAM, VdNop0) Y(VdRetrainEDRAMWorker, VdNop0)                           \
    Y(VdEnableDisableClockGating, VdNop0) Y(VdSetDisplayMode, VdNop0) Y(VdCallGraphicsNotificationRoutines, VdNop0)
X360_GPU_CALLS(X360_DECLARE_FILE_CALL)
/* x360_crypt.c: the general-purpose crypto exports */
#define X360_CRYPT_CALLS(X)                                                                                            \
    X(XeCryptShaInit) X(XeCryptShaUpdate) X(XeCryptShaFinal) X(XeCryptSha) X(XeCryptSha256Init)                       \
    X(XeCryptSha256Update) X(XeCryptSha256Final) X(XeCryptHmacSha) X(XeCryptRc4Key) X(XeCryptRc4Ecb) X(XeCryptRc4)    \
    X(XeCryptRandom)
X360_CRYPT_CALLS(X360_DECLARE_FILE_CALL)
void x360_k_VdNop0(x360_machine *x, x360_thread *t, xe_cpu *c);
int x360_gpu_init(x360_machine *x);
void x360_gpu_free(x360_machine *x);
void x360_gpu_poll(x360_machine *x);
uint64_t x360_gpu_next_event(x360_machine *x);
void x360_gpu_stats(x360_machine *x, uint64_t *packets, uint64_t *draws, uint64_t *swaps, uint32_t *vblanks);
#define X360_DECLARE_XAM_CALL(name) void x360_x_##name(x360_machine *x, x360_thread *t, xe_cpu *c);
X360_XAM_CALLS(X360_DECLARE_XAM_CALL)
X360_INPUT_CALLS(X360_DECLARE_XAM_CALL)
void x360_input_poll(x360_machine *x);
uint64_t x360_input_next_event(x360_machine *x);

/* x360_xma.c: XMA contexts and the APU registers, the render driver's mix, the motors */
int x360_xma_init(x360_machine *x);
void x360_xma_free(x360_machine *x);
void x360_xma_poll(x360_machine *x);
void x360_xma_register(x360_machine *x);
void x360_xma_state(x360_machine *x, uint32_t *array, uint8_t *used, uint8_t *enabled, int load);
void x360_audio_mix_frame(x360_machine *x, int client, const float *planar6);
void x360_audio_tick(x360_machine *x);
void x360_pad_set_motors(x360_machine *x, int user, unsigned left, unsigned right);

/* x360_interp.c: run the instruction at the current PC; 0 on success, -1 (x->status set) otherwise */
int x360_interp_one(x360_machine *x);
/* run missed code from t's PC until lifted code (0), the end of the slice or the budget (1) or an error (-1); fresh:
 * the dispatcher came from lifted code (the PC is a new miss unless an interpreted call returns there) */
int x360_interp_run(x360_machine *x, x360_thread *t, int fresh);
int x360_interp_ret(const x360_machine *x, uint32_t a);
/* x360_thread.c: record an address the pack does not hold (once) */
void x360_note_missed(x360_machine *x, uint32_t pc);

#endif
