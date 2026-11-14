/* n64_rdp_select.c -- the RDP of n64_devices.h / n64_rdp.h as a choice of back ends (XBA-173, n64_rdp_backend.h): the
 * software RDP (n64_rdp.c, its functions renamed n64_rdp_sw_*) or the Vulkan one (n64_rdp_vk.cpp, paraLLEl-RDP). The
 * back end is chosen at n64_rdp_create; every other call goes through its table. A back end that stops being able to
 * draw (ops->failed: the Vulkan RDP with a shader its driver cannot compile) is replaced by the software RDP once
 * the RDP is idle, after n64_rdp_run or n64_rdp_flush. The internal resolution (XBA-185, n64_rdp.h) is resolved here
 * ($RECOMP_N64_UPSCALE, $RECOMP_UPSCALE) and asked of the back end; only the Vulkan one honours it. */
#include "n64_rdp.h"
#include "n64_rdp_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The software RDP, as n64_rdp_sw_names.h renames it. */
typedef struct n64_rdp_sw n64_rdp_sw;
n64_rdp_sw *n64_rdp_sw_create(n64_rcp_bus bus);
void n64_rdp_sw_destroy(n64_rdp_sw *d);
void n64_rdp_sw_reset(n64_rdp_sw *d);
void n64_rdp_sw_attach_dmem(n64_rdp_sw *d, const uint8_t *dmem);
uint32_t n64_rdp_sw_read_reg(n64_rdp_sw *d, uint32_t reg);
void n64_rdp_sw_write_reg(n64_rdp_sw *d, uint32_t reg, uint32_t v);
void n64_rdp_sw_run(n64_rdp_sw *d);
int n64_rdp_sw_set_threads(n64_rdp_sw *d, int n);
void n64_rdp_sw_flush(n64_rdp_sw *d);

static void *sw_create(n64_rcp_bus bus) { return n64_rdp_sw_create(bus); }
static void sw_destroy(void *d) { n64_rdp_sw_destroy(d); }
static void sw_reset(void *d) { n64_rdp_sw_reset(d); }
static void sw_attach_dmem(void *d, const uint8_t *dmem) { n64_rdp_sw_attach_dmem(d, dmem); }
static uint32_t sw_read_reg(void *d, uint32_t reg) { return n64_rdp_sw_read_reg(d, reg); }
static void sw_write_reg(void *d, uint32_t reg, uint32_t v) { n64_rdp_sw_write_reg(d, reg, v); }
static void sw_run(void *d) { n64_rdp_sw_run(d); }
static int sw_set_threads(void *d, int n) { return n64_rdp_sw_set_threads(d, n); }
static void sw_flush(void *d) { n64_rdp_sw_flush(d); }

const n64_rdp_ops n64_rdp_sw_ops = { "software", sw_create, sw_destroy, sw_reset, sw_attach_dmem,
                                     sw_read_reg, sw_write_reg, sw_run, sw_set_threads, sw_flush, NULL,
                                     NULL, NULL, NULL };

struct n64_rdp {
    const n64_rdp_ops *ops;
    void *impl;
    n64_rcp_bus bus;     /* for a replacement back end */
    const uint8_t *dmem;
    int threads;         /* the last n64_rdp_set_threads, 0: never called */
};

#ifndef N64_RDP_DEFAULT_BACKEND
#if defined(__ANDROID__) && defined(N64_RDP_HAVE_VULKAN)
#define N64_RDP_DEFAULT_BACKEND N64_RDP_VULKAN
#else
#define N64_RDP_DEFAULT_BACKEND N64_RDP_SOFTWARE
#endif
#endif

static n64_rdp_backend s_backend = N64_RDP_AUTO;

void n64_rdp_set_backend(n64_rdp_backend b) { s_backend = b; }

n64_rdp_backend n64_rdp_backend_parse(const char *s)
{
    if (s && (!strcmp(s, "vulkan") || !strcmp(s, "vk") || !strcmp(s, "gpu")))
        return N64_RDP_VULKAN;
    if (s && (!strcmp(s, "software") || !strcmp(s, "sw") || !strcmp(s, "cpu")))
        return N64_RDP_SOFTWARE;
    return N64_RDP_AUTO;
}

const char *n64_rdp_backend_name(const n64_rdp *d) { return d ? d->ops->name : "none"; }

/* ---- Internal resolution (XBA-185) ---------------------------------------------------------------------------------- */
static int s_upscale; /* n64_rdp_set_upscale; 0: the environment */

int n64_rdp_upscale_parse(const char *s)
{
    if (!s || !*s)
        return 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end || v < 1)
        return 0;
    return v >= 8 ? 8 : v >= 4 ? 4 : v >= 2 ? 2 : 1;
}

int n64_rdp_upscale_env(void)
{
    int f = n64_rdp_upscale_parse(getenv("RECOMP_N64_UPSCALE"));
    if (!f)
        f = n64_rdp_upscale_parse(getenv("RECOMP_UPSCALE"));
    return f ? f : 1;
}

void n64_rdp_set_upscale(int factor)
{
    s_upscale = factor <= 0 ? 0 : factor >= 8 ? 8 : factor >= 4 ? 4 : factor >= 2 ? 2 : 1;
}

int n64_rdp_upscale_wanted(void) { return s_upscale ? s_upscale : n64_rdp_upscale_env(); }

int n64_rdp_upscale(const n64_rdp *d) { return d && d->ops->upscale ? d->ops->upscale(d->impl) : 1; }

int n64_rdp_scanout_scale(const n64_rdp *d)
{
    return d && d->ops->scanout_scale && d->ops->scanout ? d->ops->scanout_scale(d->impl) : 0;
}

