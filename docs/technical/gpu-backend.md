# GPU Back End (XBA-27)

The pushbuffer executor ([pushbuffer-executor.md](pushbuffer-executor.md))
renders on the CPU by default. In a 3D scene that is the frame-rate limit: on
Fable in-world, 41 % of the executor's time goes to the vertex-program
interpreter and 47 % to the rasteriser, on one thread. The GPU back end moves
the per-pixel half (rasteriser, texturing, register combiners, depth, stencil,
blending) to OpenGL. `RECOMP_GPU=1` selects it; the CPU rasteriser stays the
default and is unchanged.

Stages: **7a** interface (done), **7b** GL back end with CPU-transformed
vertices (done), **7c** vertex programs in GLSL, **7d** surfaces and
readback, **7e** presenting from the GPU. The last section lists what 7c–7e
have to do.

## Layout

| File | Role |
|---|---|
| `src/kernel/nv2a_backend.h` | The interface. `NV2A_BACKEND_CAP_PIXEL` gives a back end the full vertex (`Nv2aVertexFull`) and the pixel state (`Nv2aPixelState`) with every batch; `zpass_read` returns visibility-test counts. |
| `src/gpu/nv2a_gl.c` | The back end: render targets, texture cache, state, draws, readback, queries. |
| `src/gpu/nv2a_gl_shader.c` | Combiner program → fragment shader, one per pipeline key. No GL calls. |
| `src/gpu/nv2a_gl_context.c` | Headless context: CGL (macOS), EGL pbuffer (Android, Linux). |
| `src/gpu/nv2a_gl_api.h` | GL headers per platform: `OpenGL/gl3.h`, `GLES3/gl3.h`, libepoxy. |
| `tests/nv2a_gl_shader` | 400 random combiner programs through the generated shaders, each pixel compared with `nv2a_rc_eval`. |

One code path for desktop GL 3.3 core / macOS 4.1 core and GLES 3.0: the
GLSL is the common subset of `#version 330 core` and `#version 300 es`, and
the version line is chosen at run time (`nv2a_gl_glsl_header`). What the two
do not share is asked for at run time: S3TC (`GL_EXT_texture_compression_s3tc`),
depth clamp (core on desktop, `GL_EXT_depth_clamp` on GLES), exact occlusion
counts (`GL_SAMPLES_PASSED` on desktop, `GL_ANY_SAMPLES_PASSED` on GLES).

## Threading

The executor calls `clear`, `draw`, `flip` and the present sync on its own
thread, one call at a time. GL contexts are bound to a thread, so the back
end's context is **created on the executor thread at its first call and stays
current there**. It has no window (CGL headless context, EGL pbuffer); it
renders into framebuffer objects only. No other thread makes a GL call.

The SDL presenter keeps its own renderer on the main thread (Metal on macOS,
GLES on Android) and is fed exactly as with the CPU rasteriser: through guest
memory. At `FLIP_STALL` the executor calls the back end's present sync before
it snapshots the frame for the window and for `RECOMP_FB_DUMP`; the present
sync reads every surface the GPU drew that frame back into guest memory. So
the presenter, the perf overlay, frame dumps and the Android surface need no
change, and the CPU and GPU paths can be compared frame against frame.

