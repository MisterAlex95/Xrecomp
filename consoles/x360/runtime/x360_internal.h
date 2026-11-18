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
#define X360_MAX_HANDLES 4096
#define X360_MAX_WAIT 64
#define X360_MAX_TIMERS 64
#define X360_TLS_SLOTS 64
#define X360_THREAD_EXIT 0xBCBCBCBCu /* a thread's first LR: returning there ends the thread */
#define X360_SLICE 4096              /* blocks per time slice */

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

typedef struct x360_thread {
    xe_cpu cpu;
    int state;
    int suspend;
    uint32_t id, handle;
    uint32_t kthread, pcr, tls_static, tls_slots;
    uint32_t stack_base, stack_limit;
    int wait_kind, wait_all, wait_count;
    uint32_t wait_obj[X360_MAX_WAIT];
    uint64_t wait_deadline; /* virtual time, 100 ns; 0: none */
    uint32_t exit_code;
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

    x360_thunk *thunks;
    unsigned nthunks;
    uint32_t thunk_lo, thunk_hi;
    uint32_t vars[2][4096];   /* guest address of each variable export, by library and ordinal (0: none) */

    xe_rc_fn *fn_tab;         /* lifted code by (pc - code_lo) / 4 */
    uint32_t code_lo, code_hi;

    x360_thread threads[X360_MAX_THREADS];
    int nthreads, cur;
    uint32_t next_thread_id;
    x360_handle handles[X360_MAX_HANDLES];
    x360_timer timers[X360_MAX_TIMERS];
    uint64_t tls_used;        /* KeTlsAlloc slots */
    uint64_t time_skip;       /* virtual 100 ns skipped while every thread waited */

    int status;
    uint32_t exit_code;
    char error[256];
    uint32_t missed[256];
    unsigned nmissed;
    uint8_t unimpl_seen[2][4096];

    FILE *dbg, *log;
    int verbose;
    x360_stats st;
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
x360_thread *x360_thread_by_kthread(x360_machine *x, uint32_t kthread);
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

/* x360_kernel.c */
void x360_hle_call(x360_machine *x, const x360_thunk *t);
int x360_kernel_init(x360_machine *x);
const char *x360_export_name(int lib, unsigned ordinal);

/* x360_interp.c: run the instruction at the current PC; 0 on success, -1 (x->status set) otherwise */
int x360_interp_one(x360_machine *x);

#endif