int n64_rdp_scanout(n64_rdp *d, const uint32_t *vi, uint32_t field, uint32_t *xrgb, int max_w, int max_h, int *w,
                    int *h)
{
    if (!n64_rdp_scanout_scale(d))
        return -1;
    return d->ops->scanout(d->impl, vi, field, xrgb, max_w, max_h, w, h);
}

/* The software RDP draws at 1x: said once when more was asked (also when the Vulkan RDP could not start or failed). */
static void upscale_needs_vulkan(void)
{
    int f = n64_rdp_upscale_wanted();
    if (f > 1)
        fprintf(stderr, "n64: upscale %dx needs the Vulkan RDP, the software RDP draws at 1x\n", f);
}

n64_rdp *n64_rdp_create(n64_rcp_bus bus)
{
    n64_rdp *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->bus = bus;
    n64_rdp_backend b = s_backend != N64_RDP_AUTO ? s_backend : n64_rdp_backend_parse(getenv("RECOMP_N64_RDP"));
    const int asked = b == N64_RDP_VULKAN;
    (void)asked; /* only reported in builds without the Vulkan RDP */
    if (b == N64_RDP_AUTO)
        b = N64_RDP_DEFAULT_BACKEND;
    if (b == N64_RDP_VULKAN) {
#ifdef N64_RDP_HAVE_VULKAN
        d->ops = &n64_rdp_vk_ops;
        d->impl = d->ops->create(bus);
        if (!d->impl)
            fprintf(stderr, "n64: the Vulkan RDP cannot run here (no device, or its shaders fail), "
                            "software RDP instead\n");
#else
        if (asked)
            fprintf(stderr, "n64: this build has no Vulkan RDP (XBR_N64_RDP_VULKAN), software RDP instead\n");
#endif
    }
    if (!d->impl) {
        d->ops = &n64_rdp_sw_ops;
        d->impl = d->ops->create(bus);
        if (d->impl)
            upscale_needs_vulkan();
    }
    if (!d->impl) {
        free(d);
        return NULL;
    }
    return d;
}

void n64_rdp_destroy(n64_rdp *d)
{
    if (!d)
        return;
    d->ops->destroy(d->impl);
    free(d);
}

void n64_rdp_reset(n64_rdp *d) { d->ops->reset(d->impl); }
void n64_rdp_attach_dmem(n64_rdp *d, const uint8_t *dmem)
{
    d->dmem = dmem;
    d->ops->attach_dmem(d->impl, dmem);
}
uint32_t n64_rdp_read_reg(n64_rdp *d, uint32_t reg) { return d->ops->read_reg(d->impl, reg); }
void n64_rdp_write_reg(n64_rdp *d, uint32_t reg, uint32_t v) { d->ops->write_reg(d->impl, reg, v); }
int n64_rdp_set_threads(n64_rdp *d, int n)
{
    d->threads = n;
    return d->ops->set_threads(d->impl, n);
}

/* The software RDP in place of a failed back end, with the DPC registers the game sees (the RDP is idle: CURRENT is
 * END) and its status bits. The RDP state the old back end held (modes, tiles, TMEM) is lost: games set it again in
 * their next display list. */
static void replace_failed(n64_rdp *d)
{
    const uint32_t end = d->ops->read_reg(d->impl, 1);
    const uint32_t status = d->ops->read_reg(d->impl, 3);
    if (d->ops->read_reg(d->impl, 2) != end)
        return; /* commands still to run: at a later flush */
    void *sw = n64_rdp_sw_ops.create(d->bus);
    if (!sw) {
        fprintf(stderr, "n64: the software RDP cannot start, the %s RDP stays\n", d->ops->name);
        return;
    }
    fprintf(stderr, "n64: the %s RDP failed, software RDP from now on\n", d->ops->name);
    if (n64_rdp_upscale(d) > 1)
        upscale_needs_vulkan();
    d->ops->destroy(d->impl);
    d->ops = &n64_rdp_sw_ops;
    d->impl = sw;
    if (d->dmem)
        d->ops->attach_dmem(d->impl, d->dmem);
    if (d->threads > 0)
        d->ops->set_threads(d->impl, d->threads);
    d->ops->write_reg(d->impl, 3, (status & 1 ? 0x002 : 0) | (status & 2 ? 0x008 : 0) | (status & 4 ? 0x020 : 0));
    d->ops->write_reg(d->impl, 0, end); /* START then END: CURRENT = END, nothing to run */
    d->ops->write_reg(d->impl, 1, end);
}

/* After the commands ran, the RDP is idle (CURRENT is END): the moment to replace a failed back end. */
void n64_rdp_run(n64_rdp *d)
{
    d->ops->run(d->impl);
    if (d->ops->failed && d->ops->failed(d->impl))
        replace_failed(d);
}

void n64_rdp_flush(n64_rdp *d)
{
    d->ops->flush(d->impl);
    if (d->ops->failed && d->ops->failed(d->impl))
        replace_failed(d);
}

/* The 16-bit pixels' hidden coverage bits (n64_rdp.h, for the VI's anti-aliasing filter; NULL: none, the VI then uses
 * the pixel's low bit). The software RDP keeps them in a side array; the Vulkan RDP keeps them on the GPU: NULL. */
const uint8_t *n64_rdp_sw_hidden_bits(n64_rdp_sw *d);
const uint8_t *n64_rdp_hidden_bits(n64_rdp *d)
{
    return d->ops == &n64_rdp_sw_ops ? n64_rdp_sw_hidden_bits(d->impl) : NULL;
}
