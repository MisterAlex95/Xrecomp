/* nds_internal.h -- the DS machine (XBA-223 spike, XBA-272): the two CPUs and what they see. Not an API. */
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
#define NDS_WRAM_SIZE 0x8000u   /* shared WRAM (WRAMCNT splits it) */
#define NDS_WRAM7_SIZE 0x10000u /* the ARM7's own WRAM */
#define NDS_VRAM_SIZE 0xA4000u  /* banks A-I */
#define NDS_IO_SIZE 0x1100u     /* ARM9 0x04000000-0x040010FF: engine A, system, engine B */
#define NDS_IO7_SIZE 0x520u     /* ARM7 0x04000000-0x0400051F: system, sound */
#define NDS_FW_SIZE 0x40000u    /* firmware flash (SPI NVRAM), 256 KB */

/* Timing: one time unit = one 33 MHz cycle; each CPU runs one instruction per unit. A line is 2130 units (355 dots
 * at 6 cycles), 263 lines. The two CPUs run in turn in slices of at most NDS_SLICE units. */
#define NDS_LINE_CYCLES 2130u
#define NDS_LINES 263u
#define NDS_VISIBLE_LINES 192u
#define NDS_SLICE 256u

/* The BIOS addresses the HLE traps (an instruction fetched there runs C instead). ARM9: high vectors; ARM7: low. */
#define NDS_BIOS9_SWI 0xFFFF0008u
#define NDS_BIOS9_IRQ 0xFFFF0018u
#define NDS_BIOS9_IRQ_RET 0xFFFF0400u
#define NDS_BIOS7_SWI 0x00000008u
#define NDS_BIOS7_IRQ 0x00000018u
#define NDS_BIOS7_IRQ_RET 0x00000400u

typedef struct nds_dma {
    uint32_t sad, dad, cnt;
    uint32_t cur_s, cur_d;
    bool armed; /* enabled, waiting for its start condition */
} nds_dma;

typedef struct nds_timer {
    uint16_t reload, ctl;
    uint32_t counter;  /* value at `since` */
    uint64_t since;    /* time of the last update */
    uint32_t frac;     /* units not yet counted (prescaler) */
} nds_timer;

/* One CPU and its private devices (IRQ controller, timers, DMA, its side of the IPC). */
typedef struct nds_arm {
    nds_rc_head h; /* first: lifted code sees it (nds_recomp_rt.h) */
    nds_machine *m;
    unsigned id;   /* 0 = ARM9 (ARMv5TE), 1 = ARM7 (ARMv4T) */
    uint8_t *io;   /* this CPU's I/O register bytes (m->io or m->io7) */

    uint32_t ime, ie, irqf;
    nds_timer tm[4];
    nds_dma dma[4];

    /* IPC: this CPU's side */
    uint16_t ipcsync;        /* bits 8-11 (output nibble), 14 (IRQ enable) */
    uint16_t fifocnt;        /* bits 2, 10, 15 */
    uint32_t fifo[16];       /* this CPU's send FIFO (the other one receives) */
    unsigned fifo_n, fifo_h;
    uint32_t fifo_last;      /* last word popped from the other's FIFO (read on empty) */
    bool fifo_error;

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

    uint32_t trace[64];      /* last interpreted PCs (bit 0 = Thumb), for the error report */
    unsigned trace_at;
} nds_arm;

/* SPI devices behind the ARM7's SPICNT (nds_arm7.c). */
typedef struct nds_spi {
    uint16_t cnt;
    uint8_t data;            /* last byte the selected device returned */
    int dev;                 /* selected device (chip select held), -1 = none */
    unsigned pos;            /* bytes exchanged since the device was selected */
    uint8_t cmd;
    uint32_t addr;
    uint8_t pmic[8];
    uint16_t tsc_result;     /* the touch controller's conversion being shifted out */
} nds_spi;

/* The Seiko S-3511A real-time clock on the ARM7's RTC port (bit-banged serial, nds_arm7.c). */
typedef struct nds_rtc {
    uint8_t port;            /* last value written to 0x04000138 */
    bool cs;
    unsigned bit;            /* bits shifted in the current byte */
    unsigned nbytes;         /* bytes exchanged since CS went high */
    uint8_t cmd, sh;
    uint8_t out[8];          /* data bytes of a read command */
    uint8_t stat1, stat2;
    uint8_t dt[7];           /* year, month, day, weekday, hour, minute, second (BCD) */
} nds_rtc;

struct nds_machine {
    nds_arm a9;              /* first */
    nds_arm a7;

    uint8_t *main_ram;
    uint8_t itcm[NDS_ITCM_SIZE];
    uint8_t dtcm[NDS_DTCM_SIZE];
    uint8_t wram[NDS_WRAM_SIZE];
    uint8_t wram7[NDS_WRAM7_SIZE];
    uint8_t palette[0x800];
    uint8_t oam[0x800];
    uint8_t vram[NDS_VRAM_SIZE];
    uint8_t io[NDS_IO_SIZE];
    uint8_t io7[NDS_IO7_SIZE];
    uint8_t vramcnt[9];
    uint8_t wramcnt;
    uint8_t *vmap_abg[32], *vmap_bbg[8], *vmap_aobj[16], *vmap_bobj[8]; /* 16 KB pages, NULL = unmapped */
    uint8_t *vmap_arm7[2];   /* banks C / D at 0x06000000 / 0x06020000 for the ARM7 (128 KB each) */
    uint8_t *firmware;       /* NDS_FW_SIZE bytes */

