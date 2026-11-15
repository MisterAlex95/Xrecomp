/* recomp_cheats -- cheat codes for the console runtimes (XBA-197): GBA, PS1 and N64.
 *
 * A game's cheat list lives in a text file (the app's files/settings/game/<KEY>.cheats, RECOMP_CHEATS_FILE for the
 * runtime, --cheats FILE for the desktop players), one section per cheat:
 *
 *     # comment
 *     [Infinite lives]
 *     enabled=1
 *     format=auto            (optional: auto, raw, gsa, gsa-raw, arv3, arv3-raw, cb, gs)
 *     8003A2C4 0063          (the code, one line per code line)
 *
 * Each cheat's lines are compiled into a small op list (writes with repeat, adds, ORs, ANDs, conditional jumps,
 * pointer writes, copies, GBA cartridge patches) once, when the file is read; the runtime runs the enabled lists
 * after every console frame through a bus it supplies (recomp_cheat_bus). Writes go through the console's memory
 * accessors and only when the value differs, so recompiled code reading RAM sees them, and a write that lands on
 * code the PS1 / N64 packs run from RAM bumps that page's write counter: the pack's content check drops the lifted
 * function and the interpreter runs the patched code. GBA cartridge patches (AR / GS ROM patch codes) change the
 * ROM bytes; the runtime detaches the GBA pack's lifted code while any is active (the interpreter runs the whole
 * cartridge then, recompute_needed in recomp_cheats_apply's result).
 *
 * Formats (docs: consoles/README.md « Cheats »):
 *  GBA  raw       AAAAAAAA:VV / :VVVV / :VVVVVVVV (or a space): constant write, its size from the value's digits
 *       gsa       GameShark / Action Replay v1-v2, encrypted (TEA, the v1 seeds); gsa-raw: the same decrypted
 *       arv3      Action Replay v3 / GameShark v3, encrypted (TEA, the v3 seeds); arv3-raw: decrypted
 *       cb        CodeBreaker / GameShark SP, unencrypted (AAAAAAAA VVVV)
 *  PS1  gs        GameShark / Action Replay (AAAAAAAA VVVV): 30 80 10 11 20 21 D0-D3 E0-E3 50 C0 C1 C2
 *  N64  gs        GameShark / Action Replay (AAAAAAAA VVVV): 80 81 A0 A1 88 89 D0-D3 50 F0 F1
 *  auto           GBA: raw when a line has ':', cb for 8+4 digit lines, else the encrypted or decrypted GSA / AR v3
 *                 reading whose lines decode to the most plausible codes; PS1 / N64: raw with ':', else gs.
 * Not supported (the cheat is refused with a reason): GBA DEADFACE reseeding, encrypted CodeBreaker (9xxxxxxx),
 * AR v3 slowdown; PS1 D4-D6 joker codes; N64 DE (entry point). Ignored with a note: master / hook / ID codes (GBA
 * Fxxxxxxx, C4xxxxxx, 0000xxxx / 1xxxxxxx, xxxxxxxx 001DC0DE), N64 enabler codes (EE, FF, CC).
 *
 * Timing: constant writes, adds and conditionals run after every console frame. GBA button codes (GS 8x, AR v3
 * 00000000 10/12/14) and N64 88 / 89 (the GameShark's button) and F0 / F1 (boot-time writes) apply once, on the
 * first frame after the cheat is turned on (or the game starts with it on). PS1 C1 delays the codes after it by
 * YYYY / 3 frames (about YYYY / 180 s) counted from the start.
 *
 * Save states and rewind keep working: cheats are not part of a state; after a load they apply again from the next
 * frame.
 *
 * libc only; not thread-safe (the runtime's frame thread owns its list). */
#ifndef XBR_RECOMP_CHEATS_H
#define XBR_RECOMP_CHEATS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum recomp_cheat_console {
    RECOMP_CHEAT_GBA = 1,
    RECOMP_CHEAT_PS1 = 2,
    RECOMP_CHEAT_N64 = 3,
} recomp_cheat_console;

typedef enum recomp_cheat_format {
    RECOMP_CF_AUTO = 0,
    RECOMP_CF_RAW,
    RECOMP_CF_GSA,      /* GBA GameShark / AR v1-v2, encrypted */
    RECOMP_CF_GSA_RAW,  /* the same decrypted */
    RECOMP_CF_ARV3,     /* GBA Action Replay v3 / GameShark v3, encrypted */
    RECOMP_CF_ARV3_RAW, /* the same decrypted */
    RECOMP_CF_CB,       /* GBA CodeBreaker, unencrypted */
    RECOMP_CF_GS,       /* PS1 / N64 GameShark */
    RECOMP_CF_COUNT
} recomp_cheat_format;

/* "auto", "raw", "gsa", "gsa-raw", "arv3", "arv3-raw", "cb", "gs"; parse returns -1 for an unknown name. */
const char *recomp_cheat_format_name(recomp_cheat_format f);
int recomp_cheat_format_parse(const char *name);

/* One compiled operation (see recomp_cheats.c). */
typedef struct recomp_cheat_op {
    uint8_t kind, size, cond, once; /* size in bytes: 1, 2, 4 */
    uint32_t addr, value;
    uint32_t count;                 /* writes: repeats (>= 1); copies: bytes; jumps: target index */
    uint32_t astep, vstep;          /* writes: address and value steps; pointer writes: astep = offset */
} recomp_cheat_op;

