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
 * once they are in RDRAM (after a wait), never before, as n64_rdp.c does.
 *
 * Drivers that cannot compile a shader (Adreno 740 on the AYN Thor: one compute pipeline failed at every dispatch,
 * black screen at 15 fps): at create, a few commands are drawn into a corner of RDRAM (saved and restored) and must
 * compile and come out right (check()). If not, paraLLEl-RDP is tried again without subgroup operations, then with its
 * ubershader (not on Qualcomm's driver: ubershader_draws_wrong), then create returns NULL and the software RDP takes
 * over. A failed pipeline is never compiled again
 * (our Granite patch); one that still fails during the game is logged once and reported through vk_failed, and
 * n64_rdp_select.c switches to the software RDP. RECOMP_N64_RDP_CHECK=0 skips the check.
 *
 * Custom driver (Mesa Turnip on the Thor's Adreno, n64_vk_driver.h): when the app selected one (RECOMP_VK_DRIVER_DIR),
 * its vkGetInstanceProcAddr (libadrenotools, libxba_vkdriver.so) is the first loader, and the same tries run on it.
 * If it cannot be opened, makes no device, or fails every try, the system's libvulkan is loaded again and the tries
 * run there: custom driver -> system driver -> software RDP. The start line names the driver (driverName, driverInfo,
 * custom <id> or system). */
#include "n64_rdp_backend.h"
#include "n64_rdp.h"
#include "n64_vk_driver.h"

#include "context.hpp"
#include "device.hpp"
#include "rdp_device.hpp"
#include "thread_id.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

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
    RDP::CommandProcessorFlags cp_flags = 0;
    bool failed = false; /* a pipeline failed during the game: its draws are dropped (vk_failed) */
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
        if (!d->failed && d->cp->has_pipeline_failure()) {
            d->failed = true;
            fprintf(stderr, "n64: Vulkan RDP: a shader failed to compile on %s during the game (the Granite lines "
                            "above name it); switching to the software RDP\n",
                    d->dev->get_gpu_properties().deviceName);
        }
    }
    wrote_report(d);
}

/* ---- The check at create -------------------------------------------------------------------------------------------- */
/* A fill of the colour and Z images, a texture loaded and drawn as a rectangle with a Z test: tile binning, the
 * rasterizer (or the ubershader), depth / blend, TMEM loads and the RDRAM write-back all run. The commands are
 * encoded as in consoles/n64/tests/rdp_cmd.h. */
constexpr uint32_t CHECK_W = 32, CHECK_SPAN = 0x2000; /* 32 x 32 RGBA16 colour, Z, an 8 x 8 texture */
constexpr uint32_t CHECK_FILL = 0xF801F801u;           /* red, coverage bit */

void put(std::vector<uint32_t> &l, uint64_t w)
{
    l.push_back((uint32_t)(w >> 32));
    l.push_back((uint32_t)w);
}

uint64_t op(int id) { return (uint64_t)id << 56; }

