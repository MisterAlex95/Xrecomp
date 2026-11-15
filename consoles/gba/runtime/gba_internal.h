/* Internal state of the GBA core (XBA-152 / XBA-153). Not a public API: gba_core.h is. The unit tests include
 * it to set up CPU states directly. */
#ifndef GBA_INTERNAL_H
#define GBA_INTERNAL_H

#include "gba_core.h"
#include "gba_ppu.h"
#include "gba_recomp_rt.h"

#include <stddef.h>

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
#define GBA_SAVE_MAX 0x20000u      /* largest save chip: 128 KB flash */
#define GBA_AUDIO_RING 8192u       /* output ring, stereo frames (> 8 video frames at 48 kHz); power of two */

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


typedef struct gba_timer {
    uint16_t reload;
    uint16_t ctl;
    uint16_t counter;       /* value at base_time (counting timers) or the current value (stopped / cascade) */
    uint64_t base_time;     /* cycle the counter had `counter` (counting, non-cascade) */
    uint64_t overflow_at;   /* next overflow cycle, UINT64_MAX when none is due */
    int shift;              /* prescaler: 0, 6, 8, 10 */
} gba_timer;

/* ---- APU (gba_audio.c) ---- */

/* Square channels 1 and 2 (channel 2 has no sweep). */
typedef struct gba_psg_square {
    bool on;
    int vol, env_period, env_timer;
    bool env_up;
    int length;             /* counts down to 0 at 256 Hz while len_en */
    bool len_en;
    int duty, pos;          /* duty pattern 0-3, step 0-7 */
    uint16_t freq;          /* 11-bit rate */
    int32_t timer, period;  /* CPU cycles to the next duty step / between steps */
    int sw_period, sw_shift, sw_timer;
    bool sw_down, sw_en;
    uint16_t shadow;
} gba_psg_square;

typedef struct gba_psg_wave {
    bool on;
    int length;
    bool len_en;
    int pos;                /* sample 0-31 (one bank) or 0-63 (two banks) */
    uint16_t freq;
    int32_t timer, period;
    int vol_mul;            /* volume in quarters: 0, 1, 2, 3 (forced 75 %) or 4 */
} gba_psg_wave;

typedef struct gba_psg_noise {
    bool on;
    int vol, env_period, env_timer;
    bool env_up;
    int length;
    bool len_en;
    uint16_t lfsr;
    bool out;               /* last bit shifted out: high or low */
    int32_t timer, period;  /* period 0: the clock is stopped (shift 14 / 15) */
} gba_psg_noise;

#define GBA_APU_WINDOW 128   /* cycles per mixing window: 131072 Hz */

typedef struct gba_apu {
    uint64_t time;          /* cycle the APU has run up to */
    int win_pos;            /* cycles into the current mixing window (0..GBA_APU_WINDOW-1) */
    int32_t acc[6];         /* the window's integrals: square 1, square 2, wave, noise, FIFO A, FIFO B */
    int fs_windows;         /* windows until the next frame sequencer step (512 Hz) */
    int fs_step;            /* 0-7 */
    gba_psg_square sq[2];
    gba_psg_wave wave;
    gba_psg_noise noise;
    uint8_t wave_ram[2][16];
    int8_t fifo[2][32];
    int fifo_r[2], fifo_n[2];
    int8_t ds_sample[2];    /* sample each Direct Sound channel is playing */
    /* Low-pass (Q8), resampler (window rate -> GBA_AUDIO_RATE, area average) and the output ring. */
    int32_t lpf_l, lpf_r;
    uint32_t out_rem;
    int64_t out_l, out_r;
    uint32_t ring_r, ring_n;
    int16_t ring[GBA_AUDIO_RING * 2];
} gba_apu;

/* ---- save memory (gba_save.c) ---- */
typedef struct gba_save {
    gba_save_kind kind;
    bool dirty;
    /* flash */
    int flash_seq;          /* 0: idle, 1: got AA at 5555, 2: then 55 at 2AAA */
    bool flash_id, flash_erase, flash_program, flash_bank_sel;
    uint32_t flash_bank;    /* 0 or 0x10000 */
    /* EEPROM */
    int eeprom_abits;       /* address width: 6 (512 B) or 14 (8 KB) */
    int eeprom_nbits;       /* bits of the current request received */
    int eeprom_cmd;         /* 2: write, 3: read */
    uint32_t eeprom_addr;
    uint64_t eeprom_data;
    int eeprom_read_left;   /* bits left in a read reply: 68 (4 dummy + 64 data) .. 0 */
    uint8_t data[GBA_SAVE_MAX];
} gba_save;

typedef struct gba_dma {
    uint32_t sad, dad;      /* register values */
    uint16_t cnt_l, cnt_h;
    uint32_t src, dst;      /* internal (latched) */
    uint32_t count;
    bool active_special;
} gba_dma;

struct gba_rc_state; /* gba_recomp.c */

