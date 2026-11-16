/* xbox_cheats -- an Xbox title's cheat codes (XBA-241): guest-memory writes applied once per vblank.
 *
 * The shared engine (src/platform/recomp_cheats.h, console RECOMP_CHEAT_XBOX, format "xbox") reads the game's cheat
 * file, RECOMP_CHEATS_FILE (the app sets it to files/settings/game/<TITLE ID>.cheats; a desktop run sets it by hand),
 * the same file format and the same Cheats screens as the GBA / PS1 / N64 titles. The lifted code cannot be patched,
 * so the codes are data writes: on every vblank (xbox_VblankHookSet's slot in kernel_bridge.c, on the kernel's timer
 * thread) the enabled codes run once against guest memory, like a cheat device's per-frame writes, and the file is
 * looked at again every 30 vblanks (the in-game menu's changes take effect within half a second).
 *
 * What a code may reach: guest addresses from 0x00010000 (the XBE base: not the kernel's low page structures) to the
 * top of the mapped RAM (xbox_GetMappedSize), and the lazy reservations above it (xbox_ReserveHolds). Writes into a
 * code section (executable and not writable, xbox_SmcInCode) are dropped: the lifted C never reads them back and
 * their pages may be write-protected. Anything else (the RAM mirrors, MMIO, the kernel's 0x80000000 view, unmapped
 * addresses) is refused: a write is dropped and a conditional on it fails; the first few refusals are logged.
 *
 * No vblank, no cheats: a bare runtime without RECOMP_VBLANK (a title project gets "paced" by default, XDK titles)
 * does not run them. POSIX hosts; on Windows the functions do nothing (the engine is part of the POSIX host core). */
#ifndef XBOXRECOMP_XBOX_CHEATS_H
#define XBOXRECOMP_XBOX_CHEATS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The kernel's vblank: loads RECOMP_CHEATS_FILE the first time, polls it every 30 vblanks, applies the codes. */
void xbox_CheatsVblank(void);

/* 1 when size bytes at guest va may be read (write 0) or written (write 1) by a code (the rules above). */
int xbox_CheatsGuestOk(uint32_t va, uint32_t size, int write);

/* Tests and tools: replace the list with a cheat file's text (returns the number of cheats), apply the enabled codes
 * once now (returns how many are active), drop the list. Not to be called while vblanks run. */
int xbox_CheatsLoadText(const char *text);
size_t xbox_CheatsApply(void);
void xbox_CheatsReset(void);
/* Accesses refused since start (unmapped, low page, code). */
unsigned xbox_CheatsRefused(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_XBOX_CHEATS_H */