bool check(VkRdp *d)
{
    if (const char *e = getenv("RECOMP_N64_RDP_CHECK"))
        if (atoi(e) == 0)
            return !d->cp->has_pipeline_failure();
    const uint32_t base = d->bus.rdram_size - CHECK_SPAN; /* colour, then Z at +0x800, texture at +0x1000 */
    const uint32_t z = base + 0x800, tex = base + 0x1000;
    std::vector<uint8_t> saved(d->bus.rdram + base, d->bus.rdram + base + CHECK_SPAN);
    for (uint32_t i = 0; i < 128; i++) /* the texture: 8 x 8 RGBA16, opaque */
        d->bus.rdram[tex + i] = (uint8_t)(i & 1 ? 0xFF : 0x7C);

    const uint64_t c10 = (uint64_t)(CHECK_W * 4), last = (uint64_t)((CHECK_W - 1) * 4);
    std::vector<uint32_t> l;
    put(l, op(0x2D) | c10 << 12 | c10);                                                     /* scissor 32 x 32 */
    put(l, op(0x2F) | 3ull << 52);                                                          /* fill mode */
    put(l, op(0x3F) | 2ull << 51 | (uint64_t)(CHECK_W - 1) << 32 | z);                      /* colour image = Z */
    put(l, op(0x37) | 0xFFFCFFFCu);                                                         /* far */
    put(l, op(0x36) | last << 44 | last << 32);                                             /* fill rectangle */
    put(l, op(0x27));                                                                       /* sync pipe */
    put(l, op(0x3F) | 2ull << 51 | (uint64_t)(CHECK_W - 1) << 32 | base);                   /* colour image */
    put(l, op(0x3E) | z);                                                                   /* Z image */
    put(l, op(0x37) | CHECK_FILL);
    put(l, op(0x36) | last << 44 | last << 32);
    put(l, op(0x27));
    put(l, op(0x3D) | 2ull << 51 | 7ull << 32 | tex);                                       /* texture image */
    put(l, op(0x35) | 2ull << 51 | 2ull << 41);                                             /* tile 0, line 2 */
    put(l, op(0x34) | (7ull * 4) << 12 | 7ull * 4);                                         /* load tile 8 x 8 */
    put(l, op(0x26));                                                                       /* sync load */
    put(l, op(0x3C) | 15ull << 52 | 31ull << 47 | 7ull << 44 | 7ull << 41 | 15ull << 37 | 31ull << 32 | 15ull << 28 |
               15ull << 24 | 7ull << 21 | 7ull << 18 | 1ull << 15 | 7ull << 12 | 6ull << 9 | 1ull << 6 | 7ull << 3 |
               6ull);                                                                       /* combine: texel 0 */
    put(l, op(0x2E) | 0x100ull << 16);                                                      /* prim depth */
    put(l, op(0x2F) | 1ull << 43 | 1ull << 42 | 3ull << 38 | 3ull << 36 | 1ull << 5 | 1ull << 4 | 1ull << 2);
    put(l, op(0x24) | (24ull * 4) << 44 | (24ull * 4) << 32 | (8ull * 4) << 12 | 8ull * 4); /* texture rectangle */
    put(l, 1ull << 26 | 1ull << 10);                                                        /* s, t 0; 1 texel / px */
    put(l, op(0x29));                                                                       /* sync full */
    for (size_t i = 0; i < l.size();) {
        const int id = (int)(l[i] >> 24) & 0x3F;
        const unsigned words = (unsigned)cmd_len(id) * 2;
        d->cp->enqueue_command(words, &l[i]);
        i += words;
    }
    d->cp->wait_for_timeline(d->cp->signal_timeline());
    d->cp->begin_frame_context();

    const uint8_t *fb = d->bus.rdram + base;
    const uint32_t far_px = (CHECK_W * CHECK_W - 1) * 2; /* the last pixel: filled, outside the rectangle */
    const uint32_t tex_px = (12 * CHECK_W + 12) * 2;     /* inside the rectangle: a texel, not the fill colour */
    const bool fill_ok = fb[0] == 0xF8 && fb[1] == 0x01 && fb[far_px] == 0xF8 && fb[far_px + 1] == 0x01;
    const bool tex_ok = fb[tex_px] == 0x7C && (fb[tex_px + 1] & 0xFE) == 0xFE; /* low bit: coverage */
    const bool pipelines_ok = !d->cp->has_pipeline_failure();
    if (pipelines_ok && !(fill_ok && tex_ok))
        fprintf(stderr, "n64: Vulkan RDP: check drew %02x%02x (fill, expected f801) and %02x%02x (texture, expected "
                        "7cff)\n", fb[0], fb[1], fb[tex_px], fb[tex_px + 1]);
    memcpy(d->bus.rdram + base, saved.data(), CHECK_SPAN);
    return pipelines_ok && fill_ok && tex_ok;
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

/* The Qualcomm proprietary driver (Adreno 740 on the AYN Thor) compiles the ubershader and draws check() right, but
 * games come out black (Ocarina of Time: nothing but a few pixels, no Vulkan error): the software RDP instead.
 * RECOMP_N64_RDP_UBERSHADER=1 tries it anyway. */
bool ubershader_draws_wrong(Vulkan::Device *dev)
{
    if (const char *e = getenv("RECOMP_N64_RDP_UBERSHADER"))
        if (atoi(e) != 0)
            return false;
    return dev->get_device_features().driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY_KHR;
}

/* ---- n64_rdp_ops ---------------------------------------------------------------------------------------------------- */
void vk_reset(void *p);

/* "driver <name> <info>, custom <id>" or "..., system", for the start line. */
void driver_label(Vulkan::Device *dev, bool custom, char *out, size_t n)
{
    const Vulkan::DeviceFeatures &f = dev->get_device_features();
    const char *name = f.driver_name[0] ? f.driver_name : "unknown";
    if (custom)
        snprintf(out, n, "driver %s %s, custom %s", name, f.driver_info, n64_vk_driver_id());
    else
        snprintf(out, n, "driver %s %s, system", name, f.driver_info);
}

/* Instance, device and the shader tries on the Vulkan loader init_loader was given last. 1: d->cp is kept for the game;
 * 0: this driver cannot run paraLLEl-RDP (d->ctx / dev / cp reset); custom is 1 for a player's driver (the start line,
 * and the check that adrenotools did not fall back to the device's own driver, which then counts as the system one). */
bool try_driver(VkRdp *d, bool *custom)
{
    const n64_rcp_bus &bus = d->bus;
    d->ctx.reset(new Vulkan::Context);
    Vulkan::Context::SystemHandles handles = {};
    d->ctx->set_system_handles(handles);
    d->ctx->set_num_thread_indices(1);
    if (!d->ctx->init_instance_and_device(nullptr, 0, nullptr, 0, 0)) {
        fprintf(stderr, "n64: Vulkan RDP: no Vulkan device%s\n", *custom ? " on the custom driver" : "");
        d->ctx.reset();
        return false;
    }
    d->dev.reset(new Vulkan::Device);
    d->dev->set_context(*d->ctx);
    const char *gpu = d->dev->get_gpu_properties().deviceName;
    char drv[2 * VK_MAX_DRIVER_NAME_SIZE + 64];
    if (*custom && d->dev->get_device_features().driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY_KHR) {
        /* adrenotools' hook loads the device's own driver when the custom one fails to load (logcat tag hook_impl) */
        fprintf(stderr, "n64: Vulkan RDP: custom driver %s was not loaded, %s runs on Qualcomm's driver (see logcat "
                        "tag hook_impl)\n", n64_vk_driver_id(), gpu);
        n64_vk_driver_done("not_loaded");
        *custom = false;
    }
    driver_label(d->dev.get(), *custom, drv, sizeof drv);

    /* The default shaders, then without subgroup operations, then the ubershader: the first set that compiles and
     * draws the check right. The device keeps its programs and their failed pipelines from one try to the next. */
    static const struct { RDP::CommandProcessorFlags flags; const char *name; } tries[] = {
        { 0, "default shaders" },
        { RDP::COMMAND_PROCESSOR_FLAG_NO_SUBGROUPS_BIT, "no subgroup operations" },
        { RDP::COMMAND_PROCESSOR_FLAG_NO_SUBGROUPS_BIT | RDP::COMMAND_PROCESSOR_FLAG_UBERSHADER_BIT, "ubershader" },
    };
    bool subgroups = true; /* the last try used subgroup operations */
    for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        if (i == 1 && !subgroups)
            continue; /* the same shaders as the default ones */
        if ((tries[i].flags & RDP::COMMAND_PROCESSOR_FLAG_UBERSHADER_BIT) && ubershader_draws_wrong(d->dev.get())) {
            fprintf(stderr, "n64: Vulkan RDP: the ubershader passes the check on %s but draws a black screen in "
                            "games (Ocarina of Time), not used (RECOMP_N64_RDP_UBERSHADER=1 to try it)\n", gpu);
            break;
        }
        d->cp.reset(new RDP::CommandProcessor(*d->dev, bus.rdram, 0, bus.rdram_size, bus.rdram_size / 2,
                                              tries[i].flags));
        if (!d->cp->device_is_supported()) {
            fprintf(stderr, "n64: Vulkan RDP: the device (%s, %s) lacks what paraLLEl-RDP needs\n", gpu, drv);
            break;
        }
        subgroups = d->cp->uses_subgroups();
        if (check(d)) {
            d->cp_flags = tries[i].flags;
            /* A fresh command processor for the game: none of the check's RDP state. */
            d->cp->idle();
            d->cp.reset(new RDP::CommandProcessor(*d->dev, bus.rdram, 0, bus.rdram_size, bus.rdram_size / 2,
                                                  d->cp_flags));
            printf("n64: Vulkan RDP on %s (paraLLEl-RDP, %s; %s)\n", gpu, tries[i].name, drv);
            fflush(stdout);
            return true;
        }
        fprintf(stderr, "n64: Vulkan RDP: %s fail on %s%s\n", tries[i].name, gpu,
                i + 1 < sizeof tries / sizeof tries[0] ? ", trying again" : "");
        d->cp->idle();
    }
    if (d->cp && d->cp->device_is_supported())
        fprintf(stderr, "n64: Vulkan RDP: the driver of %s (%s) cannot run paraLLEl-RDP's shaders\n", gpu, drv);
    d->cp.reset(); /* the command processor, then the device, then the instance */
    d->dev.reset();
    d->ctx.reset();
    return false;
}

