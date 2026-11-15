/* XMV movies decoded by the runtime (XBA-70, docs/runtime/xmv.md).
 *
 * A title plays its movies with the XDK's XMV library, linked into the title
 * and lifted like the rest of it. Where that lifted decoder cannot run, a
 * title layer replaces the library's XMVDecoder_GetNextFrame with
 * xmv_hle_get_next_frame (in its overrides.c, the way Fable's intro movies
 * were once skipped): the runtime then decodes the movie the title opened
 * (src/video/xmv: XMV demuxer, WMV2 video, Xbox ADPCM audio), paces it on the
 * host clock, writes each picture into the title's surface and plays the
 * sound. RECOMP_XMV=skip turns every movie into "end of file" (the old skip
 * override); a movie the runtime cannot open ends at once the same way.
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

#ifdef __cplusplus
}
#endif

#endif
