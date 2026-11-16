/* ps2_internal.h -- the PS2 spike's machine state, shared by the core's files (XBA-226). */
#ifndef PS2_INTERNAL_H
#define PS2_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ps2_core.h"
#include "ps2_recomp_rt.h"

#define PS2_RAM_SIZE 0x02000000u /* 32 MB */
#define PS2_SCRATCH_BASE 0x70000000u
#define PS2_SCRATCH_SIZE 0x00004000u
#define PS2_IO_BASE 0x10000000u /* EE registers: timers, IPU, GIF, VIF, DMAC, INTC, SIF, SIO ... */
#define PS2_IO_SIZE 0x00010000u
#define PS2_GS_BASE 0x12000000u /* GS privileged registers */
#define PS2_GS_SIZE 0x00002000u
#define PS2_SIO_TXFIFO 0x1000F180u
#define PS2_KERNEL_TOP 0x00080000u /* below: the kernel's memory (nothing of ours lives there but the stubs) */

#define PS2_TRACE 32
#define PS2_MAX_SEMAS 64
#define PS2_MAX_THREADS 32
#define PS2_MAX_SYSCALL 256
#define PS2_SYSCALL_DEPTH 4

/* Kernel stubs the HLE plants in kernel memory (never lifted: outside the executable's code). */
#define PS2_STUB_THREAD_EXIT 0x00001000u /* a thread's ra: li v1, ExitThread ; syscall */
#define PS2_STUB_SYSRET 0x00001010u      /* a SetSyscall handler's ra: li v1, PS2_SC_SYSRET ; syscall */
#define PS2_SC_SYSRET 0x7FFF

typedef struct ps2_sema {
    bool used;
    int32_t count, max, init;
    uint32_t attr, option;
} ps2_sema;

enum { TH_FREE = 0, TH_DORMANT, TH_READY, TH_RUNNING, TH_WAITING };
enum { WAIT_NONE = 0, WAIT_SLEEP, WAIT_SEMA };

typedef struct ps2_thread {
    int state;
    uint32_t func, stack, stack_size, gp, attr, option;
    int init_prio, prio;
    int wait, wait_id, wakeups;
    bool suspended;
    uint64_t seq; /* FIFO order among equal priorities */
    /* saved context (while not running) */
    uint64_t r[32], rh[32], hi, lo, hi1, lo1;
    uint32_t sa, f[32], acc, fcr31, pc, next_pc;
} ps2_thread;

typedef struct ps2_sysframe { /* a SetSyscall handler running as guest code */
    uint32_t pc, next_pc;
    uint64_t ra;
} ps2_sysframe;

struct ps2_machine {
    ps2_rc_head h; /* first: lifted code sees the machine through it */
    uint8_t scratch[PS2_SCRATCH_SIZE];
    uint8_t io[PS2_IO_SIZE]; /* latched: reads give what was written (but the registers ps2_bus.c models) */
    uint8_t gs[PS2_GS_SIZE];
    char error[256];
    int verbose;
    bool exited;
    int exit_code;
    bool branched; /* the last interpreted instruction was a taken transfer */
    uint32_t cop0[32];

    /* the executable */
    uint32_t entry, code_lo, code_hi, image_hi;

    /* HLE kernel */
    uint32_t heap_end, stack_lo, stack_hi;
    ps2_sema semas[PS2_MAX_SEMAS];
    ps2_thread threads[PS2_MAX_THREADS]; /* index = thread id; 1 is the main thread, 0 unused */
    int cur;                             /* running thread */
    uint64_t seq;
    bool switched; /* the HLE switched threads in the last syscall */
    uint32_t syscall_override[PS2_MAX_SYSCALL]; /* SetSyscall: guest handler addresses */
    ps2_sysframe sysframes[PS2_SYSCALL_DEPTH];
    int nsysframes;
    uint32_t osd_config;
    int32_t unimpl[32]; /* syscalls made without a handler (logged once each) */
    int nunimpl;
    char *out; /* SIO text */
    size_t out_len, out_cap;

    /* lifted code */
    const ps2_recomp_table *rc;
    uint8_t *rc_valid; /* per entry: its bytes match memory */
    uint32_t rc_stale;
    bool rc_after_jump;
    uint32_t *missed;
    size_t nmissed, cap_missed;
    ps2_recomp_stats stats;

    uint32_t trace[PS2_TRACE];
    unsigned trace_pos;
};

/* ps2_bus.c */
uint8_t *ps2_ptr(ps2_machine *m, uint32_t addr, uint32_t len); /* host pointer to len bytes of RAM / scratchpad */
int ps2_read(ps2_machine *m, uint32_t addr, int width, void *out);
int ps2_write(ps2_machine *m, uint32_t addr, int width, const void *in);
uint32_t ps2_r32(ps2_machine *m, uint32_t addr); /* 0 when unmapped */
void ps2_w32(ps2_machine *m, uint32_t addr, uint32_t v);

/* ps2_cpu.c */
void ps2_cpu_step(ps2_machine *m);
void ps2_fail(ps2_machine *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ps2_load.c */
int ps2_load_elf(ps2_machine *m, const uint8_t *data, size_t size, char *why, size_t why_len);

/* ps2_hle.c */
void ps2_hle_init(ps2_machine *m);
void ps2_hle_syscall(ps2_machine *m, int32_t code);

/* ps2_recomp.c */
bool ps2_recomp_dispatch(ps2_machine *m);

#endif
