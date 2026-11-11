/* src/framegen/lsfg_bridge.cpp on a real Vulkan device (tests/lsfg_bridge/CMakeLists.txt).
 *
 *   lsfg_bridge_test            argument checks, then a moving square: frame A has a white square at x = 64, frame B
 *                               at x = 64 + 2d; the generated frame must hold it in between
 *   lsfg_bridge_test --bench [frames]   [LSFG] bench lines for 640x480, 1280x960, 1920x1080, full and performance
 *
 * Shaders: RECOMP_LSFG_DIR (a cache directory with translated or embedded SPIR-V), else RECOMP_LSFG_DLL (imported
 * into a temporary cache, translated when the test is built with the DXVK translator). Exit 77 (skipped) without a
 * Vulkan driver or without shaders. Prints sizes, timings and positions only. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "lsfg_bridge.h"
#include "lsfg_cache.h"
#ifdef LSFG_TEST_TRANSLATOR
#include "lsfg_dxbc.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long a_ = (long)(a), b_ = (long)(b); if (a_ != b_) { \
    fprintf(stderr, "FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #a, a_, b_); g_fail++; } } while (0)

static void test_no_device_needed(void)
{
    uint8_t px[16] = { 0 }, *out[1] = { px };
    CHECK_EQ(lsfg_bridge_generate(px, 16, out, 16), LSFG_E_STATE);
    LsfgConfig bad = { .width = 0, .height = 480, .multiplier = 2 };
    char err[256];
    CHECK_EQ(lsfg_bridge_open(&bad, err, sizeof err), LSFG_E_ARG);
    bad.width = 640;
    bad.multiplier = 9;
    CHECK_EQ(lsfg_bridge_open(&bad, err, sizeof err), LSFG_E_ARG);
    CHECK_EQ(lsfg_bridge_open(NULL, err, sizeof err), LSFG_E_ARG);
    lsfg_bridge_close();
    lsfg_bridge_close();   /* twice: harmless */
    CHECK(strcmp(lsfg_bridge_strerror(LSFG_E_MODULE), "shader module rejected") == 0);
}

/* The cache directory to use, or NULL (skip). */
static const char *shader_dir(char *buf, size_t cap)
{
    const char *dir = getenv("RECOMP_LSFG_DIR");
    if (dir && dir[0])
        return dir;
    const char *dll = getenv("RECOMP_LSFG_DLL");
    if (!dll || !dll[0])
        return NULL;
    char base[1024], err[512] = "";
    const char *t = getenv("TMPDIR");
    snprintf(base, sizeof base, "%s/lsfg_bridge_test.%d", t && *t ? t : "/tmp", (int)getpid());
    LsfgCacheInfo info;
#ifdef LSFG_TEST_TRANSLATOR
    int rc = lsfg_cache_import_path(dll, base, lsfg_dxbc_translator_name(), lsfg_dxbc_translate, NULL, &info, buf, cap,
                                    err, sizeof err);
#else
    int rc = lsfg_cache_import_path(dll, base, NULL, NULL, NULL, &info, buf, cap, err, sizeof err);
#endif
    if (rc) {
        fprintf(stderr, "RECOMP_LSFG_DLL: %s: %s\n", lsfg_cache_strerror(rc), err);
        return NULL;
    }
    printf("[LSFG] shader cache: LSFG 3.1 %u/%u translated, performance %u/%u translated\n", info.spv[0],
           info.needed[0], info.spv[1], info.needed[1]);
    return buf;
}

/* A dark frame with a w x w white square at x0 (BGRX). */
static void square(uint8_t *px, uint32_t w, uint32_t h, uint32_t x0, uint32_t s)
{
    memset(px, 0x10, (size_t)w * h * 4);
    for (uint32_t y = h / 2 - s / 2; y < h / 2 + s / 2; y++)
        for (uint32_t x = x0; x < x0 + s; x++)
            memset(px + ((size_t)y * w + x) * 4, 0xf0, 4);
}

/* Centroid x of the bright pixels on the middle row band, -1 if none. */
static double centroid(const uint8_t *px, uint32_t w, uint32_t h, uint32_t s)
{
    double sum = 0, n = 0;
    for (uint32_t y = h / 2 - s / 4; y < h / 2 + s / 4; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = px + ((size_t)y * w + x) * 4;
            double v = (p[0] + p[1] + p[2]) / 3.0 - 0x10;
            if (v > 0x40) {
                sum += x * v;
                n += v;
            }
        }
    return n > 0 ? sum / n : -1;
}

