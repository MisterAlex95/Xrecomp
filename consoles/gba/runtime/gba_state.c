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
#include <stdio.h>
#include <stdlib.h>
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

/* Cheats patch the cartridge (gba_rom_patch16): the game's hash is the unpatched ROM's. */
void gba_state_pin_rom_hash(gba_machine *m)
{
    (void)rom_hash(m);
}

/* The BIOS: 0 for the built-in one (its code is ours, rebuilt at creation), else the image's hash. */
static uint64_t bios_hash(gba_machine *m)
{
    return m->builtin_bios ? 0 : fnv1a(m->bios, GBA_BIOS_SIZE);
}

size_t gba_state_size(gba_machine *m)
{
    (void)m;
    return HEADER_BYTES + 8 + GBA_MACHINE_SAVED;
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
    put32(s + 4, (uint32_t)GBA_MACHINE_SAVED);
    uint8_t *c = s + 8;
    memcpy(c, m, GBA_MACHINE_SAVED); /* the host-only tail (blocks, ...) is not the machine's */
    /* Host pointers and host-side flags: not the machine's (the blob may not be aligned: by offset). */
#define CLEAR(f) do { size_t n_ = sizeof m->f; memset(c + offsetof(gba_machine, f), 0, n_); } while (0)
    CLEAR(rom);
    CLEAR(rc_ewram);
    CLEAR(rc_iwram);
    CLEAR(rc);
    CLEAR(fb);
    CLEAR(rom_hash);
    CLEAR(save.dirty);
    CLEAR(dbg_cb);
    CLEAR(dbg_ctx);
#undef CLEAR
    return need;
}

/* The ROM and BIOS hashes say which game a state is for, not that its bytes are sane: a crafted (or damaged) state
 * passes them with any value in any field. These are the saved fields the core later uses without masking them, as
 * an index or an offset (a host out-of-bounds access), a shift count or a loop step (undefined behaviour, a loop that
 * never ends); everything else is masked at each use. Each range is what the core itself ever writes. t: the
 * state's machine bytes, copied (not the live machine). */
static bool bool_ok(const bool *b)
{
    uint8_t v;
    memcpy(&v, b, 1); /* a bool holding another byte is undefined to read as a bool */
    return v <= 1;
}