/* The loaders, in order: the player's driver (n64_vk_driver.h) when one is selected and opens, then the system's.
 * Custom driver -> system driver -> NULL, and n64_rdp_select.c starts the software RDP; each step is logged. */
void *vk_create(n64_rcp_bus bus)
{
    /* paraLLEl-RDP's incoherent path pages RDRAM by a power of two. */
    if (!bus.rdram || bus.rdram_size < (1u << 20) || (bus.rdram_size & (bus.rdram_size - 1)))
        return nullptr;
    on_thread();
    std::unique_ptr<VkRdp> d(new (std::nothrow) VkRdp);
    if (!d)
        return nullptr;
    d->bus = bus;

    if (void *gipa = n64_vk_driver_open()) {
        bool custom = true;
        /* vkCreateInstance resolved: a loader that answers nothing would be called through a null pointer */
        if (Vulkan::Context::init_loader(reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa)) && vkCreateInstance) {
            const bool ok = try_driver(d.get(), &custom);
            if (custom)
                n64_vk_driver_done(ok ? "ok" : "failed");
            if (ok) {
                vk_reset(d.get());
                return d.release();
            }
        } else {
            n64_vk_driver_done("failed");
        }
        if (!custom) {
            fprintf(stderr, "n64: Vulkan RDP: the device's own driver was already tried\n");
            return nullptr; /* adrenotools fell back to it: the same driver again would fail the same way */
        }
        fprintf(stderr, "n64: Vulkan RDP: custom driver %s cannot run paraLLEl-RDP, trying the system driver\n",
                n64_vk_driver_id());
        /* the system's libvulkan again, volk re-initialised (the adrenotools handle stays loaded) */
        if (!Vulkan::Context::init_loader(nullptr, true)) {
            fprintf(stderr, "n64: Vulkan RDP: no Vulkan loader\n");
            return nullptr;
        }
    } else if (!Vulkan::Context::init_loader(nullptr)) {
        fprintf(stderr, "n64: Vulkan RDP: no Vulkan loader\n");
        return nullptr;
    }
    bool custom = false;
    if (!try_driver(d.get(), &custom))
        return nullptr;
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

int vk_failed(void *p) { return static_cast<VkRdp *>(p)->failed; }

} // namespace

extern "C" const n64_rdp_ops n64_rdp_vk_ops = { "vulkan", vk_create, vk_destroy, vk_reset, vk_attach_dmem,
                                                vk_read_reg, vk_write_reg, vk_run, vk_set_threads, vk_flush,
                                                vk_failed };
