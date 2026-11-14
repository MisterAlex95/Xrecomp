/* Internal state of the GBA core (XBA-152 / XBA-153). Not a public API: gba_core.h is. The unit tests include
 * it to set up CPU states directly. */
#ifndef GBA_INTERNAL_H
#define GBA_INTERNAL_H

#include "gba_core.h"
#include "gba_ppu.h"

#include <stdbool.h>
#include <stdint.h>

#define GBA_BIOS_SIZE 0x4000u
#define GBA_EWRAM_SIZE 0x40000u
#define GBA_IWRAM_SIZE 0x8000u
#define GBA_IO_SIZE 0x400u
#define GBA_PAL_SIZE 0x400u
#define GBA_VRAM_SIZE 0x18000u
#define GBA_OAM_SIZE 0x400u
#define GBA_ROM_MAX 0x2000000u
#define GBA_SRAM_SIZE 0x10000u

/* CPU modes (CPSR bits 0-4). */
enum {
    MODE_USR = 0x10,
    MODE_FIQ = 0x11,
    MODE_IRQ = 0x12,
    MODE_SVC = 0x13,
    MODE_ABT = 0x17,
    MODE_UND = 0x1B,
    MODE_SYS = 0x1F,
};

/* Bank index of a mode's r13/r14/SPSR. */
enum { BANK_USR, BANK_FIQ, BANK_IRQ, BANK_SVC, BANK_ABT, BANK_UND, BANK_COUNT };

#define CPSR_T 0x20u
#define CPSR_F 0x40u
#define CPSR_I 0x80u

/* Interrupt sources (IE / IF bits). */
enum {
    IRQ_VBLANK = 1 << 0,
    IRQ_HBLANK = 1 << 1,
    IRQ_VCOUNT = 1 << 2,
    IRQ_TIMER0 = 1 << 3,
    IRQ_SERIAL = 1 << 7,
    IRQ_DMA0 = 1 << 8,
    IRQ_KEYPAD = 1 << 12,
    IRQ_GAMEPAK = 1 << 13,
};

/* DMA start timings. */
enum { DMA_NOW = 0, DMA_VBLANK = 1, DMA_HBLANK = 2, DMA_SPECIAL = 3 };

typedef struct gba_cpu {
    uint32_t r[16];         /* current bank; between instructions r[15] = next instruction + 8 (ARM) / + 4 (Thumb) */
    uint32_t n, z, c, v;    /* flags, 0 or 1 */
    uint32_t cpsr_ctl;      /* CPSR bits 0-7 (mode, T, F, I); flags live in n z c v */
    uint32_t bank_r13[BANK_COUNT], bank_r14[BANK_COUNT], spsr[BANK_COUNT];
    uint32_t fiq_r8_12[5], usr_r8_12[5];
    bool branched;          /* set by an instruction that wrote r15 */
    bool halted;
    bool stopped;
} gba_cpu;

typedef struct gba_timer {
    uint16_t reload;
    uint16_t ctl;
    uint16_t counter;       /* value at base_time (counting timers) or the current value (stopped / cascade) */
    uint64_t base_time;     /* cycle the counter had `counter` (counting, non-cascade) */
    uint64_t overflow_at;   /* next overflow cycle, UINT64_MAX when none is due */
    int shift;              /* prescaler: 0, 6, 8, 10 */
} gba_timer;

typedef struct gba_dma {
    uint32_t sad, dad;      /* register values */
    uint16_t cnt_l, cnt_h;
    uint32_t src, dst;      /* internal (latched) */
    uint32_t count;
    bool active_special;
} gba_dma;

struct gba_machine {
    gba_cpu cpu;

    uint64_t now;           /* cycles since creation */
    uint64_t next_event;    /* the CPU loop runs until now reaches it */
    uint64_t line_start;    /* cycle the current line began */
    int line_phase;         /* 0: HDraw, 1: after the render point, 2: HBlank */
    uint32_t vcount;
    uint64_t frames;
    bool frame_done;
    uint16_t *fb;           /* framebuffer of the running frame, or NULL */

    /* Interrupts. */
    uint16_t ie, if_, ime;
    bool irq_line;          /* IME && (IE & IF): the CPU takes an IRQ when CPSR.I is clear */

    uint16_t keys;          /* KEYINPUT as read (0 = pressed) */
    uint16_t keycnt;