static bool machine_ok(const gba_machine *t)
{
    const uint64_t frame = GBA_CYCLES_PER_FRAME, never = UINT64_MAX;
    const gba_save *s = &t->save;
    const gba_apu *a = &t->apu;

    /* The CPU: the flags index a shift (cond_lut[cc] >> nzcv), the bools are bytes. */
    if ((t->cpu.n | t->cpu.z | t->cpu.c | t->cpu.v) > 1 || !bool_ok(&t->cpu.branched) || !bool_ok(&t->cpu.halted) ||
        !bool_ok(&t->cpu.stopped) || !bool_ok(&t->prefetch) || !bool_ok(&t->irq_line) || !bool_ok(&t->frame_done) ||
        !bool_ok(&t->pf.valid) || !bool_ok(&t->pf.run) || !bool_ok(&t->pf.thumb) || !bool_ok(&t->dma_active) ||
        !bool_ok(&t->dbg_open))
        return false;
    /* Wait states: a zero cost lets the CPU loop run without time passing. */
    for (int r = 0; r < 16; r++)
        if (!t->wait16[r][0] || !t->wait16[r][1] || !t->wait32[r][0] || !t->wait32[r][1] || !t->fetch16[r] ||
            !t->fetch32[r] || !t->fetchmax16[r] || !t->fetchmax32[r])
            return false;
    /* The video line: phase_offset[line_phase]; vcount past the last line never wraps (no VBlank, no frame end). */
    if (t->line_phase < 0 || t->line_phase > 2 || t->vcount >= GBA_LINES)
        return false;
    /* Times: one far in the past makes the catch-up loops run for ages, the next event far ahead stalls the frame. */
    if (t->line_start > t->now || t->now - t->line_start >= frame || t->next_event > t->now + frame ||
        (t->irq_at != never && t->irq_at + frame < t->now) || (t->dma_due != never && t->dma_due + frame < t->now) ||
        (t->sio_at != never && t->sio_at + frame < t->now) || a->time + frame < t->now || a->time > t->now + frame)
        return false;
    for (int i = 0; i < 4; i++) {
        const gba_timer *tm = &t->timer[i];
        /* The prescaler: a shift count (1u << shift, counter << shift); one of timer_shift[] (gba_io.c). */
        if (tm->shift != 0 && tm->shift != 6 && tm->shift != 8 && tm->shift != 10)
            return false;
        if ((tm->overflow_at != never && tm->overflow_at + frame < t->now) || tm->base_time > t->now + frame)
            return false;
        const gba_dma *d = &t->dma[i];
        if (d->count > 0x10000 || !bool_ok(&d->active_special) || !bool_ok(&d->due))
            return false;
    }
    /* The save chip: flash_bank offsets data[] (data[flash_bank + 0xFFFF]); the EEPROM's address offsets it too
     * (eeprom_addr * 64 bits) and is at most abits wide, abits following the kind (set_kind, gba_eeprom_dma_hint). */
    if ((unsigned)s->kind > GBA_SAVE_EEPROM8K)
        return false;
    if (s->flash_bank != 0 && !(s->flash_bank == 0x10000 && s->kind == GBA_SAVE_FLASH128))
        return false;
    if (s->eeprom_abits != (s->kind == GBA_SAVE_EEPROM8K ? 14 : 6) || s->eeprom_addr >= 1u << s->eeprom_abits ||
        s->eeprom_nbits < 0 || s->eeprom_nbits > 2 + s->eeprom_abits + 64 || s->eeprom_cmd < 0 ||
        s->eeprom_cmd > 3 || s->eeprom_read_left < 0 || s->eeprom_read_left > 68)
        return false;
    if (!bool_ok(&s->flash_id) || !bool_ok(&s->flash_erase) || !bool_ok(&s->flash_program) ||
        !bool_ok(&s->flash_bank_sel))
        return false;
    /* The APU: duty_masks[duty], pat >> pos, shadow >> sw_shift, wave_ram[bank from pos], fifo[f][fifo_r]; a period
     * of 0 or less never ends the channel's step loop, a mixing position past the window or a frame-sequencer count
     * of 0 or less never reaches its wrap, out_rem 0 never ends the resampler's loop. */
    for (int i = 0; i < 2; i++) {
        const gba_psg_square *q = &a->sq[i];
        if (q->duty < 0 || q->duty > 3 || q->pos < 0 || q->pos > 7 || q->sw_shift < 0 || q->sw_shift > 7 ||
            q->freq > 2047 || (q->on && q->period < 1))
            return false;
        if (!bool_ok(&q->on) || !bool_ok(&q->env_up) || !bool_ok(&q->len_en) || !bool_ok(&q->sw_down) ||
            !bool_ok(&q->sw_en))
            return false;
        if (a->fifo_r[i] < 0 || a->fifo_r[i] > 31 || a->fifo_n[i] < 0 || a->fifo_n[i] > 32)
            return false;
    }
    if (a->wave.pos < 0 || a->wave.pos > 63 || (a->wave.on && a->wave.period < 1) || a->noise.period < 0 ||
        !bool_ok(&a->wave.on) || !bool_ok(&a->wave.len_en) || !bool_ok(&a->noise.on) || !bool_ok(&a->noise.env_up) ||
        !bool_ok(&a->noise.len_en) || !bool_ok(&a->noise.out))
        return false;
    if (a->win_pos < 0 || a->win_pos >= GBA_APU_WINDOW || a->fs_windows < 1 || a->fs_windows > 32768 / GBA_APU_WINDOW ||
        a->out_rem < 1 || a->out_rem > (1u << 24) || a->ring_n > GBA_AUDIO_RING)
        return false;
    return true;
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
    if (rd32(data + HEADER_BYTES + 4) != GBA_MACHINE_SAVED)
        return GBA_STATE_EVERSION;
    if (size < HEADER_BYTES + 8 + GBA_MACHINE_SAVED)
        return GBA_STATE_ECORRUPT;
    /* The fields, on a copy: the blob may not be aligned, and a refused state leaves the machine as it was. */
    gba_machine *t = malloc(sizeof *t);
    if (!t)
        return GBA_STATE_ECORRUPT;
    memcpy(t, data + HEADER_BYTES + 8, GBA_MACHINE_SAVED);
    bool ok = machine_ok(t);
    free(t);
    return ok ? GBA_STATE_OK : GBA_STATE_ECORRUPT;
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
    gba_debug_log_fn dbg_cb = m->dbg_cb;
    void *dbg_ctx = m->dbg_ctx;
    memcpy(m, data + HEADER_BYTES + 8, GBA_MACHINE_SAVED); /* the host-only tail stays */
    m->rom = rom;
    m->rc_ewram = ewram_p;
    m->rc_iwram = iwram_p;
    m->rc = rcs;
    m->fb = fb;
    m->rom_size = rom_size;
    m->rom_hash = rh;
    m->builtin_bios = builtin;
    m->dbg_cb = dbg_cb;
    m->dbg_ctx = dbg_ctx;
    m->save.dirty = false; /* the state's save memory reaches storage only once the game writes it again */
    gba_recomp_state_loaded(m);
    gba_cpu_flush_blocks(m);
    m->blk_attn++;
    return GBA_STATE_OK;
}

