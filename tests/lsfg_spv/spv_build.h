/* Hand-assembled synthetic SPIR-V compute modules for the LSFG tests (tests/lsfg_spv, tests/lsfg_dll). Our own
 * minimal modules: a GLCompute entry point that does nothing, plus the resource variables a test asks for. No
 * shader from any DLL. */
#ifndef LSFG_TEST_SPV_BUILD_H
#define LSFG_TEST_SPV_BUILD_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { SB_UBO, SB_SAMPLER, SB_SAMPLED, SB_STORAGE, SB_SAMPLED_ARRAY, SB_COMBINED, SB_SSBO, SB_TEXEL_BUFFER };

typedef struct SbRes {
    int kind;
    uint32_t set, binding;
    int no_set;          /* omit the DescriptorSet decoration */
} SbRes;

typedef struct SbModule {
    uint32_t w[4096];
    size_t n;
    uint32_t bound;
} SbModule;

static inline void sb_op(SbModule *m, uint32_t op, const uint32_t *args, uint32_t nargs)
{
    m->w[m->n++] = ((nargs + 1) << 16) | op;
    for (uint32_t i = 0; i < nargs; i++)
        m->w[m->n++] = args[i];
}
#define SB(m, op, ...) do { const uint32_t a_[] = { __VA_ARGS__ }; sb_op((m), (op), a_, sizeof a_ / 4); } while (0)

/* Builds a module with the resources res[0..nres) declared in that order. Returns the word count. */
static inline size_t sb_build(SbModule *m, const SbRes *res, size_t nres)
{
    memset(m, 0, sizeof *m);
    uint32_t next = 1;
    uint32_t id_main = next++, id_void = next++, id_fn = next++, id_float = next++, id_uint = next++;
    uint32_t id_sampler = next++, id_img = next++, id_simg = next++, id_struct = next++, id_arr = next++;
    uint32_t id_comb = next++, id_buf_img = next++, id_ssbo_struct = next++, id_label = next++, id_len = next++;
    uint32_t p_sampler = next++, p_img = next++, p_simg = next++, p_struct = next++, p_arr = next++, p_comb = next++,
             p_buf_img = next++, p_ssbo = next++;
    uint32_t var0 = next;
    next += (uint32_t)nres;
    m->n = 5;
    SB(m, 17, 1);                                  /* OpCapability Shader */
    SB(m, 14, 0, 1);                               /* OpMemoryModel Logical GLSL450 */
    SB(m, 15, 5, id_main, 0x6e69616d, 0);          /* OpEntryPoint GLCompute %main "main" */
    SB(m, 16, id_main, 17, 8, 8, 1);               /* OpExecutionMode LocalSize 8 8 1 */
    for (size_t i = 0; i < nres; i++) {
        if (!res[i].no_set)
            SB(m, 71, var0 + (uint32_t)i, 34, res[i].set);   /* DescriptorSet */
        SB(m, 71, var0 + (uint32_t)i, 33, res[i].binding);   /* Binding */
    }
    SB(m, 71, id_struct, 2);                       /* Block */
    SB(m, 72, id_struct, 0, 35, 0);                /* OpMemberDecorate Offset 0 */
    SB(m, 71, id_ssbo_struct, 2);
    SB(m, 72, id_ssbo_struct, 0, 35, 0);
    SB(m, 19, id_void);
    SB(m, 33, id_fn, id_void);
    SB(m, 22, id_float, 32);
    SB(m, 21, id_uint, 32, 0);
    SB(m, 43, id_uint, id_len, 4);                 /* OpConstant uint 4 */
    SB(m, 26, id_sampler);
    SB(m, 25, id_img, id_float, 1, 0, 0, 0, 1, 0);         /* sampled 2D */
    SB(m, 25, id_simg, id_float, 1, 0, 0, 0, 2, 4);        /* storage 2D rgba8 */
    SB(m, 25, id_buf_img, id_float, 5, 0, 0, 0, 1, 0);     /* sampled buffer */
    SB(m, 27, id_comb, id_img);
    SB(m, 30, id_struct, id_float);
    SB(m, 30, id_ssbo_struct, id_float);
    SB(m, 28, id_arr, id_img, id_len);
    SB(m, 32, p_sampler, 0, id_sampler);
    SB(m, 32, p_img, 0, id_img);
    SB(m, 32, p_simg, 0, id_simg);
    SB(m, 32, p_struct, 2, id_struct);
    SB(m, 32, p_arr, 0, id_arr);
    SB(m, 32, p_comb, 0, id_comb);
    SB(m, 32, p_buf_img, 0, id_buf_img);
    SB(m, 32, p_ssbo, 12, id_ssbo_struct);
    for (size_t i = 0; i < nres; i++) {
        uint32_t ptr = 0, sc = 0;
        switch (res[i].kind) {
        case SB_UBO: ptr = p_struct; sc = 2; break;
        case SB_SAMPLER: ptr = p_sampler; break;
        case SB_SAMPLED: ptr = p_img; break;
        case SB_STORAGE: ptr = p_simg; break;
        case SB_SAMPLED_ARRAY: ptr = p_arr; break;
        case SB_COMBINED: ptr = p_comb; break;
        case SB_SSBO: ptr = p_ssbo; sc = 12; break;
        case SB_TEXEL_BUFFER: ptr = p_buf_img; break;
        }
        SB(m, 59, ptr, var0 + (uint32_t)i, sc);
    }
    SB(m, 54, id_void, id_main, 0, id_fn);         /* OpFunction */
    SB(m, 248, id_label);
    SB(m, 253);
    SB(m, 56);
    m->bound = next;
    m->w[0] = 0x07230203u;
    m->w[1] = 0x00010000u;
    m->w[2] = 0;
    m->w[3] = next;
    m->w[4] = 0;
    return m->n;
}

/* Word index of the Binding literal of resource i (0-based, in declaration order). */
static inline size_t sb_binding_at(const SbModule *m, uint32_t var_id)
{
    for (size_t at = 5; at < m->n; at += m->w[at] >> 16)
        if ((m->w[at] & 0xffff) == 71 && m->w[at + 1] == var_id && m->w[at + 2] == 33)
            return at + 3;
    return 0;
}

/* The id of resource i (the variables are numbered after the fixed ids). */
static inline uint32_t sb_var_id(const SbModule *m, size_t nres, size_t i)
{
    return m->bound - (uint32_t)nres + (uint32_t)i;
}

/* A module with HLSL-style per-class bindings for a layout: cbuffers b0.., samplers s0.. (DXVK-like class offsets:
 * ubo 0, samplers 14, SRVs 30, UAVs 100), declared in a scrambled order. */
static inline size_t sb_build_layout(SbModule *m, unsigned ubo, unsigned samp, unsigned sampled, unsigned storage)
{
    SbRes r[128];
    size_t n = 0;
    for (unsigned i = storage; i-- > 0;) r[n++] = (SbRes){ SB_STORAGE, 0, 100 + i, 0 };
    for (unsigned i = 0; i < sampled; i++) r[n++] = (SbRes){ SB_SAMPLED, 0, 30 + i, 0 };
    for (unsigned i = 0; i < ubo; i++) r[n++] = (SbRes){ SB_UBO, 0, 0 + i, 0 };
    for (unsigned i = samp; i-- > 0;) r[n++] = (SbRes){ SB_SAMPLER, 0, 14 + i, 0 };
    return sb_build(m, r, n);
}

#endif
