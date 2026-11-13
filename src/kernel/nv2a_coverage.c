/* What the NV2A executor and its back ends do not draw, by name (XBA-120):
 * nv2a_coverage.h. Depends only on the status writer, so a test builds the
 * two alone. */
#include "nv2a_coverage.h"
#include "recomp_status.h"

#include <stddef.h>
#include <string.h>

#define NV_FMT_UNKNOWN "texture format (not an NV2A format)"

/* SET_TEXTURE_FORMAT colour codes 0x00-0x41 (xemu nv2a_regs.h and
 * kelvin_color_format_info). `decoded` must say exactly what sample_tex in
 * nv2a_pb_exec.c reads: tests/nv2a_coverage decodes every code and compares. */
static const Nv2aTexFormat s_fmt[0x42] = {
    /* 0x00 */ { "SZ_Y8", "texture format 0x00 SZ_Y8", 1, 1, 1 },
    /* 0x01 */ { "SZ_AY8", "texture format 0x01 SZ_AY8", 1, 1, 1 },
    /* 0x02 */ { "SZ_A1R5G5B5", "texture format 0x02 SZ_A1R5G5B5", 2, 1, 1 },
    /* 0x03 */ { "SZ_X1R5G5B5", "texture format 0x03 SZ_X1R5G5B5", 2, 1, 1 },
    /* 0x04 */ { "SZ_A4R4G4B4", "texture format 0x04 SZ_A4R4G4B4", 2, 1, 1 },
    /* 0x05 */ { "SZ_R5G6B5", "texture format 0x05 SZ_R5G6B5", 2, 1, 1 },
    /* 0x06 */ { "SZ_A8R8G8B8", "texture format 0x06 SZ_A8R8G8B8", 4, 1, 1 },
    /* 0x07 */ { "SZ_X8R8G8B8", "texture format 0x07 SZ_X8R8G8B8", 4, 1, 1 },
    /* 0x08 */ { NULL, NV_FMT_UNKNOWN, 4, 0, 0 },
    /* 0x09 */ { NULL, NV_FMT_UNKNOWN, 4, 0, 0 },
    /* 0x0A */ { NULL, NV_FMT_UNKNOWN, 4, 0, 0 },
    /* 0x0B */ { "SZ_I8_A8R8G8B8", "texture format 0x0B SZ_I8_A8R8G8B8", 1, 1, 1 },
    /* 0x0C */ { "L_DXT1_A1R5G5B5", "texture format 0x0C L_DXT1_A1R5G5B5", 0, 0, 1 },
    /* 0x0D */ { NULL, NV_FMT_UNKNOWN, 4, 0, 0 },
    /* 0x0E */ { "L_DXT23_A8R8G8B8", "texture format 0x0E L_DXT23_A8R8G8B8", 0, 0, 1 },
    /* 0x0F */ { "L_DXT45_A8R8G8B8", "texture format 0x0F L_DXT45_A8R8G8B8", 0, 0, 1 },
    /* 0x10 */ { "LU_IMAGE_A1R5G5B5", "texture format 0x10 LU_IMAGE_A1R5G5B5", 2, 0, 1 },
    /* 0x11 */ { "LU_IMAGE_R5G6B5", "texture format 0x11 LU_IMAGE_R5G6B5", 2, 0, 1 },
    /* 0x12 */ { "LU_IMAGE_A8R8G8B8", "texture format 0x12 LU_IMAGE_A8R8G8B8", 4, 0, 1 },
    /* 0x13 */ { "LU_IMAGE_Y8", "texture format 0x13 LU_IMAGE_Y8", 1, 0, 1 },
    /* 0x14 */ { "LU_IMAGE_SY8", "texture format 0x14 LU_IMAGE_SY8", 1, 0, 0 },
    /* 0x15 */ { "LU_IMAGE_X7SY9", "texture format 0x15 LU_IMAGE_X7SY9", 2, 0, 0 },
    /* 0x16 */ { "LU_IMAGE_R8B8", "texture format 0x16 LU_IMAGE_R8B8", 2, 0, 0 },
    /* 0x17 */ { "LU_IMAGE_G8B8", "texture format 0x17 LU_IMAGE_G8B8", 2, 0, 0 },
    /* 0x18 */ { "LU_IMAGE_SG8SB8", "texture format 0x18 LU_IMAGE_SG8SB8", 2, 0, 0 },
    /* 0x19 */ { "SZ_A8", "texture format 0x19 SZ_A8", 1, 1, 1 },
    /* 0x1A */ { "SZ_A8Y8", "texture format 0x1A SZ_A8Y8", 2, 1, 0 },
    /* 0x1B */ { "LU_IMAGE_AY8", "texture format 0x1B LU_IMAGE_AY8", 1, 0, 1 },
    /* 0x1C */ { "LU_IMAGE_X1R5G5B5", "texture format 0x1C LU_IMAGE_X1R5G5B5", 2, 0, 1 },
    /* 0x1D */ { "LU_IMAGE_A4R4G4B4", "texture format 0x1D LU_IMAGE_A4R4G4B4", 2, 0, 1 },
    /* 0x1E */ { "LU_IMAGE_X8R8G8B8", "texture format 0x1E LU_IMAGE_X8R8G8B8", 4, 0, 1 },
    /* 0x1F */ { "LU_IMAGE_A8", "texture format 0x1F LU_IMAGE_A8", 1, 0, 1 },
    /* 0x20 */ { "LU_IMAGE_A8Y8", "texture format 0x20 LU_IMAGE_A8Y8", 2, 0, 0 },
    /* 0x21 */ { NULL, NV_FMT_UNKNOWN, 4, 0, 0 },
    /* 0x22 */ { NULL, NV_FMT_UNKNOWN, 4, 0, 0 },
    /* 0x23 */ { NULL, NV_FMT_UNKNOWN, 4, 0, 0 },
    /* 0x24 */ { "LC_IMAGE_CR8YB8CB8YA8", "texture format 0x24 LC_IMAGE_CR8YB8CB8YA8", 2, 0, 1 },
    /* 0x25 */ { "LC_IMAGE_YB8CR8YA8CB8", "texture format 0x25 LC_IMAGE_YB8CR8YA8CB8", 2, 0, 1 },
    /* 0x26 */ { "LU_IMAGE_A8CR8CB8Y8", "texture format 0x26 LU_IMAGE_A8CR8CB8Y8", 4, 0, 0 },
    /* 0x27 */ { "SZ_R6G5B5", "texture format 0x27 SZ_R6G5B5", 2, 1, 0 },
    /* 0x28 */ { "SZ_G8B8", "texture format 0x28 SZ_G8B8", 2, 1, 0 },
    /* 0x29 */ { "SZ_R8B8", "texture format 0x29 SZ_R8B8", 2, 1, 0 },
    /* 0x2A */ { "SZ_DEPTH_X8_Y24_FIXED", "texture format 0x2A SZ_DEPTH_X8_Y24_FIXED", 4, 1, 0 },
    /* 0x2B */ { "SZ_DEPTH_X8_Y24_FLOAT", "texture format 0x2B SZ_DEPTH_X8_Y24_FLOAT", 4, 1, 0 },
    /* 0x2C */ { "SZ_DEPTH_Y16_FIXED", "texture format 0x2C SZ_DEPTH_Y16_FIXED", 2, 1, 0 },
    /* 0x2D */ { "SZ_DEPTH_Y16_FLOAT", "texture format 0x2D SZ_DEPTH_Y16_FLOAT", 2, 1, 0 },
    /* 0x2E */ { "LU_IMAGE_DEPTH_X8_Y24_FIXED", "texture format 0x2E LU_IMAGE_DEPTH_X8_Y24_FIXED", 4, 0, 0 },
    /* 0x2F */ { "LU_IMAGE_DEPTH_X8_Y24_FLOAT", "texture format 0x2F LU_IMAGE_DEPTH_X8_Y24_FLOAT", 4, 0, 0 },
    /* 0x30 */ { "LU_IMAGE_DEPTH_Y16_FIXED", "texture format 0x30 LU_IMAGE_DEPTH_Y16_FIXED", 2, 0, 0 },
    /* 0x31 */ { "LU_IMAGE_DEPTH_Y16_FLOAT", "texture format 0x31 LU_IMAGE_DEPTH_Y16_FLOAT", 2, 0, 0 },
    /* 0x32 */ { "SZ_Y16", "texture format 0x32 SZ_Y16", 2, 1, 0 },
    /* 0x33 */ { "SZ_YB16YA16", "texture format 0x33 SZ_YB16YA16", 4, 1, 0 },
    /* 0x34 */ { "LC_IMAGE_A4V6YB6A4U6YA6", "texture format 0x34 LC_IMAGE_A4V6YB6A4U6YA6", 2, 0, 0 },
    /* 0x35 */ { "LU_IMAGE_Y16", "texture format 0x35 LU_IMAGE_Y16", 2, 0, 0 },
    /* 0x36 */ { "LU_IMAGE_YB16YA16", "texture format 0x36 LU_IMAGE_YB16YA16", 4, 0, 0 },
    /* 0x37 */ { "LU_IMAGE_R6G5B5", "texture format 0x37 LU_IMAGE_R6G5B5", 2, 0, 0 },
    /* 0x38 */ { "SZ_R5G5B5A1", "texture format 0x38 SZ_R5G5B5A1", 2, 1, 0 },
    /* 0x39 */ { "SZ_R4G4B4A4", "texture format 0x39 SZ_R4G4B4A4", 2, 1, 0 },
    /* 0x3A */ { "SZ_A8B8G8R8", "texture format 0x3A SZ_A8B8G8R8", 4, 1, 0 },
    /* 0x3B */ { "SZ_B8G8R8A8", "texture format 0x3B SZ_B8G8R8A8", 4, 1, 0 },
    /* 0x3C */ { "SZ_R8G8B8A8", "texture format 0x3C SZ_R8G8B8A8", 4, 1, 0 },
    /* 0x3D */ { "LU_IMAGE_R5G5B5A1", "texture format 0x3D LU_IMAGE_R5G5B5A1", 2, 0, 0 },
    /* 0x3E */ { "LU_IMAGE_R4G4B4A4", "texture format 0x3E LU_IMAGE_R4G4B4A4", 2, 0, 0 },
    /* 0x3F */ { "LU_IMAGE_A8B8G8R8", "texture format 0x3F LU_IMAGE_A8B8G8R8", 4, 0, 1 },
    /* 0x40 */ { "LU_IMAGE_B8G8R8A8", "texture format 0x40 LU_IMAGE_B8G8R8A8", 4, 0, 1 },
    /* 0x41 */ { "LU_IMAGE_R8G8B8A8", "texture format 0x41 LU_IMAGE_R8G8B8A8", 4, 0, 1 },
};

