# GPU Back End (XBA-27)

The pushbuffer executor ([pushbuffer-executor.md](pushbuffer-executor.md))
renders on the CPU by default. In a 3D scene that is the frame-rate limit: on
Fable in-world, 41 % of the executor's time goes to the vertex-program
interpreter and 47 % to the rasteriser, on one thread. The GPU back end moves
the per-pixel half (rasteriser, texturing, register combiners, depth, stencil,
blending) to OpenGL. `RECOMP_GPU=1` selects it; it is the default on Android
(XBA-119, `RECOMP_GPU=0` keeps the CPU rasteriser), the CPU rasteriser stays
the default on desktop and is unchanged.

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
| `src/gpu/nv2a_gl_scale.c` | Internal resolution policy (which targets scale) and rectangle rounding. No GL calls. |
| `src/gpu/nv2a_gl_context.c` | Headless context: CGL (macOS), EGL pbuffer or surfaceless (Android, Linux); loss detection. |
| `src/gpu/nv2a_gl_api.h` | GL headers per platform: `OpenGL/gl3.h`, `GLES3/gl3.h`, libepoxy. |
| `tests/nv2a_gl_shader` | 400 random combiner programs through the generated shaders, each pixel compared with `nv2a_rc_eval`. |
| `tests/nv2a_gl_scale` | The internal resolution policy (0x121 / 0x2121 back buffer, swizzled and depth-only targets), the scale sticking to a scaled target's memory (Burnout 3's HUD clip and menu scissor, promotion, other pitch / swizzled), scissor rounding at 2.25, visibility count scaling, `RECOMP_GPU_SCALE_TARGETS` parsing. |
| `tests/nv2a_gl_vsh` | 600 random vertex programs plus ILU/a0 edge cases through the translator (transform feedback), compared with `nv2a_vsh_interp.c`; eight deliberate translator mutations must be caught. |
| `src/kernel/nv2a_coverage.c` | What is not drawn, by name (XBA-120): the NV2A texture format table, xemu's method names, the registry the report and the status file read. |
| `tests/nv2a_coverage` | The executor over a fake guest memory: the format table against the decoder, method names, each gap counted once per batch and reaching the status file. |
| `tests/nv2a_gl_coverage` | Pushbuffer methods through the real executor into the back end (GL and forced GLES 3.0), pixels read back: points and lines from the screen-space, fixed-function and vertex-program paths, point sprites; then mip chains, cube and volume textures, the added formats (XBA-120). |
| `tests/nv2a_gl_backend` | The back end itself (nv2a_gl.c included) on a real context and a fake guest memory: guest CPU writes into targets (clean, dirty, host present, scaled 16-bit, swizzled, none), the scratch depth growing, an anti-aliased surface presented. `-cpu-writes-off` runs the same cases with `RECOMP_GPU_CPU_WRITES=0` and expects them wrong. |

The GL tests are built twice on Linux: for desktop GL 3.3 core and, with
`NV2A_GL_FORCE_ES=1` (nv2a_gl_api.h), for the GLES 3.0 path Android runs
(GLSL ES header, GLES read formats, `GL_ANY_SAMPLES_PASSED`), through
libepoxy on an EGL ES context; the `-es` entries run with Mesa held to GLES
3.0 (`MESA_GLES_VERSION_OVERRIDE=3.0`). In a container Mesa's EGL finds no
display: every GL test sets `EGL_PLATFORM=surfaceless`, so CI runs them on
llvmpipe instead of skipping (before XBA-119 they all skipped there).

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
memory (or, with host present, the back end's own pixels at their own size:
"Host present" below). At `FLIP_STALL` the executor calls the back end's present hook
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
- its eviction (least recently drawn, 32 targets) or a change of its bytes
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

#### Host present

