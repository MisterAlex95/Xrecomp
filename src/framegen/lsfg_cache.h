/* The LSFG shader cache: shaders taken from the user's own Lossless.dll, in the user's private storage
 * (docs/technical/framegen.md, "Shader cache").
 *
 *   <base>/current             the key of the active cache (one line)
 *   <base>/<key>/manifest.txt  what the directory holds (format below)
 *   <base>/<key>/<id>.dxbc     the DXBC blob of resource <id> (255..302), kept so a translator can run later
 *                              without the DLL (Android never keeps the DLL)
 *   <base>/<key>/<id>.fp16.spv embedded SPIR-V found at <id> + 49 (FP16), when the DLL has it
 *   <base>/<key>/<id>.fp32.spv embedded SPIR-V found at <id> + 98 (FP32), when the DLL has it
 *   <base>/<key>/<id>.spv      SPIR-V translated from <id>.dxbc by a translator (lsfg_translate_fn)
 *   <base>/<key>/pipeline-*.bin  the bridge's VkPipelineCache (lsfg_bridge.h)
 *
 * <key> is the first 16 hex digits of the DLL's SHA-256. <base> is the app's getFilesDir()/lsfg on Android, and on
 * a desktop $RECOMP_LSFG_CACHE, else ~/Library/Caches/xboxrecomp/lsfg (macOS) or $XDG_CACHE_HOME/xboxrecomp/lsfg
 * (~/.cache/...). Nothing in it may ever be committed, bundled into an APK, uploaded by CI or written to a log.
 *
 * Manifest (text, one "key value..." per line, unknown keys ignored):
 *   lsfg-shader-cache 1
 *   extractor <LSFG_CACHE_EXTRACTOR>
 *   dll_sha256 <64 hex>
 *   dll_size <bytes>
 *   rcdata <number of RT_RCDATA resources in the DLL>
 *   dxbc <ids...>      ids with a .dxbc file
 *   fp16 <ids...>      ids with a .fp16.spv file
 *   fp32 <ids...>      ids with a .fp32.spv file
 *   spv <ids...>       ids with a translated .spv file
 *   translator <name>  who made the .spv files (free text, one word)
 *
 * Translator contract (lsfg_translate_fn): input one DXBC container (a compute shader, SM 5.0); output one SPIR-V
 * module (little-endian, magic 0x07230203) with a GLCompute entry point named "main", every resource a plain
 * (non-array) variable with Binding/DescriptorSet decorations in HLSL register order within each class (cbuffers,
 * samplers, SRV textures, UAV textures: the per-class ranges DXVK's compiler emits qualify), no combined image
 * samplers, no storage buffers, no push constants. lsfg_spv_renumber() then maps the bindings to framegen's layout;
 * the cache refuses a module whose resource counts differ from the pass layout (lsfg_spv.h).
 *
 * libc only. Thread-safety: no global state; two imports into the same <base> at once are not supported.
 */
#ifndef LSFG_CACHE_H
#define LSFG_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSFG_CACHE_EXTRACTOR 1   /* bump when what the extractor writes changes */

/* Error codes: negative, 0 is success. Values below -100 are LSFG_DLL_* codes - 100 (lsfg_cache_strerror knows). */
#define LSFG_CACHE_EIO          (-1)   /* creating, writing or renaming a cache file failed */
#define LSFG_CACHE_ENOMANIFEST  (-2)   /* no manifest, or one this version cannot read */
#define LSFG_CACHE_ENOSHADER    (-3)   /* the shader is in none of the cached forms */
#define LSFG_CACHE_EBADSHADER   (-4)   /* a cached shader failed the SPIR-V / layout check */
#define LSFG_CACHE_EUNSUPPORTED (-5)   /* the DLL has none of the LSFG 3.1 shaders: not a supported Lossless.dll */
#define LSFG_CACHE_ENOMEM       (-6)
#define LSFG_CACHE_EINVAL       (-7)
#define LSFG_CACHE_ETRANSLATE   (-8)   /* the translator failed on at least one shader (err says which) */
#define LSFG_CACHE_DLL_BASE     (-100)

