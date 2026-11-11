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
| `src/gpu/nv2a_gl_context.c` | Headless context: CGL (macOS), EGL pbuffer or surfaceless (Android, Linux); loss detection. |
| `src/gpu/nv2a_gl_api.h` | GL headers per platform: `OpenGL/gl3.h`, `GLES3/gl3.h`, libepoxy. |
| `tests/nv2a_gl_shader` | 400 random combiner programs through the generated shaders, each pixel compared with `nv2a_rc_eval`. |

One code path for desktop GL 3.3 core / macOS 4.1 core and GLES 3.0: the
GLSL is the common subset of `#version 330 core` and `#version 300 es`, and
the version line is chosen at run time (`nv2a_gl_glsl_header`). What the two
do not share is asked for at run time: S3TC (`GL_EXT_texture_compression_s3tc`),
depth clamp (core on desktop, `GL_EXT_depth_clamp` on GLES, else clamped in
the vertex shader), border clamp (core on desktop and GLES 3.2,
`GL_EXT/OES_texture_border_clamp` before), exact occlusion counts
(`GL_SAMPLES_PASSED` on desktop, `GL_ANY_SAMPLES_PASSED` on GLES). The
start-up line `[GPUGL] S3TC on|off, depth clamp on|off, border clamp on|off`
says which.

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

## Frame cap

