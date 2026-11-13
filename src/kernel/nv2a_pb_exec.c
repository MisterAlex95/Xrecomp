/**
 * Execute the parts of the title's pushbuffer that produce visible pixels.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; without
 * something consuming them the framebuffer stays whatever it was, which is how
 * a fully booted title renders a black screen. This walks the same command
 * stream nv2a_pb_scan.c surveys and carries out the subset that decides what is
 * on screen: which surface is being drawn into, and clearing it.
 *
 * It also rasterises geometry, but only the part that can be drawn honestly:
 * batches whose attribute 0 is already in screen space, flat-shaded, straight
 * into the same guest framebuffer the clear writes. Titles draw their UI, HUD
 * and 2D overlays that way, so it is the first geometry to appear. Batches that
 * need a vertex program executed are counted and skipped rather than drawn
 * somewhere wrong -- see raster_batch(). Texturing, depth and vertex programs
 * are still a renderer, not a command decoder; the upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC. RECOMP_RASTER_TEST draws one known triangle
 * after every clear, which separates "the pixel path is broken" from "the title
 * has not given us any vertices". RECOMP_FB_DUMP=<prefix> writes the surface to
 * <prefix>NNN.bmp, so the result can be looked at without a display.
 */
#if defined(__ANDROID__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* sched_getaffinity / CPU_COUNT: the raster pool size */
#endif
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "xbox_memory_layout.h"   /* xbox_Nv2aFrameCounterFlip */
#include "nv2a_flip_hold.h"
#include "frame_pacing.h"     /* RECOMP_PACING=auto */
#include "recomp_status.h"   /* RECOMP_STATUS: frames for the compat runner */
#include "platform/host_cpus.h"   /* recomp_host_cpus_track (XBA-50) */
#include "platform/host_time.h"   /* xbox_HostNowNs */
#ifdef XBOXRECOMP_LSFG
#include "framegen/framegen_policy.h"   /* frame generation's cap rule */
#endif
#if !defined(_WIN32)
#include <pthread.h>              /* the raster worker pool */
#include <unistd.h>               /* sysconf(_SC_NPROCESSORS_ONLN) */
#include <sched.h>                /* sched_yield; sched_getaffinity on Android */
#endif
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "../d3d/d3d8_swizzle.h"
#include "nv2a_vsh_interp.h"
#include "nv2a_combiner.h"
#include "nv2a_backend.h"
#if defined(_WIN32)
#include <windows.h>
#define NV_TLS __declspec(thread)
#else
#define NV_TLS __thread
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowSetAA(uint32_t sy);
extern uint32_t xbox_Nv2aFenceWordVa(void);
extern int nv2a_pb_dma_walk(void);   /* nv2a_pb_scan.c: RECOMP_PB_WALK=dma */
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Would writing this surface land on the title's own image?
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset within the colour DMA object,
 * not a guest virtual address, and this executor has always used it as one.
 * That is harmless while the two happen to agree and catastrophic when they do
 * not: the Xbox Dashboard names surface 0x00088000 at 1280x960x4, so clearing
 * it wrote 4.9 MB of opaque black from 0x00088000 to 0x00538000 -- straight
 * over its own code, its D3D context at 0x000BBFC0 and the register-block
 * pointer at 0x000BE2C4. The symptom was a title that submitted one perfect
 * frame and then spun forever in a pushbuffer-full loop, three layers away,
 * with every D3D global reading 0xFF000000: the clear colour.
 *
 * So refuse, and say so. Getting the address right needs the DMA object base
 * this ignores (NV097_SET_CONTEXT_DMA_COLOR); until that exists, writing
 * nothing is strictly better than writing over the guest, and a title that
 * cannot draw is easier to debug than one that has been overwritten.
 */
static int guest_span_ok(uint64_t addr, uint32_t bytes);

static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!bytes)
        return 0;
    /* The thread stacks sit just above the image. No surface lives there, so
     * a DMA offset that reads as a VA in that range is just as wrong as one
     * that lands on the image. */
    if (base < XBOX_HEAP_BASE && (uint64_t)base + bytes > XBOX_STACK_BASE)
        return 1;
    /* And anything that is not guest memory at all: a clear wrote to
     * 0xC532311B, an offset that names no memory the guest owns. */
    if (!guest_span_ok(base, bytes))
        return 1;
    if (!g_xbox_image_hi)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

/* Where a DMA-object offset actually lives.
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset inside the colour DMA object,
 * and for a framebuffer that object covers physical memory -- so the offset
 * is a physical address, not a guest VA. Those are the same number in this
 * runtime, which is why treating it as a VA works until it does not: on
 * hardware the image is mapped at VA 0x00010000 from arbitrary physical
 * pages, so a framebuffer at physical 0x84000 does not overlap it. Here it
 * would.
 *
 * The title tells us which it is by where it allocated. Half-Life 2's
 * framebuffer comes from MmAllocateContiguousMemory, which this runtime
 * serves from the window at XBOX_CONTIG_BASE, so physical P is visible at
 * XBOX_CONTIG_BASE + P -- clear of the image, and the same bytes the title's
 * own writes and the framebuffer window reach.
 *
 * So: use the offset as a VA when that is credible, and fall back to the
 * physical mirror exactly when it is not. Titles whose surfaces already sit
 * in ordinary RAM (Wreckless renders to the tiled alias of physical
 * 0x01954000) keep the first path and are unaffected.
 */
static uint32_t dma_resolve(uint32_t offset)
{
    extern uint32_t xbox_ContiguousAllocatedBytes(void);

    /* Did this runtime hand the offset out as contiguous memory? Then the
     * bytes live in the window, and that is not a guess: the arena is a bump
     * allocator from XBOX_CONTIG_BASE, so everything below its high-water
     * mark is memory some MmAllocateContiguousMemory call returned. The
     * title's own writes go through the window, so the executor's must too.
     *
     * Checking this BEFORE the image test is the whole point. The image test
     * only catches an offset that would land on the title's code, and whether
     * it does is an accident of where the image happens to end: Half-Life 2's
     * colour surface is physical 0x00A6C000, which clears the image by 700 KB.
     * So it looked like an ordinary VA, and the executor cleared 1.2 MB of
     * black straight through the guest heap -- which faulted the title three
     * frames later on a pointer that had been overwritten, while the real
     * framebuffer at 0x80A6C000 stayed untouched and the screen stayed black. */
    if (offset < xbox_ContiguousAllocatedBytes())
        return XBOX_CONTIG_BASE + offset;
    if (!surface_hits_image(offset, 1))
        return offset;
    if ((uint64_t)offset < XBOX_CONTIG_SIZE)
        return XBOX_CONTIG_BASE + offset;
    return offset;                         /* nothing better to offer */
}

static int surface_write_refused(uint32_t base, uint32_t bytes, const char *what)
{
    static int said;

    if (!surface_hits_image(base, bytes))
        return 0;
    if (!said) {
        said = 1;
        fprintf(stderr,
                "  [GPU] REFUSING to %s surface 0x%08X..0x%08X: that overlaps "
                "the loaded image (0x%08X..0x%08X), a thread stack, or is "
                "outside guest memory.\n"
                "  [GPU]   SET_SURFACE_COLOR_OFFSET is a DMA-object offset, not "
                "a guest VA, and this executor treats it as one. Writing here "
                "would destroy the title's own code and globals.\n",
                what, base, base + bytes, g_xbox_image_lo, g_xbox_image_hi);
        fflush(stderr);
    }
    return 1;
}

/* NV097 methods this executor acts on. */
/* Blending. The pair this title programs, read from its own pushbuffer
 * rather than guessed: BLEND_ENABLE written 1168 times and left on,
 * SFACTOR 0x0302 (SRC_ALPHA) and DFACTOR 0x0303 (ONE_MINUS_SRC_ALPHA).
 * ALPHA_TEST_ENABLE is written 390 times and left at zero, so this is
 * blending and not an alpha test. */
#define NV097_SET_BLEND_ENABLE            0x0304
#define NV097_SET_BLEND_FUNC_SFACTOR      0x0344
#define NV097_SET_BLEND_FUNC_DFACTOR      0x0348
#define NV_BLEND_SRC_ALPHA                0x0302
#define NV_BLEND_ONE_MINUS_SRC_ALPHA      0x0303
#define NV097_SET_BLEND_COLOR             0x034C
#define NV097_SET_BLEND_EQUATION          0x0350

#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720   /* +i*4, 16 attributes */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760   /* +i*4 */
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_SET_TEXTURE_OFFSET          0x1B00   /* +i*0x40 */
#define NV097_SET_TEXTURE_FORMAT          0x1B04
#define NV097_SET_TEXTURE_PALETTE         0x1B20   /* offset | size<<2 | dma */
#define NV097_SET_TEXTURE_ADDRESS         0x1B08
#define NV097_SET_TEXTURE_CONTROL1        0x1B10
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1C
/* The buffer flip. A title double-buffers by telling the GPU which buffer
 * the CRTC reads and which it draws into, advancing the write index and
 * then stalling until the flip has happened. Ignoring these means the
 * stall never clears: Half-Life 2's loader submits its initialisation,
 * asks for a flip, and waits for it in a loop that makes no kernel calls
 * and burns no dispatch, which reads as a hang with no cause.
 *
 * The flip completes here at once, because there is no scanout to be in the
 * middle of; what the console's stall did to the title (wait for the vblank
 * the swap asked for) is the flip hold's job: FLIP_STALL arms it, and the
 * ack loop holds the next walk until the guest's vblank count allows the
 * flip (nv2a_flip_hold.h). The swap's interval comes from the NO_OPERATION
 * D3D pushes before it, software method 1. */
#define NV097_NO_OPERATION                0x0100
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800
/* Draw a run of vertices straight out of the arrays, with no index list:
 * bits 0..23 are the first vertex, bits 24..31 the count minus one. It may
 * appear several times inside one BEGIN_END to draw a longer run. */
#define NV097_DRAW_ARRAYS                 0x1810
#define NV097_INLINE_ARRAY                0x1818
/* Immediate-mode vertices. SET_VERTEX3F/4F carry the position, and writing
 * its last component completes a vertex using whatever the SET_VERTEX_DATA*
 * registers currently hold for the other attributes. This is how Half-Life
 * 2's Xbox loader and the game's own 2D drawing submit every quad -- neither
 * uses INLINE_ARRAY -- so without these the executor saw SET_BEGIN_END pairs
 * with nothing attached and reported `draws 0` while a million and a half
 * textured quads a minute went past it. */
#define NV097_SET_VERTEX3F                0x1500   /* +0..0x08, 3 floats */
#define NV097_SET_VERTEX4F                0x1518   /* +0..0x0C, 4 floats */
#define NV097_SET_VERTEX_DATA2F_M         0x1880   /* + attr*8,  2 floats */
#define NV097_SET_VERTEX_DATA4F_M         0x1A00   /* + attr*16, 4 floats */
#define NV097_SET_VERTEX_DATA4UB          0x1940   /* + attr*4,  4 x u8 */
#define NV097_SET_VERTEX_DATA2S           0x1900   /* + attr*4,  2 x s16 */
#define NV097_SET_VERTEX_DATA4S_M         0x1980   /* + attr*8,  4 x s16 */

/* One immediate vertex, as this file packs it for the shared draw path:
 * position float4, diffuse D3DCOLOR, texcoord0 float2. */
/* An immediate-mode vertex: all 16 attributes as float4. */
#define IMM_VERTEX_DWORDS (NV_VERTEX_ATTRS * 4)

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
#define NV_MAX_INDICES  65536
#define NV_MAX_INLINE   65536           /* dwords of INLINE_ARRAY per batch */

/* Texture stage 0, decoded from what the title programmed.
 *
 * Only stage 0: it is the only one the dashboard configures, and a stage
 * nothing writes to is a stage nothing can be sampled from. The rest arrive
 * as unhandled methods and are counted as such, which is how the next title
 * that needs them will say so. */
typedef struct {
    uint32_t offset;                    /* guest address of texel (0,0)  */
    uint32_t width, height;             /* from IMAGE_RECT               */
    uint32_t pitch;                     /* bytes per row, from CONTROL1  */
    uint32_t color;                     /* NV097 colour-format code      */
    uint32_t addr_u, addr_v;            /* wrap mode per axis            */
    uint32_t palette;                   /* guest address of the CLUT, P8 */
    uint32_t levels;                    /* mip levels, from FORMAT       */
    uint32_t filter;                    /* SET_TEXTURE_FILTER            */
    uint32_t border;                    /* SET_TEXTURE_BORDER_COLOR      */
    int      border_color;              /* FORMAT: BORDER reads `border` */
    int      cube;                      /* FORMAT: six faces, not one    */
    int      valid;
} Texture;

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint32_t   idx[NV_MAX_INDICES];     /* DRAW_ARRAYS starts are 24-bit */
    uint32_t   idx_count;
    /* INLINE_ARRAY payload: vertices written straight into the pushbuffer
     * instead of into a buffer the title points at. Same vertex format, a
     * different place to read them from. */
    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;
    /* Current values of the immediate-mode attributes, and how many complete
     * vertices they have produced in this batch. */
    /* Immediate mode: every attribute's current value, as the SET_VERTEX
     * methods leave it. Writing attribute 0 (position) emits a vertex. */
    float      imm_attr[NV_VERTEX_ATTRS][4];
    uint16_t   imm_used;                /* attributes written since BEGIN */
    uint32_t   imm_count;
    int        inline_active;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;
    /* The surface the last batch actually drew into. A double-buffered title
     * has already pointed color_offset at the next buffer and cleared it by
     * the time the flip arrives, so dumping the current one dumps the frame
     * that has not been drawn yet -- which is how a correctly rendered
     * sequence came out as 12 black BMPs. */
    uint32_t drawn_offset;
    /* ...and the shape of that surface. A title also draws into small
     * render targets (Burnout 3: 128x128 shadow/reflection maps, pitch 512),
     * often after the main scene, so "last surface drawn" alone picked one of
     * those and read it with the framebuffer's pitch: horizontal noise. The
     * biggest surface drawn since the last flip is the one being presented. */
    uint32_t drawn_pitch, drawn_x, drawn_y, drawn_w, drawn_h, drawn_bpp;
    uint32_t drawn_format;  /* SET_SURFACE_FORMAT of that surface */
    int drawn_stale;      /* set at a flip: the next draw starts a new frame */
    uint64_t pixels;
    uint32_t pixel_max;   /* brightest value any pixel write carried */
    uint32_t clip_x, clip_w, clip_y, clip_h;       /* real (anti-aliased) pixels */
    uint32_t clip_raw_h, clip_raw_v;    /* SET_SURFACE_CLIP_* as sent: logical */
    float    aa_sx, aa_sy;              /* anti-aliasing scale of the surface */
    uint32_t clear_color;
    uint32_t clears, unhandled_total;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    /* Why a batch came out flat. "Untextured" has two causes that look
     * identical on screen and want opposite fixes: the batch carried no
     * texture coordinates, or it did and the stage was not usable. */
    uint32_t batches_textured, batches_no_uv, batches_no_tex;
    uint32_t blend_enable, blend_sfactor, blend_dfactor;
    uint32_t blend_equation, blend_color;
    uint32_t color_mask;        /* SET_COLOR_MASK: A<<24 R<<16 G<<8 B */
    /* Visibility tests (D3D's Begin/EndVisibilityTest): pixels that pass the
     * depth test while counting is on, reported by GET_REPORT. */
    uint32_t zpass_enable, zpass_count, reports;
    uint32_t blend_pairs[16];   /* sfactor<<16 | dfactor seen, for the report */
    int blend_npairs;
    Texture  texs[4];                   /* one per texture stage */
    /* Register combiners and the per-pixel state around them. rc_seen: the
     * title has programmed the combiners, so they decide every pixel's
     * colour; until then the old "stage 0 texel * diffuse" stands in. */
    Nv2aCombiner rc;
    int      rc_seen;
    uint32_t clip_plane_mode;           /* SET_SHADER_CLIP_PLANE_MODE */
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t fog_enable, fog_mode, fog_color;
    float    fog_param[2];
    /* Vertex programs (nv2a_vsh_interp.c) and the depth buffer, which
     * together are what a 3D scene needs and a 2D one never used. */
    uint32_t xf_mode;                   /* TRANSFORM_EXECUTION_MODE: 2 = program */
    uint32_t batches_program, verts_program, tris_behind;
    /* Where program-path triangles go, so "nothing drew" has a reason. */
    uint32_t xf_degenerate, xf_offscreen, xf_drawn;
    uint64_t xf_depth_fail, xf_pixels;
    float xf_min[3], xf_max[3];
    uint32_t xf_fmt[16];  /* attribute formats seen: type | size<<4 | slot<<8 */
    int xf_nfmt, xf_seeded;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t zeta_offset, zstencil_clear;
    /* SET_CLEAR_RECT_HORIZONTAL / _VERTICAL (x0 | x1 << 16, inclusive):
     * the area CLEAR_SURFACE covers on a swizzled surface (clear_surface).
     * clear_rect_set: bit 0 horizontal, bit 1 vertical sent; the rect
     * applies once both have been. */
    uint32_t clear_rect_h, clear_rect_v;
    int clear_rect_set;
    /* Fixed-function transform: the composite matrix (world * view *
     * projection * viewport, as D3D uploads it) and whether the title has
     * sent one. The viewport offset is the constant-file entry c[59]. */
    float    composite[16];
    int      composite_set;
    uint32_t batches_ffp;
} s_gpu;

/* The last value written to every Kelvin method, as the GPU's register file
 * would hold it. State the executor does not model explicitly (lights,
 * material colours, cull and clip state handed to a back end) is read from
 * here. */
static uint32_t s_reg[0x2000 / 4];
static float reg_f(uint32_t method)
{
    float f;
    memcpy(&f, &s_reg[method / 4], 4);
    return f;
}

/* Render time: wall time the executor spends drawing (batches from vertex
 * fetch to the last pixel, clears, a back end's present sync), per frame.
 * The front end of a title that sleeps between frames runs at its own cap
 * whatever the renderer costs, so fps alone says nothing about the renderer;
 * this does. Reported with the fps line. */
static uint64_t s_render_ns;            /* this frame so far */
static uint64_t s_render_sum_ns, s_render_max_ns;
static uint32_t s_render_frames;

static void render_time_frame(void)
{
    s_render_sum_ns += s_render_ns;
    if (s_render_ns > s_render_max_ns)
        s_render_max_ns = s_render_ns;
    s_render_frames++;
    s_render_ns = 0;
}

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE lands; see nv2a_backend.h.
 *
 * The title hands the address over from its own thread, and D3D can release
 * before that: Fable's glue polls for it every millisecond, and in 2 of 14
 * starts the release that ended D3D's init wait came first, was dropped, and
 * D3D waited for it for ever (0 flips). So the executor keeps the last value
 * released, and a hand-over made while no target was set writes it (a change
 * from one target to another does not: the value belonged to the old one).
 *
 * Two threads, no lock: each side stores its own word, fences, then reads the
 * other's, so at least one of them sees both (the executor sees the target,
 * or the hand-over sees the value). The hand-over rewrites until the value
 * holds still, so a release the executor wrote meanwhile is not overwritten
 * by an older one. */
static volatile uint32_t s_sem_va;
static volatile uint32_t s_sem_last;
static volatile int s_sem_seen;
#if defined(_WIN32)
#define SEM_FENCE() MemoryBarrier()
#else
#define SEM_FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif
void nv2a_pb_set_semaphore_target(uint32_t guest_va)
{
    uint32_t v, old = s_sem_va;

    s_sem_va = guest_va;
    SEM_FENCE();
    if (old || !guest_va || !s_sem_seen)
        return;
    do {
        v = s_sem_last;
        *(volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + guest_va) = v;
        SEM_FENCE();
    } while (s_sem_last != v);
}

/* D3D's software methods other than the swap; see nv2a_backend.h and
 * NV097_NO_OPERATION below. Counted for the report. */
static int (*volatile s_swm_hook)(uint32_t param);
static uint32_t s_swm_serviced, s_swm_ignored[32];
void nv2a_pb_set_software_method_hook(int (*hook)(uint32_t param)) { s_swm_hook = hook; }

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count, last_param; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

/* Every texture-stage register, as the title last set it.
 * Texturing is not implemented yet; knowing which formats and sizes a title
 * actually programs is what decides which ones are worth implementing. */
#define NV_TEX_FIRST 0x1B00
#define NV_TEX_LAST  0x1BFC
static uint32_t s_tex_reg[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];
static uint8_t  s_tex_set[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];

/* Formats whose dimensions come from the format word and whose coordinates
 * arrive normalised, rather than from a pitch and SET_TEXTURE_IMAGE_RECT with
 * coordinates in texels. Swizzled and block-compressed are both in this group,
 * and every place that used to test only for swizzled needs the pair. */
static int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

static void tex_update_valid(Texture *t)
{
    t->valid = t->offset && t->width && t->height
            && (tex_size_from_format(t->color) || t->pitch);
}

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
    /* A pitch is a linear texture's property. A swizzled one has no rows and
     * so no pitch, and requiring one here refused every swizzled texture --
     * which is nearly all of them, since swizzled is the Xbox default. That
     * left the title's own textures unsampled and every textured quad drawn in
     * flat vertex colour. */
    tex_update_valid(&s_gpu.texs[0]);
}

/* Every distinct texture a batch was drawn with, and how many batches used it.
 *
 * The per-draw verbose print shows the first few draws of the first frame,
 * which is enough to see that texturing works at all and not enough to answer
 * "is a font page ever bound". This is the same shape as the unhandled-method
 * table below it: a small set, ranked, printed with the rest of the report. */
#define PB_EXEC_MAX_TEXTURES 64
typedef struct {
    uint32_t offset, color, width, height, batches;
} PbTexUse;
static PbTexUse s_tex_use[PB_EXEC_MAX_TEXTURES];
static int s_tex_use_count;

/* Defined below, next to the sampler it goes through. */
static void dump_texture_bmp(uint32_t seq);

/* Repeat dumps are numbered from well past the first-use sequence, so a
 * listing sorts them after the textures they came from and no first-use file
 * is ever overwritten by one. */
#define TEX_DUMP_SEQ_BASE 1000u
#define TEX_DUMP_SEQ_MAX  40u

static void note_texture_use(void)
{
    int i;

    if (!s_gpu.texs[0].valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == s_gpu.texs[0].offset
         && s_tex_use[i].color  == s_gpu.texs[0].color) {
            s_tex_use[i].batches++;
            /* Dump a surface that is redrawn, every Nth time it is bound.
             *
             * First use alone cannot tell a decode error that is wrong in
             * every frame from one that accumulates across them. A block
             * transform that is wrong is wrong on its own, in the keyframe
             * as much as anywhere; motion compensation that is wrong starts
             * from a clean keyframe and smears further with each predicted
             * frame after it. In a single frame the two look identical, and
             * in a sequence they look nothing alike -- so the sequence is
             * what has to be captured.
             *
             * It belongs on this side of the return: a video surface keeps
             * one address for the whole film, so after the first frame it is
             * only ever found here, and the first-use dump below never fires
             * for it again. RECOMP_TEX_DUMP_EVERY=<n> sets the interval, and
             * RECOMP_TEX_DUMP still names the files. */
            {
                static int every = -1;
                static unsigned binds, seq;
                if (every < 0) {
                    const char *e = getenv("RECOMP_TEX_DUMP_EVERY");
                    every = e ? atoi(e) : 0;
                }
                if (every > 0 && ++binds % (unsigned)every == 0
                    && seq < TEX_DUMP_SEQ_MAX)
                    dump_texture_bmp(TEX_DUMP_SEQ_BASE + seq++);
            }
            return;
        }
    }
    if (s_tex_use_count < PB_EXEC_MAX_TEXTURES) {
        s_tex_use[s_tex_use_count].offset  = s_gpu.texs[0].offset;
        s_tex_use[s_tex_use_count].color   = s_gpu.texs[0].color;
        s_tex_use[s_tex_use_count].width   = s_gpu.texs[0].width;
        s_tex_use[s_tex_use_count].height  = s_gpu.texs[0].height;
        s_tex_use[s_tex_use_count].batches = 1;
        s_tex_use_count++;
        dump_texture_bmp((uint32_t)s_tex_use_count - 1);
    }
}

/* Slot + 1 of each method in s_unhandled: this runs for every method the
 * executor does not model, millions of times a second, so no search. The
 * report sorts s_unhandled and rebuilds the slots afterwards. */
static uint16_t s_unhandled_slot[0x2000 / 4];

static void note_unhandled(uint32_t method, uint32_t param)
{
    uint16_t *slot = &s_unhandled_slot[(method & 0x1FFC) / 4];

    s_gpu.unhandled_total++;
    if (*slot) {
        s_unhandled[*slot - 1].count++;
        s_unhandled[*slot - 1].last_param = param;
        return;
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled[s_unhandled_count].last_param = param;
        s_unhandled_count++;
        *slot = (uint16_t)s_unhandled_count;
    }
}

/* Is [addr, addr + bytes) inside memory the guest owns? That is ordinary RAM
 * from 0, and the contiguous window at XBOX_CONTIG_BASE. The index of a vertex
 * comes from the title's pushbuffer, 32 bits of it, so after a desync it can
 * be anything; this is the one place every vertex read passes through. */
static int guest_span_ok(uint64_t addr, uint32_t bytes)
{
    uint64_t end = addr + bytes;

    if (end <= (uint64_t)XBOX_CONTIG_SIZE)
        return 1;
    return addr >= XBOX_CONTIG_BASE
        && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
}

/* Bytes one element of attribute `a` occupies, as fetch_attr reads it. */
static uint32_t attr_elem_bytes(const VertexAttr *a)
{
    uint32_t n = a->size < 4 ? a->size : 4;

    switch (a->type) {
    case 0: case 6: return 4;
    case 1: case 5: return 2u * n;
    case 4:         return n;
    default:        return 4u * n;
    }
}

/* Read attribute `a` of vertex `index` as floats. Only the float and the
 * normalised-byte types appear in practice; anything else returns 0 so a
 * caller sees a degenerate vertex rather than reading past the array. */
static int decode_attr(uint32_t type, uint32_t size, const uint8_t *p, float out[4]);

static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->size || !a->stride)
        return 0;
    if (s_gpu.inline_active) {
        /* The batch arrived as INLINE_ARRAY, so `offset` is a byte offset into
         * the buffered payload rather than a guest address -- and 0 is a legal
         * one there, which is why the offset test is on the other side of this
         * branch. */
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > (size_t)s_gpu.inline_count * 4)
            return 0;
        p = (const uint8_t *)s_gpu.inline_buf + at;
    } else {
        uint64_t at = (uint64_t)a->offset + (uint64_t)index * a->stride;

        if (!a->offset || !guest_span_ok(at, attr_elem_bytes(a)))
            return 0;
        p = mem + at;
    }
    return decode_attr(a->type, a->size, p, out);
}

/* One element at `p` of an attribute of `type` and `size`, as floats; out
 * already holds 0,0,0,1. 0 for a format the executor does not read. */
