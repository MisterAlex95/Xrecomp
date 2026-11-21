/* n64_state.c -- save states of the N64 machine (n64_core.h « Save states », XBA-183).
 *
 * The blob: a header (magic "XRSTATE", console "n64", N64_STATE_VERSION, the ROM's hash, RDRAM's size, the section
 * count), then sections (id, size, bytes) in order, each checked against this build before anything changes, so a
 * state from another build (another layout) or another RDRAM size is refused whole. Host byte order and sizes, like
 * the GBA's and the PS1's (gba_state.c, ps1_state.c): a state belongs to the build family that wrote it.
 *
 * Not state: the ROM (identified by its hash), the machine's options, the RDRAM page write counters of the recompiled
 * code (every page is bumped on load, so each lifted function is checked against RDRAM again before it runs), the
 * attached table's dispatch cache, the VI's picture path (n64_vi.c: canvas and line buffers, rebuilt from the next
 * picture), host pointers (devices, buffers, the tty sink), the RTC's test clock. A deferred DP interrupt (EV_DP,
 * n64_core.c) is completed before a save, so a state never holds one. */
#include "n64_internal.h"
#include "n64_rdp.h"

#include <stdlib.h>
#include <string.h>

#define STATE_MAGIC "XRSTATE\0"
#define HEADER_BYTES 40u
#define PAGE 4096u
#define ISV_SIZE 0x10000u

enum { SEC_MACHINE = 1, SEC_RDRAM, SEC_RSP, SEC_RDP, SEC_FLASH, SEC_ISV, SEC_RC, SEC_COUNT = SEC_RC };

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

