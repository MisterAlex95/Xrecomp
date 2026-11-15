/* Game Boy Advance core (XBA-152 interpreter, XBA-153 bus / timers / DMA / IRQ / keypad / BIOS).
 *
 * An ARM7TDMI interpreter (ARM + Thumb) with the GBA memory map, IO, timers, DMA, interrupts and LCD timing.
 * Graphics are drawn by the PPU (gba_ppu.h), called once per visible line. All the state lives in one
 * gba_machine: no globals, several machines may run side by side (one per thread).
 *
 * BIOS: pass a BIOS image (16 KB, e.g. the open-source Cult-of-GBA BIOS built by
 * tools/consoles/build_gba_bios.sh) and the SWIs run through it (LLE). Pass NULL and a small built-in BIOS of
 * our own takes over: IRQ vector, IntrWait / VBlankIntrWait in ARM code, the other SWIs in C (HLE). Never a
 * Nintendo BIOS dump in this repository. */
#ifndef GBA_CORE_H
#define GBA_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gba_machine gba_machine;

#define GBA_SCREEN_W 240
#define GBA_SCREEN_H 160
#define GBA_CYCLES_PER_LINE 1232
#define GBA_LINES 228
#define GBA_CYCLES_PER_FRAME (GBA_CYCLES_PER_LINE * GBA_LINES) /* 280896, 16.78 MHz / 59.73 Hz */

/* KEYINPUT bit order (here 1 = pressed). */
enum {
    GBA_KEY_A = 1 << 0,
    GBA_KEY_B = 1 << 1,
    GBA_KEY_SELECT = 1 << 2,
    GBA_KEY_START = 1 << 3,
    GBA_KEY_RIGHT = 1 << 4,
    GBA_KEY_LEFT = 1 << 5,
    GBA_KEY_UP = 1 << 6,
    GBA_KEY_DOWN = 1 << 7,
    GBA_KEY_R = 1 << 8,
    GBA_KEY_L = 1 << 9,
};

/* Creates a machine and direct-boots it: registers, stacks and IO as the BIOS leaves them, PC = 0x08000000.
 * rom: cartridge image (copied, at most 32 MB). bios: a 16 KB BIOS image (copied) or NULL for the built-in
 * one. Returns NULL on a bad size or out of memory. */
gba_machine *gba_create(const uint8_t *rom, size_t rom_size, const uint8_t *bios, size_t bios_size);

/* Restarts from address 0 through the BIOS's own boot code (LLE boot, needs a real BIOS image: with the
 * built-in one this is a direct boot again). */
void gba_boot_bios(gba_machine *m);

/* Keys held down, KEYINPUT bit order, 1 = pressed. */
void gba_set_keys(gba_machine *m, uint16_t keys_pressed_mask);

/* Runs until the start of the next VBlank (line 160), so the frame's 160 lines are drawn. framebuffer:
 * 240 x 160 RGB555, may be NULL (then the PPU is not called). */
void gba_run_frame(gba_machine *m, uint16_t *framebuffer);

/* Debug / test accessors: read memory without side effects or cycles (IO reads return the raw register
 * image). */
uint32_t gba_peek32(gba_machine *m, uint32_t addr);
uint16_t gba_peek16(gba_machine *m, uint32_t addr);
uint8_t gba_peek8(gba_machine *m, uint32_t addr);

/* Cheats (XBA-197, recomp_cheats.h): a write of size 1, 2 or 4 bytes without bus timing, between frames. EWRAM,
 * IWRAM, palette, VRAM and OAM are written directly, I/O registers through their write handlers; anything else is
 * ignored. gba_rom_patch16: replaces the 16-bit cartridge word at addr (0x08000000-0x09FFFFFF, inside the ROM),
 * returns 0 or -1. The ROM's hash for save states is taken before the first patch, so states stay the game's. */
