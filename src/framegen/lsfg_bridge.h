/* The LSFG bridge: frame generation as a Vulkan compute sidecar (docs/technical/framegen.md, WP0/WP1).
 *
 * The GL back end keeps rendering and the SDL presenter keeps presenting. The bridge owns a small Vulkan device (no
 * surface, no swapchain) and runs framegen (the MIT library of the pinned lsfg-vk fork, GameNative/lsfg-vk-android
 * d33caceb) on it through initializeExternal + createContextFromImages: two input images used alternately, one
 * output image per generated frame, all R8G8B8A8_UNORM in GENERAL layout. Phase 1 is a CPU bridge: each call
 * uploads one guest frame, generates, and reads the generated frame(s) back.
 *
 * Vulkan is loaded at run time by volk (dlopen of libvulkan / MoltenVK): a binary built with the bridge still starts
 * on a machine without Vulkan, lsfg_bridge_open then fails with LSFG_E_NOLOADER. Shaders come from the user's shader
 * cache (lsfg_cache.h), never from this repository.
 *
 * Threading: every call from one thread (the pacer's framegen thread). Not reentrant: one bridge per process. Every
 * C++ exception is caught at this boundary and turned into an error code; the message is kept for
 * lsfg_bridge_last_error().
 */
#ifndef LSFG_BRIDGE_H
#define LSFG_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Results: 0 = generated, 1 = history primed (first frame after open / reset: no output), negative = error. */
#define LSFG_OK            0
#define LSFG_PRIMED        1
#define LSFG_E_NOLOADER   (-1)   /* no Vulkan loader / driver library could be loaded */
#define LSFG_E_NODEVICE   (-2)   /* no Vulkan 1.1 device with a compute queue */
#define LSFG_E_FEATURE    (-3)   /* a required feature or storage-image format is missing */
#define LSFG_E_SHADERS    (-4)   /* shader cache missing, incomplete or invalid */
#define LSFG_E_MODULE     (-5)   /* the driver rejected a shader module or pipeline */
#define LSFG_E_VULKAN     (-6)   /* another Vulkan call failed */
#define LSFG_E_DEVICE_LOST (-7)
#define LSFG_E_TIMEOUT    (-8)   /* generation did not finish within the bounded wait (2 s) */
#define LSFG_E_STATE      (-9)   /* not open, or already open */
#define LSFG_E_ARG        (-10)
#define LSFG_E_NOMEM      (-11)
#define LSFG_E_INTERNAL   (-12)  /* an unexpected exception */

/* fp16 */
#define LSFG_FP16_AUTO  (-1)     /* FP16 shaders when cached and the device has shaderFloat16, else full precision */
#define LSFG_FP16_OFF     0
#define LSFG_FP16_ON      1

typedef struct LsfgConfig {
    const char *shader_dir;  /* a cache directory (<base>/<key>, lsfg_cache.h); NULL = the current cache of
                              * lsfg_cache_default_base(). The VkPipelineCache is kept there too. */
    uint32_t width, height;  /* frame size */
    uint32_t multiplier;     /* 2 = one generated frame per real frame (2x), 3 = two, ... (2..4) */
    float flow_scale;        /* 0.25..1.0; 0 = 1.0 */
    int performance;         /* 1: LSFG 3.1 performance shader set */
    int fp16;                /* LSFG_FP16_* */
} LsfgConfig;

/* What open found and how long it took. */
typedef struct LsfgBridgeInfo {
    char device[256];        /* "<name> (Vulkan x.y.z, driver 0x..)" */
    uint32_t vendor_id, device_id, driver_version, api_version;
    int fp16;                /* the shaders in use are FP16 */
    unsigned shader_form;    /* LSFG_FORM_* of the shaders loaded (lsfg_cache.h) */
    double device_ms;        /* instance + device + images */
    double pipeline_ms;      /* createContextFromImages: shader modules + pipelines */
    size_t pipeline_cache_bytes_in;   /* VkPipelineCache data loaded at open (0 = cold) */
} LsfgBridgeInfo;

/* Creates the device, images and the framegen context. err (may be NULL) gets a one-line reason on failure.
 * Returns 0 or a negative LSFG_E_* code. */
int lsfg_bridge_open(const LsfgConfig *cfg, char *err, size_t errlen);

/* Uploads a frame (32 bpp, BGRX or RGBX as the guest has it: the channel order passes through unchanged), generates
 * the multiplier - 1 frames between the previous frame and this one, and reads them back into out[0..multiplier-2]
 * (same layout, out_pitch bytes per row), oldest first. Blocks until done (bounded). The first call after open or
 * lsfg_bridge_reset(), and any call with out == NULL, only records the frame as the history (the previous frame of
 * the next generation) and returns LSFG_PRIMED. 16 bpp guest frames are expanded by the caller (the pacer). */
int lsfg_bridge_generate(const uint8_t *frame, uint32_t pitch, uint8_t *const *out, uint32_t out_pitch);

/* Drops the frame history (pause/resume, scene reset): the next generate primes again. */
void lsfg_bridge_reset(void);

/* Writes the VkPipelineCache to the shader directory (also done by open after the pipelines are built, and by
 * close). Returns 0 or a negative code. */
int lsfg_bridge_save_pipeline_cache(void);

/* Destroys everything (safe when not open). */
void lsfg_bridge_close(void);

int lsfg_bridge_get_info(LsfgBridgeInfo *info);

/* Last generation's wall time from submit to readback done, in microseconds (0 before the first generation). */
uint32_t lsfg_bridge_last_gen_us(void);

/* Probes Vulkan without shaders: loader, device, the features framegen needs. desc gets a one-line description.
 * Returns 0 when frame generation could run, or LSFG_E_NOLOADER / NODEVICE / FEATURE. */
int lsfg_bridge_probe(char *desc, size_t desclen);

/* Benchmark: open with cfg, fill both inputs with synthetic frames on the GPU side (no upload), then time `frames`
 * generations (GPU work only: presentContext + a fence) and `frames` full round trips (upload + generate +
 * readback), then close. */
typedef struct LsfgBench {
    LsfgBridgeInfo info;
    uint32_t frames;
    double gen_ms_avg, gen_ms_min, gen_ms_max;
    double roundtrip_ms_avg;
} LsfgBench;
int lsfg_bridge_bench(const LsfgConfig *cfg, uint32_t frames, LsfgBench *out, char *err, size_t errlen);

const char *lsfg_bridge_strerror(int err);
const char *lsfg_bridge_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* LSFG_BRIDGE_H */
