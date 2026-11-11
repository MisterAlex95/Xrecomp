/* src/framegen/lsfg_spv.c: SPIR-V magic checks, binding renumbering to framegen's layout order, the shader table.
 * Every module is hand-assembled here (spv_build.h): no shader from any DLL. */
#include "lsfg_spv.h"
#include "spv_build.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long a_ = (long)(a), b_ = (long)(b); if (a_ != b_) { \
    fprintf(stderr, "FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #a, a_, b_); g_fail++; } } while (0)

static void test_magic(void)
{
    SbModule m;
    size_t n = sb_build_layout(&m, 1, 1, 1, 1);
    CHECK(lsfg_spv_is_spirv(m.w, n * 4));
    CHECK(!lsfg_spv_is_spirv(m.w, n * 4 - 2));           /* not a multiple of 4 */
    CHECK(!lsfg_spv_is_spirv(m.w, 16));                  /* shorter than the header */
    CHECK(!lsfg_spv_is_spirv(NULL, 64));
    uint8_t dxbc[64] = "DXBC";
    CHECK(!lsfg_spv_is_spirv(dxbc, sizeof dxbc));
    CHECK(lsfg_spv_is_dxbc(dxbc, sizeof dxbc));
    CHECK(!lsfg_spv_is_dxbc(m.w, n * 4));
    uint32_t be[8] = { 0x03022307u, 0x00000100u, 0, 1, 0 };   /* byte-swapped magic: refused */
    CHECK(!lsfg_spv_is_spirv(be, sizeof be));
    CHECK_EQ(lsfg_spv_renumber(be, 8, NULL, NULL), LSFG_SPV_ENOTSPV);
}

/* Bindings after renumbering, by resource declaration index. */
static uint32_t binding_of(const SbModule *m, size_t nres, size_t i)
{
    return m->w[sb_binding_at(m, sb_var_id(m, nres, i))];
}

static void test_renumber_order(void)
{
    /* Declared: storage u1, storage u0, sampled t0, sampled t1, ubo b0, sampler s1, sampler s0. framegen's order:
     * ubo, samplers, sampled, storage, each by register: b0=0, s0=1, s1=2, t0=3, t1=4, u0=5, u1=6. */
    SbModule m;
    size_t n = sb_build_layout(&m, 1, 2, 2, 2);
    LsfgSpvLayout want = { 1, 2, 2, 2 }, found;
    CHECK_EQ(lsfg_spv_renumber(m.w, n, &want, &found), 0);
    CHECK(memcmp(&want, &found, sizeof want) == 0);
    CHECK_EQ(binding_of(&m, 7, 0), 6);   /* u1 */
    CHECK_EQ(binding_of(&m, 7, 1), 5);   /* u0 */
    CHECK_EQ(binding_of(&m, 7, 2), 3);   /* t0 */
    CHECK_EQ(binding_of(&m, 7, 3), 4);   /* t1 */
    CHECK_EQ(binding_of(&m, 7, 4), 0);   /* b0 */
    CHECK_EQ(binding_of(&m, 7, 5), 2);   /* s1 */
    CHECK_EQ(binding_of(&m, 7, 6), 1);   /* s0 */

    /* Idempotent: a renumbered module renumbers to itself. */
    SbModule again = m;
    CHECK_EQ(lsfg_spv_renumber(again.w, n, &want, NULL), 0);
    CHECK(memcmp(again.w, m.w, n * 4) == 0);

    /* Inspect counts without touching anything. */
    SbModule fresh;
    size_t n2 = sb_build_layout(&fresh, 1, 2, 12, 2);
    SbModule copy = fresh;
    CHECK_EQ(lsfg_spv_inspect(fresh.w, n2, &found), 0);
    CHECK(found.uniform_buffers == 1 && found.samplers == 2 && found.sampled_images == 12 && found.storage_images == 2);
    CHECK(memcmp(copy.w, fresh.w, n2 * 4) == 0);
}

static void test_sets(void)
{
    /* Same register in two sets: sorted by (set, binding); DescriptorSet becomes 0; a missing set counts as 0. */
    SbRes r[] = {
        { SB_SAMPLED, 1, 0, 0 },
        { SB_SAMPLED, 0, 5, 0 },
        { SB_SAMPLER, 0, 0, 1 },
    };
    SbModule m;
    size_t n = sb_build(&m, r, 3);
    CHECK_EQ(lsfg_spv_renumber(m.w, n, NULL, NULL), 0);
    CHECK_EQ(binding_of(&m, 3, 2), 0);   /* sampler */
    CHECK_EQ(binding_of(&m, 3, 1), 1);   /* set 0 t5 */
    CHECK_EQ(binding_of(&m, 3, 0), 2);   /* set 1 t0 */
    for (size_t at = 5; at < m.n; at += m.w[at] >> 16)
        if ((m.w[at] & 0xffff) == 71 && m.w[at + 2] == 34)
            CHECK_EQ(m.w[at + 3], 0);
}

static void test_layout_mismatch(void)
{
    SbModule m;
    size_t n = sb_build_layout(&m, 0, 1, 1, 2);
    LsfgSpvLayout want = { 0, 1, 2, 2 }, found;
    CHECK_EQ(lsfg_spv_renumber(m.w, n, &want, &found), LSFG_SPV_ELAYOUT);
    CHECK(found.sampled_images == 1);
}

static void test_unsupported(void)
{
    static const int kinds[] = { SB_SAMPLED_ARRAY, SB_COMBINED, SB_SSBO, SB_TEXEL_BUFFER };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        SbRes r[] = { { SB_SAMPLER, 0, 0, 0 }, { kinds[i], 0, 1, 0 } };
        SbModule m;
        size_t n = sb_build(&m, r, 2);
        CHECK_EQ(lsfg_spv_renumber(m.w, n, NULL, NULL), LSFG_SPV_EUNSUPPORTED);
    }
}