static int decode_attr(uint32_t type, uint32_t size, const uint8_t *p, float out[4])
{
    uint32_t i;

    switch (type) {
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    /* The formats 3D geometry uses and screen-space quads never did. */
    case 1:                                  /* signed short, normalised */
        for (i = 0; i < size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i] / 32767.0f;
        return 1;
    case 5:                                  /* signed short, as is */
        for (i = 0; i < size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6: {                                /* packed 11:11:10 normal */
        uint32_t v = *(const uint32_t *)p;
        out[0] = (float)((int32_t)(v << 21) >> 21) / 1023.0f;
        out[1] = (float)((int32_t)(v << 10) >> 21) / 1023.0f;
        out[2] = (float)((int32_t)v >> 22) / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

static uint32_t surface_bpp(void);

static int surface_color_valid(int masked);

static void note_drawn(void)
{
    /* A depth-only pass (a shadow map at colour offset 0) is not the frame:
     * left to count, its 768x768 surface became "the surface drawn" and the
     * flip presented colour offset 0 by luck of the size check below. */
    if (!surface_color_valid(1))
        return;
    if (!s_gpu.drawn_stale && s_gpu.drawn_offset
        && s_gpu.clip_w * s_gpu.clip_h < s_gpu.drawn_w * s_gpu.drawn_h)
        return;
    s_gpu.drawn_stale = 0;
    s_gpu.drawn_offset = s_gpu.color_offset;
    s_gpu.drawn_pitch = s_gpu.pitch;
    s_gpu.drawn_bpp = surface_bpp();
    s_gpu.drawn_format = s_gpu.format;
    s_gpu.drawn_x = s_gpu.clip_x; s_gpu.drawn_y = s_gpu.clip_y;
    s_gpu.drawn_w = s_gpu.clip_w; s_gpu.drawn_h = s_gpu.clip_h;
}

static uint32_t surface_bpp(void)
{
    /* The surface format says it outright (NV097_SET_SURFACE_FORMAT_COLOR).
     * Deriving it from pitch / clip width -- the only way before -- is right
     * only while the clip spans the whole surface: Burnout 3 narrows the clip
     * to a 250-pixel window to draw its option values, 2560 / 250 came out as
     * 10 bytes a pixel, and every such text quad was refused. */
    switch (s_gpu.format & 0xF) {
    case 0x1: case 0x2: case 0x3: case 0xA:
        return 2;
    case 0x4: case 0x5: case 0x6: case 0x7: case 0x8:
        return 4;
    case 0x9:
        return 1;
    default:
        break;
    }
    /* The pitch and the clip width together give the pixel size, which is more
     * reliable than decoding the format field: the format's colour code is
     * only meaningful alongside a type the title also sets, while the pitch is
     * always exactly how many bytes a row occupies. */
    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
}


/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * ponytail: bottom-up 24bpp BMP, no palette, no compression. That is the one
 * format every viewer reads and it is 30 lines; PNG would need a dependency.
 */
/* Set while RECOMP_FB_DUMP_FLIPS is dumping every flip: the report and
 * after-draw dumps stand down then, or they would put duplicate frames into
 * the sequence. */
static int s_flip_dumping;

/* Set by the report (fb_dump_report) for the next FLIP_STALL to dump the
 * frame it presents. */
static int s_dump_at_flip;

/* The back end's present hook (nv2a_backend_register_with_present). */
static int (*s_backend_present)(const Nv2aPresentSurface *now, int sync,
                                Nv2aPresentSurface *ready);

/* What the screen shows when it is not the surface: the video overlay's
 * picture while one is up (nv2a_pb_exec_set_screen_dump). */
static int (*volatile s_screen_dump)(const char *path);

void nv2a_pb_exec_set_screen_dump(int (*dump)(const char *path))
{
    s_screen_dump = dump;           /* set once, before the guest starts */
}

static void dump_surface_bmp(void)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    int drawn = s_gpu.drawn_offset != 0;
    uint32_t offset = drawn ? s_gpu.drawn_offset : s_gpu.color_offset;
    uint32_t pitch = drawn ? s_gpu.drawn_pitch : s_gpu.pitch;
    uint32_t cx = drawn ? s_gpu.drawn_x : s_gpu.clip_x;
    uint32_t cy = drawn ? s_gpu.drawn_y : s_gpu.clip_y;
    static int seq;
    char path[512];
    uint32_t w = drawn ? s_gpu.drawn_w : s_gpu.clip_w;
    uint32_t h = drawn ? s_gpu.drawn_h : s_gpu.clip_h, y, x;
    uint32_t bpp = drawn ? s_gpu.drawn_bpp : surface_bpp();
    /* 16-bit pixels as the presenter reads them (FLIP_STALL): the surface's
     * own format from a back end with a present hook, else R5G6B5. */
    uint32_t cfmt = (drawn ? s_gpu.drawn_format : s_gpu.format) & 0xF;
    int x555 = s_backend_present && (cfmt == 0x1 || cfmt == 0x2);
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (prefix) {
        int (*screen)(const char *) = s_screen_dump;
        if (screen) {
            snprintf(path, sizeof path, "%s%05d.bmp", prefix, seq);
            if (screen(path) > 0) {
                seq++;
                return;
            }
        }
    }
    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !offset)
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%05d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *row = mem + dma_resolve(offset)
                           + (size_t)(cy + y) * pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[cx + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else if (x555) {
                uint16_t v = ((const uint16_t *)row)[cx + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x1F) << 3);
                bgr[2] = (uint8_t)(((v >> 10) & 0x1F) << 3);
            } else {
                uint16_t v = ((const uint16_t *)row)[cx + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2]);

/* The surface rectangle in real pixels.
 *
 * SET_SURFACE_CLIP_* is in logical pixels; with anti-aliasing on, the surface
 * is larger by the AA factor in SET_SURFACE_FORMAT bits 12-15 (0 = 1x1,
 * 1 = 2x1 "center corner 2", 2 = 2x2 "square offset 4"), and so are the
 * coordinates a vertex program or the composite matrix produces. X-Men
 * Legends renders its 640x480 menu into a 1280x960 surface (pitch 5120),
 * which read as an 8-bytes-per-pixel surface and drew nothing until this was
 * applied. */
static void surface_apply_clip(void)
{
    uint32_t aa = (s_gpu.format >> 12) & 0xF;
    uint32_t sx = aa ? 2 : 1, sy = aa == 2 ? 2 : 1;

    s_gpu.aa_sx = (float)sx;
    s_gpu.aa_sy = (float)sy;

    s_gpu.clip_x = (s_gpu.clip_raw_h & 0xFFFF) * sx;
    s_gpu.clip_w = ((s_gpu.clip_raw_h >> 16) & 0xFFFF) * sx;
    s_gpu.clip_y = (s_gpu.clip_raw_v & 0xFFFF) * sy;
    s_gpu.clip_h = ((s_gpu.clip_raw_v >> 16) & 0xFFFF) * sy;
}

/* ---- Render back end (nv2a_backend.h) ------------------------------- */

static const Nv2aBackend *s_backend;
static void (*s_backend_present_sync)(void);

void nv2a_backend_register_with_present_sync(const Nv2aBackend *backend,
                                             void (*present_sync)(void))
{
    s_backend = backend;
    s_backend_present_sync = backend ? present_sync : NULL;
    s_backend_present = NULL;
}

void nv2a_backend_register_with_present(const Nv2aBackend *backend,
                                        int (*present)(const Nv2aPresentSurface *now,
                                                       int sync,
                                                       Nv2aPresentSurface *ready))
{
    s_backend = backend;
    s_backend_present_sync = NULL;
    s_backend_present = backend ? present : NULL;
}

void nv2a_backend_register(const Nv2aBackend *backend)
{
    nv2a_backend_register_with_present_sync(backend, NULL);
}

static int backend_draws(void)
{
    return s_backend && s_backend->draw;
}

/* Is there colour memory to draw into? Not at colour offset 0 (a depth-only
 * pass: Fable renders its shadow map with offset 0 and the colour mask 0),
 * nor where a write would land on the image. `masked`: also not when
 * SET_COLOR_MASK writes no channel (draws; a clear ignores the mask). */
static int surface_color_valid(int masked)
{
    uint32_t base;

    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || !surface_bpp())
        return 0;
    if (masked && !(s_gpu.color_mask & 0x01010101u))
        return 0;
    base = dma_resolve(s_gpu.color_offset);
    return !surface_hits_image(base, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch);
}

/* ---- Visibility reports (GET_REPORT) with a back end ----------------
 *
 * With a back end the pixel count comes from the GPU, and reading it at
 * GET_REPORT waits for the GPU to finish everything queued: Fable makes 341
 * visibility tests a frame, and those waits were a third of the executor's
 * time in-world (and on a tiler each one also ends the render pass).
 *
 * So by default (a back end with zpass_end) a report is deferred: GET_REPORT
 * closes the count and queues the slot; the 16 bytes are written when the
 * count is in. Until then the slot keeps the 0xFFFFFFFF D3D's
 * BeginVisibilityTest wrote at +0xC, which is exactly "not done yet" to the
 * title. Reports are resolved in order, and only what the GPU has finished,
 * without waiting: whenever the walker has caught up with PUT
 * (nv2a_pb_exec_idle), at FLIP_STALL and again after the present (whose
 * read-back has waited for the GPU anyway). CLEAR_REPORT_VALUE closes the
 * count too, and the queue applies it in order, so the counts stay
 * cumulative from the clear exactly as the synchronous path reports them.
 *
 * Not waiting is what titles want: Fable reads frame N-1's results lazily,
 * per object, and returns the cached one while a test is pending. The idle
 * hook used to block until the GPU drained, 9-14 times a frame, 17-22 % of
 * the executor in Oakvale for nothing. But a title can also spin on one
 * result (Fable's sub_0012F7F0) and submit nothing until it has it, so a
 * report at the head of the queue that stays unresolved across idle calls
 * is pushed: after NV_ZP_FLUSH_NS the back end is asked once to submit its
 * queued GPU work (zpass_flush: a GL driver may hold the query in its
 * command buffer for ever), after NV_ZP_BLOCK_NS the executor waits for it.
 * Past NV_ZP_CAP pending the oldest are waited for too.
 *
 * RECOMP_GPU_ZPASS=sync reads synchronously as before; =always runs no
 * queries at all and reports 1 (everything visible); =lazy is the deferred
 * path that never waits or flushes mid-frame: where it would wait, the
 * reports pending then are answered 1 (visible) at once, and the GPU's
 * counts for them are dropped when they come in -- a candidate default for a
 * tiler, where a mid-frame flush ends the render pass. The CPU rasteriser
 * counts its own pixels and is not affected. */
enum { ZPASS_DEFER = 0, ZPASS_SYNC = 1, ZPASS_ALWAYS = 2, ZPASS_LAZY = 3 };

static int zpass_mode(void)
{
    static int mode = -1;

    if (!s_backend || !s_backend->draw)
        return ZPASS_SYNC;                 /* the CPU rasteriser's own count */
    if (mode < 0) {
        const char *e = getenv("RECOMP_GPU_ZPASS");
        mode = e && strcmp(e, "always") == 0 ? ZPASS_ALWAYS
             : e && strcmp(e, "sync") == 0   ? ZPASS_SYNC
             : e && strcmp(e, "lazy") == 0   ? ZPASS_LAZY
             : ZPASS_DEFER;
    }
    if ((mode == ZPASS_DEFER || mode == ZPASS_LAZY)
            && !(s_backend->zpass_end && s_backend->zpass_resolve))
        return ZPASS_SYNC;
    return mode;
}

/* Reports go through the queue (deferred or lazy). */
static int zpass_queued(void)
{
    int mode = zpass_mode();
    return mode == ZPASS_DEFER || mode == ZPASS_LAZY;
}

#define NV_ZP_MAX 1024                     /* ring size */
#define NV_ZP_CAP 512                      /* pending past this: resolve the oldest */
/* The head report unresolved across idle calls for this long: flush, then
 * wait (see above). */
#define NV_ZP_FLUSH_NS (300u * 1000u)
#define NV_ZP_BLOCK_NS (2u * 1000u * 1000u)
typedef struct {
    uint32_t ticket;                       /* from zpass_end; 0: nothing to wait for */
    uint32_t at;                           /* resolved slot address; 0: a CLEAR */
    uint32_t cpu;                          /* the executor's own count at GET_REPORT */
    uint64_t stamp;
} ZpPending;
static ZpPending s_zp[NV_ZP_MAX];
static uint32_t s_zp_head, s_zp_count;
static uint32_t s_zp_answered;             /* lazy: the head entries already answered 1 */
static uint32_t s_zp_acc;                  /* back-end count since the last CLEAR, applied */
/* Per report period, for the log: resolved (at the idle hook, at the flip
 * and after the present), waited for, flushes asked, answered lazily, and
 * the idle hook's time. */
static uint32_t s_zp_resolves, s_zp_blocks;
static uint32_t s_zp_res_idle, s_zp_res_flip, s_zp_flushes, s_zp_lazy, s_zp_waits;
static uint64_t s_zp_idle_ns;
enum { ZP_AT_OTHER, ZP_AT_IDLE, ZP_AT_FLIP };
static int s_zp_at;                        /* where zpass_resolve_queue runs */

/* Write a report slot as the GPU would: stamp, count, and 0 (done) at +0xC. */
static void zpass_write(const ZpPending *e, uint32_t v)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t zero = 0;

    memcpy(mem + e->at, &e->stamp, 8);
    memcpy(mem + e->at + 8, &v, 4);
    memcpy(mem + e->at + 12, &zero, 4);
}

/* Resolve queued reports in order until at most `keep` remain; wait for the
 * GPU only when `block` (otherwise stop at the first count not yet in). */
static void zpass_resolve_queue(int block, uint32_t keep)
{
    int blocked = 0;

    while (s_zp_count > keep || (s_zp_count && !block)) {
        ZpPending *e = &s_zp[s_zp_head];
        uint32_t count = 0;

        if (e->ticket && s_backend && s_backend->zpass_resolve) {
            int must = block && s_zp_count > keep;
            if (must && !blocked) {
                blocked = 1;      /* one wait, however many it resolves */
                s_zp_waits++;
            }
            if (!s_backend->zpass_resolve(e->ticket, must, &count))
                break;                     /* not in yet, and not waiting */
            s_zp_blocks += must;
        }
        if (!e->at) {
            s_zp_acc = 0;                  /* CLEAR_REPORT_VALUE */
        } else {
            s_zp_acc += count;
            if (!s_zp_answered) {          /* else answered already (lazy) */
                zpass_write(e, e->cpu + s_zp_acc);
                s_zp_resolves++;
                s_zp_res_idle += s_zp_at == ZP_AT_IDLE;
                s_zp_res_flip += s_zp_at == ZP_AT_FLIP;
            }
        }
        if (s_zp_answered)
            s_zp_answered--;
        s_zp_head = (s_zp_head + 1) % NV_ZP_MAX;
        s_zp_count--;
    }
}

/* RECOMP_GPU_ZPASS=lazy, instead of waiting: every report pending now and
 * not answered yet is written as visible (count 1 on top of the executor's
 * own); its GPU count is dropped when it resolves. */
static void zpass_answer_lazily(void)
{
    for (; s_zp_answered < s_zp_count; s_zp_answered++) {
        const ZpPending *e = &s_zp[(s_zp_head + s_zp_answered) % NV_ZP_MAX];
        if (e->at) {
            zpass_write(e, e->cpu + 1);
            s_zp_lazy++;
        }
    }
}

static void zpass_queue(uint32_t at, uint32_t cpu, uint64_t stamp)
{
    ZpPending *e;

    if (s_zp_count >= NV_ZP_CAP) {
        zpass_resolve_queue(0, 0);         /* whatever is in already */
        if (s_zp_count >= NV_ZP_CAP)
            zpass_resolve_queue(1, NV_ZP_CAP - 1);
    }
    e = &s_zp[(s_zp_head + s_zp_count) % NV_ZP_MAX];
    e->ticket = s_backend->zpass_end();
    e->at = at;
    e->cpu = cpu;
    e->stamp = stamp;
    s_zp_count++;
}

/* The walker has caught up with PUT: whatever the title may be waiting for
 * is queued. Write what the GPU has finished, without waiting; push a head
 * report that stays unresolved (see "Visibility reports" above). Called by
 * the NV2A poll loop when a tick had nothing new to walk. */
void nv2a_pb_exec_idle(void)
{
    static uint32_t stuck_ticket, stuck_at;    /* the head report seen stuck */
    static uint64_t stuck_ns;
    static int stuck, flushed;
    const ZpPending *e;
    uint64_t t0, now;

    if (!s_zp_count) {
        stuck = 0;
        return;
    }
    t0 = xbox_HostNowNs();
    s_zp_at = ZP_AT_IDLE;
    zpass_resolve_queue(0, 0);
    s_zp_at = ZP_AT_OTHER;
    if (s_zp_answered >= s_zp_count) {         /* nothing left the title waits for */
        stuck = 0;
        s_zp_idle_ns += xbox_HostNowNs() - t0;
        return;
    }
    /* The first report the title still sees pending. */
    e = &s_zp[(s_zp_head + s_zp_answered) % NV_ZP_MAX];
    now = xbox_HostNowNs();
    if (!stuck || e->ticket != stuck_ticket || e->at != stuck_at) {
        stuck = 1;
        flushed = 0;
        stuck_ticket = e->ticket;
        stuck_at = e->at;
        stuck_ns = now;
    } else if (now - stuck_ns >= NV_ZP_BLOCK_NS) {
        if (zpass_mode() == ZPASS_LAZY) {
            zpass_answer_lazily();
        } else {
            s_zp_at = ZP_AT_IDLE;
            zpass_resolve_queue(1, 0);
            s_zp_at = ZP_AT_OTHER;
        }
        stuck = 0;
    } else if (!flushed && now - stuck_ns >= NV_ZP_FLUSH_NS
               && zpass_mode() != ZPASS_LAZY) {
        flushed = 1;
        if (s_backend && s_backend->zpass_flush) {
            s_backend->zpass_flush();
            s_zp_flushes++;
        }
    }
    s_zp_idle_ns += xbox_HostNowNs() - t0;
}

static void current_surface(Nv2aSurface *out)
{
    out->color_va        = dma_resolve(s_gpu.color_offset);
    out->width           = s_gpu.clip_w;
    out->height          = s_gpu.clip_h;
    out->pitch           = s_gpu.pitch;
    out->bytes_per_pixel = surface_bpp();
    out->aa_sx           = s_gpu.aa_sx > 1.5f ? 2 : 1;
    out->aa_sy           = s_gpu.aa_sy > 1.5f ? 2 : 1;
    out->format          = s_gpu.format;
    out->clip_x          = s_gpu.clip_x;
    out->clip_y          = s_gpu.clip_y;
    out->color_valid     = (uint32_t)surface_color_valid(0);
    out->color_masked    = !(s_gpu.color_mask & 0x01010101u);
}

/* A texture stage as nv2a_backend.h describes it. */
static void texture_to_backend(const Texture *t, Nv2aTexture *o)
{
    o->offset = t->offset;
    o->width  = t->width;
    o->height = t->height;
    o->pitch  = t->pitch;
    o->color  = t->color;
    o->addr_u = t->addr_u;
    o->addr_v = t->addr_v;
    o->levels = t->pitch ? 1 : t->levels;
    o->filter = t->filter;
    o->palette = t->palette;
    o->border = t->border;
    o->border_color = (uint32_t)t->border_color;
    o->cube   = (uint32_t)t->cube;
}

/* Render state in the NV2A's own values, gathered from what the executor
 * tracks and from the register mirror for what it does not (culling, the
 * depth range). */
static void current_render_state(Nv2aRenderState *r)
{
    r->blend_enable      = s_gpu.blend_enable;
    r->blend_src         = s_gpu.blend_sfactor;
    r->blend_dst         = s_gpu.blend_dfactor;
    r->blend_eq          = s_gpu.blend_equation;
    r->blend_color       = s_gpu.blend_color;
    r->alpha_test_enable = s_gpu.alpha_test;
    r->alpha_func        = s_gpu.alpha_func;
    r->alpha_ref         = s_gpu.alpha_ref;
    r->depth_test_enable = s_gpu.depth_test;
    r->depth_func        = s_gpu.depth_func;
    r->depth_write       = s_gpu.depth_mask;
    r->color_mask        = s_gpu.color_mask;
    r->cull_enable       = s_reg[0x0308 / 4];        /* SET_CULL_FACE_ENABLE */
    r->cull_face         = s_reg[0x039C / 4];        /* SET_CULL_FACE */
    r->front_face        = s_reg[0x03A0 / 4];        /* SET_FRONT_FACE */
    r->zeta_va           = s_gpu.zeta_offset ? dma_resolve(s_gpu.zeta_offset) : 0;
    r->depth_min         = reg_f(0x0394);            /* SET_CLIP_MIN */
    r->depth_max         = reg_f(0x0398);            /* SET_CLIP_MAX */
    r->stencil_enable    = s_reg[0x032C / 4];        /* SET_STENCIL_TEST_ENABLE */
    r->stencil_write_mask = s_reg[0x0360 / 4];       /* SET_STENCIL_MASK */
    r->stencil_func      = s_reg[0x0364 / 4];        /* SET_STENCIL_FUNC */
    r->stencil_ref       = s_reg[0x0368 / 4];        /* SET_STENCIL_FUNC_REF */
    r->stencil_func_mask = s_reg[0x036C / 4];        /* SET_STENCIL_FUNC_MASK */
    r->stencil_fail      = s_reg[0x0370 / 4];        /* SET_STENCIL_OP_FAIL */
    r->stencil_zfail     = s_reg[0x0374 / 4];        /* SET_STENCIL_OP_ZFAIL */
    r->stencil_zpass     = s_reg[0x0378 / 4];        /* SET_STENCIL_OP_ZPASS */
    r->zpass_enable      = zpass_mode() == ZPASS_ALWAYS ? 0 : s_gpu.zpass_enable;
    r->poly_offset_fill  = s_reg[0x0338 / 4];        /* SET_POLY_OFFSET_FILL_ENABLE */
    r->poly_offset_factor = reg_f(0x0384);           /* SET_POLYGON_OFFSET_SCALE_FACTOR */
    r->poly_offset_units = reg_f(0x0388);            /* SET_POLYGON_OFFSET_BIAS */
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb);
static uint32_t tex_face_stride(const Texture *t);

uint32_t nv2a_backend_texture_bytes(const Nv2aTexture *tex)
{
    Texture t;

    memset(&t, 0, sizeof t);
    t.width = tex->width;
    t.height = tex->height;
    t.pitch = tex->pitch;
    t.color = tex->color;
    t.levels = tex->levels;
    if (!tex_size_from_format(t.color))
        return t.pitch * t.height;
    return tex_face_stride(&t) * (tex->cube ? 6u : 1u);
}

int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out)
{
    Texture saved = s_gpu.texs[0];
    uint32_t x, y;
    int ok = 1;

    s_gpu.texs[0].offset = tex->offset;
    s_gpu.texs[0].width  = tex->width;
    s_gpu.texs[0].height = tex->height;
    s_gpu.texs[0].pitch  = tex->pitch;
    s_gpu.texs[0].color  = tex->color;
    s_gpu.texs[0].addr_u = 3;
    s_gpu.texs[0].addr_v = 3;
    s_gpu.texs[0].palette = tex->palette;
    s_gpu.texs[0].border = tex->border;
    s_gpu.texs[0].border_color = 0;     /* addresses clamp: no border taps */
    s_gpu.texs[0].levels = 1;
    s_gpu.texs[0].filter = 0;
    s_gpu.texs[0].cube   = 0;
    s_gpu.texs[0].valid  = 1;
    for (y = 0; y < tex->height && ok; y++)
        for (x = 0; x < tex->width; x++)
            if (!sample_texture(x, y, &argb_out[(size_t)y * tex->width + x])) {
                ok = 0;
                break;
            }
    s_gpu.texs[0] = saved;
    return ok;
}

/* Swizzled colour surfaces (SURFACE_FORMAT type 2, log2 width/height in bits
 * 16-19/24-27): render-to-texture. The pixel at (x, y) lives at the same
 * Morton offset a swizzled texture of that size uses, so the texture sampler
 * reads back what was drawn. Written row-major instead, every render target a
 * title samples later comes back scrambled -- SSX 3 draws six 128x128 rider
 * shadow maps per frame and projects them onto the snow. s_swz_* are set with
 * the surface, before the batch's pixels. RECOMP_SWIZZLED_RT=0 writes
 * row-major. */
static int s_swz_on;
static uint32_t s_swz_mx, s_swz_my;

static void surface_swizzle_update(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_SWIZZLED_RT");
        enabled = !(e && e[0] == '0');
    }
    s_swz_on = enabled && ((s_gpu.format >> 8) & 0xF) == 2;
    if (s_swz_on)
        xbox_swizzle_masks(1u << ((s_gpu.format >> 16) & 0xF),
                           1u << ((s_gpu.format >> 24) & 0xF),
                           &s_swz_mx, &s_swz_my);
}

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;

    /* A back end clears colour and/or depth/stencil itself (flags are the
     * CLEAR_SURFACE bits: Z 0x1, stencil 0x2, colour 0xF0). */
    if (s_backend && s_backend->clear) {
        Nv2aSurface surf;
        Nv2aRenderState rs;

        if (!s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
            return;
        current_surface(&surf);
        current_render_state(&rs);
        /* A depth-only surface (colour offset 0) still clears its depth:
         * Fable clears its shadow map's Z this way every frame, and the
         * clear never reached the back end while it was skipped whole. */
        surf.color_valid = (uint32_t)surface_color_valid(0);
        surf.color_masked = 0;             /* a clear ignores SET_COLOR_MASK */
        if (!surf.color_valid)
            param &= ~(uint32_t)NV097_CLEAR_COLOR_MASK;
        if (!(param & 0x3u) && !surf.color_valid)
            return;
        if (!surf.color_valid && !rs.zeta_va)
            return;
        /* A swizzled surface clears SET_CLEAR_RECT, as the CPU path below
         * does (SSX 3's 128x128 shadow maps, cleared whole and drawn
         * through a 124x124 clip): the back end clears the clip rectangle
         * it is handed, so it is handed the clear rectangle. */
        if (((s_gpu.format >> 8) & 0xF) == 2 && (s_gpu.clear_rect_set & 3) == 3) {
            uint32_t sw = 1u << ((s_gpu.format >> 16) & 0xF);
            uint32_t sh = 1u << ((s_gpu.format >> 24) & 0xF);
            uint32_t x0 = s_gpu.clear_rect_h & 0xFFFF, x1 = (s_gpu.clear_rect_h >> 16) + 1;
            uint32_t y0 = s_gpu.clear_rect_v & 0xFFFF, y1 = (s_gpu.clear_rect_v >> 16) + 1;
            if (x1 > sw) x1 = sw;
            if (y1 > sh) y1 = sh;
            if (x0 >= x1 || y0 >= y1)
                return;
            surf.clip_x = x0;
            surf.clip_y = y0;
            surf.width = x1 - x0;
            surf.height = y1 - y0;
        }
        s_backend->clear(&surf, &rs, param, s_gpu.clear_color,
                         s_gpu.zstencil_clear);
        s_gpu.clears++;
        return;
    }
    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;                            /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return;
    {
        uint32_t base = dma_resolve(s_gpu.color_offset);
        if (surface_write_refused(base,
                                  (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch,
                                  "clear"))
            return;
        s_gpu.color_base = base;
    }

    surface_swizzle_update();
    if (s_swz_on) {
        /* The clear covers CLEAR_RECT, not the surface clip: SSX 3 clears
         * its 128x128 shadow maps whole but draws them through a 124x124
         * clip, and samples them clamped -- the ring the clip leaves must be
         * the clear colour, or everything outside a shadow is dark. */
        uint8_t *base = mem + s_gpu.color_base;
        uint32_t sw = 1u << ((s_gpu.format >> 16) & 0xF);
        uint32_t sh = 1u << ((s_gpu.format >> 24) & 0xF);
        uint32_t x0 = s_gpu.clip_x, x1 = s_gpu.clip_x + s_gpu.clip_w;
        uint32_t y0 = s_gpu.clip_y, y1 = s_gpu.clip_y + s_gpu.clip_h;
        uint16_t v16 = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                | ((s_gpu.clear_color >> 5) & 0x07E0)
                                | ((s_gpu.clear_color >> 3) & 0x001F));
        if ((s_gpu.clear_rect_set & 3) == 3) {
            x0 = s_gpu.clear_rect_h & 0xFFFF; x1 = (s_gpu.clear_rect_h >> 16) + 1;
            y0 = s_gpu.clear_rect_v & 0xFFFF; y1 = (s_gpu.clear_rect_v >> 16) + 1;
        }
        if (x1 > sw) x1 = sw;
        if (y1 > sh) y1 = sh;
        for (y = y0; y < y1; y++)
            for (x = x0; x < x1; x++) {
                size_t i = swizzle_deposit(x, s_swz_mx) | swizzle_deposit(y, s_swz_my);
                if (bpp == 4)
                    ((uint32_t *)base)[i] = s_gpu.clear_color;
                else if (bpp == 2)
                    ((uint16_t *)base)[i] = v16;
            }
    } else
    for (y = 0; y < s_gpu.clip_h; y++) {
        uint8_t *row = mem + s_gpu.color_base
                     + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {
            /* The clear value is always given as A8R8G8B8; a 16-bit surface
             * takes the same colour reduced to 5:6:5. */
            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = v;
        }
    }
    s_gpu.clears++;
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    static int raster_test = -1;
    if (raster_test < 0)
        raster_test = getenv("RECOMP_RASTER_TEST") != NULL;
    if (raster_test) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL);  /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    /* The window has to read where the pixels actually are, which is the
     * resolved address rather than the DMA-object offset. */
    /* Only until the title flips. Following the draw surface on every scan
     * shows the buffer being written right now, half a frame at a time; past
     * the first flip the window is repointed at the finished one instead. */
    if (s_gpu.flips == 0) {
        xbox_FramebufferWindowSet(dma_resolve(s_gpu.color_offset), s_gpu.pitch);
        xbox_FramebufferWindowSetAA(s_gpu.aa_sy > 1.5f ? 2 : 1);
    }

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

/* One texel, in the title's own format.
 *
 * The codes are the NV097 colour field, which is the Xbox D3DFMT_ enum --
 * src/d3d/d3d8_xbox.h is the table, and it is the table to check against
 * rather than recollection: 0x1E is LIN_X8R8G8B8 and not, as this first read
 * it, a byte-reversed BGRA. Getting that one wrong turned an opaque black
 * render target into a screen of pure blue, which is the kind of wrong that
 * looks like content.
 *
 * Only the linear (LIN_) formats are read. A swizzled texture stores its
 * texels in Morton order rather than in rows, so reading one as if it had a
 * pitch does not give a slightly wrong colour, it gives a different image --
 * and inventing that image is exactly what this is not for. An unsupported
 * format samples nothing and the caller keeps the vertex colour, which is
 * visibly wrong rather than quietly wrong.
 *
 * Nearest texel, whatever SET_TEXTURE_FILTER asked for, unless the title opts
 * in to RECOMP_TEX_BILINEAR or RECOMP_TEX_ANISO (tex_filter_config below).
 */
/* Off the edge of the texture, the way the title asked for.
 *
 * Refusing to sample instead is not neutral: it hands the caller back the
 * vertex colour, so a pass whose coordinates reach the last texel by half a
 * texel gets a bright line down the edge of the screen. The dashboard's
 * resolve does exactly that -- its last column and last row, 1119 pixels of
 * white on a black frame, from a rounding step at the boundary.
 */
static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    /* Coordinates are signed: a bilinear tap one texel left of or above
     * texel 0 is -1, which must wrap to the far edge or clamp to 0 -- as an
     * unsigned value it clamped to the far edge instead. */
    int32_t sc = (int32_t)c, n = (int32_t)size;
    if (!size)
        return 0;
    if (mode == 1)                         /* wrap */
        return (uint32_t)(((sc % n) + n) % n);
    if (mode == 2) {                       /* mirror */
        int32_t m = ((sc % (2 * n)) + 2 * n) % (2 * n);
        return (uint32_t)(m < n ? m : 2 * n - 1 - m);
    }
    return sc < 0 ? 0u : (sc >= n ? size - 1 : c);   /* clamp, and the rest */
}

static uint32_t expand(uint32_t v, uint32_t bits)
{
    return d3d8_expand_channel(v, bits);
}

/* The linear format that decodes the same texels as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06) and
 * LIN_A8R8G8B8 (0x12) are the same four bytes in the same order. So the whole
 * difference is the address calculation, and one of those lets every format
 * below serve both. Pairs read off the table in d3d8_xbox.h rather than
 * recalled -- the comment above this one is about getting exactly that wrong. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;                /* L8        -> LIN_L8        */
    case 0x01: return 0x1B;                /* AL8       -> LIN_AL8       */
    case 0x02: return 0x10;                /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;                /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;                /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;                /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;                /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;                /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;                /* A8        -> LIN_A8        */
    default:   return fmt;                 /* already linear, or unhandled */
    }
}

/* The 16 decoded texels of DXT block (bx, by) of the image at `base`.
 *
 * Decoded blocks, direct-mapped by address. Every texel of a DXT texture
 * decodes its whole 4x4 block, and bilinear reads four neighbours that nearly
 * always share one -- this was a tenth of the executor's time. Keyed on the
 * block's bytes as well as its address, so a texture rewritten in place (a
 * render target reused, a streamed mip) never returns stale texels. One table
 * per raster thread. */
/* Entries per raster thread (power of two). 16384 x 92 bytes is about 1.5 MB:
 * a frame that magnifies a few 256x256 DXT textures across the screen touches
 * 4096 blocks each, and with 4096 entries they evicted one another. */
#define NV_DXT_CACHE 16384u
static const uint32_t *dxt_block_px(const Texture *t, uint32_t base,
                                    uint32_t bx, uint32_t by)
{
    static NV_TLS struct { uintptr_t key; uint32_t fmt; uint64_t raw[2];
                    uint32_t px[16]; } cache[NV_DXT_CACHE];
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t fmt = t->color, bb = d3d8_format_dxt_block_bytes(fmt);
    uint32_t bw = (t->width + 3) >> 2;
    const uint8_t *blk = mem + base + ((size_t)by * bw + bx) * bb;
    uintptr_t key = (uintptr_t)blk;
    size_t slot = ((key / bb) ^ (key >> 16)) & (NV_DXT_CACHE - 1);
    uint64_t raw[2] = {0, 0};
    memcpy(raw, blk, bb);
    if (cache[slot].key != key || cache[slot].fmt != fmt
        || cache[slot].raw[0] != raw[0] || cache[slot].raw[1] != raw[1]) {
        d3d8_dxt_decode_block(blk, fmt, cache[slot].px);
        cache[slot].key = key;
        cache[slot].fmt = fmt;
        cache[slot].raw[0] = raw[0];
        cache[slot].raw[1] = raw[1];
    }
    return cache[slot].px;
}

/* Sample texel (u, v) of stage texture `t`; `face` offsets a cube map. */
static int sample_tex(const Texture *t, uint32_t face_offset,
                      uint32_t u, uint32_t v, uint32_t *argb)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t fmt, base = t->offset + face_offset;

    if (!t->valid)
        return 0;
    /* BORDER (4) with FORMAT's border source set to "colour": texels outside
     * the image read SET_TEXTURE_BORDER_COLOR. Projected shadow maps rely on
     * it -- clamped instead, everything outside the projection takes the
     * edge texel and the shadow's frustum is drawn as a dark wedge.
     * (Coordinates are signed in unsigned: a negative one is >= the size.) */
    if (t->border_color
        && ((t->addr_u == 4 && u >= t->width) || (t->addr_v == 4 && v >= t->height))) {
        *argb = t->border;
        return 1;
    }
    u = wrap_coord(u, t->width,  t->addr_u);
    v = wrap_coord(v, t->height, t->addr_v);

    fmt = t->color;
    if (d3d8_format_dxt_block_bytes(fmt)) {
        *argb = dxt_block_px(t, base, u >> 2, v >> 2)[(v & 3) * 4 + (u & 3)];
        return 1;
    }
    if (d3d8_format_is_swizzled(fmt)) {
        /* Morton order: a texel's index is interleaved from x and y instead of
         * v*pitch + u, so index from the base of the image. The switch below
         * casts to each format's own width, which makes that index a texel
         * index for every one of them. */
        fmt = linear_twin(fmt);
        p = mem + base;
        u = swizzle_offset(u, v, t->width, t->height);
    } else {
        p = mem + base + (size_t)v * t->pitch;
    }

    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 8-bit palette index, swizzled. Burnout 3 draws its logo and frontend
     * header art this way; without a case every such quad came out as the
     * unsupported-format fill, a white box where the logo should be. */
    case 0x0B:                                      /* SZ_I8_A8R8G8B8 */
        if (!t->palette)
            return 0;
        *argb = ((const uint32_t *)(mem + t->palette))[p[u]];
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F: {                                    /* LIN_A8B8G8R8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    }
    case 0x40: {                                    /* LIN_B8G8R8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)                 /* A, from the bottom */
              | (((t >>  8) & 0xFFu) << 16)         /* R */
              | (((t >> 16) & 0xFFu) <<  8)         /* G */
              |  ((t >> 24) & 0xFFu);               /* B, from the top */
        return 1;
    }
    case 0x41: {                                    /* LIN_R8G8B8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;
    }

    /* 16-bit. */
    case 0x10: {                                    /* LIN_A1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1C: {                                    /* LIN_X1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x11: {                                    /* LIN_R5G6B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x3F, 6) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1D: {                                    /* LIN_A4R4G4B4 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4) << 24)
              | (expand((t >>  8) & 0x0F, 4) << 16)
              | (expand((t >>  4) & 0x0F, 4) <<  8)
              |  expand( t        & 0x0F, 4);
        return 1;
    }

    /* 8-bit. */
    case 0x13: {                                    /* LIN_L8 */
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    }
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;

    /* One byte that is both the alpha and the luminance (NV2A AY8). Fable:
     * The Lost Chapters keeps its glyph cache in a 512x512 swizzled AL8
     * texture; without a case every glyph quad was the unsupported-format
     * fill, so its text came out as solid blocks the shape of the words. */
    case 0x1B: {                                    /* LIN_AL8 */
        uint32_t t = p[u];
        *argb = (t << 24) | (t << 16) | (t << 8) | t;
        return 1;
    }

    /* 4:2:2 packed YUV, two texels per four bytes.
     *
     * This is how a title hands over a decoded video frame, and without it
     * the frame falls through to `default` -- which returns 0, so the caller
     * paints the quad's vertex colour and the movie is a flat rectangle.
     *
     * The chroma pair is shared between an even texel and the one after it,
     * so the group is found by masking the bottom bit of the index. BT.601,
     * the same coefficients the D3D8 upload path converts with, so the two
     * paths agree rather than each having its own idea of the colour. */
    case 0x24:                                      /* LC_CR8YB8CB8YA8, YUY2 */
    case 0x25: {                                    /* LC_YB8CR8YA8CB8, UYVY */
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        int r = (298 * c + 409 * cv + 128) >> 8;
        int gg = (298 * c - 100 * cu - 208 * cv + 128) >> 8;
        int b = (298 * c + 516 * cu + 128) >> 8;
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (gg < 0) gg = 0;
        if (gg > 255) gg = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        *argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8)
              | (uint32_t)b;
        return 1;
    }

    default:
        return 0;
    }
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    return sample_tex(&s_gpu.texs[0], 0, u, v, argb);
}

/* Write a bound texture out as a BMP, through the sampler rather than around it.
 *
 * "Which texture is this" is not answerable from an address and a format, and
 * it is the question behind most of the ones that matter -- is that a font
 * page or an icon atlas, did the swizzle decode, is the alpha inverted. Going
 * through sample_texture means the file shows exactly what the rasteriser
 * sees, so a decode bug appears here rather than only as a wrong-looking
 * triangle.
 *
 * ponytail: RGB only, alpha dropped. A glyph page is alpha and would come out
 * black, so alpha is composited onto mid-grey to stay legible; that is a
 * viewing choice, not a decode. One file per distinct texture, first use only.
 */
