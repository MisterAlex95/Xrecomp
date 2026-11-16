/* psp_internal.h -- the PSP spike's machine state, shared by the core's files (XBA-224). */
#ifndef PSP_INTERNAL_H
#define PSP_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "psp_core.h"
#include "psp_recomp_rt.h"

#define PSP_RAM_BASE 0x08000000u
#define PSP_RAM_SIZE 0x02000000u /* 32 MB (PSP-1000) */
#define PSP_USER_LO 0x08800000u  /* the user partition */
#define PSP_USER_HI 0x0A000000u
#define PSP_VRAM_BASE 0x04000000u
#define PSP_VRAM_SIZE 0x00200000u
#define PSP_SCRATCH_BASE 0x00010000u
#define PSP_SCRATCH_SIZE 0x00004000u
#define PSP_LOAD_BASE 0x08804000u /* where the firmware loads the first user module (psp_tools.prx too) */

/* Kernel stubs the HLE plants in the kernel partition (not user code: never lifted). */
#define PSP_STUB_THREAD_RETURN 0x08000000u /* a thread's ra: syscall PSP_SC_THREAD_RETURN */
#define PSP_SC_THREAD_RETURN 0xFFFFFu

#define PSP_CPU_HZ 222000000u
#define PSP_CYCLES_PER_FRAME (PSP_CPU_HZ / 60u)
#define PSP_CYCLES_PER_US (PSP_CPU_HZ / 1000000u)

#define PSP_MAX_THREADS 32
#define PSP_MAX_OBJECTS 64
#define PSP_TRACE 32

typedef struct psp_import {
    char lib[32];
    uint32_t nid;
    uint32_t stub;
    int handler; /* index in psp_hle.c's table, -1: not implemented */
} psp_import;

enum { TH_FREE = 0, TH_DORMANT, TH_READY, TH_RUNNING, TH_WAITING, TH_DEAD };
enum { WAIT_NONE = 0, WAIT_DELAY, WAIT_VBLANK, WAIT_SEMA, WAIT_SLEEP };

typedef struct psp_thread {
    int state;
    int uid;
    char name[32];
    uint32_t entry, attr, stack, stack_size, gp;
    int init_prio, prio;
    int wait, wait_id, wait_count;
    uint64_t wake_at;
    uint32_t exit_status;
    uint64_t seq; /* FIFO order among equal priorities */
    /* saved context (while not running) */
    uint32_t r[32], pc, next_pc, hi, lo, fcr31;
    float f[32];
} psp_thread;

enum { OBJ_FREE = 0, OBJ_SEMA, OBJ_BLOCK, OBJ_LWMUTEX, OBJ_CALLBACK };

typedef struct psp_object {
    int kind;
    int uid;
    char name[32];
    int32_t count, max;  /* semaphores */
    uint32_t addr, size; /* memory blocks */
} psp_object;

struct psp_machine {
    psp_rc_head h; /* first: lifted code sees the machine through it */
    uint8_t *vram;
    uint8_t scratch[PSP_SCRATCH_SIZE];
    char error[256];
    int verbose;
    bool exited;
    uint64_t frames;
    uint64_t frame_end;
    bool branched; /* the last interpreted instruction was a taken transfer */
    bool switched; /* the HLE switched threads in the last instruction */
    uint32_t ic;   /* MFIC / MTIC: the interrupt enable word (no interrupts in the spike) */

    /* the module */
    char mod_name[32];
    uint32_t mod_lo, mod_hi, code_lo, code_hi, entry, gp;
    uint32_t start_prio, start_stack, start_attr;
    psp_import *imports;
    int nimports;

    /* HLE kernel */
    psp_thread threads[PSP_MAX_THREADS];
    int cur; /* running thread, -1: none (idle) */
    uint64_t seq;
    psp_object objs[PSP_MAX_OBJECTS];
    int next_uid;
    uint32_t heap_lo, heap_hi; /* free user memory between the bump pointers */
    uint32_t fb_addr, fb_width, fb_format;
    uint32_t buttons, ctrl_cycle, ctrl_mode;
    uint32_t unimpl[32]; /* NIDs called without a handler (logged once each) */
    int nunimpl;
    char *out; /* stdout / stderr text */
    size_t out_len, out_cap;

    /* lifted code */
    const psp_recomp_table *rc;
    uint8_t *rc_valid; /* per entry: its bytes match memory */
    uint32_t rc_stale;
    bool rc_after_jump;
    uint32_t *missed;
    size_t nmissed, cap_missed;
    psp_recomp_stats stats;

    uint32_t trace[PSP_TRACE];
    unsigned trace_pos;
};

/* psp_bus.c */
uint8_t *psp_ptr(psp_machine *m, uint32_t addr, uint32_t len); /* host pointer to len bytes, or NULL */
int psp_read(psp_machine *m, uint32_t addr, int width, uint32_t *v);
int psp_write(psp_machine *m, uint32_t addr, int width, uint32_t v);
uint32_t psp_r32(psp_machine *m, uint32_t addr); /* 0 when unmapped */
void psp_w32(psp_machine *m, uint32_t addr, uint32_t v);

/* psp_cpu.c */
void psp_cpu_step(psp_machine *m);
void psp_fail(psp_machine *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* psp_load.c */
int psp_load_module(psp_machine *m, const uint8_t *data, size_t size, char *why, size_t why_len);

/* psp_hle.c */
void psp_hle_init(psp_machine *m);
int psp_hle_start_module(psp_machine *m);
void psp_hle_syscall(psp_machine *m, uint32_t code);
uint64_t psp_hle_next_wake(psp_machine *m);
void psp_hle_timers(psp_machine *m);
void psp_hle_vblank(psp_machine *m);
bool psp_hle_schedule(psp_machine *m);
void psp_hle_preempt(psp_machine *m);
int psp_hle_handler_of(const char *lib, uint32_t nid);
const char *psp_hle_name(int handler);

/* psp_recomp.c */
bool psp_recomp_dispatch(psp_machine *m);

#endif
