/*
 * xbox_mods.h - what the player's enabled mods ask of the Xbox runtime (docs/technical/mods.md)
 *
 * Prepare applies a mod's patches to a copy of the XBE and lifts that copy; it hands the rest to the runtime as
 * options the pack forces (pack.json env_force / src/title/env_force.h):
 *
 *   RECOMP_MOD_XBE          the patched copy: xbox_MemoryLayoutInit loads its headers and sections instead of the
 *                           disc's, so a patch's data changes are there too (its code changes came with the lift)
 *   RECOMP_MOD_FRAMES       "anamorphic-16:9": the mod makes the game draw its 3D scenes squeezed for a 16:9
 *                           screen. Presented wide (xbox_FramebufferWindowSetWide) when RECOMP_ASPECT says so:
 *                           16:9 always, 4:3 never, auto on Android only (a 16:9 handheld; a desktop window keeps
 *                           the console's shape)
 *   RECOMP_MOD_WIDE_WRITES  "<va hex>=<bytes hex>;...": guest data written once the XBE is loaded, only when the
 *                           frames are presented wide (a mod's switch between its 16:9 and 4:3 pictures)
 *   RECOMP_MOD_SET          the mod set, in a save state's identity (xbox_state.h)
 *   RECOMP_MOD_FOR          "<size>-<CRC32>" of the disc's XBE the mods were made for: any other XBE (another image
 *                           of the disc, which shares the pack's options) gets none of the above
 */
#ifndef XBOX_MODS_H
#define XBOX_MODS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_MODS_WRITES_MAX 8
#define XBOX_MODS_WRITE_BYTES 64

typedef struct XboxModWrite {
    uint32_t va;
    uint32_t n;
    uint8_t bytes[XBOX_MODS_WRITE_BYTES];
} XboxModWrite;

/* 1 when frames of this kind (RECOMP_MOD_FRAMES) are presented wide under this RECOMP_ASPECT, on Android
 * (android 1) or a desktop (0). Pure. */
int xbox_ModsFramesWide(const char *frames, const char *aspect, int android);

/* RECOMP_MOD_WIDE_WRITES' format into w (max entries): the count, or -1 when s is malformed (nothing is used
 * then). NULL or "" is 0. Pure. */
int xbox_ModsParseWrites(const char *s, XboxModWrite *w, int max);

/* CRC32 (the zlib / IEEE one) of n bytes. */
uint32_t xbox_ModsCrc32(const uint8_t *b, size_t n);

/* 1 when the mods are for this XBE: mod_for (RECOMP_MOD_FOR) is unset, empty or "<n>-<CRC32 upper-case hex>" of it. */
int xbox_ModsForThis(const uint8_t *xbe, size_t n, const char *mod_for);

/* The XBE to load: RECOMP_MOD_XBE's file when set, readable and an XBE of the same title as xbe (its bytes, kept
 * for the whole run; *n set to its size), else xbe itself; nothing when RECOMP_MOD_FOR names another XBE (then
 * xbox_ModsAfterLoad does nothing either). Logs which. */
const uint8_t *xbox_ModsXbe(const uint8_t *xbe, size_t *n);

/* Once the XBE's headers and sections are in guest memory: the frames' presentation and the wide writes (each
 * must lie inside the loaded image). Returns 1 when the frames are presented wide. */
int xbox_ModsAfterLoad(void);

#ifdef __cplusplus
}
#endif

#endif
