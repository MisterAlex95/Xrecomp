/* n64_rdp_vk.cpp -- the Vulkan RDP (XBA-173): the DPC front end of n64_devices.h over paraLLEl-RDP (Themaister, MIT,
 * third_party/parallel-rdp), an LLE RDP on Vulkan compute that aims at angrylion's output bit for bit. A back end of
 * n64_rdp_select.c (n64_rdp_backend.h): n64_rdp_vk_ops.create returns NULL when there is no Vulkan loader, no device,
 * or a device without what paraLLEl-RDP needs (Vulkan 1.1, 8 / 16-bit storage), and the software RDP takes over.
 *
 * Front end: the same DPC registers as n64_rdp.c (START / END / CURRENT / STATUS with start pending, XBUS from DMEM,
 * freeze, CLOCK / BUFBUSY / PIPEBUSY / TMEM counters approximated from the commands the same way): whole commands are
 * collected from RDRAM or DMEM and queued to paraLLEl-RDP's command thread as 32-bit words, high word first.
 *
 * RDRAM: the core's own array (big-endian bytes) is handed to paraLLEl-RDP as host RDRAM, without
 * VK_EXT_external_memory_host (its "incoherent" path): before a render pass it copies the pages the commands read into
 * its GPU copy, after the pass it writes back, byte-masked, the bytes the GPU wrote (third_party/parallel-rdp patch:
 * every word is byte-swapped on the way, the GPU copy is in host words). Coherence with the CPU:
 *   - SYNC_FULL: the commands are flushed and the CPU waits for the GPU and for the write-back (wait_for_timeline),
 *     then the DP interrupt is raised: whatever the game reads after the interrupt (frame buffer, RDRAM the recompiled
 *     code may run from) is in the array;
 *   - n64_rdp_flush (the VI is about to read the frame buffer, n64_display): commands queued since the last SYNC_FULL
 *     are drawn and waited for;
 *   - CPU writes (textures, command lists) are seen by the GPU when the commands that read them are flushed, which is
 *     at the latest at SYNC_FULL: as on the hardware, the game must not change what the RDP has not read yet.
 * Write tracking: the colour and Z image rows the drawing commands cover (scissored) are reported through bus.wrote
 * once they are in RDRAM (after a wait), never before, as n64_rdp.c does. */
#include "n64_rdp_backend.h"
#include "n64_rdp.h"

#include "context.hpp"
#include "device.hpp"
#include "rdp_device.hpp"
#include "thread_id.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace {

enum : uint32_t {
    ST_XBUS = 0x001,
    ST_FREEZE = 0x002,
    ST_FLUSH = 0x004,
    ST_GCLK = 0x008,
    ST_PIPE_BUSY = 0x020,
    ST_CMD_BUSY = 0x040,
    ST_CBUF_READY = 0x080,
    ST_DMA_BUSY = 0x100,
    ST_START_PENDING = 0x400,
};
enum { CYC_1 = 0, CYC_2 = 1, CYC_COPY = 2, CYC_FILL = 3 };

struct VkRdp {
    n64_rcp_bus bus{};
    const uint8_t *dmem = nullptr;
    uint32_t start = 0, end = 0, current = 0, status = 0;
    uint32_t clock = 0, bufbusy = 0, pipebusy = 0, tmemc = 0;
    int start_pending = 0;
    uint32_t pend[44]{}; /* one command, 32-bit words (high word first), up to 22 64-bit words */
    int npend = 0;       /* 64-bit words collected */
    bool unsynced = false; /* commands queued since the last wait */

    /* Front-end view of the state, for the costs and the written ranges (as in n64_rdp.c). */
    int fe_cycle = 0, fe_ci_size = 2, fe_ci_width = 320, fe_zon = 0, fe_sc_y0 = 0, fe_sc_y1 = 0xFFF;
    uint32_t fe_ci_addr = 0, fe_zi_addr = 0;
    uint32_t wr_lo[2]{}, wr_hi[2]{};

    std::unique_ptr<Vulkan::Context> ctx;
    std::unique_ptr<Vulkan::Device> dev;
    std::unique_ptr<RDP::CommandProcessor> cp;
};

/* Granite records command buffers per registered thread index: whichever thread drives the RDP is index 0, like
 * paraLLEl-RDP's own command and timeline threads (one thread at a time drives a machine). */
void on_thread()
{
    static thread_local bool registered;
    if (!registered) {
        Util::register_thread_index(0);
        registered = true;
    }
}

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

