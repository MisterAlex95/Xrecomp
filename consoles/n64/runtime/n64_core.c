/* N64 machine (XBA-169 / 171 / 172): creation, reset, the event scheduler (VI lines, COUNT = COMPARE, PI / SI / AI
 * DMA completions) around the CPU loop, the RSP's time slices, and the debug accessors. */
#include "n64_internal.h"
#include "n64_rdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* While the RSP runs, the CPU runs in slices this long (ticks) and the RSP catches up after each. */
#define RSP_SLICE 600

static void rcp_raise(void *ctx, int irq) { n64_mi_raise(ctx, irq); }
static void rcp_clear(void *ctx, int irq) { n64_mi_clear(ctx, irq); }
static void rcp_wrote(void *ctx, uint32_t addr, uint32_t len) { n64_rdram_wrote(ctx, addr, len); }

static void seterr(char *err, size_t n, const char *msg)
{
    if (err && n)
        snprintf(err, n, "%s", msg);
}

static void machine_reset(n64_machine *m)
{
    m->now = 0;
    m->stop_at = 0;
    for (int i = 0; i < EV_COUNT; i++)
        m->ev[i] = EV_NEVER;
    m->instructions = 0;
    m->frames = 0;
    m->frame_done = false;
    memset(m->rdram, 0, m->rdram_size);
    m->mi_mode = 0;
    m->mi_intr = 0;
    m->mi_mask = 0;
    memset(m->ri, 0, sizeof m->ri);
    memset(m->rdram_regs, 0, sizeof m->rdram_regs);
    m->rdram_regs[0] = 0xB4190010u; /* device type */
    m->pi_dram = m->pi_cart = m->pi_rd_len = m->pi_wr_len = m->pi_status = 0;
    memset(m->pi_dom, 0, sizeof m->pi_dom);
    m->si_dram = m->si_status = 0;
    m->si_read_pending = false;
    m->pif_rom_lock = 0;
    m->flash_mode = FLASH_IDLE;
    m->flash_status = 0;
    n64_rsp_reset(m->rsp);
    n64_rdp_reset(m->rdp);
    m->rsp_tick = 0;
    n64_cpu_reset(m);
    n64_vi_reset(m);
    n64_ai_reset(m);
    n64_pif_boot(m);
    n64_cpu_set_count(m, 0);
    for (uint32_t p = 0; p < (m->rdram_size >> N64_PAGE_SHIFT); p++)
        m->rdram_gen[p]++; /* RDRAM was cleared and rewritten */
    m->rc_lo = m->rc_len = 0;
    m->rc_hit = 0;
    if (m->rc)
        n64_recomp_reset(m);
}

n64_machine *n64_create(const uint8_t *rom, size_t size, const n64_options *opts, char *err, size_t errlen)
{
    if (!rom || size < 0x1000) {
        seterr(err, errlen, "ROM too small (at least 4 KB)");
        return NULL;
    }
    if (size > N64_ROM_MAX) {
        seterr(err, errlen, "ROM too large (at most 64 MB)");
        return NULL;
    }
    n64_machine *m = calloc(1, sizeof *m);
    if (!m) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    if (opts)
        m->opts = *opts;
    else
        m->opts.force_pal = -1;
    uint32_t rsize = (uint32_t)((size + 3) & ~(size_t)3);
    m->rom = calloc(1, rsize);
    m->rdram_size = m->opts.rdram_size == 0x400000u ? 0x400000u : N64_RDRAM_SIZE;
    m->rdram = calloc(1, m->rdram_size);
    m->rdram_gen = calloc(m->rdram_size >> N64_PAGE_SHIFT, sizeof *m->rdram_gen);
    if (!m->rom || !m->rdram || !m->rdram_gen) {
        seterr(err, errlen, "out of memory");
        n64_destroy(m);
        return NULL;
    }
    memcpy(m->rom, rom, size);
    m->rom_size = rsize;
    n64_rom_order order = n64_rom_detect(m->rom, m->rom_size);
    if (n64_rom_normalize(m->rom, m->rom_size) != 0 || n64_rom_parse(m->rom, m->rom_size, &m->info) != 0) {
        seterr(err, errlen, "not an N64 ROM (unknown byte order)");
        n64_destroy(m);
        return NULL;
    }
    m->info.order = order;
    m->pal = m->opts.force_pal >= 0 ? m->opts.force_pal != 0 : m->info.pal != 0;

    n64_rcp_bus bus = {m->rdram, m->rdram_size, rcp_raise, rcp_clear, m, rcp_wrote};
    m->rsp = n64_rsp_create(bus);
    m->rdp = n64_rdp_create(bus);
    if (!m->rsp || !m->rdp) {
        seterr(err, errlen, "cannot create the RSP / RDP");
        n64_destroy(m);
        return NULL;
    }
    n64_rsp_attach_rdp(m->rsp, m->rdp);
    m->spmem = n64_rsp_mem(m->rsp);
    n64_rdp_attach_dmem(m->rdp, m->spmem);

    if (m->opts.save_kind != N64_SAVE_NONE && m->opts.save_kind < N64_SAVE_PAK0)
        n64_save_set_kind(m, m->opts.save_kind);
    else if (m->info.save_hint != N64_SAVE_NONE)
        n64_save_set_kind(m, m->info.save_hint);
    m->save_fixed = m->save_kind != N64_SAVE_NONE;
    for (int p = 0; p < 4; p++)
        n64_pak_format(m->pak[p]);
    m->pad[0].connected = true;
    m->pad[0].acc = N64_ACC_CONTROLLER_PAK;
    machine_reset(m);
    return m;
}

