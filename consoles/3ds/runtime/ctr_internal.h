/* ctr_internal.h -- the 3DS spike's machine (XBA-228): the process, its threads, kernel objects and services. Not an
 * API. */
#ifndef CTR_INTERNAL_H
#define CTR_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctr_core.h"
#include "ctr_recomp_rt.h"

/* ARM11 at 268 MHz (268,111,856 Hz), 59.83 frames per second. */
#define CTR_CPU_HZ 268111856ull
#define CTR_FRAME_CYCLES 4481136ull

/* The process's address space (3dbrew « Memory layout », a libctru application on a current firmware). */
#define CTR_CODE_BASE 0x00100000u
#define CTR_HEAP_BASE 0x08000000u   /* svcControlMemory MEMOP_ALLOC (libctru's heap) */
#define CTR_STACK_TOP 0x10000000u   /* the main thread's stack, below */
#define CTR_STACK_SIZE 0x00100000u
#define CTR_LINEAR_BASE 0x30000000u /* MEMOP_ALLOC with MEMOP_LINEAR_FLAG: FCRAM, physical 0x20000000 + offset */
#define CTR_LINEAR_PHYS 0x20000000u
#define CTR_CONFIG_MEM 0x1FF80000u  /* configuration memory (read-only) */
#define CTR_SHARED_PAGE 0x1FF81000u /* shared page (read-only) */
#define CTR_TLS_BASE 0x1FF82000u    /* thread local storage, 0x200 bytes per thread */
#define CTR_APP_MEMORY 0x04000000u  /* the APPLICATION memory region (64 MB, Old 3DS) */

#define CTR_MAX_THREADS 32
#define CTR_MAX_OBJECTS 512
#define CTR_MAX_HANDLES 512
#define CTR_MAX_WAIT 64
#define CTR_MAX_REGIONS 64

/* Result codes (3dbrew « Error codes »). */
#define CTR_RES_OK 0u
#define CTR_RES_TIMEOUT 0x09401BFEu
#define CTR_RES_INVALID_HANDLE 0xD8E007F7u
#define CTR_RES_NOT_IMPLEMENTED 0xF8C007F4u
#define CTR_RES_INVALID_ADDRESS 0xE0E01BF5u
#define CTR_RES_OUT_OF_MEMORY 0xD86007F3u
#define CTR_RES_NOT_FOUND 0xD88007FAu
#define CTR_RES_FS_NOT_FOUND 0xC8804478u

enum ctr_obj_type {
    CTR_OBJ_FREE = 0,
    CTR_OBJ_EVENT,
    CTR_OBJ_MUTEX,
    CTR_OBJ_SEMAPHORE,
    CTR_OBJ_TIMER,
    CTR_OBJ_THREAD,
    CTR_OBJ_PROCESS,
    CTR_OBJ_SESSION,    /* a client session to an HLE service */
    CTR_OBJ_SHMEM,
    CTR_OBJ_ARBITER,
    CTR_OBJ_RESLIMIT,
};

enum { CTR_RESET_ONESHOT = 0, CTR_RESET_STICKY = 1, CTR_RESET_PULSE = 2 };

typedef struct ctr_obj {
    int type;
    char name[16];
    union {
        struct {
            int reset;
            bool signaled;
        } ev; /* events and timers */
        struct {
            int owner; /* thread index, -1 = free */
            int count;
        } mtx;
        struct {
            int32_t count, max;
        } sem;
        struct {
            int thread;
        } th;
        struct {
            int service; /* enum ctr_service (CTR_SVC_SRV: the srv: port's session) */
        } sess;
        struct {
            uint32_t size;
            uint8_t *buf;  /* backing store while unmapped */
            uint32_t addr; /* guest address once mapped, 0 = not mapped */
        } shm;
    } u;
    uint64_t fire_at, interval; /* timers */
} ctr_obj;

enum ctr_thread_state { CTR_TH_FREE = 0, CTR_TH_READY, CTR_TH_WAIT, CTR_TH_DEAD };

enum ctr_wait_kind { CTR_WAIT_NONE = 0, CTR_WAIT_SYNC, CTR_WAIT_ARBITER, CTR_WAIT_SLEEP };

typedef struct ctr_thread {
    int state;
    int prio;
    uint32_t id;
    ctr_cpu cpu;
    ctr_obj *obj;       /* the thread's kernel object (waitable: signaled once it exits) */
    int wait;           /* enum ctr_wait_kind */
    ctr_obj *waits[CTR_MAX_WAIT];
    int nwaits;
    bool wait_all;
    bool wait_n;        /* svcWaitSynchronizationN: r1 = the index */
    ctr_obj *arbiter;
    uint32_t arb_addr;
    uint64_t wake_at;   /* timeout, UINT64_MAX = none */
} ctr_thread;

typedef struct ctr_region {
    uint32_t base, size;
    uint32_t perm;      /* 1 R, 2 W, 4 X */
    uint32_t state;     /* MemState (3dbrew « Memory Management »): 5 private, 6 shared, 8 aliased... */
} ctr_region;

