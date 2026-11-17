/* gc_internal.h -- the GameCube spike's machine state, shared by the core's files (XBA-225). */
#ifndef GC_INTERNAL_H
#define GC_INTERNAL_H

#include <fenv.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "gc_core.h"
#include "gc_recomp_rt.h"
#include "gx/gx.h"

/* Clocks: the Gekko at 486 MHz, the time base and decrementer at a twelfth of it (bus clock / 4). The core counts
 * an estimate of each instruction's cycles (gc_cycles: 1 for most, more for multiplies, divides, FP divides,
 * lmw / stmw, synchronisation), lifted code the same sum per block. */
#define GC_CPU_HZ 486000000u
#define GC_TB_DIV 12u
/* Video timing (XBA-259): NTSC 525 lines per frame at 30 frames (60 fields) per second, PAL 625 lines at 25 frames
 * (50 fields). Which one follows the video interface's DCR format (FMT 1 = PAL), read at each field start. */
#define GC_LINES 525u
#define GC_CYCLES_PER_LINE (GC_CPU_HZ / 30u / GC_LINES)
#define GC_LINES_PAL 625u
#define GC_CYCLES_PER_LINE_PAL (GC_CPU_HZ / 25u / GC_LINES_PAL)

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
    uint32_t line;        /* current line, 0..frame_lines - 1 */
    uint32_t frame_lines; /* 525 NTSC, 625 PAL */
    uint32_t field_line;  /* lines since the field started */
    uint64_t line_cycles; /* cycles per line */
    uint64_t tb_base;     /* time base = tb_base + now / GC_TB_DIV (ticks) */
    uint64_t dec_zero;    /* the tick (now / GC_TB_DIV) where the decrementer reads 0: DEC = dec_zero - tick */
    uint64_t dec_fire_tick; /* the next 0 -> -1 transition (the exception), every 2^32 ticks after a write */
    uint64_t dec_fire;    /* ... in cycles */
    bool dec_pending;

    /* address translation (gc_mmu.c): whether the BATs are the default mapping, per MSR[PR] */
    bool dbats_default[2], ibats_default[2];
    uint32_t ifast_mask, ifast_val; /* instruction fetch: as gc_rc_head's dfast_mask / dfast_val */
    /* DSI / ISI from the middle of an instruction: the run loop's resume point */
    jmp_buf fault_jb;
    bool fault_armed;

    /* FPSCR[RN] / [NI] applied to the host's floating-point environment while the CPU runs */
    fenv_t host_fenv;
    uint32_t host_fp_mode; /* RN | NI bits in force (the host's default: 0) */
    bool warned_illegal;

    /* processor interface */
    uint32_t pi_intsr, pi_intmr;
    uint32_t pi_reg[0x40];
    /* video interface: 16-bit registers */
    uint16_t vi[0x80];
    /* DSP interface, ARAM DMA, audio DMA, audio interface, the microcode's HLE (gc_dsp.c, gc_ai.c; XBA-263) */
    struct gc_audio *audio;
    /* serial interface, external interface: 32-bit registers */
    uint32_t si[0x40];    /* gc_si.c: channel buffers, poll, COMCSR, SISR, the 128-byte I/O buffer at 0x20 */
    gc_pad_state pad[4];  /* what each SI port has plugged in (gc_set_pad) */
    uint32_t exi[15];
    struct gc_exi *exidev; /* gc_exi.c: the devices on the EXI (memory cards, RTC, SRAM; XBA-264) */
    uint32_t mi_reg[0x40];
    /* the GPU (gx/: FIFO, CP, PE, renderer; XBA-262) */
    gx_state *gx;

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
    struct gc_rel_state *rel; /* REL modules the game loaded (gc_rel.c, XBA-264) */

    struct gc_di *di;     /* the DVD interface and its disc (gc_di.c), NULL until a disc is inserted */

    int verbose;
    char error[200];
    uint32_t trace[64];   /* last PCs the interpreter ran */
    unsigned trace_at;
};

/* gc_cpu.c */
void gc_cpu_step(gc_machine *m);
unsigned gc_cycles(uint32_t w);
void gc_exception(gc_machine *m, uint32_t vector, uint32_t srr1_bits);
void gc_fp_enter(gc_machine *m); /* the host FP environment for guest code (RN, NI), flags cleared */
void gc_fp_leave(gc_machine *m); /* host exception flags folded into the FPSCR, the host environment back */
void gc_fpscr_sync(gc_machine *m);