static void dump_texture_bmp(uint32_t seq)
{
    const char *prefix = getenv("RECOMP_TEX_DUMP");
    uint32_t w = s_gpu.texs[0].width, h = s_gpu.texs[0].height, x, y;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !w || !h || w > 4096 || h > 4096)
        return;
    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%02u_%08X_fmt%02X.bmp",
             prefix, seq, s_gpu.texs[0].offset, s_gpu.texs[0].color);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t argb = 0, a;
            uint8_t px[3];
            if (!sample_texture(x, h - 1 - y, &argb))
                argb = 0;
            a = (argb >> 24) & 0xFFu;
            /* over mid-grey, so an alpha-only page is visible either way */
            px[0] = (uint8_t)(((argb & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[1] = (uint8_t)((((argb >> 8) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[2] = (uint8_t)((((argb >> 16) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXDUMP] %s (%ux%u fmt 0x%02X)\n",
            path, w, h, s_gpu.texs[0].color);
    fflush(stderr);
}

/* The surface, resolved once per batch.
 *
 * dma_resolve consults the contiguous arena's high-water mark and
 * surface_hits_image walks the image range; both were being done per pixel
 * -- dma_resolve twice -- which cost more than the rasterisation they
 * guarded. Neither answer can change inside a batch, because the colour
 * offset arrives as a method and a method cannot arrive mid-triangle.
 *
 * This is not a micro-optimisation for its own sake: the loader's video
 * paces on frames actually presented, so the rasteriser's throughput is the
 * playback rate. */
static uint8_t *s_surface;          /* host address of surface row 0 */

/* Element index of pixel (x, y) in the current colour surface, relative to
 * the `row` it sets: the row itself, or the surface base for a swizzled one. */
static inline size_t surface_index(int x, int y, uint8_t **row)
{
    if (s_swz_on) {
        *row = s_surface;
        return swizzle_deposit((uint32_t)x, s_swz_mx)
             | swizzle_deposit((uint32_t)y, s_swz_my);
    }
    *row = s_surface + (size_t)y * s_gpu.pitch;
    return (size_t)x;
}

static int surface_begin_batch(const uint8_t *mem)
{
    uint32_t base = dma_resolve(s_gpu.color_offset);

    surface_swizzle_update();

    if (surface_hits_image(base, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch))
        return 0;
    s_surface = (uint8_t *)mem + base;
    return 1;
}

static uint32_t pack_color(const float c[4]);

/* One GL blend factor, per channel (r g b a). */
static void blend_factor(uint32_t f, const float s[4], const float d[4],
                         float out[4])
{
    int k;
    for (k = 0; k < 4; k++) {
        float c = (float)((s_gpu.blend_color >> (k == 3 ? 24 : 16 - 8 * k))
                          & 0xFF) / 255.0f;
        switch (f) {
        case 0x0000: out[k] = 0.0f;            break;   /* ZERO                */
        case 0x0001: out[k] = 1.0f;            break;   /* ONE                 */
        case 0x0300: out[k] = s[k];            break;   /* SRC_COLOR           */
        case 0x0301: out[k] = 1.0f - s[k];     break;
        case 0x0302: out[k] = s[3];            break;   /* SRC_ALPHA           */
        case 0x0303: out[k] = 1.0f - s[3];     break;
        case 0x0304: out[k] = d[3];            break;   /* DST_ALPHA           */
        case 0x0305: out[k] = 1.0f - d[3];     break;
        case 0x0306: out[k] = d[k];            break;   /* DST_COLOR           */
        case 0x0307: out[k] = 1.0f - d[k];     break;
        case 0x0308: out[k] = k == 3 ? 1.0f : fminf(s[3], 1.0f - d[3]); break;
        case 0x8001: out[k] = c;               break;   /* CONSTANT_COLOR      */
        case 0x8002: out[k] = 1.0f - c;        break;
        case 0x8003: out[k] = (float)(s_gpu.blend_color >> 24) / 255.0f; break;
        case 0x8004: out[k] = 1.0f - (float)(s_gpu.blend_color >> 24) / 255.0f; break;
        default:     out[k] = f ? 1.0f : 0.0f; break;
        }
    }
}

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;
    size_t xi;

    (void)mem;
    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    /* A swizzled target is 1<<log on a side, whatever the clip says: past
     * it the swizzle index wraps onto other texels (xemu bounds it too). */
    if (s_swz_on && ((uint32_t)x >> ((s_gpu.format >> 16) & 0xF)
                     || (uint32_t)y >> ((s_gpu.format >> 24) & 0xF)))
        return;
    s_gpu.pixels++;
    if ((argb & 0x00FFFFFFu) > (s_gpu.pixel_max & 0x00FFFFFFu))
        s_gpu.pixel_max = argb;
    xi = surface_index(x, y, &row);

    /* Blending, with the GL factor set and equations the NV2A takes.
     *
     * Only SRC_ALPHA / ONE_MINUS_SRC_ALPHA used to be honoured and every other
     * pair was written opaque. Menus never noticed; a race does: Burnout 3
     * finishes its 3D frame with screen-space passes that modulate or add
     * over the scene, and written opaque they paint the whole view over. */
    if (s_gpu.blend_enable
        && !(s_gpu.blend_sfactor == 1 && s_gpu.blend_dfactor == 0)) {
        uint32_t dst = 0xFF000000u;
        float sc[4], dc[4], sf[4], df[4], out[4];
        int k;
        if (bpp == 4) {
            dst = ((const uint32_t *)row)[xi];
        } else if (bpp == 2) {
            uint32_t t = ((const uint16_t *)row)[xi];
            dst = 0xFF000000u | ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
                | ((t & 0x001Fu) << 3);
        }
        for (k = 0; k < 4; k++) {           /* r g b a, 0..1 */
            int sh = k == 3 ? 24 : 16 - 8 * k;
            sc[k] = (float)((argb >> sh) & 0xFF) / 255.0f;
            dc[k] = (float)((dst  >> sh) & 0xFF) / 255.0f;
        }
        blend_factor(s_gpu.blend_sfactor, sc, dc, sf);
        blend_factor(s_gpu.blend_dfactor, sc, dc, df);
        for (k = 0; k < 4; k++) {
            float a = sc[k] * sf[k], b = dc[k] * df[k];
            switch (s_gpu.blend_equation) {
            case 0x800A: out[k] = a - b; break;               /* SUBTRACT     */
            case 0x800B: out[k] = b - a; break;               /* REV_SUBTRACT */
            case 0x8007: out[k] = fminf(sc[k], dc[k]); break; /* MIN          */
            case 0x8008: out[k] = fmaxf(sc[k], dc[k]); break; /* MAX          */
            default:     out[k] = a + b; break;               /* ADD          */
            }
        }
        argb = pack_color(out);
    }

    if (s_gpu.color_mask != 0x01010101u) {
        uint32_t keep = 0, old;
        if (!(s_gpu.color_mask & 0x01000000u)) keep |= 0xFF000000u;
        if (!(s_gpu.color_mask & 0x00010000u)) keep |= 0x00FF0000u;
        if (!(s_gpu.color_mask & 0x00000100u)) keep |= 0x0000FF00u;
        if (!(s_gpu.color_mask & 0x00000001u)) keep |= 0x000000FFu;
        if (keep == 0xFFFFFFFFu)
            return;
        if (bpp == 4) {
            old = ((const uint32_t *)row)[xi];
        } else {
            uint32_t t = ((const uint16_t *)row)[xi];
            old = ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5) | ((t & 0x001Fu) << 3);
        }
        argb = (argb & ~keep) | (old & keep);
    }
    if (bpp == 4) {
        ((uint32_t *)row)[xi] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[xi] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
}

/* Texture filtering for the CPU rasteriser. Both switches are per-title
 * opt-ins: off, the default, keeps the nearest texel for every title.
 *
 *   RECOMP_TEX_BILINEAR=1    four taps, blended
 *   RECOMP_TEX_ANISO=8|16    up to that many bilinear taps along the longer
 *                            texel gradient of the triangle; implies bilinear.
 *                            Any other value leaves it off.
 *
 * ponytail: no mip levels, so a minified texture is not blurred by level, only
 * averaged along its stretched axis. Add the level choice when a title needs it. */
static void tex_filter_config(int *bilinear, int *aniso)
{
    static int b = -1, n = -1;

    if (b < 0) {
        const char *e = getenv("RECOMP_TEX_BILINEAR");
        b = e && strcmp(e, "0") != 0;
    }
    if (n < 0) {
        const char *e = getenv("RECOMP_TEX_ANISO");
        int v = e ? atoi(e) : 0;
        n = v >= 16 ? 16 : v >= 8 ? 8 : 0;
    }
    *aniso = n;
    *bilinear = b || n;
}

/* Adds the bilinear blend around (su, sv), texel units, to acc[] (one sum per
 * ARGB channel, 65536 per tap). 0: a tap sampled nothing, so the caller keeps
 * the vertex colour. A tap off the edge goes through sample_tex, which wraps,
 * mirrors, clamps or reads the border colour as the title asked. */
static int bilinear_accum(float su, float sv, uint32_t acc[4])
{
    const Texture *t = &s_gpu.texs[0];
    float fx = su - 0.5f, fy = sv - 0.5f;
    int32_t x0 = (int32_t)floorf(fx), y0 = (int32_t)floorf(fy);
    uint32_t wx = (uint32_t)((fx - (float)x0) * 256.0f);
    uint32_t wy = (uint32_t)((fy - (float)y0) * 256.0f);
    uint32_t c[4], w[4], i, ch;

    if (wx > 255) wx = 255;
    if (wy > 255) wy = 255;
    if (!sample_tex(t, 0, (uint32_t)x0,       (uint32_t)y0,       &c[0])
     || !sample_tex(t, 0, (uint32_t)(x0 + 1), (uint32_t)y0,       &c[1])
     || !sample_tex(t, 0, (uint32_t)x0,       (uint32_t)(y0 + 1), &c[2])
     || !sample_tex(t, 0, (uint32_t)(x0 + 1), (uint32_t)(y0 + 1), &c[3]))
        return 0;
    w[0] = (256 - wx) * (256 - wy);
    w[1] = wx * (256 - wy);
    w[2] = (256 - wx) * wy;
    w[3] = wx * wy;
    for (ch = 0; ch < 4; ch++)
        for (i = 0; i < 4; i++)
            acc[ch] += ((c[i] >> (8 * ch)) & 0xFFu) * w[i];
    return 1;
}

/* The average of `taps` bilinear blends gathered in acc[]. */
static uint32_t filter_result(const uint32_t acc[4], uint32_t taps)
{
    uint32_t total = taps << 16, out = 0, ch;

    for (ch = 0; ch < 4; ch++)
        out |= ((acc[ch] + total / 2) / total) << (8 * ch);
    return out;
}

/* Taps for one pixel, from its texel gradients gx = (du, dv) per screen x and
 * gy per screen y. The longer one is the major axis and the shorter one sets
 * how stretched the footprint is. The taps spread over the major gradient,
 * centred on the pixel: start + i * step for i < the returned count. */
static uint32_t aniso_footprint(const float gx[2], const float gy[2],
                                uint32_t max_taps, float start[2], float step[2])
{
    float lx = sqrtf(gx[0] * gx[0] + gx[1] * gx[1]);
    float ly = sqrtf(gy[0] * gy[0] + gy[1] * gy[1]);
    const float *major = lx >= ly ? gx : gy;
    float longer = lx >= ly ? lx : ly, shorter = lx >= ly ? ly : lx;
    uint32_t n = 1;

    if (longer > 1e-6f) {
        float ratio = shorter > 1e-6f ? longer / shorter : (float)max_taps;
        n = ratio >= (float)max_taps ? max_taps : (uint32_t)ceilf(ratio);
        if (n < 1) n = 1;
    }
    step[0] = major[0] / (float)n;
    step[1] = major[1] / (float)n;
    start[0] = -0.5f * major[0] + 0.5f * step[0];
    start[1] = -0.5f * major[1] + 0.5f * step[1];
    return n;
}

/* The same for a triangle whose screen-to-texel mapping is affine: the
 * footprint is then the same everywhere in it, so it is worked out once.
 * `area` is the triangle's signed edge-function area. */
static uint32_t aniso_taps(const float a[2], const float b[2], const float c[2],
                           const float uv[3][2], float area, uint32_t max_taps,
                           float start[2], float step[2])
{
    float e1x = b[0] - a[0], e1y = b[1] - a[1];
    float e2x = c[0] - a[0], e2y = c[1] - a[1];
    float du1 = uv[1][0] - uv[0][0], du2 = uv[2][0] - uv[0][0];
    float dv1 = uv[1][1] - uv[0][1], dv2 = uv[2][1] - uv[0][1];
    float gx[2] = { (du1 * e2y - du2 * e1y) / area, (dv1 * e2y - dv2 * e1y) / area };
    float gy[2] = { (du2 * e1x - du1 * e2x) / area, (dv2 * e1x - dv1 * e2x) / area };

    return aniso_footprint(gx, gy, max_taps, start, step);
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2])
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;
    int textured = uv && s_gpu.texs[0].valid;
    int bilinear = 0, aniso = 0;
    uint32_t taps = 1;
    float tap_start[2] = {0.0f, 0.0f}, tap_step[2] = {0.0f, 0.0f};

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */

    if (textured) {
        tex_filter_config(&bilinear, &aniso);
        if (aniso)
            taps = aniso_taps(a, b, c, uv, area, (uint32_t)aniso, tap_start, tap_step);
    }

    /* Where this batch writes. The same check the per-pixel path made, made
     * once: a surface address landing on the title's own image is no safer
     * one pixel at a time than 4.9 MB at once. */
    if (!surface_begin_batch(mem))
        return;

    minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));

    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            if (textured) {
                /* Barycentric, straight from the edge functions already
                 * computed: w1 is the area opposite a, w2 opposite b, w0
                 * opposite c, and the three sum to the whole triangle.
                 *
                 * No perspective divide. These are screen-space vertices with
                 * no w to divide by -- which is exactly the case a full-screen
                 * pass is, and the only case that reaches here. */
                uint32_t texel;
                float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                if (bilinear) {
                    /* Filtered: a negative coordinate is a tap off the edge
                     * and must reach sample_tex's addressing, not be clamped. */
                    uint32_t acc[4] = {0, 0, 0, 0}, i;
                    for (i = 0; i < taps; i++)
                        if (!bilinear_accum(su + tap_start[0] + (float)i * tap_step[0],
                                            sv + tap_start[1] + (float)i * tap_step[1], acc))
                            break;
                    if (i == taps) {
                        put_pixel(mem, bpp, x, y, filter_result(acc, taps));
                        continue;
                    }
                    put_pixel(mem, bpp, x, y, argb);
                    continue;
                }
                if (su < 0.0f) su = 0.0f;
                if (sv < 0.0f) sv = 0.0f;
                if (sample_texture((uint32_t)su, (uint32_t)sv, &texel)) {
                    put_pixel(mem, bpp, x, y, texel);
                    continue;
                }
            }
            put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    note_drawn();
}

/* Attribute 3 is diffuse colour in every NV2A layout that sets one. Absent it,
 * white -- a visible wrong colour beats an invisible correct one during
 * bring-up. */
/* Which attribute carries the colour.
 *
 * Slot 3 is diffuse by convention and titles that follow it are read straight
 * from there. Half-Life 2 does not: its vertex is position, colour, texcoord
 * at stride 24, and the colour arrives in slot 5. So fall back to the format
 * rather than the slot number -- D3DCOLOR is the one attribute type that is
 * only ever a colour, which makes it a stronger signal than the convention. */
static const VertexAttr *color_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[3].offset && s_gpu.attr[3].stride)
        return &s_gpu.attr[3];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 0 && s_gpu.attr[a].size == 4
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[3];
}

/* Attribute 9 is texture coordinate 0 in the NV2A vertex layout, the same way
 * 0 is position and 3 is diffuse -- for a title that follows the convention.
 *
 * Half-Life 2 does not, in either place. Its menu and HUD vertex is position,
 * colour, texcoord at stride 24, with the colour in slot 5 and the texcoords
 * in slot 7, so reading slot 9 found nothing and every batch drew untextured.
 * That is invisible rather than wrong-looking: the menu paints a full-screen
 * quad and then draws its text over it, and with no sampling both come out
 * white, so the screen is blank white and nothing suggests the text was ever
 * drawn.
 *
 * Falling back to the format works because the three attributes of such a
 * vertex are distinguishable: position is float3, colour is D3DCOLOR, and a
 * float2 is a texture coordinate and nothing else.
 *
 * ponytail: takes the first float2 it finds, so a title with two texcoord sets
 * gets stage 0's -- which is what this single-texture rasteriser samples
 * anyway. Multi-texture wants the D3D11 translator, not another heuristic. */
static const VertexAttr *texcoord_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[9].offset && s_gpu.attr[9].stride)
        return &s_gpu.attr[9];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 2 && s_gpu.attr[a].size == 2
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[9];
}

/* Texel coordinates, whichever convention the title used.
 *
 * The two are not interchangeable and the format decides which is in force: a
 * swizzled texture is addressed in [0,1], a linear one in texels. Both are
 * scaled to texels here so that everything downstream -- the barycentric
 * interpolation and the sampler -- works in one unit.
 *
 * This mattered the moment swizzled formats became samplable. Normalised
 * coordinates truncated to a texel index land on texel 0 for any coordinate
 * below 1.0, so a whole quad sampled a single texel and came out flat: the
 * background painted one near-black colour, which looks like a texture that
 * decoded wrong rather than one that was never indexed. */
static int fetch_texcoord(uint32_t index, float out[2])
{
    float t[4];

    if (!fetch_attr(texcoord_attr(), index, t))
        return 0;
    out[0] = t[0];
    out[1] = t[1];
    if (tex_size_from_format(s_gpu.texs[0].color)) {
        out[0] *= (float)s_gpu.texs[0].width;
        out[1] *= (float)s_gpu.texs[0].height;
    }
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];

    if (!fetch_attr(color_attr(), index, c))
        return 0xFFFFFFFFu;
    return ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
}

/* ---- Fixed-function transform and lighting --------------------------------
 *
 * With TRANSFORM_EXECUTION_MODE on "fixed" (not "program") and a composite
 * matrix uploaded, attribute 0 is an object-space position and the hardware
 * computes
 *     clip[i] = sum_j M[4i+j] * pos[j]
 * with the 16 floats in the order the methods deliver them (translation in
 * M[3], M[7], M[11]; X-Men Legends' menu matrix ends in the row 0 0 0 1), then
 *     screen = clip.xyz / clip.w + viewport offset (c[59]).
 * The composite matrix carries the viewport scale for the *logical* surface;
 * with anti-aliasing the result is scaled up to the real surface.
 *
 * The result is handed on in the same shape a vertex program produces
 * (Nv2aVshOutput, position already in screen space, w the clip w), so a
 * fixed-function batch shares the program path's near-plane clipping,
 * texturing, combiners and depth buffer.
 *
 * ponytail: no texture-coordinate generation or texture matrices (texture
 * coordinates are taken from the attribute streams as they are), no fog
 * coordinate, no specular, no point sprites. */

static int batch_is_screen_space(void);

/* RECOMP_FFP_INLINE: how an inline batch (INLINE_ARRAY or SET_VERTEX4F, i.e.
 * D3D Begin/End and DrawPrimitiveUP) is drawn under the fixed-function unit.
 *   1 (default) transformed by the composite matrix like any other batch, as
 *     the hardware does;
 *   0 the old way: left to the screen-space guess, which skips it when its
 *     positions look like model units;
 *   2 transformed only when the screen-space guess would skip it.
 * KOTOR draws its whole GUI as unit quads under a 640/-480 composite matrix:
 * under 0 every one of them was skipped. 0 and 2 are an A/B switch for one
 * release, in case a title relied on the old reading. */
static int ffp_inline_mode(void)
{
    static int mode = -1;

    if (mode < 0) {
        const char *e = getenv("RECOMP_FFP_INLINE");
        mode = e && *e ? atoi(e) : 1;
        if (mode < 0 || mode > 2)
            mode = 1;
    }
    return mode;
}

/* Is this batch transformed by the fixed-function unit? */
static int batch_is_ffp(void)
{
    if (!s_gpu.composite_set || (s_gpu.xf_mode & 3) == 2)
        return 0;
    if (!s_gpu.inline_active)
        return 1;
    switch (ffp_inline_mode()) {
    case 0:  return 0;
    case 2:  return !batch_is_screen_space();
    default: return 1;
    }
}

/* Fixed-function vertex lighting (SET_LIGHTING_ENABLE).
 *
 * With lighting on, the diffuse colour the combiners see is computed per
 * vertex from the normal and up to eight lights; the vertex's own colour
 * stream is ignored unless SET_COLOR_MATERIAL routes it in. Using the colour
 * stream anyway is what drew every lit mesh -- characters, streets -- black.
 *
 * XDK D3D pre-multiplies material into the light colours and folds
 * emission + material ambient * global ambient into SCENE_AMBIENT_COLOR, so
 * the hardware sum is simply
 *   colour = scene_ambient + sum_i atten_i * (ambient_i + diffuse_i * max(0, N.L_i))
 * with alpha from SET_MATERIAL_ALPHA. Everything is in eye space.
 *
 * ponytail: normals go through the model-view matrix itself rather than the
 * inverse transpose (exact for rotations and uniform scale), no specular, no
 * spot cones, no back-face colours, no skinning. Add them when a mesh looks
 * wrong rather than dark. */
#define NV_LIGHTS 8

/* RECOMP_STRIP_ALT_WINDING, off by default: emit every odd triangle of a
 * triangle strip with its first two vertices swapped, so all of a strip's
 * triangles wind the same way, as a strip does on hardware. Left alone the
 * executor emits (i, i+1, i+2) for every i, so a back end that culls or lights
 * two-sided from the winding drops or back-lights every second triangle.
 * On whenever the back end culls (NV2A_BACKEND_CAP_CULL). */
static int strip_alt_winding(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("RECOMP_STRIP_ALT_WINDING");
        on = e && strcmp(e, "0") != 0;
    }
    return on || (s_backend && s_backend->draw
                  && (s_backend->caps & NV2A_BACKEND_CAP_CULL));
}

/* RECOMP_FFP_FLIP_NORMALS: a compatibility option, off by default. See
 * lit_color(). */
static int ffp_flip_normals(void)
{
    static int on = -1;

    if (on < 0)
        on = getenv("RECOMP_FFP_FLIP_NORMALS") != NULL;
    return on;
}

static int lit_color(uint32_t index, float out[4])
{
    const float *mv = (const float *)&s_reg[0x0480 / 4];
    float pos[4], nrm[4], vc[4], e[3], n[3], len;
    uint32_t mask = s_reg[0x03BC / 4], colmat = s_reg[0x0298 / 4];
    int i, have_vc;

    if (!s_reg[0x0314 / 4] || !s_gpu.attr[2].size)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], index, pos)
     || !fetch_attr(&s_gpu.attr[2], index, nrm))
        return 0;
    have_vc = fetch_attr(color_attr(), index, vc);

    for (i = 0; i < 3; i++) {
        e[i] = mv[4 * i] * pos[0] + mv[4 * i + 1] * pos[1]
             + mv[4 * i + 2] * pos[2] + mv[4 * i + 3];
        n[i] = mv[4 * i] * nrm[0] + mv[4 * i + 1] * nrm[1]
             + mv[4 * i + 2] * nrm[2];
    }
    len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    /* Optional (RECOMP_FFP_FLIP_NORMALS): light the side the viewer sees.
     * Some meshes carry normals that face away from the camera (X-Men
     * Legends' streets and sidewalks: authentic file data, drawn with culling
     * off), and lighting them as stored leaves only the light's ambient term
     * -- black ground. Flipping a normal that points away from the eye is
     * two-sided lighting with the back material equal to the front one.
     * This is NOT hardware behaviour: it applies even when the title left
     * two-sided lighting off (NV097_SET_TWO_SIDE_LIGHT_EN, method 0x17C4,
     * which X-Men Legends writes as 0), and how the hardware makes such a mesh
     * look right is not established. So it is a compatibility option a game
     * project turns on for meshes it has seen need it, and off otherwise, so
     * it cannot change how any other title is lit.
     * ponytail: per vertex, not per face; exact for flat ground, a vertex on
     * a silhouette may pick the other side. */
    if (ffp_flip_normals() && n[0] * e[0] + n[1] * e[1] + n[2] * e[2] > 0.0f) {
        n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
    }

    out[0] = reg_f(0x0A10); out[1] = reg_f(0x0A14); out[2] = reg_f(0x0A18);
    for (i = 0; i < NV_LIGHTS; i++) {
        uint32_t kind = (mask >> (2 * i)) & 3, b = 0x1000 + (uint32_t)i * 0x80;
        float l[3], att = 1.0f, ndl, amb[3], dif[3];
        int k;

        if (!kind)
            continue;
        if (kind == 1) {                                    /* infinite */
            l[0] = reg_f(b + 0x34); l[1] = reg_f(b + 0x38); l[2] = reg_f(b + 0x3C);
        } else {                                            /* local / spot */
            float d;
            l[0] = reg_f(b + 0x5C) - e[0];
            l[1] = reg_f(b + 0x60) - e[1];
            l[2] = reg_f(b + 0x64) - e[2];
            d = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            if (d > reg_f(b + 0x24) && reg_f(b + 0x24) > 0.0f)
                continue;                                   /* out of range */
            if (d > 1e-8f) { l[0] /= d; l[1] /= d; l[2] /= d; }
            att = reg_f(b + 0x68) + reg_f(b + 0x6C) * d + reg_f(b + 0x70) * d * d;
            att = att > 1e-8f ? 1.0f / att : 1.0f;
        }
        ndl = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
        if (ndl < 0.0f) ndl = 0.0f;
        for (k = 0; k < 3; k++) {
            amb[k] = reg_f(b + 0x00 + 4 * k);
            dif[k] = reg_f(b + 0x0C + 4 * k);
            /* COLOR_MATERIAL: 1 in the ambient (bits 2-3) or diffuse (bits
             * 4-5) field takes that material colour from the vertex. */
            if (have_vc && ((colmat >> 2) & 3) == 1) amb[k] *= vc[k];
            if (have_vc && ((colmat >> 4) & 3) == 1) dif[k] *= vc[k];
            out[k] += att * (amb[k] + dif[k] * ndl);
        }
    }
    out[3] = (have_vc && ((colmat >> 4) & 3) == 1) ? vc[3] : reg_f(0x03B4);
    for (i = 0; i < 4; i++)
        out[i] = out[i] < 0.0f ? 0.0f : out[i] > 1.0f ? 1.0f : out[i];
    return 1;
}

/* One fixed-function vertex, in the shape a vertex program produces. */
static int ffp_vertex(uint32_t index, Nv2aVshOutput *out)
{
    const float *m = s_gpu.composite, *off = nv2a_vsh_constant(59);
    float in[4], clip[4], c[4], w;
    uint32_t st;
    int i;

    if (!fetch_attr(&s_gpu.attr[0], index, in))
        return 0;
    for (i = 0; i < 4; i++)
        clip[i] = m[4 * i] * in[0] + m[4 * i + 1] * in[1]
                + m[4 * i + 2] * in[2] + m[4 * i + 3] * in[3];
    /* A vertex behind the camera (w < 0) is kept: the clipper cuts it at the
     * near plane, and rejecting it dropped every batch that reached behind
     * the eye -- X-Men Legends' street and sidewalk meshes, which extend past
     * a top-down camera. Only w = 0 has no image. */
    w = clip[3];
    if (fabsf(w) <= 1e-6f)
        return 0;

    memset(out, 0, sizeof *out);
    out->pos[0] = (clip[0] / w + off[0]) * s_gpu.aa_sx;
    out->pos[1] = (clip[1] / w + off[1]) * s_gpu.aa_sy;
    out->pos[2] = clip[2] / w + off[2];
    out->pos[3] = w;

    if (!lit_color(index, c) && !fetch_attr(color_attr(), index, c))
        c[0] = c[1] = c[2] = c[3] = 1.0f;
    for (i = 0; i < 4; i++)
        out->d0[i] = c[i] < 0.0f ? 0.0f : c[i] > 1.0f ? 1.0f : c[i];

    fetch_attr(texcoord_attr(), index, out->tex[0]);
    for (st = 1; st < 4; st++)
        fetch_attr(&s_gpu.attr[9 + st], index, out->tex[st]);
    return 1;
}

/* ---- the same, with the batch's state resolved once ---------------------
 *
 * lit_color and ffp_vertex read every light's registers and resolve every
 * attribute (which slot, inline or guest memory, which format) for each
 * vertex: in a KOTOR dialogue close-up that was about 40 % of the executor's
 * time. Nothing they read can change inside a batch, so ffp_batch_begin
 * copies it into s_ffp once and ffp_batch_vertex reads only that and guest
 * memory. The arithmetic is lit_color's and ffp_vertex's, expression for
 * expression and light for light in the same order, so the result is bit for
 * bit the same; and since it writes only *out, it may run on a pool thread
 * (RECOMP_FFP_POOL). RECOMP_FFP_BATCH=0 keeps the per-vertex functions above. */
typedef struct {
    const uint8_t *base;    /* guest memory or the inline payload; NULL: absent */
    uint64_t offset;
    uint32_t stride, type, size, elem;
    size_t   limit;         /* inline payload: bytes buffered */
    int      inl;
} FfpAttr;

typedef struct {
    int   kind;             /* 1 infinite, else local / spot */
    float l[3];             /* direction (infinite) or eye-space position */
    float range, att[3];
    float amb[3], dif[3];
} FfpLight;

static struct {
    FfpAttr pos, nrm, col, tex[4];
    float   m[16], off[3], aa_sx, aa_sy;
    int     lit, flip, amb_vc, dif_vc, nlights;
    float   mv[12], scene[3], alpha;
    FfpLight light[NV_LIGHTS];
} s_ffp;

static int s_ffp_batch = -1;                /* -1: read RECOMP_FFP_BATCH */
void nv2a_ffp_batch_enable(int on) { s_ffp_batch = on ? 1 : 0; }

static int ffp_batch_on(void)
{
    if (s_ffp_batch < 0) {
        const char *e = getenv("RECOMP_FFP_BATCH");
        s_ffp_batch = !(e && e[0] == '0');
    }
    return s_ffp_batch;
}

static void ffp_attr_resolve(FfpAttr *f, const VertexAttr *a)
{
    memset(f, 0, sizeof *f);
    if (!a->size || !a->stride)
        return;
    f->offset = a->offset;
    f->stride = a->stride;
    f->type = a->type;
    f->size = a->size;
    f->elem = attr_elem_bytes(a);
    f->inl = s_gpu.inline_active;
    if (f->inl) {
        f->base = (const uint8_t *)s_gpu.inline_buf;
        f->limit = (size_t)s_gpu.inline_count * 4;
    } else if (a->offset) {
        f->base = (const uint8_t *)xbox_GetMemoryOffset();
    }
}

/* fetch_attr on a resolved attribute. */
static int ffp_fetch(const FfpAttr *f, uint32_t index, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!f->base)
        return 0;
    if (f->inl) {
        size_t at = (size_t)f->offset + (size_t)index * f->stride;
        if (at + 4 > f->limit)
            return 0;
        return decode_attr(f->type, f->size, f->base + at, out);
    } else {
        uint64_t at = f->offset + (uint64_t)index * f->stride;
        if (!guest_span_ok(at, f->elem))
            return 0;
        return decode_attr(f->type, f->size, f->base + at, out);
    }
}

static void ffp_batch_begin(void)
{
    const float *off = nv2a_vsh_constant(59);
    uint32_t mask = s_reg[0x03BC / 4], colmat = s_reg[0x0298 / 4], i;
    int k;

    ffp_attr_resolve(&s_ffp.pos, &s_gpu.attr[0]);
    ffp_attr_resolve(&s_ffp.nrm, &s_gpu.attr[2]);
    ffp_attr_resolve(&s_ffp.col, color_attr());
    ffp_attr_resolve(&s_ffp.tex[0], texcoord_attr());
    for (i = 1; i < 4; i++)
        ffp_attr_resolve(&s_ffp.tex[i], &s_gpu.attr[9 + i]);
    memcpy(s_ffp.m, s_gpu.composite, sizeof s_ffp.m);
    s_ffp.off[0] = off[0]; s_ffp.off[1] = off[1]; s_ffp.off[2] = off[2];
    s_ffp.aa_sx = s_gpu.aa_sx;
    s_ffp.aa_sy = s_gpu.aa_sy;

    s_ffp.lit = s_reg[0x0314 / 4] && s_gpu.attr[2].size;
    s_ffp.nlights = 0;
    if (!s_ffp.lit)
        return;
    memcpy(s_ffp.mv, &s_reg[0x0480 / 4], sizeof s_ffp.mv);
    s_ffp.flip = ffp_flip_normals();
    s_ffp.amb_vc = ((colmat >> 2) & 3) == 1;
    s_ffp.dif_vc = ((colmat >> 4) & 3) == 1;
    s_ffp.scene[0] = reg_f(0x0A10);
    s_ffp.scene[1] = reg_f(0x0A14);
    s_ffp.scene[2] = reg_f(0x0A18);
    s_ffp.alpha = reg_f(0x03B4);
    for (i = 0; i < NV_LIGHTS; i++) {
        uint32_t kind = (mask >> (2 * i)) & 3, b = 0x1000 + i * 0x80;
        FfpLight *L = &s_ffp.light[s_ffp.nlights];

        if (!kind)
            continue;
        L->kind = (int)kind;
        if (kind == 1) {
            L->l[0] = reg_f(b + 0x34); L->l[1] = reg_f(b + 0x38); L->l[2] = reg_f(b + 0x3C);
        } else {
            L->l[0] = reg_f(b + 0x5C); L->l[1] = reg_f(b + 0x60); L->l[2] = reg_f(b + 0x64);
            L->range = reg_f(b + 0x24);
            L->att[0] = reg_f(b + 0x68);
            L->att[1] = reg_f(b + 0x6C);
            L->att[2] = reg_f(b + 0x70);
        }
        for (k = 0; k < 3; k++) {
            L->amb[k] = reg_f(b + 0x00 + 4 * (uint32_t)k);
            L->dif[k] = reg_f(b + 0x0C + 4 * (uint32_t)k);
        }
        s_ffp.nlights++;
    }
}