void n64_destroy(n64_machine *m)
{
    if (!m)
        return;
    if (m->rsp)
        n64_rsp_destroy(m->rsp);
    if (m->rdp)
        n64_rdp_destroy(m->rdp);
    n64_recomp_free(m);
    n64_cpu_free(m);
    free(m->rom);
    free(m->rdram);
    free(m->rdram_gen);
    free(m->flash);
    free(m->isv);
    n64_vi_free(m);
    free(m);
}

void n64_reset(n64_machine *m)
{
    machine_reset(m);
}

const n64_rom_info *n64_info(n64_machine *m)
{
    return &m->info;
}

/* ---- Scheduler ------------------------------------------------------------------------------------------------- */

static void dispatch(n64_machine *m)
{
    for (;;) {
        int e = -1;
        uint64_t t = m->now;
        for (int i = 0; i < EV_COUNT; i++)
            if (m->ev[i] <= t) {
                t = m->ev[i];
                e = i;
            }
        if (e < 0)
            return;
        switch (e) {
        case EV_VI:
            n64_vi_event(m);
            break;
        case EV_COMPARE:
            m->cpu.cop0[C0_CAUSE] |= CAUSE_IP7;
            n64_cpu_update_irq(m);
            m->ev[EV_COMPARE] = EV_NEVER;
            n64_cpu_schedule_compare(m);
            if (m->ev[EV_COMPARE] <= m->now) /* COMPARE = COUNT right now: next wrap */
                m->ev[EV_COMPARE] = m->now + (0x100000000ull << 2);
            break;
        case EV_PI:
            m->ev[EV_PI] = EV_NEVER;
            m->pi_status &= ~1u;
            n64_mi_raise(m, N64_MI_PI);
            break;
        case EV_SI:
            m->ev[EV_SI] = EV_NEVER;
            m->si_status &= ~1u;
            n64_mi_raise(m, N64_MI_SI);
            break;
        case EV_AI:
            n64_ai_event(m);
            break;
        default:
            m->ev[e] = EV_NEVER;
            break;
        }
    }
}

static void slice(n64_machine *m, uint64_t end)
{
    uint64_t stop = end;
    for (int i = 0; i < EV_COUNT; i++)
        if (m->ev[i] < stop)
            stop = m->ev[i];
    bool rsp_on = !n64_rsp_halted(m->rsp) && !(m->boot_x105 && n64_pif_x105_hold(m));
    if (rsp_on && stop > m->now + RSP_SLICE)
        stop = m->now + RSP_SLICE;
    m->stop_at = stop;
    if (m->now < stop)
        n64_cpu_run(m);
    if (rsp_on) {
        uint64_t d = (m->now - m->rsp_tick) / 3;
        while (d > 0 && !n64_rsp_halted(m->rsp)) {
            uint32_t n = d > 1000000 ? 1000000 : (uint32_t)d;
            n64_rsp_run(m->rsp, n);
            d -= n;
        }
        m->rsp_tick = m->now;
        n64_rdp_run(m->rdp);
    } else {
        m->rsp_tick = m->now; /* halted, or started during this slice: it runs from now */
    }
    if (N64_UNLIKELY(m->boot_x105))
        n64_pif_x105_step(m);
    dispatch(m);
}

void n64_run_frame(n64_machine *m)
{
    m->frame_done = false;
    uint64_t cap = m->now + m->vi_line_ticks * (uint64_t)(m->vi_lines + 2) * 2;
    while (!m->frame_done && m->now < cap)
        slice(m, cap);
    n64_rdp_frame_end(m->rdp); /* the software RDP's pixels in RDRAM at an emulated moment, shown or not */
    n64_ai_advance(m);
}