Rejected for 7b: a command queue to a GL thread (one more copy of every
batch, and the executor would have to wait for readbacks anyway); a context
shared with the presenter (SDL's renderer owns its context, is Metal on
macOS, and sharing with it is stage 7e's problem, not a prerequisite).

`RECOMP_GPU=1` is read by `xbox_HostWindowMain` (fb_present_sdl.c) before the
guest starts, windowed or headless, which registers the back end; the
executor must be on (`RECOMP_PB_EXEC`). If the context cannot be created the
back end unregisters itself at its first call and the CPU rasteriser draws.

## What the back end does

**Vertices.** The executor still transforms: vertex programs run in
`nv2a_vsh_interp.c`, fixed-function vertices in `ffp_vertex`, and triangles
are clipped at the near plane. The back end gets surface-pixel x/y, window z,
clip-space w, both colours, the fog factor and four texture-coordinate sets
per vertex, already as a triangle list. The vertex shader maps them back to
clip space (`(x·2/W − 1)·w`, …, `w`), so the GPU interpolates with 1/w as
the CPU rasteriser interpolates texture coordinates, and `Nv2aVertexFull` is
uploaded as it is (one `glBufferData` per batch).

**Render targets.** One RGBA8 texture + FBO per colour surface address and
shape (pitch, bpp, linear or swizzled), up to 16, least recently drawn
evicted. Created from the guest bytes (a title may draw over a picture the
CPU put there). Rows map one to one, guest row y = GL row y, so nothing is
flipped. Depth/stencil is a D24S8 renderbuffer per zeta address (up to 8),
attached to whichever colour target draws with it, as the CPU rasteriser
keeps one buffer per zeta address. A 16-bit surface keeps alpha at 1 (colour
mask), as the CPU path reads 565 back with alpha 0xFF. Pixels go back to
guest memory (in the surface's own format and swizzle) at the present sync
and when a batch samples memory a dirty target covers (render to texture).

**Textures.** Cached by address, format, size, pitch, palette and cube-ness
(512 entries; the cache is flushed whole when three quarters full). A 64-bit
hash of the guest bytes (all mip levels and faces, plus the palette for P8)
decides re-upload; it is checked once per frame and whenever a texture unit
switches to the texture, so a glyph cache written between two text batches
is seen (`RECOMP_GPU_TEX_RECHECK=frame` checks once per frame only). DXT
goes up compressed when S3TC exists (`RECOMP_GPU_S3TC=0` turns it off);
every other format is decoded once by the executor's own sampler through
`nv2a_backend_decode_texture`, so the CPU and GPU paths cannot disagree on a
format. Level 0 only, like the CPU rasteriser. Filtering follows the CPU
rasteriser's choice: combiner batches honour the title's TENT filters,
the two simpler colour rules take the nearest texel unless
`RECOMP_TEX_BILINEAR`/`RECOMP_TEX_ANISO`.

**Pixels.** `nv2a_gl_shader.c` writes a fragment shader per pipeline key: the
combiner words, stage count and flags, stage-program modes, which stages hold
a texture of which kind, the alpha-test function. Combiner factors, the final
combiner's constants, fog colour, alpha reference and texture-coordinate
scales are uniforms, so a title animating a factor does not compile shaders.
The generator transcribes `nv2a_rc_eval` (input mappings, dot products, mux,
output scale/bias, the final combiner) and `rc_stage_fetch` (2D/3D projective,
cube, pass-through, clip plane; bump and dot-product modes read zero, as on
the CPU). The CPU rasteriser's two other colour rules are kept: stage 0 ×
diffuse before a title programs the combiners, and the screen-space path's
texel-or-flat-colour without depth or alpha test (`Nv2aPixelState.shade`).
Blend factors, equations, depth and stencil functions arrive in GL's own
enum values and are passed through after a range check.

**Visibility tests.** Batches drawn while `SET_ZPASS_PIXEL_COUNT_ENABLE` is
on are wrapped in occlusion queries; `GET_REPORT` reads them synchronously
through `zpass_read`. GLES 3.0 only says whether any sample passed, which
reports 1.

## Verification (2026-11-11, M1 Max, Fable title screen)

Back to back, 45 s each on the title screen, `[GPU] render … ms/frame` is the
executor's wall time per frame spent drawing (batches including the CPU
vertex-program run, clears, present sync):

| Path | render ms/frame (avg) | fps (uncapped, no flip pacing) |
|---|---|---|
| CPU rasteriser | 71–93 | 10.5–13.8 |
| GL back end | 2.1–2.6 | 225–278 |

(Three other builds shared the machine; the CPU numbers are pessimistic.)
Frames dumped with `RECOMP_FB_DUMP_FLIPS` and compared pixel by pixel: the
"Press START" title screen at the same animation phase differs by 0.34/255
mean per channel (no pixel off by more than 24); the "Saving game
(AutoSave)" screen after Start by 0.33/255 mean, 0.01 % of pixels above 24,
which is the progress bar's moving end. The rest is ±1–2 rounding (bilinear
weights, S3TC decode). `tests/nv2a_gl_shader`: 400 random combiner programs,
0 pixels off by more than 2/255.

Past the AutoSave screen both paths stop flipping: that is the pushbuffer
stall the jump-following and software-method work (WP2/WP3) removes, not the
back end. In-world rendering is untested.

## Known differences from the CPU rasteriser (7b)

