/* ps1_state.c -- save states of the PS1 machine (ps1_core.h « Save states », XBA-182).
 *
 * The blob: a header (magic "XRSTATE", console "ps1", PS1_STATE_VERSION, the game's and the BIOS's FNV-1a 64 hashes,
 * the section count), then sections (id, size, bytes), each checked against this build's size before anything is
 * changed, so a state from another build (another layout) is refused whole. Host byte order and sizes, like the
 * GBA's (gba_state.c): a state belongs to the build family that wrote it.
 *
 * Not state: the BIOS image and the EXE given (identified by hash), the disc image (the drive keeps the disc it has;
 * its identity is in the header), the RAM page write counters of the recompiled code (every page is bumped on load,
 * so each lifted function is hashed against RAM again before it runs), the attached code's dispatch cache, host
 * pointers (devices, buffers, the tty sink). */
#include "ps1_hle.h"
#include "ps1_internal.h"

#include <stdlib.h>
#include <string.h>

#define STATE_MAGIC "XRSTATE\0"
#define HEADER_BYTES 40u

enum { SEC_MACHINE = 1, SEC_RAM, SEC_GPU, SEC_CDROM, SEC_SPU, SEC_MDEC, SEC_HLE, SEC_RC, SEC_COUNT = SEC_RC };

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}

static uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}
#define FNV0 0xCBF29CE484222325ull

/* The game: the EXE fast-booted (its bytes), else the disc in the drive (its size and sector 16, the ISO 9660 volume
 * descriptor: the game's own disc, whichever image format holds it), else nothing (0). */
static uint64_t game_hash(ps1_machine *m)
{
    uint64_t h = FNV0;
    if (m->exe)
        h = fnv1a(h, m->exe, m->exe_size);
    if (m->disc) {
        uint8_t sec[2352];
        uint32_t size = ps1_disc_size(m->disc);
        h = fnv1a(h, (const uint8_t *)&size, sizeof size);
        if (ps1_disc_read_sector(m->disc, 16, sec) == 0)
            h = fnv1a(h, sec + 24, 2048);
    }
    return m->exe || m->disc ? h : 0;
}

static uint64_t bios_hash(ps1_machine *m)
{
    return m->hle ? 0 : fnv1a(FNV0, m->bios, PS1_BIOS_SIZE);
}

/* Section sizes as this build writes them (the MDEC's varies with its output queue). */
static size_t sec_size(ps1_machine *m, int id)
{
    switch (id) {
    case SEC_MACHINE: return PS1_MACHINE_SAVED;
    case SEC_RAM: return PS1_RAM_SIZE;
    case SEC_GPU: return m->gpu ? ps1_gpu_state_size(m->gpu) : 0;
    case SEC_CDROM: return m->cdrom ? ps1_cdrom_state_size(m->cdrom) : 0;
    case SEC_SPU: return m->spu ? ps1_spu_state_size(m->spu) : 0;
    case SEC_MDEC: return m->mdec ? ps1_mdec_state_size(m->mdec) : 0;
    case SEC_HLE: return ps1_hle_state_size(m);
    case SEC_RC: return 4;
    default: return 0;
    }
}

size_t ps1_state_size(ps1_machine *m)
{
    size_t n = HEADER_BYTES;
    for (int id = 1; id <= SEC_COUNT; id++)
        n += 8 + sec_size(m, id);
    return n;
}

static void save_machine(ps1_machine *m, uint8_t *out)
{
    memcpy(out, m, PS1_MACHINE_SAVED);
    /* Host pointers and host-side flags: not the machine's (the blob may not be aligned: by offset). */
#define CLEAR(f) do { size_t n_ = sizeof m->f; memset(out + offsetof(ps1_machine, f), 0, n_); } while (0)
    CLEAR(ram);
    CLEAR(rc_scratch);
    CLEAR(ram_gen);
    CLEAR(rc);
    CLEAR(bios);
    CLEAR(gpu);
    CLEAR(cdrom);
    CLEAR(spu);
    CLEAR(mdec);
    CLEAR(disc);
    CLEAR(exe);
    CLEAR(exe_size);
    CLEAR(tty);
    CLEAR(tty_ctx);
    CLEAR(hle);
    CLEAR(cards[0].dirty);
    CLEAR(cards[1].dirty);
#undef CLEAR
}

size_t ps1_state_save(ps1_machine *m, uint8_t *out, size_t cap)
{
    if (!m || !out || ps1_hle_state_busy(m))
        return 0;
    size_t need = ps1_state_size(m);
    if (cap < need)
        return 0;
    memset(out, 0, HEADER_BYTES);
    memcpy(out, STATE_MAGIC, 8);
    memcpy(out + 8, "ps1", 4);
    put32(out + 12, PS1_STATE_VERSION);
    put64(out + 16, game_hash(m));
    put64(out + 24, bios_hash(m));
    put32(out + 32, SEC_COUNT);
    uint8_t *p = out + HEADER_BYTES;
    for (int id = 1; id <= SEC_COUNT; id++) {
        size_t n = sec_size(m, id);
        put32(p, (uint32_t)id);
        put32(p + 4, (uint32_t)n);
        p += 8;
        switch (id) {
        case SEC_MACHINE: save_machine(m, p); break;
        case SEC_RAM: memcpy(p, m->ram, PS1_RAM_SIZE); break;
        case SEC_GPU: if (n) ps1_gpu_state_save(m->gpu, p); break;
        case SEC_CDROM: if (n) ps1_cdrom_state_save(m->cdrom, p); break;
        case SEC_SPU: if (n) ps1_spu_state_save(m->spu, p); break;
        case SEC_MDEC: if (n) ps1_mdec_state_save(m->mdec, p); break;
        case SEC_HLE: ps1_hle_state_save(m, p); break;
        case SEC_RC: put32(p, (uint32_t)ps1_recomp_booted(m)); break;
        }
        p += n;
    }
    return need;
}