/* Per variant (0: LSFG 3.1, 1: 3.1 performance), how many of its 25 resource IDs are cached in each form. */
typedef struct LsfgCacheInfo {
    char key[17];
    char dll_sha256[65];
    uint64_t dll_size;
    uint32_t rcdata;
    uint32_t extractor;
    uint32_t needed[2];              /* 25 and 25 */
    uint32_t dxbc[2], fp16[2], fp32[2], spv[2];
    char translator[32];
} LsfgCacheInfo;

/* Ready means every ID of the variant is there in that form. */
int lsfg_cache_variant_ready(const LsfgCacheInfo *info, int performance, int *has_fp16, int *has_fp32, int *has_spv);

/* Converts a DXBC blob to SPIR-V (see the contract above). *spv is malloc'd by the translator and freed by the
 * caller with free(). Returns 0 or nonzero (err gets a short reason). */
typedef int (*lsfg_translate_fn)(const uint8_t *dxbc, size_t len, uint32_t base_id, uint8_t **spv, size_t *spv_len,
                                 char *err, size_t errlen, void *user);

/* Extracts the LSFG shaders of a DLL held in memory into <base>/<key> (through <key>.partial, then renamed), points
 * <base>/current at it, and runs the translator (may be NULL) over the DXBC blobs. A directory already holding this
 * DLL's shaders from the same extractor version is reused (only the missing translations run). dir_out (may be
 * NULL) gets <base>/<key>. Returns 0, LSFG_CACHE_EUNSUPPORTED when no LSFG 3.1 shader is present at all, or another
 * negative code; info (may be NULL) is filled whenever the manifest was written. */
int lsfg_cache_import(const uint8_t *dll, size_t len, const char *base, const char *translator_name,
                      lsfg_translate_fn tr, void *user, LsfgCacheInfo *info, char *dir_out, size_t dir_cap,
                      char *err, size_t errlen);

/* Same, reading the DLL from an open fd (the Android picker's ParcelFileDescriptor) or a path. */
int lsfg_cache_import_fd(int fd, const char *base, const char *translator_name, lsfg_translate_fn tr, void *user,
                         LsfgCacheInfo *info, char *dir_out, size_t dir_cap, char *err, size_t errlen);
int lsfg_cache_import_path(const char *dll_path, const char *base, const char *translator_name, lsfg_translate_fn tr,
                           void *user, LsfgCacheInfo *info, char *dir_out, size_t dir_cap, char *err, size_t errlen);

/* Runs the translator over the .dxbc files of an existing cache directory that have no .spv yet, and rewrites the
 * manifest. Returns 0 or a negative code (LSFG_CACHE_ETRANSLATE when some shader failed; the others are kept). */
int lsfg_cache_translate(const char *dir, const char *translator_name, lsfg_translate_fn tr, void *user,
                         LsfgCacheInfo *info, char *err, size_t errlen);

int lsfg_cache_read_manifest(const char *dir, LsfgCacheInfo *info);

/* <base> for a desktop (see above). Returns 0 or LSFG_CACHE_EINVAL (no HOME, or too long). */
int lsfg_cache_default_base(char *out, size_t cap);

/* <base>/<contents of <base>/current>. Returns 0 or LSFG_CACHE_ENOMANIFEST. */
int lsfg_cache_current(const char *base, char *dir_out, size_t cap);

/* Shader forms, for lsfg_cache_load_shader. */
#define LSFG_FORM_FP16 1u   /* embedded FP16 SPIR-V (needs shaderFloat16) */
#define LSFG_FORM_FP32 2u   /* embedded FP32 SPIR-V */
#define LSFG_FORM_SPV  4u   /* translated from DXBC */

/* Loads the shader framegen asks for by name ("alpha[0]", "p_generate", ...) from a cache directory, renumbered to
 * framegen's layout and checked against it. allowed: LSFG_FORM_* bits; prefer_fp16 tries FP16 first, otherwise the
 * order is translated, FP32, FP16. *spv is malloc'd (free()); *form_out (may be NULL) gets the form used. */
int lsfg_cache_load_shader(const char *dir, const char *name, unsigned allowed, int prefer_fp16, uint8_t **spv,
                           size_t *len, unsigned *form_out, char *err, size_t errlen);

/* SHA-256 (FIPS 180-4). */
void lsfg_sha256(const void *data, size_t len, uint8_t out[32]);

const char *lsfg_cache_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* LSFG_CACHE_H */
