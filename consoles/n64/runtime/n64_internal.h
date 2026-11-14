/* Internal state of the N64 core (XBA-169 / 171 / 172). Not a public API: n64_core.h is. The unit tests include it to
 * set up CPU states directly. */
#ifndef N64_INTERNAL_H
#define N64_INTERNAL_H

#include "n64_core.h"
#include "n64_devices.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define N64_LIKELY(x) __builtin_expect(!!(x), 1)
#define N64_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define N64_LIKELY(x) (x)
#define N64_UNLIKELY(x) (x)
#endif

/* Time: the core counts ticks of half a CPU cycle (187.5 MHz), so the 1.5-cycle average instruction is a whole
 * number: COUNT advances every 4 ticks, the RCP (62.5 MHz) every 3. */
#define N64_TICKS_PER_CYCLE 2
#define N64_TICK_HZ (N64_CPU_HZ * 2ull)

/* ---- COP0 ------------------------------------------------------------------------------------------------------ */
enum {
    C0_INDEX = 0,
    C0_RANDOM = 1,
    C0_ENTRYLO0 = 2,
    C0_ENTRYLO1 = 3,
    C0_CONTEXT = 4,
    C0_PAGEMASK = 5,
    C0_WIRED = 6,
    C0_BADVADDR = 8,
    C0_COUNT = 9,
    C0_ENTRYHI = 10,
    C0_COMPARE = 11,
    C0_STATUS = 12,
    C0_CAUSE = 13,
    C0_EPC = 14,
    C0_PRID = 15,
    C0_CONFIG = 16,
    C0_LLADDR = 17,
    C0_WATCHLO = 18,
    C0_WATCHHI = 19,
    C0_XCONTEXT = 20,
    C0_PARITY = 26,
    C0_CACHEERR = 27,
    C0_TAGLO = 28,
    C0_TAGHI = 29,
    C0_ERROREPC = 30,
};

/* Exception codes (CAUSE bits 2-6). */
enum {
    EXC_INT = 0,
    EXC_MOD = 1,
    EXC_TLBL = 2,
    EXC_TLBS = 3,
    EXC_ADEL = 4,
    EXC_ADES = 5,
    EXC_IBE = 6,
    EXC_DBE = 7,
    EXC_SYS = 8,
    EXC_BP = 9,
    EXC_RI = 10,
    EXC_CPU = 11,
    EXC_OV = 12,
    EXC_TR = 13,
    EXC_FPE = 15,
    EXC_WATCH = 23,
};

#define SR_IE 0x00000001u
#define SR_EXL 0x00000002u
#define SR_ERL 0x00000004u
#define SR_KSU 0x00000018u
#define SR_UX 0x00000020u
#define SR_SX 0x00000040u
#define SR_KX 0x00000080u
#define SR_IM 0x0000FF00u
#define SR_BEV 0x00400000u
#define SR_FR 0x04000000u
#define SR_RE 0x02000000u
#define SR_CU0 0x10000000u
#define SR_CU1 0x20000000u
#define SR_CU2 0x40000000u

#define CAUSE_IP2 0x400u  /* the RCP (MI) */
#define CAUSE_IP7 0x8000u /* COUNT = COMPARE */
#define CAUSE_BD 0x80000000u

/* FCSR. */
#define FCSR_RM 0x3u
#define FCSR_FLAGS_SHIFT 2
#define FCSR_ENABLES_SHIFT 7
#define FCSR_CAUSE_SHIFT 12
#define FCSR_C 0x00800000u
#define FCSR_FS 0x01000000u
enum { FPE_I = 1, FPE_U = 2, FPE_O = 4, FPE_Z = 8, FPE_V = 16, FPE_E = 32 };

typedef struct n64_tlb_entry {
    uint64_t hi;      /* EntryHi as written: R (63:62), VPN2 (39:13), ASID (7:0) */
    uint32_t mask;    /* PageMask (24:13) */
    uint32_t lo[2];   /* EntryLo0 / 1: PFN (25:6), C (5:3), D (2), V (1), G (0) */
    /* Derived on write: the bits a virtual address is compared on, and the global flag (G of both halves). */
    uint64_t vpn_mask;
    uint64_t vpn;
    bool global;
} n64_tlb_entry;