static int moving_square(const char *dir)
{
    const uint32_t w = 640, h = 480, s = 48, d = 24;
    LsfgConfig cfg = { .shader_dir = dir, .width = w, .height = h, .multiplier = 2, .flow_scale = 1.0f,
                       .fp16 = LSFG_FP16_AUTO };
    char err[512];
    int rc = lsfg_bridge_open(&cfg, err, sizeof err);
    if (rc == LSFG_E_NOLOADER || rc == LSFG_E_NODEVICE) {
        printf("lsfg_bridge: skipped: %s\n", err);
        return 77;
    }
    if (rc) {
        fprintf(stderr, "FAIL lsfg_bridge_open: %s: %s\n", lsfg_bridge_strerror(rc), err);
        return 1;
    }
    LsfgBridgeInfo info;
    CHECK_EQ(lsfg_bridge_get_info(&info), 0);
    printf("[LSFG] device %s; pipelines %.1f ms (pipeline cache %zu bytes in), device %.1f ms\n", info.device,
           info.pipeline_ms, info.pipeline_cache_bytes_in, info.device_ms);
    uint8_t *a = malloc((size_t)w * h * 4), *b = malloc((size_t)w * h * 4), *g = malloc((size_t)w * h * 4);
    uint8_t *out[1] = { g };
    double prev = -1;
    /* A few steps so the flow history settles; check every generated frame lies between its two inputs. */
    for (uint32_t i = 0; i < 6; i++) {
        uint32_t x = 64 + i * 2 * d;
        square(b, w, h, x, s);
        rc = lsfg_bridge_generate(b, w * 4, out, w * 4);
        if (i == 0) {
            CHECK_EQ(rc, LSFG_PRIMED);
        } else {
            CHECK_EQ(rc, LSFG_OK);
            double cg = centroid(g, w, h, s), ca = centroid(a, w, h, s), cb = centroid(b, w, h, s);
            printf("[LSFG] step %u: square centre %.1f -> generated %.1f -> %.1f (%u us)\n", i, ca, cg, cb,
                   lsfg_bridge_last_gen_us());
            CHECK(cg > ca + d / 4.0 && cg < cb - d / 4.0);
            prev = cg;
        }
        memcpy(a, b, (size_t)w * h * 4);
    }
    (void)prev;
    /* out == NULL only feeds the history; reset primes again. */
    CHECK_EQ(lsfg_bridge_generate(b, w * 4, NULL, w * 4), LSFG_PRIMED);
    lsfg_bridge_reset();
    CHECK_EQ(lsfg_bridge_generate(b, w * 4, out, w * 4), LSFG_PRIMED);
    CHECK_EQ(lsfg_bridge_generate(b, w * 4, out, w * 4), LSFG_OK);
    CHECK_EQ(lsfg_bridge_save_pipeline_cache(), 0);
    lsfg_bridge_close();
    /* Second open: the pipeline cache from the first is used. */
    rc = lsfg_bridge_open(&cfg, err, sizeof err);
    CHECK_EQ(rc, 0);
    if (rc == 0) {
        lsfg_bridge_get_info(&info);
        printf("[LSFG] reopen: pipelines %.1f ms (pipeline cache %zu bytes in)\n", info.pipeline_ms,
               info.pipeline_cache_bytes_in);
        CHECK(info.pipeline_cache_bytes_in > 0);
        lsfg_bridge_close();
    }
    free(a);
    free(b);
    free(g);
    return 0;
}

static int bench(const char *dir, uint32_t frames)
{
    static const uint32_t sizes[][2] = { { 640, 480 }, { 1280, 960 }, { 1920, 1080 } };
    for (int perf = 0; perf < 2; perf++)
        for (int i = 0; i < 3; i++) {
            LsfgConfig cfg = { .shader_dir = dir, .width = sizes[i][0], .height = sizes[i][1], .multiplier = 2,
                               .flow_scale = 1.0f, .performance = perf, .fp16 = LSFG_FP16_AUTO };
            LsfgBench b;
            char err[512];
            int rc = lsfg_bridge_bench(&cfg, frames, &b, err, sizeof err);
            if (rc == LSFG_E_NOLOADER || rc == LSFG_E_NODEVICE) {
                printf("lsfg_bridge: skipped: %s\n", err);
                return 77;
            }
            if (rc) {
                printf("[LSFG] bench %ux%u %s: %s: %s\n", sizes[i][0], sizes[i][1], perf ? "perf" : "full",
                       lsfg_bridge_strerror(rc), err);
                g_fail++;
                continue;
            }
            printf("[LSFG] bench %ux%u %s: generate %.2f ms (min %.2f, max %.2f), round trip %.2f ms, "
                   "pipelines %.0f ms (cache %zu bytes in)%s; %s\n", sizes[i][0], sizes[i][1], perf ? "perf" : "full",
                   b.gen_ms_avg, b.gen_ms_min, b.gen_ms_max, b.roundtrip_ms_avg, b.info.pipeline_ms,
                   b.info.pipeline_cache_bytes_in, b.info.fp16 ? ", fp16" : "", b.info.device);
        }
    return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_no_device_needed();
    if (g_fail)
        return 1;
    char probe[512];
    int rc = lsfg_bridge_probe(probe, sizeof probe);
    if (rc) {
        printf("lsfg_bridge: skipped: %s: %s\n", lsfg_bridge_strerror(rc), probe);
        return 77;
    }
    printf("[LSFG] probe: %s\n", probe);
    char buf[4096];
    const char *dir = shader_dir(buf, sizeof buf);
    if (!dir) {
        printf("lsfg_bridge: skipped: set RECOMP_LSFG_DIR (a shader cache) or RECOMP_LSFG_DLL (your Lossless.dll)\n");
        return 77;
    }
    if (argc > 1 && !strcmp(argv[1], "--bench"))
        rc = bench(dir, argc > 2 ? (uint32_t)atoi(argv[2]) : 60);
    else
        rc = moving_square(dir);
    if (rc == 0 && g_fail)
        rc = 1;
    printf("lsfg_bridge: %s\n", rc == 0 ? "ok" : rc == 77 ? "skipped" : "FAILED");
    return rc;
}