static void test_corrupt(void)
{
    SbModule m;
    size_t n = sb_build_layout(&m, 1, 1, 1, 1);
    /* Every truncation: never reads past the end. Cuts inside an instruction are ECORRUPT. */
    for (size_t k = 5; k < n; k++) {
        SbModule t = m;
        int rc = lsfg_spv_inspect(t.w, k, NULL);
        CHECK(rc == 0 || rc == LSFG_SPV_ECORRUPT || rc == LSFG_SPV_EUNSUPPORTED);
    }
    SbModule t = m;
    t.w[5] = 0x00000011u;                 /* word count 0 */
    CHECK_EQ(lsfg_spv_inspect(t.w, n, NULL), LSFG_SPV_ECORRUPT);
    t = m;
    t.w[5] = 0xffff0011u;                 /* runs past the end */
    CHECK_EQ(lsfg_spv_inspect(t.w, n, NULL), LSFG_SPV_ECORRUPT);
    t = m;
    t.w[3] = 2;                           /* ids beyond the bound */
    CHECK_EQ(lsfg_spv_inspect(t.w, n, NULL), LSFG_SPV_ECORRUPT);
    t = m;
    t.w[3] = 0xffffffffu;                 /* absurd bound */
    CHECK_EQ(lsfg_spv_inspect(t.w, n, NULL), LSFG_SPV_ECORRUPT);
    /* A Binding on a non-variable (the void type, id 2). */
    t = m;
    size_t at = sb_binding_at(&t, sb_var_id(&t, 4, 0));
    t.w[at - 2] = 2;
    CHECK_EQ(lsfg_spv_inspect(t.w, n, NULL), LSFG_SPV_EUNSUPPORTED);
}

static void test_table(void)
{
    size_t n;
    const LsfgShaderInfo *t = lsfg_shader_table(&n);
    CHECK_EQ(n, 52);
    uint16_t ids[64];
    CHECK_EQ(lsfg_shader_ids(0, ids, 64), 25);
    CHECK_EQ(ids[0], 255);
    CHECK_EQ(ids[24], 279);
    CHECK_EQ(lsfg_shader_ids(1, ids, 64), 25);
    CHECK_EQ(ids[0], 255);
    CHECK_EQ(ids[1], 256);
    CHECK_EQ(ids[2], 280);
    CHECK_EQ(ids[24], 302);
    const LsfgShaderInfo *g = lsfg_shader_find("generate");
    CHECK(g && g->base_id == 256 && g->layout.uniform_buffers == 1 && g->layout.samplers == 2 &&
          g->layout.sampled_images == 5 && g->layout.storage_images == 1);
    const LsfgShaderInfo *pb = lsfg_shader_find("p_beta[0]");
    CHECK(pb && pb->base_id == 298 && pb->performance == 1 && pb->layout.sampled_images == 6);
    CHECK(lsfg_shader_find("nope") == NULL);
    /* Names sharing an ID share the layout. */
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < n; j++)
            if (t[i].base_id == t[j].base_id)
                CHECK(memcmp(&t[i].layout, &t[j].layout, sizeof t[i].layout) == 0);
    /* Every table layout renumbers a module built for it. */
    for (size_t i = 0; i < n; i++) {
        SbModule m;
        size_t w = sb_build_layout(&m, t[i].layout.uniform_buffers, t[i].layout.samplers, t[i].layout.sampled_images,
                                   t[i].layout.storage_images);
        CHECK_EQ(lsfg_spv_renumber(m.w, w, lsfg_shader_layout_for_id(t[i].base_id), NULL), 0);
    }
}

int main(void)
{
    test_magic();
    test_renumber_order();
    test_sets();
    test_layout_mismatch();
    test_unsupported();
    test_corrupt();
    test_table();
    if (g_fail) {
        fprintf(stderr, "lsfg_spv: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("lsfg_spv: ok\n");
    return 0;
}
