/* gc_exi.h -- the GameCube's external interface devices (XBA-264): memory cards in slots A and B, the IPL chip's
 * real-time clock and SRAM. The machine's EXI channels (0x0C006800, gc_exi.c) talk to:
 *
 *   channel 0 device 0  memory card slot A          channel 1 device 0  memory card slot B
 *   channel 0 device 1  IPL ROM (reads zeros: no IPL, no font), RTC, SRAM, UART (writes dropped)
 *   channel 0 device 2, channel 2: nothing (reads give zeros)
 *
 * A memory card is the flash chip the SDK's and libogc's CARD libraries drive with the standard command set (ID,
 * status, read array 0x52, page program 0xF2, sector erase 0xF1, chip erase 0xF4, interrupt enable, sleep / wake),
 * always unlocked (no DSP handshake), latency 4, 8 KB sectors. Sizes: 4 / 16 / 64 Mbit = 59 / 251 / 1019 blocks of
 * 8 KB for saves plus 5 system blocks (512 KB, 2 MB, 8 MB). Its contents live in memory; the host loads and writes
 * the file (gc_runtime: <save dir>/gc-<KEY>/cardA.raw, the raw image as Dolphin and real-card dumps have it).
 * A blank card is all 0xFF, as an erased flash chip: the game sees an unformatted card and offers to format it.
 *
 * The RTC counts seconds since 2000-01-01 00:00 local time (the host's clock by default); SRAM holds the settings
 * the IPL keeps: language, video mode (as the TV mode), stereo, with a valid checksum. A card's header (its serial,
 * made from the console's flash ID when formatted) sets that slot's flash ID in SRAM when it is inserted, so the SDK
 * takes a card formatted elsewhere as this console's. See consoles/README.md « GameCube ». */
#ifndef GC_EXI_H
#define GC_EXI_H

#include <stddef.h>
#include <stdint.h>

#include "gc_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Card sizes in Mbit (the size the card's ID reports). */
enum { GC_CARD_MBIT_59 = 4, GC_CARD_MBIT_251 = 16, GC_CARD_MBIT_1019 = 64 };
enum { GC_CARD_SLOT_A = 0, GC_CARD_SLOT_B = 1 };

/* Bytes of a card of mbit Mbit (mbit << 17), 0 for a size that is not 4 / 16 / 64. */
size_t gc_card_bytes(int mbit);
/* The card size in Mbit of a raw image of n bytes, 0 when n is no card's size. */
int gc_card_mbit_of_size(size_t n);
/* The card size in Mbit for a block count (59 / 251 / 1019, or 4 / 16 / 64 as is), 0 for anything else. */
int gc_card_mbit_of_blocks(long blocks);

/* Inserts a card of mbit Mbit into slot (GC_CARD_SLOT_*): image (gc_card_bytes(mbit) bytes, copied) or NULL for a
 * blank card. Replaces the card there. Returns 0, -1 on a bad size or slot. Inserting while the machine runs signals
 * the change (EXTINT), as a hand on the slot would. */
int gc_card_insert(gc_machine *m, int slot, int mbit, const uint8_t *image);
/* Takes the card out of slot (the game sees it go: EXTINT). */
void gc_card_remove(gc_machine *m, int slot);
/* The card's contents (n: its size), NULL when the slot is empty. Valid until the card is removed or replaced. */
const uint8_t *gc_card_data(const gc_machine *m, int slot, size_t *n);
/* A counter that grows with each erase / program that changed the card (0 until the first): the host writes the
 * file back when it changed since it last looked. */
uint64_t gc_card_writes(const gc_machine *m, int slot);

/* The RTC: seconds since 2000-01-01 00:00 (local time). gc_exi_set_rtc fixes it at `seconds` when the machine
 * starts (it then runs with emulated time: deterministic tests); a negative value goes back to the host's clock.
 * A game that sets the clock moves it by the difference either way. */
void gc_exi_set_rtc(gc_machine *m, int64_t seconds);
uint32_t gc_exi_rtc(gc_machine *m);
/* SRAM: the language (0 English, 1 German, 2 French, 3 Spanish, 4 Italian, 5 Dutch; the checksum is kept valid), and
 * the 64 bytes as they are (a copy). */
void gc_exi_set_language(gc_machine *m, int lang);
void gc_exi_sram(const gc_machine *m, uint8_t out[64]);

/* Save states (the states work, XBA-264): the EXI channels' registers and the devices' transfer state (the card
 * commands under way, the cards' status and interrupt bits, the IPL's command, the SRAM, the RTC's offset), not the
 * cards' contents: those are the save files, as in Dolphin. gc_exi_state_save writes at most cap bytes and returns
 * how many (0 when cap < gc_exi_state_size()); gc_exi_state_load returns 0, or -1 on a blob of another size or
 * version (nothing changed then). A card present in the state but not in the machine (or of another size) is left
 * as the machine has it. */
size_t gc_exi_state_size(void);
size_t gc_exi_state_save(const gc_machine *m, uint8_t *out, size_t cap);
int gc_exi_state_load(gc_machine *m, const uint8_t *in, size_t n);
/* What gc_exi_state_load checks before it changes anything (size, magic): 0, or -1. gc_state_check calls it, so a
 * damaged EXI section refuses the whole state instead of failing half way through a load. */
int gc_exi_state_check(const uint8_t *in, size_t n);

#ifdef __cplusplus
}
#endif

#endif