/* Walks the sections: each id once, in order, sizes this build's (the MDEC's checked by its own load). Fills at[]
 * with each section's bytes. */
static int parse(ps1_machine *m, const uint8_t *data, size_t size, const uint8_t *at[], size_t len[])
{
    if (!m || !data || size < HEADER_BYTES || memcmp(data, STATE_MAGIC, 8) != 0 || memcmp(data + 8, "ps1", 4) != 0)
        return PS1_STATE_ECORRUPT;
    if (get32(data + 12) != PS1_STATE_VERSION)
        return PS1_STATE_EVERSION;
    if (get64(data + 16) != game_hash(m) || get64(data + 24) != bios_hash(m))
        return PS1_STATE_EGAME;
    if (get32(data + 32) != SEC_COUNT)
        return PS1_STATE_EVERSION;
    size_t off = HEADER_BYTES;
    for (int id = 1; id <= SEC_COUNT; id++) {
        if (size - off < 8)
            return PS1_STATE_ECORRUPT;
        uint32_t sid = get32(data + off), n = get32(data + off + 4);
        off += 8;
        if (sid != (uint32_t)id || n > size - off)
            return PS1_STATE_ECORRUPT;
        if (id != SEC_MDEC && n != sec_size(m, id))
            return PS1_STATE_EVERSION;
        at[id] = data + off;
        len[id] = n;
        off += n;
    }
    return PS1_STATE_OK;
}

int ps1_state_check(ps1_machine *m, const uint8_t *data, size_t size)
{
    const uint8_t *at[SEC_COUNT + 1];
    size_t len[SEC_COUNT + 1];
    return parse(m, data, size, at, len);
}

int ps1_state_load(ps1_machine *m, const uint8_t *data, size_t size)
{
    const uint8_t *at[SEC_COUNT + 1];
    size_t len[SEC_COUNT + 1];
    int rc = parse(m, data, size, at, len);
    if (rc != PS1_STATE_OK)
        return rc;
    /* The MDEC first: the only section whose size its own load checks, so a refusal leaves everything as it was. */
    if (m->mdec ? ps1_mdec_state_load(m->mdec, at[SEC_MDEC], len[SEC_MDEC]) != 0 : len[SEC_MDEC] != 0)
        return PS1_STATE_EVERSION;
    if ((m->gpu && ps1_gpu_state_load(m->gpu, at[SEC_GPU], len[SEC_GPU]) != 0) ||
        (m->cdrom && ps1_cdrom_state_load(m->cdrom, at[SEC_CDROM], len[SEC_CDROM]) != 0) ||
        (m->spu && ps1_spu_state_load(m->spu, at[SEC_SPU], len[SEC_SPU]) != 0) ||
        ps1_hle_state_load(m, at[SEC_HLE], len[SEC_HLE]) != 0)
        return PS1_STATE_EVERSION; /* not reached: the sizes were checked */

    /* The machine, keeping this one's host side. */
    uint8_t *ram = m->ram, *rc_scratch = m->rc_scratch, *bios = m->bios, *exe = m->exe;
    uint32_t *ram_gen = m->ram_gen;
    struct ps1_rc_state *rcs = m->rc;
    ps1_gpu *gpu = m->gpu;
    ps1_cdrom *cdrom = m->cdrom;
    ps1_spu *spu = m->spu;
    ps1_mdec *mdec = m->mdec;
    ps1_disc *disc = m->disc;
    size_t exe_size = m->exe_size;
    void (*tty)(void *, char) = m->tty;
    void *tty_ctx = m->tty_ctx;
    struct ps1_hle *hle = m->hle;
    memcpy(m, at[SEC_MACHINE], PS1_MACHINE_SAVED); /* the host-only tail (blocks, interp_ref) stays */
    m->ram = ram;
    m->rc_scratch = rc_scratch;
    m->ram_gen = ram_gen;
    m->rc = rcs;
    m->bios = bios;
    m->gpu = gpu;
    m->cdrom = cdrom;
    m->spu = spu;
    m->mdec = mdec;
    m->disc = disc;
    m->exe = exe;
    m->exe_size = exe_size;
    m->tty = tty;
    m->tty_ctx = tty_ctx;
    m->hle = hle;
    m->cards[0].dirty = m->cards[1].dirty = false;
    memcpy(m->ram, at[SEC_RAM], PS1_RAM_SIZE);
    for (uint32_t i = 0; i < PS1_RAM_PAGES; i++)
        m->ram_gen[i]++; /* new contents everywhere, as far as lifted code is concerned */
    ps1_recomp_state_loaded(m, (int32_t)get32(at[SEC_RC]));
    return PS1_STATE_OK;
}
