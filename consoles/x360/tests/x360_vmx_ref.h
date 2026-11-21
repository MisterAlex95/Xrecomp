/* x360_vmx_ref.h -- the VMX helpers x360_vmx_ref_test compares: each built twice, once as the packs build them
 * (x360_vmx_ref_test.c: host-order lanes, GCC / clang vector ops) and once as plain C (x360_vmx_ref_scalar.c,
 * -DXV_SCALAR), as the tables xv_ref_fast and xv_ref_scalar. */
#ifndef X360_VMX_REF_H
#define X360_VMX_REF_H

#include "xenon_rt.h"

/* (a, b) */
#define XV_REF_BIN(X)                                                                                                  \
    X(xv_addcuw) X(xv_addubm) X(xv_adduhm) X(xv_avgsb) X(xv_avgsh) X(xv_avgsw) X(xv_avgub) X(xv_avguh) X(xv_avguw)    \
    X(xv_cmpequb) X(xv_cmpequh) X(xv_cmpgtsb) X(xv_cmpgtsh) X(xv_cmpgtub) X(xv_cmpgtuh) X(xv_maxsb) X(xv_maxsh)       \
    X(xv_maxub) X(xv_maxuh) X(xv_maxuw) X(xv_minsb) X(xv_minsh) X(xv_minub) X(xv_minuh) X(xv_minuw) X(xv_rlb)         \
    X(xv_rlh) X(xv_slb) X(xv_slh) X(xv_srab) X(xv_srah) X(xv_srb) X(xv_srh) X(xv_subcuw) X(xv_sububm) X(xv_subuhm)
/* (a, b, vscr) */
#define XV_REF_SAT(X)                                                                                                  \
    X(xv_addsbs) X(xv_addshs) X(xv_addsws) X(xv_addubs) X(xv_adduhs) X(xv_adduws) X(xv_subsbs) X(xv_subshs)            \
    X(xv_subsws) X(xv_sububs) X(xv_subuhs) X(xv_subuws)

typedef xe_v128 (*xv_ref_bin)(xe_v128, xe_v128);
typedef xe_v128 (*xv_ref_sat)(xe_v128, xe_v128, uint32_t *);

typedef struct xv_ref_set {
    xv_ref_bin bin[64];
    xv_ref_sat sat[64];
    xe_v128 (*mrgb)(xe_v128, xe_v128, int);
    xe_v128 (*mrgh)(xe_v128, xe_v128, int);
    xe_v128 (*pkh)(xe_v128, xe_v128, int, uint32_t *);
    xe_v128 (*perm)(xe_v128, xe_v128, xe_v128);
    xe_v128 (*sldoi)(xe_v128, xe_v128, int);
    void (*to_bytes)(xe_v128, uint8_t *);
    xe_v128 (*from_bytes)(const uint8_t *);
    /* the vector loads and stores on a machine's memory (the tests' harness xe_ptr: mem_base / mem_size) */
    xe_v128 (*lvlx)(xe_machine *, uint32_t);
    xe_v128 (*lvrx)(xe_machine *, uint32_t);
    void (*stvlx)(xe_machine *, uint32_t, xe_v128);
    void (*stvrx)(xe_machine *, uint32_t, xe_v128);
    void (*lve)(xe_machine *, xe_v128 *, uint32_t, uint32_t);
    void (*stve)(xe_machine *, uint32_t, xe_v128, uint32_t);
    xe_v128 (*daz)(uint32_t, xe_v128);
    xe_v128 (*lvsl)(uint32_t);
    xe_v128 (*lvsr)(uint32_t);
} xv_ref_set;

#define XV_REF_SET(set_)                                                                                               \
    static xe_v128 w_mrgb(xe_v128 a, xe_v128 b, int lo) { return lo ? xv_mrgb(a, b, 1) : xv_mrgb(a, b, 0); }          \
    static xe_v128 w_mrgh(xe_v128 a, xe_v128 b, int lo) { return lo ? xv_mrgh(a, b, 1) : xv_mrgh(a, b, 0); }          \
    static xe_v128 w_pkh(xe_v128 a, xe_v128 b, int m, uint32_t *v) { return xv_pkh(a, b, m, v); }                      \
    static xe_v128 w_sldoi(xe_v128 a, xe_v128 b, int sh) { return xv_sldoi(a, b, sh); }                                \
    static void w_to(xe_v128 v, uint8_t *b) { xv_to_bytes(v, b); }                                                    \
    static xe_v128 w_from(const uint8_t *b) { return xv_from_bytes(b); }                                              \
    static void w_lve(xe_machine *m, xe_v128 *v, uint32_t ea, uint32_t n)                                              \
    {                                                                                                                  \
        if (n == 1)                                                                                                    \
            xv_lve(m, v, ea, 1);                                                                                       \
        else if (n == 2)                                                                                               \
            xv_lve(m, v, ea, 2);                                                                                       \
        else                                                                                                           \
            xv_lve(m, v, ea, 4);                                                                                       \
    }                                                                                                                  \
    static void w_stve(xe_machine *m, uint32_t ea, xe_v128 v, uint32_t n)                                              \
    {                                                                                                                  \
        if (n == 1)                                                                                                    \
            xv_stve(m, ea, v, 1);                                                                                      \
        else if (n == 2)                                                                                               \
            xv_stve(m, ea, v, 2);                                                                                      \
        else                                                                                                           \
            xv_stve(m, ea, v, 4);                                                                                      \
    }                                                                                                                  \
    const xv_ref_set set_ = {{XV_REF_BIN(XV_REF_PTR)}, {XV_REF_SAT(XV_REF_PTR)}, w_mrgb, w_mrgh, w_pkh, xv_perm,     \
                             w_sldoi, w_to, w_from, xv_lvlx, xv_lvrx, xv_stvlx, xv_stvrx, w_lve, w_stve, xv_daz,     \
                             xv_lvsl, xv_lvsr};
#define XV_REF_PTR(n) n,

extern const xv_ref_set xv_ref_fast, xv_ref_scalar;

#endif
