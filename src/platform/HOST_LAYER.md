# Host layer (`xbr_host`)

What every console runtime (Xbox, `consoles/gba`, `consoles/ps1`, `consoles/n64`) shares on the host: window + GL
present, pads, audio out, pacing and frame generation, storage, lifecycle, config, host time and cores, logging.
XBA-145 (C0-1), Plane spec « Spec · Other consoles ».

`src/host/CMakeLists.txt` defines `xbr_host`. Adding that directory alone builds no Xbox source: no XBE, NV2A,
kernel, `win32_compat` or MMIO decode. `tests/host_layer` links only `xbr_host` and checks that at configure time. POSIX
hosts only (Linux, macOS, Android).

| Target | Sources | Defined in |
|---|---|---|
| `xbr_host_core` | `android_log.c`, `recomp_lifecycle.c`, `recomp_quit.c` (+ `host_time`, `host_cpus`, `host_perf_hint`, `host_perf_line`, `recomp_cfg`, `recomp_exit_hook`) | `src/platform/host_core.cmake` |
| `recomp_storage` | `recomp_storage.c` | `src/platform/host_core.cmake` |
| `input_ports` | `src/input/input_ports.c` | `src/input/mapping/input_ports.cmake` |
| `host_framegen` | `src/framegen/framegen_pacer.c`, `framegen_fake.c`, `framegen_console.c` (the console window's front end, XBA-186) | `src/framegen/host_framegen.cmake` |
| `xbr_host` | `src/video/host_window.c`, `perf_overlay.c`, `display_vsync_android.c` (Android), `src/input/host_pad.c`, `src/audio/host_audio.c`; links all of the above + SDL2 | `src/host/CMakeLists.txt` |

The Xbox libraries keep their names and link the host layer. `platform` links `xbr_host_core`, `xbox_input_map` links
`input_ports`, `xbox_input` and `xbox_video` link `xbr_host`, and `xbox_framegen` links `host_framegen` (it is an
interface target when there is no Vulkan bridge). The `*.cmake` files have include guards, so `src/platform` and
`src/host` can each be configured alone.

## Classification

**G** generic (in the host layer), **X** Xbox-specific, **M** mixed (what was done is in the last column).

### src/platform
| File | Class | Note |
|---|---|---|
| `android_log.[ch]` | G | log pump; moved from `platform` to `xbr_host_core` |
| `recomp_lifecycle.[ch]` | G | moved from `platform` to `xbr_host_core` |
| `recomp_quit.[ch]` | G | moved from `platform` to `xbr_host_core` |
| `host_time.[ch]`, `host_cpus.[ch]`, `recomp_cfg.[ch]`, `recomp_exit_hook.[ch]` | G | already their own targets, now defined in `host_core.cmake` |
| `host_perf_line.[ch]` | G (new) | the performance line every runtime logs every 10 s and over the run, and the overlay's live figures (XBA-243, [below](#performance-line-xba-243)) |
| `recomp_storage.[ch]` | M → split | new `recomp_storage_resolve_entry(ext, int, entry, out)` is the generic call (any entry file, or none). `recomp_storage_resolve` = that call with `"default.xbe"`, unchanged |
| `recomp_app.[ch]` | M, kept X | the Android start-up: host parts plus `xbox_path_init` and the kernel fault handler; it stays a thin Xbox wrapper |
| `recomp_pack.[ch]` | M, kept X | pack.json parse/validate/dlopen is generic, the XBE title ID and the exported symbols are Xbox. Generalised with the manifest `console` field (C0-3) |
| `win32_compat.[ch]`, `xbox_winnt.h` | X | Win32/NT vocabulary for lifted Xbox code (`platform`) |
| `mmio_decode*.h` | X | trapped guest MMIO decode |

### src/video
| File | Class | Note |
|---|---|---|
| `host_window.[ch]` | G (new) | SDL window, GL ES 3.0 / 3.2 core context, RGBA or GL-texture present, letterboxed (aspect, integer or stretch scaling, linear or nearest: `host_window_set_scaling`; a fixed display shape such as the PS1's 4:3: `host_window_set_aspect`), a capture of the drawable as presented (`host_window_capture`), events → lifecycle and pads; from any thread, the present rate (`host_window_stats`) and the last picture as a BMP (`host_window_screenshot_bmp`; a picture presented with `host_window_present_rgba_shared` is not copied, the caller gives it on demand); the shared display keys `RECOMP_SCALE` (aspect, integer or stretch) / `RECOMP_FILTER` / `RECOMP_PRESENT_VSYNC` (`host_window_env_*`, XBA-181); picture shaders none / sharp / scanlines / crt / lcd / fxaa / cas (`host_window_set_shader`, `RECOMP_SHADER*`, XBA-195, [below](#picture-shaders-xba-195)); a minified picture's texels averaged (`RECOMP_PRESENT_DOWNSCALE`, [below](#minified-pictures)) |
| `present_shaders.[ch]` | G (new) | the picture shaders (XBA-203): presets, `RECOMP_SHADER*` keys, GLSL in two dialects, GL program and uniforms, shared by `host_window` and the Xbox presenter ([below](#picture-shaders-xba-195)) |
| `perf_overlay.[ch]` | G | the performance overlay (`RECOMP_OVERLAY=1`); moved from `xbox_video` to `xbr_host`. Drawn by the Xbox presenter through SDL_Renderer and, since XBA-243, by `host_window` (its pixels as a GL texture), with the performance line's fields |
| `display_vsync_android.[ch]` | M → split | AChoreographer grid; now hands samples to a sink passed to `display_vsync_android_start`, and the Xbox presenter passes `nv2a_pb_exec_display_clock`. Moved to `xbr_host` |
| `display_clock.h`, `frame_times.h` | G | header-only |
| `fb_present_sdl.c` | M, kept X | the Xbox presenter: an SDL loop around guest framebuffers, the NV2A executor's pacing and the LSFG feed. Left whole (no behaviour change); its generic helpers moved out. Moving it onto `host_window` is later work |
| `fb_present.c`, `gpu_present.[ch]` | X | Windows GDI / D3D11 presenters |
| `pvideo_posix.[ch]`, `pvideo_scanout.h` | X | NV2A PVIDEO overlay |
| `video_player.[ch]`, `video_pump.c` | X | Media Foundation → D3D8 FMV |
| `aspect_policy.[ch]` | X | Xbox widescreen setting and guest-frame classification |

### src/input
| File | Class | Note |
|---|---|---|
| `host_pad.[ch]` | G (new) | SDL controllers on 4 ports (`input_ports` rules), console-neutral state, rumble (`RECOMP_RUMBLE=0`: `host_pad_rumble_env`) |
| `input_ports.[ch]` | G | pure logic; split out of `xbox_input_map` into `input_ports` |
| `xinput_device.c`, `xinput_xbox.h`, `mkb_input.[ch]` | X | XInput-shaped Xbox pads, Win32 raw input |
| `keyboard.[ch]`, `keyboard_sdl.[ch]`, `input_map.[ch]`, `input_overlay.[ch]` | M, kept X | generic rules over the Xbox pad struct (`XINPUT_GAMEPAD`); the type would have to change, so they stay with the Xbox side |

### src/audio
| File | Class | Note |
|---|---|---|
| `host_audio.[ch]` | G (new) | one SDL device, queued S16 frames, paused with the app; its buffer from `RECOMP_AUDIO_BUF_SAMPLES`, the output volume (`host_audio_set_volume`, the Android menu) |
| `dsound_device.c`, `dsound_xbox.h` | X | DirectSound over the MCPX APU |
| `wma_decoder.[ch]` | X | Media Foundation WMA |

The Xbox audio out is `src/apu/apu_sdl2.c` (APU-bound, X).

### src/framegen
| File | Class | Note |
|---|---|---|
| `framegen_pacer.[ch]`, `framegen_fake.c`, `framegen_policy.h`, `framegen_console.[ch]` | G | `host_framegen` |
| `lsfg_*` (bridge, generator, cache, dll, spv, dxbc) | G, optional | Vulkan LSFG; stays in `xbox_framegen` / `lsfg_shaders` (heavy; in the default build since XBA-187, `-DXBOXRECOMP_LSFG=OFF` leaves it out). A console can link them as they are |

## Picture shaders (XBA-195)

The console window (`host_window.c`) can draw the picture through one of seven presets, our own small fragment shaders
(GLSL ES 3.0 on Android / Mesa, GLSL 1.50 on a 3.2 core context; no third-party shader pack). Each is a single pass
at the output resolution: the same full-viewport triangle and letterbox as the plain present, the shader replacing
the plain sampling. It therefore applies after the picture is made, whatever made it: an upscaled PS1 / N64 frame
(XBA-184 / XBA-185), a frame generation FIFO entry (XBA-186: `fg_service` draws through `upload_draw` like any
other), and every scale mode (aspect, integer, stretch, `host_window_set_aspect`).

| Preset | `RECOMP_SHADER` | What it draws |
|---|---|---|
| None (default) | `none` | the plain program, unchanged (no cost) |
| Sharp bilinear | `sharp` | as if the picture were enlarged by the largest whole factor with nearest, then linearly to the window: square pixels at any scale, the blend only over the output pixel that straddles a texel edge. Samples the window's texture linearly whatever `RECOMP_FILTER` says |
| Scanlines | `scanlines` | the lower half of each console line darkened (strength), the upper half slightly brightened; antialiased over one output pixel; faded out below two output pixels per line |
| CRT | `crt` | scanlines + an aperture grille (red / green / blue columns one output pixel wide); `RECOMP_SHADER_CURVATURE` adds a barrel curvature (black outside) and a vignette |
| LCD grid | `lcd` | each console pixel sharp, its right and bottom output pixel darkened (strength), faded out below 2.5 output pixels per cell; `RECOMP_SHADER_GHOSTING` blends the previous picture in (present_rgba alternates two textures for it) |
| Edge smoothing | `fxaa` | FXAA-class (our own): luma at the four diagonal half-steps gives an edge's direction, two and four taps along it are blended, the four-tap blend kept unless it leaves the neighbourhood's luma range; low-contrast areas untouched. The step is one output pixel, or one texel when the picture is enlarged. Samples linearly |
| Sharpen | `cas` | contrast-adaptive sharpening (our own, after the published CAS idea): centre and four neighbours (same step as FXAA), the negative lobe up to -0.2 per neighbour at strength 100 (default 50), shrunk where the neighbourhood nears black or white (no halo or clipping); strength 0 draws as none. Samples linearly |

« Console lines »: the picture's height divided by `round(height / 240)`, so an upscaled 960-line PS1 picture still
gets 240 scanlines and a GBA 160. Strength, curvature and ghosting are 0..100 (strength unset: the preset's default,
scanlines 50, CRT 60, LCD 50, CAS 50).

API: `host_window_set_shader(&HostShader)` (preset, strength, curvature, ghosting; before or after open, kept across
open / close), `host_window_shader_active()` (the preset drawn: none after a failure), `host_window_shader_parse` /
`_name`, `host_window_env_shader(console_var, def, &out)` and `host_window_shader_env(console_var)`, which the GBA,
PS1 and N64 runtimes call next to `host_window_set_scaling` (`RECOMP_GBA_SHADER` / `RECOMP_PS1_SHADER` /
`RECOMP_N64_SHADER` win over `RECOMP_SHADER`). A program is built on its first draw; one that does not compile or
link is logged once (« picture shader X does not build here; drawing without one ») and none is drawn instead
(`RECOMP_SHADER_TEST_FAIL=<preset>` forces that, for the test). `host_window_screenshot_bmp` stays the raw picture,
without the shader; `host_window_capture` reads the drawable as drawn, shader included.

The presets, their keys, the GLSL and the uniforms live in `present_shaders.[ch]` (XBA-203), shared by the console
window and the Xbox presenter: the same fragment bodies in two dialects, GLSL ES 3.00 / 1.50 for a context of our
own (`host_window`: no vertex buffer, `gl_VertexID`) and GLSL ES 1.00 / 1.20 for SDL_Renderer's context (a vertex
attribute; `texture()` mapped onto `texture2D`, with a B/R swap for the textures SDL's opengles2 renderer keeps as
swapped RGBA). `present_shader_env`, `_parse`, `_name`, `_strength`, `_clamp`, `_console_cells` / `_xbox_cells`,
`_build` (with the `RECOMP_SHADER_TEST_FAIL` fallback and its log line), `_use` (the uniforms) and
`_draw_legacy` (a draw inside SDL's renderer that puts back every GL state SDL caches).

### Minified pictures

An upscaled scanout is often larger than the rectangle it is shown in: the N64 at 4x hands the window 2560x1152
(the VI's 640 columns with the guard band, `scanout_steps` in `n64_rdp_vk.cpp`) for a 1440x1080 4:3 viewport on a
1080p panel, 1.8x horizontally, 1.07x vertically; a PS1 640-wide mode at 4x is 2560x1920. One bilinear tap per output
pixel then skips texels past ~1.5x (thin lines and text break up, edges crawl); nearest keeps one column in 1.8 and
drops the rest. So in an axis minified by more than 1.25x (`PRESENT_DOWNSCALE_MIN`) the picture is drawn with the
texels under each output pixel averaged: a box filter over texel centres (`xr_area` in the shared GLSL), exact while
the pixel spans up to 7 texels, `ceil(ratio) + 1` taps in that axis (3 for the N64's 1.8x, 3 x 3 at 2x), at most 8,
spread bilinear taps beyond 7x; an axis that is not minified keeps the single tap of the texture's filter.
`present_shader_taps` decides the taps (also the uniform `u_taps`, with the footprint); every preset samples through
it (`xr_sample`: scanlines, CRT, LCD, FXAA and CAS average too, sharp bilinear draws the area filter when
minified, there being no whole factor to keep), and with no preset `host_window` draws a small area program
(`PRESENT_SHADER_AREA`, not a preset: `present_shader_parse("area")` is -1) instead of the plain one. A picture that
is not minified draws exactly as before (the plain program, no cost).

Nearest: the filter means square pixels for an enlarged picture. Minified by a non-integer factor it only drops
texels, so the averaging applies whatever `RECOMP_FILTER` says (the texel-centre taps read the same with either
filter); an axis that is not minified stays nearest. `RECOMP_PRESENT_DOWNSCALE=0` turns it off (one tap at any scale,
as before); `host_window_set_downscale(1 / 0 / -1 the variable)`, `host_window_downscale_drawn()`. The change is
logged once (« picture 2560x1152 in 1440x1080: minified, texels averaged »). The Xbox presenter averages inside its
presets only (its plain copy stays SDL's).

Cost: the N64 case is three texture fetches and a short uniform loop per output pixel (~1.5 M pixels), against one;
FXAA ~19 and CAS ~15 fetches when minified (each neighbour averaged too). `host_layer_test shaders` prints both
(`HOST_LAYER_BENCH_SIZE=1440x1080`), on llvmpipe with one thread (a software rasteriser: the shader's own
arithmetic, not a GPU's): the 2560x1152 picture at 4:3, one tap 41.5 ms, averaged 101.7, FXAA 402, CAS 362. On an
Adreno 740 the averaged pass should cost well under 0.1 ms over the plain one (4.7 M fetches); FXAA / CAS about
0.2-0.3 ms: to be confirmed on the device (`perf` present column).

### The Xbox presenter (XBA-203)

`fb_present_sdl.c` draws its frame through the same presets: `RECOMP_XBOX_SHADER` wins over `RECOMP_SHADER`, with
the same strength and curvature keys. `lcd` is a handheld's grid: an Xbox frame is drawn without one (logged). The
pass replaces the plain `SDL_RenderCopy` of whatever is shown (a flip, a GPU back end's frame at its
`RECOMP_GPU_SCALE` size, a frame generation entry, a ZOOM crop) and draws into the letterboxed rectangle of the
logical size (640x480 or 16:9) at the output resolution, inside SDL's renderer: its GL context (the `opengles2`
renderer on Android, GLSL ES 1.00; `opengl` on Linux, GLSL 1.20; on macOS a shader asks SDL for its OpenGL renderer
instead of Metal), its texture (`SDL_GL_BindTexture`), and every GL state SDL caches put back after the draw. The
perf overlay is drawn over it by SDL. Scanlines follow the guest's lines: the frame's height is 480, 576 or 720
lines (or a GPU multiple), and a standard-definition frame draws one scanline per pair of lines, as a TV shows one
field (240 / 288); a 720-line frame one per line. Sharp bilinear samples linearly. Another renderer, a texture SDL
pads, or a shader that does not build: the plain copy, logged once. `xbox_FramebufferDumpBmp` stays the raw frame;
`xbox_FramebufferWindowCapture` writes the drawable as drawn (PPM), `xbox_FramebufferWindowShaderActive` the preset
drawn.

Tests: `tests/host_layer`, `host_layer_test shaders` (`host_layer_shaders`, skipped without a display): each preset
drawn into a 640x480 window and read back with `host_window_capture`: scanlines darken rows 2-3 of every 4-row line
and leave the columns alike, strength 0 equals none, the CRT's columns are red / green / blue and its curvature
blacks the corners, the LCD grid darkens each cell's last column and row and keeps its inside, ghosting shows half
of a white picture under the next black one, sharp bilinear blends a black / white edge over fewer columns than
linear at 4.27x, the BMP screenshot is the raw grey, a forced build failure draws exactly what none draws. Run it
under Xvfb + Mesa (`DISPLAY=:N`). Minified pictures: 1-texel black / white
columns 1152 wide (and 2.25x vertically) in 640 output columns: one tap a moire (mean absolute deviation along a row
67 linear, 128 nearest), averaged near the mean grey (14), with linear and nearest; every preset averages; a picture
that is not minified (4x) draws the same on and off, 1.07x stays one tap. FXAA blends a hard 1:1 diagonal (0
blended pixels with none, ~900 with it) and leaves a flat picture as none; CAS steepens a soft 64 / 192 edge, leaves
flat parts as they are, strength 0 as none. `host_layer_test present_shaders` (`host_layer_present_shaders`): the shared
module's keys, lines, taps and sources on every host, and with a display every preset built in each context Mesa offers
(GLES 3.0 with GLSL ES 3.00 and 1.00, GLES 2.0, 3.2 core, compatibility). `tests/fb_present_sdl`
(`fb_present_sdl_shaders`, skipped without a display): the Xbox presenter's window with SDL's opengl and opengles2
renderers, each case a child process presenting a frame and capturing the window: none, scanlines (240 over 960
rows, also through an RGBX frame; the BMP screenshot raw), CRT columns, curvature corners, sharp against linear at
4.27x, `lcd` drawn as none, `RECOMP_XBOX_SHADER` over `RECOMP_SHADER`, the forced build failure.

Cost (`HOST_LAYER_BENCH_SIZE=1920x1080 HOST_LAYER_BENCH=100 host_layer_test shaders`, ms per present of a 160x120
picture in a 1920x1080 window, Mesa llvmpipe on one CPU thread, `LP_NUM_THREADS=1`; a software rasteriser, so the
difference to none is the shader's own arithmetic): none 13.2, sharp 17.0, scanlines 15.8, CRT 19.5, LCD with
ghosting 23.8. On a GPU the pass writes the same pixels as none; the shaders add 15 to 40 ALU operations and one
or two texture fetches per output pixel, about 0.1 GFLOP per 1080p frame: well under 0.2 ms on an Adreno 740-class
GPU (AYN Thor), to be confirmed on the device.

## Performance line (XBA-243)

Every runtime logs the same line every 10 s and once over the run when the game ends, as the N64 did first (0.6.0):

```
gba: perf speed 100 % (59.7 of 59.7 fps); emu 3.1 audio 0.1 present 0.4 idle 12.6 other 0.4 ms/frame; worst 9 ms,
0 > 16.7 ms, 0 > 33.5 ms of 597 frames; shown 59.7 fps, busy 3.9 ms/frame
gba: perf over the run: ...
```

`host_perf_line.[ch]` (libc and pthreads, in `xbr_host_core`) holds the accounting and the text. The runtime calls
`host_perf_frame` at the top of each host frame, `host_perf_stage` / `host_perf_idle` as it times its work and its
sleep, `host_perf_guest` / `host_perf_shown` for the frames it ran and the pictures it handed to the window,
`host_perf_pause` / `host_perf_resume` around a time away, then `host_perf_log_due` (the 10 s line) and
`host_perf_log_run` (the run's). The fields:

- **speed**: guest frames per second against the guest's full rate; `speed ? (29.9 fps)` without one (the Xbox: a
  title picks its own rate).
- **stages**: wall time per guest frame in each of the runtime's stages, its sleep to the next deadline (`idle`) and
  the rest no stage covers (`other`; `guest` on the Xbox, where the game's own work is what the presenter does not
  time).
- **frames**: the slowest host frame (its work, not its sleep), the host frames longer than the budget (one guest frame,
  60 Hz on the Xbox) and than twice it, out of the host frames.
- **shown**: pictures handed to the window per second, and the mean work of a host frame.

| Runtime | Prefix | Stages | Host frame |
|---|---|---|---|
| Xbox (`fb_present_sdl.c`) | `xbox:` | `render` (the executor's drawing, `nv2a_pb_exec_render_ns`), `present` (the copy into the window's slot), rest `guest` | one flip |
| GBA (`gba_runtime.c`) | `gba:` | `emu`, `audio`, `present` | one loop turn |
| PS1 (`ps1_runtime.c`) | `ps1:` | `emu`, `audio`, `present` (the full rate follows NTSC / PAL) | one loop turn |
| N64 (`n64_runtime.c`) | `n64:` | its own longer line (`n64_runtime_perf_format`, unchanged text) built on the same accounting (`core`, `vi`, `present`, `audio`) and the shared fields (`host_perf_format_speed`, `host_perf_format_frames`) | one loop turn |

Every half second the figures of the last half second are published (`host_perf_live_get`): the performance overlay
(`perf_overlay.c`, `RECOMP_OVERLAY=1`) shows them the same way on all four consoles, under its FPS and CPU lines:

```
SPEED 100 %  59.7/59.7 fps        (GAME 30.0 fps on the Xbox)
FRAME 3.9  EMU 3.1  DRAW 0.4 MS   (work per host frame; emulation and drawing per guest frame)
WORST 9 MS  SLOW 0/30  SHOWN 60   (slowest frame, frames over the budget, pictures per second)
```

Tests: `tests/host_perf_line` (the helper with made-up clocks), `host_layer_test overlay` (the overlay's lines fed by
each console's configuration), and each runtime's headless test checks its run line (`gba_runtime`, `ps1_runtime`,
`n64_runtime`, `fb_present_sdl_test perf`).