static const Nv2aTexFormat s_fmt_unknown = { NULL, NV_FMT_UNKNOWN, 4, 0, 0 };

const Nv2aTexFormat *nv2a_tex_format(uint32_t fmt)
{
    return fmt < sizeof s_fmt / sizeof s_fmt[0] && s_fmt[fmt].name ? &s_fmt[fmt] : &s_fmt_unknown;
}

/* Every Kelvin (NV097) method xemu's nv2a_regs.h names, by address
 * (generated from it, sorted). An address inside a method array (16 matrix
 * words, 8 lights, 4 texture stages) is named after the array (s_arrays). */
typedef struct { uint16_t method; const char *name; } NvMethodName;
static const NvMethodName s_methods[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0110, "WAIT_FOR_IDLE" },
    { 0x0120, "SET_FLIP_READ" },
    { 0x0124, "SET_FLIP_WRITE" },
    { 0x0128, "SET_FLIP_MODULO" },
    { 0x012C, "FLIP_INCREMENT_WRITE" },
    { 0x0130, "FLIP_STALL" },
    { 0x0180, "SET_CONTEXT_DMA_NOTIFIES" },
    { 0x0184, "SET_CONTEXT_DMA_A" },
    { 0x0188, "SET_CONTEXT_DMA_B" },
    { 0x0190, "SET_CONTEXT_DMA_STATE" },
    { 0x0194, "SET_CONTEXT_DMA_COLOR" },
    { 0x0198, "SET_CONTEXT_DMA_ZETA" },
    { 0x019C, "SET_CONTEXT_DMA_VERTEX_A" },
    { 0x01A0, "SET_CONTEXT_DMA_VERTEX_B" },
    { 0x01A4, "SET_CONTEXT_DMA_SEMAPHORE" },
    { 0x01A8, "SET_CONTEXT_DMA_REPORT" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0260, "SET_COMBINER_ALPHA_ICW" },
    { 0x0288, "SET_COMBINER_SPECULAR_FOG_CW0" },
    { 0x028C, "SET_COMBINER_SPECULAR_FOG_CW1" },
    { 0x0290, "SET_CONTROL0" },
    { 0x0294, "SET_LIGHT_CONTROL" },
    { 0x0298, "SET_COLOR_MATERIAL" },
    { 0x029C, "SET_FOG_MODE" },
    { 0x02A0, "SET_FOG_GEN_MODE" },
    { 0x02A4, "SET_FOG_ENABLE" },
    { 0x02A8, "SET_FOG_COLOR" },
    { 0x02B4, "SET_WINDOW_CLIP_TYPE" },
    { 0x02C0, "SET_WINDOW_CLIP_HORIZONTAL" },
    { 0x02E0, "SET_WINDOW_CLIP_VERTICAL" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x0308, "SET_CULL_FACE_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x0318, "SET_POINT_PARAMS_ENABLE" },
    { 0x031C, "SET_POINT_SMOOTH_ENABLE" },
    { 0x0320, "SET_LINE_SMOOTH_ENABLE" },
    { 0x0324, "SET_POLY_SMOOTH_ENABLE" },
    { 0x0328, "SET_SKIN_MODE" },
    { 0x032C, "SET_STENCIL_TEST_ENABLE" },
    { 0x0330, "SET_POLY_OFFSET_POINT_ENABLE" },
    { 0x0334, "SET_POLY_OFFSET_LINE_ENABLE" },
    { 0x0338, "SET_POLY_OFFSET_FILL_ENABLE" },
    { 0x033C, "SET_ALPHA_FUNC" },
    { 0x0340, "SET_ALPHA_REF" },
    { 0x0344, "SET_BLEND_FUNC_SFACTOR" },
    { 0x0348, "SET_BLEND_FUNC_DFACTOR" },
    { 0x034C, "SET_BLEND_COLOR" },
    { 0x0350, "SET_BLEND_EQUATION" },
    { 0x0354, "SET_DEPTH_FUNC" },
    { 0x0358, "SET_COLOR_MASK" },
    { 0x035C, "SET_DEPTH_MASK" },
    { 0x0360, "SET_STENCIL_MASK" },
    { 0x0364, "SET_STENCIL_FUNC" },
    { 0x0368, "SET_STENCIL_FUNC_REF" },
    { 0x036C, "SET_STENCIL_FUNC_MASK" },
    { 0x0370, "SET_STENCIL_OP_FAIL" },
    { 0x0374, "SET_STENCIL_OP_ZFAIL" },
    { 0x0378, "SET_STENCIL_OP_ZPASS" },
    { 0x037C, "SET_SHADE_MODE" },
    { 0x0384, "SET_POLYGON_OFFSET_SCALE_FACTOR" },
    { 0x0388, "SET_POLYGON_OFFSET_BIAS" },
    { 0x038C, "SET_FRONT_POLYGON_MODE" },
    { 0x0390, "SET_BACK_POLYGON_MODE" },
    { 0x0394, "SET_CLIP_MIN" },
    { 0x0398, "SET_CLIP_MAX" },
    { 0x039C, "SET_CULL_FACE" },
    { 0x03A0, "SET_FRONT_FACE" },
    { 0x03A4, "SET_NORMALIZATION_ENABLE" },
    { 0x03A8, "SET_MATERIAL_EMISSION" },
    { 0x03B4, "SET_MATERIAL_ALPHA" },
    { 0x03B8, "SET_SPECULAR_ENABLE" },
    { 0x03BC, "SET_LIGHT_ENABLE_MASK" },
    { 0x03C0, "SET_TEXGEN_S" },
    { 0x03C4, "SET_TEXGEN_T" },
    { 0x03C8, "SET_TEXGEN_R" },
    { 0x03CC, "SET_TEXGEN_Q" },
    { 0x0420, "SET_TEXTURE_MATRIX_ENABLE" },
    { 0x043C, "SET_POINT_SIZE" },
    { 0x0440, "SET_PROJECTION_MATRIX" },
    { 0x0480, "SET_MODEL_VIEW_MATRIX" },
    { 0x0580, "SET_INVERSE_MODEL_VIEW_MATRIX" },
    { 0x0680, "SET_COMPOSITE_MATRIX" },
    { 0x06C0, "SET_TEXTURE_MATRIX" },
    { 0x0840, "SET_TEXGEN_PLANE_S" },
    { 0x0850, "SET_TEXGEN_PLANE_T" },
    { 0x0860, "SET_TEXGEN_PLANE_R" },
    { 0x0870, "SET_TEXGEN_PLANE_Q" },
    { 0x09C0, "SET_FOG_PARAMS" },
    { 0x09CC, "SET_TEXGEN_VIEW_MODEL" },
    { 0x09D0, "SET_FOG_PLANE" },
    { 0x09E0, "SET_SPECULAR_PARAMS" },
    { 0x09FC, "SET_PROVOKING_VERTEX" },
    { 0x0A10, "SET_SCENE_AMBIENT_COLOR" },
    { 0x0A20, "SET_VIEWPORT_OFFSET" },
    { 0x0A30, "SET_POINT_PARAMS" },
    { 0x0A50, "SET_EYE_POSITION" },
    { 0x0A60, "SET_COMBINER_FACTOR0" },
    { 0x0A80, "SET_COMBINER_FACTOR1" },
    { 0x0AA0, "SET_COMBINER_ALPHA_OCW" },
    { 0x0AC0, "SET_COMBINER_COLOR_ICW" },
    { 0x0AE0, "SET_COLOR_KEY_COLOR" },
    { 0x0AF0, "SET_VIEWPORT_SCALE" },
    { 0x0B00, "SET_TRANSFORM_PROGRAM" },
    { 0x0B80, "SET_TRANSFORM_CONSTANT" },
    { 0x0C00, "SET_BACK_LIGHT_AMBIENT_COLOR" },
    { 0x0C0C, "SET_BACK_LIGHT_DIFFUSE_COLOR" },
    { 0x0C18, "SET_BACK_LIGHT_SPECULAR_COLOR" },
    { 0x1000, "SET_LIGHT_AMBIENT_COLOR" },
    { 0x100C, "SET_LIGHT_DIFFUSE_COLOR" },
    { 0x1018, "SET_LIGHT_SPECULAR_COLOR" },
    { 0x1024, "SET_LIGHT_LOCAL_RANGE" },
    { 0x1028, "SET_LIGHT_INFINITE_HALF_VECTOR" },
    { 0x1034, "SET_LIGHT_INFINITE_DIRECTION" },
    { 0x1040, "SET_LIGHT_SPOT_FALLOFF" },
    { 0x104C, "SET_LIGHT_SPOT_DIRECTION" },
    { 0x105C, "SET_LIGHT_LOCAL_POSITION" },
    { 0x1068, "SET_LIGHT_LOCAL_ATTENUATION" },
    { 0x1500, "SET_VERTEX3F" },
    { 0x1518, "SET_VERTEX4F" },
    { 0x1530, "SET_NORMAL3F" },
    { 0x1540, "SET_NORMAL3S" },
    { 0x1550, "SET_DIFFUSE_COLOR4F" },
    { 0x1560, "SET_DIFFUSE_COLOR3F" },
    { 0x156C, "SET_DIFFUSE_COLOR4UB" },
    { 0x1570, "SET_SPECULAR_COLOR4F" },
    { 0x1580, "SET_SPECULAR_COLOR3F" },
    { 0x158C, "SET_SPECULAR_COLOR4UB" },
    { 0x1590, "SET_TEXCOORD0_2F" },
    { 0x1598, "SET_TEXCOORD0_2S" },
    { 0x15A0, "SET_TEXCOORD0_4F" },
    { 0x15B0, "SET_TEXCOORD0_4S" },
    { 0x15B8, "SET_TEXCOORD1_2F" },
    { 0x15C0, "SET_TEXCOORD1_2S" },
    { 0x15C8, "SET_TEXCOORD1_4F" },
    { 0x15D8, "SET_TEXCOORD1_4S" },
    { 0x15E0, "SET_TEXCOORD2_2F" },
    { 0x15E8, "SET_TEXCOORD2_2S" },
    { 0x15F0, "SET_TEXCOORD2_4F" },
    { 0x1600, "SET_TEXCOORD2_4S" },
    { 0x1608, "SET_TEXCOORD3_2F" },
    { 0x1610, "SET_TEXCOORD3_2S" },
    { 0x1620, "SET_TEXCOORD3_4F" },
    { 0x1630, "SET_TEXCOORD3_4S" },
    { 0x1698, "SET_FOG_COORD" },
    { 0x169C, "SET_WEIGHT1F" },
    { 0x16A0, "SET_WEIGHT2F" },
    { 0x16B0, "SET_WEIGHT3F" },
    { 0x16C0, "SET_WEIGHT4F" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17BC, "SET_LOGIC_OP_ENABLE" },
    { 0x17C0, "SET_LOGIC_OP" },
    { 0x17C8, "CLEAR_REPORT_VALUE" },
    { 0x17CC, "SET_ZPASS_PIXEL_COUNT_ENABLE" },
    { 0x17D0, "GET_REPORT" },
    { 0x17E0, "SET_EYE_DIRECTION" },
    { 0x17F8, "SET_SHADER_CLIP_PLANE_MODE" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "ARRAY_ELEMENT32" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1818, "INLINE_ARRAY" },
    { 0x181C, "SET_EYE_VECTOR" },
    { 0x1880, "SET_VERTEX_DATA2F_M" },
    { 0x1900, "SET_VERTEX_DATA2S" },
    { 0x1940, "SET_VERTEX_DATA4UB" },
    { 0x1980, "SET_VERTEX_DATA4S_M" },
    { 0x1A00, "SET_VERTEX_DATA4F_M" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B10, "SET_TEXTURE_CONTROL1" },
    { 0x1B14, "SET_TEXTURE_FILTER" },
    { 0x1B1C, "SET_TEXTURE_IMAGE_RECT" },
    { 0x1B20, "SET_TEXTURE_PALETTE" },
    { 0x1B24, "SET_TEXTURE_BORDER_COLOR" },
    { 0x1B28, "SET_TEXTURE_SET_BUMP_ENV_MAT" },
    { 0x1B38, "SET_TEXTURE_SET_BUMP_ENV_SCALE" },
    { 0x1B3C, "SET_TEXTURE_SET_BUMP_ENV_OFFSET" },
    { 0x1D6C, "SET_SEMAPHORE_OFFSET" },
    { 0x1D70, "BACK_END_WRITE_SEMAPHORE_RELEASE" },
    { 0x1D78, "SET_ZMIN_MAX_CONTROL" },
    { 0x1D7C, "SET_ANTI_ALIASING_CONTROL" },
    { 0x1D8C, "SET_ZSTENCIL_CLEAR_VALUE" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D98, "SET_CLEAR_RECT_HORIZONTAL" },
    { 0x1D9C, "SET_CLEAR_RECT_VERTICAL" },
    { 0x1E20, "SET_SPECULAR_FOG_FACTOR" },
    { 0x1E28, "SET_SPECULAR_PARAMS_BACK" },
    { 0x1E40, "SET_COMBINER_COLOR_OCW" },
    { 0x1E60, "SET_COMBINER_CONTROL" },
    { 0x1E68, "SET_SHADOW_ZSLOPE_THRESHOLD" },
    { 0x1E6C, "SET_SHADOW_DEPTH_FUNC" },
    { 0x1E70, "SET_SHADER_STAGE_PROGRAM" },
    { 0x1E78, "SET_SHADER_OTHER_STAGE_INPUT" },
    { 0x1E80, "SET_TRANSFORM_DATA" },
    { 0x1E90, "LAUNCH_TRANSFORM_PROGRAM" },
    { 0x1E94, "SET_TRANSFORM_EXECUTION_MODE" },
    { 0x1E98, "SET_TRANSFORM_PROGRAM_CXT_WRITE_EN" },
    { 0x1E9C, "SET_TRANSFORM_PROGRAM_LOAD" },
    { 0x1EA0, "SET_TRANSFORM_PROGRAM_START" },
    { 0x1EA4, "SET_TRANSFORM_CONSTANT_LOAD" },
};