With an SDL window, the presented target can skip guest memory altogether
(`RECOMP_GPU_PRESENT=host`; on by itself when `RECOMP_GPU_SCALE` is above 1,
since guest memory only holds 640x480). At flip N `gl_present` reads the
target at its **real GL size** into one of two pack buffers of its own
(`host_issue`); at flip N+1 it maps that buffer (`host_map`) and hands the
pointer back in `Nv2aPresentSurface.host_px` / `host_w` / `host_h` /
`host_pitch` / `host_format` (`NV2A_HOST_FORMAT_BGRX8888` from desktop GL's
`GL_BGRA` read, `NV2A_HOST_FORMAT_RGBX8888` from GLES's `GL_RGBA`), while flip
N+1's read goes into the other buffer. `FLIP_STALL` then calls
`xbox_FramebufferWindowPresentHost(px, w, h, pitch, format)` instead of
`xbox_FramebufferWindowPresent(va, pitch)` (src/gpu never calls src/video);
the window copies the pixels into a slot before the call returns, and
`gl_flip` unmaps the buffer. With `RECOMP_GPU_PRESENT=sync` (or frame
generation forcing it) the frame is read and mapped at its own flip.

- **Where the size comes from**: one accessor, `rt_present_source(r, &w, &h)`,
  returns the framebuffer and the real GL size of target `r`'s guest area
  (`r->w x r->h`), origin (0, 0), top row first: `r->fbo` and
  round(w·sc) x round(h·sc) for a target drawn at an internal resolution
  (see below), the guest size at 1x. Nothing else in the present asks for
  a target's size.
- **Guest memory**: a host frame does not clear `dirty` (guest bytes stale);
  "host frame issued" is tracked apart (`s_rt_host_serial`, the target's
  write serial at its last host read: a target not drawn since is not read
  again and the window keeps its frame). The native copy happens at every
  flip only with `RECOMP_GPU_READBACK=present` (or `=all`); otherwise the
  target is read back on demand like any other (texture fallback, eviction,
  out-of-memory reset). Guest CPU reads of the front buffer (a `Lock`) then
  see the bytes of the last read-back.
- **Not host frames**: headless runs (the presenter never called
  `nv2a_gl_set_display_size`: no host read-back at all), a flip that dumps
  (`RECOMP_FB_DUMP`, `_FLIPS`: synchronous native read-back into guest memory,
  the dump files are unchanged and 640x480), and frames the guest CPU wrote
  (no GPU target at the presented address). The window switches between
  guest and host frames from one flip to the next; a pending host frame is
  dropped when a guest frame comes between.
- **The window** (fb_present_sdl.c): slots carry width, height, pitch and SDL
  pixel format (general, so a frame pacer can feed them the same way);
  `xbox_FramebufferWindowPresentHost` has the same lifecycle hold and
  close handshake as the guest present. The streaming texture is re-created
  when the size or format changes (`[FBWIN] texture WxH` is logged); a host
  frame is filtered linearly unless it maps 1:1 onto the letterboxed
  rectangle, guest frames stay nearest. `SDL_RenderSetLogicalSize(640, 480)`
  is kept, so letterboxing and the perf overlay are unchanged.
  `xbox_FramebufferDumpBmp` (the companion's Screenshot) writes the shown
  frame at its own size. The Win32 GDI window takes no host frames (a stub
  returning 0) and never sets a display size.
- **Frame generation** (`RECOMP_LSFG=1`, [Frame Generation](framegen.md#presenter)):
  host frames go to the pacer (`framegen_pacer_submit_format`, BGRX or RGBX at
  their own size) instead of a slot, and the window shows the pacer's real and
  generated frames from its timed FIFO; the generator reopens when the size
  changes. The `[FBWIN] host frames ...: copy` figure is then the pacer's
  submit (copy and conversion) on the executor.
- **Cost** (M1 Max, 1x, Fable intro to Oakvale, windowed): read-back issue +
  map 0.16-0.24 ms/frame on the executor, the window's copy 0.045-0.055
  ms/frame (`[GPUGL] host present: ...` and `[FBWIN] host frames ...: copy`
  lines, every 600 frames). Picture: the same frame through both paths
  (target read back synchronously into guest memory next to the host read)
  differs by at most 7/255, mean 1.8-2.4/255 -- the X1R5G5B5 rounding of the
  guest path, which host frames do not have.

#### Present of an anti-aliased surface

The SDL window reads a guest frame as 640 pixels a row of pitch / 640 bytes.
A title that presents its anti-aliased back buffer as is (1280 samples a
row: AA 2x1 or 2x2 of the 640x480 frame) cannot go through guest memory: a
32-bit one was refused ("8 bytes a pixel"), a 16-bit 2x1 one read as 32-bit
garbage, with either rasteriser. With a window, the GL back end hands such a
surface (`nv2a_gl_present_needs_host`: pitch != 640 x bytes per pixel) over
as a host frame whatever `RECOMP_GPU_PRESENT` says, resolved on the GPU
first: when its format has an anti-aliasing mode and it is 1280 wide and/or
960 high (`nv2a_gl_aa_factors`), a linear blit to half size along each
doubled axis -- an exact box filter over the 2 or 4 samples -- into a
texture of its own (`s_aa_tex`), read back at 640x480 times the internal
scale. Fable's 0x2121 binding (640 real pixels) and Burnout 3's HUD clip are
not touched. Headless runs and frame dumps keep the full surface in guest
memory. The `[GPUGL] host present` line counts the resolved frames.

Rejected for 7b: a command queue to a GL thread (one more copy of every
batch, and the executor would have to wait for readbacks anyway); a context
shared with the presenter (SDL's renderer owns its context, is Metal on
macOS, and sharing with it is stage 7e's problem, not a prerequisite).

`RECOMP_GPU` is read by `xbox_HostWindowMain` (fb_present_sdl.c) before the
guest starts, windowed or headless, which registers the back end; the
executor must be on (`RECOMP_PB_EXEC`). Unset, it means the platform's
default (`gpu_selected` in nv2a_gl.c): on for Android, off elsewhere. If the
context cannot be created (no EGL display, no GLES 3.0 config, a context the
driver refuses) the back end unregisters itself at its first call, logs
`[GPUGL] no GL context: falling back to the CPU rasteriser`, and the CPU
rasteriser draws; the executor does not call a back end again once it has
unregistered inside a call (the raw-batch retry did, through NULL, before
XBA-119).

## Internal resolution

`RECOMP_GPU_SCALE` (default 1): 2, 3, a decimal 1-4, or `auto`. `auto` fits
the window's drawable height: `nv2a_gl_set_display_size(w, h)` (nv2a_gl.h)
is called by the SDL presenter with `SDL_GetRendererOutputSize` once its
renderer exists and before the guest starts, and `ensure_init` reads it:
scale = height / 480, at most 3 (1080 -> 2.25, 1440x1080 shown 1:1; a
default Mac window on a Retina display -> 3), 1 headless; any scale is held
to `GL_MAX_TEXTURE_SIZE / 640`. The choice is logged:
`[GPUGL] internal scale 2.25 (RECOMP_GPU_SCALE=auto, window drawable
2400x1080; max texture 16384)`. The parsed value is `s_scale` in
nv2a_gl.c (`scale_from_env`). Above 1 the window is fed host frames
(Present, above) read at the target's real size through
`rt_present_source`.

What scales: the frame-buffer class of render targets is drawn at s_scale
times its guest size. Positions need no change: a vertex program's `oPos`
and the CPU-transformed vertices are in surface pixels, mapped against the
target's **logical** size (`u_vp` = 2 / gw, 2 / gh), so a viewport of
round(gw·n) x round(gh·n) over a texture that size scales them
continuously; half-pixel conventions, text and HUD stay as the title drew
them, sharper. No title patching, no shader changes.

Which targets (`nv2a_gl_scale.c`, no GL, tested by `tests/nv2a_gl_scale`),
decided from the colour surface when a batch binds it:

1. **Frame-buffer class → n**: a linear (not swizzled) colour surface whose
   real size (clip origin + size, real pixels) is 640x480 (the presenter's
   frame, `NV2A_GL_FRAME_W/H`) or 640x480 times its anti-aliasing factor.
   The key is real pixels: Fable binds its back buffer as
   `SET_SURFACE_FORMAT` 0x2121 (AA 2x2, logical clip 320x240) once a frame
   and 0x121 (640x480) otherwise -- the same pixels; keying on the logical
   size re-created the target twice a frame (13.8–14.9 ms, 49 fps in the
   prototype).
2. **Everything else → 1**: swizzled and power-of-two effect targets
   (Fable's bloom, noise, the 256² scene copy the bump-env pass reads), and
   depth-only targets (the 768² shadow map).
3. `RECOMP_GPU_SCALE_SHADOW=1` scales depth-only targets too;
   `RECOMP_GPU_SCALE_TARGETS=WxH[,WxH]` adds real sizes to the frame class.
4. **Depth follows colour**: depth textures are keyed by (zeta address,
   scale), so zeta 0 (the scratch depth) used by a scaled and a native
   target has one of each. The 640x480 minimum stays logical.
5. A batch with every colour channel masked and no colour target yet (the
   depth-only framebuffer) takes its colour surface's scale.
6. **The scale sticks to a scaled target's memory**
   (`nv2a_gl_target_scale`): a linear surface bound on memory that already
   has a scaled target with the same bytes per pixel and pitch keeps that
   target's scale, whatever its clip -- a sub-rectangle of a frame-class
   surface is the frame. Burnout 3 binds its 640x480 back buffer with clip
   0,0 640x480 for its post-process quads and 0,16 640x448 for its HUD
   (menus: other scissor rectangles, 160,16 480x447); keyed on the clip
   alone the target went 1 -> 2 -> 1 twice a frame (120 rescales per 60
   frames) and the presented target was the 1x one. Never down: a native
   target later bound as frame class is promoted (re-created once) and
   stays. Swizzled surfaces, another pitch and depth-only passes follow
   rules 1-5; a title that only ever binds its back buffer through a
   sub-rectangle stays native. All four callers (`rt_get`,
   `bind_depth_only`, both draw paths' `s_draw_sc`) go through
   `surface_scale`, so draw, depth and target agree.
   `RECOMP_GPU_SCALE_STICKY=0` turns the rule off, for comparison.

How the rest follows (`nv2a_gl.c`): `GlRT.w/h/gw/gh` and `GlDepth.w/h` stay
in guest pixels; `GlRT.sc` / `GlDepth.sc` give the scale and every GL size
is round(logical·sc): texture creation, `rt_grow`, the feedback copy
(`break_feedback`), the viewport and the scissor (`set_view`: both edges
rounded, round(x·sc) .. round((x+w)·sc), so adjacent rectangles stay
adjacent), the clear's invalidate rectangle. The polygon offset factor is
multiplied by the target's scale (it multiplies the depth slope per pixel,
and a pixel at scale n covers 1/n of a guest pixel); the units are not, and
depth-only targets keep the factor. Visibility counts on desktop GL are
exact samples, n² times more: each query keeps its target's scale and its
count is divided by sc², rounded, at least 1 when any sample passed (GLES
reports 1 either way). A target whose scale changes (a native target
promoted, memory reused at another pitch; neither seen in Fable or
Burnout 3's race) goes back through guest memory and is re-created
("target rescales").

**Scratch texture unit.** A batch binds its stage textures before its
target (`bind_stage_textures`, then `bind_target`), so anything that binds
a texture in between must not use the active unit -- the stage bound last
would sample it. Creating, uploading to or copying into a texture
(target creation and `rt_grow`, depth creation, seeding, resolves,
`tex_upload`) binds through `sh_tex_scratch`, on unit `SH_UNITS` (4),
which no stage samples; stage binds make their own unit active
(`sh_active`). Burnout 3's white centre at scale 2 was this: the back
buffer, re-created at another scale while stage 1 held the bloom, became
the composite's bloom texture, seeded with the previous final frame, a
frame fed back into itself every frame. `rt_free` also drops the stage
entries pointing at the freed target (`s_stage_rt`).

The **native resolve** of a scaled target (`rt_resolve`: a guest-size
texture + FBO, linear `glBlitFramebuffer` through `sh_blit`, cached by the
target's write serial) serves what needs guest-size pixels:

- read-back to guest memory (`rt_readback`, the guest-path present's
  `pbo_issue`): guest memory always holds the native picture in its own
  format, so headless runs, `RECOMP_FB_DUMP` and the guest-memory present
  show a supersampled 640x480;
- a native target sampling a scaled one (Fable's bloom and scene copy read
  the back buffer), which would otherwise under-filter n² texels per pixel
  (`RECOMP_GPU_SCALE_RESOLVE=0` samples the scaled texture instead, for
  comparison); a scaled target sampling a scaled one reads it directly.
  Texture coordinates stay logical either way (`tscale` from gw/gh);
- seeding: guest bytes go up at guest size into the resolve and are blitted
  up (nearest), and the resolve then holds the exact guest picture.

In Oakvale that is two resolves a frame plus one for a guest-path present.
At 2x the linear blit is an exact 2x2 box filter; at 3x it samples the
centre texel of each 3x3 block (the guest's own sample position).
`RECOMP_GPU_SCALE_DUMP=1` writes the presented target at its real size as
`<RECOMP_FB_DUMP>S<n>.bmp` at every dumping flip, next to the 640x480 dumps.
With a scale above 1 the start-up line `[GPUGL] scaled targets at 2.00:
frame 640x480 -> 1280x960, ...` and a statistics line `[GPUGL] internal
scale 2.00: N resolves, N seed blits, N target rescales, N depth
re-creations` (every 600 frames, 60 with `RECOMP_GPU_VERBOSE`) are logged;
rescales and depth re-creations stay 0 in steady state. At scale 1 nothing
changes: no GL call more.

Not scaled yet (nothing sends them on this path today; comments at
`apply_state` and `gl_clear`): points and lines, `SET_WINDOW_CLIP_*`,
`SET_CLEAR_RECT`. Guest CPU reads of a scaled target see the native
read-back, as at scale 1. Eviction of a scaled target loses its resolution
(it reseeds from guest memory). Memory: about 17 MB more at 2x, 36 MB at 3x
for Fable.

Measured (M1 Max, Fable, boot through the intro to Oakvale, 2026-11-11):
0 target rescales and 0 depth re-creations over whole runs at 2, 2.25 and
3; 3 resolves a frame in Oakvale headless (two for the effect targets,
one for the guest-memory present; a host present skips the last), none on
the title screen windowed; no GL errors. Render time against scale 1 back to back, in-world 5 s
windows: 7.50 vs 7.78 ms/frame (+0.28 mean, -0.03 to +0.64 per window, a
shared and noisy machine), 57.6-59.8 fps both. 2.25 and 3 (run in
parallel): 7.85-7.92 ms. Windowed at 2: `[FBWIN] texture 1280x960`, host
present 0.17-0.19 ms/frame. The native (guest-memory) dump differs from
scale 1 by 5.5/255 mean at 2 (mostly cloud animation and supersampled
edges), 3.5 at 2.25, 0.14 at 3. At scale 1, 45 of 54 windows of 60
presents on the title screen had exactly the same count of every GL entry
point as the build without this work (an interposer counting 86
functions; the rest differ by timing), and no blit or extra query at all.
RECOMP_GPU_FAKE_RESET (lost and oom) at scale 2 recovers on the title
screen (the back buffer reseeds by blit).

Measured (M1 Max, Burnout 3 PAL, boot through the menus to a Silver Lake
race on a pad script, headless, 335 s, 2026-11-11), with the sticky rule
and the scratch unit: 0 target rescales, 0 depth re-creations and no
`rescaled` line over whole runs at 2, 2.02 and 2.25; the presented target
1280x960 / 1293x970 / 1440x1080; 14 colour targets; race brightness (mean
luma of the native dumps) 52-92 at 2.02, 52-85 at 2.25, 59-83 at 2, against
53-92 at scale 1; about 59 fps at every scale. Before: at 2, 120 rescales
per 60 frames, the race white from the centre (luma 202-226), the
presented target 640x464 (native). With the scratch unit alone
(`RECOMP_GPU_SCALE_STICKY=0`) the picture is right but the rescales stay
(120 per 60 frames, presented 640x464). Fable at 2 and 2.25 is unchanged:
the same targets, 0 rescales, the same statistics, last dumps within
0.3/255 of the build without these rules (that build against itself:
0.33-0.37/255).

Launcher: Settings › Graphics › **Internal resolution (GPU renderer)**: Auto
(the profile's choice: 1 for now, no title sets it), 1× (original), 2×, 3×,
Fit the screen (`auto`). The default stays 1 until the device check.

## Aspect modes

The console renders 640x480 whatever the screen. A title can still make
16:9 pictures: it widens its camera and draws the 3D scene anamorphically,
squeezed into 640x480 (Fable: profiles/titles/4d5300d1, the camera's aspect
override), so the frame is right once stretched to 16:9; its menus and
full-screen movies stay 4:3, and its story movies are 16:9 letterboxed
inside 4:3. The SDL presenter therefore picks a shape per frame
(`src/video/aspect_policy.{c,h}`, pure C, `tests/aspect_policy`):

| Class | Shown | When |
|---|---|---|
| NORMAL | 4:3, logical size 640x480 (as before) | `RECOMP_ASPECT=4:3`; menus, logos, HUD-only frames |
| WIDE | whole frame stretched to 16:9, logical 1280x720 | title declared wide and the frame drew >= `RECOMP_ASPECT_MIN_TRIS` (5000) triangles, two frames of hysteresis; or `RECOMP_ASPECT=16:9` for a title that never declared (a plain stretch) |
| ZOOM | rows 60-419 (scaled by h/480) at 16:9 | wide policy and a NORMAL frame letterboxed for 6 frames in a row: every 4th row of 0-55 and 424-479, every 8th pixel, all channels <= 24; crop from the first picture row in 56-64 (movies start at 61) else 60; leaves at the first frame that is not letterboxed |

API (`video_player.h`): `xbox_AspectSetting()` (RECOMP_ASPECT: 0 auto, 1
4:3, 2 16:9, every configuration), `xbox_FramebufferWindowSetWide(int)`
(the title, any thread), `xbox_FramebufferWindowFrameTris(uint32_t)` (the
executor at each flip, next to `xbox_FramebufferWindowFrameStats`: the
triangles `draw_primitive` was handed since the last flip, from the
primitive type and vertex count, so the CPU rasteriser and the GL back end's
fixed-function and vertex-program batches count alike). The Win32 window
has no-op stubs; the unit tests that link the executor stub the call.

Presenter (`fb_present_sdl.c`): each published frame is classed in
`publish()` (or, under frame generation, when it is submitted to the pacer;
the FIFO entry takes the latest class when it reaches the sink) and the
slot carries class and crop rows. The main loop switches
`SDL_RenderSetLogicalSize` between 640x480 and 1280x720 when the shown
frame's class changes, copies the crop rectangle for ZOOM, and picks the
filter from the current logical aspect: nearest for a 640x480 NORMAL frame
(as before) or when the shown part maps 1:1 onto the letterboxed
rectangle, linear otherwise. The perf overlay scales with the logical
height. `xbox_FramebufferDumpBmp` (the Android companion's screenshot)
writes the shown shape: WIDE resampled linearly along x to round(h·16/9)
(1440x1080 -> 1920x1080, 640x480 -> 853x480), ZOOM cropped then the same
(1440x810 from 1080 rows). `RECOMP_FB_DUMP` and headless runs are
untouched (raw 640x480 guest frames; no classification without a window).

Internal resolution in WIDE: the scale stays uniform (2.25 on the Thor:
1440x1080 stretched 1.33x horizontally, linear, slightly soft along x). A
non-uniform scale (1920x1080 rendered) would need the 640x480 assumptions
to move (`FB_W/FB_H` in fb_present_sdl.c, `NV2A_GL_FRAME_W/H` in
nv2a_gl_scale.h, the auto scale's /480 and 640 cap in nv2a_gl.c); later.

Measured (M1 Max, Fable, windowed, RECOMP_GPU=1 RECOMP_GPU_SCALE=2.25
RECOMP_ASPECT=16:9, wide declared by a test build of the title patch,
2026-11-11): logos and title screen NORMAL (2 and 552 triangles), the
intro story movie `NORMAL -> ZOOM (1440x1080, rows 135-944)` with 1440x810
screenshots, the AutoSave cutscene `ZOOM -> WIDE (226876 triangles)`, the
pause menu `WIDE -> NORMAL (316 triangles)` and back `NORMAL -> WIDE
(361176 triangles)`, gameplay screenshots 1920x1080 with correct
proportions. `RECOMP_ASPECT=4:3`: one `- -> NORMAL` line, every screenshot
1440x1080. `RECOMP_ASPECT=16:9` without the declaration: every frame WIDE,
1920x1080. With frame generation (RECOMP_LSFG=1, MoltenVK) the FIFO frames
are shown and dumped ZOOM (1440x810) during the movie.

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

**Guest vertex buffers (`RECOMP_GPU_VBO`, WP-3).** Gathering cost 21–25 % of
the executor in Oakvale (190 k vertices a frame through `fetch_attr`), yet
every indexed vertex range Fable draws is the same frame after frame. By
default (`RECOMP_GPU_VBO=0` gathers every batch, as before) the GL back end sets `NV2A_BACKEND_CAP_RAW_VERTEX` and a
vertex-program batch drawn from vertex arrays arrives with its arrays as the
title stored them (`Nv2aProgramBatch.raw`: offset, type, size, stride per
input; `raw_first` the smallest index; `indices` the triangle list counted
from it; `vertex_count` the index range). The executor only finds the index
range and checks it lies in guest memory; inline arrays, ranges over 65536
elements and types `fetch_attr` does not read are gathered as before, and a
back end that declines a raw batch gets it again gathered.

The back end keeps one GL buffer per (first byte, length, stride) of a
batch's index range, streams that interleave (same stride, less than a stride
apart) sharing one. A buffer is hashed again the first time it is used in a
frame and re-uploaded when its bytes changed; a buffer created or changed in
this frame is hashed at every use, so a ring of dynamic vertices rewritten
within a frame is caught (a static range rewritten mid-frame is caught at the
next frame). Index lists are cached by content (element buffer per distinct
list), and each layout (buffers, offsets, formats, strides) has its vertex
array object, with the element buffer it last bound. Bounds: 4096 vertex
buffers / 64 MB and 8192 index lists / 32 MB, then the whole cache (and the
vertex arrays) is dropped. Attributes keep their guest formats
(`nv2a_gl_vsh_attr`): floats as floats, unsigned bytes normalised by GL,
D3DCOLOR as four normalised bytes swizzled `.zyxw` in the shader, normalised
shorts as integers divided by 32767 in the shader (GL's own normalisation
differs between GL 4.1 and 4.2+/ES 3), plain shorts as is, 11:11:10 as an
integer attribute unpacked in the shader. The input formats are part of the
vertex shader's cache key. `tests/nv2a_gl_vsh` runs every format and size
through the gathered and the raw path against the interpreter. The frame line
gains a `vertex buffers:` line (raw batches, KB uploaded and hashed per
frame, buffers, vertex arrays, index lists).

Oakvale at the tutorial prompt, M1 Max (shared), same build back to back,
steady 30 s averages: `RECOMP_GPU_VBO=0` render 13.0 ms/frame, 40 fps;
vertex buffers only 7.5 ms/frame, 48–50 fps; with the index list cache
6.85 ms/frame, 52.6 fps. `fetch_attr` and the dedup (24.9 % of the executor)
gone, batch build 2.6 %; 1046 raw batches/frame, 0 KB/frame vertex and index
upload in steady state, 3.1 MB/frame hashed (1.5 vertex, 1.6 index), 1357
vertex buffers (4.6 MB), 979 index lists (2.3 MB), 730 vertex arrays. No GL
errors; the frame dump differs from the gathered run only where the scene
animates (sky, grass, the character, the pulsing button).

**Fixed function on the GPU (`RECOMP_GPU_FFP=1`, XBA-134).** Off by
default. A fixed-function batch then reaches `draw_program` like a
vertex-program batch, with `Nv2aProgramBatch.ffp` set instead of program
words: the state `ffp_batch_begin` resolves once a batch (commit 511f2c1)
as a key (which inputs are streamed, lighting, the light kinds, colour
material, normal flipping) and as constants (composite and model-view rows,
viewport offset, scene ambient and material alpha, four per light; layout
in `nv2a_backend.h`, change stamps so a program uploads only what moved).
`nv2a_gl_vsh_ffp_body` writes `ffp_vertex` / `lit_color` expression for
expression as a body for the program translator's own wrapper (same inputs
-- gathered or `RECOMP_GPU_VBO` raw --, same `oPos` mapping, near plane and
fog), one shader per key. So the batch is no longer transformed, lit and
uploaded on the CPU each time (the `KB transformed on the CPU` of the frame
line). `tests/nv2a_gl_coverage` compares it with the CPU transform output by
output over 41 random states (worst difference 1.2e-7 on llvmpipe) and
draws the same picture both ways. Differences left: a vertex with clip w
within 1e-6 of 0 drops the whole batch on the CPU and is drawn on the GPU;
texgen, texture matrices, specular, skinning and spot cones are modelled by
neither (counted, "What is not drawn").

**Vertices.** Otherwise the executor transforms: vertex programs run in
`nv2a_vsh_interp.c`, fixed-function vertices in `ffp_vertex`, and triangles
are clipped at the near plane. The back end gets surface-pixel x/y, window z,
clip-space w, both colours, the fog factor and four texture-coordinate sets
per vertex, already as a triangle list. The vertex shader maps them back to
clip space (`(x·2/W − 1)·w`, …, `w`), so the GPU interpolates with 1/w as
the CPU rasteriser interpolates texture coordinates, and `Nv2aVertexFull` is
uploaded as it is (one `glBufferData` per batch).

**Points and lines (XBA-120).** With `NV2A_BACKEND_CAP_POINTS_LINES` the
executor hands points and lines over as such (`Nv2aBatch.prim`) from every
path: screen-space batches gathered like their triangles, fixed-function and
CPU-transformed batches clipped at the same two planes as triangles (a point
is dropped behind them, a segment cut), vertex-program batches as an index
list for the GLSL program. Strips and loops arrive as segment pairs (a
loop's closing segment last). The back end draws `GL_POINTS` / `GL_LINES`.
Point size is `SET_POINT_SIZE` (0x043C) read as xemu reads it, 6.3 fixed
point in logical pixels (1 while never sent), times the anti-aliasing factor
and the internal scale, written to `gl_PointSize` by both vertex shaders and
clamped to `GL_ALIASED_POINT_SIZE_RANGE` (desktop core enables
`GL_PROGRAM_POINT_SIZE`). Point sprites: `SET_POINT_SMOOTH_ENABLE` (0x031C)
is the NV2A's sprite switch (xemu); every stage then samples at
`gl_PointCoord`, its t turned over since guest rows are GL rows, (0,0) at
the point's top left. Not modelled, counted as unsupported: distance
attenuation (`SET_POINT_PARAMS_ENABLE`, the point keeps `SET_POINT_SIZE`)
and a vertex program's `oPts`. Lines are one pixel wide times the internal
scale on GLES, clamped to `GL_ALIASED_LINE_WIDTH_RANGE` (Adreno offers
wide lines), and one pixel on desktop GL, where a forward-compatible core
context refuses wider ones; the NV2A's own line width is not modelled
(xemu's register list does not name it). The CPU rasteriser still draws
neither. The statistics line gains `[GPUGL] N point batches, M line
batches` when there were any.

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

**Guest CPU writes (XBA-119).** A title may write a GPU target's memory
with the CPU: a movie decoded into the back buffer, a loading bar or text
drawn into the frame, a surface filled with memset. Before, the target kept
the GPU's pixels, its next read-back overwrote the CPU's bytes, and a host
frame never showed them. Each target now keeps a copy of its guest bytes as
they were when guest memory and the target last agreed (`snap`: the seed
upload, a read-back -- which refreshes the copy of every target it overlaps
-- or a merge). `rt_cpu_check` compares guest memory with it when the target
is first bound or sampled directly in a frame, before every read-back, and
for the presented target at every flip; the pixels that differ go into the
GL texture run by run along each row (a scaled target through its resolve,
the box around them blitted up nearest), every other pixel stays as the GPU
drew it -- a bar drawn over a GPU frame keeps the frame around it. Past 4096
runs the bounding box goes up whole. A memcmp per check when nothing
changed (Fable's back buffer: 600 KB). The statistics line `[GPUGL] guest
CPU writes into targets: N merged (K kpx)` appears when any were;
`RECOMP_GPU_VERBOSE=1` logs each merge. `RECOMP_GPU_CPU_WRITES=0` turns it
off. Not covered: a CPU write into a target that is then sampled through
the texture cache (another format) while the target is clean.

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
format. Filtering follows the CPU
rasteriser's choice: combiner batches honour the title's TENT filters,
the two simpler colour rules take the nearest texel unless
`RECOMP_TEX_BILINEAR`/`RECOMP_TEX_ANISO`.

**Mip chains (XBA-120).** Every level a swizzled or DXT texture declares
(`MIPMAP_LEVELS`) goes up, down to 1x1: NV2A levels follow one another,
each in the base level's own layout (`nv2a_backend_texture_level` gives a
level's offset and size; the cache key includes the level count), cube
faces each with their chain. The minifying filter takes the title's level
mode from `SET_TEXTURE_FILTER` MIN (xemu: 3 BOX_NEARESTLOD, 4
TENT_NEARESTLOD, 5 BOX_TENT_LOD, 6 TENT_TENT_LOD -- trilinear), the texel
filter staying the one the rule above chose; the LOD0 modes sample level 0.
`SET_TEXTURE_CONTROL0`'s LOD clamps (MIN bits 18-29, MAX bits 6-17, 4.8
fixed point as xemu reads them) go to `GL_TEXTURE_MIN_LOD`/`MAX_LOD`; the
LOD bias (`SET_TEXTURE_FILTER` bits 0-12, signed 5.8) is a shader uniform
(`u_lodb`, the `texture()` bias argument: GLES has no
`GL_TEXTURE_LOD_BIAS`). Linear (LU_IMAGE) textures have no levels.
`RECOMP_GPU_MIPS=0` keeps level 0 only. The CPU rasteriser still samples
level 0 (minified textures shimmer there); that is not counted as a gap.

**Formats (XBA-120).** The decoder both paths share (`sample_tex` /
`decode_texel`) now reads 51 of the 59 NV2A formats (the table in
`nv2a_coverage.c`): added the swizzled twins SZ_A8Y8, SZ_R6G5B5, SZ_G8B8,
SZ_R8B8, SZ_Y16, SZ_YB16YA16, SZ_R5G5B5A1, SZ_R4G4B4A4, SZ_A8B8G8R8,
SZ_B8G8R8A8, SZ_R8G8B8A8 and the linear LU_IMAGE_SY8, X7SY9, R8B8, G8B8,
SG8SB8, A8Y8, R6G5B5, Y16, YB16YA16, R5G5B5A1, R4G4B4A4, and the fixed depth
formats LU_IMAGE_DEPTH_X8_Y24_FIXED (depth in the top 24 bits, as a Z24S8
surface stores it) and _Y16_FIXED read as grey. Bump-map formats (D3D's
V8U8 = G8B8, L6V5U5 = R6G5B5, V16U16 = YB16YA16) hand their bytes over as
they are; a channel a format lacks reads 1, as alpha does (an assumption
xemu's own table should confirm); 16-bit values keep their top byte. Still
counted as unsupported: LU_IMAGE_A8CR8CB8Y8 and LC_IMAGE_A4V6YB6A4U6YA6
(`d3d8_format_is_swizzled` calls them swizzled, which they are not), the
swizzled depth formats (0x2A-0x2D, which the same helper calls linear) and
the float depth formats. A depth format on a projective combiner stage is
still a shadow map: compared on the GPU, white on the CPU path (which no
longer reaches the decoder for it). `SET_TEXTURE_FILTER`'s signed channels
(bits 28-31: A, R, G, B) are honoured on both paths: the texel's byte read
as two's complement over 127 after the fetch (`rc_signed`, `nv_s8`), except
on a stage a bump-env stage reads, which signs dU/dV itself.

**Cube and volume textures (XBA-120).** Cube maps (FORMAT bit 2, a
CUBEMAP stage) were already sampled as `samplerCube` from six faces, each
face's levels padded to 128 bytes; they now carry their mip chains too.
Volumes (FORMAT `DIMENSIONALITY` 3, `BASE_SIZE_P` slices; ADDRESS bits
16-19 the third axis' wrap) reach a back end with
`NV2A_BACKEND_CAP_VOLUME` as `Nv2aTexture.depth`: the executor decodes each
level whole through its own texel decoder (`decode_texel`, the format
switch of `sample_tex` split out), one 3D Morton order per level (xemu
`swizzle.c`: a bit of x, y and z in turn; `xbox_swizzle_offset_3d`), each
level halving all three sizes, and the back end uploads it with
`glTexImage3D` and samples it as `sampler3D` at (s/q, t/q, r/q) on a
PROJECT2D/3D stage or through the colour rule without combiners. Swizzled
formats only; a DXT volume is counted (`volume texture, DXT`) and reads
white. The CPU rasteriser samples neither its slices nor its levels and
counts every volume (`volume texture`).

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

## What is not drawn (XBA-120)

What a batch needs and the path drawing it (the GL back end or the CPU
rasteriser) leaves out is counted under a fixed name, once per batch, by
`note_batch_gaps` in `nv2a_pb_exec.c` (registry: `src/kernel/nv2a_coverage.c`).
A back end says what it draws through its caps; what neither path models is
counted for both. The names:

| Name | Counted when |
|---|---|
| `texture format 0xNN <xemu name>` | a stage in use samples a format the decoder (`sample_tex`, shared by both paths) cannot read: the stage reads white. The table in `nv2a_coverage.c` lists every NV2A format with its texel size, swizzle and whether it is decoded; `tests/nv2a_coverage` checks it against the decoder code by code. |
| `shadow map (CPU rasteriser)` | a depth format in a projective stage on the CPU path (the GPU compares with its zeta surface instead). |
| `points`, `lines`, `line strip`, `line loop` | the primitive, on a path that does not draw it (the CPU rasteriser, a back end without `NV2A_BACKEND_CAP_POINTS_LINES`). |
| `point size attenuation` | points with `SET_POINT_PARAMS_ENABLE` on (drawn at `SET_POINT_SIZE`). |
| `volume texture` | a stage in use has `DIMENSIONALITY` 3, on a path without `NV2A_BACKEND_CAP_VOLUME` (the CPU rasteriser). |
| `volume texture, DXT` | a DXT volume on the GPU (not decoded: white). |
| `texture stage mode 0xNN <name>` | dot-product and dependent-read modes (8 and up) on both paths, bump-env (6, 7) on the CPU path: they read zero. |
| `logic op`, `polygon mode point`, `polygon mode line`, `flat shading` | the state is set for the batch; nothing models it. |
| `stencil test (CPU rasteriser)` | stencil on, CPU path. |
| `fixed-function texgen`, `fixed-function texture matrix`, `fixed-function vertex blending (skinning)`, `fixed-function specular`, `two-sided lighting`, `spot lights` | a fixed-function batch with that state on (`ffp_vertex` / `lit_color` model the composite matrix and lights without specular or cones). |
| `batch with no vertex program, matrix or screen-space positions` | the batch is skipped. |

The report prints them every period, most used first (`[GPU] unsupported
(batches): points x12, texture format 0x26 LU_IMAGE_A8CR8CB8Y8 x40, ...`), and
the status file carries them as `"unsupported":{name: batches}`
(`recomp_status.h`), which the compat runner and Check my library append to a
run's blocker ([compat-runner.md](compat-runner.md)). Methods the executor
does not model at all are named too: each line of the report's unhandled
table carries xemu's name (`nv2a_regs.h`), "(read from the register file)"
for those modelled through `s_reg` (lights, culling, stencil, polygon
offset); the 16 most frequent unmodelled ones go to the status file as
`"unmodelled":{name: count}` (not a blocker: D3D sends many it never
relies on).

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
- Points and lines are drawn on the GPU only ("Points and lines" above);
  the CPU rasteriser draws neither and counts them as unsupported.
- Guest CPU writes into a GPU target are seen when the target is next bound
  or sampled (once a frame), read back or presented ("Guest CPU writes"
  below); a write between two batches of the same frame on the same target
  is merged at the next of those points, not at once.

## What is left

### 7c: vertex programs on the GPU

Done as 7c-lite (above): items 1–3 and 5, with attributes gathered on the CPU
(the first option of item 4); item 4 proper as `RECOMP_GPU_VBO` (guest vertex
buffers, above). Left: item 6, inline arrays (still gathered), and programs
that write c[] (`CXT_WRITE`), which stay on the CPU.

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
6. Done behind `RECOMP_GPU_FFP=1` (XBA-134, "Fixed function on the GPU"
   above); the original note: fixed-function T&L (`ffp_vertex`, `lit_color`) either as a second
   generated vertex shader or kept on the CPU (menus only use it lightly).

### 7d: surfaces, readback, reports

1. Done: render to texture without readback (above, "Render to texture").
2. Done (XBA-119): guest CPU writes into surfaces ("Guest CPU writes"
   below).
3. Done: the presented surface only, asynchronously (above, "Present");
   none once 7e presents from the GPU.
4. Done on the GPU: depth surfaces read as shadow maps (above). Left: a
   depth surface read as plain texels, or by the guest CPU (Z16/Z24
   read-back into guest memory), and shadow maps on the CPU path.
5. Done (XBA-119): anti-aliased surfaces (2×1, 2×2) render at the real
   size; a presented one is resolved on the GPU into a host frame ("Present
   of an anti-aliased surface" below).
6. Done: asynchronous visibility queries (deferred reports, "Visibility
   tests" above).
7. Done (XBA-119): `SET_CLEAR_RECT` for swizzled surfaces (the executor
   hands a back end the clear rectangle as the clip, as the CPU path clears
   it).
8. Done on the GPU (XBA-120): mip chains ("Mip chains" above). The CPU
   path still samples level 0.

### 7e: presenting from the GPU

0. Done: host present (Present, above) -- the presented target goes to the
   window at its real size through a pack buffer, bypassing guest memory
   (an internal resolution needs it). Still a read-back and a texture
   upload per frame; the zero-copy options below remove them.
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
([app-shell.md](../android/app-shell.md#settings)). Auto writes nothing:
the runtime's Android default applies. Fable's profile sets
`RECOMP_GPU_FPS_CAP=30` on Android (`env_default_android` in
`profiles/titles/4d5300d1/title.json`), since an Oakvale frame does not fit
16.7 ms on the Thor and a 60 cap lets the rate wander.

**Default on Android (XBA-119).** With `RECOMP_GPU` unset the GL back end
draws on Android (`gpu_selected`, `NV2A_GL_DEFAULT_ON`); the runtime then
sets `RECOMP_GPU=1` so code that reads it later sees the choice. The CPU
rasteriser draws instead when the launcher says Software (`RECOMP_GPU=0`,
the A/B switch), when a title's pack forces `RECOMP_GPU=0` (`env_force`:
a title profile's default comes too late for this choice, which is made
before the guest thread runs `xba_title_configure`), or by itself when the
context cannot be made (no EGL display, no GLES 3.0 config, a context the
driver refuses, the base shader failing), logged. Desktop keeps the CPU
rasteriser by default: the GL path there is a development tool, macOS
presents through Metal, and the CPU path is what the compat runner's
reference screenshots were taken with.

The blockers the earlier note listed, checked against the code then:

| Blocker | State |
|---|---|
| Synchronous visibility queries ending the render pass | Gone with deferred reports (G2, "Visibility tests"): no wait at `GET_REPORT`, a flush after 300 us and a wait after 2 ms only for a report the title spins on. `RECOMP_GPU_ZPASS=lazy` (never flush or wait from the idle hook) stays the candidate for a tiler, to measure on the device. |
| Per-frame vertex uploads (115 MB/frame) | Vertex-program batches come from cached guest vertex buffers (G3, `RECOMP_GPU_VBO`, on by default; 0 KB a frame in Oakvale's steady state). Fixed-function and screen-space batches are still transformed on the CPU and uploaded per batch by default; the frame line gives that figure (`KB transformed on the CPU`). `RECOMP_GPU_FFP=1` (XBA-134) moves fixed-function batches to a generated vertex shader over cached vertex buffers: to measure on the device before it becomes the default. |
| CPU writes into a surface the GPU does not see | Done: "Guest CPU writes". |
| Clear rectangles on swizzled surfaces | Done: the executor hands a back end `SET_CLEAR_RECT` on a swizzled surface. |
| Anti-aliased surfaces without a resolve | Done: "Present of an anti-aliased surface"; drawing and sampling them already worked at their real size. |

Found on the way: a depth re-creation (the scratch depth growing for a
target bigger than 640x480) left another target's framebuffer bound, so the
clear or batch that caused it went into that target (`depth_free`, fixed);
and a back end that unregistered inside `draw_program` was called again
through NULL by the raw-batch retry (fixed in the executor).

### Device check, GPU by default (XBA-119), for Alex

On the Thor with a fresh install of the APK built from this branch, Settings
› Graphics › Renderer left on Auto (`recomp.cfg` without `RECOMP_GPU`).
`[FBWIN] frame times` and `[GPU] ... fps` come from logcat (tag
`xboxrecomp`); screenshots from the companion panel › Screenshot.

1. Start Fable. logcat: `[GPUGL] GPU back end by default on this platform`,
   the EGL context line, `Adreno (TM) 740 | OpenGL ES 3.2`, no `no GL
   context`, `shader compile failed`, `link failed` or `GL error` line.
2. Fable, CPU vs GPU, same spots: logos, intro movie, title screen, main
   menu, the AutoSave screen, Oakvale after the first cutscene, the pause
   menu. Take a screenshot at each with Renderer: Auto, then again with
   Renderer: Software (the game restarts when the setting changes). Note
   any spot where the GPU picture misses something the software one has.
3. The same for Burnout 3 (logos, attract movie, menus, the loading screen
   before a race, a race), Black (intro movie, menus, the first mission's
   loading screen, in-game) and KOTOR (logos, the movie, character
   creation, the Endar Spire): movies and loading screens/bars must show on
   the GPU (they are the CPU writes XBA-119 merges); logcat `[GPUGL] guest
   CPU writes into targets: N merged` tells whether a title does that.
4. For each title, in-game, 60 s on GPU then 60 s on Software: the
   `[FBWIN] frame times` lines (median and p95) and `[GPU] ... fps`; copy
   the `[GPUGL] frame ...` line (draws, `KB transformed on the CPU`, uploads,
   readbacks) and the `[GPU] visibility reports` line.
5. Fable in Oakvale on GPU: repeat 4 with `RECOMP_GPU_ZPASS=lazy` added to
   `recomp.cfg` by hand (adb): frame times against the default.
6. Thermal: 10 minutes in-game in Fable on GPU, then 10 minutes on
   Software, from the same starting temperature; `adb shell dumpsys
   thermalservice | grep -i temperature` (or the Thor's overlay) and the
   battery percentage before and after each.
7. A title that presents an anti-aliased back buffer (if any of the four
   does: logcat `[GPUGL] host present: ... N resolved from an anti-aliased
   surface` with N > 0): the picture is whole and 4:3, not a quarter or
   garbage.
8. Renderer: Software: no `[GPUGL]` line at all, the game draws as before;
   Settings shows Frame cap and Resolution dimmed. Back to Auto: not
   dimmed.

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
12. Renderer GPU, Settings › Internal resolution: 2×. logcat: `[GPUGL]
    internal scale 2.00`, `[GPUGL] scaled targets at 2.00: frame 640x480 ->
    1280x960`, `[GPUGL] host present on (window WxH)`, `[FBWIN]
    texture 1280x960 (SDL_PIXELFORMAT_XBGR8888)`, every 600 frames
    `[GPUGL] internal scale 2.00: ... 0 target rescales, 0 depth
    re-creations`,
    then every 600 frames `[GPUGL] host present: ... ms/frame` and `[FBWIN]
    host frames ...: copy ... ms/frame`. Picture, letterbox and perf overlay
    in place; no `GL error`.
13. Fit the screen: `internal scale` = screen height / 480 (2.25 at 1080).
    Auto: `RECOMP_GPU_SCALE` gone from recomp.cfg, no `internal scale` line.
14. Companion panel › Screenshot at 2×: the BMP is the scaled size.
