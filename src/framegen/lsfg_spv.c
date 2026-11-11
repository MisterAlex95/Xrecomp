/* LSFG shader table and SPIR-V binding renumbering (lsfg_spv.h). Written from the SPIR-V specification; libc only.
 *
 * The table below states facts about the pinned framegen library (MIT, GameNative/lsfg-vk-android d33caceb): the
 * shader names it requests, the Lossless.dll resource ID behind each name (the table lsfg-vk 1.0 used), and the
 * descriptor counts each pass declares (framegen/v3.1_src/shaders/ *.cpp, framegen/v3.1p_src/shaders/ *.cpp). */
#include "lsfg_spv.h"

#include <stdlib.h>
#include <string.h>

static const LsfgShaderInfo k_table[] = {
    { "mipmaps", 255, 0, { 1, 1, 1, 7 } },
    { "alpha[0]", 267, 0, { 0, 1, 1, 2 } },
    { "alpha[1]", 268, 0, { 0, 1, 2, 2 } },
    { "alpha[2]", 269, 0, { 0, 1, 2, 4 } },
    { "alpha[3]", 270, 0, { 0, 1, 4, 4 } },
    { "beta[0]", 275, 0, { 0, 1, 12, 2 } },
    { "beta[1]", 276, 0, { 0, 1, 2, 2 } },
    { "beta[2]", 277, 0, { 0, 1, 2, 2 } },
    { "beta[3]", 278, 0, { 0, 1, 2, 2 } },
    { "beta[4]", 279, 0, { 1, 1, 2, 6 } },
    { "gamma[0]", 257, 0, { 1, 2, 9, 3 } },
    { "gamma[1]", 259, 0, { 0, 1, 3, 4 } },
    { "gamma[2]", 260, 0, { 0, 1, 4, 4 } },
    { "gamma[3]", 261, 0, { 0, 1, 4, 4 } },
    { "gamma[4]", 262, 0, { 1, 2, 6, 1 } },
    { "delta[0]", 257, 0, { 1, 2, 9, 3 } },
    { "delta[1]", 263, 0, { 0, 1, 3, 4 } },
    { "delta[2]", 264, 0, { 0, 1, 4, 4 } },
    { "delta[3]", 265, 0, { 0, 1, 4, 4 } },
    { "delta[4]", 266, 0, { 1, 2, 6, 1 } },
    { "delta[5]", 258, 0, { 1, 2, 10, 2 } },
    { "delta[6]", 271, 0, { 0, 1, 2, 2 } },
    { "delta[7]", 272, 0, { 0, 1, 2, 2 } },
    { "delta[8]", 273, 0, { 0, 1, 2, 2 } },
    { "delta[9]", 274, 0, { 1, 2, 3, 1 } },
    { "generate", 256, 0, { 1, 2, 5, 1 } },
    { "p_mipmaps", 255, 1, { 1, 1, 1, 7 } },
    { "p_alpha[0]", 290, 1, { 0, 1, 1, 1 } },
    { "p_alpha[1]", 291, 1, { 0, 1, 1, 1 } },
    { "p_alpha[2]", 292, 1, { 0, 1, 1, 2 } },
    { "p_alpha[3]", 293, 1, { 0, 1, 2, 2 } },
    { "p_beta[0]", 298, 1, { 0, 1, 6, 2 } },
    { "p_beta[1]", 299, 1, { 0, 1, 2, 2 } },
    { "p_beta[2]", 300, 1, { 0, 1, 2, 2 } },
    { "p_beta[3]", 301, 1, { 0, 1, 2, 2 } },
    { "p_beta[4]", 302, 1, { 1, 1, 2, 6 } },
    { "p_gamma[0]", 280, 1, { 1, 2, 5, 3 } },
    { "p_gamma[1]", 282, 1, { 0, 1, 3, 2 } },
    { "p_gamma[2]", 283, 1, { 0, 1, 2, 2 } },
    { "p_gamma[3]", 284, 1, { 0, 1, 2, 2 } },
    { "p_gamma[4]", 285, 1, { 1, 2, 4, 1 } },
    { "p_delta[0]", 280, 1, { 1, 2, 5, 3 } },
    { "p_delta[1]", 286, 1, { 0, 1, 3, 2 } },
    { "p_delta[2]", 287, 1, { 0, 1, 2, 2 } },
    { "p_delta[3]", 288, 1, { 0, 1, 2, 2 } },
    { "p_delta[4]", 289, 1, { 1, 2, 4, 1 } },
    { "p_delta[5]", 281, 1, { 1, 2, 6, 1 } },
    { "p_delta[6]", 294, 1, { 0, 1, 1, 1 } },
    { "p_delta[7]", 295, 1, { 0, 1, 1, 1 } },
    { "p_delta[8]", 296, 1, { 0, 1, 1, 1 } },
    { "p_delta[9]", 297, 1, { 1, 2, 2, 1 } },
    { "p_generate", 256, 1, { 1, 2, 5, 1 } },
};