int cmd_len(int id)
{
    if (id >= 0x08 && id <= 0x0F)
        return 4 + ((id & 4) ? 8 : 0) + ((id & 2) ? 8 : 0) + ((id & 1) ? 2 : 0);
    if (id == 0x24 || id == 0x25)
        return 2;
    return 1;
}

void wrote_add(VkRdp *d, int y0, int y1)
{
    y0 = imax(y0, d->fe_sc_y0 >> 2);
    y1 = imin(y1, d->fe_sc_y1 >> 2);
    if (y0 < 0)
        y0 = 0;
    if (y1 < y0)
        return;
    const uint32_t stride[2] = { ((uint32_t)d->fe_ci_width << d->fe_ci_size) >> 1, (uint32_t)d->fe_ci_width * 2u };
    const uint32_t base[2] = { d->fe_ci_addr, d->fe_zi_addr };
    const int n = d->fe_zon && d->fe_cycle != CYC_FILL && d->fe_cycle != CYC_COPY ? 2 : 1;
    for (int i = 0; i < n; i++) {
        uint32_t lo = base[i] + (uint32_t)y0 * stride[i], hi = base[i] + (uint32_t)(y1 + 1) * stride[i];
        if (d->wr_hi[i] <= d->wr_lo[i]) {
            d->wr_lo[i] = lo;
            d->wr_hi[i] = hi;
        } else {
            d->wr_lo[i] = lo < d->wr_lo[i] ? lo : d->wr_lo[i];
            d->wr_hi[i] = hi > d->wr_hi[i] ? hi : d->wr_hi[i];
        }
    }
}

void wrote_report(VkRdp *d)
{
    for (int i = 0; i < 2; i++) {
        if (d->wr_hi[i] > d->wr_lo[i] && d->bus.wrote)
            d->bus.wrote(d->bus.ctx, d->wr_lo[i], d->wr_hi[i] - d->wr_lo[i]);
        d->wr_lo[i] = d->wr_hi[i] = 0;
    }
}

/* Everything queued is drawn and written back into the core's RDRAM. */
void sync(VkRdp *d)
{
    if (d->unsynced) {
        on_thread();
        d->cp->wait_for_timeline(d->cp->signal_timeline());
        d->unsynced = false;
    }
    wrote_report(d);
}

uint32_t cmd_cost(VkRdp *d, const uint32_t *w, int id)
{
    const uint64_t w0 = (uint64_t)w[0] << 32 | w[1];
    const uint32_t per = d->fe_cycle == CYC_2 ? 2 : 1;
    if (id >= 0x08 && id <= 0x0F) {
        int yh = sext((uint32_t)w0 & 0x3FFF, 14), yl = sext((uint32_t)(w0 >> 32) & 0x3FFF, 14);
        int64_t xh = (int32_t)w[4], xm = (int32_t)w[6], xl = (int32_t)w[2];
        int64_t wd = (llabs(xm - xh) + llabs(xl - xh)) >> 17;
        int64_t rows = imax(yl - yh, 0) >> 2;
        int64_t c = rows * (wd + 1) * per + 24;
        return c > 0xFFFFFF ? 0xFFFFFF : (uint32_t)c;
    }
    if (id == 0x24 || id == 0x25 || id == 0x36) {
        int xl = (int)(w0 >> 44) & 0xFFF, yl = (int)(w0 >> 32) & 0xFFF, xh = (int)(w0 >> 12) & 0xFFF, yh = (int)w0 & 0xFFF;
        uint32_t px = (uint32_t)(imax((xl >> 2) - (xh >> 2), 0) + 1) * (uint32_t)(imax((yl >> 2) - (yh >> 2), 0) + 1);
        if (d->fe_cycle == CYC_FILL)
            return px / (d->fe_ci_size == 3 ? 2 : 4) + 8;
        if (d->fe_cycle == CYC_COPY)
            return px / 4 + 8;
        return px * per + 8;
    }
    return 1;
}

