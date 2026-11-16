/* gc_internal.h -- the GameCube spike's machine state, shared by the core's files (XBA-225). */
#ifndef GC_INTERNAL_H
#define GC_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "gc_core.h"
#include "gc_recomp_rt.h"

/* Clocks: the Gekko at 486 MHz, the time base and decrementer at a twelfth of it (bus clock / 4). The core counts
 * one cycle per instruction. */
#define GC_CPU_HZ 486000000u
#define GC_TB_DIV 12u
/* NTSC: 525 lines per frame, two fields per frame at 59.94 fields per second. */
#define GC_LINES 525u
#define GC_CYCLES_PER_LINE (GC_CPU_HZ / 30u / GC_LINES)

#define GC_ARAM_SIZE 0x01000000u
#define GC_LC_SIZE 0x4000u  /* the locked L1 cache (dcbz_l), at 0xE0000000 */

/* PI interrupt causes (INTSR / INTMR bits). */
enum {
    GC_INT_ERR = 1u << 0, GC_INT_RSW = 1u << 1, GC_INT_DI = 1u << 2, GC_INT_SI = 1u << 3, GC_INT_EXI = 1u << 4,
    GC_INT_AI = 1u << 5, GC_INT_DSP = 1u << 6, GC_INT_MEM = 1u << 7, GC_INT_VI = 1u << 8, GC_INT_PE_TOKEN = 1u << 9,
    GC_INT_PE_FINISH = 1u << 10, GC_INT_CP = 1u << 11,
};

struct gc_machine {
    gc_rc_head h; /* first: lifted code sees the machine as a gc_rc_head */
    uint8_t *aram;
    uint8_t lc[GC_LC_SIZE];

    /* time */
    uint64_t frames;
    uint64_t field_end;   /* cycle where the current field ends */
    uint64_t next_line;   /* cycle of the next line start */
    uint32_t line;        /* current line, 0..524 */
    uint64_t tb_base;     /* time base = tb_base + now / GC_TB_DIV */
    uint32_t dec;         /* decrementer value written at dec_written */
    uint64_t dec_written;
    uint64_t dec_fire;    /* cycle where it goes negative, UINT64_MAX when it does not count down past 0 again */
    bool dec_pending;

    /* processor interface */
    uint32_t pi_intsr, pi_intmr;
    uint32_t pi_reg[0x40];
    /* video interface: 16-bit registers */
    uint16_t vi[0x80];
    /* DSP interface: 16-bit registers, mailbox, ARAM DMA */
    uint16_t dsp[0x40];
    uint32_t dsp_mail;    /* DSP -> CPU mailbox */
    bool dsp_mail_full;
    bool dsp_booted;
    /* audio interface, serial interface, external interface: 32-bit registers */
    uint32_t ai[8];
    uint32_t si[0x40];
    uint32_t exi[15];
    uint32_t mi_reg[0x40];

    /* interpreter and recompiled code */
    bool branched;        /* the last instruction the interpreter ran was a taken branch */
    const gc_recomp_table *rc;
    uint8_t *span_state;  /* per span: 0 unknown, 1 valid, 2 stale */
    uint32_t *rc_hash_keys;
    int32_t *rc_hash_idx;
    uint32_t rc_hash_mask;
    uint32_t text_lo[8], text_hi[8];
    int ntext;
    uint32_t *missed;
    size_t nmissed, cap_missed;
    gc_recomp_stats stats;

    int verbose;
    char error[200];
    uint32_t trace[64];   /* last PCs the interpreter ran */
    unsigned trace_at;
};

/* gc_cpu.c */
void gc_cpu_step(gc_machine *m);
void gc_exception(gc_machine *m, uint32_t vector, uint32_t srr1_bits);
void gc_check_interrupts(gc_machine *m);
bool gc_irq_pending(gc_machine *m);
void gc_stop(gc_machine *m, const char *fmt, ...);

/* gc_bus.c */
uint32_t gc_hw_read(gc_machine *m, uint32_t pa, unsigned size);
void gc_hw_write(gc_machine *m, uint32_t pa, uint32_t v, unsigned size);
void gc_vi_line(gc_machine *m);

/* gc_core.c */
void gc_schedule(gc_machine *m);
uint64_t gc_tb(gc_machine *m);

/* gc_recomp.c */
bool gc_recomp_dispatch(gc_machine *m);
void gc_recomp_icbi(gc_machine *m, uint32_t ea);

#endif