- Colours and fog are interpolated with perspective (GL's default); the CPU
  rasteriser interpolates them linearly in screen space. GLES 3.0 has no
  `noperspective`; only large perspective triangles with colour gradients
  differ.
- Stencil is honoured on the GPU and ignored by the CPU rasteriser.
- Window z is clamped to the zeta format's range (depth clamp); the CPU keeps
  unbounded floats. `SET_CLIP_MIN/MAX` are ignored by both. Float zeta
  formats are treated as integer.
- Address mode 4 with a border colour clamps to the edge on GLES (no
  `CLAMP_TO_BORDER` in GLES 3.0).
- No culling, like the CPU rasteriser (strips are emitted without
  alternating winding unless `RECOMP_STRIP_ALT_WINDING`).
- Points and lines are not drawn, like the CPU rasteriser.
- A guest CPU write into a surface after the GPU created its target is not
  seen (FMV written straight into the framebuffer, a CPU-drawn loading bar):
  nothing tracks those writes yet.

## What is left

### 7c: vertex programs on the GPU

The interpreter is 41 % of the in-world executor. Needed:

1. Interface: a batch carrying raw attribute streams instead of transformed
   vertices: the 16 `VertexAttr` (offset, type, size, stride, or the
   immediate value for size 0), the index list (or inline/immediate data),
   the program (136 slots × 4 words, start slot) with a dirty flag, and the
   192 constants with dirty ranges. Capability bit, e.g.
   `NV2A_BACKEND_CAP_PROGRAM`; the executor keeps transforming for back ends
   without it and for fixed-function batches until those move too.
2. Translator `nv2a_vsh_interp.c` encoding → GLSL per program hash (MAC/ILU
   pairs, swizzles, masks, `a0.x` relative addressing, `c[-96..95]` D3D
   offset, `oPos`/`oD0`/`oD1`/`oFog`/`oPts`/`oB0`/`oB1`/`oT0..3`). xemu's
   `vsh.c` is the reference, as `psh.c` was for the combiners.
3. The program's `oPos` is already screen space (D3D appends the viewport and
   the divide by w). The vertex shader already maps screen space back to clip
   space (`(x·2/W − 1)·w`); applied to `oPos` it lets GL clip at w > 0 and
   0 ≤ z ≤ zmax, which is the executor's near-plane clip. `oPos.w == +0`
   becomes 1 as in `transform_vertex`; the anti-aliasing scale multiplies x/y.
4. Attribute fetch on the GPU: vertex arrays uploaded from guest memory by
   range, cached with the same hash scheme as textures (or first, gathered
   per batch on the CPU into one VBO, which already removes the interpreter).
   Formats: float, D3DCOLOR (BGRA swizzle), normalised and plain shorts,
   11:11:10 normals, unsigned bytes.
5. Fog: the CPU computes the fog factor per vertex (`fog_factor`); move it
   into the vertex shader from `oFog.x` and the fog mode/parameters.
6. Fixed-function T&L (`ffp_vertex`, `lit_color`) either as a second
   generated vertex shader or kept on the CPU (menus only use it lightly).

### 7d: surfaces, readback, reports

1. Render to texture without readback: sample the target's texture directly
   when a stage's texture address and format match a target (linear: direct;
   swizzled: the target is stored linear, so the texture coordinates are the
   same); read back only for guest CPU reads.
2. Guest CPU writes into surfaces: hash the surface's guest bytes against
   the last readback before drawing into it, or write-protect the pages, and
   re-upload when they changed.
3. Present sync reads back every dirty target every flip (≈1 ms at 640×480).
   Read back the presented surface only (and any surface a texture or the
   guest needs), or none once 7e presents from the GPU.
4. Depth surfaces read as textures (shadow maps): a depth texture instead of
   a renderbuffer, Z16/Z24 readback into guest memory when sampled.
5. Anti-aliased surfaces (2×1, 2×2): render at the real size as today;
   resolve on present.
6. Asynchronous visibility queries: answer `GET_REPORT` when the result is
   ready instead of stalling the pipeline (D3D polls the report slot).
7. `SET_CLEAR_RECT` for swizzled surfaces (clears use the clip rectangle).
8. Mip chains (upload every level, GL mip filters), which the CPU path never
   had either.

### 7e: presenting from the GPU

1. Present the finished FBO texture directly. On Android the SDL renderer is
   GLES: create the executor context shared with a presenter-owned GL context
   (or an `EGLImage` of the FBO texture), draw it in the presenter, and drop
   the readback of the presented surface. On macOS the presenter would need
   the SDL GL renderer (today Metal) or an IOSurface-backed texture.
2. Keep `RECOMP_FB_DUMP` and the perf overlay working (dump reads back on
   demand).
3. Flip pacing: with the GPU the front end runs at 200+ fps uncapped (no
   flip hold); a title that paces on vblank wants `RECOMP_FLIP_PACING` or
   vsync in the presenter.
4. Device: profile on the AYN Thor (Adreno), check S3TC availability (if
   absent, DXT is decoded on the CPU once per upload), driver shader
   compile times.