/* The ROM (big-endian, as the machine holds it), 8 bytes at a time: computed once per machine (up to 64 MB). */
static uint64_t rom_hash(n64_machine *m)
{
    if (!m->rom_hashed) {
        uint64_t h = 0xCBF29CE484222325ull ^ m->rom_size;
        for (uint32_t i = 0; i + 8 <= m->rom_size; i += 8) {
            uint64_t w;
            memcpy(&w, m->rom + i, 8);
            h = (h ^ w) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        m->rom_hash = h;
        m->rom_hashed = true;
    }
    return m->rom_hash;
}

static int page_zero(const uint8_t *p)
{
    static const uint8_t zero[PAGE];
    return memcmp(p, zero, PAGE) == 0;
}

/* RDRAM: a bitmap of the pages stored (bit set: not all zero), then those pages. all: every page, zero or not (rewind
 * captures, XBA-278: a fixed layout, nothing tested). */
static size_t rdram_bytes(n64_machine *m, int all)
{
    uint32_t pages = m->rdram_size / PAGE;
    size_t n = (pages + 7) / 8;
    for (uint32_t i = 0; i < pages; i++)
        if (all || !page_zero(m->rdram + (size_t)i * PAGE))
            n += PAGE;
    return n;
}

static void rdram_save(n64_machine *m, uint8_t *out, int all)
{
    uint32_t pages = m->rdram_size / PAGE;
    uint8_t *bits = out, *p = out + (pages + 7) / 8;
    memset(bits, 0, (pages + 7) / 8);
    for (uint32_t i = 0; i < pages; i++) {
        const uint8_t *src = m->rdram + (size_t)i * PAGE;
        if (!all && page_zero(src))
            continue;
        bits[i >> 3] |= (uint8_t)(1u << (i & 7));
        memcpy(p, src, PAGE);
        p += PAGE;
    }
}

/* The bitmap's page count matches the section's size. */
static int rdram_check(n64_machine *m, const uint8_t *in, size_t n)
{
    uint32_t pages = m->rdram_size / PAGE;
    size_t head = (pages + 7) / 8, stored = 0;
    if (n < head)
        return -1;
    for (uint32_t i = 0; i < pages; i++)
        stored += (in[i >> 3] >> (i & 7)) & 1;
    return n == head + stored * PAGE ? 0 : -1;
}

static void rdram_load(n64_machine *m, const uint8_t *in)
{
    uint32_t pages = m->rdram_size / PAGE;
    const uint8_t *p = in + (pages + 7) / 8;
    for (uint32_t i = 0; i < pages; i++) {
        uint8_t *dst = m->rdram + (size_t)i * PAGE;
        if ((in[i >> 3] >> (i & 7)) & 1) {
            memcpy(dst, p, PAGE);
            p += PAGE;
        } else {
            memset(dst, 0, PAGE);
        }
    }
}

/* Section sizes as this build writes them now (RDRAM, the RDP, FlashRAM and the IS-Viewer vary). */
static size_t sec_size(n64_machine *m, int id, int all)
{
    switch (id) {
    case SEC_MACHINE: return N64_MACHINE_STATE_BYTES;
    case SEC_RDRAM: return rdram_bytes(m, all);
    case SEC_RSP: return n64_rsp_state_size(m->rsp);
    case SEC_RDP: return n64_rdp_state_size(m->rdp);
    case SEC_FLASH: return m->flash ? N64_FLASH_SIZE : 0;
    case SEC_ISV: return m->isv ? ISV_SIZE : 0;
    case SEC_RC: return 4;
    default: return 0;
    }
}

static size_t state_size(n64_machine *m, int all)
{
    if (!m)
        return 0;
    n64_rdp_flush(m->rdp); /* everything drawn is in RDRAM (its zero pages are counted) */
    size_t n = HEADER_BYTES;
    for (int id = 1; id <= SEC_COUNT; id++)
        n += 8 + sec_size(m, id, all);
    return n;
}

size_t n64_state_size(n64_machine *m)
{
    return state_size(m, 0);
}

size_t n64_state_size_rewind(n64_machine *m)
{
    return state_size(m, 1);
}

static void save_machine(n64_machine *m, uint8_t *out)
{
    memcpy(out, m, N64_MACHINE_STATE_BYTES); /* the host-only tail (blocks, interp_ref) left out */
    /* Host pointers and host-side fields: not the machine's (the blob may not be aligned: by offset). */
#define CLEAR(f) do { size_t n_ = sizeof m->f; memset(out + offsetof(n64_machine, f), 0, n_); } while (0)
    CLEAR(rdram);
    CLEAR(rdram_gen);
    CLEAR(rc);
    CLEAR(rom);
    CLEAR(rsp);
    CLEAR(rdp);
    CLEAR(spmem);
    CLEAR(vis);
    CLEAR(flash);
    CLEAR(isv);
    CLEAR(tty);
    CLEAR(tty_ctx);
    CLEAR(rom_hash);
    CLEAR(rom_hashed);
#undef CLEAR
}

static size_t state_save(n64_machine *m, uint8_t *out, size_t cap, int all)
{
    if (!m || !out)
        return 0;
    n64_dp_complete_now(m); /* a deferred DP interrupt comes now: a state never holds one (n64_core.c) */
    n64_rdp_flush(m->rdp);  /* everything drawn is in RDRAM */
    size_t need = state_size(m, all);
    if (cap < need)
        return 0;
    memset(out, 0, HEADER_BYTES);
    memcpy(out, STATE_MAGIC, 8);
    memcpy(out + 8, "n64", 4);
    put32(out + 12, N64_STATE_VERSION);
    put64(out + 16, rom_hash(m));
    put32(out + 24, m->rdram_size);
    put32(out + 28, SEC_COUNT);
    uint8_t *p = out + HEADER_BYTES;
    for (int id = 1; id <= SEC_COUNT; id++) {
        size_t n = sec_size(m, id, all);
        put32(p, (uint32_t)id);
        put32(p + 4, (uint32_t)n);
        p += 8;
        switch (id) {
        case SEC_MACHINE: save_machine(m, p); break;
        case SEC_RDRAM: rdram_save(m, p, all); break;
        case SEC_RSP: n64_rsp_state_save(m->rsp, p); break;
        case SEC_RDP: n64_rdp_state_save(m->rdp, p); break;
        case SEC_FLASH: if (n) memcpy(p, m->flash, n); break;
        case SEC_ISV: if (n) memcpy(p, m->isv, n); break;
        case SEC_RC: put32(p, (uint32_t)n64_recomp_booted(m)); break;
        }
        p += n;
    }
    return need;
}

size_t n64_state_save(n64_machine *m, uint8_t *out, size_t cap)
{
    return state_save(m, out, cap, 0);
}

size_t n64_state_save_rewind(n64_machine *m, uint8_t *out, size_t cap)
{
    return state_save(m, out, cap, 1);
}

/* The machine's saved fields that the core uses as indices, offsets, divisors or loop bounds without masking them: each
 * within what the core itself can reach (a crafted state can hold anything; the ROM's hash only names the game). Read
 * by offset (the blob may not be aligned). flash: the FlashRAM section's size. 0, or -1. */
#define MFIELD(dst, f)                                                                                                 \
    do {                                                                                                               \
        (void)sizeof(char[sizeof(dst) == sizeof(((n64_machine *)0)->f) ? 1 : -1]);                                     \
        memcpy(&(dst), in + offsetof(n64_machine, f), sizeof(dst));                                                    \
    } while (0)

static int finite_in(double v, double lo, double hi) { return v >= lo && v <= hi; } /* false for NaN */

static int machine_check(const uint8_t *in, size_t flash)
{
    uint64_t now, rsp_tick, vi_line_ticks;
    uint32_t vi[16], vi_line, vi_field, vi_lines, ai_dacrate, audio_rd, audio_wr, si_dram, flash_offset, wired_prev;
    int ai_count;
    double ai_src_phase, ai_out_phase;
    bool pal;
    n64_save_kind save_kind;
    n64_flash_mode flash_mode;
    int64_t rtc_offset;
    MFIELD(now, now);
    MFIELD(rsp_tick, rsp_tick);
    MFIELD(vi, vi);
    MFIELD(vi_line, vi_line);
    MFIELD(vi_field, vi_field);
    MFIELD(vi_line_ticks, vi_line_ticks);
    MFIELD(vi_lines, vi_lines);
    MFIELD(pal, pal);
    MFIELD(ai_dacrate, ai_dacrate);
    MFIELD(ai_count, ai_count);
    MFIELD(ai_src_phase, ai_src_phase);
    MFIELD(ai_out_phase, ai_out_phase);
    MFIELD(audio_rd, audio_rd);
    MFIELD(audio_wr, audio_wr);
    MFIELD(si_dram, si_dram);
    MFIELD(save_kind, save_kind);
    MFIELD(flash_mode, flash_mode);
    MFIELD(flash_offset, flash_offset);
    MFIELD(wired_prev, cpu.wired_prev);
    MFIELD(rtc_offset, rtc_offset);

    /* The cartridge save's kind indexes per-kind tables (n64_runtime.c); FlashRAM needs its buffer (n64_pif.c). */
    if ((unsigned)save_kind > N64_SAVE_FLASH || (save_kind == N64_SAVE_FLASH && flash != N64_FLASH_SIZE))
        return -1;
    /* FlashRAM: the mode, and an erase / program target a command can set (0xFFFFFFFF: chip erase; a page, 16 bits). */
    if ((unsigned)flash_mode > FLASH_WRITE ||
        (flash_offset != 0xFFFFFFFFu && (flash_offset % 128 || flash_offset > 0xFFFFu * 128)))
        return -1;
    /* RANDOM's span divides by (32 - WIRED) or (96 - WIRED) (n64_cpu.c random_reg): WIRED is 6 bits. */
    if (wired_prev > 63)
        return -1;
    /* SI DMA: si_dram + 64 must not wrap (n64_bus.c si_write); the register is masked so. */
    if (si_dram & ~0x00FFFFFCu)
        return -1;
    /* The RSP catches up with (now - rsp_tick) (n64_core.c). */
    if (rsp_tick > now)
        return -1;
    /* VI: the field picks a canvas row parity (n64_vi.c), the line length paces the VI event (0: it never advances)
     * and both come from V_SYNC / H_SYNC (n64_io.c vi_timing). */
    uint64_t ticks;
    uint32_t lines;
    n64_vi_timing_of(vi, pal, &ticks, &lines);
    if (vi_field > 1 || vi_line_ticks != ticks || vi_lines != lines || vi_line >= vi_lines)
        return -1;
    /* AI: two buffers queue (ai_fifo[ai_count]), the DAC rate divides the VI clock (n64_audio_source_rate), the phases
     * pick resampler rows ((int) of them: finite), the output ring holds N64_AUDIO_RING frames. */
    if (ai_count < 0 || ai_count > 2 || ai_dacrate > 0x3FFF || audio_wr - audio_rd > N64_AUDIO_RING)
        return -1;
    if (!finite_in(ai_src_phase, 0, 1e12) || !finite_in(ai_out_phase, 0, (double)N64_TICK_HZ / N64_AUDIO_RATE))
        return -1;
    for (size_t i = 0; i < 2 * AI_HIST; i++) { /* the DAC's 16-bit samples */
        float h;
        memcpy(&h, in + offsetof(n64_machine, ai_hist) + i * sizeof h, sizeof h);
        if (!finite_in(h, -32768.0, 32767.0))
            return -1;
    }
    for (size_t i = 0; i < (AI_PHASES + 1) * AI_TAPS; i++) { /* windowed-sinc taps, each row summing to 1 */
        float k;
        memcpy(&k, in + offsetof(n64_machine, ai_kernel) + i * sizeof k, sizeof k);
        if (!finite_in(k, -4.0, 4.0))
            return -1;
    }
    /* The RTC: seconds added to the host clock (a game sets a date of 1900..2099); gmtime_r must stay in range. */
    if (rtc_offset < -((int64_t)1 << 40) || rtc_offset > ((int64_t)1 << 40))
        return -1;
    return 0;
}
#undef MFIELD

/* Walks the sections: each id once, in order, sizes this build's, then the values of the machine, the RSP and the RDP
 * (every section checked before n64_state_load changes anything). Fills at[] with each section's bytes. */
static int parse(n64_machine *m, const uint8_t *data, size_t size, const uint8_t *at[], size_t len[])
{
    if (!m || !data || size < HEADER_BYTES || memcmp(data, STATE_MAGIC, 8) != 0 || memcmp(data + 8, "n64", 4) != 0)
        return N64_STATE_ECORRUPT;
    if (get32(data + 12) != N64_STATE_VERSION)
        return N64_STATE_EVERSION;
    if (get64(data + 16) != rom_hash(m))
        return N64_STATE_EGAME;
    if (get32(data + 24) != m->rdram_size || get32(data + 28) != SEC_COUNT)
        return N64_STATE_EVERSION;
    size_t off = HEADER_BYTES;
    for (int id = 1; id <= SEC_COUNT; id++) {
        if (size - off < 8)
            return N64_STATE_ECORRUPT;
        uint32_t sid = get32(data + off), n = get32(data + off + 4);
        off += 8;
        if (sid != (uint32_t)id || n > size - off)
            return N64_STATE_ECORRUPT;
        int ok;
        switch (id) {
        case SEC_MACHINE: ok = n == N64_MACHINE_STATE_BYTES; break;
        case SEC_RDRAM: ok = rdram_check(m, data + off, n) == 0; break;
        case SEC_RSP: ok = n == n64_rsp_state_size(m->rsp); break;
        case SEC_RDP: ok = 1; break; /* n64_rdp_state_check, below */
        case SEC_FLASH: ok = n == 0 || n == N64_FLASH_SIZE; break;
        case SEC_ISV: ok = n == 0 || n == ISV_SIZE; break;
        default: ok = n == 4; break;
        }
        if (!ok)
            return id == SEC_RDRAM ? N64_STATE_ECORRUPT : N64_STATE_EVERSION;
        at[id] = data + off;
        len[id] = n;
        off += n;
    }
    int rdp = n64_rdp_state_check(m->rdp, at[SEC_RDP], len[SEC_RDP]);
    if (rdp == -1)
        return N64_STATE_EVERSION;
    if (rdp != 0 || machine_check(at[SEC_MACHINE], len[SEC_FLASH]) != 0 ||
        n64_rsp_state_check(m->rsp, at[SEC_RSP], len[SEC_RSP]) != 0)
        return N64_STATE_ECORRUPT;
    return N64_STATE_OK;
}

int n64_state_check(n64_machine *m, const uint8_t *data, size_t size)
{
    const uint8_t *at[SEC_COUNT + 1];
    size_t len[SEC_COUNT + 1];
    return parse(m, data, size, at, len);
}

int n64_state_load(n64_machine *m, const uint8_t *data, size_t size)
{
    const uint8_t *at[SEC_COUNT + 1];
    size_t len[SEC_COUNT + 1];
    int rc = parse(m, data, size, at, len);
    if (rc != N64_STATE_OK)
        return rc;
    /* What may still fail first: the buffers the state needs, then the RDP's load (parse checked its bytes). */
    uint8_t *flash = m->flash, *isv = m->isv;
    if (len[SEC_FLASH] && !flash && !(flash = malloc(N64_FLASH_SIZE)))
        return N64_STATE_ECORRUPT;
    if (len[SEC_ISV] && !isv && !(isv = calloc(1, ISV_SIZE))) {
        if (flash != m->flash)
            free(flash);
        return N64_STATE_ECORRUPT;
    }
    if (n64_rdp_state_load(m->rdp, at[SEC_RDP], len[SEC_RDP]) != 0) {
        if (flash != m->flash)
            free(flash);
        if (isv != m->isv)
            free(isv);
        return N64_STATE_EVERSION;
    }
    n64_rsp_state_load(m->rsp, at[SEC_RSP], len[SEC_RSP]); /* its size was checked */

    /* The machine, keeping this one's host side. */
    uint8_t *rdram = m->rdram, *rom = m->rom, *spmem = m->spmem;
    uint32_t *rdram_gen = m->rdram_gen;
    uint32_t rdram_size = m->rdram_size, rom_size = m->rom_size;
    struct n64_rc_state *rcs = m->rc;
    n64_rom_info info = m->info;
    n64_options opts = m->opts;
    n64_rsp *rsp = m->rsp;
    n64_rdp *rdp = m->rdp;
    struct n64_vi_state *vis = m->vis;
    void (*tty)(void *, char) = m->tty;
    void *tty_ctx = m->tty_ctx;
    int64_t rtc_fixed = m->rtc_fixed;
    uint64_t hash = m->rom_hash;
    bool hashed = m->rom_hashed;
    memcpy(m, at[SEC_MACHINE], N64_MACHINE_STATE_BYTES); /* the host-only tail stays */
    m->rdram = rdram;
    m->rdram_gen = rdram_gen;
    m->rdram_size = rdram_size;
    m->rc = rcs;
    m->rom = rom;
    m->rom_size = rom_size;
    m->info = info;
    m->opts = opts;
    m->rsp = rsp;
    m->rdp = rdp;
    m->spmem = spmem;
    m->vis = vis;
    m->tty = tty;
    m->tty_ctx = tty_ctx;
    m->rtc_fixed = rtc_fixed;
    m->rom_hash = hash;
    m->rom_hashed = hashed;
    m->save_dirty = false;
    m->ai_quiet = 0; /* the loaded history is filtered until AI_TAPS zero samples follow */
    for (int p = 0; p < 4; p++)
        m->pak_dirty[p] = false;

    m->flash = flash;
    if (len[SEC_FLASH])
        memcpy(m->flash, at[SEC_FLASH], N64_FLASH_SIZE);
    else if (m->flash)
        memset(m->flash, 0xFF, N64_FLASH_SIZE); /* erased, as before the game first used it */
    if (len[SEC_ISV]) {
        m->isv = isv;
        memcpy(m->isv, at[SEC_ISV], ISV_SIZE);
    } else {
        free(isv);
        m->isv = NULL;
    }
    rdram_load(m, at[SEC_RDRAM]);
    for (uint32_t p = 0; p < (m->rdram_size >> N64_PAGE_SHIFT); p++)
        m->rdram_gen[p]++; /* new contents everywhere, as far as lifted code is concerned */
    n64_vi_free(m); /* the picture path starts afresh at the next picture */
    n64_dp_state_loaded(m); /* the deferred DP interrupt's policy too (the RDP's load dropped any pending one) */
    n64_recomp_state_loaded(m, (int32_t)get32(at[SEC_RC]));
    return N64_STATE_OK;
}