struct gba_machine {
    /* These first members are gba_rc_head (gba_recomp_rt.h), what lifted code sees: same order, checked below. */
    gba_cpu cpu;
    uint64_t now;           /* cycles since creation */
    uint64_t next_event;    /* the CPU loop runs until now reaches it */
    /* Wait states, rebuilt on WAITCNT writes: cycles of one access, [region 0..15][seq]; fetch16 / fetch32: a
     * sequential opcode fetch, the cartridge prefetch included. */
    uint8_t wait16[16][2];
    uint8_t wait32[16][2];
    uint8_t fetch16[16];
    uint8_t fetch32[16];
    bool prefetch;
    bool irq_line;          /* IME && (IE & IF): the CPU takes an IRQ when CPSR.I is clear */
    uint8_t rc_exit;        /* GBA_RC_EXIT_* */
    uint8_t rc_hint;        /* set by lifted code with GBA_RC_EXIT_INTERP (gba_recomp_rt.h), cleared before each call */
    uint32_t open_bus;      /* last fetched opcode (open bus approximation) */
    uint64_t rc_native;     /* instructions lifted code ran (packs built with -DGBA_RC_COUNT) */
    uint8_t *rc_ewram;      /* = ewram, iwram below (lifted code's inline accesses) */
    uint8_t *rc_iwram;

    struct gba_rc_state *rc; /* the attached recompiled code (gba_recomp.c), NULL: interpreter only */

    uint64_t line_start;    /* cycle the current line began */
    int line_phase;         /* 0: HDraw, 1: after the render point, 2: HBlank */
    uint32_t vcount;
    uint64_t frames;
    bool frame_done;
    uint16_t *fb;           /* framebuffer of the running frame, or NULL */

    /* Interrupts. irq_line (what the CPU sees) follows IME && (IE & IF) GBA_IRQ_LATENCY cycles late: irq_at is the
     * cycle a pending request reaches the CPU, UINT64_MAX when none is on its way. */
    uint16_t ie, if_, ime;
    uint64_t irq_at;

    uint16_t keys;          /* KEYINPUT as read (0 = pressed) */
    uint16_t keycnt;

    gba_timer timer[4];
    gba_dma dma[4];
    uint32_t dma_latch;     /* last value a DMA moved (open bus of DMA reads) */

    /* Affine reference points, 20.8 fixed point sign-extended from 28 bits. */
    int32_t bg2x, bg2y, bg3x, bg3y;

    uint32_t bios_latch;    /* last opcode read from the BIOS (what other code reads there) */
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

    gba_apu apu;
    gba_save save;

    int16_t sine[256];      /* BIOS sine table (HLE BgAffineSet / ObjAffineSet), 1.14 fixed point */

    uint64_t rom_hash;      /* FNV-1a 64 of the ROM | 1, 0 until a save state needs it (gba_state.c) */

    /* mGBA's debug-print port (gba_set_debug_log): the host's callback (not state), whether the game opened the port
     * and the string it is writing. */
    gba_debug_log_fn dbg_cb;
    void *dbg_ctx;
    bool dbg_open;
    char dbg_str[257];
};

