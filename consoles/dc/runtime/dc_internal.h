/* dc_internal.h -- the Dreamcast spike's machine state, shared by the core's files (XBA-227). */
#ifndef DC_INTERNAL_H
#define DC_INTERNAL_H

#include "dc_core.h"

#define DC_RAM_SIZE (16u << 20)
#define DC_VRAM_SIZE (8u << 20)
#define DC_ARAM_SIZE (2u << 20)
/* Naomi (XBA-235): main RAM and sound RAM grow; VRAM is 16 MB on the board but stays 8 MB here (to do). */
#define NAOMI_RAM_SIZE (32u << 20)
#define NAOMI_ARAM_SIZE (8u << 20)
#define NAOMI_SRAM_SIZE (32u << 10)

#define DC_CPU_HZ 200000000u
#define DC_FRAME_CYCLES (DC_CPU_HZ / 60u)

/* Where the BIOS in C lives (main RAM below the program, which KOS leaves alone): the syscall vectors at
 * 0x8C0000B0.. point at these stubs; the dispatcher serves a stub when PC reaches it. */
#define DC_HLE_BASE 0x8C000800u
enum {
    DC_HLE_SYSINFO = 0,
    DC_HLE_ROMFONT,
    DC_HLE_FLASHROM,
    DC_HLE_GDROM,
    DC_HLE_GDROM2,
    DC_HLE_SYSTEM,
    DC_HLE_EXIT, /* the return address the program starts with: returning there ends the run */
    DC_HLE_COUNT
};
#define DC_HLE_ID_ADDR 0x8C000880u /* the console ID syscall_sysinfo_id returns a pointer to */

/* The Naomi cartridge (dc_naomi.c): a plain / decrypted ROM image the user supplies, read through the board's
 * PIO and DMA registers at 0x005F7000. */
typedef struct dc_naomi_cart {
    uint8_t *rom;
    size_t size;
    uint32_t rom_offset; /* ROM_OFFSETH:L; bit 31 auto-advance, bits 28-30 mode */
    uint32_t dma_offset; /* DMA_OFFSETH:L */
    uint32_t dma_count;
    uint16_t boardid;
    uint32_t g1[0x100 / 4]; /* G1 DMA block 0x005F7400-0x005F74FF (GDSTAR, GDLEN, GDDIR, GDEN, GDST...) */
    uint64_t pio_reads, dma_bytes;
} dc_naomi_cart;

typedef struct dc_tmu {
    uint32_t tcor, tcr;
    uint32_t base_cnt;   /* TCNT at base_cycle */
    uint64_t base_cycle; /* when the count was last set */
} dc_tmu;

struct dc_machine {
    sh4_cpu cpu; /* first: the bus functions cast sh4_cpu * to dc_machine * */
    uint8_t *ram, *vram, *aram;
    uint32_t ram_size, aram_size;
    dc_model model;
    int verbose;

    /* Naomi (model DC_MODEL_NAOMI): battery SRAM at 0x00200000, the cartridge, the JVS I/O state */
    uint8_t *sram;
    dc_naomi_cart cart;
    dc_jvs_state jvs;

    /* SH-4 on-chip */
    uint32_t ccr, mmucr, qacr[2], pmcr[2];
    uint32_t pctra, pdtra, pctrb, pdtrb;
    uint16_t icr, ipr[4];
    uint8_t tocr, tstr;
    dc_tmu tmu[3];
    uint32_t sq[2][8];
    uint16_t scsmr, scscr, scfcr, scsptr, sclsr, scfsr;
    uint8_t scbrr;
    uint32_t onchip_misc[64]; /* registers stored and read back, nothing else */

    /* Holly: system bus block (0x005F6800-0x005F7FFF) and PowerVR2 registers (0x005F8000-0x005F9FFF) */
    uint32_t sb[0x1800 / 4];
    uint32_t pvr[0x2000 / 4];
    uint32_t istnrm, istext, isterr;
    uint32_t aica[0x8000 / 4];

    /* video timing */
    uint32_t line;          /* current scanline */
    uint64_t next_line_at;  /* cycle of the next scanline */
    uint64_t frame;

    /* serial output */
    char *serial;
    size_t serial_len, serial_cap;

    /* run state */
    bool exited;
    int exit_code;
    uint64_t run_end;

    /* lifted code */
    const dc_recomp_table *rc;
    uint32_t *missed;
    size_t nmissed, missed_cap;
    uint32_t img_lo, img_hi; /* the loaded image (P1 addresses) */
    dc_stats stats;
    uint32_t unknown_logged;
};

/* dc_bus.c */
void dc_bus_reset(dc_machine *m);
void dc_hw_write_log(dc_machine *m, const char *what, uint32_t a, uint32_t v);

/* dc_hw.c: timers, video timing, interrupts */
void dc_hw_reset(dc_machine *m);
void dc_hw_events(dc_machine *m);     /* catch up timers / scanlines to cpu.cycles, set cpu.next_event */
bool dc_hw_take_interrupt(dc_machine *m);
uint32_t dc_tmu_read(dc_machine *m, uint32_t reg);
void dc_tmu_write(dc_machine *m, uint32_t reg, uint32_t v);
uint32_t dc_spg_status(dc_machine *m);
void dc_hw_kick(dc_machine *m);       /* interrupt state may have changed: return to the dispatcher soon */

/* dc_naomi.c: the board's G1 window (0x005F7000-0x005F70FF) and G1 DMA (0x005F7400-0x005F74FF) */
uint32_t dc_naomi_g1_read(dc_machine *m, uint32_t p);
void dc_naomi_g1_write(dc_machine *m, uint32_t p, uint32_t v);
void dc_naomi_free(dc_machine *m);

/* dc_bios.c */
void dc_bios_setup(dc_machine *m, uint32_t entry);
bool dc_bios_is_stub(uint32_t pc);
bool dc_bios_call(dc_machine *m, uint32_t pc); /* true when pc was a stub (served) */

/* sh4_interp.c */
void sh4_decode_init(void);
/* Runs instructions from cpu.pc until a taken branch lands on an address with lifted code, max instructions ran,
 * or cycles reach next_event. Returns the instructions run. */
uint32_t sh4_interp_run(dc_machine *m, uint32_t max);
dc_recomp_fn dc_rc_lookup(dc_machine *m, uint32_t addr);

#endif
