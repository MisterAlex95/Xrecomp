/* n64_rdp_select.c -- the RDP of n64_devices.h / n64_rdp.h as a choice of back ends (XBA-173, n64_rdp_backend.h): the
 * software RDP (n64_rdp.c, its functions renamed n64_rdp_sw_*) or the Vulkan one (n64_rdp_vk.cpp, paraLLEl-RDP). The
 * back end is chosen once, at n64_rdp_create; every other call goes through its table. */
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
                                     sw_read_reg, sw_write_reg, sw_run, sw_set_threads, sw_flush };

struct n64_rdp {
    const n64_rdp_ops *ops;
    void *impl;
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

n64_rdp *n64_rdp_create(n64_rcp_bus bus)
{
    n64_rdp *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
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
            fprintf(stderr, "n64: no Vulkan device for the RDP, software RDP instead\n");
#else
        if (asked)
            fprintf(stderr, "n64: this build has no Vulkan RDP (XBR_N64_RDP_VULKAN), software RDP instead\n");
#endif
    }
    if (!d->impl) {
        d->ops = &n64_rdp_sw_ops;
        d->impl = d->ops->create(bus);
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
void n64_rdp_attach_dmem(n64_rdp *d, const uint8_t *dmem) { d->ops->attach_dmem(d->impl, dmem); }
uint32_t n64_rdp_read_reg(n64_rdp *d, uint32_t reg) { return d->ops->read_reg(d->impl, reg); }
void n64_rdp_write_reg(n64_rdp *d, uint32_t reg, uint32_t v) { d->ops->write_reg(d->impl, reg, v); }
void n64_rdp_run(n64_rdp *d) { d->ops->run(d->impl); }
int n64_rdp_set_threads(n64_rdp *d, int n) { return d->ops->set_threads(d->impl, n); }
void n64_rdp_flush(n64_rdp *d) { d->ops->flush(d->impl); }

/* The 16-bit pixels' hidden coverage bits (n64_rdp.h, for the VI's anti-aliasing filter; NULL: none, the VI then uses
 * the pixel's low bit). The software RDP keeps them in a side array; the Vulkan RDP keeps them on the GPU: NULL. */
const uint8_t *n64_rdp_sw_hidden_bits(n64_rdp_sw *d);
const uint8_t *n64_rdp_hidden_bits(n64_rdp *d)
{
    return d->ops == &n64_rdp_sw_ops ? n64_rdp_sw_hidden_bits(d->impl) : NULL;
}