/* ---- whole-machine comparison (gba_run --lockstep-ref, XBA-208) ---- */

#define FIELD(f) {#f, offsetof(gba_machine, f), sizeof(((gba_machine *)0)->f)}
static const struct {
    const char *name;
    size_t off, size;
} fields[] = {
    FIELD(cpu.r), FIELD(cpu.n), FIELD(cpu.z), FIELD(cpu.c), FIELD(cpu.v), FIELD(cpu.cpsr_ctl), FIELD(cpu.bank_r13),
    FIELD(cpu.bank_r14), FIELD(cpu.spsr), FIELD(cpu.fiq_r8_12), FIELD(cpu.usr_r8_12), FIELD(cpu.branched),
    FIELD(cpu.halted), FIELD(cpu.stopped), FIELD(now), FIELD(next_event), FIELD(wait16), FIELD(wait32),
    FIELD(fetch16), FIELD(fetch32), FIELD(prefetch), FIELD(irq_line), FIELD(rc_exit), FIELD(rc_hint),
    FIELD(open_bus), FIELD(rc_native), FIELD(fetchmax16), FIELD(fetchmax32), FIELD(pf), FIELD(line_start),
    FIELD(line_phase), FIELD(vcount), FIELD(frames), FIELD(frame_done), FIELD(ie), FIELD(if_), FIELD(ime),
    FIELD(irq_at), FIELD(keys), FIELD(keycnt), FIELD(timer), FIELD(dma), FIELD(dma_latch), FIELD(dma_active),
    FIELD(dma_due), FIELD(sio_at), FIELD(bg_delay), FIELD(win_latch), FIELD(oam_line), FIELD(bg2x), FIELD(bios_latch),
    FIELD(ewram), FIELD(iwram), FIELD(io), FIELD(pal), FIELD(vram), FIELD(oam), FIELD(apu), FIELD(save),
    FIELD(dbg_open), FIELD(dbg_str),
};
#undef FIELD

int gba_state_compare(gba_machine *a, gba_machine *b, char *why, size_t why_n)
{
    size_t n = gba_state_size(a);
    uint8_t *x = malloc(n), *y = malloc(n);
    int d = 0;
    if (!x || !y || gba_state_save(a, x, n) != n || gba_state_save(b, y, n) != n) {
        snprintf(why, why_n, "cannot save the states");
        d = 1;
    } else {
        for (size_t i = HEADER_BYTES + 8; i < n; i++) {
            if (x[i] == y[i])
                continue;
            size_t off = i - HEADER_BYTES - 8, base = 0;
            const char *name = "?";
            for (size_t k = 0; k < sizeof fields / sizeof fields[0]; k++)
                if (off >= fields[k].off && off < fields[k].off + fields[k].size) {
                    name = fields[k].name;
                    base = fields[k].off;
                }
            snprintf(why, why_n, "machine byte 0x%zX (%s + 0x%zX): 0x%02X vs 0x%02X", off, name, off - base, x[i],
                     y[i]);
            d = 1;
            break;
        }
    }
    free(x);
    free(y);
    return d;
}