void n64_run_cycles(n64_machine *m, uint64_t cycles)
{
    uint64_t end = m->now + cycles * N64_TICKS_PER_CYCLE;
    while (m->now < end)
        slice(m, end);
    n64_ai_advance(m);
}

/* ---- Accessors ------------------------------------------------------------------------------------------------- */

static const uint8_t *peek_ptr(n64_machine *m, uint32_t addr, uint32_t n)
{
    uint32_t pa;
    if (addr >= 0x80000000u && addr < 0xC0000000u)
        pa = addr & 0x1FFFFFFFu;
    else if (addr < 0x20000000u)
        pa = addr;
    else
        return NULL;
    if (pa + n <= m->rdram_size)
        return m->rdram + pa;
    if (pa >= 0x04000000u && pa < 0x04040000u)
        return m->spmem + (pa & 0x1FFF);
    if (pa >= 0x10000000u && pa - 0x10000000u + n <= m->rom_size)
        return m->rom + (pa - 0x10000000u);
    if (pa >= 0x1FC007C0u && pa + n <= 0x1FC00800u)
        return m->pif_ram + (pa - 0x1FC007C0u);
    return NULL;
}

uint32_t n64_peek32(n64_machine *m, uint32_t addr)
{
    const uint8_t *p = peek_ptr(m, addr & ~3u, 4);
    return p ? n64_be32(p) : 0;
}

uint16_t n64_peek16(n64_machine *m, uint32_t addr)
{
    const uint8_t *p = peek_ptr(m, addr & ~1u, 2);
    return p ? n64_be16(p) : 0;
}

uint8_t n64_peek8(n64_machine *m, uint32_t addr)
{
    const uint8_t *p = peek_ptr(m, addr, 1);
    return p ? *p : 0;
}

void n64_poke32(n64_machine *m, uint32_t addr, uint32_t v)
{
    uint8_t *p = (uint8_t *)peek_ptr(m, addr & ~3u, 4);
    if (p && (p < m->rom || p >= m->rom + m->rom_size) && (p < m->pif_ram || p >= m->pif_ram + 64)) {
        n64_wbe32(p, v);
        if (p >= m->rdram && p < m->rdram + m->rdram_size)
            n64_rdram_wrote(m, (uint32_t)(p - m->rdram), 4);
    }
}

uint64_t n64_reg(n64_machine *m, int r) { return r >= 0 && r < 32 ? m->cpu.gpr[r] : 0; }
uint64_t n64_pc(n64_machine *m) { return m->cpu.pc; }
uint64_t n64_cop0(n64_machine *m, int r)
{
    if (r == C0_COUNT)
        return n64_cpu_count(m);
    return r >= 0 && r < 32 ? m->cpu.cop0[r] : 0;
}
uint64_t n64_fpr(n64_machine *m, int r) { return r >= 0 && r < 32 ? m->cpu.fpr[r] : 0; }
uint32_t n64_fcsr(n64_machine *m) { return m->cpu.fcsr; }
uint64_t n64_cycles(n64_machine *m) { return m->now / N64_TICKS_PER_CYCLE; }
uint64_t n64_frames(n64_machine *m) { return m->frames; }
uint8_t *n64_rdram(n64_machine *m)
{
    for (uint32_t p = 0; p < (m->rdram_size >> N64_PAGE_SHIFT); p++)
        m->rdram_gen[p]++; /* the caller may write code there: predecoded blocks and lifted code check it again */
    return m->rdram;
}
uint32_t n64_rdram_size(n64_machine *m) { return m->rdram_size; }
uint64_t n64_instructions(n64_machine *m) { return m->instructions; }

const n64_guest_fault *n64_guest_fault_info(n64_machine *m) { return &m->fault; }

void n64_guest_activity(n64_machine *m, n64_activity *out)
{
    out->vi_origin_changes = m->vi_origin_changes;
    out->si_dmas = m->si_dmas;
    out->pi_dmas = m->pi_dmas;
    out->ai_buffers = m->ai_buffers;
}

void n64_set_tty(n64_machine *m, void (*fn)(void *ctx, char c), void *ctx)
{
    m->tty = fn;
    m->tty_ctx = ctx;
}

struct n64_rsp *n64_get_rsp(n64_machine *m) { return m->rsp; }
struct n64_rdp *n64_get_rdp(n64_machine *m) { return m->rdp; }