/* gc_mmu.c */
void gc_mmu_bats(gc_machine *m);        /* after a write to a BAT (then as gc_mmu_update) */
void gc_mmu_update(gc_machine *m);      /* after a change of MSR (IR, DR, PR) */
bool gc_ifetch(gc_machine *m, uint32_t pc, uint32_t *w); /* false: ISI raised */
bool gc_icode_direct(gc_machine *m, uint32_t pc);         /* the code at pc is MEM1[pc & 0x3FFFFFFF] */
void gc_lc_dma(gc_machine *m);          /* after a write to DMAL with the trigger bit */
uint32_t gc_phys_read(gc_machine *m, uint32_t pa, unsigned size);
void gc_phys_write(gc_machine *m, uint32_t pa, uint32_t v, unsigned size);
void gc_check_interrupts(gc_machine *m);
bool gc_irq_pending(gc_machine *m);
void gc_stop(gc_machine *m, const char *fmt, ...);

/* gc_bus.c */
uint32_t gc_hw_read(gc_machine *m, uint32_t pa, unsigned size);
void gc_hw_write(gc_machine *m, uint32_t pa, uint32_t v, unsigned size);
void gc_vi_line(gc_machine *m);

/* gc_exi.c (XBA-264): the EXI registers 0x0C006800-0x0C00683B (idx = offset / 4) and their devices */
struct gc_exi *gc_exi_create(void);
void gc_exi_free(struct gc_exi *x);
uint32_t gc_exi_read32(gc_machine *m, unsigned idx);
void gc_exi_write32(gc_machine *m, unsigned idx, uint32_t v);
void gc_exi_tv_mode(gc_machine *m, int pal); /* SRAM's video mode follows the TV mode the IPL left */

/* gc_si.c: the serial interface and the standard controllers on it (XBA-259) */
uint32_t gc_si_read(gc_machine *m, unsigned idx);
void gc_si_write(gc_machine *m, unsigned idx, uint32_t v);
/* A line of the field starts (0: the field's first): every X lines of SIPOLL from the field start, the channels that
 * poll latch their controller's state (as Dolphin does; Y, the polls per field, follows from X). */
void gc_si_line(gc_machine *m, uint32_t field_line);

/* gc_core.c */
gc_machine *gc_alloc_machine(char *why, size_t why_len);
void gc_schedule(gc_machine *m);
uint64_t gc_tb(gc_machine *m);

/* gc_recomp.c */
bool gc_recomp_dispatch(gc_machine *m);
void gc_recomp_icbi(gc_machine *m, uint32_t ea);

/* gc_rel.c (XBA-264): after each field, the loaded REL modules (OSLink's list at 0x800030C8) and their records;
 * whether pc is in a loaded module's code; freeing it. */
void gc_rel_field(gc_machine *m);
bool gc_rel_in_code(gc_machine *m, uint32_t pc);
void gc_rel_free(gc_machine *m);

/* gc_di.c (XBA-261): registers 0x0C006000-0x0C00603F (idx = offset / 4), its next event (UINT64_MAX: none), the
 * event itself (a command completes, the cover closes), freeing it and its discs. */
uint32_t gc_di_read32(gc_machine *m, unsigned idx);
void gc_di_write32(gc_machine *m, unsigned idx, uint32_t v);
uint64_t gc_di_next_event(gc_machine *m);
void gc_di_event(gc_machine *m);
void gc_di_free(gc_machine *m);
/* The disc's audio stream (XBA-263): its next frame (L, R) at the streaming rate; 0 when nothing plays. */
int gc_di_stream_frame(gc_machine *m, int16_t lr[2]);

/* gc_dsp.c / gc_ai.c (XBA-263): the DSP interface's registers 0x0C005000.. (idx = offset / 2), the audio
 * interface's 0x0C006C00.. (idx = offset / 4), set-up at power-on, the next event (DSP mail interrupts, ARAM DMA
 * done, audio DMA blocks, the streaming counter's interrupt) and handling it, freeing. */
uint16_t gc_dsp_read16(gc_machine *m, unsigned idx);
void gc_dsp_write16(gc_machine *m, unsigned idx, uint16_t v);
uint32_t gc_ai_read32(gc_machine *m, unsigned idx);
void gc_ai_write32(gc_machine *m, unsigned idx, uint32_t v);
bool gc_audio_init(gc_machine *m);
uint64_t gc_audio_next_event(gc_machine *m);
void gc_audio_event(gc_machine *m);
void gc_audio_free(gc_machine *m);

#endif