typedef struct n64_cpu {
    uint64_t gpr[32];
    uint64_t hi, lo;
    uint64_t pc;      /* the instruction to execute next */
    uint64_t npc;     /* the one after (a branch target once a branch ran) */
    uint64_t cur;     /* the instruction being executed (EPC of its exceptions) */
    bool ds;          /* the instruction at pc is in a delay slot */
    bool next_ds;
    bool llbit;
    uint64_t cop0[32];
    uint64_t cop0_latch;  /* last value written to a COP0 register (unused registers read it) */
    n64_tlb_entry tlb[32];
    uint64_t fpr[32];
    uint32_t fcsr;
    uint32_t cop2_latch_lo;
    uint64_t cop2_latch;
    /* COUNT runs at half the CPU clock: COUNT = count_base + (now - count_tick) / 4. */
    uint32_t count_base;
    uint64_t count_tick;
    /* Derived from STATUS (n64_cpu_status_changed). */
    int ksu;          /* 0 kernel, 1 supervisor, 2 user (EXL / ERL force kernel) */
    bool addr64;      /* 64-bit addressing in the current mode (KX / SX / UX) */
    bool ops64;       /* 64-bit operations allowed (kernel, or SX / UX) */
    bool irq;         /* an interrupt is to be taken before the next instruction */
} n64_cpu;

/* ---- Events ---------------------------------------------------------------------------------------------------- */
enum { EV_VI, EV_COMPARE, EV_PI, EV_SI, EV_AI, EV_RSP, EV_COUNT };
#define EV_NEVER UINT64_MAX

/* ---- Joybus / saves -------------------------------------------------------------------------------------------- */
typedef struct n64_pad {
    bool connected;
    uint16_t buttons;
    int8_t x, y;
    n64_accessory acc;
    bool rumble;
    bool rumble_probe; /* 0x80 written to 0x8000: the rumble pak answers 0x80 */
} n64_pad;

#define N64_PAK_SIZE 0x8000u
#define N64_EEPROM_MAX 2048u
#define N64_SRAM_SIZE 0x8000u
#define N64_FLASH_SIZE 0x20000u

typedef enum { FLASH_IDLE = 0, FLASH_READ, FLASH_STATUS, FLASH_ERASE, FLASH_WRITE } n64_flash_mode;

/* ---- Audio ----------------------------------------------------------------------------------------------------- */
#define N64_AUDIO_RING 16384u /* sample frames, power of two */

struct n64_machine {
    n64_cpu cpu;
    uint64_t now;                /* ticks */
    uint64_t stop_at;            /* the CPU loop runs while now < stop_at */
    uint64_t ev[EV_COUNT];
    uint64_t instructions;
    uint64_t frames;
    bool frame_done;

    uint8_t *rdram;
    uint32_t rdram_size;
    uint8_t *rom;
    uint32_t rom_size;
    n64_rom_info info;
    n64_options opts;
    bool pal;

    n64_rsp *rsp;
    n64_rdp *rdp;
    uint8_t *spmem;              /* n64_rsp_mem: DMEM + IMEM */
    uint64_t rsp_tick;           /* the RSP has run up to here */

    /* MI */
    uint32_t mi_mode, mi_intr, mi_mask;
    /* RI and the RDRAM registers (stubs IPL3 is happy with) */
    uint32_t ri[8];
    uint32_t rdram_regs[16];
    /* PI */
    uint32_t pi_dram, pi_cart, pi_rd_len, pi_wr_len, pi_status;
    uint32_t pi_dom[2][4];       /* LAT, PWD, PGS, RLS per domain */
    uint32_t pi_latch;           /* a CPU write to the cartridge bus, held by the PI */
    bool pi_latch_valid;
    uint64_t pi_latch_until;
    /* SI / PIF */
    uint32_t si_dram, si_status;
    bool si_read_pending;        /* the DMA in flight is PIF -> RDRAM */
    uint8_t pif_ram[64];
    uint8_t pif_rom_lock;
    n64_pad pad[4];
    uint8_t pak[4][N64_PAK_SIZE];
    bool pak_dirty[4];
    /* VI */
    uint32_t vi[16];
    uint32_t vi_line;            /* the half-line counter's line (0 .. lines per field - 1) */
    uint32_t vi_field;
    uint64_t vi_line_ticks;      /* length of a line */
    uint32_t vi_lines;           /* lines per field */
    /* AI */
    uint32_t ai_dram, ai_len, ai_control, ai_dacrate, ai_bitrate;
    struct { uint32_t addr, len; } ai_fifo[2];
    int ai_count;                /* buffers queued (0..2); ai_fifo[0] plays */
    uint32_t ai_pos;             /* bytes of ai_fifo[0] played */
    uint64_t ai_tick;            /* audio made up to here */
    double ai_src_phase;         /* source samples (fraction) since the current one started */
    double ai_out_phase;         /* output samples (fraction) owed */
    int16_t ai_cur[2];           /* the sample the DAC holds */
    int16_t audio[N64_AUDIO_RING * 2];
    uint32_t audio_rd, audio_wr; /* sample frames, free-running */
    /* Cartridge save */
    n64_save_kind save_kind;     /* NONE until known */
    uint8_t eeprom[N64_EEPROM_MAX];
    uint8_t sram[N64_SRAM_SIZE];
    uint8_t *flash;              /* N64_FLASH_SIZE */
    bool save_dirty;
    n64_flash_mode flash_mode;
    uint32_t flash_status;
    uint32_t flash_offset;       /* erase / write target */
    uint8_t flash_buf[128];
    /* IS-Viewer */
    uint8_t *isv;                /* 64 KB buffer at 0x13FF0000 (allocated at the first write) */
    void (*tty)(void *ctx, char c);
    void *tty_ctx;
};

