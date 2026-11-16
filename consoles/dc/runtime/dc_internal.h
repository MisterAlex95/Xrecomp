/* dc_internal.h -- the Dreamcast spike's machine state, shared by the core's files (XBA-227). */
#ifndef DC_INTERNAL_H
#define DC_INTERNAL_H

#include "dc_core.h"

#define DC_RAM_SIZE (16u << 20)
#define DC_VRAM_SIZE (8u << 20)
#define DC_ARAM_SIZE (2u << 20)

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

typedef struct dc_tmu {
    uint32_t tcor, tcr;
    uint32_t base_cnt;   /* TCNT at base_cycle */
    uint64_t base_cycle; /* when the count was last set */
} dc_tmu;

struct dc_machine {
    sh4_cpu cpu; /* first: the bus functions cast sh4_cpu * to dc_machine * */
    uint8_t *ram, *vram, *aram;
    int verbose;

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
