# GPU Back End (XBA-27)

The pushbuffer executor ([pushbuffer-executor.md](pushbuffer-executor.md))
renders on the CPU by default. In a 3D scene that is the frame-rate limit: on
Fable in-world, 41 % of the executor's time goes to the vertex-program
interpreter and 47 % to the rasteriser, on one thread. The GPU back end moves
the per-pixel half (rasteriser, texturing, register combiners, depth, stencil,
blending) to OpenGL. `RECOMP_GPU=1` selects it; the CPU rasteriser stays the
default and is unchanged.

Stages: **7a** interface (done), **7b** GL back end with CPU-transformed
vertices (done), **7c** vertex programs in GLSL (7c-lite done: programs in
GLSL, attributes gathered on the CPU), **7d** surfaces and readback, **7e**
presenting from the GPU. The last section lists what 7c–7e have to do.

## Layout

| File | Role |
|---|---|
| `src/kernel/nv2a_backend.h` | The interface. `NV2A_BACKEND_CAP_PIXEL` gives a back end the full vertex (`Nv2aVertexFull`) and the pixel state (`Nv2aPixelState`) with every batch; `zpass_read` returns visibility-test counts. |
| `src/gpu/nv2a_gl.c` | The back end: render targets, texture cache, state, draws, readback, queries. |
| `src/gpu/nv2a_gl_shader.c` | Combiner program → fragment shader, one per pipeline key. No GL calls. |
| `src/gpu/nv2a_gl_vsh.c` | Vertex program (nv2a_vsh_interp.c encoding) → GLSL vertex shader, one per program hash. No GL calls. |
| `src/gpu/nv2a_gl_context.c` | Headless context: CGL (macOS), EGL pbuffer or surfaceless (Android, Linux); loss detection. |
| `src/gpu/nv2a_gl_api.h` | GL headers per platform: `OpenGL/gl3.h`, `GLES3/gl3.h`, libepoxy. |
| `tests/nv2a_gl_shader` | 400 random combiner programs through the generated shaders, each pixel compared with `nv2a_rc_eval`. |
| `tests/nv2a_gl_vsh` | 600 random vertex programs plus ILU/a0 edge cases through the translator (transform feedback), compared with `nv2a_vsh_interp.c`; eight deliberate translator mutations must be caught. |

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

The executor calls `clear`, `draw`, `flip` and the present hook on its own
thread, one call at a time. GL contexts are bound to a thread, so the back
end's context is **created on the executor thread at its first call and stays
current there**. It has no window (CGL headless context, EGL pbuffer); it
renders into framebuffer objects only. No other thread makes a GL call.

The SDL presenter keeps its own renderer on the main thread (Metal on macOS,
GLES on Android) and is fed exactly as with the CPU rasteriser: through guest
memory. At `FLIP_STALL` the executor calls the back end's present hook
(`nv2a_backend_register_with_present`) before it snapshots the frame for the
window and for `RECOMP_FB_DUMP`, with the surface the flip presents. The
hook reads back that surface only -- see "Present" below. So the presenter,
the perf overlay, frame dumps and the Android surface need no change, and
the CPU and GPU paths can be compared frame against frame.

### Present

Only the presented surface is read back at a flip, and by default one frame
late: `glReadPixels` into a pixel pack buffer at flip N, mapped at flip N+1,
when the GPU has long finished frame N, and the window is handed frame N's
surface (whose bytes have just landed in guest memory) at flip N+1. The
executor is the bottleneck in-world (it walks frame N+1 while the GPU draws
frame N); a synchronous read made it wait at every flip for the GPU to
finish the frame, and on a tiler it also ends the render pass early. The
cost is one frame of display latency, invisible at 30 fps next to the
title's own input latency. A frame dump (`RECOMP_FB_DUMP_FLIPS`, the
periodic report dump) needs the frame it names in guest memory, so a flip
that dumps reads synchronously; so does every flip with
`RECOMP_GPU_PRESENT=sync`. A presented surface that was not drawn since its
last read-back is shown as it is.

Every other target stays on the GPU until something needs its bytes in
guest memory:

- a texture the GPU cannot sample it as (another format or shape, an
  offset inside it: "Render to texture" below) -- read back before the
  batch;
- its eviction (least recently drawn, 16 targets) or a change of its bytes
  per pixel;
- an out-of-memory reset of the GL objects (targets reseed from guest
  memory);