/* ---- CPU (n64_cpu.c) ------------------------------------------------------------------------------------------- */
void n64_cpu_reset(n64_machine *m);
/* Runs instructions while now < stop_at. */
void n64_cpu_run(n64_machine *m);
/* STATUS or CAUSE changed: recompute the mode and the interrupt line. */
void n64_cpu_status_changed(n64_machine *m);
void n64_cpu_update_irq(n64_machine *m);
uint32_t n64_cpu_count(n64_machine *m);
void n64_cpu_set_count(n64_machine *m, uint32_t v);
void n64_cpu_schedule_compare(n64_machine *m);
/* Raises an exception (EPC from the instruction being run). */
void n64_cpu_exception(n64_machine *m, int code, int ce);
/* Address translation without side effects (debug): 0 and the physical address, or -1. */
int n64_cpu_probe(n64_machine *m, uint64_t va, uint32_t *pa);

/* ---- COP1 (n64_fpu.c) ------------------------------------------------------------------------------------------ */
/* Executes a COP1 instruction (opcode 0x11), CU1 already checked. */
void n64_fpu_cop1(n64_machine *m, uint32_t ins);
/* CTC1 to FCSR (may raise the FPE exception). */
void n64_fpu_write_fcsr(n64_machine *m, uint32_t v);
uint32_t n64_fpu_read_s(n64_cpu *c, int r);
void n64_fpu_write_s(n64_cpu *c, int r, uint32_t v);
uint64_t n64_fpu_read_d(n64_cpu *c, int r);
void n64_fpu_write_d(n64_cpu *c, int r, uint64_t v);

/* ---- Bus (n64_bus.c) ------------------------------------------------------------------------------------------- */
/* Physical accesses (addresses below 0x20000000; RDRAM handled inline by the CPU for speed). Return the extra ticks
 * the access costs through *cost when non-NULL. */
uint32_t n64_bus_read32(n64_machine *m, uint32_t pa);
void n64_bus_write32(n64_machine *m, uint32_t pa, uint32_t v, uint32_t mask);
uint64_t n64_bus_read64(n64_machine *m, uint32_t pa);
void n64_bus_write64(n64_machine *m, uint32_t pa, uint64_t v);
uint32_t n64_bus_cost(uint32_t pa); /* extra ticks of an uncached access there */
void n64_mi_raise(n64_machine *m, int irq);
void n64_mi_clear(n64_machine *m, int irq);
void n64_schedule(n64_machine *m, int ev, uint64_t at);

/* ---- Video / audio (n64_io.c) ---------------------------------------------------------------------------------- */
void n64_vi_reset(n64_machine *m);
uint32_t n64_vi_read(n64_machine *m, uint32_t reg);
void n64_vi_write(n64_machine *m, uint32_t reg, uint32_t v);
void n64_vi_event(n64_machine *m);
void n64_ai_reset(n64_machine *m);
uint32_t n64_ai_read(n64_machine *m, uint32_t reg);
void n64_ai_write(n64_machine *m, uint32_t reg, uint32_t v);
void n64_ai_advance(n64_machine *m); /* make the audio up to now */
void n64_ai_event(n64_machine *m);

/* ---- PIF / joybus / saves (n64_pif.c) -------------------------------------------------------------------------- */
void n64_pif_boot(n64_machine *m);
void n64_pif_process(n64_machine *m); /* runs the joybus commands in PIF RAM */
void n64_pif_write_ram(n64_machine *m, uint32_t off, uint32_t v); /* CPU or SI write of a word */
uint32_t n64_cart2_read32(n64_machine *m, uint32_t pa);
void n64_cart2_write32(n64_machine *m, uint32_t pa, uint32_t v);
void n64_cart2_dma_read(n64_machine *m, uint32_t cart, uint8_t *dst, uint32_t len);  /* cart -> RDRAM */
void n64_cart2_dma_write(n64_machine *m, uint32_t cart, const uint8_t *src, uint32_t len); /* RDRAM -> cart */
void n64_save_set_kind(n64_machine *m, n64_save_kind kind);

#endif
