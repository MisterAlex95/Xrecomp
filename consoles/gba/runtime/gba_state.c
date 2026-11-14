/* gba_state.c -- save states of the GBA machine (gba_core.h « Save states », XBA-182).
 *
 * The blob: a header (magic "XRSTATE", console "gba", GBA_STATE_VERSION, the ROM's and the BIOS's FNV-1a 64
 * hashes, the section count) and sections (id, size, bytes). One section today: the whole gba_machine (CPU, IO,
 * timers, DMA, PPU registers and memories, APU, the save chip and its contents) with its host pointers cleared.
 * Its size is checked on load, so a machine whose layout changed (another build) refuses the state; a change that
 * keeps the size and changes the meaning bumps GBA_STATE_VERSION. Host byte order: a state is for the host family
 * that wrote it (every host we build is little-endian, LP64 or ILP32 with its own sizes, so the size check catches a
 * state carried across).
 *
 * What is not state: the ROM and the BIOS image (identified by hash: another game or BIOS is refused), the attached
 * recompiled code (its dispatch is a function of the ROM, which never changes; the hand-offs in flight are reset
 * by gba_recomp_state_loaded), the framebuffer pointer of a running frame (states are taken between frames). */
#include "gba_internal.h"

#include <stddef.h>
#include <string.h>

#define STATE_MAGIC "XRSTATE\0"
#define HEADER_BYTES 40u
#define SEC_MACHINE 1u

static void put32(uint8_t *p, uint32_t v)
{
    wr32(p, v);
}
static void put64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}
static uint64_t get64(const uint8_t *p)
{
    return rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

/* The game: the ROM's hash, computed once (gba_create). */
static uint64_t rom_hash(gba_machine *m)
{
    if (!m->rom_hash)
        m->rom_hash = fnv1a(m->rom, m->rom_size) | 1;
    return m->rom_hash;
}

/* The BIOS: 0 for the built-in one (its code is ours, rebuilt at creation), else the image's hash. */
static uint64_t bios_hash(gba_machine *m)
{
    return m->builtin_bios ? 0 : fnv1a(m->bios, GBA_BIOS_SIZE);
}

size_t gba_state_size(gba_machine *m)
{
    (void)m;
    return HEADER_BYTES + 8 + sizeof(gba_machine);
}

size_t gba_state_save(gba_machine *m, uint8_t *out, size_t cap)
{
    size_t need = gba_state_size(m);
    if (!m || !out || cap < need)
        return 0;
    memset(out, 0, HEADER_BYTES);
    memcpy(out, STATE_MAGIC, 8);
    memcpy(out + 8, "gba", 4);
    put32(out + 12, GBA_STATE_VERSION);
    put64(out + 16, rom_hash(m));
    put64(out + 24, bios_hash(m));
    put32(out + 32, 1); /* sections */
    put32(out + 36, 0);
    uint8_t *s = out + HEADER_BYTES;
    put32(s, SEC_MACHINE);
    put32(s + 4, (uint32_t)sizeof(gba_machine));
    uint8_t *c = s + 8;
    memcpy(c, m, sizeof *m);
    /* Host pointers and host-side flags: not the machine's (the blob may not be aligned: by offset). */
#define CLEAR(f) do { size_t n_ = sizeof m->f; memset(c + offsetof(gba_machine, f), 0, n_); } while (0)
    CLEAR(rom);
    CLEAR(rc_ewram);
    CLEAR(rc_iwram);
    CLEAR(rc);
    CLEAR(fb);
    CLEAR(rom_hash);
    CLEAR(save.dirty);
#undef CLEAR
    return need;
}

int gba_state_check(gba_machine *m, const uint8_t *data, size_t size)
{
    if (!m || !data || size < HEADER_BYTES || memcmp(data, STATE_MAGIC, 8) != 0 || memcmp(data + 8, "gba", 4) != 0)
        return GBA_STATE_ECORRUPT;
    if (rd32(data + 12) != GBA_STATE_VERSION)
        return GBA_STATE_EVERSION;
    if (get64(data + 16) != rom_hash(m) || get64(data + 24) != bios_hash(m))
        return GBA_STATE_EGAME;
    if (rd32(data + 32) != 1 || size < HEADER_BYTES + 8 || rd32(data + HEADER_BYTES) != SEC_MACHINE)
        return GBA_STATE_ECORRUPT;
    if (rd32(data + HEADER_BYTES + 4) != sizeof(gba_machine))
        return GBA_STATE_EVERSION;
    if (size < HEADER_BYTES + 8 + sizeof(gba_machine))
        return GBA_STATE_ECORRUPT;
    return GBA_STATE_OK;
}

int gba_state_load(gba_machine *m, const uint8_t *data, size_t size)
{
    int rc = gba_state_check(m, data, size);
    if (rc != GBA_STATE_OK)
        return rc;
    /* What stays this machine's: the ROM, the BIOS image (same hash, kept anyway), the host pointers. */
    uint8_t *rom = m->rom, *ewram_p = m->rc_ewram, *iwram_p = m->rc_iwram;
    struct gba_rc_state *rcs = m->rc;
    uint16_t *fb = m->fb;
    uint32_t rom_size = m->rom_size;
    uint64_t rh = m->rom_hash;
    bool builtin = m->builtin_bios;
    memcpy(m, data + HEADER_BYTES + 8, sizeof *m);
    m->rom = rom;
    m->rc_ewram = ewram_p;
    m->rc_iwram = iwram_p;
    m->rc = rcs;
    m->fb = fb;
    m->rom_size = rom_size;
    m->rom_hash = rh;
    m->builtin_bios = builtin;
    m->save.dirty = false; /* the state's save memory reaches storage only once the game writes it again */
    gba_recomp_state_loaded(m);
    return GBA_STATE_OK;
}