/* lit_color, for a vertex whose position `pos` is already fetched. */
static int ffp_batch_lit(uint32_t index, const float pos[4], float out[4])
{
    const float *mv = s_ffp.mv;
    float nrm[4], vc[4], e[3], n[3], len;
    int i, have_vc;

    if (!s_ffp.lit || !ffp_fetch(&s_ffp.nrm, index, nrm))
        return 0;
    have_vc = ffp_fetch(&s_ffp.col, index, vc);

    for (i = 0; i < 3; i++) {
        e[i] = mv[4 * i] * pos[0] + mv[4 * i + 1] * pos[1]
             + mv[4 * i + 2] * pos[2] + mv[4 * i + 3];
        n[i] = mv[4 * i] * nrm[0] + mv[4 * i + 1] * nrm[1]
             + mv[4 * i + 2] * nrm[2];
    }
    len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    if (s_ffp.flip && n[0] * e[0] + n[1] * e[1] + n[2] * e[2] > 0.0f) {
        n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
    }

    out[0] = s_ffp.scene[0]; out[1] = s_ffp.scene[1]; out[2] = s_ffp.scene[2];
    for (i = 0; i < s_ffp.nlights; i++) {
        const FfpLight *L = &s_ffp.light[i];
        float l[3], att = 1.0f, ndl, amb[3], dif[3];
        int k;

        if (L->kind == 1) {
            l[0] = L->l[0]; l[1] = L->l[1]; l[2] = L->l[2];
        } else {
            float d;
            l[0] = L->l[0] - e[0];
            l[1] = L->l[1] - e[1];
            l[2] = L->l[2] - e[2];
            d = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            if (d > L->range && L->range > 0.0f)
                continue;
            if (d > 1e-8f) { l[0] /= d; l[1] /= d; l[2] /= d; }
            att = L->att[0] + L->att[1] * d + L->att[2] * d * d;
            att = att > 1e-8f ? 1.0f / att : 1.0f;
        }
        ndl = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
        if (ndl < 0.0f) ndl = 0.0f;
        for (k = 0; k < 3; k++) {
            amb[k] = L->amb[k];
            dif[k] = L->dif[k];
            if (have_vc && s_ffp.amb_vc) amb[k] *= vc[k];
            if (have_vc && s_ffp.dif_vc) dif[k] *= vc[k];
            out[k] += att * (amb[k] + dif[k] * ndl);
        }
    }
    out[3] = (have_vc && s_ffp.dif_vc) ? vc[3] : s_ffp.alpha;
    for (i = 0; i < 4; i++)
        out[i] = out[i] < 0.0f ? 0.0f : out[i] > 1.0f ? 1.0f : out[i];
    return 1;
}

/* ffp_vertex after ffp_batch_begin. */
static int ffp_batch_vertex(uint32_t index, Nv2aVshOutput *out)
{
    const float *m = s_ffp.m, *off = s_ffp.off;
    float in[4], clip[4], c[4], w;
    uint32_t st;
    int i;

    if (!ffp_fetch(&s_ffp.pos, index, in))
        return 0;
    for (i = 0; i < 4; i++)
        clip[i] = m[4 * i] * in[0] + m[4 * i + 1] * in[1]
                + m[4 * i + 2] * in[2] + m[4 * i + 3] * in[3];
    w = clip[3];
    if (fabsf(w) <= 1e-6f)
        return 0;

    memset(out, 0, sizeof *out);
    out->pos[0] = (clip[0] / w + off[0]) * s_ffp.aa_sx;
    out->pos[1] = (clip[1] / w + off[1]) * s_ffp.aa_sy;
    out->pos[2] = clip[2] / w + off[2];
    out->pos[3] = w;

    if (!ffp_batch_lit(index, in, c) && !ffp_fetch(&s_ffp.col, index, c))
        c[0] = c[1] = c[2] = c[3] = 1.0f;
    for (i = 0; i < 4; i++)
        out->d0[i] = c[i] < 0.0f ? 0.0f : c[i] > 1.0f ? 1.0f : c[i];

    for (st = 0; st < 4; st++)
        ffp_fetch(&s_ffp.tex[st], index, out->tex[st]);
    return 1;
}

/* An untransformed batch drawn as if it were screen space smears a few pixels
 * into the corner, so the batch has to be classified before it is rasterised.
 *
 * This used to demand that every vertex land inside the surface, which is a
 * different question and the wrong one: geometry that extends past the
 * viewport is ordinary, and clipping it is raster_triangle's job (it clamps
 * its span to the clip rect). The dashboard is exactly the case that exposed
 * it -- a full-screen pass drawn as one oversized triangle, vertices at
 * (-0.5,-0.5), (2*w,-0.5), (-0.5,2*h), all correct and all rejected.
 *
 * What actually separates the two is scale. Object-space positions are model
 * units, a handful either side of the origin; screen-space ones are measured
 * in pixels of a surface hundreds of pixels wide. So: the batch has to be able
 * to touch the surface at all, and it has to be bigger than object space.
 *
 * ponytail: a genuinely tiny screen-space sprite reads as object space and is
 * skipped. It is counted as skipped rather than silently dropped, and the
 * unambiguous answer needs the vertex-program state, which is not tracked yet.
 */
#define OBJECT_SPACE_SPAN 8.0f

/* The surface's anti-aliasing scale, never below 1, for positions that arrive
 * in logical pixels (pre-transformed vertices) and must land in real ones. */
static float aa_scale_x(void) { return s_gpu.aa_sx > 1.0f ? s_gpu.aa_sx : 1.0f; }
static float aa_scale_y(void) { return s_gpu.aa_sy > 1.0f ? s_gpu.aa_sy : 1.0f; }

static int batch_is_screen_space(void)
{
    float p[4], lo_x, hi_x, lo_y, hi_y;
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h || !s_gpu.idx_count)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < s_gpu.idx_count; i++) {
        if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], p))
            return 0;
        if (p[0] < lo_x) lo_x = p[0];
        if (p[0] > hi_x) hi_x = p[0];
        if (p[1] < lo_y) lo_y = p[1];
        if (p[1] > hi_y) hi_y = p[1];
    }

    /* Entirely off the surface: nothing to draw under either reading. The
     * positions are in logical pixels and the clip rect is in real ones. */
    if (hi_x * aa_scale_x() < (float)s_gpu.clip_x
     || lo_x * aa_scale_x() > (float)(s_gpu.clip_x + s_gpu.clip_w)
     || hi_y * aa_scale_y() < (float)s_gpu.clip_y
     || lo_y * aa_scale_y() > (float)(s_gpu.clip_y + s_gpu.clip_h))
        return 0;

    /* Small enough to be model units rather than pixels. */
    if (hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

/* NV097 primitive types.
 *
 * These are the operand of SET_BEGIN_END, where 0 is END and the list starts
 * at 1. They were each one too low, so every title's geometry was decomposed
 * as the primitive below the one it asked for -- a strip as a fan, a fan as
 * quads, and TRIANGLES, the one case whose vertex count must be a multiple
 * of three, as a strip.
 *
 * The vertex order says which numbering is right without taking a table on
 * trust: a strip arrives in Z order and a fan in cyclic order, and they only
 * line up with the primitive under this one. */
#define NV_PRIM_POINTS         1
#define NV_PRIM_LINES          2
#define NV_PRIM_LINE_LOOP      3
#define NV_PRIM_LINE_STRIP     4
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9
#define NV_PRIM_POLYGON        10

/* How many post-draw captures to keep: enough to see whether the geometry
 * is stable from frame to frame, few enough not to fill a directory. */
#define FB_DUMP_AFTER_DRAW 8
static int s_drawn_dumps;

static void dump_surface_bmp(void);

/* A fixed-function batch without texture coordinates is drawn untextured, as
 * the screen-space path does, whatever stale texture the stage still holds. */
static int s_xf_no_tex;

/* Triangles bound for a back end: collected for the whole batch, handed over
 * in one draw() call by backend_flush(). Whatever feeds them -- the program
 * and fixed-function paths here, or the screen-space path below -- the back
 * end only ever sees surface pixels. Past the cap the rest of the batch is
 * dropped. */
#define NV_BACKEND_MAX_VERTS (NV_MAX_INDICES * 3)
static Nv2aVertex *s_bverts;     /* about 5.5 MB: allocated by the first vertex */
static uint32_t   s_bcount;
/* The full vertices, for a back end with NV2A_BACKEND_CAP_PIXEL (about 23 MB,
 * allocated only then). Same index as s_bverts. */
static Nv2aVertexFull *s_bfull;

static int backend_wants_pixel(void)
{
    return s_backend && (s_backend->caps & NV2A_BACKEND_CAP_PIXEL);
}

static Nv2aVertex *backend_slot(void)
{
    if (!s_bverts
        && !(s_bverts = (Nv2aVertex *)calloc(NV_BACKEND_MAX_VERTS, sizeof *s_bverts)))
        return NULL;
    if (backend_wants_pixel() && !s_bfull
        && !(s_bfull = (Nv2aVertexFull *)calloc(NV_BACKEND_MAX_VERTS, sizeof *s_bfull)))
        return NULL;
    if (s_bcount + 3 > NV_BACKEND_MAX_VERTS)
        return NULL;
    return &s_bverts[s_bcount];
}

static float fog_factor(float d);

static void backend_from_xf(const Nv2aVshOutput *v, Nv2aVertex *o)
{
    float su = 1.0f, sv = 1.0f;

    if (tex_size_from_format(s_gpu.texs[0].color)) {
        su = (float)s_gpu.texs[0].width;
        sv = (float)s_gpu.texs[0].height;
    }
    o->x = v->pos[0];
    o->y = v->pos[1];
    o->z = v->pos[2];
    o->rhw = 1.0f / v->pos[3];
    o->diffuse = pack_color(v->d0);
    o->u = v->tex[0][0] * su;
    o->v = v->tex[0][1] * sv;
    if (s_bfull) {
        Nv2aVertexFull *f = &s_bfull[o - s_bverts];
        memcpy(f->pos, v->pos, sizeof f->pos);
        memcpy(f->d0, v->d0, sizeof f->d0);
        memcpy(f->d1, v->d1, sizeof f->d1);
        memcpy(f->tex, v->tex, sizeof f->tex);
        f->fog = fog_factor(v->fog[0]);
    }
}

/* What the CPU rasteriser would do with this batch's pixels (see
 * nv2a_backend.h), for a back end with NV2A_BACKEND_CAP_PIXEL. `textured`
 * as for backend_flush. */
static void current_pixel_state(Nv2aPixelState *p, uint32_t shade, int textured)
{
    int st;

    memset(p, 0, sizeof *p);
    p->shade = shade;
    p->rc = s_gpu.rc;
    /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
    p->fog_color = (s_gpu.fog_color & 0xFF00FF00u)
                 | ((s_gpu.fog_color & 0xFFu) << 16)
                 | ((s_gpu.fog_color >> 16) & 0xFFu);
    p->clip_plane_mode = s_gpu.clip_plane_mode;
    p->shadow_func = s_reg[0x1E6C / 4] & 7;            /* SET_SHADOW_DEPTH_FUNC */
    for (st = 0; st < 4; st++) {
        const Texture *t = &s_gpu.texs[st];
        if (shade == NV2A_SHADE_COMBINERS) {
            p->stage[st].mode = (s_gpu.rc.stage_program >> (st * 5)) & 0x1F;
            p->stage[st].valid = (uint32_t)t->valid;
        } else {
            /* Stage 0 only, and only if the batch carries coordinates. */
            p->stage[st].mode = st == 0 && textured && t->valid ? 1u : 0u;
            p->stage[st].valid = p->stage[st].mode;
        }
        if (p->stage[st].valid)
            texture_to_backend(t, &p->stage[st].tex);
        if (st > 0 && (p->stage[st].mode == 6 || p->stage[st].mode == 7)) {
            /* SET_TEXTURE_SET_BUMP_ENV_MAT/_SCALE/_OFFSET(st): words 00,
             * 01, 11, 10 (xemu pgraph.c's swizzle), scale, offset. */
            const uint32_t *m = &s_reg[(0x1B28u + 64u * (uint32_t)st) / 4];
            Nv2aTexStage *o = &p->stage[st];
            memcpy(&o->bump_mat[0], &m[0], 4);
            memcpy(&o->bump_mat[1], &m[1], 4);
            memcpy(&o->bump_mat[3], &m[2], 4);
            memcpy(&o->bump_mat[2], &m[3], 4);
            memcpy(&o->bump_scale, &m[4], 4);
            memcpy(&o->bump_offset, &m[5], 4);
        }
    }
    p->other_stage_input = s_reg[0x1E78 / 4];          /* SET_SHADER_OTHER_STAGE_INPUT */
}

/* `textured`: the batch carried texture coordinates, so the bound texture is
 * part of it. `screen`: the batch came down the screen-space path (no
 * combiners, no depth on the CPU rasteriser). */
static void backend_flush(int textured, int screen)
{
    Nv2aSurface surf;
    Nv2aRenderState rs;
    Nv2aTexture tex;
    Nv2aBatch batch;
    Nv2aPixelState ps;

    if (!s_bcount)
        return;
    current_surface(&surf);
    current_render_state(&rs);
    batch.vertices = s_bverts;
    batch.count = s_bcount;
    batch.texture = NULL;
    batch.state = &rs;
    batch.full = NULL;
    batch.pixel = NULL;
    batch.program = NULL;
    if (textured && s_gpu.texs[0].valid) {
        texture_to_backend(&s_gpu.texs[0], &tex);
        batch.texture = &tex;
    }
    if (backend_wants_pixel() && s_bfull) {
        static int no_rc = -1;
        uint32_t shade;
        if (no_rc < 0)
            no_rc = getenv("RECOMP_NO_COMBINERS") != NULL;
        shade = screen ? NV2A_SHADE_REPLACE
              : (s_gpu.rc_seen && !no_rc) ? NV2A_SHADE_COMBINERS
              : NV2A_SHADE_MODULATE;
        current_pixel_state(&ps, shade, textured);
        batch.full = s_bfull;
        batch.pixel = &ps;
    }
    s_backend->draw(&surf, &batch);
    s_gpu.tris_drawn += s_bcount / 3;
    s_bcount = 0;
    note_drawn();
}

/* One triangle by vertex index: gather position and, if the batch has one,
 * texture coordinate 0. A vertex whose position cannot be read is not drawn;
 * a batch whose texcoords cannot be read is drawn untextured rather than not
 * at all, so a missing coordinate stream costs the colour and not the shape.
 */
static void raster_indexed(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb)
{
    float p[3][4], uv[3][2];
    int textured, j;

    if (!fetch_attr(&s_gpu.attr[0], i0, p[0])
     || !fetch_attr(&s_gpu.attr[0], i1, p[1])
     || !fetch_attr(&s_gpu.attr[0], i2, p[2]))
        return;
    for (j = 0; j < 3; j++) {                        /* logical -> real pixels */
        p[j][0] *= aa_scale_x();
        p[j][1] *= aa_scale_y();
    }

    textured = fetch_texcoord(i0, uv[0])
            && fetch_texcoord(i1, uv[1])
            && fetch_texcoord(i2, uv[2]);

    if (backend_draws()) {                 /* a back end rasterises */
        Nv2aVertex *o = backend_slot();
        const uint32_t ix[3] = { i0, i1, i2 };
        int k;

        if (!o)
            return;
        for (k = 0; k < 3; k++) {
            o[k].x = p[k][0];
            o[k].y = p[k][1];
            o[k].z = p[k][2];
            o[k].rhw = p[k][3];
            o[k].diffuse = vertex_color(ix[k]);
            o[k].u = textured ? uv[k][0] : 0.0f;
            o[k].v = textured ? uv[k][1] : 0.0f;
            if (s_bfull) {
                /* The CPU path draws these flat in the first vertex's
                 * colour (argb) and without perspective. */
                Nv2aVertexFull *f = &s_bfull[s_bcount + (uint32_t)k];
                memset(f, 0, sizeof *f);
                f->pos[0] = p[k][0];
                f->pos[1] = p[k][1];
                f->pos[2] = p[k][2];
                f->pos[3] = 1.0f;
                nv2a_rc_unpack(argb, f->d0);
                f->fog = 1.0f;
                /* Texel units, as the CPU path samples: the texture is
                 * handed over as linear-addressed by these coordinates. */
                f->tex[0][0] = textured ? uv[k][0] : 0.0f;
                f->tex[0][1] = textured ? uv[k][1] : 0.0f;
                f->tex[0][3] = 1.0f;
            }
        }
        s_bcount += 3;
        return;
    }
    raster_triangle(p[0], p[1], p[2], argb,
                    textured ? (const float (*)[2])uv : NULL);
}

/* ---- 3D: vertex programs and depth -------------------------------------
 *
 * A batch drawn while TRANSFORM_EXECUTION_MODE says "program" carries
 * model-space positions. Each vertex is run through the title's own vertex
 * program (nv2a_vsh_interp.c), whose oPos output on Xbox D3D is already
 * screen space -- the runtime appends the viewport transform and the divide
 * by w -- with the clip-space w left in oPos.w. So the rasteriser gets pixel
 * coordinates and a depth, and w for perspective-correct texturing.
 *
 * ponytail: no clipping. A triangle with any vertex behind the eye (w <= 0)
 * is dropped rather than clipped, which loses the slivers that cross the near
 * plane; road right under the camera is where that shows. Clip in
 * homogeneous space if it matters. */

/* Depth buffers, host-side, one per zeta surface the title uses. The real one
 * lives in guest memory in a tiled format nothing here decodes, and nothing
 * the title does reads it back, so a float buffer per zeta offset is enough. */
#define NV_ZBUF_W 1280   /* 640x480 at 2x2 anti-aliasing */
#define NV_ZBUF_H 1024
static struct { uint32_t offset; float *z; } s_zbufs[4];
static int s_ftrace;            /* frame trace: 0 idle, 2 tracing, 3 done */
static int s_zbuf_next;

static float zclear_value(void)
{
    /* Zeta format in SURFACE_FORMAT bits 4-7: 1 is Z16, 2 is Z24S8. */
    uint32_t zf = (s_gpu.format >> 4) & 0xF;
    return zf == 1 ? (float)(s_gpu.zstencil_clear & 0xFFFF)
                   : (float)(s_gpu.zstencil_clear >> 8);
}

static float *zbuf_current(int create)
{
    int i;
    size_t k;

    for (i = 0; i < 4; i++)
        if (s_zbufs[i].z && s_zbufs[i].offset == s_gpu.zeta_offset)
            return s_zbufs[i].z;
    if (!create)
        return NULL;
    i = s_zbuf_next++ & 3;
    if (!s_zbufs[i].z)
        s_zbufs[i].z = (float *)malloc(sizeof(float) * NV_ZBUF_W * NV_ZBUF_H);
    if (!s_zbufs[i].z)
        return NULL;
    s_zbufs[i].offset = s_gpu.zeta_offset;
    for (k = 0; k < (size_t)NV_ZBUF_W * NV_ZBUF_H; k++)
        s_zbufs[i].z[k] = 3.4e38f;
    return s_zbufs[i].z;
}

static void zbuf_clear(void)
{
    float *z = zbuf_current(1), v = zclear_value();
    size_t k;
    if (z)
        for (k = 0; k < (size_t)NV_ZBUF_W * NV_ZBUF_H; k++)
            z[k] = v;
}

static int depth_pass(float z, float stored)
{
    switch (s_gpu.depth_func) {                   /* GL enums, as the NV2A */
    case 0x200: return 0;
    case 0x201: return z <  stored;
    case 0x202: return z == stored;
    case 0x203: return z <= stored;
    case 0x204: return z >  stored;
    case 0x205: return z != stored;
    case 0x206: return z >= stored;
    default:    return 1;
    }
}

static uint32_t pack_color(const float c[4])
{
    int i;
    uint32_t b[4];
    for (i = 0; i < 4; i++) {
        float f = c[i] < 0.0f ? 0.0f : (c[i] > 1.0f ? 1.0f : c[i]);
        b[i] = (uint32_t)(f * 255.0f + 0.5f);
    }
    return (b[3] << 24) | (b[0] << 16) | (b[1] << 8) | b[2];
}

static uint32_t modulate(uint32_t t, const float c[4])
{
    float f[4];
    f[0] = (float)((t >> 16) & 0xFF) / 255.0f * c[0];
    f[1] = (float)((t >> 8) & 0xFF) / 255.0f * c[1];
    f[2] = (float)(t & 0xFF) / 255.0f * c[2];
    f[3] = (float)(t >> 24) / 255.0f * c[3];
    return pack_color(f);
}

/* Bytes one texel of a (non-DXT) format takes, for cube-face strides. */
static uint32_t tex_texel_bytes(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1C: case 0x1D:
        return 2;
    default:
        return 4;
    }
}

/* Bytes between cube faces: one face with all its mip levels, rounded up to
 * the NV2A's 128-byte face alignment (xemu texture.c). */
static uint32_t tex_face_stride(const Texture *t)
{
    uint32_t w = t->width, h = t->height, lv, total = 0;
    uint32_t block = d3d8_format_dxt_block_bytes(t->color);
    uint32_t levels = t->levels ? t->levels : 1;

    if (!tex_size_from_format(t->color))
        return (t->pitch * t->height + 127u) & ~127u;
    for (lv = 0; lv < levels; lv++) {
        uint32_t lw = (w >> lv) ? (w >> lv) : 1, lh = (h >> lv) ? (h >> lv) : 1;
        total += block ? ((lw + 3) / 4) * ((lh + 3) / 4) * block
                       : lw * lh * tex_texel_bytes(t->color);
    }
    return (total + 127u) & ~127u;
}

/* One texel at normalised (swizzled/DXT) or texel (linear) coordinates, as
 * floats. An unusable stage reads white, so a missing texture multiplies
 * through instead of blacking the pixel out. */
static void rc_tent(const Texture *t, uint32_t face, float u, float v,
                    float out[4])
{
    uint32_t texel;
    float fu = u - 0.5f, fv = v - 0.5f, wu, wv, c[4][4];
    int32_t iu = (int32_t)floorf(fu), iv = (int32_t)floorf(fv), k, j;

    wu = fu - (float)iu;
    wv = fv - (float)iv;
    /* The 2x2 footprint inside one DXT block and inside the image (no wrap,
     * clamp or border to apply): decode the block once and read the four
     * texels from it, instead of four trips through sample_tex. Bilinear is
     * what Fable: The Lost Chapters asks for on its frontend, and those four
     * trips were half of the rasteriser's time. Same texels as the general
     * path below, which every other case still takes. */
    if (t->valid && d3d8_format_dxt_block_bytes(t->color)
        && iu >= 0 && iv >= 0 && (uint32_t)iu + 1 < t->width
        && (uint32_t)iv + 1 < t->height && (iu & 3) != 3 && (iv & 3) != 3) {
        const uint32_t *px = dxt_block_px(t, t->offset + face,
                                          (uint32_t)iu >> 2, (uint32_t)iv >> 2);
        uint32_t o = (uint32_t)(iv & 3) * 4 + (uint32_t)(iu & 3);
        nv2a_rc_unpack(px[o], c[0]);
        nv2a_rc_unpack(px[o + 1], c[1]);
        nv2a_rc_unpack(px[o + 4], c[2]);
        nv2a_rc_unpack(px[o + 5], c[3]);
        for (j = 0; j < 4; j++)
            out[j] = (c[0][j] * (1.0f - wu) + c[1][j] * wu) * (1.0f - wv)
                   + (c[2][j] * (1.0f - wu) + c[3][j] * wu) * wv;
        return;
    }
    for (k = 0; k < 4; k++) {
        if (sample_tex(t, face, (uint32_t)(iu + (k & 1)),
                       (uint32_t)(iv + (k >> 1)), &texel))
            nv2a_rc_unpack(texel, c[k]);
        else
            c[k][0] = c[k][1] = c[k][2] = c[k][3] = 1.0f;
    }
    for (j = 0; j < 4; j++)
        out[j] = (c[0][j] * (1.0f - wu) + c[1][j] * wu) * (1.0f - wv)
               + (c[2][j] * (1.0f - wu) + c[3][j] * wu) * wv;
}

/* `g`, when not NULL, is this pixel's gradient of (u, v): du/dx, dv/dx, du/dy,
 * dv/dy, in the units u and v are given in. RECOMP_TEX_ANISO uses it. */
static void rc_texel(const Texture *t, uint32_t face, float u, float v,
                     const float *g, float out[4])
{
    uint32_t texel, mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    int bilinear, aniso;
    float gs[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    tex_filter_config(&bilinear, &aniso);
    if (tex_size_from_format(t->color)) {
        u *= (float)t->width;
        v *= (float)t->height;
        if (g) {
            gs[0] = g[0] * (float)t->width;  gs[1] = g[1] * (float)t->height;
            gs[2] = g[2] * (float)t->width;  gs[3] = g[3] * (float)t->height;
            g = gs;
        }
    }
    /* RECOMP_TEX_ANISO (per-title opt-in): bilinear taps along the footprint,
     * whatever filter the title asked for. */
    if (aniso && g) {
        float gx[2] = { g[0], g[1] }, gy[2] = { g[2], g[3] }, start[2], step[2];
        float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f}, tap[4];
        uint32_t n = aniso_footprint(gx, gy, (uint32_t)aniso, start, step), i;
        int j;
        for (i = 0; i < n; i++) {
            rc_tent(t, face, u + start[0] + (float)i * step[0],
                    v + start[1] + (float)i * step[1], tap);
            for (j = 0; j < 4; j++)
                sum[j] += tap[j];
        }
        for (j = 0; j < 4; j++)
            out[j] = sum[j] / (float)n;
        return;
    }
    /* TENT (bilinear) when the title asks for it, magnifying or minifying:
     * a 64x32 sky gradient stretched over the screen is blocks with nearest
     * texels and a gradient with four. MAG 2 is tent; MIN 2 is tent at LOD 0
     * and 4/6 the tent mip modes. RECOMP_TEX_BILINEAR (per-title opt-in)
     * asks for it for every texture.
     * ponytail: level 0 only, no mip selection -- minified textures shimmer.
     * Add LOD from the screen-space derivative when that shows. */
    if (bilinear || mag == 2 || min == 2 || min == 4 || min == 6) {
        rc_tent(t, face, u, v, out);
        return;
    }
    if (sample_tex(t, face, (uint32_t)(int32_t)floorf(u),
                   (uint32_t)(int32_t)floorf(v), &texel))
        nv2a_rc_unpack(texel, out);
    else
        out[0] = out[1] = out[2] = out[3] = 1.0f;
}

/* What texture stage `st` contributes, by its SHADER_STAGE_PROGRAM mode
 * (xemu psh.c). Returns 0 when a clip-plane stage kills the pixel. */
static int rc_stage_fetch(int st, const float c[4], const float *cx,
                          const float *cy, float out[4])
{
    const Texture *t = &s_gpu.texs[st];
    uint32_t mode = (s_gpu.rc.stage_program >> (st * 5)) & 0x1F, j;
    float q = c[3] != 0.0f ? c[3] : 1.0f;

    switch (mode) {
    case 0:                                       /* NONE */
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
        return 1;
    case 1: case 2:                               /* PROJECT2D / 3D */
        if (cx && cy) {                           /* neighbours: the gradient */
            float qx = cx[3] != 0.0f ? cx[3] : 1.0f, qy = cy[3] != 0.0f ? cy[3] : 1.0f;
            float u = c[0] / q, v = c[1] / q;
            float g[4] = { cx[0] / qx - u, cx[1] / qx - v,
                           cy[0] / qy - u, cy[1] / qy - v };
            rc_texel(t, 0, u, v, g, out);
            return 1;
        }
        rc_texel(t, 0, c[0] / q, c[1] / q, NULL, out);
        return 1;
    case 3: {                                     /* CUBEMAP */
        float x = c[0], y = c[1], z = c[2];
        float ax = fabsf(x), ay = fabsf(y), az = fabsf(z), ma, sc, tc;
        uint32_t face;
        if (!t->cube) {
            rc_texel(t, 0, c[0], c[1], NULL, out);
            return 1;
        }
        if (ax >= ay && ax >= az) {
            face = x > 0 ? 0 : 1; ma = ax; sc = x > 0 ? -z : z; tc = -y;
        } else if (ay >= az) {
            face = y > 0 ? 2 : 3; ma = ay; sc = x; tc = y > 0 ? z : -z;
        } else {
            face = z > 0 ? 4 : 5; ma = az; sc = z > 0 ? x : -x; tc = -y;
        }
        if (ma == 0.0f)
            ma = 1.0f;
        rc_texel(t, face * tex_face_stride(t), (sc / ma + 1.0f) * 0.5f,
                 (tc / ma + 1.0f) * 0.5f, NULL, out);
        return 1;
    }
    case 4:                                       /* PASSTHRU */
        for (j = 0; j < 4; j++)
            out[j] = c[j] < 0.0f ? 0.0f : (c[j] > 1.0f ? 1.0f : c[j]);
        return 1;
    case 5:                                       /* CLIPPLANE */
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        for (j = 0; j < 4; j++) {
            int ge = (s_gpu.clip_plane_mode >> (st * 4 + j)) & 1;
            if (ge ? c[j] >= 0.0f : c[j] < 0.0f)
                return 0;
        }
        return 1;
    default:
        /* ponytail: bump-env and dot-product modes read zero; they are water
         * and bump effects -- add them when a title's look depends on one. */
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return 1;
    }
}

/* The fixed-function fog unit, per vertex, from the program's oFog.x
 * (xemu vsh.c). Fog disabled reads as factor 1: no fog. */