- `RECOMP_GPU_READBACK=all`: every target drawn since the last flip, at the
  flip, as before 7d -- for a title whose CPU reads its render targets
  (a save-game thumbnail read with `Lock`, a CPU post-process). Nothing
  traps guest CPU reads of a target: a title that reads one without
  sampling it on the GPU sees the bytes of the last read-back.

In Oakvale that is one read-back a frame (was 16 before 7d, 9 after direct
sampling). Measured on the M1 Max, back to back: the present costs 1.5 % of
the executor's samples asynchronously and 2.8 % with `RECOMP_GPU_PRESENT=sync`;
render time is the same within noise (19-20 ms/frame), because the
visibility reports resolved just before the present already wait for the
GPU there. Where reports do not drain the GPU first (a title without
visibility tests, `RECOMP_GPU_ZPASS=always`) and on a tiler, the
synchronous read is the one that waits for the whole frame.

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

- `FLIP_STALL`, after the present hook, the window snapshot and the back
  end's `flip`, books the frame's slot on an absolute schedule 1/cap s apart
  (`flip_cap_book`, nv2a_pb_exec.c);
- the executor sleeps until the slot before it walks the next segment
  (`flip_cap_wait`, from `nv2a_pb_exec_flip_poll` at the top of the ack
  loop). The segment holding the flip finishes first, so `DMA_GET` reaches
  `PUT` and the title builds its next frame while the executor sleeps;
- absolute, so a sleep that wakes late (macOS wakes an ordinary thread's
  nanosleep 3–5 ms late) shortens the next one and the average stays at the
  cap without spinning; a frame that took longer than a whole period is not
  held at all (its slot is the flip itself) and the schedule restarts from
  there, so slow frames (Fable in-world) pay no sleep and the next frames do
  not run uncapped to catch up.

It is independent of `RECOMP_FLIP_PACING` (the flip hold on the guest
vblank clock), which still applies after it. The `[GPU] frame cap` report
line gives the waits, the time slept, how late the sleeps woke, and how
many frames took longer than a period (not held: the title, not the cap,
was the limit for those). `nv2a_pb_exec_flip_cap_until_ns()` (nv2a_flip_hold.h)
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
from guest memory. On `GL_OUT_OF_MEMORY` (the context lives) every target
and the pending present read-back go to guest memory first; on a lost
context the targets not presented since their last draw lose what the GPU
held.
After four resets the back end unregisters and the CPU rasteriser draws.
A new cache of GL names (query ring, depth textures) belongs in
`gl_reset_objects`. `RECOMP_GPU_FAKE_RESET=<frame>[,oom]` exercises the path:
on the title screen both variants re-create everything and the next frames
dump the full title screen.

## What the back end does

**Vertex programs (7c-lite).** With `NV2A_BACKEND_CAP_PROGRAM` a batch drawn
with a vertex program reaches `draw_program` untransformed
(`Nv2aProgramBatch`): the executor gathers each unique vertex's attributes
once through its own `fetch_attr` (only the inputs the program reads, as
vec4s; the others as their current value), builds a 16-bit triangle index
list in the CPU path's order and winding, and passes the program words with a
hash and the 192 constants with per-constant change stamps. `nv2a_gl_vsh.c`
turns the program into a GLSL vertex shader (compiled once per hash), which
computes exactly what `nv2a_vsh_interp.c` does and then maps `oPos` as the
fixed vertex shader below maps a CPU-transformed vertex: x, y times the
anti-aliasing scale (the title's viewport is its own business: c[58]/c[59]
are not assumed), `oPos.w == +0` as 1, z measured from the near plane
`SET_CLIP_MIN` (0x0394). Desktop GL clips there with `gl_ClipDistance[0]`
and keeps depth clamp (nothing clips at the far plane, as on the CPU path);
GLES turns depth clamp off for these draws so GL's own near plane is there;
both use `glDepthRange(zmin/zmax, 1)` so window depth stays z/zmax. The fog
factor (`fog_factor`) and the colour clamp are computed in the shader. Each
linked program keeps its own copy of `uniform vec4 c[192]` and uploads only
the constants whose stamp is newer than its last sync. The executor skips
the interpreter, the near-plane clipper and the triangle expansion for these
batches; `RECOMP_GPU_VSH=0` keeps the CPU transform, and programs that write
c[] with `CXT_WRITE` on stay on the CPU (they change constants vertex by
vertex), as does any batch the back end declines (a shader that fails to
compile, a full cache).