A menu frame costs the GL back end 2–3 ms, and a front end that does not pace
itself (Fable's title, menus and pause screen) then flips 200–280 times a
second: the GPU and two cores busy for frames nobody sees, which on a
handheld is battery and heat. `RECOMP_GPU_FPS_CAP` (default 60 on macOS and
Android, 0 = off; off by default elsewhere, where a game project's own back
end may present with vsync) holds a back end that draws to that many flips
per second of host time:

- `FLIP_STALL`, after the present sync, the window snapshot and the back
  end's `flip`, books the frame's slot on an absolute schedule 1/cap s apart
  (`flip_cap_book`, nv2a_pb_exec.c);
- the executor sleeps until the slot before it walks the next segment
  (`flip_cap_wait`, from `nv2a_pb_exec_flip_poll` at the top of the ack
  loop). The segment holding the flip finishes first, so `DMA_GET` reaches
  `PUT` and the title builds its next frame while the executor sleeps;
- absolute, so a sleep that wakes late (macOS wakes an ordinary thread's
  nanosleep 3–5 ms late) shortens the next one and the average stays at the
  cap without spinning; a frame later than a whole period restarts the
  schedule instead of letting the next frames run uncapped to catch up.

It is independent of `RECOMP_FLIP_PACING` (the flip hold on the guest
vblank clock), which still applies after it. The `[GPU] frame cap` report
line gives the waits, the time slept, how late the sleeps woke, and how
many frames took longer than a period (the title, not the cap, was the
limit for those). `nv2a_pb_exec_flip_cap_until_ns()` (nv2a_flip_hold.h)
returns the wake time while the executor sleeps, so a guest wait loop that
polls GPU progress can sleep until then rather than spin.

Measured on the M1 Max, Fable title screen, `sample` of the executor (ack)
thread, back to back on a shared machine (load 13–15):

| | fps | executor on CPU | of which yield polling | process (top) |
|---|---|---|---|---|
| `RECOMP_GPU_FPS_CAP=0` | 143–240 | 96 % | 60 % | 146 % |
| default (60) | 59.8 (quiet machine), 40 under load | 54 % | 36 % | 98 % |

Draw work drops from 36 % to 18 % of a core. The rest is the ack loop's
`Sleep(0)` polling between segments and the guest's own spin in
`sub_00855520` (it waits for `DMA_GET == DMA_PUT` and an idle PGRAPH),
both outside the flip point: blocking the ack thread on `PUT` changes and a
yield in that wait are separate work (WP5). Under load, the title's own
frame time, not the cap, held the capped run at 40 fps.

### Context loss and GL_OUT_OF_MEMORY

At every flip `gl_check_frame` drains `glGetError` and asks
`nv2a_gl_context_lost()` (EGL with `EGL_EXT_create_context_robustness`:
`glGetGraphicsResetStatus`; any host: the context is no longer current). On a
lost context, `GL_CONTEXT_LOST`, or `GL_OUT_OF_MEMORY`, `gl_reset_objects`
forgets every GL object (deleting them first when the context still lives):
render targets, depth buffers, textures, programs, queries, the VAO/VBO and
the vertex shader. A lost context is destroyed; the next call re-creates
context and objects lazily through `ensure_init`, and render targets reseed
from guest memory, which the present sync has just brought up to date.
After four resets the back end unregisters and the CPU rasteriser draws.
A new cache of GL names (query ring, depth textures) belongs in
`gl_reset_objects`. `RECOMP_GPU_FAKE_RESET=<frame>[,oom]` exercises the path:
on the title screen both variants re-create everything and the next frames
dump the full title screen.

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
- Address mode 4 with a border colour clamps to the edge on GLES 3.0/3.1
  without `GL_EXT_texture_border_clamp` / `GL_OES_texture_border_clamp`.
- Without depth clamp (GLES lacking `GL_EXT_depth_clamp`) z is clamped per
  vertex in the vertex shader, which changes the depth slope of a triangle
  crossing the zeta range.
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
3. Flip pacing: done for the uncapped case (`RECOMP_GPU_FPS_CAP`, above); a
   title that paces on vblank still wants `RECOMP_FLIP_PACING`.
4. Device: profile on the AYN Thor (Adreno), check S3TC availability (if
   absent, DXT is decoded on the CPU once per upload), driver shader
   compile times.

## Android

What already works without change: a second EGL context on the executor
thread on `eglGetDisplay(EGL_DEFAULT_DISPLAY)`, the display SDL uses (Android's
`eglInitialize` is reference-counted, and the back end never calls
`eglTerminate`); a 1×1 pbuffer (Adreno offers RGBA8888 pbuffer configs with
`EGL_OPENGL_ES3_BIT`); pause and resume (SDL re-creates only its own surface,
and `recomp_lifecycle` holds the flip while paused); the companion panel
(Java `Presentation` views, no GL of ours); GLESv3 and EGL from the NDK.

The context (`nv2a_gl_context.c`):

- `eglCreatePbufferSurface` is checked. Without a pbuffer (or a pbuffer
  config) the context is made current with no surface where
  `EGL_KHR_surfaceless_context` exists; otherwise creation fails cleanly,
  every EGL object is released and the CPU rasteriser draws.
- `EGL_IMG_context_priority`: a high-priority context when offered
  (`RECOMP_GPU_PRIORITY=0` keeps the driver default); the level the driver
  granted is logged. `EGL_EXT_create_context_robustness`: lose-context-on-
  reset, so a GPU reset is seen at the next flip. Either attribute is dropped
  again if the driver refuses the context with it.
- `[GPUGL] EGL context: 1x1 pbuffer|surfaceless, priority high|medium|default,
  reset notification on|off` is logged at creation; `RECOMP_GPU_VERBOSE=1`
  adds the EGL version, vendor and extension list.

The shaders: the GLSL ES header declares `precision highp` for `sampler2D`,
`samplerCube` and `sampler2DShadow` (ES 3.00 defaults samplers to lowp); depth
clamp and border clamp fall back as listed above. Occlusion counts on GLES
are "any sample passed" (1), enough for Fable's visibility tests.

Settings: the launcher's Graphics section offers **Renderer** (Auto /
Software / GPU, `RECOMP_GPU`) and **Frame cap (GPU renderer)** (30 / 60 /
Off, `RECOMP_GPU_FPS_CAP`), written to `recomp.cfg`
([app-shell.md](../android/app-shell.md#settings)).

Not default on Android yet. The in-world blockers are not in this back end's
context code: 341 synchronous visibility queries and mid-frame readbacks per
frame each end the render pass on a tiler (deferred reports, G2, is the gate),
and about 115 MB/frame of `glBufferData` of CPU-transformed vertices (vertex
programs in GLSL, G3). A title profile turns it on
(`SETENV_DEFAULT("RECOMP_GPU", "1")`) once those are in and the device check
below passes.

### Device check (AYN Thor, Adreno 740), for Alex

1. Settings › Graphics › Renderer: GPU. `adb shell cat
   /sdcard/Android/data/<app id>/files/recomp.cfg` shows `RECOMP_GPU=1`;
   add `RECOMP_GPU_VERBOSE=1` by hand.
2. Start the game. logcat (tag `xboxrecomp`): `[GPUGL] EGL ... extensions:`
   (note whether `EGL_KHR_surfaceless_context`, `EGL_IMG_context_priority`,
   `EGL_EXT_create_context_robustness` are listed), `[GPUGL] EGL context:
   1x1 pbuffer, priority high, reset notification on` (or what it says),
   the renderer line (`Adreno (TM) 740 | OpenGL ES 3.2 ...`),
   `[GPUGL] S3TC on|off, depth clamp on|off, border clamp on|off`. No
   `shader compile failed`, `link failed` or `GL error` lines.
3. Title screen: the picture is complete; `[GPU] ... fps` reads about 60 and
   `[GPU] frame cap 60: waits ...` appears every report.
4. Settings › Frame cap: Off. Title screen fps now well above 60 (Mac: 200+);
   back to 60. Then 30: about 30 fps.
5. Start, AutoSave, Oakvale: picture and fps (in-world GL is slow before
   G2/G3: expect a few fps); repeat once with `RECOMP_GPU_ZPASS=always` in
   recomp.cfg once deferred reports land.
6. Home for 30 s, then back: the game resumes, picture intact, no
   `context lost` line (or, if there is one, the picture comes back within
   a frame).
7. Screen off, 10 s, on: same.
8. The companion panel (bottom screen) still updates.
9. Pause menu for 10 minutes at the 60 cap: device temperature and battery
   drop, compared with Renderer: Software and with Frame cap: Off.
10. `adb shell top -H -p $(pidof <app id>)` on the pause menu: the executor
    thread's share at cap 60 vs Off; and in-world.
11. Settings › Renderer: Auto. `RECOMP_GPU` is gone from recomp.cfg and the
    software renderer draws (no `[GPUGL]` lines).