/* The HLE services (ctr_services.c). */
enum ctr_service {
    CTR_SVC_SRV = 0, CTR_SVC_APT, CTR_SVC_HID, CTR_SVC_GSP, CTR_SVC_FS,
    CTR_SVC_CFG, CTR_SVC_NDM, /* known names with no command implemented yet (answered with success, counted) */
    CTR_SVC_COUNT
};

typedef struct ctr_fb_info {
    uint32_t addr, stride, format;
    bool valid;
} ctr_fb_info;

struct ctr_machine {
    ctr_rc_head h; /* first: lifted code sees it (ctr_recomp_rt.h) */

    /* memory */
    ctr_region regions[CTR_MAX_REGIONS];
    int nregions;
    uint32_t heap_size, linear_size;
    uint32_t code_lo, code_hi, ro_hi, data_hi; /* the 3DSX image: code, rodata, data + bss */
    uint32_t entry;

    /* kernel */
    ctr_obj objs[CTR_MAX_OBJECTS];
    ctr_obj *handles[CTR_MAX_HANDLES];
    ctr_thread threads[CTR_MAX_THREADS];
    int cur;               /* running thread, -1 = none ready */
    bool resched;          /* a thread switch is due at the next instruction boundary */
    uint32_t next_thread_id;
    bool exited;
    ctr_obj *process, *reslimit;

    /* services */
    ctr_obj *gsp_event;    /* GSPGPU_RegisterInterruptRelayQueue's event */
    ctr_obj *gsp_shm;      /* GSP's shared memory: interrupt queue, framebuffer info */
    bool gsp_right;
    ctr_obj *hid_shm, *hid_events[5];
    ctr_obj *apt_lock, *apt_notify, *apt_resume;
    uint32_t apt_param_cmd; /* parameter waiting for APT:ReceiveParameter, 0 = none */
    uint32_t apt_param_sender;
    ctr_obj *srv_notify;
    uint32_t ipc_unknown;  /* commands answered with « not implemented » */

    /* video */
    uint64_t frame_end;
    uint64_t frames;
    ctr_fb_info fb[2];     /* top, bottom: what GSP shows */
    uint8_t top_rgb[CTR_TOP_W * CTR_TOP_H * 3], bottom_rgb[CTR_BOTTOM_W * CTR_BOTTOM_H * 3];
    bool top_valid, bottom_valid;

    /* recompiled code */
    const ctr_recomp_table *rc;
    uint32_t rc_cache_pc[1024];
    int32_t rc_cache_idx[1024];
    uint32_t *missed;
    size_t nmissed, cap_missed;
    ctr_recomp_stats stats;
    bool rc_after_jump;
    bool branched;         /* the instruction being run wrote PC (ctr_cpu.c) */

    int verbose;
    uint32_t trace[64];
    unsigned trace_at;
    char error[200];
};

/* ctr_mem.c */
int ctr_mem_init(ctr_machine *m);
void ctr_mem_free(ctr_machine *m);
int ctr_mem_map(ctr_machine *m, uint32_t base, uint32_t size, uint32_t perm, uint32_t state);
void ctr_mem_unmap(ctr_machine *m, uint32_t base, uint32_t size);
void ctr_mem_protect(ctr_machine *m, uint32_t base, uint32_t size, uint32_t perm);
const ctr_region *ctr_mem_region(const ctr_machine *m, uint32_t addr);
bool ctr_mem_mapped(const ctr_machine *m, uint32_t addr, uint32_t size);

/* ctr_loader.c */
int ctr_load_3dsx(ctr_machine *m, const uint8_t *data, size_t size, char *why, size_t why_n);

/* ctr_cpu.c */
void ctr_cpu_step(ctr_machine *m);
uint32_t ctr_cpu_cpsr(const ctr_cpu *c);
void ctr_cpu_set_cpsr(ctr_cpu *c, uint32_t v);

/* ctr_kernel.c */
void ctr_kernel_init(ctr_machine *m);
int ctr_kernel_start(ctr_machine *m, uint32_t entry);
void ctr_svc(ctr_machine *m, uint32_t num);
ctr_obj *ctr_obj_new(ctr_machine *m, int type, const char *name);
uint32_t ctr_handle_new(ctr_machine *m, ctr_obj *o);
ctr_obj *ctr_handle_get(ctr_machine *m, uint32_t handle);
void ctr_event_signal(ctr_machine *m, ctr_obj *ev);
void ctr_schedule(ctr_machine *m);
uint64_t ctr_next_timeout(const ctr_machine *m);
void ctr_timeouts(ctr_machine *m);
uint8_t *ctr_shm_ptr(ctr_machine *m, ctr_obj *shm);
void ctr_stop(ctr_machine *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ctr_services.c */
int ctr_service_lookup(const char *name);
void ctr_service_call(ctr_machine *m, ctr_obj *session, uint32_t cmdbuf);
void ctr_services_vblank(ctr_machine *m);

/* ctr_recomp.c */
bool ctr_recomp_dispatch(ctr_machine *m);

static inline uint32_t ctr_r32(const ctr_machine *m, uint32_t a)
{
    return ctr_rd32(m->h.mem, a);
}
static inline void ctr_w32(ctr_machine *m, uint32_t a, uint32_t v)
{
    ctr_wr32(m->h.mem, a, v);
}

#endif