static float fog_factor(float d)
{
    float f, px = s_gpu.fog_param[0], py = s_gpu.fog_param[1];
    if (!s_gpu.fog_enable)
        return 1.0f;
    switch (s_gpu.fog_mode) {
    case 0x800: case 0x802:                       /* EXP, EXP_ABS */
        f = px + exp2f(d * py * 16.0f) - 1.5f;
        break;
    case 0x801: case 0x803:                       /* EXP2, EXP2_ABS */
        f = px + exp2f(-d * d * py * py * 32.0f) - 1.5f;
        break;
    default:                                      /* LINEAR, LINEAR_ABS */
        f = px + d * py - 1.0f;
        break;
    }
    if (s_gpu.fog_mode == 0x802 || s_gpu.fog_mode == 0x803
        || s_gpu.fog_mode == 0x804)
        f = fabsf(f);
    if (f != f)
        f = 1.0f;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

static int alpha_test_pass(float a)
{
    int v = (int)(a * 255.0f + 0.5f), r = (int)(s_gpu.alpha_ref & 0xFF);
    switch (s_gpu.alpha_func) {
    case 0x200: return 0;                         /* NEVER    */
    case 0x201: return v <  r;                    /* LESS     */
    case 0x202: return v == r;                    /* EQUAL    */
    case 0x203: return v <= r;                    /* LEQUAL   */
    case 0x204: return v >  r;                    /* GREATER  */
    case 0x205: return v != r;                    /* NOTEQUAL */
    case 0x206: return v >= r;                    /* GEQUAL   */
    default:    return 1;                         /* ALWAYS   */
    }
}

/* Everything a triangle's pixels need, computed once per triangle, so the
 * pixel loop can run over any subset of rows on any thread. */
typedef struct {
    uint8_t *mem;
    uint32_t bpp;
    const Nv2aVshOutput *va, *vb, *vc;
    const float *a, *b, *c;
    float iw[3], uv[3][2], stc[3][4][4], vfog[3], fogc[4], inv_area;
    float dl[3][2];                  /* d(l0,l1,l2)/dx and /dy, for texel gradients */
    float *zb;
    int minx, maxx, miny, maxy, use_rc, textured;
} XfTri;

typedef struct { uint64_t depth_fail, pixels; uint32_t zpass; } XfCount;

/* Perspective-correct 1/w at the barycentrics (l0, l1, l2) moved one pixel
 * right (axis 0) or down (axis 1), and the matching barycentrics. */
static float xf_step_l(const XfTri *T, float l0, float l1, float l2, int axis,
                       float *m0, float *m1, float *m2)
{
    *m0 = l0 + T->dl[0][axis];
    *m1 = l1 + T->dl[1][axis];
    *m2 = l2 + T->dl[2][axis];
    return *m0 * T->iw[0] + *m1 * T->iw[1] + *m2 * T->iw[2];
}

/* Texture coordinate of stage `st` one pixel over, as the pixel loop makes it. */
static void xf_tc_step(const XfTri *T, int st, float l0, float l1, float l2,
                       int axis, float out[4])
{
    float m0, m1, m2, pw = xf_step_l(T, l0, l1, l2, axis, &m0, &m1, &m2);
    int j;

    for (j = 0; j < 4; j++)
        out[j] = (m0 * T->stc[0][st][j] + m1 * T->stc[1][st][j]
                + m2 * T->stc[2][st][j]) / pw;
}

/* The same for the fixed-function uv. */
static void xf_uv_step(const XfTri *T, float l0, float l1, float l2, int axis,
                       float *u, float *v)
{
    float m0, m1, m2, pw = xf_step_l(T, l0, l1, l2, axis, &m0, &m1, &m2);

    *u = (m0 * T->uv[0][0] + m1 * T->uv[1][0] + m2 * T->uv[2][0]) / pw;
    *v = (m0 * T->uv[0][1] + m1 * T->uv[1][1] + m2 * T->uv[2][1]) / pw;
}

/* Rows y0, y0+step, ... < maxy of triangle T. */
static void xf_rows(const XfTri *T, int y0, int step, XfCount *cnt)
{
    const float *a = T->a, *b = T->b, *c = T->c;
    const Nv2aVshOutput *va = T->va, *vb = T->vb, *vc = T->vc;
    int x, y, k, bilinear, aniso;

    tex_filter_config(&bilinear, &aniso);
    for (y = y0; y < T->maxy; y += step) {
        for (x = T->minx; x < T->maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            float l0, l1, l2, z, col[4], pw;
            uint32_t argb;
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            /* w1 is opposite a, w2 opposite b, w0 opposite c. */
            l0 = w1 * T->inv_area; l1 = w2 * T->inv_area; l2 = w0 * T->inv_area;
            z = l0 * a[2] + l1 * b[2] + l2 * c[2];
            {
                float *zp = T->zb ? &T->zb[(size_t)y * NV_ZBUF_W + x] : NULL;
                int shade = (s_gpu.color_mask & 0x01010101u) || s_gpu.alpha_test;
                /* Depth test, then shade, then alpha test, and only then the
                 * depth write: an alpha-tested texel that is cut away must
                 * not leave its depth behind (foliage, fences). */
                if (zp && s_gpu.depth_test && !depth_pass(z, *zp)) {
                    cnt->depth_fail++;
                    continue;
                }
                argb = 0;
                if (shade) {
                    for (k = 0; k < 4; k++)
                        col[k] = l0 * va->d0[k] + l1 * vb->d0[k] + l2 * vc->d0[k];
                    pw = l0 * T->iw[0] + l1 * T->iw[1] + l2 * T->iw[2];
                    if (T->use_rc) {
                        float d1[4], fog[4], t[4][4], out[4], tc[4];
                        int st, j, keep = 1;
                        for (k = 0; k < 4; k++)
                            d1[k] = l0 * va->d1[k] + l1 * vb->d1[k] + l2 * vc->d1[k];
                        fog[0] = T->fogc[0]; fog[1] = T->fogc[1]; fog[2] = T->fogc[2];
                        fog[3] = l0 * T->vfog[0] + l1 * T->vfog[1] + l2 * T->vfog[2];
                        for (st = 0; st < 4 && keep; st++) {
                            if (!((s_gpu.rc.stage_program >> (st * 5)) & 0x1F)) {
                                t[st][0] = t[st][1] = t[st][2] = 0.0f;
                                t[st][3] = 1.0f;
                                continue;
                            }
                            for (j = 0; j < 4; j++)
                                tc[j] = (l0 * T->stc[0][st][j] + l1 * T->stc[1][st][j]
                                       + l2 * T->stc[2][st][j]) / pw;
                            if (aniso) {
                                /* The same coordinate one pixel right and one
                                 * down: the difference is the gradient. */
                                float tcx[4], tcy[4];
                                xf_tc_step(T, st, l0, l1, l2, 0, tcx);
                                xf_tc_step(T, st, l0, l1, l2, 1, tcy);
                                keep = rc_stage_fetch(st, tc, tcx, tcy, t[st]);
                            } else {
                                keep = rc_stage_fetch(st, tc, NULL, NULL, t[st]);
                            }
                        }
                        if (!keep)
                            continue;
                        nv2a_rc_eval(&s_gpu.rc, col, d1, fog,
                                     (const float (*)[4])t, out);
                        if (s_gpu.alpha_test && !alpha_test_pass(out[3]))
                            continue;
                        argb = pack_color(out);
                    } else {
                        argb = pack_color(col);
                        if (T->textured) {
                            uint32_t texel;
                            float tu, tv;
                            tu = (l0 * T->uv[0][0] + l1 * T->uv[1][0] + l2 * T->uv[2][0]) / pw;
                            tv = (l0 * T->uv[0][1] + l1 * T->uv[1][1] + l2 * T->uv[2][1]) / pw;
                            if (bilinear) {
                                uint32_t acc[4] = {0, 0, 0, 0}, n = 1, i;
                                float tap0[2] = {0.0f, 0.0f}, dtap[2] = {0.0f, 0.0f};
                                if (aniso) {
                                    float ux, vx, uy, vy, gx[2], gy[2];
                                    xf_uv_step(T, l0, l1, l2, 0, &ux, &vx);
                                    xf_uv_step(T, l0, l1, l2, 1, &uy, &vy);
                                    gx[0] = ux - tu; gx[1] = vx - tv;
                                    gy[0] = uy - tu; gy[1] = vy - tv;
                                    n = aniso_footprint(gx, gy, (uint32_t)aniso, tap0, dtap);
                                }
                                for (i = 0; i < n; i++)
                                    if (!bilinear_accum(tu + tap0[0] + (float)i * dtap[0],
                                                        tv + tap0[1] + (float)i * dtap[1], acc))
                                        break;
                                if (i == n)
                                    argb = modulate(filter_result(acc, n), col);
                            } else if (sample_texture((uint32_t)(int32_t)floorf(tu),
                                                      (uint32_t)(int32_t)floorf(tv), &texel))
                                argb = modulate(texel, col);
                        }
                        if (s_gpu.alpha_test
                            && !alpha_test_pass((float)(argb >> 24) / 255.0f))
                            continue;
                    }
                }
                if (zp && s_gpu.depth_mask)
                    *zp = z;
                if (s_gpu.zpass_enable)
                    cnt->zpass++;
                if (!(s_gpu.color_mask & 0x01010101u))
                    continue;              /* colour writes off: depth only */
                cnt->pixels++;
            }
            put_pixel(T->mem, T->bpp, x, y, argb);
        }
    }
}

/* Worker threads for big triangles.
 *
 * The rasteriser is a software GPU, and a Burnout 3 frame is dominated by a
 * handful of full-screen passes -- composites, blurs, the menu backdrops --
 * each two triangles of 300,000 pixels through the register combiners. Those
 * split cleanly by row: every pixel reads only its own depth and colour, so
 * interleaved rows on N threads need no locking. Small triangles stay on the
 * executor thread, where waking workers would cost more than it saves.
 * RECOMP_RASTER_THREADS=<n> sets the count (1 = off); the default leaves a
 * few cores for the title and the host.
 * ponytail: s_gpu.pixels/pixel_max in put_pixel are unsynchronised stats and
 * may undercount; nothing depends on them. */
#define NV_RASTER_MAX_THREADS 16
#define NV_RASTER_MT_MIN_PIXELS 8192
#define NV_RASTER_SPIN_US 20

/* Worker k owns the rows y with y % n == k, for every triangle of the run,
 * in submission order. Each pixel therefore sees its triangles in the order
 * the title drew them, whichever thread draws it. */
static void xf_rows_owned(const XfTri *tris, int ntris, int k, int n,
                          XfCount *cnt)
{
    int i;
    for (i = 0; i < ntris; i++) {
        int y0 = tris[i].miny + ((k - tris[i].miny) % n + n) % n;
        xf_rows(&tris[i], y0, n, cnt);
    }
}

/* Workers that actually drew in the most recent batch, and the configured
 * pool size. Diagnostics, and the only way a test can tell a pool that ran
 * from a pool that was merely created -- tests/nv2a_raster_posix_pool. */
static int s_raster_last_workers = 1;
int nv2a_raster_last_workers(void) { return s_raster_last_workers; }

/* The pool runs one job at a time for the executor thread: a function called
 * as fn(k, n, ctx) on every thread, k = 0..n-1, the caller being k = n - 1.
 * Rasterising a run of triangles is one job, shading a batch's vertices
 * another. */
typedef void (*PoolJob)(int k, int n, void *ctx);
/* A job cut into chunks any thread may take: fn(chunk, thread, ctx). */
typedef void (*PoolChunk)(int chunk, int thread, void *ctx);

#if defined(_WIN32)
static struct {
    int n;                                  /* threads incl. the caller */
    HANDLE start[NV_RASTER_MAX_THREADS], done;
    volatile LONG pending;
    PoolJob fn;
    void *ctx;
} s_pool;

static DWORD WINAPI raster_worker(LPVOID arg)
{
    int k = (int)(intptr_t)arg;
    for (;;) {
        WaitForSingleObject(s_pool.start[k], INFINITE);
        s_pool.fn(k, s_pool.n, s_pool.ctx);
        if (InterlockedDecrement(&s_pool.pending) == 0)
            SetEvent(s_pool.done);
    }
    return 0;
}

static int raster_pool_size(void)
{
    static int init;
    if (!init) {
        const char *e = getenv("RECOMP_RASTER_THREADS");
        SYSTEM_INFO si;
        int n, k;
        init = 1;
        GetSystemInfo(&si);
        n = e ? atoi(e) : (int)si.dwNumberOfProcessors - 4;
        if (n < 1) n = 1;
        if (n > NV_RASTER_MAX_THREADS) n = NV_RASTER_MAX_THREADS;
        s_pool.n = n;
        if (n > 1) {
            s_pool.done = CreateEventA(NULL, FALSE, FALSE, NULL);
            for (k = 0; k < n - 1; k++) {
                s_pool.start[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
                CloseHandle(CreateThread(NULL, 0, raster_worker,
                                         (LPVOID)(intptr_t)k, 0, NULL));
            }
        }
    }
    return s_pool.n;
}

/* Run fn on every thread of the pool (size > 1) and wait for all of them. */
static void pool_run(PoolJob fn, void *ctx)
{
    int n = s_pool.n, k;

    s_pool.fn = fn;
    s_pool.ctx = ctx;
    s_pool.pending = n - 1;
    for (k = 0; k < n - 1; k++)
        SetEvent(s_pool.start[k]);
    fn(n - 1, n, ctx);                      /* the caller's share */
    WaitForSingleObject(s_pool.done, INFINITE);
}

/* Chunks on this pool: thread k takes chunks k, k + n, ... (every thread
 * joins every job here, so a static split costs nothing extra). */
static struct { PoolChunk fn; void *ctx; int nchunks; } s_chunks;

static void pool_chunks_static(int k, int n, void *ctx)
{
    int c;
    (void)ctx;
    for (c = k; c < s_chunks.nchunks; c += n)
        s_chunks.fn(c, k, s_chunks.ctx);
}

static void pool_run_chunks(PoolChunk fn, void *ctx, int nchunks)
{
    s_chunks.fn = fn;
    s_chunks.ctx = ctx;
    s_chunks.nchunks = nchunks;
    pool_run(pool_chunks_static, NULL);
}
#else
/* The same pool on pthreads. One mutex and two conditions rather than a
 * per-worker event: a generation counter, bumped under the lock, is what a
 * worker waits on, so a batch that starts and finishes before a worker reaches
 * its wait is still seen -- a plain signal there would be lost and the batch
 * would hang on the join. */
static struct {
    int n;                                  /* threads incl. the caller */
    pthread_mutex_t lock;
    pthread_cond_t start, done;
    unsigned long generation;
    int pending;
    PoolJob fn;
    void *ctx;
    /* A chunked job (pool_run_chunks): workers take chunks while there are
     * any and are not waited for. claim = generation << 32 | next chunk. */
    int chunked, nchunks;
    PoolChunk cfn;
    uint64_t claim;
    int chunks_done;
    int sleepers;                           /* workers parked on `start` */
} s_pool;

/* RECOMP_RASTER_SPIN_US: how long an idle worker watches for the next job
 * before it parks (0: park at once). */
static long s_pool_spin_ns = -1;

static int pool_spin_for_job(unsigned long seen)
{
    struct timespec t0, t;
    int i = 0;

    if (s_pool_spin_ns <= 0)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (__atomic_load_n(&s_pool.generation, __ATOMIC_ACQUIRE) != seen)
            return 1;
#if defined(__aarch64__)
        __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause");
#endif
        if (++i % 64 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t);
            if ((t.tv_sec - t0.tv_sec) * 1000000000L + (t.tv_nsec - t0.tv_nsec)
                    > s_pool_spin_ns)
                return 0;
        }
    }
}

/* Take and run chunks of job `gen` until none is left or the job is over. */
static void pool_take_chunks(uint32_t gen, PoolChunk fn, void *ctx,
                             int nchunks, int thread)
{
    for (;;) {
        uint64_t c = __atomic_load_n(&s_pool.claim, __ATOMIC_ACQUIRE);
        if ((uint32_t)(c >> 32) != gen || (int)(uint32_t)c >= nchunks)
            return;
        if (!__atomic_compare_exchange_n(&s_pool.claim, &c, c + 1, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        fn((int)(uint32_t)c, thread, ctx);
        __atomic_add_fetch(&s_pool.chunks_done, 1, __ATOMIC_RELEASE);
    }
}

static void *raster_worker(void *arg)
{
    int k = (int)(intptr_t)arg;
    unsigned long seen = 0;
    recomp_host_cpus_track(0);          /* its mask, restored after a resume (XBA-50) */
    for (;;) {
        PoolJob fn;
        PoolChunk cfn;
        void *ctx;
        int chunked, nchunks;

        /* The executor hands batches over back to back; a worker that
         * parked after each would be woken for every one, and a wake costs
         * tens of microseconds. So look for the next job a little before
         * parking. */
        if (seen && pool_spin_for_job(seen)) {
            pthread_mutex_lock(&s_pool.lock);
        } else {
            pthread_mutex_lock(&s_pool.lock);
            s_pool.sleepers++;
            while (__atomic_load_n(&s_pool.generation, __ATOMIC_ACQUIRE) == seen)
                pthread_cond_wait(&s_pool.start, &s_pool.lock);
            s_pool.sleepers--;
        }
        seen = s_pool.generation;
        fn = s_pool.fn;
        cfn = s_pool.cfn;
        ctx = s_pool.ctx;
        chunked = s_pool.chunked;
        nchunks = s_pool.nchunks;
        pthread_mutex_unlock(&s_pool.lock);

        if (chunked) {
            /* Nobody waits for this worker: a job it wakes too late for
             * is simply over, and the claim word says so. */
            pool_take_chunks((uint32_t)seen, cfn, ctx, nchunks, k);
            continue;
        }
        fn(k, s_pool.n, ctx);

        pthread_mutex_lock(&s_pool.lock);
        if (--s_pool.pending == 0)
            pthread_cond_signal(&s_pool.done);
        pthread_mutex_unlock(&s_pool.lock);
    }
    return NULL;
}

/* The default pool size, the executor thread included.
 *
 * Desktop: every online core but four, as on Windows -- the title's threads
 * and the host keep the rest.
 *
 * Android: the cores the runtime may run on, less one. recomp_host_cpus_init
 * restricts the process to the fast cluster (an AYN Thor: cores 3-7 of 8),
 * and "online cores - 4" counted the slow ones it can never use -- 4 on the
 * Thor by coincidence, 2 on a 2+6 phone, 6 on a 10-core one. One fast core
 * is left to the guest's main thread, which spins in D3D waiting for the
 * GPU (Fable: sub_00855520), and to audio. */
static int raster_pool_default(void)
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
#if defined(__ANDROID__)
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0 && CPU_COUNT(&set) > 0)
        return CPU_COUNT(&set) - 1;
#endif
    return (int)(cpus > 0 ? cpus : 1) - 4;
}

static int raster_pool_size(void)
{
    static int init;
    if (!init) {
        const char *e = getenv("RECOMP_RASTER_THREADS");
        int n, k;
        init = 1;
        n = e ? atoi(e) : raster_pool_default();
        if (n < 1) n = 1;
        if (n > NV_RASTER_MAX_THREADS) n = NV_RASTER_MAX_THREADS;
        s_pool.n = n;
        {
            const char *sp = getenv("RECOMP_RASTER_SPIN_US");
            s_pool_spin_ns = (sp ? atol(sp) : NV_RASTER_SPIN_US) * 1000L;
        }
        if (n > 1) {
            pthread_mutex_init(&s_pool.lock, NULL);
            pthread_cond_init(&s_pool.start, NULL);
            pthread_cond_init(&s_pool.done, NULL);
            for (k = 0; k < n - 1; k++) {
                pthread_t t;
                if (pthread_create(&t, NULL, raster_worker,
                                   (void *)(intptr_t)k) != 0) {
                    /* Short pool rather than no drawing: the rows this worker
                     * would have owned must still be drawn by someone, so fall
                     * back to the size that did start. */
                    s_pool.n = k + 1;
                    break;
                }
                pthread_detach(t);
            }
        }
    }
    return s_pool.n;
}

/* Run fn on every thread of the pool (size > 1) and wait for all of them. */
static void pool_run(PoolJob fn, void *ctx)
{
    int n = s_pool.n;

    pthread_mutex_lock(&s_pool.lock);
    s_pool.chunked = 0;
    s_pool.fn = fn;
    s_pool.ctx = ctx;
    s_pool.pending = n - 1;
    __atomic_add_fetch(&s_pool.generation, 1, __ATOMIC_RELEASE);
    if (s_pool.sleepers)
        pthread_cond_broadcast(&s_pool.start);
    pthread_mutex_unlock(&s_pool.lock);

    fn(n - 1, n, ctx);                      /* the caller's share */

    pthread_mutex_lock(&s_pool.lock);
    while (s_pool.pending != 0)
        pthread_cond_wait(&s_pool.done, &s_pool.lock);
    pthread_mutex_unlock(&s_pool.lock);
}

/* Run fn on chunks 0..nchunks-1, each exactly once, on whichever threads of
 * the pool are awake to take them, and return when every chunk is done.
 *
 * pool_run waits for every worker to wake, run its share and report; a
 * thousand vertex batches a frame made that wake-and-join the cost (on a
 * loaded machine a 480-vertex batch was slower on six threads than on one).
 * Here the caller starts on the chunks at once and only waits for chunks
 * already taken; a worker that wakes after the last one is claimed finds
 * the job over and goes back to sleep. A job's chunks are claimed by
 * compare-and-swap on generation << 32 | next, so a late worker can never
 * take a chunk of a later job with the earlier job's function. */
static void pool_run_chunks(PoolChunk fn, void *ctx, int nchunks)
{
    uint32_t gen;

    pthread_mutex_lock(&s_pool.lock);
    s_pool.chunked = 1;
    s_pool.cfn = fn;
    s_pool.ctx = ctx;
    s_pool.nchunks = nchunks;
    __atomic_store_n(&s_pool.chunks_done, 0, __ATOMIC_RELAXED);
    gen = (uint32_t)(s_pool.generation + 1);
    __atomic_store_n(&s_pool.claim, (uint64_t)gen << 32, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_pool.generation, 1, __ATOMIC_RELEASE);
    if (s_pool.sleepers)
        pthread_cond_broadcast(&s_pool.start);
    pthread_mutex_unlock(&s_pool.lock);

    pool_take_chunks(gen, fn, ctx, nchunks, s_pool.n - 1);
    /* Only chunks a worker is running are left: short, and that worker is
     * awake, so spin rather than sleep. */
    while (__atomic_load_n(&s_pool.chunks_done, __ATOMIC_ACQUIRE) < nchunks)
        sched_yield();
}
#endif

/* Each thread's counters on a cache line of their own. */
typedef union { XfCount c; char line[64]; } XfCountLine;

typedef struct {
    const XfTri *tris;
    int ntris;
    XfCountLine cnt[NV_RASTER_MAX_THREADS];
} RasterJob;

static void raster_job(int k, int n, void *ctx)
{
    RasterJob *J = (RasterJob *)ctx;
    memset(&J->cnt[k].c, 0, sizeof J->cnt[k].c);
    xf_rows_owned(J->tris, J->ntris, k, n, &J->cnt[k].c);
}

static void xf_rows_parallel(const XfTri *T, int ntris, long px,
                             XfCount *total)
{
    static RasterJob job;                   /* executor thread only */
    int n = raster_pool_size(), k, ran = 1; /* the caller always draws */

    if (n <= 1 || px < NV_RASTER_MT_MIN_PIXELS) {
        s_raster_last_workers = 1;
        for (k = 0; k < ntris; k++)
            xf_rows(&T[k], T[k].miny, 1, total);
        return;
    }
    job.tris = T;
    job.ntris = ntris;
    pool_run(raster_job, &job);
    for (k = 0; k < n; k++) {
        const XfCount *c = &job.cnt[k].c;
        if (k < n - 1 && c->pixels) ran++;
        total->depth_fail += c->depth_fail;
        total->pixels += c->pixels;
        total->zpass += c->zpass;
    }
    s_raster_last_workers = ran;
}

int nv2a_raster_pool_size(void) { return raster_pool_size(); }

/* Triangles waiting for the end of their batch.
 *
 * A level is thousands of small triangles, each far below the size worth
 * waking the pool for, so they all drew on the executor thread while the
 * workers slept: Fallout: Brotherhood of Steel's first level ran at 5 fps
 * with the executor at 100% and every worker idle. Queuing a batch's
 * triangles and handing the whole run over at once costs one wake per batch.
 * GPU state cannot change inside a batch, so drawing later reads the same
 * state. The vertices are copied: near-plane clipping passes stack locals.
 * ponytail: a fixed-size queue flushed when full; 2048 covers a typical
 * batch, and a bigger one just splits into several runs. */
#define XF_QUEUE_MAX 2048
typedef struct { XfTri t; Nv2aVshOutput v[3]; } XfQueued;
static XfQueued *s_xfq;
static XfTri s_xfq_tris[XF_QUEUE_MAX];
static int s_xfq_n;
static long s_xfq_px;

static void xf_queue_flush(void)
{
    XfCount cnt = {0, 0, 0};
    int i;

    if (!s_xfq_n)
        return;
    for (i = 0; i < s_xfq_n; i++)
        s_xfq_tris[i] = s_xfq[i].t;
    xf_rows_parallel(s_xfq_tris, s_xfq_n, s_xfq_px, &cnt);
    s_gpu.xf_depth_fail += cnt.depth_fail;
    s_gpu.xf_pixels += cnt.pixels;
    s_gpu.zpass_count += cnt.zpass;
    s_xfq_n = 0;
    s_xfq_px = 0;
}

static void xf_queue_push(const XfTri *T)
{
    XfQueued *q;

    if (!s_xfq && !(s_xfq = (XfQueued *)malloc(XF_QUEUE_MAX * sizeof *s_xfq))) {
        XfCount cnt = {0, 0, 0};
        xf_rows_parallel(T, 1, 0, &cnt);
        return;
    }
    if (s_xfq_n == XF_QUEUE_MAX)
        xf_queue_flush();
    q = &s_xfq[s_xfq_n++];
    q->t = *T;
    q->v[0] = *T->va; q->v[1] = *T->vb; q->v[2] = *T->vc;
    q->t.va = &q->v[0]; q->t.vb = &q->v[1]; q->t.vc = &q->v[2];
    q->t.a = q->v[0].pos; q->t.b = q->v[1].pos; q->t.c = q->v[2].pos;
    s_xfq_px += (long)(T->maxx - T->minx) * (T->maxy - T->miny);
}

static void raster_xf_triangle(const Nv2aVshOutput *va, const Nv2aVshOutput *vb,
                               const Nv2aVshOutput *vc)
{
    XfTri T;
    XfCount cnt = {0, 0, 0};
    const Nv2aVshOutput *v[3];
    const float *a, *b, *c;
    float area, su = 1.0f, sv = 1.0f;
    int k;
    static int no_rc = -1;

    if (no_rc < 0)
        no_rc = getenv("RECOMP_NO_COMBINERS") != NULL;
    memset(&T, 0, sizeof T);
    T.mem = (uint8_t *)xbox_GetMemoryOffset();
    T.bpp = surface_bpp();
    T.textured = s_gpu.texs[0].valid && !s_xf_no_tex;
    T.use_rc = s_gpu.rc_seen && !no_rc;
    T.va = va; T.vb = vb; T.vc = vc;

    v[0] = va; v[1] = vb; v[2] = vc;
    a = va->pos; b = vb->pos; c = vc->pos;
    T.a = a; T.b = b; T.c = c;
    if (T.bpp != 4 && T.bpp != 2)
        return;
    if (a[3] <= 0.0f || b[3] <= 0.0f || c[3] <= 0.0f) {
        s_gpu.tris_behind++;
        return;
    }
    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f || area != area) {
        s_gpu.xf_degenerate++;
        return;
    }
    if (backend_draws()) {                     /* a back end rasterises */
        Nv2aVertex *o = backend_slot();
        if (o) {
            backend_from_xf(va, &o[0]);
            backend_from_xf(vb, &o[1]);
            backend_from_xf(vc, &o[2]);
            s_bcount += 3;
        }
        return;
    }
    for (k = 0; k < 3; k++) {
        if (!s_gpu.xf_seeded)
            s_gpu.xf_min[k] = s_gpu.xf_max[k] = a[k];
        if (a[k] < s_gpu.xf_min[k]) s_gpu.xf_min[k] = a[k];
        if (a[k] > s_gpu.xf_max[k]) s_gpu.xf_max[k] = a[k];
    }
    s_gpu.xf_seeded = 1;
    if (!surface_begin_batch(T.mem))
        return;
    if (s_gpu.depth_test || s_gpu.depth_mask)
        T.zb = zbuf_current(1);
    if (tex_size_from_format(s_gpu.texs[0].color)) {
        su = (float)s_gpu.texs[0].width;
        sv = (float)s_gpu.texs[0].height;
    }
    for (k = 0; k < 3; k++) {
        T.iw[k] = 1.0f / v[k]->pos[3];
        T.uv[k][0] = v[k]->tex[0][0] * su * T.iw[k];
        T.uv[k][1] = v[k]->tex[0][1] * sv * T.iw[k];
    }
    if (T.use_rc) {
        int st, j;
        for (k = 0; k < 3; k++) {
            for (st = 0; st < 4; st++)
                for (j = 0; j < 4; j++)
                    T.stc[k][st][j] = v[k]->tex[st][j] * T.iw[k];
            T.vfog[k] = fog_factor(v[k]->fog[0]);
        }
        /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
        nv2a_rc_unpack(s_gpu.fog_color, T.fogc);
        { float r = T.fogc[2]; T.fogc[2] = T.fogc[0]; T.fogc[0] = r; }
    }

    T.minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    T.maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    T.miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    T.maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));
    if (T.minx < (int)s_gpu.clip_x) T.minx = (int)s_gpu.clip_x;
    if (T.miny < (int)s_gpu.clip_y) T.miny = (int)s_gpu.clip_y;
    if (T.maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) T.maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (T.maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) T.maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (T.maxx > NV_ZBUF_W) T.maxx = NV_ZBUF_W;
    if (T.maxy > NV_ZBUF_H) T.maxy = NV_ZBUF_H;
    if (T.minx >= T.maxx || T.miny >= T.maxy) {
        s_gpu.tris_skipped_offscreen++;
        s_gpu.xf_offscreen++;
        return;
    }
    s_gpu.xf_drawn++;
    T.inv_area = 1.0f / area;
    /* The edge functions are linear, so a barycentric moves by a constant per
     * pixel step. l0 = w1/area, l1 = w2/area, l2 = w0/area as in xf_rows. */
    T.dl[0][0] = -(c[1] - b[1]) * T.inv_area;  T.dl[0][1] = (c[0] - b[0]) * T.inv_area;
    T.dl[1][0] = -(a[1] - c[1]) * T.inv_area;  T.dl[1][1] = (a[0] - c[0]) * T.inv_area;
    T.dl[2][0] = -(b[1] - a[1]) * T.inv_area;  T.dl[2][1] = (b[0] - a[0]) * T.inv_area;

    xf_queue_push(&T);
    (void)cnt;
    s_gpu.tris_drawn++;
    note_drawn();
}


/* Near-plane clipping.
 *
 * A triangle with a vertex behind the eye (w <= 0) used to be dropped whole.
 * That is invisible for distant geometry and ruinous close up: in a chase
 * view the road under the camera is exactly the set of triangles that reach
 * behind it, so the bottom of the frame showed a hole -- a quarter of every
 * race frame's triangles went this way.
 *
 * Clipping has to happen in clip space, but the D3D epilogue has already
 * divided x, y and z by w and applied the viewport, leaving w alone. Each
 * vertex is taken to (x*w, y*w, z*w, w): screen = clip / w * scale + offset
 * makes that an affine image of clip space, so clipping and interpolating
 * there is exact without knowing the scale. That matters: the viewport is
 * the title's own business -- c[58]/c[59] for most, but SSX 3's programs
 * keep it elsewhere and c[58] reads 0, which turned every clipped triangle
 * into a point. Everything else the vertex carries -- colours, fog, texture
 * coordinates -- is linear in clip space and is interpolated directly.
 *
 * The polygon is clipped against w = NV_CLIP_W and then the near plane
 * z = 0 (Sutherland-Hodgman), divided back by w, and fanned. The near plane
 * is screen z = SET_CLIP_MIN (s_xf_o[2], 0 for D3D); a vertex on it is
 * inside. Clipping at
 * w alone let geometry between the eye and the near plane through at huge
 * screen coordinates: torn streaks at the frame's edges. RECOMP_CLIP_NEAR_Z=0
 * clips at w only.
 * ponytail: near planes only; x/y/far fall to the rasteriser's bounds
 * check, which is correct, just slower for huge off-screen triangles. */
#define NV_CLIP_W 1e-3f

/* [2]: the near plane in window z (SET_CLIP_MIN, see above); [0], [1]: the
 * c[59] viewport offset, scaled like the vertices. Set by xf_set_viewport(). */
static float s_xf_o[3];

static void xf_lerp(const Nv2aVshOutput *a, const Nv2aVshOutput *b, float t,
                    Nv2aVshOutput *out)
{
    const float *pa = (const float *)a, *pb = (const float *)b;
    float *po = (float *)out;
    size_t i, n = sizeof(Nv2aVshOutput) / sizeof(float);
    for (i = 0; i < n; i++)
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

/* Signed distance of a vertex in (x*w, y*w, z*w, w) to clip plane `pl`:
 * 0 is w = NV_CLIP_W, 1 the near plane. */
static float xf_clip_dist(const Nv2aVshOutput *v, int pl)
{
    return pl == 0 ? v->pos[3] - NV_CLIP_W : v->pos[2] - s_xf_o[2] * v->pos[3];
}

/* Sutherland-Hodgman: clip polygon in[0..n) against plane pl into out. */
static int xf_clip_poly(const Nv2aVshOutput *in, int n, int pl,
                        Nv2aVshOutput *out)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const Nv2aVshOutput *p = &in[i], *q = &in[(i + 1) % n];
        float dp = xf_clip_dist(p, pl), dq = xf_clip_dist(q, pl);
        if (dp >= 0.0f)
            out[m++] = *p;
        if ((dp >= 0.0f) != (dq >= 0.0f)) {
            Nv2aVshOutput *v = &out[m++];
            xf_lerp(p, q, dp / (dp - dq), v);
            /* Exactly on the plane: at w in the 1e4s the lerp's rounding is
             * bigger than NV_CLIP_W, and a w <= 0 divides to inf/NaN. */
            if (pl == 0)
                v->pos[3] = NV_CLIP_W;
            else
                v->pos[2] = s_xf_o[2] * v->pos[3];
        }
    }
    return m;
}

static void raster_xf_clipped(const Nv2aVshOutput *a, const Nv2aVshOutput *b,
                              const Nv2aVshOutput *c)
{
    static int near_z = -1;
    const Nv2aVshOutput *in[3];
    Nv2aVshOutput p0[8], p1[8];
    int n = 3, i, j, pl, planes, inside = 1;

    if (near_z < 0) {
        const char *e = getenv("RECOMP_CLIP_NEAR_Z");
        near_z = !(e && e[0] == '0');
    }
    planes = near_z ? 2 : 1;
    in[0] = a; in[1] = b; in[2] = c;
    /* The plane distances need only the position (xf_clip_dist on the
     * w-multiplied copy below); the whole vertex is copied only for a
     * triangle that is actually clipped. */
    for (i = 0; i < 3; i++) {
        Nv2aVshOutput q;
        for (j = 0; j < 3; j++)
            q.pos[j] = in[i]->pos[j] * in[i]->pos[3];
        q.pos[3] = in[i]->pos[3];
        for (pl = 0; pl < planes; pl++) {
            float d = xf_clip_dist(&q, pl);
            if (d != d || d - d != 0.0f)
                return;                       /* not finite: no position at all */
            if (d < 0.0f)
                inside = 0;
        }
    }
    if (inside) {
        raster_xf_triangle(a, b, c);
        return;
    }
    for (i = 0; i < 3; i++) {
        p0[i] = *in[i];
        for (j = 0; j < 3; j++)
            p0[i].pos[j] = in[i]->pos[j] * in[i]->pos[3];
    }
    n = xf_clip_poly(p0, n, 0, p1);
    if (planes > 1 && n)
        n = xf_clip_poly(p1, n, 1, p0);
    else
        memcpy(p0, p1, sizeof p1);
    if (n < 3) {
        s_gpu.tris_behind++;                  /* wholly clipped away */
        return;
    }
    /* Back to screen space: divide by the (now positive) w. */
    for (i = 0; i < n; i++)
        for (j = 0; j < 3; j++)
            p0[i].pos[j] /= p0[i].pos[3];
    for (i = 1; i + 1 < n; i++)
        raster_xf_triangle(&p0[0], &p0[i], &p0[i + 1]);
}

/* About 7 MB, so allocated by the first batch that transforms rather than
 * carried in every build that never turns the executor on. */
static Nv2aVshOutput *s_xf;

/* Does this batch run the title's vertex program? RECOMP_NO_VSH turns the
 * interpreter off (the batch then falls to the screen-space heuristics). */
static int batch_uses_program(void)
{
    static int no_vsh = -1;

    if (no_vsh < 0)
        no_vsh = getenv("RECOMP_NO_VSH") != NULL;
    return (s_gpu.xf_mode & 3) == 2 && !no_vsh;
}

/* One vertex through the title's program. `prepared`: the batch was set up
 * by nv2a_vsh_prepare_parallel, and this may run on any pool thread; it then
 * reads only s_gpu and guest memory, writes only *out. */
static int program_vertex(uint32_t index, Nv2aVshOutput *out, int prepared)
{
    float in[NV2A_VSH_INPUTS][4];
    uint32_t a;

    for (a = 0; a < NV2A_VSH_INPUTS; a++) {
        /* An attribute with no array (size 0) reads its current value, the
         * one the SET_VERTEX_DATA* methods left -- as the hardware does --
         * not a fixed 0,0,0,1. SSX 3's sprite program passes its 4x4
         * transforms in v8..v15 this way, set per batch with
         * SET_VERTEX_DATA4F; reading 0,0,0,1 there sent every vertex of
         * its front end to the viewport offset. imm_attr starts at 0,0,0,1,
         * so an attribute nobody set still reads the old default. */
        if (!s_gpu.attr[a].size && a < NV_VERTEX_ATTRS)
            memcpy(in[a], s_gpu.imm_attr[a], sizeof in[a]);
        else
            fetch_attr(&s_gpu.attr[a], index, in[a]);   /* absent: 0,0,0,1 */
    }
    if (!(prepared ? nv2a_vsh_run_prepared((const float (*)[4])in, out)
                   : nv2a_vsh_run((const float (*)[4])in, out)))
        return 0;
    /* oPos.w == 0 comes from a program that never divided by w: x, y and z
     * are already surface units. The hardware draws such a vertex (xemu
     * clamps w away from zero, in front of the eye), but raster_xf_clipped
     * would count it behind the eye. w = 1 draws it at the same place and
     * keeps 1/w finite. -0 stays behind the eye and is clipped, as on xemu. */
    if (out->pos[3] == 0.0f && !signbit(out->pos[3]))
        out->pos[3] = 1.0f;
    /* The program's viewport is in logical pixels; the surface may be larger
     * (anti-aliasing). */
    out->pos[0] *= s_gpu.aa_sx;
    out->pos[1] *= s_gpu.aa_sy;
    return 1;
}

static int transform_vertex(uint32_t index, Nv2aVshOutput *out)
{
    if (!batch_uses_program())
        return ffp_vertex(index, out);
    return program_vertex(index, out, 0);
}