    gba_timer timer[4];
    gba_dma dma[4];
    uint32_t dma_latch;     /* last value a DMA moved (open bus of DMA reads) */
    int fifo_count[2];      /* bytes queued in sound FIFO A / B (no sound yet: only drives the FIFO DMAs) */

    /* Affine reference points, 20.8 fixed point sign-extended from 28 bits. */
    int32_t bg2x, bg2y, bg3x, bg3y;

    /* Wait states, rebuilt on WAITCNT writes: cycles of one access, [region 0..15][seq]. */
    uint8_t wait16[16][2];
    uint8_t wait32[16][2];
    bool prefetch;

    uint32_t bios_latch;    /* last opcode read from the BIOS (what other code reads there) */
    uint32_t open_bus;      /* last fetched opcode (open bus approximation) */
    bool builtin_bios;      /* no BIOS image given: our own vectors + HLE SWIs */
    uint32_t rom_size;

    uint8_t *rom;           /* rom_size bytes, padded to a word */
    uint8_t bios[GBA_BIOS_SIZE];
    uint8_t ewram[GBA_EWRAM_SIZE];
    uint8_t iwram[GBA_IWRAM_SIZE];
    uint8_t io[GBA_IO_SIZE];
    uint8_t pal[GBA_PAL_SIZE];
    uint8_t vram[GBA_VRAM_SIZE];
    uint8_t oam[GBA_OAM_SIZE];
    uint8_t sram[GBA_SRAM_SIZE];

    int16_t sine[256];      /* BIOS sine table (HLE BgAffineSet / ObjAffineSet), 1.14 fixed point */
};

/* Little-endian helpers. */
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* gba_bus.c: CPU-visible accesses (add their cycles to m->now; addresses are aligned by the caller's width,
 * rotation of misaligned loads is the CPU's job). seq: sequential access. */
uint8_t gba_read8(gba_machine *m, uint32_t addr, bool seq);
uint16_t gba_read16(gba_machine *m, uint32_t addr, bool seq);
uint32_t gba_read32(gba_machine *m, uint32_t addr, bool seq);
void gba_write8(gba_machine *m, uint32_t addr, uint8_t v, bool seq);
void gba_write16(gba_machine *m, uint32_t addr, uint16_t v, bool seq);
void gba_write32(gba_machine *m, uint32_t addr, uint32_t v, bool seq);
uint16_t gba_fetch16(gba_machine *m, uint32_t addr, bool seq);
uint32_t gba_fetch32(gba_machine *m, uint32_t addr, bool seq);
void gba_update_waitstates(gba_machine *m);

/* gba_io.c */
uint16_t gba_io_read16(gba_machine *m, uint32_t addr);
void gba_io_write16(gba_machine *m, uint32_t addr, uint16_t v);
void gba_io_write8(gba_machine *m, uint32_t addr, uint8_t v);
void gba_raise_irq(gba_machine *m, uint16_t bits);
void gba_update_irq(gba_machine *m);
void gba_schedule(gba_machine *m);
void gba_io_reset(gba_machine *m);

/* gba_cpu.c */
void gba_cpu_reset(gba_machine *m, uint32_t pc, uint32_t cpsr);
/* Runs instructions until m->now >= m->next_event (or the CPU halts). */
void gba_cpu_run(gba_machine *m);
/* Executes exactly one instruction (or takes a pending IRQ). For tests. */
void gba_cpu_step(gba_machine *m);
uint32_t gba_get_cpsr(const gba_machine *m);
void gba_set_cpsr(gba_machine *m, uint32_t v);
void gba_set_mode(gba_machine *m, uint32_t mode);
/* Address of the next instruction. */
uint32_t gba_cpu_pc(const gba_machine *m);
/* Jumps to addr in the given state (thumb = 1 for Thumb). */
void gba_cpu_jump(gba_machine *m, uint32_t addr, bool thumb);

/* gba_bios.c */
void gba_bios_install_builtin(gba_machine *m);
void gba_bios_direct_boot(gba_machine *m);
/* The built-in BIOS's SWI handler hit its HLE trap: run SWI `num` in C. */
void gba_bios_hle_swi(gba_machine *m, uint32_t num);
#define GBA_HLE_TRAP 0xE7F0DEF0u   /* an ARM undefined encoding, only honoured inside the built-in BIOS */

#endif