#define TABLE_N (sizeof k_table / sizeof k_table[0])

const LsfgShaderInfo *lsfg_shader_table(size_t *count)
{
    if (count)
        *count = TABLE_N;
    return k_table;
}

const LsfgShaderInfo *lsfg_shader_find(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < TABLE_N; i++)
        if (strcmp(k_table[i].name, name) == 0)
            return &k_table[i];
    return NULL;
}

size_t lsfg_shader_ids(int performance, uint16_t *ids, size_t cap)
{
    size_t n = 0;
    for (uint32_t id = 0; id < 0x10000; id++) {
        int used = 0;
        for (size_t i = 0; i < TABLE_N && !used; i++)
            used = k_table[i].base_id == id && k_table[i].performance == (performance ? 1 : 0);
        if (!used)
            continue;
        if (n < cap && ids)
            ids[n] = (uint16_t)id;
        n++;
    }
    return n < cap ? n : cap;
}

const LsfgSpvLayout *lsfg_shader_layout_for_id(uint32_t base_id)
{
    for (size_t i = 0; i < TABLE_N; i++)
        if (k_table[i].base_id == base_id)
            return &k_table[i].layout;
    return NULL;
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int lsfg_spv_is_spirv(const void *data, size_t len)
{
    return data && len >= 20 && len % 4 == 0 && rd32le(data) == LSFG_SPV_MAGIC;
}

int lsfg_spv_is_dxbc(const void *data, size_t len)
{
    return data && len >= 32 && memcmp(data, "DXBC", 4) == 0;
}

/* SPIR-V opcodes, decorations and storage classes used here. */
enum {
    OP_DECORATE = 71, OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28,
    OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59,
    DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_DESCRIPTOR_SET = 34,
    SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12,
    DIM_BUFFER = 5, DIM_SUBPASS = 6,
};

#define MAX_BOUND (1u << 22)
#define MAX_RESOURCES 256u

/* Per id: what the module declares about it. */
enum { K_NONE, K_IMAGE, K_SAMPLER, K_SAMPLED_IMAGE, K_ARRAY, K_STRUCT, K_POINTER, K_VARIABLE };
typedef struct IdInfo {
    uint8_t kind;
    uint8_t block;          /* struct decorated Block (1) or BufferBlock (2) */
    uint8_t image_sampled;  /* OpTypeImage Sampled operand */
    uint8_t image_dim;
    uint32_t storage;       /* pointer / variable storage class */
    uint32_t type;          /* pointer pointee / variable pointer type */
    uint32_t binding_at;    /* word index of the Binding literal, 0 = none */
    uint32_t set_at;        /* word index of the DescriptorSet literal, 0 = none */
} IdInfo;

/* Classes in framegen's order. */
enum { C_UBO, C_SAMPLER, C_SAMPLED, C_STORAGE, C_COUNT };

typedef struct Res {
    uint32_t cls, set, binding, id, binding_at, set_at;
} Res;

static int res_cmp(const void *a, const void *b)
{
    const Res *x = a, *y = b;
    if (x->cls != y->cls) return x->cls < y->cls ? -1 : 1;
    if (x->set != y->set) return x->set < y->set ? -1 : 1;
    if (x->binding != y->binding) return x->binding < y->binding ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

static int collect(const uint32_t *words, size_t nwords, Res *res, size_t *nres)
{
    if (!words || nwords < 5 || words[0] != LSFG_SPV_MAGIC)
        return LSFG_SPV_ENOTSPV;
    uint32_t bound = words[3];
    if (bound == 0 || bound > MAX_BOUND)
        return LSFG_SPV_ECORRUPT;
    IdInfo *ids = calloc(bound, sizeof *ids);
    if (!ids)
        return LSFG_SPV_ECORRUPT;
    int rc = 0;
    for (size_t at = 5; at < nwords;) {
        uint32_t wc = words[at] >> 16, op = words[at] & 0xffffu;
        if (wc == 0 || wc > nwords - at) {
            rc = LSFG_SPV_ECORRUPT;
            goto out;
        }
        const uint32_t *w = words + at;
        uint32_t target = wc > 1 ? w[1] : 0;
#define NEED(n, idx) do { if (wc < (n) || (idx) >= bound) { rc = LSFG_SPV_ECORRUPT; goto out; } } while (0)
        switch (op) {
        case OP_DECORATE:
            NEED(3, target);
            if (w[2] == DEC_BINDING || w[2] == DEC_DESCRIPTOR_SET) {
                NEED(4, target);
                if (w[2] == DEC_BINDING) ids[target].binding_at = (uint32_t)(at + 3);
                else ids[target].set_at = (uint32_t)(at + 3);
            } else if (w[2] == DEC_BLOCK) {
                ids[target].block = 1;
            } else if (w[2] == DEC_BUFFER_BLOCK) {
                ids[target].block = 2;
            }
            break;
        case OP_TYPE_IMAGE:
            NEED(9, target);
            ids[target].kind = K_IMAGE;
            ids[target].image_dim = (uint8_t)(w[3] > 255 ? 255 : w[3]);
            ids[target].image_sampled = (uint8_t)(w[7] > 255 ? 255 : w[7]);
            break;
        case OP_TYPE_SAMPLER:
            NEED(2, target);
            ids[target].kind = K_SAMPLER;
            break;
        case OP_TYPE_SAMPLED_IMAGE:
            NEED(2, target);
            ids[target].kind = K_SAMPLED_IMAGE;
            break;
        case OP_TYPE_ARRAY:
        case OP_TYPE_RUNTIME_ARRAY:
            NEED(2, target);
            ids[target].kind = K_ARRAY;
            break;
        case OP_TYPE_STRUCT:
            NEED(2, target);
            ids[target].kind = K_STRUCT;
            break;
        case OP_TYPE_POINTER:
            NEED(4, target);
            NEED(4, w[3]);
            ids[target].kind = K_POINTER;
            ids[target].storage = w[2];
            ids[target].type = w[3];
            break;
        case OP_VARIABLE:
            NEED(4, w[2]);
            NEED(4, w[1]);
            ids[w[2]].kind = K_VARIABLE;
            ids[w[2]].type = w[1];
            ids[w[2]].storage = w[3];
            break;
        default:
            break;
        }
#undef NEED
        at += wc;
    }

    size_t n = 0;
    for (uint32_t id = 1; id < bound; id++) {
        const IdInfo *v = &ids[id];
        if (!v->binding_at)
            continue;
        if (v->kind != K_VARIABLE || ids[v->type].kind != K_POINTER) {
            rc = LSFG_SPV_EUNSUPPORTED;   /* Binding on something that is not a resource variable */
            goto out;
        }
        const IdInfo *t = &ids[ids[v->type].type];
        uint32_t cls;
        if (v->storage == SC_UNIFORM_CONSTANT && t->kind == K_SAMPLER) {
            cls = C_SAMPLER;
        } else if (v->storage == SC_UNIFORM_CONSTANT && t->kind == K_IMAGE && t->image_dim != DIM_BUFFER &&
                   t->image_dim != DIM_SUBPASS && (t->image_sampled == 1 || t->image_sampled == 2)) {
            cls = t->image_sampled == 1 ? C_SAMPLED : C_STORAGE;
        } else if (v->storage == SC_UNIFORM && t->kind == K_STRUCT && t->block == 1) {
            cls = C_UBO;
        } else {
            rc = LSFG_SPV_EUNSUPPORTED;   /* arrays, combined samplers, SSBOs, texel buffers, ... */
            goto out;
        }
        if (n == MAX_RESOURCES) {
            rc = LSFG_SPV_EUNSUPPORTED;
            goto out;
        }
        res[n].cls = cls;
        res[n].id = id;
        res[n].binding_at = v->binding_at;
        res[n].binding = words[v->binding_at];
        res[n].set_at = v->set_at;
        res[n].set = v->set_at ? words[v->set_at] : 0;
        n++;
    }
    *nres = n;
out:
    free(ids);
    return rc;
}

static void count(const Res *res, size_t n, LsfgSpvLayout *l)
{
    uint32_t c[C_COUNT] = { 0 };
    for (size_t i = 0; i < n; i++)
        c[res[i].cls]++;
    l->uniform_buffers = (uint8_t)c[C_UBO];
    l->samplers = (uint8_t)c[C_SAMPLER];
    l->sampled_images = (uint8_t)c[C_SAMPLED];
    l->storage_images = (uint8_t)c[C_STORAGE];
}

int lsfg_spv_inspect(const uint32_t *words, size_t nwords, LsfgSpvLayout *found)
{
    Res res[MAX_RESOURCES];
    size_t n = 0;
    int rc = collect(words, nwords, res, &n);
    if (rc)
        return rc;
    if (found)
        count(res, n, found);
    return 0;
}

int lsfg_spv_renumber(uint32_t *words, size_t nwords, const LsfgSpvLayout *expect, LsfgSpvLayout *found)
{
    Res res[MAX_RESOURCES];
    size_t n = 0;
    int rc = collect(words, nwords, res, &n);
    if (rc)
        return rc;
    LsfgSpvLayout l;
    count(res, n, &l);
    if (found)
        *found = l;
    qsort(res, n, sizeof res[0], res_cmp);
    for (size_t i = 0; i < n; i++) {
        words[res[i].binding_at] = (uint32_t)i;
        if (res[i].set_at)
            words[res[i].set_at] = 0;
    }
    if (expect && memcmp(expect, &l, sizeof l) != 0)
        return LSFG_SPV_ELAYOUT;
    return 0;
}

int lsfg_spv_requirements(const uint32_t *words, size_t nwords, uint32_t *version, uint32_t *caps)
{
    if (!words || nwords < 5 || words[0] != LSFG_SPV_MAGIC)
        return LSFG_SPV_ENOTSPV;
    uint32_t c = 0;
    for (size_t at = 5; at < nwords;) {
        uint32_t wc = words[at] >> 16, op = words[at] & 0xffffu;
        if (wc == 0 || wc > nwords - at)
            return LSFG_SPV_ECORRUPT;
        if (op == 17 && wc >= 2) {   /* OpCapability */
            switch (words[at + 1]) {
            case 1: case 50: case 2: break;   /* Shader, ImageQuery, Matrix */
            case 5345: c |= LSFG_SPV_CAP_VULKAN_MEMORY_MODEL; break;
            case 55: c |= LSFG_SPV_CAP_READ_WITHOUT_FORMAT; break;
            case 56: c |= LSFG_SPV_CAP_WRITE_WITHOUT_FORMAT; break;
            case 49: c |= LSFG_SPV_CAP_EXTENDED_FORMATS; break;
            case 9: c |= LSFG_SPV_CAP_FLOAT16; break;
            case 22: c |= LSFG_SPV_CAP_INT16; break;
            default: c |= LSFG_SPV_CAP_OTHER; break;
            }
        } else if (op != 17 && op != 11 && op != 10) {   /* capabilities come first (after OpCapability: ext/imports) */
            break;
        }
        at += wc;
    }
    if (version)
        *version = words[1];
    if (caps)
        *caps = c;
    return 0;
}

const char *lsfg_spv_strerror(int err)
{
    switch (err) {
    case 0: return "ok";
    case LSFG_SPV_ENOTSPV: return "not a SPIR-V module";
    case LSFG_SPV_ECORRUPT: return "malformed SPIR-V module";
    case LSFG_SPV_EUNSUPPORTED: return "shader resource framegen cannot bind";
    case LSFG_SPV_ELAYOUT: return "shader resources do not match the pass layout";
    case LSFG_SPV_EINVAL: return "invalid argument";
    default: return "unknown error";
    }
}