**Vertices.** Otherwise the executor transforms: vertex programs run in
`nv2a_vsh_interp.c`, fixed-function vertices in `ffp_vertex`, and triangles
are clipped at the near plane. The back end gets surface-pixel x/y, window z,
clip-space w, both colours, the fog factor and four texture-coordinate sets
per vertex, already as a triangle list. The vertex shader maps them back to
clip space (`(x·2/W − 1)·w`, …, `w`), so the GPU interpolates with 1/w as
the CPU rasteriser interpolates texture coordinates, and `Nv2aVertexFull` is
uploaded as it is (one `glBufferData` per batch).

**Render targets.** One RGBA8 texture + FBO per colour surface address and
bytes per pixel, up to 16, least recently drawn evicted. Created from the
guest bytes (a title may draw over a picture the CPU put there). The GL
texture holds pixels by (x, y) whatever the guest layout, so a target is
kept when the title switches the same memory between swizzled and linear or
between sizes (Fable's bloom chain draws 0x80D38080 swizzled in one pass and
linear in the next, and 0x80CF9080 as 256x256 and 128x128 surfaces in
turn): only the layout that read-back and seeding convert changes, and a
shape that no longer fits grows the texture by a GPU copy. Each switch used
to cost a read-back and an upload (16 a frame in Oakvale). Only a change of
bytes per pixel recreates the target. Rows map one to one, guest row y = GL row y, so nothing is
flipped. Depth/stencil is a D24S8 texture per zeta address (up to 8),
attached to whichever colour target draws with it, as the CPU rasteriser
keeps one buffer per zeta address; a texture rather than a renderbuffer so
that a shadow map drawn into it can be sampled (below). A 16-bit surface keeps alpha at 1 (colour
mask). 16-bit surfaces go to and from guest memory in their own format:
R5G6B5 (colour format 3) or X1R5G5B5 (1 and 2; the X bit 0 or 1). Fable's
back buffer is X1R5G5B5 (`SET_SURFACE_FORMAT` 0x121) and its bloom samples
it as `LU_X1R5G5B5` (0x1C); the executor tells the presenter
(`xbox_FramebufferWindowSetFormat`, SDL texture `RGB555`) and the frame
dump the format when a back end with a present hook drew the frame. The
CPU rasteriser still writes R5G6B5 into every 16-bit surface, and its
window keeps reading R5G6B5. Pixels go back to
guest memory (in the surface's own format and swizzle) by the rules under
"Present" above.
A target is dirty only after a batch or clear that writes colour (colour
mask non-zero).

**Clear on bind (tilers).** A new target is not seeded from the guest
bytes at creation but at its first draw or sample (`rt_seed`); a clear of
every channel over the whole guest surface before that skips the upload.
Where the API has it (GLES 3.0, desktop GL 4.3 or
`GL_ARB_invalidate_subdata`; not macOS's 4.1) every clear of all colour
channels, or of depth and stencil together, first calls
`glInvalidateSubFramebuffer` on the cleared rectangle, so a tiler neither
loads nor keeps the old contents (`[GPUGL] ... invalidate on|off` at
start-up; `RECOMP_GPU_INVALIDATE=0` turns it off). The depth of a zeta
surface is not invalidated at the end of a pass: it may be sampled later as
a shadow map, and nothing says the title is done with it. A target drawn
without a zeta surface (`zeta_va` 0: Fable's bloom and noise chains, 16x16
to 256x256, about 15 passes a frame) still has a depth attached, the
scratch one of address 0; its contents mean nothing after the pass, so it
is invalidated (`glInvalidateFramebuffer`, depth/stencil attachment) when
the next draw or clear switches away (`leave_target`), and the tiler does
not store it. In Oakvale the
bloom targets are drawn without a clear, so the seed skip seldom applies
(2 of 14 creations in a run); the clears it invalidates are the frame's 3
(back buffer colour, main depth/stencil, shadow map depth).

**Shadow maps.** A combiner stage in PROJECT2D/PROJECT3D mode reading a
depth format (0x2A-0x31; Fable binds its 768x768 Z16 map as 0x30,
`LU_IMAGE_DEPTH_Y16_FIXED`, on stage 1 of about 150 batches a frame) at the
address of a zeta surface the GPU holds samples that depth texture as a
`sampler2DShadow` with `GL_TEXTURE_COMPARE_MODE`. The NV2A computes
"stored depth <func> r/q" with r/q clamped to the depth format's range
(0xFFFF for Y16, 0xFFFFFF for Y24) and `SET_SHADOW_DEPTH_FUNC` (0x1E6C) as
func (xemu `psh.c`, `psh_append_shadowmap`); PROJECT2D compares with 0.
GL compares "r <func> stored", so the back end sets the swapped function
(Fable's GREATER becomes `GL_LESS`: lit where r/q is nearer than the map)
and the shader passes r/q / range, matching the depth buffer's z/zmax. The
title's filter applies (bilinear gives GL's 2x2 percentage-closer
filtering, as the hardware does). Polygon offset (`SET_POLY_OFFSET_FILL_ENABLE`,
`SET_POLYGON_OFFSET_SCALE_FACTOR`/`_BIAS`, 0x0338/0x0384/0x0388; Fable's
shadow pass uses 3, 1) goes to `glPolygonOffset`, the units scaled by 256
on Z16 surfaces since the buffer is D24. A depth texture the GPU did not
draw (CPU-only zeta) reads white, as on the CPU path, which has no shadow
maps. `tests/nv2a_gl_shader` checks the compare functions and PROJECT2D on
a 2x1 depth texture.

**Render to texture.** A texture stage whose address is a colour target's
samples the target's GL texture directly (`rt_for_texture`): no read-back,
no hash, no upload. The format must read the target's pixels as they are --
A8R8G8B8 / X8R8G8B8 (swizzled or linear) on a 32-bit target, R5G6B5 on a
565 target, X1R5G5B5 / A1R5G5B5 on a 1555 one; the X variants read alpha 1
through `GL_TEXTURE_SWIZZLE_A`. A swizzled texture must cover the target
(the target is stored linear, so a normalised coordinate means the same
texel), a linear one must have the target's pitch; coordinates scale to the
GL texture's size. A batch that samples the target it draws into reads a
copy taken just before it (`break_feedback`; GL leaves the loop undefined).
Anything else -- another format (a 32-bit target read as 16-bit texels,
BGRA orders, P8), a texture at an offset inside a target, a cube map --
still reads the target back and uploads it as below. `RECOMP_GPU_RT_DIRECT=0`
turns direct sampling off. In Oakvale that removed every texture upload in
steady state (16 a frame, 3 MB, all bloom-chain targets) and 7 of the 16
read-backs a frame. A surface without colour (`Nv2aSurface.color_valid` 0:
colour offset 0, as in Fable's 768x768 shadow pass, or a surface that would
land on the image) draws into a depth-only framebuffer per zeta address
(`glDrawBuffers(GL_NONE)`); no colour target is created and nothing is read
back. Its depth clears reach the back end. A batch with a colour surface but
a colour mask of 0 (`Nv2aSurface.color_masked`: depth pre-passes, visibility
proxies) stays on that surface's colour target with `glColorMask(0,0,0,0)`
and does not make it dirty; only when no target exists for that memory yet
does it go to the depth-only framebuffer. Sent as "no colour" before,
Fable's masked batches on the back buffer switched to the depth-only
framebuffer of the same zeta and back 65-90 times a frame in Oakvale. The
statistics line ends with `target switches/frame` (framebuffer changes
between draws and clears, averaged over the line's period). A texture whose
upload failed (`GlTex.failed`, e.g. the Z16 shadow map bound as format
0x30) is drawn as "no texture" until its bytes change, instead of sampling
an incomplete GL texture as black.

**Textures.** Cached by address, format, size, pitch, palette and cube-ness
(512 entries; the cache is flushed whole when three quarters full). A 64-bit
hash of the guest bytes (all mip levels and faces, plus the palette for P8)
decides re-upload. It is computed once per frame per texture, and again
within the frame when a texture unit switches back to it only for a
volatile texture (one whose bytes changed in the last 60 frames, such as a
glyph cache written between two text batches). A texture overlapping a
zeta surface bound in the last two frames is never hashed (the GPU's depth
is not in guest memory; Fable's 1.2 MB shadow map was hashed every frame for
nothing); one overlapping a colour target drawn in the last two frames is not
hashed either but read back and re-uploaded when a target it overlaps was
written since its upload (render to texture). In-world on Fable that took
hashing from 14.9 % of the executor's samples to 0.5 %.
`RECOMP_GPU_TEX_RECHECK=frame` hashes once per frame only, `=switch` keeps
the old policy (every frame and every unit switch, no target rule). DXT
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

**State shadowing.** The GL state set per batch -- framebuffer, viewport,
scissor, program, vertex array and array buffer, blend/depth/stencil/cull/
polygon-offset state, write masks, the active unit and units 0-3's texture
bindings -- is kept as last set (`s_sh` in `nv2a_gl.c`), and a call that
would set what is already there is skipped. The vertex-program VAO keeps
its attribute layout while the streams mask is unchanged and sets the
generic values of stream-less attributes only when they change. Every
change of that state goes through the `sh_*` helpers; deleting a texture or
framebuffer forgets its bindings, and context creation and
`gl_reset_objects` forget everything.

**Program binary cache.** A linked program is written with
`glGetProgramBinary` to `<save dir>/shadercache/<title id>/<hash>.bin`
(the save dir is `$XDG_DATA_HOME/xboxrecomp` on desktop POSIX and the app's
internal `save/` on Android) and loaded with `glProgramBinary` at the next
start instead of compiled; Fable links about 70 programs by Oakvale, each
the first time a batch needs it, on the executor thread. The file name is a
64-bit hash of the program's whole GLSL (header, vertex and fragment
shader), so a change of a generator, a pipeline key or a vertex program
gives another file; vertex programs are compiled only when a program that
uses them has to be linked. The header holds a generator version,
`GL_RENDERER` and `GL_VERSION`: a file from another driver (or one the
driver refuses) is deleted and the program compiled. Files found at context
creation are loaded 4 a flip (the warm-up, during the intro movies and the
title screen); a program needed before its turn is loaded from its file
there and then. The statistics line reads `N programs (C compiled, L from
cache; run C/L)`, the last pair counted since start-up; a second launch
reaches Oakvale with 0 compiled. Off when the driver offers no binary
format (`[GPUGL] program binary cache off`) or with
`RECOMP_GPU_SHADER_CACHE=0`.

**Visibility tests.** Batches drawn while `SET_ZPASS_PIXEL_COUNT_ENABLE` is
on are wrapped in occlusion queries (a ring of recycled query objects).
Reading them at every `GET_REPORT` made the executor wait for the GPU 341
times a frame in Fable's world (a third of its time; on a tiler each wait
also ends the render pass), so reports are deferred: `GET_REPORT` calls
`zpass_end`, which closes the count and returns a ticket, and the executor
queues the report slot, which keeps the 0xFFFFFFFF D3D wrote at +0xC (not
done) until `zpass_resolve` has the count. Reports are resolved in order,
only as far as the GPU has got and without waiting, when the poll loop finds
nothing new to walk (`nv2a_pb_exec_idle`), at `FLIP_STALL` and again after
the present (its read-back has waited for the GPU anyway); past 512 pending
the oldest are waited for. The idle hook used to wait for the GPU every
time (9-14 times a frame, 17-22 % of the executor in Oakvale) although
Fable reads frame N-1's results lazily and keeps the cached one while a test
is pending. A title can still spin on one result (Fable's `sub_0012F7F0`)
and submit nothing meanwhile, so a report that stays at the head of the
queue across idle calls is pushed: after 300 us the back end's optional
`zpass_flush` is called once (GL: `glFlush`, so the driver submits the
query), after 2 ms the executor waits for it. `CLEAR_REPORT_VALUE` goes
through the same queue, so counts stay cumulative from the clear, as before.
`RECOMP_GPU_ZPASS=sync` reads synchronously, `=always` runs no queries and
reports 1, `=lazy` never flushes or waits from the idle hook: where it would
wait, the reports pending then read 1 (visible) at once and their GPU counts
are dropped when they come in (a candidate default for a tiler, where a
mid-frame flush ends the render pass). The `[GPU] visibility reports` line
counts reports written when idle and at the flip, waits, flushes and lazy
answers, and the idle hook's time per frame (not part of `render`).
GLES 3.0 only says whether any sample passed, which reports 1.

## Verification (2026-11-11, M1 Max, Fable title screen)

Back to back, 45 s each on the title screen, `[GPU] render … ms/frame` is the
executor's wall time per frame spent drawing (batches including the CPU
vertex-program run, clears, present read-back):

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

**7c-lite, Fable in-world (2026-11-11, M1 Max, shared).** 130 s runs back
to back, Start pressed through the menus, Oakvale at t = 100–130 s;
`[GPU] render` per 10 s window:

| Path | render ms/frame, last three windows |
|---|---|
| GL, `RECOMP_GPU_VSH=0` (CPU transform) | 284, 260, 204 |
| GL, vertex programs in GLSL | 123, 97, 76 |

Rebased on deferred reports, the hash policy, depth-only targets, culling
and the raster pool (WP-A/B/C/F, WP5), same runs: `RECOMP_GPU_VSH=0`
169/152/114 ms/frame at 4.7–6.8 fps, GLSL vertex programs 65/66/40 at
9.5–13.9 fps (35–46 ms at 13–16 fps earlier in Oakvale); flip dumps of the
same view 1.2/255 mean apart; title screen 2.4 ms/frame at the 60 fps cap.

`sample` at t = 95 s: `vsh_run_decoded`/`run_mac` absent from the executor
(were the largest CPU item with `RECOMP_GPU_VSH=0`); what is left there is
the synchronous visibility queries (`gl_zpass_read`, about 60 % of the
executor), texture hashing and render-target readbacks inside
`bind_stage_textures`, which deferred reports, the hash policy and direct
render-target sampling (7d) address. Vertex upload went from about 115 MB to
about 7 MB a frame. Flip dumps of the same Oakvale view on both paths: 2.9/255
mean, and the difference image is black on every static surface (houses,
ground); what differs is what animates between the two runs (clouds,
villagers, grass).

## Fable's bloom (root cause of the green glare, 2026-11-11)

Once the near-plane fix let Fable's post-process run, the GL frame showed
a green glare in the bottom right and cyan on faces. The chain: the 640x480
back buffer (`SET_SURFACE_FORMAT` 0x121, X1R5G5B5, 16-bit) is sampled as
`LU_X1R5G5B5` (0x1C) into a 256x256 target at 0x80D38080 (drawn linear and
swizzled in turn), downsampled through swizzled A8R8G8B8 targets
(`SET_SURFACE_FORMAT` 0x08080228, 0x06060228, 0x04040228: 256², 64², 16²;
0x80CF9080 alternates 256² and 128², 0x80D39080 128² and 64², with 64²,
32² and 16² targets after them), and added back over the frame.

Before 7d every link went through guest memory: each target was read back
and uploaded again where the next pass sampled it, and the back buffer was
read back as **R5G6B5** and decoded by the texture path as **X1R5G5B5**.
Read that way a 565 pixel RRRRRGGGGGGBBBBB gives R' = (R<<1 | G>>5) & 31
and G' = G & 31: red doubles and wraps, green loses its top bit, blue is
right. Bright greens (G >= 32 of 63) wrap to dark and push red, which the
threshold and blur passes turned into the glare. Not the threshold
constants, the blur taps' clamping or the targets' initial contents: with
the targets sampled directly (no guest-memory round trip, the back buffer
read as it was drawn) and 1555 read-back, the glare is gone with the same
combiner constants and the same address modes.

Measured on flip dumps of the same Oakvale view, excess green
G - (R+B)/2 averaged over the bottom-right 160x160 pixels (four 80x80
cells): pre-7d GL 54/63/66/68, CPU rasteriser 37/41/46/48, 7d GL 29/36/39/40;
the sky is 144/167/185 (RGB) pre-7d, 174/201/218 now and 174/197/217 on the
CPU path.

The CPU rasteriser has the same mismatch and keeps it: it writes R5G6B5
into the X1R5G5B5 back buffer and samples it as 1555, which is its yellow
and purple tint; it also adds a brighter white edge glow than the GL path
(left house wall 165 vs 113-132 mean). It is therefore not the reference
for the bloom; a fix there (writing 1555 for colour formats 1 and 2 in
`put_pixel` and the CPU presenter) is left out of this change, which keeps
the CPU path as it was.

## Batch signatures

A tool for a wrong spot in the picture: which batches draw it, and what is
left without them. Each batch gets a 32-bit signature of what decides how it
looks -- target address and size, combiner program and words, each stage's
format, kind and size, blend, alpha test, depth, stencil and colour mask --
and of no texture or vertex address, so a kind of batch keeps its signature
while the title streams. Off by default.

- `RECOMP_GPU_SIGDUMP=<file>`: at the first flip where `<file>` exists the
  frame's table goes to stderr and the file is deleted (`touch` it again for
  another frame). One line per signature, `[GPUSIG] <sig> n <batches> tris
  <triangles> : rt <address> <w>x<h> [masked] sp <stage program> | t<stage>[m<mode>
  f<format> k<kind> <w>x<h> @<address> a<wrap u>/<v> flt<min filter>] ... |
  bl <blend> at <alpha test> z <test>/<func> w<write> cm <colour mask> st
  <stencil> | rc n<stages> fin <final words> fog <colour> shf <shadow func>
  zeta <address>`, where the texture addresses are the first batch's; then
  one `[GPUSEQ]` line with the frame's target runs, `<address>(<w>x<h>)x<batches>`
  (a depth-only target shows its zeta address | 1).