/* Shading a batch on the raster pool.
 *
 * In a 3D scene the vertex program is the executor's biggest single cost
 * (Fable in-world: 41 % of the executor thread, ~455,000 vertices a frame),
 * and every vertex is independent of the others. So a big enough batch is cut
 * into chunks of its index list (pool_run_chunks), each shaded by whichever
 * pool thread takes it into its own slots of s_xf: the outputs land where the
 * serial loop puts them and come from the same code, so what is drawn is bit
 * for bit the same.
 * Small batches stay on the executor thread, where waking the pool costs more
 * than it saves. RECOMP_VSH_POOL=0 shades every batch serially. */
#define NV_VSH_MT_MIN_VERTICES 192

typedef struct {
    uint32_t n, chunk;                      /* vertices; vertices per chunk */
    volatile int failed;                    /* only ever set to 1 */
    const uint32_t *idx;                    /* vertex i is element idx[i] ... */
    const uint32_t *at;                     /* ... into s_xf[at[i]], or s_xf[i] */
    int ffp;                                /* fixed function (ffp_batch_vertex) */
} VshJob;

static int s_vsh_pool = -1;                 /* -1: read RECOMP_VSH_POOL */
static int s_vsh_last_workers = 1;
void nv2a_vsh_pool_enable(int on) { s_vsh_pool = on ? 1 : 0; }
int nv2a_vsh_last_workers(void) { return s_vsh_last_workers; }

static void vsh_chunk(int c, int thread, void *ctx)
{
    VshJob *J = (VshJob *)ctx;
    uint32_t i = (uint32_t)c * J->chunk;
    uint32_t end = i + J->chunk < J->n ? i + J->chunk : J->n;

    (void)thread;
    for (; i < end; i++) {
        Nv2aVshOutput *o = &s_xf[J->at ? J->at[i] : i];
        if (J->ffp ? !ffp_batch_vertex(J->idx[i], o)
                   : !program_vertex(J->idx[i], o, 1)) {
            J->failed = 1;                  /* the batch is dropped either way */
            return;
        }
    }
}

/* Fixed-function batches transform each unique index once: an indexed mesh
 * names most of its vertices about six times, and ffp_batch_vertex depends on
 * nothing but the index. s_ffp_u[0..u) are the unique indices, s_ffp_at[k]
 * the position in the batch where index s_ffp_u[k] first appears, and
 * s_ffp_src[i] that first position for batch vertex i. */
static uint32_t *s_ffp_u, *s_ffp_at, *s_ffp_src;

/* Fixed-function batches on the pool: opt-in, RECOMP_FFP_POOL=1. Once
 * deduplicated a batch is cheap (KOTOR: a few hundred unique vertices), and
 * a worker that claims a chunk and is then descheduled keeps the executor
 * yielding far longer than the chunk would have taken it: KOTOR close-ups
 * measured 8.0 ms/frame back end with the pool, 7.2 without (Mac, load 6). */
#define NV_FFP_MT_MIN_VERTICES 192
static int s_ffp_pool = -1;
void nv2a_ffp_pool_enable(int on) { s_ffp_pool = on ? 1 : 0; }

#define VMAP_BITS 17                    /* twice NV_MAX_INDICES */
#define VMAP_SIZE (1u << VMAP_BITS)

static struct {
    float    *attrs;                    /* NV_MAX_INDICES x 16 x vec4 */
    uint16_t *tris;                     /* 3 x NV_MAX_INDICES */
    uint16_t *slot;                     /* NV_MAX_INDICES: batch vertex -> attrs row */
    uint32_t *key, *gen;                /* VMAP_SIZE: index -> row, by generation */
    uint16_t *row;
    uint32_t  cur;
} s_pv;

static int gpu_vsh_enabled(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_VSH");
        on = !(e && e[0] == '0');
    }
    return on;
}

static int pv_alloc(void)
{
    if (s_pv.attrs)
        return 1;
    s_pv.attrs = (float *)malloc((size_t)NV_MAX_INDICES * 16 * 4 * sizeof(float));
    s_pv.tris = (uint16_t *)malloc((size_t)NV_MAX_INDICES * 3 * sizeof(uint16_t));
    s_pv.slot = (uint16_t *)malloc((size_t)NV_MAX_INDICES * sizeof(uint16_t));
    s_pv.key = (uint32_t *)malloc(VMAP_SIZE * sizeof(uint32_t));
    s_pv.gen = (uint32_t *)calloc(VMAP_SIZE, sizeof(uint32_t));
    s_pv.row = (uint16_t *)malloc(VMAP_SIZE * sizeof(uint16_t));
    if (!s_pv.attrs || !s_pv.tris || !s_pv.slot || !s_pv.key || !s_pv.gen || !s_pv.row) {
        free(s_pv.attrs); free(s_pv.tris); free(s_pv.slot);
        free(s_pv.key); free(s_pv.gen); free(s_pv.row);
        memset(&s_pv, 0, sizeof s_pv);
        return 0;
    }
    return 1;
}

/* The unique indices of the batch (s_ffp_u, s_ffp_at, s_ffp_src); their
 * count, or 0 when every index is already unique or there is no memory. */
static uint32_t ffp_unique(uint32_t n)
{
    uint32_t i, u = 0;

    for (i = 1; i < n && s_gpu.idx[i] == s_gpu.idx[0] + i; i++)
        ;
    if (i == n || !pv_alloc())
        return 0;
    if (!s_ffp_u) {
        s_ffp_u = (uint32_t *)malloc((size_t)NV_MAX_INDICES * sizeof(uint32_t));
        s_ffp_at = (uint32_t *)malloc((size_t)NV_MAX_INDICES * sizeof(uint32_t));
        s_ffp_src = (uint32_t *)malloc((size_t)NV_MAX_INDICES * sizeof(uint32_t));
        if (!s_ffp_u || !s_ffp_at || !s_ffp_src) {
            free(s_ffp_u); free(s_ffp_at); free(s_ffp_src);
            s_ffp_u = s_ffp_at = s_ffp_src = NULL;
            return 0;
        }
    }
    if (++s_pv.cur == 0) {
        memset(s_pv.gen, 0, VMAP_SIZE * sizeof(uint32_t));
        s_pv.cur = 1;
    }
    for (i = 0; i < n; i++) {
        uint32_t ix = s_gpu.idx[i];
        uint32_t h = (ix * 2654435761u) >> (32 - VMAP_BITS);
        while (s_pv.gen[h] == s_pv.cur && s_pv.key[h] != ix)
            h = (h + 1) & (VMAP_SIZE - 1);
        if (s_pv.gen[h] != s_pv.cur) {
            s_pv.gen[h] = s_pv.cur;
            s_pv.key[h] = ix;
            s_pv.row[h] = (uint16_t)u;
            s_ffp_u[u] = ix;
            s_ffp_at[u] = i;
            u++;
        }
        s_ffp_src[i] = s_ffp_at[s_pv.row[h]];
    }
    return u;
}

/* s_xf[0..n) for the batch's index list; 0 when there is no program to run. */
static int transform_batch(uint32_t n)
{
    uint32_t i, cnt = n;
    int ffp = !batch_uses_program() && ffp_batch_on();
    const uint32_t *idx = s_gpu.idx, *at = NULL;

    if (s_vsh_pool < 0) {
        const char *e = getenv("RECOMP_VSH_POOL");
        s_vsh_pool = !(e && e[0] == '0');
    }
    if (ffp) {
        uint32_t u = ffp_unique(n);

        ffp_batch_begin();
        if (u) {
            idx = s_ffp_u;
            at = s_ffp_at;
            cnt = u;
        }
    }
    if (s_ffp_pool < 0) {
        const char *e = getenv("RECOMP_FFP_POOL");
        s_ffp_pool = e ? atoi(e) : 0;
    }
    if (s_vsh_pool && cnt >= NV_VSH_MT_MIN_VERTICES && raster_pool_size() > 1
        && (ffp ? s_ffp_pool && cnt >= NV_FFP_MT_MIN_VERTICES
                : batch_uses_program() && nv2a_vsh_prepare_parallel())) {
        static VshJob job;                    /* executor thread only */
        int nt = raster_pool_size();
        /* Four chunks a thread, so threads that wake late still get a
         * share, but no chunk under 64 vertices. */
        uint32_t chunk = cnt / (uint32_t)(nt * 4);

        if (chunk < 64)
            chunk = 64;
        job.n = cnt;
        job.chunk = chunk;
        job.failed = 0;
        job.idx = idx;
        job.at = at;
        job.ffp = ffp;
        pool_run_chunks(vsh_chunk, &job, (int)((cnt + chunk - 1) / chunk));
        s_vsh_last_workers = nt;
        if (job.failed)
            return 0;
    } else {
        s_vsh_last_workers = 1;
        for (i = 0; i < cnt; i++) {
            Nv2aVshOutput *o = &s_xf[at ? at[i] : i];
            if (ffp ? !ffp_batch_vertex(idx[i], o) : !transform_vertex(idx[i], o))
                return 0;
        }
    }
    /* Repeated indices take the first occurrence's result. */
    if (at)
        for (i = 0; i < n; i++)
            if (s_ffp_src[i] != i)
                s_xf[i] = s_xf[s_ffp_src[i]];
    return 1;
}

/* The near plane is SET_CLIP_MIN (0x0394), the window z the hardware clips
 * below, which D3D sets from the viewport's MinZ. It used to be c[59].z, on
 * the assumption that c[59] holds D3D's viewport offset; Fable keeps other
 * data in c[58]/c[59] (c[59].z = 1, 0.99, 15.87 ...), so every z = 0 quad
 * -- all its text and the whole bloom chain -- was clipped away. */
static void xf_set_viewport(void)
{
    const float *of = nv2a_vsh_constant(59);

    s_xf_o[0] = of[0] * s_gpu.aa_sx;
    s_xf_o[1] = of[1] * s_gpu.aa_sy;
    s_xf_o[2] = reg_f(0x0394);                    /* SET_CLIP_MIN */
}

static void raster_batch_program(void)
{
    uint32_t i, n = s_gpu.idx_count;

    if (!s_xf && !(s_xf = (Nv2aVshOutput *)calloc(NV_MAX_INDICES, sizeof *s_xf)))
        return;
    if (!transform_batch(n))
        return;                                        /* no program loaded */
    xf_set_viewport();
    {
        const VertexAttr *tc = texcoord_attr();
        s_xf_no_tex = !batch_uses_program() && !(tc->offset && tc->stride);
    }
    if (batch_uses_program()) {
        s_gpu.batches_program++;
        s_gpu.verts_program += n;
    } else {
        static int ftrace = -1;

        s_gpu.batches_ffp++;
        /* RECOMP_FFP_TRACE: the matrix and the first vertices of the first few
         * fixed-function batches, raw and transformed -- the one view that
         * tells a wrong matrix layout from wrong vertex data. */
        if (ftrace < 0)
            ftrace = getenv("RECOMP_FFP_TRACE") != NULL;
        if (ftrace && s_gpu.batches_ffp <= 4) {
            const float *m = s_gpu.composite;
            float in[4];

            fprintf(stderr, "  [FFP] batch %u prim %u n %u mode 0x%X"
                            " clip %ux%u+%u+%u aa %gx%g\n"
                            "  [FFP]   M = %g %g %g %g | %g %g %g %g |"
                            " %g %g %g %g | %g %g %g %g\n",
                    s_gpu.batches_ffp, s_gpu.prim, n, s_gpu.xf_mode,
                    s_gpu.clip_w, s_gpu.clip_h, s_gpu.clip_x, s_gpu.clip_y,
                    s_gpu.aa_sx, s_gpu.aa_sy,
                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                    m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            for (i = 0; i < n && i < 4; i++) {
                fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], in);
                fprintf(stderr, "  [FFP]   v%u (%g %g %g %g) -> (%g %g %g, w %g)\n",
                        s_gpu.idx[i], in[0], in[1], in[2], in[3],
                        s_xf[i].pos[0], s_xf[i].pos[1], s_xf[i].pos[2],
                        s_xf[i].pos[3]);
            }
        }
    }
    if (s_gpu.blend_enable) {
        uint32_t pair = s_gpu.blend_sfactor << 16 | (s_gpu.blend_dfactor & 0xFFFF);
        int j;
        for (j = 0; j < s_gpu.blend_npairs && s_gpu.blend_pairs[j] != pair; j++)
            ;
        if (j == s_gpu.blend_npairs && j < 16)
            s_gpu.blend_pairs[s_gpu.blend_npairs++] = pair;
    }
    {
        /* RECOMP_VSH_TRACE=<n>: print n program batches -- attribute setup,
         * raw inputs, key constants, and what came out. */
        static int left = -1;
        if (left < 0) {
            const char *t = getenv("RECOMP_VSH_TRACE");
            left = t ? atoi(t) : 0;
        }
        /* Spread out: one every 20000 program batches, so a run that spends
         * its first half in menus still traces the 3D scene. */
        if (left > 0 && n >= 3 && s_gpu.batches_program % 20000 == 0) {
            float in[4];
            left--;
            fprintf(stderr, "[VTRACE] prim %u n %u idx %u %u %u\n", s_gpu.prim,
                    n, s_gpu.idx[0], s_gpu.idx[1], s_gpu.idx[2]);
            for (i = 0; i < NV_VERTEX_ATTRS; i++) {
                const VertexAttr *at = &s_gpu.attr[i];
                if (!at->size)
                    continue;
                fetch_attr(at, s_gpu.idx[0], in);
                fprintf(stderr, "[VTRACE]   v%u off %08X type %u size %u"
                        " stride %u = %g %g %g %g\n", i, at->offset, at->type,
                        at->size, at->stride, in[0], in[1], in[2], in[3]);
            }
            {
                static const uint32_t cs[] = {58, 59, 96, 97, 112, 113, 114, 115};
                for (i = 0; i < 8; i++) {
                    const float *c = nv2a_vsh_constant(cs[i]);
                    fprintf(stderr, "[VTRACE]   c[%u] %g %g %g %g%c", cs[i],
                            c[0], c[1], c[2], c[3], 10);
                }
            }
            for (i = 0; i < 3; i++)
                fprintf(stderr, "[VTRACE]   out%u pos %g %g %g %g\n", i,
                        s_xf[i].pos[0], s_xf[i].pos[1], s_xf[i].pos[2],
                        s_xf[i].pos[3]);
        }
    }
    for (i = 0; i < NV2A_VSH_INPUTS; i++) {
        const VertexAttr *at = &s_gpu.attr[i];
        uint32_t f = at->type | (at->size << 4) | (i << 8), j;
        if (!at->size || !at->stride)
            continue;
        for (j = 0; j < (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_fmt[j] != f; j++)
            ;
        if (j == (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_nfmt < 16)
            s_gpu.xf_fmt[s_gpu.xf_nfmt++] = f;
    }
    if (s_gpu.texs[0].valid)
        note_texture_use();

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if ((i & 1) && strip_alt_winding())
                raster_xf_clipped(&s_xf[i+1], &s_xf[i], &s_xf[i+2]);
            else
                raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++)
            raster_xf_clipped(&s_xf[0], &s_xf[i], &s_xf[i+1]);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+2], &s_xf[i+3]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+3]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+3], &s_xf[i+2]);
        }
        break;
    default:
        break;
    }
    /* The batch's triangles, all at once (see xf_queue_push). */
    xf_queue_flush();
    if (backend_draws())
        backend_flush(!s_xf_no_tex, 0);
}

/* ---- vertex programs on the back end (NV2A_BACKEND_CAP_PROGRAM) ---------
 *
 * Instead of running the program here (transform_vertex), clipping and
 * expanding triangles, hand a back end that runs vertex programs itself the
 * batch as it came: each unique vertex's attributes once (what fetch_attr
 * reads, only the inputs the program reads), an index list in the CPU path's
 * triangle order, the program and the constants. RECOMP_GPU_VSH=0 keeps the
 * CPU transform for every batch. Programs that write c[] with CXT_WRITE on
 * change the constants vertex by vertex and stay on the CPU. */

/* RECOMP_GPU_VBO (NV2A_BACKEND_CAP_RAW_VERTEX): can the batch go to the back
 * end with its vertex arrays as stored? Every streamed input in a format
 * fetch_attr reads, the batch's index range at most 65536 elements and
 * inside guest memory for every stream. *lo, *hi: the smallest and largest
 * index. Inline arrays live in the pushbuffer copy, not guest memory: no. */
static Nv2aRawAttr s_raw[NV2A_VSH_INPUTS];

static int raw_batch_ok(uint32_t streams, uint32_t n, uint32_t *lo, uint32_t *hi)
{
    uint32_t a, i, mn = 0xFFFFFFFFu, mx = 0;

    if (s_gpu.inline_active || !n
        || !(s_backend->caps & NV2A_BACKEND_CAP_RAW_VERTEX))
        return 0;
    for (a = 0; a < NV2A_VSH_INPUTS; a++) {
        const VertexAttr *at = &s_gpu.attr[a];
        if (!(streams & (1u << a)))
            continue;
        if (!at->offset || !at->size || at->size > 4 || at->type == 3 || at->type > 6)
            return 0;
    }
    for (i = 0; i < n; i++) {
        uint32_t ix = s_gpu.idx[i];
        if (ix < mn) mn = ix;
        if (ix > mx) mx = ix;
    }
    if (mx - mn >= 65536u)
        return 0;
    for (a = 0; a < NV2A_VSH_INPUTS; a++) {
        const VertexAttr *at = &s_gpu.attr[a];
        if (!(streams & (1u << a)))
            continue;
        if (!guest_span_ok((uint64_t)at->offset + (uint64_t)mn * at->stride,
                           (mx - mn) * at->stride + attr_elem_bytes(at)))
            return 0;
        s_raw[a].offset = at->offset;
        s_raw[a].type = at->type;
        s_raw[a].size = at->size;
        s_raw[a].stride = at->stride;
    }
    *lo = mn;
    *hi = mx;
    return 1;
}

static int raster_batch_backend_program(void)
{
    Nv2aProgramBatch pb;
    Nv2aSurface surf;
    Nv2aRenderState rs;
    Nv2aPixelState ps;
    Nv2aTexture tex;
    Nv2aBatch batch;
    static float imm[NV2A_VSH_INPUTS][4];
    uint32_t inputs, streams = 0, nstream = 0, n = s_gpu.idx_count, rows = 0;
    uint32_t i, a, t = 0, lo = 0, hi = 0;
    static int no_rc = -1;
    int raw;

    if (!s_backend || !s_backend->draw_program
        || (s_backend->caps & (NV2A_BACKEND_CAP_PROGRAM | NV2A_BACKEND_CAP_PIXEL))
               != (NV2A_BACKEND_CAP_PROGRAM | NV2A_BACKEND_CAP_PIXEL)
        || !gpu_vsh_enabled())
        return 0;
    memset(&pb, 0, sizeof pb);
    pb.ins_count = nv2a_vsh_program(&pb.ins, &pb.hash, &inputs);
    if (!pb.ins_count || nv2a_vsh_program_writes_constants() || !pv_alloc())
        return 0;

    /* What each input reads (transform_vertex): an array, else the current
     * value; an array fetch_attr cannot read gives 0,0,0,1 per vertex. */
    for (a = 0; a < NV2A_VSH_INPUTS; a++) {
        const VertexAttr *at = &s_gpu.attr[a];
        imm[a][0] = imm[a][1] = imm[a][2] = 0.0f;
        imm[a][3] = 1.0f;
        if (!(inputs & (1u << a)))
            continue;
        if (!at->size)
            memcpy(imm[a], s_gpu.imm_attr[a], sizeof imm[a]);
        else if (at->stride) {
            streams |= 1u << a;
            nstream++;
        }
    }

    /* RECOMP_GPU_VBO: no gather, the back end reads the arrays itself;
     * vertex i is element lo + i. */
    raw = raw_batch_ok(streams, n, &lo, &hi);
retry:
    rows = 0;
    t = 0;
    if (raw) {
        for (i = 0; i < n; i++)
            s_pv.slot[i] = (uint16_t)(s_gpu.idx[i] - lo);
        rows = hi - lo + 1;
        goto gathered;
    }

    /* One row per unique index. DRAW_ARRAYS and inline arrays count up from
     * their first index: no lookup needed. */
    for (i = 1; i < n && s_gpu.idx[i] == s_gpu.idx[0] + i; i++)
        ;
    if (i == n) {
        float *o = s_pv.attrs;
        for (i = 0; i < n; i++) {
            for (a = 0; a < NV2A_VSH_INPUTS; a++)
                if (streams & (1u << a)) {
                    fetch_attr(&s_gpu.attr[a], s_gpu.idx[i], o);
                    o += 4;
                }
            s_pv.slot[i] = (uint16_t)i;
        }
        rows = n;
        goto gathered;
    }
    if (++s_pv.cur == 0) {
        memset(s_pv.gen, 0, VMAP_SIZE * sizeof(uint32_t));
        s_pv.cur = 1;
    }
    for (i = 0; i < n; i++) {
        uint32_t ix = s_gpu.idx[i];
        uint32_t h = (ix * 2654435761u) >> (32 - VMAP_BITS);
        while (s_pv.gen[h] == s_pv.cur && s_pv.key[h] != ix)
            h = (h + 1) & (VMAP_SIZE - 1);
        if (s_pv.gen[h] != s_pv.cur) {
            float *o = s_pv.attrs + (size_t)rows * nstream * 4;
            s_pv.gen[h] = s_pv.cur;
            s_pv.key[h] = ix;
            s_pv.row[h] = (uint16_t)rows;
            for (a = 0; a < NV2A_VSH_INPUTS; a++)
                if (streams & (1u << a)) {
                    fetch_attr(&s_gpu.attr[a], ix, o);
                    o += 4;
                }
            rows++;
        }
        s_pv.slot[i] = s_pv.row[h];
    }

gathered:
    /* Triangles, as raster_batch_program emits them. */
#define PV_TRI(x, y, z) do { s_pv.tris[t++] = s_pv.slot[x]; s_pv.tris[t++] = s_pv.slot[y]; \
                             s_pv.tris[t++] = s_pv.slot[z]; } while (0)
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            PV_TRI(i, i + 1, i + 2);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if ((i & 1) && strip_alt_winding())
                PV_TRI(i + 1, i, i + 2);
            else
                PV_TRI(i, i + 1, i + 2);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++)
            PV_TRI(0, i, i + 1);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            PV_TRI(i, i + 1, i + 2);
            PV_TRI(i, i + 2, i + 3);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            PV_TRI(i, i + 1, i + 3);
            PV_TRI(i, i + 3, i + 2);
        }
        break;
    default:
        break;
    }
#undef PV_TRI

    pb.constants = nv2a_vsh_constants();
    pb.const_stamp = nv2a_vsh_constant_stamps(&pb.const_serial);
    pb.attrs = raw ? NULL : s_pv.attrs;
    pb.raw = raw ? s_raw : NULL;
    pb.raw_first = raw ? lo : 0;
    pb.vertex_count = rows;
    pb.streams = streams;
    pb.imm = (const float (*)[4])imm;
    pb.indices = s_pv.tris;
    pb.index_count = t;
    pb.aa_sx = s_gpu.aa_sx;
    pb.aa_sy = s_gpu.aa_sy;
    pb.fog_enable = s_gpu.fog_enable;
    pb.fog_mode = s_gpu.fog_mode;
    pb.fog_param[0] = s_gpu.fog_param[0];
    pb.fog_param[1] = s_gpu.fog_param[1];

    current_surface(&surf);
    current_render_state(&rs);
    if (no_rc < 0)
        no_rc = getenv("RECOMP_NO_COMBINERS") != NULL;
    current_pixel_state(&ps, (s_gpu.rc_seen && !no_rc) ? NV2A_SHADE_COMBINERS
                                                       : NV2A_SHADE_MODULATE, 1);
    memset(&batch, 0, sizeof batch);
    batch.state = &rs;
    batch.pixel = &ps;
    batch.program = &pb;
    if (s_gpu.texs[0].valid) {
        texture_to_backend(&s_gpu.texs[0], &tex);
        batch.texture = &tex;
    }
    if (!t)
        return 1;                       /* nothing to draw: done all the same */
    if (!s_backend->draw_program(&surf, &batch)) {
        /* The back end may have gone in the call: the GL back end
         * unregisters itself when it cannot make its context (no GLES 3.0
         * on the device), and the CPU path draws from then on. */
        if (raw && s_backend && s_backend->draw_program) {
            raw = 0;                    /* declined as stored: send it gathered */
            goto retry;
        }
        return 0;
    }

    s_gpu.batches_program++;
    s_gpu.verts_program += n;
    s_gpu.tris_drawn += t / 3;
    if (s_gpu.texs[0].valid)
        note_texture_use();
    note_drawn();
    return 1;
}

static void raster_batch(void)
{
    uint32_t i;
    uint32_t before = s_gpu.tris_drawn;
    int textured = 0;

    if (s_gpu.idx_count < 3)
        return;
    if (batch_uses_program() || batch_is_ffp()) {
        if (batch_uses_program() && raster_batch_backend_program())
            return;
        raster_batch_program();
        return;
    }
    if (!batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        return;
    }

    /* Count why, once per batch: the texture stage cannot change inside one. */
    {
        const VertexAttr *tc = texcoord_attr();

        if (!(tc->offset && tc->stride))
            s_gpu.batches_no_uv++;
        else if (!s_gpu.texs[0].valid)
            s_gpu.batches_no_tex++;
        else {
            s_gpu.batches_textured++;
            textured = 1;
            note_texture_use();
        }
    }

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++) {
            if ((i & 1) && strip_alt_winding())
                raster_indexed(s_gpu.idx[i+1], s_gpu.idx[i], s_gpu.idx[i+2],
                               vertex_color(s_gpu.idx[i]));
            else
                raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                               vertex_color(s_gpu.idx[i]));
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[0], s_gpu.idx[i], s_gpu.idx[i+1],
                           vertex_color(s_gpu.idx[0]));
        break;
    case NV_PRIM_QUADS:
        /* Independent quads, four vertices each. A batch of eight is two
         * quads, not one six-triangle fan around the first vertex; with
         * exactly four the two agreed, which is why sharing the fan arm
         * looked right. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+2], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        /* Each vertex pair past the first closes another quad against the
         * pair before it. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+3], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    default:
        break;                             /* points and lines: not yet */
    }
    if (backend_draws())
        backend_flush(textured, 1);

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);

    /* Capture the surface while the geometry is still on it.
     *
     * The periodic report dumps too, but a title clears every frame and draws
     * in only some of them, so a report almost always lands on a surface that
     * was wiped a moment ago -- which reads as "nothing was drawn" when the
     * triangles went down correctly just before it. A few frames that actually
     * contain geometry are worth more than any number of clears. Only until
     * the first flip: from then on the report dumps presented frames. */
    if (s_gpu.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW
        && !s_flip_dumping && !s_gpu.flips) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
/* RECOMP_FRAME_TRACE=<flag file>: once the file exists, log every batch of
 * the next whole frame (flip to flip) -- where it drew, with what texture,
 * blend and depth state, and how many pixels it actually wrote. "The frame
 * is black" has many causes and this is what tells them apart. */

static void frame_trace_flip(void)
{
    static const char *flag = (const char *)-1;
    if (flag == (const char *)-1)
        flag = getenv("RECOMP_FRAME_TRACE");
    if (!flag)
        return;
    if (s_ftrace == 3) {
        /* Deleting the flag file re-arms the trace for the next time it
         * appears, so one run can trace several moments. */
        FILE *f = fopen(flag, "rb");
        if (f)
            fclose(f);
        else
            s_ftrace = 0;
        return;
    }
    if (s_ftrace == 2) {
        s_ftrace = 3;
        fprintf(stderr, "[FTRACE] end of frame\n");
        return;
    }
    if (s_ftrace == 0) {
        FILE *f = fopen(flag, "rb");
        if (!f)
            return;
        fclose(f);
        s_ftrace = 2;
        fprintf(stderr, "[FTRACE] frame begins (flip %u)\n", s_gpu.flips);
    }
}

/* Triangles submitted this frame, whatever draws them (the CPU rasteriser,
 * a back end's fixed-function or vertex-program batches): the window's
 * aspect policy tells a 3D scene from a menu by it (aspect_policy.h). */
static uint32_t s_frame_tris;

/* Triangles a batch of n vertices of primitive prim makes; points and lines
 * none. */
static uint32_t prim_triangles(uint32_t prim, uint32_t n)
{
    switch (prim) {
    case NV_PRIM_TRIANGLES:      return n / 3;
    case NV_PRIM_TRIANGLE_STRIP:
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:        return n > 2 ? n - 2 : 0;
    case NV_PRIM_QUADS:          return n / 4 * 2;
    case NV_PRIM_QUAD_STRIP:     return n > 3 ? (n - 2) / 2 * 2 : 0;
    default:                     return 0;
    }
}

