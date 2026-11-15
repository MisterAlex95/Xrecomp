/* gba_cheats.h -- the GBA's side of the cheat engine (XBA-197, src/platform/recomp_cheats.h), shared by
 * gba_runtime.c and the headless gba_run: the bus (gba_peek* / gba_cheat_poke / gba_rom_patch16) and the pack's
 * lifted code put aside while a cartridge patch is in place.
 *
 * Why the pack goes aside: lifted code is the ROM's instructions compiled to C (and may fold the ROM's literal pool
 * into constants), so it would not see patched cartridge words. While any AR / GS ROM patch code is on, the table is
 * detached and the interpreter runs the whole cartridge (it reads the patched bytes); once none is, the table comes
 * back. Patches of RAM need nothing: lifted code reads RAM through the bus, and code run from RAM is interpreted. */
#ifndef GBA_CHEATS_H
#define GBA_CHEATS_H

#include "gba_core.h"
#include "gba_recomp.h"
#include "recomp_cheats.h"

#include <stdio.h>

typedef struct gba_cheats {
    recomp_cheats list;
    gba_machine *m;
    const gba_recomp_table *table; /* the pack's, NULL without one */
    int detached;                  /* the table is aside for a cartridge patch */
    unsigned polls;
} gba_cheats;

static inline uint32_t gba_cheats_read(void *user, uint32_t addr, int size)
{
    gba_machine *m = user;
    return size == 1 ? gba_peek8(m, addr) : size == 2 ? gba_peek16(m, addr) : gba_peek32(m, addr);
}

static inline void gba_cheats_write(void *user, uint32_t addr, uint32_t v, int size)
{
    gba_cheat_poke((gba_machine *)user, addr, v, size);
}

static inline int gba_cheats_rom(void *user, uint32_t addr, uint16_t v)
{
    return gba_rom_patch16((gba_machine *)user, addr, v);
}

/* RECOMP_CHEATS_FILE (or path when not NULL). table: the attached pack's, or NULL. */
static inline void gba_cheats_open(gba_cheats *c, gba_machine *m, const gba_recomp_table *table, const char *path)
{
    recomp_cheats_init(&c->list, RECOMP_CHEAT_GBA);
    c->m = m;
    c->table = table;
    c->detached = 0;
    c->polls = 0;
    if (path)
        recomp_cheats_load(&c->list, path);
    else
        recomp_cheats_load_env(&c->list, "gba");
}

/* After a GBA frame, keys: the buttons held (gba_set_keys' mask). */
static inline void gba_cheats_frame(gba_cheats *c, uint16_t keys)
{
    if (!c->list.n && !c->list.npatch)
        return;
    recomp_cheat_bus bus = {c->m, gba_cheats_read, gba_cheats_write, NULL, NULL, 0, gba_cheats_rom, keys};
    size_t patches = recomp_cheats_apply(&c->list, &bus);
    if (c->table && patches && !c->detached) {
        gba_attach_recomp(c->m, NULL);
        c->detached = 1;
        printf("gba: cheats patch the cartridge: recompiled code set aside, the interpreter runs the game\n");
        fflush(stdout);
    } else if (c->table && !patches && c->detached) {
        gba_attach_recomp(c->m, c->table);
        c->detached = 0;
        printf("gba: no cartridge patch any more: recompiled code back\n");
        fflush(stdout);
    }
}

/* Once per shown frame: every 30th reads the file again when the app changed it. */
static inline void gba_cheats_poll(gba_cheats *c)
{
    if (++c->polls % 30 == 0)
        recomp_cheats_poll(&c->list);
}

static inline void gba_cheats_close(gba_cheats *c)
{
    recomp_cheat_bus bus = {c->m, gba_cheats_read, gba_cheats_write, NULL, NULL, 0, gba_cheats_rom, 0};
    recomp_cheats_restore(&c->list, &bus);
    recomp_cheats_free(&c->list);
}

#endif /* GBA_CHEATS_H */