    /* CP15 (ARM9) */
    uint32_t cp15_ctl, cp15_dtcm, cp15_itcm, cp15_c13[2], cp15_misc[16];
    uint32_t dtcm_base, dtcm_size, itcm_size;

    /* video timing */
    uint32_t vcount;
    uint64_t line_end;       /* time the current line ends */
    uint64_t slice_end;
    uint64_t frames;
    uint16_t fb_a[256 * 192], fb_b[256 * 192]; /* engine A, engine B */

    int64_t div_numer, div_denom, div_result, div_rem;
    uint64_t sqrt_param;
    uint32_t sqrt_result;

    /* ARM7 devices */
    nds_spi spi;
    nds_rtc rtc;
    uint16_t keys;           /* KEYINPUT, active low (bits 0-9) */
    uint16_t extkeys;        /* EXTKEYIN, active low (bits 0 X, 1 Y, 6 pen, 7 hinge closed = 1) */
    bool touch_down;
    uint16_t touch_x, touch_y; /* screen pixels when touch_down */
    uint8_t haltcnt;
    uint8_t postflg7;

    int verbose;
    char error[160];         /* set when the machine stopped on something it does not support */
};

/* nds_cpu.c (one interpreter, ARMv5TE for the ARM9 and ARMv4T for the ARM7) */
void nds_cpu_reset(nds_arm *c, uint32_t entry);
void nds_cpu_step(nds_arm *c);               /* one instruction (or a BIOS HLE trap) */
void nds_cpu_irq(nds_arm *c);                /* takes the IRQ exception */
uint32_t nds_cpu_cpsr(const nds_arm *c);
void nds_cpu_set_cpsr(nds_arm *c, uint32_t v);
void nds_cpu_stop(nds_arm *c, const char *what, uint32_t pc, uint32_t op);

/* nds_bus.c: ARM9 map, the devices both CPUs have (IRQ, timers, DMA, IPC), line timing */
void nds_bus_reset(nds_machine *m);
void nds_irq_raise(nds_arm *c, uint32_t bits);
void nds_irq_update(nds_arm *c);
void nds_vram_remap(nds_machine *m);
void nds_line_event(nds_machine *m);         /* end of the current line */
void nds_dma_trigger(nds_arm *c, unsigned timing);
void nds_timers_sync(nds_arm *c);
uint32_t nds_common_io_read(nds_arm *c, uint32_t off, bool *handled);   /* 16-bit, halfword aligned */
bool nds_common_io_write(nds_arm *c, uint32_t off, unsigned size, uint32_t val); /* after storing; true = handled */
uint32_t nds_fifo_pop(nds_arm *c);           /* c reads its receive FIFO (the other CPU's send FIFO) */
uint8_t nds9_debug_read8(nds_machine *m, uint32_t addr);
uint8_t *nds9_ptr(nds_machine *m, uint32_t addr);   /* direct host pointer, NULL for I/O or unmapped */
uint32_t nds9_read32(nds_machine *m, uint32_t a);
uint16_t nds9_read16(nds_machine *m, uint32_t a);
uint8_t nds9_read8(nds_machine *m, uint32_t a);
void nds9_write32(nds_machine *m, uint32_t a, uint32_t v);
void nds9_write16(nds_machine *m, uint32_t a, uint16_t v);
void nds9_write8(nds_machine *m, uint32_t a, uint8_t v);

/* nds_bus7.c: the ARM7 map */
uint8_t *nds7_ptr(nds_machine *m, uint32_t addr);
uint32_t nds7_read32(nds_machine *m, uint32_t a);
uint16_t nds7_read16(nds_machine *m, uint32_t a);
uint8_t nds7_read8(nds_machine *m, uint32_t a);
void nds7_write32(nds_machine *m, uint32_t a, uint32_t v);
void nds7_write16(nds_machine *m, uint32_t a, uint16_t v);
void nds7_write8(nds_machine *m, uint32_t a, uint8_t v);
uint8_t nds7_debug_read8(nds_machine *m, uint32_t addr);

/* nds_arm7.c: ARM7-only devices */
void nds_arm7_devices_reset(nds_machine *m);
void nds_firmware_build(uint8_t *fw);        /* a synthetic firmware: header, user settings (no code) */
void nds_spi_cnt_write(nds_machine *m, uint16_t v);
void nds_spi_data_write(nds_machine *m, uint8_t v);
void nds_rtc_write(nds_machine *m, uint8_t v);
uint8_t nds_rtc_read(nds_machine *m);

/* nds_gpu.c */
void nds_gpu_render(nds_machine *m);         /* both engines' frames from the current registers and memory */

/* nds_recomp.c */
bool nds_recomp_dispatch(nds_arm *c);        /* runs native code at PC if any; false: the interpreter must step */
void nds_recomp_note_interp(nds_arm *c);
void nds_recomp_free(nds_arm *c);

static inline uint16_t nds_io16(const nds_machine *m, uint32_t off)
{
    return (uint16_t)(m->io[off] | m->io[off + 1] << 8);
}
static inline uint32_t nds_io32(const nds_machine *m, uint32_t off)
{
    return (uint32_t)nds_io16(m, off) | (uint32_t)nds_io16(m, off + 2) << 16;
}
static inline uint16_t nds_cio16(const nds_arm *c, uint32_t off)
{
    return (uint16_t)(c->io[off] | c->io[off + 1] << 8);
}
static inline uint32_t nds_cio32(const nds_arm *c, uint32_t off)
{
    return (uint32_t)nds_cio16(c, off) | (uint32_t)nds_cio16(c, off + 2) << 16;
}
static inline nds_arm *nds_other(nds_arm *c)
{
    return c->id ? &c->m->a9 : &c->m->a7;
}

#endif
