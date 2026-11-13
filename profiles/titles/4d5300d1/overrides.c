/* Hand-written replacements for generated functions (Fable: The Lost Chapters):
 * the optional 16:9 enhancement's entry hooks. The title runs without them
 * (--no-title-layer / XBA_NO_TITLE_LAYER=1, the launcher's « Enhancements » off).
 *
 * tools.recomp --exclude-manual (tools.prepare passes this file) skips generating the
 * bodies defined here, so every caller, direct or indirect, reaches these. */
#define RECOMP_GENERATED_CODE   /* register names (eax, esp...) as in generated code */
#include "recomp/gen/recomp_types.h"
#include <stdio.h>
#include <string.h>

/* Fibers: sub_0004B440, Fable's context switch (cli; push ebx, ebp, esi,
 * edi, fs:[0]; xchg esp, [0x00990A8C]; pop them; sti; ret), was replaced here
 * by host-stack fibers until XBA-101. The lifter and the runtime now carry it
 * like any guest stack switch, from the generated body: each fiber gets a host
 * stack, and since Fable copies every fiber's stack bytes into one shared area
 * (all of them park at the same esp, returning to the same yield), the one to
 * resume is told apart by those bytes. docs/technical/stack-switching.md. */

/* D3D's flush kick, sub_008556D0 (the wait the main thread spun in), was
 * replaced here until profile-free WP6: the lifter marks such poll loops, and
 * xbox_GuestPollWait (xbox_Nv2aKickWait for this flush bit) sleeps in them for
 * every XDK D3D (RECOMP_POLL_WAIT, docs/runtime/environment-variables.md), so
 * the generated body is used again. */

/* ── 16:9: the camera's display-aspect override and the cutscene bars ──────
 *
 * Fable has no widescreen setting of its own (it ignores the dashboard's
 * XC_VIDEO_FLAGS_WIDESCREEN and always renders 640x480), but its camera setup
 * sub_00132D00, called for every scene pass, already takes a display size:
 *
 *     w, h = viewport; p = [0x009A1188]
 *     if p && float[p+8] > 0 && float[p+0xC] > 0: w, h = float[p+8], float[p+0xC]
 *     aspect = w / h                                   -> cam+0xD0
 *     t = ((0.75 - h/w) * 0.5 + 1) * fov * 0.5; xs = cot(t)  -> cam+0xD4
 *     ys = xs * aspect                                 -> cam+0xD8
 *
 * (a camera with an explicit fov, byte [param+0x54], is left alone).
 * [0x009A1188] stays 0 in normal play; with 16 x 9 there the scene is drawn
 * anamorphic: squeezed into 640x480 so that it has the right proportions
 * once the presenter stretches the frame to 16:9 (xbox_FramebufferWindowSetWide).
 * At fov 60 that is 65.6 x 39.9 degrees instead of 60 x 46.8. The HUD, text
 * and minimap are 2D and come out 1.33x wider.
 *
 * In-engine cutscenes ask for 16:9 letterbox bars: sub_003C0070 calls the
 * thiscall sub_0023EB70(0.5, 0.5, 16/9) at 0x003C01A4 (ret 12; the AutoSave
 * cutscene comes through sub_00303170, which tail-jumps to it), which stores
 * the target aspect at this+0x8C, and the bars are drawn over the full-frame
 * picture. On a 16:9 screen they are not wanted: 4/3 there keeps the picture
 * full and the subtitles and Skip prompt where they were.
 *
 * Both are entry hooks (tools/recomp/manual_scan.py), so they need the
 * generated code regenerated with this file (--exclude-manual). They run
 * only once title.c has set g_fable_aspect_wide (fable_configure). */
int g_fable_aspect_wide;

#define FABLE_ASPECT_CFG 0x009A1188u    /* display-size override, read by sub_00132D00 */

extern uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

void sub_00132D00_enter(void)
{
    static uint32_t blob, last;
    uint32_t cfg;

    if (!g_fable_aspect_wide)
        return;
    cfg = MEM32(FABLE_ASPECT_CFG);
    if (!cfg) {
        /* The game's writers (sub_00125A20, sub_00125ED0) can point it at a
         * structure of their own; otherwise it gets a zeroed blob of ours. */
        if (!blob) {
            blob = xbox_HeapAlloc(64, 16);
            if (!blob)
                return;
            memset((void *)XBOX_PTR(blob), 0, 64);
        }
        cfg = blob;
        MEM32(FABLE_ASPECT_CFG) = cfg;
    }
    if (cfg != last) {
        fprintf(stderr, "[ASPECT] camera display size 16 x 9 at 0x%08X%s\n",
                cfg, cfg == blob ? " (ours)" : "");
        last = cfg;
    }
    MEMF(cfg + 8) = 16.0f;
    MEMF(cfg + 0xC) = 9.0f;
}

void sub_0023EB70_enter(void)
{
    /* thiscall, before its push ecx: [esp] return, [esp+4] and [esp+8] the
     * two 0.5 arguments, [esp+0xC] the target aspect. */
    static int logged;
    float a;

    if (!g_fable_aspect_wide)
        return;
    a = MEMF(esp + 0xC);
    if (a > 1.7f && a < 1.8f) {
        MEMF(esp + 0xC) = 4.0f / 3.0f;
        if (!logged) {
            fprintf(stderr, "[ASPECT] cutscene bars 16:9 -> 4:3 (this 0x%08X, from 0x%08X)\n",
                    ecx, MEM32(esp));
            logged = 1;
        }
    }
}