static void draw_primitive(void)
{
    float v[4];
    uint32_t i;

    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
    s_gpu.draws++;
    if ((s_gpu.draws % 200) == 0)
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    s_gpu.verts += s_gpu.idx_count;
    s_frame_tris += prim_triangles(s_gpu.prim, s_gpu.idx_count);

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured. */
    {
        float p[4];
        if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    {
        uint32_t t0 = s_gpu.tris_drawn;
        uint64_t p0 = s_gpu.pixels;
        uint64_t r0 = xbox_HostNowNs();
        raster_batch();
        s_render_ns += xbox_HostNowNs() - r0;
        if (s_ftrace == 2) {
            fprintf(stderr, "[FTRACE] %s prim %u n %u surf %08X %ux%u+%u+%u"
                    " pitch %u | tex %s %08X %ux%u fmt %02X | blend %u %X/%X"
                    " eq %X | z test %u func %X mask %u | tris %u px %llu\n",
                    (s_gpu.xf_mode & 3) == 2 ? "PRG" : "FIX", s_gpu.prim,
                    s_gpu.idx_count, s_gpu.color_offset, s_gpu.clip_w,
                    s_gpu.clip_h, s_gpu.clip_x, s_gpu.clip_y, s_gpu.pitch,
                    s_gpu.texs[0].valid ? "on" : "off", s_gpu.texs[0].offset,
                    s_gpu.texs[0].width, s_gpu.texs[0].height, s_gpu.texs[0].color,
                    s_gpu.blend_enable, s_gpu.blend_sfactor,
                    s_gpu.blend_dfactor, s_gpu.blend_equation,
                    s_gpu.depth_test, s_gpu.depth_func, s_gpu.depth_mask,
                    s_gpu.tris_drawn - t0,
                    (unsigned long long)(s_gpu.pixels - p0));
            if ((s_gpu.xf_mode & 3) == 2 && s_xf)
                fprintf(stderr, "[FTRACE]     v0 %g %g %g %g  v1 %g %g  v2 %g %g%c",
                        s_xf[0].pos[0], s_xf[0].pos[1], s_xf[0].pos[2],
                        s_xf[0].pos[3], s_xf[1].pos[0], s_xf[1].pos[1],
                        s_xf[2].pos[0], s_xf[2].pos[1], 10);
            if (s_gpu.rc_seen) {
                int st;
                fprintf(stderr, "[FTRACE]     rc stages %u ctl %08X prog %05X fin %08X %08X",
                        s_gpu.rc.control & 0xFF, s_gpu.rc.control,
                        s_gpu.rc.stage_program, s_gpu.rc.final0, s_gpu.rc.final1);
                for (st = 0; st < 4; st++)
                    if ((s_gpu.rc.stage_program >> (st * 5)) & 0x1F)
                        fprintf(stderr, " | t%d %08X %ux%u fmt %02X%s", st,
                                s_gpu.texs[st].offset, s_gpu.texs[st].width,
                                s_gpu.texs[st].height, s_gpu.texs[st].color,
                                s_gpu.texs[st].valid ? "" : " (invalid)");
                fputc(10, stderr);
                for (st = 0; st < (int)(s_gpu.rc.control & 0xFF) && st < 8; st++)
                    fprintf(stderr, "[FTRACE]       s%d icw %08X %08X ocw %08X %08X c %08X %08X%c",
                            st, s_gpu.rc.color_icw[st], s_gpu.rc.alpha_icw[st],
                            s_gpu.rc.color_ocw[st], s_gpu.rc.alpha_ocw[st],
                            s_gpu.rc.factor0[st], s_gpu.rc.factor1[st], 10);
            }
            if ((s_gpu.xf_mode & 3) == 2) {
                uint32_t r;
                for (r = 112; r < 116; r++) {
                    const float *c = nv2a_vsh_constant(r);
                    fprintf(stderr, "[FTRACE]     c[%u] %g %g %g %g%c", r,
                            c[0], c[1], c[2], c[3], 10);
                }
            }
        }
    }

    static int verbose = -1;               /* cached: this runs every draw */
    if (verbose < 0)
        verbose = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
    if (verbose) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            /* The texture stage, for either kind of batch. This used to print
             * only for inline batches, which meant a title drawing through
             * vertex arrays -- Half-Life 2's menu, for one -- showed no
             * texture state at all, and the reason a quad sampled flat was
             * invisible. */
            fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                            " colour 0x%02X swizzled %d valid %d\n",
                    s_gpu.texs[0].offset, s_gpu.texs[0].width, s_gpu.texs[0].height,
                    s_gpu.texs[0].pitch, s_gpu.texs[0].color,
                    d3d8_format_is_swizzled(s_gpu.texs[0].color), s_gpu.texs[0].valid);
            {
                uint32_t k;
                for (k = 0; k < s_gpu.idx_count && k < 3; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
            }
            /* An inline batch has no guest buffer to go and look at -- the
             * vertices are the payload -- so print the payload too. */
            if (s_gpu.inline_active) {
                uint32_t k;
                fprintf(stderr, "  [GPU]   inline %u dwords:", s_gpu.inline_count);
                for (k = 0; k < s_gpu.inline_count && k < 16; k++)
                    fprintf(stderr, " %08X", s_gpu.inline_buf[k]);
                fprintf(stderr, "\n");
                for (k = 0; k < s_gpu.idx_count && k < 4; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
                fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                                " colour 0x%02X valid %d\n",
                        s_gpu.texs[0].offset, s_gpu.texs[0].width, s_gpu.texs[0].height,
                        s_gpu.texs[0].pitch, s_gpu.texs[0].color, s_gpu.texs[0].valid);
            }
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

/* Draw the vertices the title wrote straight into the pushbuffer.
 *
 * INLINE_ARRAY carries no offsets and no indices: the dwords between BEGIN and
 * END *are* the vertex buffer, packed in attribute order using the same
 * SET_VERTEX_DATA_ARRAY_FORMAT registers an ordinary array would use. So the
 * whole batch is describable as a vertex array whose base happens to be that
 * payload, which means synthesising the layout and handing it to the existing
 * path -- rather than a second copy of the topology and rasterisation code.
 *
 * The title's own attribute table is saved and put back: these offsets and
 * strides are ours, and it has not stopped using its.
 *
 * ponytail: each attribute is padded to a whole dword. That is exact for the
 * float and D3DCOLOR formats every inline batch actually uses; a packed
 * sub-dword attribute would need the unpadded layout.
 */
static void draw_inline_array(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t off = 0, a, i, vsize, count;

    memcpy(saved, s_gpu.attr, sizeof saved);

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        uint32_t bytes;
        if (!s_gpu.attr[a].size)
            continue;
        switch (s_gpu.attr[a].type) {
        case 0:  bytes = 4;                        break;  /* D3DCOLOR   */
        case 2:  bytes = 4 * s_gpu.attr[a].size;   break;  /* float      */
        case 4:  bytes = s_gpu.attr[a].size;       break;  /* ubyte norm */
        default: bytes = 4 * s_gpu.attr[a].size;   break;
        }
        s_gpu.attr[a].offset = off;
        off += (bytes + 3u) & ~3u;
    }
    vsize = off;
    if (!vsize)
        goto out;

    count = (s_gpu.inline_count * 4) / vsize;
    if (count < 3 || count > NV_MAX_INDICES)
        goto out;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_gpu.attr[a].stride = vsize;

    for (i = 0; i < count; i++)
        s_gpu.idx[i] = i;
    s_gpu.idx_count = count;

    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
}

/* Draw the vertices SET_VERTEX3F/4F completed.
 *
 * Same trick as draw_inline_array: rather than a second copy of the topology
 * and rasterisation code, describe what was accumulated as an ordinary vertex
 * array and hand it to the existing path. The layout is ours and fixed, so
 * the attribute table is written out here rather than derived from the
 * title's format registers.
 *
 * The title's own table is saved and put back -- it has not stopped using it.
 *
 * ponytail: position, diffuse and texcoord0 only. That is what a 2D quad
 * carries and what this rasteriser samples; a second texcoord set or a normal
 * would need the D3D11 translator, not more slots here.
 */
static void draw_immediate(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t i;

    if (s_gpu.imm_count < 3)
        return;

    memcpy(saved, s_gpu.attr, sizeof saved);
    memset(s_gpu.attr, 0, sizeof s_gpu.attr);
    /* Offsets are byte offsets into inline_buf here, not guest addresses --
     * fetch_attr reads them that way while inline_active is set, which is
     * also why 0 is a legal offset for position. Every attribute is a float4;
     * the ones the batch never set carry their standing values, as on the
     * GPU. */
    for (i = 0; i < NV_VERTEX_ATTRS; i++) {
        s_gpu.attr[i].type = 2;
        s_gpu.attr[i].size = 4;
        s_gpu.attr[i].offset = i * 16;
        s_gpu.attr[i].stride = IMM_VERTEX_DWORDS * 4;
    }

    for (i = 0; i < s_gpu.imm_count && i < NV_MAX_INDICES; i++)
        s_gpu.idx[i] = i;
    s_gpu.idx_count = i;

    /* fetch_attr bounds-checks against inline_count dwords. */
    s_gpu.inline_count = s_gpu.imm_count * IMM_VERTEX_DWORDS;
    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
    s_gpu.inline_count = 0;
}

/* A vertex is complete: append all 16 attributes in the layout
 * draw_immediate describes. */
static void imm_emit_vertex(void)
{
    uint32_t at = s_gpu.imm_count * IMM_VERTEX_DWORDS;

    if (!s_gpu.prim || at + IMM_VERTEX_DWORDS > NV_MAX_INLINE)
        return;
    memcpy(&s_gpu.inline_buf[at], s_gpu.imm_attr, sizeof s_gpu.imm_attr);
    s_gpu.imm_count++;
}

/* The immediate-mode writes. Returns 1 if `method` was one of them.
 *
 * Split out because it is a range test against five separate bases, and that
 * reads better than five more cases in an already long switch.
 */
/* The immediate-mode attribute methods (xemu pgraph.c SET_VERTEX_DATA*).
 * Each sets an attribute's current value; completing attribute 0 -- the
 * position -- emits a vertex carrying every attribute as it stands. This used
 * to keep position, diffuse and texcoord 0 only, which dropped any quad with
 * a second texture coordinate: Burnout 3 composites its whole 3D scene into
 * the frame with exactly such a quad, and the frame never got the scene. */
static int imm_vertex_method(uint32_t method, uint32_t param)
{
    union { uint32_t u; float f; } v;
    uint32_t attr, c;
    float *a;
    v.u = param;

    if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        c = (method - NV097_SET_VERTEX4F) / 4;
        s_gpu.imm_attr[0][c] = v.f;
        s_gpu.imm_used |= 1;
        if (c == 3)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        c = (method - NV097_SET_VERTEX3F) / 4;
        s_gpu.imm_attr[0][c] = v.f;
        s_gpu.imm_used |= 1;
        if (c == 2) {
            s_gpu.imm_attr[0][3] = 1.0f;
            imm_emit_vertex();
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        attr = (method - NV097_SET_VERTEX_DATA2F_M) / 8;
        c = ((method - NV097_SET_VERTEX_DATA2F_M) % 8) / 4;
        a = s_gpu.imm_attr[attr];
        a[c] = v.f;
        if (c == 1) {
            a[2] = 0.0f; a[3] = 1.0f;
        }
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 1)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        attr = (method - NV097_SET_VERTEX_DATA4F_M) / 16;
        c = ((method - NV097_SET_VERTEX_DATA4F_M) % 16) / 4;
        s_gpu.imm_attr[attr][c] = v.f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 3)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2S
            && method < NV097_SET_VERTEX_DATA2S + NV_VERTEX_ATTRS * 4) {
        attr = (method - NV097_SET_VERTEX_DATA2S) / 4;
        a = s_gpu.imm_attr[attr];
        a[0] = (float)(int16_t)(param & 0xFFFF);
        a[1] = (float)(int16_t)(param >> 16);
        a[2] = 0.0f; a[3] = 1.0f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        /* Bytes in register order, x from the low byte (xemu). */
        attr = (method - NV097_SET_VERTEX_DATA4UB) / 4;
        a = s_gpu.imm_attr[attr];
        a[0] = (float)( param        & 0xFF) / 255.0f;
        a[1] = (float)((param >>  8) & 0xFF) / 255.0f;
        a[2] = (float)((param >> 16) & 0xFF) / 255.0f;
        a[3] = (float)( param >> 24        ) / 255.0f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4S_M
            && method < NV097_SET_VERTEX_DATA4S_M + NV_VERTEX_ATTRS * 8) {
        attr = (method - NV097_SET_VERTEX_DATA4S_M) / 8;
        c = ((method - NV097_SET_VERTEX_DATA4S_M) % 8) / 4;
        a = s_gpu.imm_attr[attr];
        a[c * 2]     = (float)(int16_t)(param & 0xFFFF);
        a[c * 2 + 1] = (float)(int16_t)(param >> 16);
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 1)
            imm_emit_vertex();
        return 1;
    }
    return 0;
}
/* The texture stage registers, all four stages. Stage i's registers are
 * 0x1B00 + 0x40*i onwards; only stage 0 used to be decoded, which was
 * enough for menus drawn one texture at a time and not for a 3D frame that
 * composites its scene through a second stage. */
static void tex_stage_method(uint32_t method, uint32_t param)
{
    uint32_t stage = (method - NV_TEX_FIRST) / 0x40;
    uint32_t reg = NV_TEX_FIRST + ((method - NV_TEX_FIRST) & 0x3F);
    Texture *t = &s_gpu.texs[stage];

    switch (reg) {
    case NV097_SET_TEXTURE_OFFSET:
        /* A DMA-object offset like a surface's: physical, reached through
         * the contiguous window when it names contiguous memory. */
        t->offset = dma_resolve(param);
        break;
    case NV097_SET_TEXTURE_FORMAT:
        t->color = (param >> 8) & 0xFF;
        t->cube = (param >> 2) & 1;
        t->border_color = (param >> 3) & 1;    /* BORDER_SOURCE: colour */
        t->levels = (param >> 16) & 0xF;
        /* A swizzled texture carries its own dimensions here, as log2 in
         * BASE_SIZE_U/V. It has to: IMAGE_RECT describes a linear image, and
         * a title that only uses swizzled textures never sends one. */
        if (tex_size_from_format(t->color)) {
            t->width  = 1u << ((param >> 20) & 0xF);
            t->height = 1u << ((param >> 24) & 0xF);
        }
        break;
    case NV097_SET_TEXTURE_PALETTE:
        /* The low six bits carry the DMA context and the entry count. */
        t->palette = dma_resolve(param & ~0x3Fu);
        break;
    case NV097_SET_TEXTURE_ADDRESS:
        /* Four bits per axis: 1 wrap, 2 mirror, 3 clamp-to-edge, 4 border
         * (the border colour when FORMAT selects it, else clamped). */
        t->addr_u =  param        & 0xF;
        t->addr_v = (param >>  8) & 0xF;
        break;
    case NV097_SET_TEXTURE_CONTROL1:
        t->pitch = param >> 16;          /* linear formats only */
        break;
    case NV097_SET_TEXTURE_IMAGE_RECT:
        t->width  = param >> 16;
        t->height = param & 0xFFFF;
        break;
    case 0x1B14:                                  /* SET_TEXTURE_FILTER */
        t->filter = param;
        break;
    case 0x1B24:                                  /* SET_TEXTURE_BORDER_COLOR */
        t->border = param;
        break;
    default:
        break;
    }
    tex_update_valid(t);
    if (stage == 0)
        record_tex_reg(method, param);
}

/* The flip hold (nv2a_flip_hold.h). The executor arms it at FLIP_STALL; the
 * ack loop in xbox_memory_layout.c polls it each pass and, while it holds,
 * walks nothing, leaves DMA_GET behind PUT and the PFB flush unacknowledged.
 * Both run on the ack thread, so the state needs no lock. */
static XboxFlipHold s_flip_hold;
static uint32_t s_vbl_count;        /* the vblank count last seen to move */
static uint64_t s_vbl_moved_ns;
/* The vblank count when the walk that reaches a FLIP_STALL began, which is
 * close to when the title submitted the segment holding its Swap. Sampled
 * at the FLIP_STALL instead, the segment's own draw time (several ms) moved
 * a frame the title swapped just before a vblank past it; the title's next
 * frame then counted as a second flip in the same vblank and was held for
 * most of a frame. */
static uint32_t s_walk_vbl;

void nv2a_pb_exec_walk_begin(void)
{
    s_walk_vbl = xbox_VblankCount();
}

/* RECOMP_GPU_FPS_CAP: with a back end drawing, at most this many flips per
 * second of host time. A GPU back end draws a menu frame in 2-3 ms, so a
 * title whose front end does not pace itself (Fable's title, menus and pause
 * screen) flips 200-280 times a second and keeps the GPU and two cores busy
 * for frames nobody sees -- on a handheld, battery and heat. The CPU
 * rasteriser is slow enough not to need it.
 *
 * FLIP_STALL (frame presented, back end flipped) books the frame's slot on
 * an absolute host-clock schedule, 1/cap s apart; the executor thread
 * sleeps until it before it walks the next segment (nv2a_pb_exec_flip_poll,
 * top of the ack loop). Not inside FLIP_STALL: the segment holding the flip
 * is finished first and DMA_GET reaches PUT, so the title builds its next
 * frame while the executor sleeps instead of waiting for GPU idle the whole
 * time, and the ack loop does not poll an empty ring meanwhile. Absolute,
 * not "1/cap after this flip": a sleep that wakes late (macOS wakes an
 * ordinary thread's nanosleep ms late) shortens the next one, so the average
 * holds at the cap without spinning. A frame later than a whole period is
 * not held at all (its slot is the flip itself) and restarts the schedule
 * from there, rather than letting the next frames run uncapped to catch up
 * or sleeping a period after a frame that was already slow.
 *
 * Independent of RECOMP_FLIP_PACING (the flip hold, guest vblank clock),
 * which still applies after. Default 60 on macOS and Android, off elsewhere
 * (a back end a game project registers may present with vsync itself);
 * 0 = off. */
static int s_cap_value = -1;             /* gpu_fps_cap's, once computed */
static int s_cap_pre_fg;                 /* the cap before frame generation lowered it */
static int pacing_auto(void);
static int gpu_fps_cap(void)
{
#define cap s_cap_value
    if (cap < 0) {
        const char *e = getenv("RECOMP_GPU_FPS_CAP");
#if defined(__APPLE__) || defined(__ANDROID__)
        int c = 60;
#else
        int c = 0;
#endif
        if (pacing_auto())
            c = 0;          /* RECOMP_PACING=auto picks the rate (pacing_cap_at_flip) */
        if (e && *e)
            c = atoi(e);
        if (c < 0)
            c = 0;
        if (c > 1000)
            c = 1000;
#ifdef XBOXRECOMP_LSFG
        /* RECOMP_LSFG=1: min(cap, display rate / multiplier), see
         * src/framegen/framegen_policy.h. */
        if (framegen_policy_fps_cap(c) != c) {
            fprintf(stderr, "  [GPU] back end frame cap %d fps for frame generation (was %d)\n",
                    framegen_policy_fps_cap(c), c);
            s_cap_pre_fg = c ? c : 60;
            c = framegen_policy_fps_cap(c);
        }
#endif
        cap = c;
        if (e)
            fprintf(stderr, "  [GPU] back end frame cap %d fps (RECOMP_GPU_FPS_CAP=%s)\n",
                    c, e);
    }
    return cap;
#undef cap
}

#ifdef XBOXRECOMP_LSFG
#include "frame_lock.h"
extern void xbox_FramegenRequestStop(const char *why);
static FrameLock s_frame_lock;
static uint64_t s_cap_held_total, s_cap_held_seen;
#endif

/* Both on the executor (ack) thread, except s_cap_until_ns, which any thread
 * may read (nv2a_pb_exec_flip_cap_until_ns). */
static uint64_t s_cap_next_ns;          /* the next frame's slot, 0: none */
static uint64_t s_cap_due_ns;           /* booked by FLIP_STALL, 0: none */
static volatile uint64_t s_cap_until_ns;
static uint64_t s_cap_slept_ns;         /* since the last report */
static uint64_t s_cap_over_ns, s_cap_over_max_ns;   /* woke after the slot */
static uint32_t s_cap_waits, s_cap_late;

/* D3D's InsertCallback (software methods 6 and 7) passes its routine and
 * context through the two clear values it sets just before: SET_ZSTENCIL_
 * CLEAR_VALUE (0x1D8C, PGRAPH 0x401A88) is the routine, SET_COLOR_CLEAR_VALUE
 * (0x1D90, PGRAPH 0x40186C) the context, which its interrupt handler reads
 * back from PGRAPH (docs/technical/pushbuffer-executor.md). XBA-94. */
void nv2a_pb_exec_insert_callback_args(uint32_t *routine, uint32_t *context)
{
    *routine = s_gpu.zstencil_clear;
    *context = s_gpu.clear_color;
}

uint64_t nv2a_pb_exec_flip_cap_until_ns(void)
{
    return s_cap_until_ns;
}

/* RECOMP_PACING=auto (XBA-107, XBA-135): with no RECOMP_GPU_FPS_CAP set (a
 * profile's value or the launcher's Frame cap wins as it is), the cap's
 * rate is chosen at every flip from the title's presentation interval and
 * the frame cost the host sustains (frame_pacing.h), on either renderer;
 * frame generation's lower cap still bounds it. =ask: the title's ask only.
 * Unset or anything else: the fixed cap as before. RECOMP_PACING_LOCK=1/0
 * forces the "locked to its ask" verdict; RECOMP_DISPLAY_HZ (set by the SDL
 * presenter) bounds an IMMEDIATE title. Vblanks keep the console's rate:
 * only the flips are paced. */
static FramePacer s_pacer;
static uint64_t s_pace_slept_ns;        /* slept at the cap since the last flip */
static uint32_t s_pace_rate, s_pace_logged;

/* XBA-136: the slots in phase with the display (frame_pacing_align_slot).
 * The presenter publishes the display's grid and its flip -> present time
 * (nv2a_pb_exec_display_clock, nv2a_pb_exec_present_latency) from its own
 * threads; the executor reads them at each booking. RECOMP_PACING_ALIGN=0
 * turns it off, a fraction (0.3) sets the present's phase in a refresh. */
static PacingAlign s_align;
static uint64_t s_align_slot;           /* the slot the last wait slept to, 0: not held */
static int s_align_restart;             /* the schedule restarted: snap */
static uint32_t s_align_logged;
static volatile uint64_t s_disp_seq, s_disp_ref, s_disp_period, s_disp_gen, s_disp_lat;

/* The display's refresh grid: a vsync (or latch-edge) time and the refresh
 * period on xbox_HostNowNs's clock; period 0: no grid. gen changes when the
 * grid's phase does. One writer at a time (the presenter's vsync source). */
void nv2a_pb_exec_display_clock(uint64_t vsync_ns, uint64_t period_ns, uint32_t gen)
{
    uint64_t q = __atomic_load_n(&s_disp_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&s_disp_seq, q + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&s_disp_ref, vsync_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&s_disp_period, period_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&s_disp_gen, (uint64_t)gen, __ATOMIC_RELAXED);
    __atomic_store_n(&s_disp_seq, q + 2, __ATOMIC_RELEASE);
}

/* The presenter's mean time from a flip's publish to its present call. */
void nv2a_pb_exec_present_latency(uint64_t lat_ns)
{
    __atomic_store_n(&s_disp_lat, lat_ns, __ATOMIC_RELAXED);
}

static int display_clock_read(uint64_t *ref, uint64_t *period, uint32_t *gen)
{
    int tries;
    for (tries = 0; tries < 4; tries++) {
        uint64_t q = __atomic_load_n(&s_disp_seq, __ATOMIC_ACQUIRE), q2;
        if (q & 1u)
            continue;
        *ref = __atomic_load_n(&s_disp_ref, __ATOMIC_RELAXED);
        *period = __atomic_load_n(&s_disp_period, __ATOMIC_RELAXED);
        *gen = (uint32_t)__atomic_load_n(&s_disp_gen, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        q2 = __atomic_load_n(&s_disp_seq, __ATOMIC_RELAXED);
        if (q == q2)
            return q != 0 && *period != 0;
    }
    return 0;
}

static void pacing_align_init(void)
{
    const char *e = getenv("RECOMP_PACING_ALIGN");
    uint32_t pm = PACING_ALIGN_PHASE_PM;
    int on = !(e && *e == '0' && !strchr(e, '.'));

    if (e && strchr(e, '.')) {
        double f = strtod(e, NULL);
        if (f > 0.0 && f < 1.0)
            pm = (uint32_t)(f * 1000.0 + 0.5);
    }
    frame_pacing_align_init(&s_align, on, pm);
    s_align_logged = s_align.changes;
}

/* At a booking under auto: the slot moved into the display's phase. */
static uint64_t pacing_align_slot(uint64_t slot, uint64_t period, uint64_t now)
{
    uint64_t ref = 0, rper = 0, r;
    uint32_t gen = 0;
    int have = s_align.on && display_clock_read(&ref, &rper, &gen);

    r = frame_pacing_align_slot(&s_align, slot, period, s_align_restart, have, ref, rper, gen,
                                __atomic_load_n(&s_disp_lat, __ATOMIC_RELAXED), now);
    s_align_restart = 0;
    if (s_align.changes != s_align_logged) {
        s_align_logged = s_align.changes;
        if (s_align.state == PACING_ALIGN_SEEKING || s_align.state == PACING_ALIGN_LOCKED)
            fprintf(stderr, "  [GPU] pacing: flips in phase with the display (%.2f Hz, %u refreshes "
                    "per frame, present at %u%% of a refresh)\n", 1e9 / (double)s_align.refresh_ns,
                    s_align.refreshes, s_align.phase_pm / 10u);
        else
            fprintf(stderr, "  [GPU] pacing: flips not aligned to the display: %s\n",
                    frame_pacing_align_state(&s_align));
    }
    return r;
}

static int pacing_auto(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PACING"), *c = getenv("RECOMP_GPU_FPS_CAP");
        const char *l = getenv("RECOMP_PACING_LOCK"), *d = getenv("RECOMP_DISPLAY_HZ");
        int want = e && (!strcmp(e, "auto") || !strcmp(e, "ask"));
        on = want && !(c && *c);
        if (on) {
            long hz = d && *d ? strtol(d, NULL, 10) : 0;
            frame_pacing_init(&s_pacer, hz >= 24 && hz <= 480 ? (uint32_t)hz : 0u,
                              l && *l ? (*l != '0') : -1, strcmp(e, "ask") != 0);
            pacing_align_init();
            fprintf(stderr, "  [GPU] pacing %s: the title's presentation interval%s; display %u Hz\n",
                    e, s_pacer.adaptive ? ", then the highest even rate the host sustains" : "",
                    frame_pacing_display(&s_pacer));
        } else if (want) {
            fprintf(stderr, "  [GPU] pacing %s off: RECOMP_GPU_FPS_CAP=%s wins\n", e, c);
        }
    }
    return on;
}

/* At FLIP_STALL, under auto: the rate for the next frame, bounded by a
 * lower cap that frame generation set (hard > 0). A new rate restarts the
 * schedule. */
static int pacing_cap_at_flip(int hard)
{
    uint64_t now = xbox_HostNowNs();
    uint32_t r;

    frame_pacing_vblank(&s_pacer, xbox_VblankCount(), now);
    r = frame_pacing_flip(&s_pacer, now, s_pace_slept_ns);
    s_pace_slept_ns = 0;
    if (s_align_slot && now > s_align_slot)     /* a frame held at the cap: slot -> flip */
        frame_pacing_align_flip(&s_align, now - s_align_slot);
    s_align_slot = 0;
    if (s_pacer.changes != s_pace_logged) {
        s_pace_logged = s_pacer.changes;
        fprintf(stderr, "  [GPU] pacing: %u fps (%s; ask %u fps: %u Hz vblank, interval %s%u)\n",
                s_pacer.rate, s_pacer.why ? s_pacer.why : "-", frame_pacing_ask(&s_pacer),
                s_pacer.vblank_hz, s_pacer.ask_iv ? "" : "IMMEDIATE, display ",
                s_pacer.ask_iv ? s_pacer.ask_iv : frame_pacing_display(&s_pacer));
    }
    if (hard > 0 && (uint32_t)hard < r)
        r = (uint32_t)hard;
    if (r != s_pace_rate) {
        s_pace_rate = r;
        s_cap_next_ns = 0;
    }
    return (int)r;
}

/* At FLIP_STALL: book the slot the next walk waits for. */
static void flip_cap_book(void)
{
    uint64_t period, now;
    int cap;

    if (pacing_auto())
        cap = pacing_cap_at_flip(gpu_fps_cap());
    else
        cap = backend_draws() ? gpu_fps_cap() : 0;
    if (cap <= 0) {
        s_cap_next_ns = s_cap_due_ns = 0;
        return;
    }
    period = 1000000000ull / (uint64_t)cap;
    now = xbox_HostNowNs();
    if (!s_cap_next_ns) {
        s_cap_next_ns = now + period;   /* the first frame starts the schedule */
        s_align_restart = 1;
    } else if (now >= s_cap_next_ns + period) {
        /* Already late: this frame took longer than a period. Its slot is
         * now -- no wait -- and the schedule restarts from here. Sleeping a
         * period after a late flip added 16.7 ms to every slow in-world
         * frame (Fable: ~125 ms frames slept 1333 ms per 10 s). */
        s_cap_late++;
        s_cap_next_ns = now;
        s_cap_due_ns = 0;
        s_align_restart = 1;
        return;
    } else {
        s_cap_next_ns += period;
    }
    if (pacing_auto())                  /* XBA-136: in phase with the display */
        s_cap_next_ns = pacing_align_slot(s_cap_next_ns, period, now);
    s_cap_due_ns = s_cap_next_ns;
}

/* Before a walk: sleep out a booked slot. */
static void flip_cap_wait(void)
{
    uint64_t due = s_cap_due_ns, now;

    if (!due)
        return;
    s_cap_due_ns = 0;
    now = xbox_HostNowNs();
    if (now >= due)
        return;
    s_cap_until_ns = due;
    xbox_HostSleepNs(due - now);
    s_cap_until_ns = 0;
    s_cap_slept_ns += due - now;
    s_cap_waits++;
#ifdef XBOXRECOMP_LSFG
    s_cap_held_total++;
#endif
    s_pace_slept_ns += xbox_HostNowNs() - now;
    s_align_slot = due;
    now = xbox_HostNowNs();
    if (now > due) {
        s_cap_over_ns += now - due;
        if (now - due > s_cap_over_max_ns)
            s_cap_over_max_ns = now - due;
    }
}

#ifdef XBOXRECOMP_LSFG
/* XBA-96: a title that asks for every vblank and is held back by frame
 * generation's cap on nearly every frame wants 60, and game logic that
 * follows the flip rate runs in slow motion at 30 (frame_lock.h). Then frame
 * generation goes off and the cap goes back to what it was before it, once,
 * with a line saying why. RECOMP_FRAME_LOCK=0 leaves it alone. */
static void frame_lock_at_flip(void)
{
    static int enabled = -1;
    int held;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_FRAME_LOCK");
        enabled = !(e && *e == '0');
    }
    if (!enabled || !s_cap_pre_fg || s_frame_lock.verdict)
        return;
    held = s_cap_held_total != s_cap_held_seen;
    s_cap_held_seen = s_cap_held_total;
    if (frame_lock_flip(&s_frame_lock, held) != 1)
        return;
    fprintf(stderr, "  [GPU] the title asks for every vblank and %u of %u frames waited at frame generation's "
                    "%d fps cap: its game logic likely follows the flip rate. Frame generation off, cap %d fps "
                    "(RECOMP_FRAME_LOCK=0 keeps it)\n", s_frame_lock.held, s_frame_lock.flips, s_cap_value,
            s_cap_pre_fg);
    s_cap_value = s_cap_pre_fg;
    s_cap_next_ns = s_cap_due_ns = 0;
    xbox_FramegenRequestStop("the title follows the flip rate (XBA-96)");
}
#endif

/* RECOMP_FLIP_PACING: unset or 0 off (flips complete at once and frame
 * counters are bumped on every flip, as before), edge (D3D's rule: a late
 * frame waits for the next vblank), anything else on. Opt-in: it holds the
 * title's GPU on the guest vblank clock, which only runs under RECOMP_VBLANK,
 * and a held flip is a stall a title's own pacing has never seen here.
 * The first caller can be the timer thread, a KeSetTimer caller (through
 * the vblank mode) or the ack thread. Two first callers read the same
 * variable and store the same values, the edge flag before the mode, so the
 * race costs at most a second log line. No Interlocked: tests link the
 * executor without the platform layer (tests/nv2a_ffp_backend). */
int nv2a_pb_exec_flip_pacing(void)
{
    static volatile int mode = -1;
    if (mode < 0) {
        const char *e = getenv("RECOMP_FLIP_PACING");
        int m = (!e || !*e || !strcmp(e, "0")) ? 0
                : !strcmp(e, "edge") ? 2 : 1;
        s_flip_hold.edge = m == 2;
        mode = m;
        if (e)
            fprintf(stderr, "  [GPU] flip pacing %s (RECOMP_FLIP_PACING=%s)\n",
                    m == 0 ? "off" : m == 2 ? "edge" : "on", e);
    }
    return mode;
}

/* The guest vblank count, and whether it is a running clock: vblanks are on
 * and the count moved within XBOX_FLIP_CLOCK_STALE_NS. Without it a hold
 * would only ever end on its timeout. */
static uint32_t flip_clock(uint64_t now_ns, int *ok)
{
    uint32_t c = xbox_VblankCount();
    if (c != s_vbl_count) {
        s_vbl_count = c;
        s_vbl_moved_ns = now_ns;
    }
    *ok = s_vbl_moved_ns && now_ns - s_vbl_moved_ns < XBOX_FLIP_CLOCK_STALE_NS;
    return c;
}

/* Once per pass of the ack loop: 1 while a flip is held. */
int nv2a_pb_exec_flip_poll(void)
{
    uint64_t now;
    uint32_t c;
    int ok;

    flip_cap_wait();
    if (!s_flip_hold.held)
        return 0;
    now = xbox_HostNowNs();
    c = flip_clock(now, &ok);
    return xbox_FlipHoldPoll(&s_flip_hold, c, now, ok);
}

/* The cap's line in the periodic [GPU] report, then reset. */
static void flip_cap_report(void)
{
    if (pacing_auto() && s_pace_rate) {
        fprintf(stderr, "[GPU] pacing %u fps: ask %u fps (vblank %u Hz, interval %s%u), %s, "
                "frame cost p90 %.1f ms, %u%% of frames missed their slot\n",
                s_pace_rate, frame_pacing_ask(&s_pacer), s_pacer.vblank_hz,
                s_pacer.ask_iv ? "" : "IMMEDIATE, display ",
                s_pacer.ask_iv ? s_pacer.ask_iv : frame_pacing_display(&s_pacer),
                s_pacer.lock_force > 0 || s_pacer.locked > 0 ? "locked to its ask"
                : frame_pacing_free(&s_pacer) ? "adaptive" : "at its ask until judged",
                s_pacer.last_p90_us / 1e3, s_pacer.last_miss_pct);
        if (s_align.state == PACING_ALIGN_SEEKING || s_align.state == PACING_ALIGN_LOCKED)
            fprintf(stderr, "[GPU] pacing display phase: %s, %u refreshes of %.3f ms per frame, "
                    "error %.2f ms, slot->flip %.1f ms, flip->present %.1f ms, %u snaps\n",
                    frame_pacing_align_state(&s_align), s_align.refreshes,
                    s_align.refresh_ns / 1e6, s_align.err_ns / 1e6, s_align.w_ns / 1e6,
                    __atomic_load_n(&s_disp_lat, __ATOMIC_RELAXED) / 1e6, s_align.snaps);
        else
            fprintf(stderr, "[GPU] pacing display phase: %s\n", frame_pacing_align_state(&s_align));
    } else if (!backend_draws() || gpu_fps_cap() <= 0)
        return;
    fprintf(stderr, "[GPU] frame cap %d: waits %u slept %.1f ms, woke %.2f ms"
            " late avg (%.2f max), frames over a period (not held) %u\n",
            pacing_auto() ? (int)s_pace_rate : gpu_fps_cap(), s_cap_waits, s_cap_slept_ns / 1e6,
            s_cap_waits ? s_cap_over_ns / 1e6 / s_cap_waits : 0.0,
            s_cap_over_max_ns / 1e6, s_cap_late);
    s_cap_waits = s_cap_late = 0;
    s_cap_slept_ns = s_cap_over_ns = s_cap_over_max_ns = 0;
}

/* The hold's line in the periodic [GPU] report, then reset. */
static void flip_hold_report(void)
{
    XboxFlipHold *h = &s_flip_hold;
    if (nv2a_pb_exec_flip_pacing() == 0)
        return;
    fprintf(stderr, "[GPU] flip hold: holds %u held_ms %.1f hold_timeouts %u"
            " clock_stops %u iv0 %u iv1 %u iv2 %u iv3 %u nonop %u"
            " counters_owned %u\n", h->holds, h->held_ns / 1e6, h->timeouts,
            h->clock_stops, h->iv[0], h->iv[1], h->iv[2], h->iv[3], h->nonop,
            xbox_Nv2aFrameCountersOwned());
    h->holds = h->timeouts = h->clock_stops = h->nonop = 0;
    h->iv[0] = h->iv[1] = h->iv[2] = h->iv[3] = 0;
    h->held_ns = 0;
}

static int s_exec_inited;

/* SET_TRANSFORM_CONSTANT(i), 0x0B80..0x0BFC on the 3D subchannel, without
 * the general dispatch. A title that skins or animates on the GPU uploads
 * these by the hundred thousand a frame -- most of its pushbuffer words --
 * and each one went through every early check in nv2a_pb_exec_method and the
 * whole range ladder of its default case before reaching the constant store.
 * The dispatcher does exactly three things with such a word: its one-time
 * setup, the register file, and nv2a_vsh_constant_word. This does the last
 * two, and leaves the word to the dispatcher (returns 0) until the setup has
 * run. RECOMP_PB_CONST_FAST=0 sends every word through the dispatcher. */
