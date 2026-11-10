/* Hand-written replacements for generated functions (Fable: The Lost Chapters).
 *
 * tools.recomp --exclude-manual (tools.prepare passes this file) skips generating the
 * bodies defined here, so every caller, direct or indirect, reaches these. */
#define RECOMP_GENERATED_CODE   /* register names (eax, esp...) as in generated code */
#include "recomp/gen/recomp_types.h"

/* XMVDecoder_GetNextFrame(IXMVDecoder *, IDirect3DSurface8 *, XMVRESULT *,
 *                         REFERENCE_TIME *)  -- stdcall, 16 bytes of args.
 *
 * The XMV decoder never produced a frame here, so the intro-movie loop
 * (sub_0003B530, lionhead_logo.xmv) polled XMV_NOFRAME (0) and slept for ever.
 * Report XMV_ENDOFFILE (2) instead: every movie ends at once and the title
 * moves on. Remove when XMV playback works. */
void sub_006440AB(void)
{
    uint32_t result_ptr = MEM32(esp + 0x0C);
    uint32_t time_ptr = MEM32(esp + 0x10);

    if (result_ptr)
        MEM32(result_ptr) = 2;           /* XMV_ENDOFFILE */
    if (time_ptr) {
        MEM32(time_ptr) = 0;
        MEM32(time_ptr + 4) = 0;
    }
    eax = 0;                             /* S_OK */
    esp += 4 + 16;                       /* ret 0x10 */
}
