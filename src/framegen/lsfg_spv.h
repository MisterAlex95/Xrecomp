/* LSFG shader table and SPIR-V binding renumbering (docs/technical/framegen.md).
 *
 * framegen (the MIT library of the pinned lsfg-vk fork) asks for each shader by name ("alpha[0]", "p_generate", ...)
 * and builds its descriptor set layout itself: framegen/src/core/shadermodule.cpp numbers the bindings 0..N-1 over
 * the pass's declared type list, in the order uniform buffers, samplers, sampled images, storage images
 * (framegen/v3.1_src/shaders/ *.cpp and v3.1p_src: the counts are in lsfg_shader_table() below). A shader compiled
 * from HLSL carries its register slots instead (b#, s#, t#, u#, or the per-class binding ranges DXVK's DXBC compiler
 * gives them). lsfg_spv_renumber() rewrites every Binding decoration to framegen's order: resources are classified
 * by their SPIR-V type, sorted by their original (set, binding) within each class, and numbered class after class;
 * DescriptorSet becomes 0. The counts found are checked against the pass's layout, so a shader that does not match
 * framegen's contract is refused before Vulkan sees it.
 *
 * Resource IDs in Lossless.dll (fixed table, the same one lsfg-vk 1.0 used): DXBC at the base ID (255..302); some
 * builds reportedly also carry SPIR-V at base + 49 (FP16) and base + 98 (FP32). libc only.
 */
#ifndef LSFG_SPV_H
#define LSFG_SPV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSFG_SPV_MAGIC 0x07230203u

/* Error codes: negative, 0 is success. */
#define LSFG_SPV_ENOTSPV    (-1)   /* no SPIR-V magic, size not a multiple of 4, or shorter than the header */
#define LSFG_SPV_ECORRUPT   (-2)   /* an instruction runs past the end, a zero word count, an id out of bounds */
#define LSFG_SPV_EUNSUPPORTED (-3) /* a resource framegen cannot bind: arrays, combined image samplers, storage
                                    * buffers, texel buffers, a binding without a type we know */
#define LSFG_SPV_ELAYOUT    (-4)   /* the resource counts differ from the pass's layout */
#define LSFG_SPV_EINVAL     (-5)

/* Descriptor counts of one pass, in framegen's order. */
typedef struct LsfgSpvLayout {
    uint8_t uniform_buffers, samplers, sampled_images, storage_images;
} LsfgSpvLayout;

/* One entry of the shader table: the name framegen asks for, the DLL resource ID and the pass layout. */
typedef struct LsfgShaderInfo {
    const char *name;        /* "mipmaps", "alpha[0]", ..., "p_generate" */
    uint16_t base_id;        /* DXBC resource ID in Lossless.dll */
    uint8_t performance;     /* 0: LSFG 3.1, 1: 3.1 performance ("p_" names) */
    LsfgSpvLayout layout;
} LsfgShaderInfo;

#define LSFG_SPV_FP16_OFFSET 49u   /* embedded SPIR-V (FP16) at base_id + 49, when the DLL has it */
#define LSFG_SPV_FP32_OFFSET 98u   /* embedded SPIR-V (FP32) at base_id + 98 */

/* The 52 names framegen requests (26 per variant; gamma[0]/delta[0] share a resource, as do the variants' mipmaps
 * and generate). *count gets the number of entries. */
const LsfgShaderInfo *lsfg_shader_table(size_t *count);

/* The entry for a name, or NULL. */
const LsfgShaderInfo *lsfg_shader_find(const char *name);

/* The distinct base IDs one variant needs (25 each: 255, 256 and 23 own IDs), ascending. Returns the count written
 * (at most cap). performance: 0 or 1. */
size_t lsfg_shader_ids(int performance, uint16_t *ids, size_t cap);

/* The layout a base ID is used with (the same for every name mapping to it), or NULL for an unknown ID. */
const LsfgSpvLayout *lsfg_shader_layout_for_id(uint32_t base_id);

/* 1 when data[0..len) starts like a SPIR-V module (little-endian magic, len a multiple of 4, >= 20 bytes). */
int lsfg_spv_is_spirv(const void *data, size_t len);

/* 1 when data[0..len) starts with the DXBC container magic. */
int lsfg_spv_is_dxbc(const void *data, size_t len);

/* Counts the descriptor resources of a module without changing it. */
int lsfg_spv_inspect(const uint32_t *words, size_t nwords, LsfgSpvLayout *found);

/* Renumbers the Binding decorations of words[0..nwords) in place (see above). expect may be NULL (no layout check);
 * found (may be NULL) gets the counts seen. Returns 0 or a negative LSFG_SPV_* code; on error the words may be
 * partly rewritten only for LSFG_SPV_ELAYOUT (the renumbering itself succeeded). */
int lsfg_spv_renumber(uint32_t *words, size_t nwords, const LsfgSpvLayout *expect, LsfgSpvLayout *found);

/* What a module asks of the device: its SPIR-V version (0x00010600 = 1.6, which needs Vulkan 1.3; 1.5 needs 1.2;
 * 1.3 needs 1.1) and the capabilities below. Shaders translated by DXVK 2.6 are SPIR-V 1.6 with VulkanMemoryModel and
 * StorageImageWriteWithoutFormat. */
#define LSFG_SPV_CAP_VULKAN_MEMORY_MODEL   0x01u
#define LSFG_SPV_CAP_READ_WITHOUT_FORMAT   0x02u
#define LSFG_SPV_CAP_WRITE_WITHOUT_FORMAT  0x04u
#define LSFG_SPV_CAP_EXTENDED_FORMATS      0x08u
#define LSFG_SPV_CAP_FLOAT16               0x10u
#define LSFG_SPV_CAP_INT16                 0x20u
#define LSFG_SPV_CAP_OTHER                 0x80u   /* a capability beyond Shader and the above */
int lsfg_spv_requirements(const uint32_t *words, size_t nwords, uint32_t *version, uint32_t *caps);

const char *lsfg_spv_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* LSFG_SPV_H */