int nv2a_pb_exec_constant_word(uint32_t method, uint32_t param)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("RECOMP_PB_CONST_FAST");
        on = !(e && e[0] == '0');
    }
    if (!on || !s_exec_inited)
        return 0;
    s_reg[(method & 0x1FFCu) / 4] = param;
    nv2a_vsh_constant_word(param);
    return 1;
}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    /* BACK_END_WRITE_SEMAPHORE_RELEASE. The title's pointer already names the
     * semaphore word; SET_SEMAPHORE_OFFSET is not added (in X-Men Legends it
     * held 0xFF000000 at times, which put the write outside guest memory). */
    if (subch == 0 && method == 0x1D70) {
        uint32_t va;

        s_sem_last = param;
        s_sem_seen = 1;
        SEM_FENCE();
        va = s_sem_va;
        if (va) {
            uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
            *(volatile uint32_t *)(mem + va) = param;
            return;
        }
    }
    if (!s_exec_inited) {
        s_exec_inited = 1;
        s_gpu.aa_sx = s_gpu.aa_sy = 1.0f;
        /* Stencil state as the hardware resets it: GL_ALWAYS, masks 0xFF,
         * GL_KEEP, until the title says otherwise. */
        s_reg[0x0364 / 4] = 0x207u;                            /* FUNC */
        s_reg[0x0360 / 4] = s_reg[0x036C / 4] = 0xFFu;         /* MASK, FUNC_MASK */
        s_reg[0x0370 / 4] = s_reg[0x0374 / 4] = s_reg[0x0378 / 4] = 0x1E00u;
        s_gpu.color_mask = 0x01010101u;           /* all channels, as reset */
        {
            int i;
            for (i = 0; i < NV_VERTEX_ATTRS; i++)
                s_gpu.imm_attr[i][3] = 1.0f;
        }
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    if (subch == 0)                        /* the 3D class's register file */
        s_reg[(method & 0x1FFCu) / 4] = param;
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. Cached: this runs for every method, and an
     * uncached getenv here was a quarter of the executor's time. */
    static int verbose = -1;
    if (verbose < 0)
        verbose = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
    if (verbose) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (s_ftrace == 2) {
        /* RECOMP_FRAME_TRACE_METHODS: every method of the traced frame,
         * less the bulk (vertex data, program and constant uploads). */
        static int all = -1;
        if (all < 0)
            all = getenv("RECOMP_FRAME_TRACE_METHODS") != NULL;
        if (all && !(method >= 0x1800 && method < 0x1A00)
            && !(method >= 0x0B00 && method < 0x0C00))
            fprintf(stderr, "[FTRACE]   m %u:%04X = %08X%c", subch, method,
                    param, 10);
    }
    if (subch == 0 && method >= NV_TEX_FIRST && method <= NV_TEX_LAST) {
        tex_stage_method(method, param);
        return;
    }
    /* XDK D3D's KickOff (found in a title's statically linked D3D, which
     * reads it back; see d3d8ltcg-device-context.md) ends every kick with
     * method 0x0310 on subchannel 5:
     *
     *     ((pushbuffer_address * 8 | (fence & 0x1F)) << 2) | (wrap & 3)
     *
     * The GPU latches it into PGRAPH 0x400B10, and D3D waits there for the
     * kick's fence bits. Unmodelled, 0x400B10 read 0 and the wait ended only
     * when the semaphore happened to be a multiple of 32. (DMA_GET, which D3D
     * reads next, is published by the walk itself; see nv2a_pb_scan.c. The
     * marker's own address is behind the executor's read position, so it is
     * not used for that.)
     *
     * D3D compares the latch's fence bits with the semaphore, so the two have
     * to tell the same story. When a back end names the semaphore the
     * executor writes it from the title's own releases, and the marker agrees
     * with it. A project that mirrors the title's fence instead (the runtime
     * writes "everything submitted is consumed" into the word,
     * xbox_Nv2aMirrorFence) holds D3D's own submitted counter there, which
     * the marker's fence bits do not match, and D3D's wait then lasts for
     * ever: the title froze at the first screen whose walk was long enough to
     * enter it. So for a mirrored fence the latch carries the mirror's fence
     * bits and the marker's address, which is what a GPU that had consumed
     * everything would report. */
    if (method == 0x0310 && subch == 5 && nv2a_pb_dma_walk()) {
        volatile uint32_t *r = xbox_Nv2aRegPtr(0x400B10);

        if (!r)
            return;
        if (!s_sem_va) {
            uint32_t va = xbox_Nv2aFenceWordVa();

            if (va) {
                uint32_t sem = *(volatile uint32_t *)
                               ((uint8_t *)xbox_GetMemoryOffset() + va);
                param = (param & ~0x7Cu) | ((sem & 0x1Fu) << 2);
            }
        }
        *r = param;
        return;
    }
    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        note_unhandled(method, param);
        return;
    }
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_raw_h = param;
        surface_apply_clip();
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_raw_v = param;
        surface_apply_clip();
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        surface_apply_clip();
        break;
    case 0x1D98:                       /* SET_CLEAR_RECT_HORIZONTAL */
        s_gpu.clear_rect_h = param;
        s_gpu.clear_rect_set |= 1;
        break;
    case 0x1D9C:                       /* SET_CLEAR_RECT_VERTICAL */
        s_gpu.clear_rect_v = param;
        s_gpu.clear_rect_set |= 2;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] color offset -> %08X%c", param, 10);
        s_gpu.color_offset = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_SET_BLEND_ENABLE:
        s_gpu.blend_enable = param;
        break;
    case NV097_SET_BLEND_EQUATION:
        s_gpu.blend_equation = param;
        break;
    case 0x0358:                                  /* SET_COLOR_MASK */
        s_gpu.color_mask = param;
        break;

    /* Register combiners. */
    case 0x0288: s_gpu.rc.final0 = param; break;  /* SPECULAR_FOG_CW0 */
    case 0x028C: s_gpu.rc.final1 = param; break;  /* SPECULAR_FOG_CW1 */
    case 0x1E20: s_gpu.rc.final_c0 = param; break;/* SPECULAR_FOG_FACTOR */
    case 0x1E24: s_gpu.rc.final_c1 = param; break;
    case 0x1E60:                                  /* COMBINER_CONTROL */
        s_gpu.rc.control = param;
        s_gpu.rc_seen = 1;
        break;
    case 0x1E70: s_gpu.rc.stage_program = param; break;
    case 0x1E78: break;                           /* OTHER_STAGE_INPUT: from s_reg */
    case 0x17F8: s_gpu.clip_plane_mode = param; break;

    /* Alpha test and fog. */
    case 0x0300: s_gpu.alpha_test = param; break;
    case 0x033C: s_gpu.alpha_func = param; break;
    case 0x0340: s_gpu.alpha_ref = param; break;
    case 0x02A4: s_gpu.fog_enable = param; break;
    case 0x02A8: s_gpu.fog_color = param; break;  /* R in bits 0-7 */
    case 0x029C: s_gpu.fog_mode = param; break;
    case 0x09C0: memcpy(&s_gpu.fog_param[0], &param, 4); break;
    case 0x09C4: memcpy(&s_gpu.fog_param[1], &param, 4); break;
    case 0x17C8:                                  /* CLEAR_REPORT_VALUE */
        if (zpass_queued())
            zpass_queue(0, 0, 0);
        else if (zpass_mode() == ZPASS_SYNC && s_backend && s_backend->zpass_read)
            (void)s_backend->zpass_read();
        s_gpu.zpass_count = 0;
        break;
    case 0x17CC:                                  /* SET_ZPASS_PIXEL_COUNT_ENABLE */
        s_gpu.zpass_enable = param;
        break;
    case 0x17D0: {                                /* GET_REPORT */
        /* The GPU's answer to a visibility test: 16 bytes at the report
         * DMA offset -- a timestamp, the pixel count, and 0 for "done".
         * Never written, a title polling it (D3D's GetVisibilityTestResult
         * starts the slot at 0xFFFFFFFF) sees every test incomplete. Burnout
         * 3 culls its world with these, so the race's main view drew only
         * the sky while the cube-map passes, which do not test, drew it all.
         * The report DMA object starts at physical 0 here, as D3D sets it. */
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        uint32_t at = dma_resolve(param & 0x00FFFFFFu);
        uint64_t stamp = (uint64_t)s_gpu.reports++ * 1000u;
        int mode = zpass_mode();
        if (mode == ZPASS_DEFER || mode == ZPASS_LAZY) {
            zpass_queue(at, s_gpu.zpass_count, stamp);
            break;
        }
        if (mode == ZPASS_ALWAYS && !s_gpu.zpass_count)
            s_gpu.zpass_count = 1;
        else if (s_backend && s_backend->zpass_read)
            s_gpu.zpass_count += s_backend->zpass_read();
        memcpy(mem + at, &stamp, 8);
        memcpy(mem + at + 8, &s_gpu.zpass_count, 4);
        memset(mem + at + 12, 0, 4);
        break;
    }
    case NV097_SET_BLEND_COLOR:
        s_gpu.blend_color = param;
        break;
    case NV097_SET_BLEND_FUNC_SFACTOR:
        s_gpu.blend_sfactor = param;
        break;
    case NV097_SET_BLEND_FUNC_DFACTOR:
        s_gpu.blend_dfactor = param;
        break;
    case NV097_CLEAR_SURFACE:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] clear %X surf %08X %ux%u colour %08X%c",
                    param, s_gpu.color_offset, s_gpu.clip_w, s_gpu.clip_h,
                    s_gpu.clear_color, 10);
    {
        uint64_t r0 = xbox_HostNowNs();
        clear_surface(param);
        if ((param & 0x01) && !backend_draws())   /* Z, on the CPU path */
            zbuf_clear();
        s_render_ns += xbox_HostNowNs() - r0;
        break;
    }

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        } else {
            /* Three ways a batch can have arrived, and only one is in use at
             * a time: vertices completed by SET_VERTEX4F, a payload written
             * with INLINE_ARRAY, or indices into the title's own arrays. */
            if (s_gpu.imm_count)
                draw_immediate();
            else if (s_gpu.inline_count)
                draw_inline_array();
            else
                draw_primitive();
            s_gpu.prim = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        }
        break;

    case NV097_INLINE_ARRAY:
        /* Vertex data, not a pointer to it. Buffered rather than decoded here
         * because the format is only fully known at END. */
        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE)
            s_gpu.inline_buf[s_gpu.inline_count++] = param;
        break;

    case NV097_DRAW_ARRAYS: {
        /* The method this title actually draws with, and the reason the
         * executor reported zero draws while geometry was being submitted the
         * whole time: BEGIN_END arrived, END arrived, and in between came a
         * run description rather than the index list the draw path wanted, so
         * every batch ended with idx_count == 0 and was dropped in silence.
         *
         * Expanded into indices because that is what the rasteriser consumes,
         * and an implicit run is just the indices start..start+count-1. */
        uint32_t start = param & 0x00FFFFFFu;
        uint32_t count = ((param >> 24) & 0xFFu) + 1u;
        uint32_t i;

        if (!s_gpu.prim)
            break;
        for (i = 0; i < count && s_gpu.idx_count < NV_MAX_INDICES; i++)
            s_gpu.idx[s_gpu.idx_count++] = start + i;
        break;
    }

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        if (s_gpu.prim && s_gpu.idx_count + 2 <= NV_MAX_INDICES) {
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
        }
        break;

    case 0x1808:                            /* ARRAY_ELEMENT32: one index */
        if (s_gpu.prim && s_gpu.idx_count < NV_MAX_INDICES)
            s_gpu.idx[s_gpu.idx_count++] = param;
        break;
    case NV097_NO_OPERATION:
        /* Non-zero, it is one of D3D's software methods, which the GPU traps
         * to D3D's interrupt handler on the console: the low five bits say
         * which, the rest is its payload. The GPU stops until the handler
         * has run, and some handlers rewrite the pushbuffer ahead of it (a
         * re-run pushbuffer's return JUMP), so a title hook that services
         * them runs here, synchronously, before the walk reads on. The
         * swap (1) is the flip hold's (nv2a_flip_hold.h).
         *
         * Not the title's own handler: its DPC loops until PGRAPH_INTR
         * reads back zero after a write-1-to-clear, which plain-RAM
         * registers never do (docs/technical/pushbuffer-executor.md). */
#ifdef XBOXRECOMP_LSFG
        if ((param & 0x1Fu) == 1u)
            frame_lock_swap(&s_frame_lock, param >> 5);
#endif
        if ((param & 0x1Fu) == 1u && pacing_auto())     /* RECOMP_PACING=auto: the title's ask */
            frame_pacing_swap(&s_pacer, param >> 5);
        if (param && (param & 0x1Fu) != 1u) {
            int (*hook)(uint32_t) = s_swm_hook;
            if (hook && hook(param)) {
                s_swm_serviced++;
                return;
            }
            s_swm_ignored[param & 0x1Fu]++;
        }
        if (!nv2a_pb_exec_flip_pacing()) {
            if (!imm_vertex_method(method, param))
                note_unhandled(method, param);   /* as before the hold */
            break;
        }
        if (param)
            xbox_FlipHoldSwap(&s_flip_hold, param);
        return;

    case NV097_SET_FLIP_READ:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] P_READ %u%c", param, 10);
        s_gpu.flip_read = param;
        return;

    case NV097_SET_FLIP_WRITE:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] P_WRITE %u%c", param, 10);
        s_gpu.flip_write = param;
        return;

    case NV097_SET_FLIP_MODULO:
        s_gpu.flip_modulo = param;
        return;

    case NV097_FLIP_INCREMENT_WRITE:
        s_gpu.flip_write = s_gpu.flip_modulo
                         ? (s_gpu.flip_write + 1) % s_gpu.flip_modulo
                         : s_gpu.flip_write + 1;
        s_gpu.flips++;
        frame_trace_flip();
        return;

    case NV097_FLIP_STALL: {
        /* The flip is measured from the vblank the title swapped in (the
         * walk's start, s_walk_vbl); the hold's clock starts here, before
         * the present below: a vsynced present can hold this thread for a
         * host frame, and that time counts against the hold. */
        uint64_t stall_ns = xbox_HostNowNs();
        int clock_ok;
        uint32_t stall_vbl;
        (void)flip_clock(stall_ns, &clock_ok);
        stall_vbl = s_walk_vbl;
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] FLIP_STALL presenting %08X (color now %08X)%c",
                    s_gpu.drawn_offset, s_gpu.color_offset, 10);
        /* The stall ends when the buffer being read is the one just finished.
         * There is no scanout here to wait for, so that is now; when the
         * title may go on is the flip hold's (below). */
        s_gpu.flip_read = s_gpu.flip_write;
        /* And this is a completed swap, which is what a title's own swap
         * counter counts -- see xbox_Nv2aFrameCounterFlip. */
        xbox_Nv2aFrameCounterFlip();
        if (s_zp_count) {                     /* what is in of the frame's reports */
            uint64_t r0 = xbox_HostNowNs();
            s_zp_at = ZP_AT_FLIP;
            zpass_resolve_queue(0, 0);
            s_zp_at = ZP_AT_OTHER;
            s_render_ns += xbox_HostNowNs() - r0;
        }
        {
            /* RECOMP_FB_DUMP_FLIPS: dump every presented frame, not one per
             * report -- a consecutive sequence, which is what a headless
             * recording (frames -> ffmpeg) needs. "1" dumps from boot; any
             * other value names a file, and frames are dumped while it
             * exists, so a capture can start mid-game without writing
             * gigabytes of menus first. Decided before the present: a dump
             * needs this frame's pixels in guest memory now. */
            static const char *spec = (const char *)-1;
            if (spec == (const char *)-1)
                spec = getenv("RECOMP_FB_DUMP_FLIPS");
            if (spec) {
                int on = strcmp(spec, "1") == 0;
                if (!on) {
                    FILE *f = fopen(spec, "rb");
                    if (f) { fclose(f); on = 1; }
                }
                s_flip_dumping = on;
            }
        }
        {
            /* The surface this flip presents: the buffer the title has
             * finished is the one the last batch drew into, which is what
             * drawn_offset holds and why it exists -- by the flip,
             * color_offset has already moved to the next buffer. */
            Nv2aPresentSurface now, ready;
            int drawn = s_gpu.drawn_offset != 0, have = 0;
            uint32_t done = drawn ? s_gpu.drawn_offset : s_gpu.color_offset;
            memset(&now, 0, sizeof now);
            now.va = done ? dma_resolve(done) : 0;
            now.pitch = drawn ? s_gpu.drawn_pitch : s_gpu.pitch;
            now.bpp = drawn ? s_gpu.drawn_bpp : surface_bpp();
            now.format = drawn ? s_gpu.drawn_format : s_gpu.format;
            now.x = drawn ? s_gpu.drawn_x : s_gpu.clip_x;
            now.y = drawn ? s_gpu.drawn_y : s_gpu.clip_y;
            now.w = drawn ? s_gpu.drawn_w : s_gpu.clip_w;
            now.h = drawn ? s_gpu.drawn_h : s_gpu.clip_h;
            ready = now;
            if (s_backend_present) {
                /* The back end brings the presented surface (and only that)
                 * into guest memory; without a dump it may hand back an
                 * earlier frame whose read-back has completed meanwhile
                 * (nv2a_backend.h). */
                uint64_t r0 = xbox_HostNowNs();
                int sync = s_flip_dumping || s_dump_at_flip;
                have = done && s_gpu.pitch
                    && s_backend_present(&now, sync, &ready) && ready.va;
                if (sync && have && (ready.va != now.va || ready.host_px))
                    ready = now;          /* the contract says otherwise: be safe */
                /* The present's read-back waited for the GPU up to some
                 * point: the reports drawn before it are in now. */
                if (s_zp_count) {
                    s_zp_at = ZP_AT_FLIP;
                    zpass_resolve_queue(0, 0);
                    s_zp_at = ZP_AT_OTHER;
                }
                s_render_ns += xbox_HostNowNs() - r0;
            } else {
                if (s_backend_present_sync) {
                    uint64_t r0 = xbox_HostNowNs();
                    s_backend_present_sync();
                    s_render_ns += xbox_HostNowNs() - r0;
                }
                have = done && s_gpu.pitch;
            }
            render_time_frame();
            /* Hand the window a copy of the frame just finished. Copying
             * here, rather than letting the window read guest memory on its
             * own clock, is what stops it showing a surface the rasteriser
             * is still writing. */
            {
                extern void xbox_FramebufferWindowFrameStats(uint32_t);
                extern void xbox_FramebufferWindowFrameTris(uint32_t);
                static uint32_t draws_at_flip;
                xbox_FramebufferWindowFrameStats(s_gpu.draws - draws_at_flip);
                draws_at_flip = s_gpu.draws;
                xbox_FramebufferWindowFrameTris(s_frame_tris);
                s_frame_tris = 0;
            }
            /* The compat runner's view of the frame (recomp_status.h). */
            if (have && s_backend_present && ready.host_px)
                recomp_status_frame(ready.host_px, ready.host_w, ready.host_h, ready.host_pitch, 4);
            else if (have)
                recomp_status_frame((const void *)((uintptr_t)ready.va + (uintptr_t)xbox_GetMemoryOffset()),
                                    ready.w, ready.h, ready.pitch, ready.bpp);
            else
                recomp_status_frame(NULL, 0, 0, 0, 0);
            if (have && s_backend_present && ready.host_px) {
                /* The back end's own pixels (an internal resolution above
                 * the guest's, or RECOMP_GPU_PRESENT=host): straight to the
                 * window, guest memory untouched. */
                extern int xbox_FramebufferWindowPresentHost(const void *, uint32_t, uint32_t,
                                                             uint32_t, uint32_t);
                (void)xbox_FramebufferWindowPresentHost(ready.host_px, ready.host_w,
                                                        ready.host_h, ready.host_pitch,
                                                        ready.host_format);
            } else if (have) {
                extern void xbox_FramebufferWindowPresent(uint32_t, uint32_t);
                extern void xbox_FramebufferWindowSetFormat(uint32_t);
                /* How 16-bit pixels sit in guest memory: a back end with a
                 * present hook stores the surface's own format (X1R5G5B5
                 * for colour formats 1 and 2); the CPU rasteriser writes
                 * R5G6B5 whatever the format. */
                xbox_FramebufferWindowSetFormat(s_backend_present ? ready.format & 0xF : 0x3);
                xbox_FramebufferWindowSet(ready.va, ready.pitch);
                xbox_FramebufferWindowSetAA(s_gpu.aa_sy > 1.5f ? 2 : 1);
                xbox_FramebufferWindowPresent(ready.va, ready.pitch);
            }
            if (s_flip_dumping)
                dump_surface_bmp();
            /* The report's picture (fb_dump_report): the frame just
             * presented, complete, rather than a surface caught mid-frame. */
            if (s_dump_at_flip && !s_flip_dumping)
                dump_surface_bmp();
            s_dump_at_flip = 0;
        }
        s_gpu.drawn_stale = 1;
        if (s_backend && s_backend->flip)
            s_backend->flip();
        flip_cap_book();
#ifdef XBOXRECOMP_LSFG
        frame_lock_at_flip();
#endif

        if (verbose) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u\n",
                        s_gpu.flips, s_gpu.flip_read, s_gpu.flip_write);
                fflush(stderr);
            }
        }
        /* Arm the hold. Not inside a pushbuffer CALL: the subroutine's
         * return lives in this walk alone, so the hold could not resume
         * there; that flip completes at once and still sets the count the
         * next one is measured from. The swap above (frame counter,
         * present) is counted now, when the hold is armed, not at its
         * release: what the title waits on is GET and the PFB flush, and
         * a title polling its swap counter sees the flip up to a vblank
         * early. */
        if (nv2a_pb_exec_flip_pacing()) {
            extern int nv2a_pb_scan_in_call(void);
            xbox_FlipHoldStall(&s_flip_hold, stall_vbl, stall_ns,
                               clock_ok && !nv2a_pb_scan_in_call());
        }
        return;
    }

    /* Vertex programs and their constants: forwarded as they arrive. */
    case 0x1E94:                                  /* TRANSFORM_EXECUTION_MODE */
        s_gpu.xf_mode = param;
        break;
    case 0x1E98:                                  /* _PROGRAM_CXT_WRITE_EN */
        nv2a_vsh_set_cxt_write(param);
        break;
    case 0x1E9C:                                  /* _PROGRAM_LOAD */
        nv2a_vsh_set_load_slot(param);
        break;
    case 0x1EA0:                                  /* _PROGRAM_START */
        nv2a_vsh_set_start_slot(param);
        break;
    case 0x1EA4:                                  /* _CONSTANT_LOAD */
        nv2a_vsh_set_constant_load(param);
        break;

    /* Fixed-function transform: the composite matrix, 16 floats. The viewport
     * offset arrives in the constant file, above. */
    case 0x0680: case 0x0684: case 0x0688: case 0x068C:
    case 0x0690: case 0x0694: case 0x0698: case 0x069C:
    case 0x06A0: case 0x06A4: case 0x06A8: case 0x06AC:
    case 0x06B0: case 0x06B4: case 0x06B8: case 0x06BC:
        memcpy(&s_gpu.composite[(method - 0x0680) / 4], &param, 4);
        s_gpu.composite_set = 1;
        break;

    /* Depth. */
    case 0x030C: s_gpu.depth_test = param; break; /* DEPTH_TEST_ENABLE */
    case 0x0354: s_gpu.depth_func = param; break; /* DEPTH_FUNC */
    case 0x0394:                                  /* SET_CLIP_MIN: the near plane */
    case 0x0398:                                  /* SET_CLIP_MAX */
        break;                                    /* read from s_reg */
    case 0x035C: s_gpu.depth_mask = param; break; /* DEPTH_MASK */
    case 0x0214: s_gpu.zeta_offset = param; break;/* SURFACE_ZETA_OFFSET */
    case 0x1D8C: s_gpu.zstencil_clear = param; break; /* ZSTENCIL_CLEAR_VALUE */

    default:
        if (method >= 0x0260 && method < 0x0280) {         /* ALPHA_ICW(i) */
            s_gpu.rc.alpha_icw[(method - 0x0260) / 4] = param;
            break;
        }
        if (method >= 0x0AC0 && method < 0x0AE0) {         /* COLOR_ICW(i) */
            s_gpu.rc.color_icw[(method - 0x0AC0) / 4] = param;
            break;
        }
        if (method >= 0x1E40 && method < 0x1E60) {         /* COLOR_OCW(i) */
            s_gpu.rc.color_ocw[(method - 0x1E40) / 4] = param;
            break;
        }
        if (method >= 0x0AA0 && method < 0x0AC0) {         /* ALPHA_OCW(i) */
            s_gpu.rc.alpha_ocw[(method - 0x0AA0) / 4] = param;
            break;
        }
        if (method >= 0x0A60 && method < 0x0A80) {         /* FACTOR0(i) */
            s_gpu.rc.factor0[(method - 0x0A60) / 4] = param;
            break;
        }
        if (method >= 0x0A80 && method < 0x0AA0) {         /* FACTOR1(i) */
            s_gpu.rc.factor1[(method - 0x0A80) / 4] = param;
            break;
        }
        /* SET_VIEWPORT_OFFSET / _SCALE. The GPU keeps these in the constant
         * file, at c[59] and c[58] -- exactly where the D3D epilogue reads
         * them -- so they are constants that happen to arrive by method.
         * Dropped, every vertex program put its whole batch at the origin:
         * no 3D anywhere, and a black car on the garage screen. */
        if (method >= 0x0A20 && method < 0x0A30) {
            nv2a_vsh_constant_component(59, (method - 0x0A20) / 4, param);
            break;
        }
        if (method >= 0x0AF0 && method < 0x0B00) {
            nv2a_vsh_constant_component(58, (method - 0x0AF0) / 4, param);
            break;
        }
        /* The rest of the transform context that shares the constant file:
         * SET_FOG_PLANE is c[57] and SET_EYE_POSITION c[56]. Vertex programs
         * read them like any constant -- SSX 3's shadow-caster program takes
         * its colour from c[57] -- and dropped, they read whatever an earlier
         * constant upload left there: half its shadow maps came out opaque. */
        if (method >= 0x09D0 && method < 0x09E0) {         /* SET_FOG_PLANE */
            nv2a_vsh_constant_component(57, (method - 0x09D0) / 4, param);
            break;
        }
        if (method >= 0x0A50 && method < 0x0A60) {         /* SET_EYE_POSITION */
            nv2a_vsh_constant_component(56, (method - 0x0A50) / 4, param);
            break;
        }
        if (method >= 0x0B00 && method < 0x0B80) {    /* TRANSFORM_PROGRAM(i) */
            nv2a_vsh_program_word(param);
            break;
        }
        if (method >= 0x0B80 && method < 0x0C00) {    /* TRANSFORM_CONSTANT(i) */
            nv2a_vsh_constant_word(param);
            break;
        }
        if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST)
            record_tex_reg(method, param);
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            /* Resolved here, once, so every consumer -- the rasteriser's
             * attribute reads and the diagnostics alike -- sees the same
             * address. A vertex array offset is a DMA-object offset exactly
             * like a surface offset: physical, and addressable only through
             * the window when it names contiguous memory. */
            s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset =
                dma_resolve(param);
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            if (a->size == 7)                   /* SIZE_3W */
                a->size = 3;
            a->stride = (param >> 8)  & 0xFF;
        } else if (!imm_vertex_method(method, param)) {
            note_unhandled(method, param);
        }
        break;
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

/* Wall-clock milliseconds for the report, without the platform library (the
 * executor tests link this file on its own). */
static unsigned long report_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000ul + (unsigned long)(ts.tv_nsec / 1000000);
#endif
}

/* The report's RECOMP_FB_DUMP picture. The report runs whenever its timer
 * says, usually in the middle of a frame: dumping the surface then showed
 * whatever part of the frame was drawn so far (Fable's grass on black, the
 * sky and the rest of the world still to come). So the dump is left to the
 * next FLIP_STALL, which writes the frame it presents. A title that has never
 * flipped, or has stopped flipping since the previous report, gets the
 * surface as it is. */
static void fb_dump_report(void)
{
    if (s_flip_dumping)
        return;                           /* every flip is dumped already */
    if (s_gpu.flips && !s_dump_at_flip) {
        s_dump_at_flip = 1;
        return;
    }
    s_dump_at_flip = 0;
    dump_surface_bmp();
}

void nv2a_pb_exec_report(void)
{
    {
        char buf[256];
        int t, o = 0;
        for (t = 0; t < 32; t++)
            if (s_swm_ignored[t] && o < (int)sizeof buf - 24)
                o += snprintf(buf + o, sizeof buf - o, " %X:%u", t, s_swm_ignored[t]);
        if (s_swm_serviced || o)
            fprintf(stderr, "[GPU] software methods: %u serviced by the title;"
                            " not serviced (type:count)%s\n",
                    s_swm_serviced, o ? buf : " none");
    }
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    {
        /* Frames per second, from flips between reports, over wall time.
         * clock() was process CPU time: with the rasteriser on six cores it
         * ran six times faster than the wall, and 11 flips a second read as
         * "1.9 fps". */
        static uint32_t last_flips;
        static unsigned long last_ms;
        unsigned long now = report_now_ms();
        if (last_ms && now > last_ms)
            fprintf(stderr, "[GPU] %.2f fps (%u flips)%c",
                    (s_gpu.flips - last_flips) * 1000.0 / (now - last_ms),
                    s_gpu.flips, 10);
        last_flips = s_gpu.flips;
        last_ms = now;
        if (s_render_frames) {
            fprintf(stderr, "[GPU] render %.2f ms/frame avg, %.2f max over %u frames (%s)%c",
                    (double)s_render_sum_ns / s_render_frames / 1e6,
                    (double)s_render_max_ns / 1e6, s_render_frames,
                    backend_draws() ? "back end" : "CPU rasteriser", 10);
            if (s_gpu.reports && backend_draws())
                fprintf(stderr, "[GPU] visibility reports: %s, %u written (%u when idle,"
                                " %u at the flip; %u waited on in %u waits, %u flushes, %u"
                                " answered lazily), %u pending; idle hook %.2f ms/frame%c",
                        zpass_mode() == ZPASS_DEFER ? "deferred"
                        : zpass_mode() == ZPASS_LAZY ? "lazy"
                        : zpass_mode() == ZPASS_ALWAYS ? "always 1" : "synchronous",
                        s_zp_resolves, s_zp_res_idle, s_zp_res_flip, s_zp_blocks,
                        s_zp_waits, s_zp_flushes, s_zp_lazy, s_zp_count,
                        (double)s_zp_idle_ns / s_render_frames / 1e6, 10);
            s_zp_resolves = s_zp_blocks = 0;
            s_zp_res_idle = s_zp_res_flip = s_zp_flushes = s_zp_lazy = s_zp_waits = 0;
            s_zp_idle_ns = 0;
            s_render_sum_ns = s_render_max_ns = 0;
            s_render_frames = 0;
        }
    }
    flip_hold_report();
    flip_cap_report();
    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    fb_dump_report();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] brightest pixel written 0x%08X\n", s_gpu.pixel_max);
    fprintf(stderr, "[GPU] %llu pixels written; draw surface 0x%08X"
                    " -> 0x%08X, clear surface 0x%08X -> 0x%08X\n",
            (unsigned long long)s_gpu.pixels, s_gpu.drawn_offset,
            dma_resolve(s_gpu.drawn_offset), s_gpu.color_offset,
            dma_resolve(s_gpu.color_offset));
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen);
    fprintf(stderr, "[GPU] vertex programs: %u batches, %u vertices;"
                    " %u triangles dropped behind the eye\n",
            s_gpu.batches_program, s_gpu.verts_program, s_gpu.tris_behind);
    if (s_gpu.batches_ffp)
        fprintf(stderr, "[GPU] fixed-function transform: %u batches\n",
                s_gpu.batches_ffp);
    fprintf(stderr, "[GPU]   of the rest: %u degenerate/NaN, %u off-surface,"
                    " %u drawn; pixels %llu written, %llu depth-failed;"
                    " x %.0f..%.0f y %.0f..%.0f z %g..%g\n",
            s_gpu.xf_degenerate, s_gpu.xf_offscreen, s_gpu.xf_drawn,
            (unsigned long long)s_gpu.xf_pixels,
            (unsigned long long)s_gpu.xf_depth_fail,
            s_gpu.xf_min[0], s_gpu.xf_max[0], s_gpu.xf_min[1], s_gpu.xf_max[1],
            s_gpu.xf_min[2], s_gpu.xf_max[2]);
    {
        fprintf(stderr, "[GPU]   blend pairs (src/dst):");
        for (j = 0; j < s_gpu.blend_npairs; j++)
            fprintf(stderr, " %X/%X", s_gpu.blend_pairs[j] >> 16,
                    s_gpu.blend_pairs[j] & 0xFFFF);
        fputc(10, stderr);
        fprintf(stderr, "[GPU]   program attribute formats (slot:type/size):");
        for (j = 0; j < s_gpu.xf_nfmt; j++)
            fprintf(stderr, " v%u:%u/%u", s_gpu.xf_fmt[j] >> 8,
                    s_gpu.xf_fmt[j] & 15, (s_gpu.xf_fmt[j] >> 4) & 15);
        fprintf(stderr, "\n");
    }

    /* And of the batches that did rasterise, how many sampled anything. A menu
     * that draws its background from one texture and its text from another
     * shows both as flat colour if either half is missing, so the split is
     * what says which half. */
    fprintf(stderr, "[GPU] batches: %u textured, %u with no texcoords,"
                    " %u with texcoords but no usable stage\n",
            s_gpu.batches_textured, s_gpu.batches_no_uv, s_gpu.batches_no_tex);
    for (i = 0; i < s_tex_use_count; i++)
        fprintf(stderr, "  [TEXUSE] 0x%08X %ux%u fmt 0x%02X%s: %u batches\n",
                s_tex_use[i].offset, s_tex_use[i].width, s_tex_use[i].height,
                s_tex_use[i].color,
                d3d8_format_dxt_block_bytes(s_tex_use[i].color) ? " dxt"
                    : d3d8_format_is_swizzled(s_tex_use[i].color) ? " swz" : " lin",
                s_tex_use[i].batches);

    if (getenv("RECOMP_TEX_STATE")) {
        uint32_t k;
        for (k = 0; k < sizeof s_tex_set / sizeof s_tex_set[0]; k++)
            if (s_tex_set[k])
                fprintf(stderr, "  [TEX] 0x%04X = 0x%08X\n",
                        (unsigned)(NV_TEX_FIRST + k * 4), s_tex_reg[k]);
    }

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm.
     *
     * RECOMP_PB_UNHANDLED_ALL lists every one instead. Ten is the right
     * default -- the tail is a long list of state registers nobody needs to
     * read -- but when a title stops and the question is which method it
     * stopped on, the answer is as likely to be the one seen twice as the
     * one seen a thousand times, and ten hides it. */
    {
        int shown = getenv("RECOMP_PB_UNHANDLED_ALL") ? s_unhandled_count : 10;
    for (i = 0; i < shown && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        {
            /* The value as well as the count. A method nobody decoded is
             * a guess until you see what it carried: screen coordinates,
             * a 0..1 texcoord and a packed colour are told apart at a
             * glance, and that is what says which vertex encoding a title
             * is using. */
            union { uint32_t u; float f; } v;
            v.u = s_unhandled[i].last_param;
            fprintf(stderr, "  [GPU]   0x%04X x%-8u last=0x%08X (%.4f)\n",
                    s_unhandled[i].method, s_unhandled[i].count,
                    v.u, v.f);
        }
    }
    }
    for (i = 0; i < s_unhandled_count; i++)       /* the sort moved them */
        s_unhandled_slot[(s_unhandled[i].method & 0x1FFC) / 4] = (uint16_t)(i + 1);
    fflush(stderr);
}