void dispatch(VkRdp *d, const uint32_t *w, int len)
{
    const uint64_t w0 = (uint64_t)w[0] << 32 | w[1];
    const int id = (int)(w0 >> 56) & 0x3F;
    uint32_t cost = cmd_cost(d, w, id);
    d->clock = (d->clock + cost) & 0xFFFFFF;
    d->bufbusy = (d->bufbusy + cost) & 0xFFFFFF;
    const bool draw = (id >= 0x08 && id <= 0x0F) || id == 0x24 || id == 0x25 || id == 0x36;
    if (draw)
        d->pipebusy = (d->pipebusy + cost) & 0xFFFFFF;
    if (id != 0x29)
        d->status |= ST_PIPE_BUSY | ST_GCLK;
    if (id == 0x30 || id == 0x33 || id == 0x34) {
        uint32_t sh = (uint32_t)(w0 >> 12) & 0xFFF, sl = (uint32_t)(w0 >> 44) & 0xFFF;
        d->tmemc = (d->tmemc + ((sh - sl) >> 2) + 4) & 0xFFFFFF;
    }
    switch (id) {
    case 0x2F: d->fe_cycle = (int)(w0 >> 52) & 3; d->fe_zon = (w0 >> 4 & 3) != 0; break;
    case 0x2D: d->fe_sc_y0 = (int)(w0 >> 32) & 0xFFF; d->fe_sc_y1 = (int)w0 & 0xFFF; break;
    case 0x3E: d->fe_zi_addr = (uint32_t)w0 & 0xFFFFFF; break;
    case 0x3F:
        d->fe_ci_addr = (uint32_t)w0 & 0xFFFFFF;
        d->fe_ci_size = (int)(w0 >> 51) & 3;
        d->fe_ci_width = (int)((w0 >> 32) & 0x3FF) + 1;
        break;
    default: break;
    }
    if (draw) {
        if (id <= 0x0F)
            wrote_add(d, sext((uint32_t)w0 & 0x3FFF, 14) >> 2, sext((uint32_t)(w0 >> 32) & 0x3FFF, 14) >> 2);
        else
            wrote_add(d, (int)(w0 & 0xFFF) >> 2, ((int)(w0 >> 32) & 0xFFF) / 4 + 1);
    }

    d->cp->enqueue_command((unsigned)len * 2, w);
    d->unsynced = true;
    if (id == 0x29) { /* SYNC_FULL: drawn and in RDRAM, then the DP interrupt */
        sync(d);
        d->cp->begin_frame_context(); /* recycles the per-frame Vulkan objects (nothing is in flight now) */
        d->status &= ~(ST_PIPE_BUSY | ST_GCLK);
        if (d->bus.raise)
            d->bus.raise(d->bus.ctx, N64_MI_DP);
    }
}

void fetch64(const VkRdp *d, uint32_t a, uint32_t *hi, uint32_t *lo)
{
    uint8_t b[8];
    if (d->status & ST_XBUS) {
        if (!d->dmem) {
            *hi = *lo = 0;
            return;
        }
        for (int i = 0; i < 8; i++)
            b[i] = d->dmem[(a + (uint32_t)i) & 0xFFF];
    } else {
        for (int i = 0; i < 8; i++) {
            uint32_t x = (a + (uint32_t)i) & 0xFFFFFF;
            b[i] = x < d->bus.rdram_size ? d->bus.rdram[x] : 0;
        }
    }
    *hi = n64_be32(b);
    *lo = n64_be32(b + 4);
}

/* ---- n64_rdp_ops ---------------------------------------------------------------------------------------------------- */
void vk_reset(void *p);

void *vk_create(n64_rcp_bus bus)
{
    /* paraLLEl-RDP's incoherent path pages RDRAM by a power of two. */
    if (!bus.rdram || bus.rdram_size < (1u << 20) || (bus.rdram_size & (bus.rdram_size - 1)))
        return nullptr;
    on_thread();
    if (!Vulkan::Context::init_loader(nullptr)) {
        fprintf(stderr, "n64: Vulkan RDP: no Vulkan loader\n");
        return nullptr;
    }
    std::unique_ptr<VkRdp> d(new (std::nothrow) VkRdp);
    if (!d)
        return nullptr;
    d->bus = bus;
    d->ctx.reset(new Vulkan::Context);
    Vulkan::Context::SystemHandles handles = {};
    d->ctx->set_system_handles(handles);
    d->ctx->set_num_thread_indices(1);
    if (!d->ctx->init_instance_and_device(nullptr, 0, nullptr, 0, 0)) {
        fprintf(stderr, "n64: Vulkan RDP: no Vulkan device\n");
        return nullptr;
    }
    d->dev.reset(new Vulkan::Device);
    d->dev->set_context(*d->ctx);
    d->cp.reset(new RDP::CommandProcessor(*d->dev, bus.rdram, 0, bus.rdram_size, bus.rdram_size / 2, 0));
    if (!d->cp->device_is_supported()) {
        fprintf(stderr, "n64: Vulkan RDP: the device (%s) lacks what paraLLEl-RDP needs\n",
                d->dev->get_gpu_properties().deviceName);
        d->cp.reset();
        return nullptr;
    }
    printf("n64: Vulkan RDP on %s (paraLLEl-RDP)\n", d->dev->get_gpu_properties().deviceName);
    fflush(stdout);
    vk_reset(d.get());
    return d.release();
}