/* gba_machine starts with gba_rc_head's members. */
#define GBA_RC_SAME(f) \
    _Static_assert(offsetof(gba_machine, f) == offsetof(gba_rc_head, f), "gba_rc_head layout: " #f)
GBA_RC_SAME(cpu);
GBA_RC_SAME(now);
GBA_RC_SAME(next_event);
GBA_RC_SAME(wait16);
GBA_RC_SAME(wait32);
GBA_RC_SAME(fetch16);
GBA_RC_SAME(fetch32);
GBA_RC_SAME(prefetch);
GBA_RC_SAME(irq_line);
GBA_RC_SAME(rc_exit);
GBA_RC_SAME(rc_hint);
GBA_RC_SAME(open_bus);
GBA_RC_SAME(rc_native);
GBA_RC_SAME(rc_ewram);
GBA_RC_SAME(rc_iwram);
_Static_assert(BANK_COUNT == 6, "gba_cpu banks");

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

/* gba_bus.c: CPU-visible accesses (add their cycles to m->now; the bus aligns addresses to the access width except
 * in the save area, whose 8-bit bus sees them whole; rotation of misaligned loads is the CPU's job). seq: sequential
 * access. */
uint8_t gba_read8(gba_machine *m, uint32_t addr, bool seq);
uint16_t gba_read16(gba_machine *m, uint32_t addr, bool seq);
uint32_t gba_read32(gba_machine *m, uint32_t addr, bool seq);
void gba_write8(gba_machine *m, uint32_t addr, uint8_t v, bool seq);
void gba_write16(gba_machine *m, uint32_t addr, uint16_t v, bool seq);
void gba_write32(gba_machine *m, uint32_t addr, uint32_t v, bool seq);
uint16_t gba_fetch16(gba_machine *m, uint32_t addr, bool seq);
uint32_t gba_fetch32(gba_machine *m, uint32_t addr, bool seq);
void gba_update_waitstates(gba_machine *m);
/* The opcode the CPU prefetched while running the one at pc (ARM [pc+8]; Thumb by region, GBATEK « Reading from
 * Unused Memory »): what the opcode fetch leaves in m->open_bus, so what unmapped and write-only reads return. */
uint32_t gba_prefetch_latch(gba_machine *m, uint32_t pc, bool thumb);
static inline uint16_t gba_rom_half(const gba_machine *m, uint32_t addr)
{
    uint32_t a = addr & 0x1FFFFFE;
    return a < m->rom_size ? rd16(&m->rom[a]) : (uint16_t)(a >> 1); /* past the end: the latched address */
}

/* gba_io.c */
uint16_t gba_io_read16(gba_machine *m, uint32_t addr);
bool gba_io_open_bus(uint32_t addr); /* a CPU / DMA read there sees the open bus */
void gba_io_write16(gba_machine *m, uint32_t addr, uint16_t v);
void gba_io_write8(gba_machine *m, uint32_t addr, uint8_t v);
void gba_raise_irq(gba_machine *m, uint16_t bits);
/* An interrupt request raised by an event due at `when` (<= now): it reaches the CPU GBA_IRQ_LATENCY cycles after. */
void gba_raise_irq_at(gba_machine *m, uint16_t bits, uint64_t when);
void gba_update_irq(gba_machine *m);
void gba_schedule(gba_machine *m);
void gba_io_reset(gba_machine *m);

/* gba_audio.c: sound registers 0x60-0xA7 (byte granularity), FIFO timer ticks, catching up. */
void gba_apu_reset(gba_machine *m);
void gba_apu_sync(gba_machine *m, uint64_t until);
uint8_t gba_apu_read8(gba_machine *m, uint32_t addr);
void gba_apu_write8(gba_machine *m, uint32_t addr, uint8_t v);
/* Timer overflow on FIFO f's timer at cycle `when`: plays the next sample; returns the bytes left queued. */
int gba_apu_fifo_tick(gba_machine *m, int f, uint64_t when);

/* gba_save.c: kind detection, the 0x0E000000 area (8-bit bus) and the EEPROM. */
void gba_save_init(gba_machine *m);
uint8_t gba_save_read8(gba_machine *m, uint32_t addr);
void gba_save_write8(gba_machine *m, uint32_t addr, uint8_t v);
uint16_t gba_eeprom_read(gba_machine *m);
void gba_eeprom_write(gba_machine *m, uint16_t v);
/* DMA3 is about to move `count` halfwords to the EEPROM: 9 / 73 mean a 6-bit address, 17 / 81 a 14-bit one. */
void gba_eeprom_dma_hint(gba_machine *m, uint32_t count);
static inline bool gba_eeprom_area(const gba_machine *m, uint32_t addr)
{
    return (m->save.kind == GBA_SAVE_EEPROM512 || m->save.kind == GBA_SAVE_EEPROM8K) && (addr >> 24) == 0xD &&
           (m->rom_size <= 0x1000000u || (addr & 0xFFFFFF) >= 0xFFFF00u);
}

/* gba_cpu.c */
void gba_cpu_reset(gba_machine *m, uint32_t pc, uint32_t cpsr);
/* Runs instructions until m->now >= m->next_event (or the CPU halts). */
void gba_cpu_run(gba_machine *m);
/* Executes exactly one instruction (or takes a pending IRQ). For tests. */
void gba_cpu_step(gba_machine *m);
/* The recompiler's loop for code it does not cover (gba_recomp_run): steps while the next instruction is outside the
 * cartridge ROM or an IRQ is due to be taken, and the frame's time is not up and the CPU not halted, as that loop
 * would call gba_cpu_step for each, without a call per instruction. */
void gba_cpu_step_outside_rom(gba_machine *m);
void gba_state_pin_rom_hash(gba_machine *m); /* gba_state.c: the ROM hash taken now (before a cheat patches it) */
uint32_t gba_get_cpsr(const gba_machine *m);
void gba_set_cpsr(gba_machine *m, uint32_t v);
void gba_set_mode(gba_machine *m, uint32_t mode);
/* Address of the next instruction. */
uint32_t gba_cpu_pc(const gba_machine *m);
/* Jumps to addr in the given state (thumb = 1 for Thumb). */
void gba_cpu_jump(gba_machine *m, uint32_t addr, bool thumb);

/* gba_recomp.c: the run loop with a table attached (dispatch to lifted code, interpreter otherwise). */
void gba_recomp_run(gba_machine *m);
void gba_recomp_free(gba_machine *m);
/* A save state was loaded (gba_state.c): the hand-offs between lifted code and the interpreter in flight are
 * forgotten (the dispatch cache stays: it is a function of the ROM). */
void gba_recomp_state_loaded(gba_machine *m);

/* gba_bios.c */
void gba_bios_install_builtin(gba_machine *m);
void gba_bios_direct_boot(gba_machine *m);
/* The built-in BIOS's SWI handler hit its HLE trap: run SWI `num` in C. */
void gba_bios_hle_swi(gba_machine *m, uint32_t num);
#define GBA_HLE_TRAP 0xE7F0DEF0u   /* an ARM undefined encoding, only honoured inside the built-in BIOS */

#endif