- `RECOMP_GPU_SIGSKIP=<file>`: batches whose signature is listed in
  `<file>` (hex words, re-read every 16 flips) are not drawn.

With `RECOMP_FB_DUMP_FLIPS=<flag file>` a frame dump and a table can be
taken at the same flip: `touch sig flag; sleep 0.4; rm flag`.

## Known differences from the CPU rasteriser (7b)

- Colours and fog are interpolated with perspective (GL's default); the CPU
  rasteriser interpolates them linearly in screen space. GLES 3.0 has no
  `noperspective`; only large perspective triangles with colour gradients
  differ.
- Stencil is honoured on the GPU and ignored by the CPU rasteriser.
- Window z is clamped to the zeta format's range (depth clamp); the CPU keeps
  unbounded floats. `SET_CLIP_MIN` is the near plane both paths clip at;
  `SET_CLIP_MAX` is ignored by both. Float zeta
  formats are treated as integer. On GLES, GLSL vertex programs are also
  clipped at the far plane (depth clamp off for those draws, so GL's own
  near plane sits at `SET_CLIP_MIN`); on desktop they keep depth clamp and
  clip the near plane with `gl_ClipDistance`.
- Address mode 4 with a border colour clamps to the edge on GLES 3.0/3.1
  without `GL_EXT_texture_border_clamp` / `GL_OES_texture_border_clamp`.
- Without depth clamp (GLES lacking `GL_EXT_depth_clamp`) z is clamped per
  vertex in the vertex shader, which changes the depth slope of a triangle
  crossing the zeta range.
- Culling (`SET_CULL_FACE_ENABLE`, `SET_CULL_FACE`, `SET_FRONT_FACE`) on the
  GPU only; the CPU rasteriser draws both faces. The back end sets
  `NV2A_BACKEND_CAP_CULL`, so strips arrive with one winding; GL's front
  face is the opposite of the title's, because guest rows map to GL rows
  unflipped. `RECOMP_GPU_CULL=0` turns it off.
- Points and lines are not drawn, like the CPU rasteriser.
- A guest CPU write into a surface after the GPU created its target is not
  seen (FMV written straight into the framebuffer, a CPU-drawn loading bar):
  nothing tracks those writes yet.

## What is left

### 7c: vertex programs on the GPU

Done as 7c-lite (above): items 1–3 and 5, with attributes gathered on the CPU
(the first option of item 4). Left: item 4 proper (attribute arrays uploaded
by range and cached, fetched by the GPU in their own formats), item 6, and
programs that write c[] (`CXT_WRITE`), which stay on the CPU.

The original list (the interpreter was 41 % of the in-world executor):

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

1. Done: render to texture without readback (above, "Render to texture").
2. Guest CPU writes into surfaces: hash the surface's guest bytes against
   the last readback before drawing into it, or write-protect the pages, and
   re-upload when they changed.
3. Done: the presented surface only, asynchronously (above, "Present");
   none once 7e presents from the GPU.
4. Done on the GPU: depth surfaces read as shadow maps (above). Left: a
   depth surface read as plain texels, or by the guest CPU (Z16/Z24
   read-back into guest memory), and shadow maps on the CPU path.
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
Software / GPU, `RECOMP_GPU`) and **Frame cap (GPU renderer)** (Auto / 30 /
60 / Off, `RECOMP_GPU_FPS_CAP`), written to `recomp.cfg`
([app-shell.md](../android/app-shell.md#settings)). Auto is the title
profile's choice: Fable's profile sets `RECOMP_GPU_FPS_CAP=30` on Android
(`SETENV_DEFAULT`, `#ifdef __ANDROID__` in `titles/4d5300d1/title.c`), since
an Oakvale frame does not fit 16.7 ms on the Thor and a 60 cap lets the rate
wander; it does not turn `RECOMP_GPU` on.

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