typedef struct recomp_cheat {
    char name[96];
    int enabled;
    recomp_cheat_format format;    /* as written in the file (AUTO: detected, see used) */
    recomp_cheat_format used;      /* the format the lines were read in */
    recomp_cheat_op *ops;
    size_t nops;
    uint8_t *data;                 /* CodeBreaker super code bytes */
    size_t ndata;
    char error[160];               /* "" when the code compiled; else why it is not applied */
    char note[160];                /* lines that were ignored (master / hook / enabler codes), "" when none */
    int ran;                       /* applied since it was turned on (its once-codes are done) */
} recomp_cheat;

/* What the runtime gives the engine: sized side-effect-free reads and writes that reach RAM (and on the GBA the I/O
 * registers), and on the GBA a cartridge patch. When write is NULL the engine writes words with read-modify-write
 * through read32 / write32 (aligned words, big_endian for the N64): the PS1 / N64 runtimes hand their peek32 /
 * poke32. keys: the console's buttons held this frame (GBA KEYINPUT order, 1 = held) for CodeBreaker D codes. */
typedef struct recomp_cheat_bus {
    void *user;
    uint32_t (*read)(void *user, uint32_t addr, int size);
    void (*write)(void *user, uint32_t addr, uint32_t v, int size);
    uint32_t (*read32)(void *user, uint32_t addr);
    void (*write32)(void *user, uint32_t addr, uint32_t v);
    int big_endian;
    int (*rom_write16)(void *user, uint32_t addr, uint16_t v); /* GBA: 0 on success */
    uint32_t keys;
} recomp_cheat_bus;

#define RECOMP_CHEATS_MAX 256
#define RECOMP_CHEAT_PATCHES_MAX 64

typedef struct recomp_cheat_patch {
    uint32_t addr;
    uint16_t value, old;
} recomp_cheat_patch;

typedef struct recomp_cheats {
    recomp_cheat_console console;
    recomp_cheat *list;
    size_t n;
    char path[1024];               /* the file watched by recomp_cheats_poll, "" for none */
    long long mtime_ns, size;      /* its stat when last read (-1: missing) */
    recomp_cheat_patch patch[RECOMP_CHEAT_PATCHES_MAX]; /* GBA cartridge patches in place */
    size_t npatch;
    uint64_t frames;               /* applies so far (PS1 C1 delays) */
} recomp_cheats;

void recomp_cheats_init(recomp_cheats *s, recomp_cheat_console console);
/* Frees the list (does not restore cartridge patches: recomp_cheats_restore first). */
void recomp_cheats_free(recomp_cheats *s);

/* Replaces the list with the cheats of a file's text (the format above). Returns the number of cheats read. */
int recomp_cheats_parse_text(recomp_cheats *s, const char *text);

/* Reads path (remembered for recomp_cheats_poll). Returns the number of cheats, 0 for a missing file, -1 when it
 * cannot be read. Prints one line per cheat with its state ("cheats: [on] Infinite lives (gs, 2 ops)"). */
int recomp_cheats_load(recomp_cheats *s, const char *path);
/* RECOMP_CHEATS_FILE: loads it when set. tag ("gba") prefixes the log. Returns recomp_cheats_load's value, or 0. */
int recomp_cheats_load_env(recomp_cheats *s, const char *tag);
/* Reads the file again when its size or time changed (the app saved a change): 1 when it did, else 0. The list's
 * once-codes of cheats that were on stay done; a cheat turned on applies them again. */
int recomp_cheats_poll(recomp_cheats *s);

/* Compiles one cheat's code text (lines) for a console in a format (AUTO detects). The result fills c->ops / data /
 * used / error / note (c->name, enabled kept). Returns 0, or -1 with c->error set. */
int recomp_cheat_compile(recomp_cheat *c, recomp_cheat_console console, recomp_cheat_format fmt, const char *code);
void recomp_cheat_clear(recomp_cheat *c);

/* Runs the enabled cheats once (after a console frame). Returns the number of GBA cartridge patches in place. */
size_t recomp_cheats_apply(recomp_cheats *s, const recomp_cheat_bus *bus);
/* Puts back the cartridge bytes the patches replaced. */
void recomp_cheats_restore(recomp_cheats *s, const recomp_cheat_bus *bus);
/* Cheats that are on and compiled. */
size_t recomp_cheats_active(const recomp_cheats *s);

/* ---- GBA code encryption (tests; the GSA / AR v3 TEA with 32 rounds) ------------------------------------------ */
extern const uint32_t recomp_cheat_seeds_gsa[4];  /* GameShark / AR v1-v2 */
extern const uint32_t recomp_cheat_seeds_arv3[4]; /* AR v3 / GS v3 */
void recomp_cheat_tea_decrypt(uint32_t *op1, uint32_t *op2, const uint32_t seeds[4]);
void recomp_cheat_tea_encrypt(uint32_t *op1, uint32_t *op2, const uint32_t seeds[4]);

#ifdef __cplusplus
}
#endif

#endif /* XBR_RECOMP_CHEATS_H */