/* Method arrays (xemu's NV097_*(i) macros): an address inside one is named
 * after its first word. The light and stage blocks are folded onto light /
 * stage 0 below first. */
static const struct { uint16_t base, bytes; } s_arrays[] = {
    { 0x0260, 32 }, { 0x02C0, 32 }, { 0x02E0, 32 }, { 0x03A8, 12 },
    { 0x0420, 16 }, { 0x0440, 64 }, { 0x0480, 256 }, { 0x0580, 256 },
    { 0x0680, 64 }, { 0x06C0, 256 }, { 0x0840, 16 }, { 0x0850, 16 },
    { 0x0860, 16 }, { 0x0870, 16 }, { 0x09C0, 12 }, { 0x09D0, 16 },
    { 0x09E0, 24 }, { 0x0A10, 12 }, { 0x0A20, 16 }, { 0x0A30, 32 },
    { 0x0A50, 16 }, { 0x0A60, 32 }, { 0x0A80, 32 }, { 0x0AA0, 32 },
    { 0x0AC0, 32 }, { 0x0AE0, 16 }, { 0x0AF0, 16 }, { 0x0B00, 128 },
    { 0x0B80, 128 }, { 0x0C00, 12 }, { 0x0C0C, 12 }, { 0x0C18, 12 },
    { 0x1000, 12 }, { 0x100C, 12 }, { 0x1018, 12 }, { 0x1028, 12 },
    { 0x1034, 12 }, { 0x1040, 12 }, { 0x104C, 16 }, { 0x105C, 12 },
    { 0x1068, 12 }, { 0x1500, 12 }, { 0x1518, 16 }, { 0x1530, 12 },
    { 0x1540, 8 }, { 0x1550, 16 }, { 0x1560, 12 }, { 0x1570, 16 },
    { 0x1580, 12 }, { 0x1590, 8 }, { 0x15A0, 16 }, { 0x15B0, 8 },
    { 0x15B8, 8 }, { 0x15C8, 16 }, { 0x15D8, 8 }, { 0x15E0, 8 },
    { 0x15F0, 16 }, { 0x1600, 8 }, { 0x1608, 8 }, { 0x1620, 16 },
    { 0x1630, 8 }, { 0x16A0, 8 }, { 0x16B0, 12 }, { 0x16C0, 16 },
    { 0x1720, 64 }, { 0x1760, 64 }, { 0x17E0, 12 }, { 0x181C, 12 },
    { 0x1880, 128 }, { 0x1900, 64 }, { 0x1940, 64 }, { 0x1980, 128 },
    { 0x1A00, 256 }, { 0x1B28, 16 }, { 0x1E28, 24 }, { 0x1E20, 8 },
    { 0x1E40, 32 }, { 0x1E80, 16 },
};

