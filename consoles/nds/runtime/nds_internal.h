/* nds_internal.h -- the DS machine of the spike (XBA-223): the ARM9 and what it sees. Not an API. */
#ifndef NDS_INTERNAL_H
#define NDS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nds_core.h"
#include "nds_recomp_rt.h"

#define NDS_MAIN_SIZE 0x400000u
#define NDS_ITCM_SIZE 0x8000u
#define NDS_DTCM_SIZE 0x4000u
#define NDS_WRAM_SIZE 0x8000u
#define NDS_VRAM_SIZE 0xA4000u /* banks A-I */
#define NDS_IO_SIZE 0x1100u    /* 0x04000000-0x040010FF: engine A, system, engine B */

/* Timing: one cycle per instruction, a line of 2130 cycles (355 dots at 6 cycles of 33 MHz), 263 lines. */
#define NDS_LINE_CYCLES 2130u
#define NDS_LINES 263u
#define NDS_VISIBLE_LINES 192u

/* The BIOS addresses the HLE traps (an instruction fetched there runs C instead). */
#define NDS_BIOS_SWI 0xFFFF0008u
#define NDS_BIOS_IRQ 0xFFFF0018u
#define NDS_BIOS_IRQ_RET 0xFFFF0400u

typedef struct nds_dma {
    uint32_t sad, dad, cnt;
    uint32_t cur_s, cur_d;
    bool armed; /* enabled, waiting for its start condition */
} nds_dma;

typedef struct nds_timer {
    uint16_t reload, ctl;
    uint32_t counter;  /* value at `since` */
    uint64_t since;    /* cycle of the last update */
    uint32_t frac;     /* cycles not yet counted (prescaler) */
} nds_timer;

struct nds_machine {
    nds_rc_head h; /* first: lifted code sees it (nds_recomp_rt.h) */

    uint8_t *main_ram;
    uint8_t itcm[NDS_ITCM_SIZE];
    uint8_t dtcm[NDS_DTCM_SIZE];
    uint8_t wram[NDS_WRAM_SIZE];
    uint8_t palette[0x800];
    uint8_t oam[0x800];
    uint8_t vram[NDS_VRAM_SIZE];
    uint8_t io[NDS_IO_SIZE];
    uint8_t vramcnt[9];
    uint8_t *vmap_abg[32], *vmap_bbg[8], *vmap_aobj[16], *vmap_bobj[8]; /* 16 KB pages, NULL = unmapped */

    /* CP15 */
    uint32_t cp15_ctl, cp15_dtcm, cp15_itcm, cp15_c13[2], cp15_misc[16];
    uint32_t dtcm_base, dtcm_size, itcm_size;

    /* IRQ */
    uint32_t ime, ie, irqf;

    /* video timing */
    uint32_t vcount;
    uint64_t line_end;   /* cycle the current line ends */
    uint64_t frames;
    uint16_t fb_a[256 * 192], fb_b[256 * 192]; /* engine A, engine B */

    nds_timer tm[4];
    nds_dma dma[4];
    uint32_t dma_fill[4];
    int64_t div_numer, div_denom, div_result, div_rem;
    uint64_t sqrt_param;
    uint32_t sqrt_result;

    /* IPC with the stub ARM7 (nds_arm7.c) */
    uint16_t ipcsync_out9;   /* ARM9's IPCSYNC bits 8-11, 13, 14 */
    uint8_t ipcsync_in9;     /* what the ARM7 sends (bits 0-3) */
    uint16_t fifocnt;        /* ARM9's IPCFIFOCNT control bits (2, 10, 14, 15) */
    uint32_t fifo79[16];     /* ARM7 -> ARM9 */
    unsigned n79, h79;
    uint32_t fifo_last;      /* last word popped (read on empty) */
    bool fifo_error;
    int arm7_phase;          /* 0 boot handshake, 1 running */
    uint32_t arm7_unknown;   /* packets the stub did not answer */

    /* BIOS HLE */
    bool swi_wait_first;
    bool branched;           /* the instruction being run wrote PC (nds_cpu.c) */

    /* recompiled code */
    const nds_recomp_table *rc;
    uint8_t *span_state;     /* per span: 0 unknown, 1 valid */
    uint64_t *span_checked;  /* per span: interpreted-instruction count at the last failed check */
    uint32_t rc_cache_pc[1024];
    int32_t rc_cache_idx[1024];
    uint32_t *missed;
    size_t nmissed, cap_missed;
    nds_recomp_stats stats;
    bool rc_after_jump;      /* the next interpreted instruction starts a run: a miss to record */

    int verbose;
    uint32_t trace[64];      /* last interpreted PCs (bit 0 = Thumb), for the error report */
    unsigned trace_at;
    char error[160];         /* set when the machine stopped on something it does not support */
};

/* nds_cpu.c */
void nds_cpu_reset(nds_machine *m, uint32_t entry);
void nds_cpu_step(nds_machine *m);           /* one instruction (or a BIOS HLE trap) */
void nds_cpu_irq(nds_machine *m);            /* takes the IRQ exception */
uint32_t nds_cpu_cpsr(const nds_machine *m);
void nds_cpu_set_cpsr(nds_machine *m, uint32_t v);

/* nds_bus.c */
void nds_bus_reset(nds_machine *m);
void nds_irq_raise(nds_machine *m, uint32_t bits);
void nds_irq_update(nds_machine *m);
void nds_vram_remap(nds_machine *m);
void nds_line_event(nds_machine *m);         /* end of the current line */
void nds_dma_trigger(nds_machine *m, unsigned timing); /* 1 = VBlank, 2 = HBlank */
void nds_timers_sync(nds_machine *m);
uint8_t nds_debug_read8(nds_machine *m, uint32_t addr);

/* nds_arm7.c: the stub ARM7 */
void nds_arm7_reset(nds_machine *m);
void nds_arm7_sync_written(nds_machine *m);  /* ARM9 wrote IPCSYNC */
void nds_arm7_fifo_recv(nds_machine *m, uint32_t word); /* ARM9 sent a word */
void nds_fifo_push79(nds_machine *m, uint32_t word);

/* nds_gpu.c */
void nds_gpu_render(nds_machine *m);         /* both engines' frames from the current registers and memory */

/* nds_recomp.c */
bool nds_recomp_dispatch(nds_machine *m);    /* runs native code at PC if any; false: the interpreter must step */
void nds_recomp_note_interp(nds_machine *m);

static inline uint16_t nds_io16(const nds_machine *m, uint32_t off)
{
    return (uint16_t)(m->io[off] | m->io[off + 1] << 8);
}
static inline uint32_t nds_io32(const nds_machine *m, uint32_t off)
{
    return (uint32_t)nds_io16(m, off) | (uint32_t)nds_io16(m, off + 2) << 16;
}

#endif