void gba_cheat_poke(gba_machine *m, uint32_t addr, uint32_t v, int size);
int gba_rom_patch16(gba_machine *m, uint32_t addr, uint16_t v);

/* CPU registers (r0..r15; r15 = address of the next instruction) and CPSR, for tests and debugging. */
uint32_t gba_reg(gba_machine *m, int r);
uint32_t gba_cpsr(gba_machine *m);
/* Cycles run since creation. */
uint64_t gba_cycles(gba_machine *m);
/* Frames run (VBlanks reached) since creation. */
uint64_t gba_frames(gba_machine *m);

/* The save chip's raw storage, writable (tests, debugging): as gba_save_data, but 32 KB when the kind is NONE. */
uint8_t *gba_sram(gba_machine *m, size_t *size);

/* Audio (G-6, XBA-155): stereo int16, interleaved L/R, at GBA_AUDIO_RATE, produced while gba_run_frame runs and
 * buffered inside the machine (at least 8 frames' worth; the oldest samples are dropped on overflow). Copies at most
 * max_frames sample frames into out and returns how many. Call it from the thread that runs gba_run_frame (no lock). */
#define GBA_AUDIO_RATE 48000
size_t gba_audio_pull(gba_machine *m, int16_t *out, size_t max_frames);

/* Saves (G-7, XBA-156). The kind is detected from the ROM (library ID strings) or from the game's first accesses.
 * gba_save_data: the current save image (NULL when none) and its size, to write to storage as is.
 * gba_save_take_dirty: 1 when the game wrote its save since the last call (then the host writes it out, e.g. a
 * second after the last write and at exit), and clears the flag. gba_save_load: puts a stored image back (before the
 * first frame); returns 0, or -1 when the size does not fit the kind. */
typedef enum gba_save_kind {
    GBA_SAVE_NONE,
    GBA_SAVE_SRAM,     /* 32 KB SRAM / FRAM */
    GBA_SAVE_FLASH64,  /* 64 KB flash */
    GBA_SAVE_FLASH128, /* 128 KB flash (two banks) */
    GBA_SAVE_EEPROM512,
    GBA_SAVE_EEPROM8K,
} gba_save_kind;
gba_save_kind gba_save_type(gba_machine *m);
const uint8_t *gba_save_data(gba_machine *m, size_t *size);
int gba_save_take_dirty(gba_machine *m);
int gba_save_load(gba_machine *m, const uint8_t *data, size_t size);

/* Save states (XBA-182): the whole machine (CPU, memories, IO, PPU, APU, timers, DMA, the save chip and its
 * contents) as a versioned blob: a header (magic, console, GBA_STATE_VERSION, the ROM's and the BIOS's hashes) and
 * its sections (gba_state.c). Take and load them between frames (not from inside gba_run_frame). A state is the
 * machine only: it loads the same whether the game runs interpreted or with a recompiled pack attached, before or
 * after (the lifted code's hand-offs in flight are reset).
 * gba_state_size: the bytes gba_state_save needs. gba_state_save: writes the state into out, returns its size (0 when
 * cap is too small). gba_state_check / gba_state_load: GBA_STATE_OK, or why the state is refused (the machine is
 * left as it was): another game or BIOS, another format version or machine layout (another build), not a state.
 * Loading leaves the save chip's dirty flag clear: the state's save memory reaches storage only once the game
 * writes its save again. */
#define GBA_STATE_VERSION 1
enum {
    GBA_STATE_OK = 0,
    GBA_STATE_EGAME = -3,
    GBA_STATE_EVERSION = -4,
    GBA_STATE_ECORRUPT = -7,
};
size_t gba_state_size(gba_machine *m);
size_t gba_state_save(gba_machine *m, uint8_t *out, size_t cap);
int gba_state_check(gba_machine *m, const uint8_t *data, size_t size);
int gba_state_load(gba_machine *m, const uint8_t *data, size_t size);

void gba_destroy(gba_machine *m);

#ifdef __cplusplus
}
#endif

#endif
