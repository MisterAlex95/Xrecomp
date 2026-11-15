/* WMV2 / MS-MPEG4 code tables (wmv2_tables.c, LGPL-2.1-or-later, from
 * FFmpeg: see that file's header and NOTICE).
 *
 * VLC tables are (code, length) pairs, the code right-aligned in its length;
 * the symbol is the pair's index. */
#ifndef XMV_WMV2_TABLES_H
#define XMV_WMV2_TABLES_H

#include <stdint.h>

/* Run/level tables. Table k has n_k codes plus one escape code (index n_k);
 * codes below last_k are "not last" coefficients, the others end the block.
 *   0, 1, 2: intra luma (low motion, high motion, mid rate)
 *   3, 4, 5: intra chroma and inter (low motion, high motion, mid rate) */
extern const uint16_t wmv2_rl0_vlc[], wmv2_rl1_vlc[], wmv2_rl2_vlc[],
                      wmv2_rl3_vlc[], wmv2_rl4_vlc[], wmv2_rl5_vlc[];
extern const int8_t wmv2_rl0_run[], wmv2_rl1_run[], wmv2_rl2_run[],
                    wmv2_rl3_run[], wmv2_rl4_run[], wmv2_rl5_run[];
extern const int8_t wmv2_rl0_level[], wmv2_rl1_level[], wmv2_rl2_level[],
                    wmv2_rl3_level[], wmv2_rl4_level[], wmv2_rl5_level[];

/* Motion vector tables: 1099 codes + the escape (index 1099). */
#define WMV2_MV_ESCAPE 1099
extern const uint16_t wmv2_mv0_code[1100], wmv2_mv1_code[1100];
extern const uint8_t wmv2_mv0_bits[1100], wmv2_mv1_bits[1100];
extern const uint8_t wmv2_mv0_x[1099], wmv2_mv0_y[1099];
extern const uint8_t wmv2_mv1_x[1099], wmv2_mv1_y[1099];

/* P-frame macroblock type / coded block pattern, four tables of 128. */
extern const uint32_t wmv2_mb_inter0[256], wmv2_mb_inter1[256],
                      wmv2_mb_inter2[256], wmv2_mb_inter3[256];
/* I-frame coded block pattern, 64 codes. */
extern const uint16_t wmv2_mb_intra[128];
/* DC differential: [table][chroma][120 codes][code, length]. */
#define WMV2_DC_MAX 119
extern const uint32_t wmv2_dc_vlc[960];

/* Scan orders: [0] inter, [1] intra, [2] intra horizontal (top prediction),
 * [3] intra vertical (left prediction). */
extern const uint8_t wmv2_scan[256];
/* Adaptive block transform scans: A for 8x4 halves, B for 4x8. */
extern const uint8_t wmv2_scan_abt_a[64], wmv2_scan_abt_b[64];

extern const uint8_t wmv2_y_dc_scale[32], wmv2_c_dc_scale[32];
extern const uint8_t wmv2_chroma_qscale[32];
extern const uint8_t wmv2_loop_strength[32];

#endif
