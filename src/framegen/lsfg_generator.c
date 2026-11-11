/* The LSFG bridge as the pacer's generator (framegen_pacer.h FramegenGenerator, docs/technical/framegen.md).
 *
 * Configuration from the environment, read at each open():
 *   RECOMP_LSFG_DIR=<dir>   shader cache directory (<base>/<key>, lsfg_cache.h); the Android app passes
 *                           getFilesDir()/lsfg/<key>
 *   RECOMP_LSFG_DLL=<path>  desktop: import this Lossless.dll into the default cache base first (lsfg_cache_import;
 *                           DXBC needs tools/lsfg/lsfg_translate once), used when RECOMP_LSFG_DIR is unset
 *   RECOMP_LSFG_FLOW=0.25..1 flow scale (default 1)
 *   RECOMP_LSFG_PERF=1      LSFG 3.1 performance shader set
 *   RECOMP_LSFG_FP16=auto|0|1
 * Neither set: the current cache of the default base. */
#include "lsfg_bridge.h"
#include "lsfg_cache.h"
#include "lsfg_generator.h"
#include "framegen_pacer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char s_dir[4096];
static uint32_t s_outputs;   /* multiplier - 1 */

static int gen_open(void *user, uint32_t width, uint32_t height, uint32_t multiplier, char *err, size_t errlen)
{
    (void)user;
    const char *dir = getenv("RECOMP_LSFG_DIR");
    const char *dll = getenv("RECOMP_LSFG_DLL");
    s_dir[0] = '\0';
    if (dir && dir[0]) {
        snprintf(s_dir, sizeof s_dir, "%s", dir);
    } else if (dll && dll[0]) {
        char base[2048], why[512] = "";
        LsfgCacheInfo info;
        int rc = lsfg_cache_default_base(base, sizeof base);
        if (rc == 0)
            rc = lsfg_cache_import_path(dll, base, NULL, NULL, NULL, &info, s_dir, sizeof s_dir, why, sizeof why);
        if (rc) {
            snprintf(err, errlen, "RECOMP_LSFG_DLL: %s", why[0] ? why : lsfg_cache_strerror(rc));
            return LSFG_E_SHADERS;
        }
        fprintf(stderr, "[LSFG] shader cache %s: LSFG 3.1 %u/%u DXBC %u translated, performance %u/%u DXBC %u translated\n",
                s_dir, info.dxbc[0], info.needed[0], info.spv[0], info.dxbc[1], info.needed[1], info.spv[1]);
    }
    LsfgConfig cfg = {
        .shader_dir = s_dir[0] ? s_dir : NULL,
        .width = width,
        .height = height,
        .multiplier = multiplier,
        .flow_scale = 1.0f,
        .performance = 0,
        .fp16 = LSFG_FP16_AUTO,
    };
    const char *v = getenv("RECOMP_LSFG_FLOW");
    if (v && v[0]) {
        float f = strtof(v, NULL);
        cfg.flow_scale = f < 0.25f ? 0.25f : f > 1.0f ? 1.0f : f;
    }
    v = getenv("RECOMP_LSFG_PERF");
    cfg.performance = v && v[0] == '1';
    v = getenv("RECOMP_LSFG_FP16");
    if (v && (v[0] == '0' || v[0] == '1'))
        cfg.fp16 = v[0] - '0';
    int rc = lsfg_bridge_open(&cfg, err, errlen);
    if (rc)
        return rc;
    s_outputs = multiplier - 1;
    LsfgBridgeInfo bi;
    if (lsfg_bridge_get_info(&bi) == 0)
        fprintf(stderr, "[LSFG] device %s; %ux%u %ux%s%s, pipelines %.0f ms (cache %zu bytes in), device %.0f ms\n",
                bi.device, width, height, multiplier, cfg.performance ? " performance" : "", bi.fp16 ? " fp16" : "",
                bi.pipeline_ms, bi.pipeline_cache_bytes_in, bi.device_ms);
    return 0;
}

static int gen_generate(void *user, const uint8_t *frame, uint32_t pitch, uint8_t *const *out, uint32_t out_pitch)
{
    (void)user;
    int rc = lsfg_bridge_generate(frame, pitch, out, out_pitch);
    if (rc == LSFG_PRIMED)
        return 0;
    if (rc < 0)
        return rc;
    return out ? (int)s_outputs : 0;
}

static void gen_reset(void *user)
{
    (void)user;
    lsfg_bridge_reset();
}

static void gen_close(void *user)
{
    (void)user;
    lsfg_bridge_close();
}

const FramegenGenerator lsfg_bridge_generator = {
    .name = "lsfg",
    .user = NULL,
    .open = gen_open,
    .generate = gen_generate,
    .reset = gen_reset,
    .close = gen_close,
};

void lsfg_bridge_register(void)
{
    framegen_pacer_set_generator(&lsfg_bridge_generator);
}