void vk_destroy(void *p)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    if (!d)
        return;
    on_thread();
    if (d->cp)
        d->cp->idle();
    d->cp.reset(); /* the command processor, then the device, then the instance */
    d->dev.reset();
    d->ctx.reset();
    delete d;
}

void vk_reset(void *p)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    sync(d);
    d->start = d->end = d->current = 0;
    d->status = 0;
    d->clock = d->bufbusy = d->pipebusy = d->tmemc = 0;
    d->start_pending = 0;
    d->npend = 0;
    d->fe_cycle = 0;
    d->fe_ci_size = 2;
    d->fe_ci_width = 320;
    d->fe_ci_addr = d->fe_zi_addr = 0;
    d->fe_sc_y0 = 0;
    d->fe_sc_y1 = 0xFFF;
    d->fe_zon = 0;
    d->wr_lo[0] = d->wr_hi[0] = d->wr_lo[1] = d->wr_hi[1] = 0;
}

void vk_attach_dmem(void *p, const uint8_t *dmem) { static_cast<VkRdp *>(p)->dmem = dmem; }

uint32_t vk_read_reg(void *p, uint32_t reg)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    switch (reg & 7) {
    case 0: return d->start;
    case 1: return d->end;
    case 2: return d->current;
    case 3: {
        uint32_t st = d->status | ST_CBUF_READY;
        if (d->current != d->end || d->npend)
            st |= ST_CMD_BUSY | ST_DMA_BUSY;
        if (d->start_pending)
            st |= ST_START_PENDING;
        return st;
    }
    case 4: return d->clock;
    case 5: return d->bufbusy;
    case 6: return d->pipebusy;
    default: return d->tmemc;
    }
}

void vk_write_reg(void *p, uint32_t reg, uint32_t v)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    switch (reg & 7) {
    case 0:
        if (!d->start_pending) {
            d->start = v & 0xFFFFF8;
            d->start_pending = 1;
        }
        break;
    case 1:
        d->end = v & 0xFFFFF8;
        d->status |= ST_PIPE_BUSY | ST_GCLK;
        if (d->start_pending) {
            d->current = d->start;
            d->start_pending = 0;
            d->npend = 0;
        }
        break;
    case 3:
        if (v & 0x001) d->status &= ~ST_XBUS;
        if (v & 0x002) d->status |= ST_XBUS;
        if (v & 0x004) d->status &= ~ST_FREEZE;
        if (v & 0x008) d->status |= ST_FREEZE;
        if (v & 0x010) d->status &= ~ST_FLUSH;
        if (v & 0x020) d->status |= ST_FLUSH;
        if (v & 0x040) d->tmemc = 0;
        if (v & 0x080) d->pipebusy = 0;
        if (v & 0x100) d->bufbusy = 0;
        if (v & 0x200) d->clock = 0;
        break;
    default:
        break;
    }
}

void vk_run(void *p)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    if (d->status & ST_FREEZE)
        return;
    if (d->current < d->end)
        on_thread();
    while (d->current < d->end) {
        fetch64(d, d->current, &d->pend[2 * d->npend], &d->pend[2 * d->npend + 1]);
        d->npend++;
        d->current += 8;
        int need = cmd_len((int)(d->pend[0] >> 24) & 0x3F);
        if (d->npend >= need) {
            dispatch(d, d->pend, need);
            d->npend = 0;
        }
    }
}

int vk_set_threads(void *p, int n)
{
    (void)p;
    (void)n;
    return 1;
}

void vk_flush(void *p)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    sync(d);
}

} // namespace

extern "C" const n64_rdp_ops n64_rdp_vk_ops = { "vulkan", vk_create, vk_destroy, vk_reset, vk_attach_dmem,
                                                vk_read_reg, vk_write_reg, vk_run, vk_set_threads, vk_flush };