const char *nv2a_method_name(uint32_t method)
{
    size_t lo = 0, hi = sizeof s_methods / sizeof s_methods[0];

    method &= 0x1FFCu;
    /* Repeating blocks first: the per-light, per-back-light and per-stage
     * registers name only light 0 / stage 0 in the table. */
    if (method >= 0x1000 && method < 0x1400)
        method = 0x1000 + (method & 0x7F);
    else if (method >= 0x0C00 && method < 0x0E00)
        method = 0x0C00 + (method & 0x3F);
    else if (method >= 0x1B00 && method < 0x1C00)
        method = 0x1B00 + (method & 0x3F);
    else if (method >= 0x03C0 && method < 0x0400)
        method = 0x03C0 + (method & 0x0F);
    else if (method >= 0x0840 && method < 0x0940)
        method = 0x0840 + (method & 0x3F);
    {
        size_t i;
        for (i = 0; i < sizeof s_arrays / sizeof s_arrays[0]; i++)
            if (method >= s_arrays[i].base && method < s_arrays[i].base + s_arrays[i].bytes) {
                method = s_arrays[i].base;
                break;
            }
    }
    while (lo < hi) {                       /* exact match */
        size_t mid = (lo + hi) / 2;
        if (s_methods[mid].method == method)
            return s_methods[mid].name;
        if (s_methods[mid].method < method)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* xemu psh.c PS_TEXTUREMODES_*, as gap names. */
static const char *const s_modes[] = {
    "texture stage mode 0x00 NONE",
    "texture stage mode 0x01 PROJECT2D",
    "texture stage mode 0x02 PROJECT3D",
    "texture stage mode 0x03 CUBEMAP",
    "texture stage mode 0x04 PASSTHRU",
    "texture stage mode 0x05 CLIPPLANE",
    "texture stage mode 0x06 BUMPENVMAP",
    "texture stage mode 0x07 BUMPENVMAP_LUM",
    "texture stage mode 0x08 BRDF",
    "texture stage mode 0x09 DOT_ST",
    "texture stage mode 0x0A DOT_ZW",
    "texture stage mode 0x0B DOT_RFLCT_DIFF",
    "texture stage mode 0x0C DOT_RFLCT_SPEC",
    "texture stage mode 0x0D DOT_STR_3D",
    "texture stage mode 0x0E DOT_STR_CUBE",
    "texture stage mode 0x0F DPNDNT_AR",
    "texture stage mode 0x10 DPNDNT_GB",
    "texture stage mode 0x11 DOTPRODUCT",
    "texture stage mode 0x12 DOT_RFLCT_SPEC_CONST",
};

const char *nv2a_stage_mode_name(uint32_t mode)
{
    return mode < sizeof s_modes / sizeof s_modes[0] ? s_modes[mode] : NULL;
}

/* ---- the registry -------------------------------------------------------
 * Keyed by the name's pointer (open addressing, NV_GAP_SLOTS): a batch
 * notes a handful of gaps at most, and a lookup is a hash and a compare.
 * Two pointers with the same text (a literal in two files) are merged by
 * nv2a_gap_count and nv2a_gap_list's callers only through the text. */
#define NV_GAP_SLOTS 256u                   /* power of two */
#define NV_GAP_MAX   128
static struct { const char *name; uint64_t count; } s_gap[NV_GAP_MAX];
static int s_ngap;
static uint8_t s_slot[NV_GAP_SLOTS];        /* index + 1 into s_gap */

void nv2a_gap_note(const char *name, uint32_t n)
{
    uint32_t h = (uint32_t)(((uintptr_t)name >> 3) * 2654435761u) & (NV_GAP_SLOTS - 1);
    uint32_t i;

    if (!name || !n)
        return;
    for (i = 0; i < NV_GAP_SLOTS; i++, h = (h + 1) & (NV_GAP_SLOTS - 1)) {
        if (!s_slot[h]) {
            if (s_ngap >= NV_GAP_MAX)
                return;
            s_gap[s_ngap].name = name;
            s_gap[s_ngap].count = 0;
            s_slot[h] = (uint8_t)++s_ngap;
        }
        if (s_gap[s_slot[h] - 1].name == name) {
            s_gap[s_slot[h] - 1].count += n;
            recomp_status_unsupported(name, n);
            return;
        }
    }
}

uint64_t nv2a_gap_count(const char *name)
{
    uint64_t c = 0;
    int i;

    for (i = 0; i < s_ngap; i++)
        if (!strcmp(s_gap[i].name, name))
            c += s_gap[i].count;
    return c;
}

int nv2a_gap_list(const char **names, uint64_t *counts, int max)
{
    uint8_t taken[NV_GAP_MAX];
    int i, k, n = 0;

    memset(taken, 0, sizeof taken);
    for (k = 0; k < max && k < s_ngap; k++) {     /* selection: most used first */
        int best = -1;
        for (i = 0; i < s_ngap; i++)
            if (!taken[i] && (best < 0 || s_gap[i].count > s_gap[best].count))
                best = i;
        taken[best] = 1;
        names[n] = s_gap[best].name;
        counts[n++] = s_gap[best].count;
    }
    return n;
}

void nv2a_gap_reset(void)
{
    memset(s_gap, 0, sizeof s_gap);
    memset(s_slot, 0, sizeof s_slot);
    s_ngap = 0;
}
