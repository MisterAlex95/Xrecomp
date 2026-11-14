/* n64_rdp_backend.h -- the RDP back ends behind n64_devices.h (XBA-173), internal to consoles/n64/runtime.
 *
 * n64_rdp_select.c implements the n64_rdp_* functions of n64_devices.h and n64_rdp.h: n64_rdp_create picks a back end
 * (n64_rdp.h: n64_rdp_set_backend, $RECOMP_N64_RDP) and every call goes through its table:
 *   software  n64_rdp.c (or the placeholder n64_rdp_stub.c), compiled with n64_rdp_sw_names.h forced in, so its
 *             public functions are n64_rdp_sw_* and its handle struct n64_rdp_sw: both RDPs link side by side;
 *   vulkan    n64_rdp_vk.cpp: paraLLEl-RDP (third_party/parallel-rdp) on a Vulkan compute device, built when
 *             XBR_N64_RDP_VULKAN is ON (N64_RDP_HAVE_VULKAN defined for n64_rdp_select.c).
 * A test may also reach both directly (consoles/n64/tests/test_rdp_vk.c compares their RDRAM). */
#ifndef N64_RDP_BACKEND_H
#define N64_RDP_BACKEND_H

#include "n64_devices.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct n64_rdp_ops {
    const char *name; /* "software", "vulkan" */
    void *(*create)(n64_rcp_bus bus); /* NULL: this back end cannot run here */
    void (*destroy)(void *d);
    void (*reset)(void *d);
    void (*attach_dmem)(void *d, const uint8_t *dmem);
    uint32_t (*read_reg)(void *d, uint32_t reg);
    void (*write_reg)(void *d, uint32_t reg, uint32_t v);
    void (*run)(void *d);
    int (*set_threads)(void *d, int n);
    void (*flush)(void *d);
    /* Non-zero once this back end can no longer draw (the Vulkan RDP: a shader the driver failed to compile during
     * the game); n64_rdp_select.c then goes on with the software RDP. NULL: never. */
    int (*failed)(void *d);
} n64_rdp_ops;

extern const n64_rdp_ops n64_rdp_sw_ops; /* n64_rdp_select.c, over n64_rdp_sw_* */
#ifdef N64_RDP_HAVE_VULKAN
extern const n64_rdp_ops n64_rdp_vk_ops; /* n64_rdp_vk.cpp */
#endif

#ifdef __cplusplus
}
#endif

#endif
