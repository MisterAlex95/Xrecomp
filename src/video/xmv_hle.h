/* XMV movies decoded by the runtime (XBA-70, docs/runtime/xmv.md).
 *
 * A title plays its movies with the XDK's XMV library, linked into the title
 * and lifted like the rest of it. tools.prepare finds the library's
 * XMVDecoder_GetNextFrame in the XBE (tools/xmv/find_api.py, XBA-199) and
 * the lifter binds xmv_hle_get_next_frame in its place (generated
 * recomp_xmv_bind.c; a developer's overrides.c can do the same by hand, the
 * way Fable's intro movies were once skipped): the runtime then decodes the movie the title opened
 * (src/video/xmv: XMV demuxer, WMV2 video, Xbox ADPCM audio), paces it on the
 * host clock, writes each picture into the title's surface and plays the
 * sound. Bound by Prepare, a movie the runtime would not decode exactly (X8
 * intra frames, WMA or 5.1 sound, damage) or cannot open goes to the title's
 * own decoder instead (xmv_hle_route, with a watchdog for a hang in it).
 * RECOMP_XMV=skip turns every movie into "end of file" (the old skip
 * override); called directly, a movie the runtime cannot open ends the same
 * way.
 *
 * Every host: the decoder is portable C; audio goes out through the host
 * layer on POSIX hosts (none on Windows yet). */
#ifndef XBOX_XMV_HLE_H
#define XBOX_XMV_HLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* XMVRESULT */
#define XMV_NOFRAME    0u
#define XMV_NEWFRAME   1u
#define XMV_ENDOFFILE  2u
#define XMV_FAIL       3u

/* The kernel calls this when the title opens a file whose name ends in
 * ".xmv" (the host path): the movie the next new decoder plays. */
void xbox_MovieFileOpened(const char *host_path);

/* XMVDecoder_GetNextFrame(decoder, surface, &result, &timestamp), stdcall,
 * four guest pointers: writes XMVRESULT at result_va (if not 0) and the
 * frame's time in 100 ns units (64-bit) at time_va (if not 0). Returns the
 * HRESULT for eax (S_OK). A title override:
 *
 *     void sub_XXXXXXXX(void)            // XMVDecoder_GetNextFrame
 *     {
 *         eax = xmv_hle_get_next_frame(MEM32(esp + 4), MEM32(esp + 8),
 *                                      MEM32(esp + 12), MEM32(esp + 16));
 *         esp += 4 + 16;                 // ret 0x10
 *     }
 */
uint32_t xmv_hle_get_next_frame(uint32_t decoder_va, uint32_t surface_va,
                                uint32_t result_va, uint32_t time_va);

/* XMVDecoder_CloseDecoder: forget the decoder's movie. */
void xmv_hle_close(uint32_t decoder_va);

/* The binding tools.prepare generates (XBA-199, tools.recomp --xmv-bind:
 * recomp_xmv_bind.c) asks this on each call of the bound function: what
 * 0 = XMVDecoder_GetNextFrame, 1 = XMVDecoder_CloseDecoder, at guest va, found
 * with confidence_pct / 100. Returns 1 to use the runtime's decoder, 0 for the
 * title's own lifted body (RECOMP_XMV=native). Logs what was bound once. */
int xmv_hle_bound(int what, uint32_t va, unsigned confidence_pct);

/* The binding's XMVDecoder_GetNextFrame asks this on each call, with the
 * decoder object (its first argument): 1 = call xmv_hle_get_next_frame, 0 =
 * call the title's own lifted body. 0 with RECOMP_XMV=native, and, with
 * RECOMP_XMV=auto, for a movie the runtime would not decode exactly (X8
 * intra frames, WMA or 5.1 sound, damaged data) or cannot open: that movie
 * is handed to the title's own decoder, whole, the decision taken at its
 * first call and logged once. */
int xmv_hle_route(uint32_t va, unsigned confidence_pct, uint32_t decoder_va);

/* After the title's own GetNextFrame ran for a movie handed to it: the
 * XMVRESULT it wrote at result_va. A watchdog: no frame for
 * RECOMP_XMV_WATCHDOG seconds (default 10, 0 = off) while the title polls,
 * and the runtime plays the movie instead (nothing of it shown yet) or ends
 * it (XMV_ENDOFFILE from then on). */
void xmv_hle_native_result(uint32_t decoder_va, uint32_t result_va);

#ifdef __cplusplus
}
#endif

#endif
