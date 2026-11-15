# Host layer (`xbr_host`)

What every console runtime (Xbox, `consoles/gba`, `consoles/ps1`, `consoles/n64`) shares on the host: window + GL
present, pads, audio out, pacing and frame generation, storage, lifecycle, config, host time and cores, logging.
XBA-145 (C0-1), Plane spec « Spec · Other consoles ».

`src/host/CMakeLists.txt` defines `xbr_host`. Adding that directory alone builds no Xbox source: no XBE, NV2A,
kernel, `win32_compat` or MMIO decode. `tests/host_layer` links only `xbr_host` and checks that at configure time. POSIX
hosts only (Linux, macOS, Android).

| Target | Sources | Defined in |
|---|---|---|
| `xbr_host_core` | `android_log.c`, `recomp_lifecycle.c`, `recomp_quit.c` (+ `host_time`, `host_cpus`, `recomp_cfg`, `recomp_exit_hook`) | `src/platform/host_core.cmake` |
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
| `recomp_storage.[ch]` | M → split | new `recomp_storage_resolve_entry(ext, int, entry, out)` is the generic call (any entry file, or none). `recomp_storage_resolve` = that call with `"default.xbe"`, unchanged |
| `recomp_app.[ch]` | M, kept X | the Android start-up: host parts plus `xbox_path_init` and the kernel fault handler; it stays a thin Xbox wrapper |
| `recomp_pack.[ch]` | M, kept X | pack.json parse/validate/dlopen is generic, the XBE title ID and the exported symbols are Xbox. Generalised with the manifest `console` field (C0-3) |
| `win32_compat.[ch]`, `xbox_winnt.h` | X | Win32/NT vocabulary for lifted Xbox code (`platform`) |
| `mmio_decode*.h` | X | trapped guest MMIO decode |

### src/video
| File | Class | Note |
|---|---|---|
| `host_window.[ch]` | G (new) | SDL window, GL ES 3.0 / 3.2 core context, RGBA or GL-texture present, letterboxed (aspect, integer or stretch scaling, linear or nearest: `host_window_set_scaling`; a fixed display shape such as the PS1's 4:3: `host_window_set_aspect`), a capture of the drawable as presented (`host_window_capture`), events → lifecycle and pads; from any thread, the present rate (`host_window_stats`) and the last picture as a BMP (`host_window_screenshot_bmp`); the shared display keys `RECOMP_SCALE` (aspect, integer or stretch) / `RECOMP_FILTER` / `RECOMP_PRESENT_VSYNC` (`host_window_env_*`, XBA-181); picture shaders none / sharp / scanlines / crt / lcd (`host_window_set_shader`, `RECOMP_SHADER*`, XBA-195, [below](#picture-shaders-xba-195)) |
| `perf_overlay.[ch]` | G | SDL_Renderer overlay; moved from `xbox_video` to `xbr_host` |
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

The console window (`host_window.c`) can draw the picture through one of five presets, our own small fragment shaders
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

« Console lines »: the picture's height divided by `round(height / 240)`, so an upscaled 960-line PS1 picture still
gets 240 scanlines and a GBA 160. Strength, curvature and ghosting are 0..100 (strength unset: the preset's default,
scanlines 50, CRT 60, LCD 50).

API: `host_window_set_shader(&HostShader)` (preset, strength, curvature, ghosting; before or after open, kept across
open / close), `host_window_shader_active()` (the preset drawn: none after a failure), `host_window_shader_parse` /
`_name`, `host_window_env_shader(console_var, def, &out)` and `host_window_shader_env(console_var)`, which the GBA,
PS1 and N64 runtimes call next to `host_window_set_scaling` (`RECOMP_GBA_SHADER` / `RECOMP_PS1_SHADER` /
`RECOMP_N64_SHADER` win over `RECOMP_SHADER`). A program is built on its first draw; one that does not compile or
link is logged once (« picture shader X does not build here; drawing without one ») and none is drawn instead
(`RECOMP_SHADER_TEST_FAIL=<preset>` forces that, for the test). `host_window_screenshot_bmp` stays the raw picture,
without the shader; `host_window_capture` reads the drawable as drawn, shader included. The Xbox presenter
(`fb_present_sdl.c`) has none of this: moving it onto `host_window` (or giving it the same pass) is follow-up work.

Tests: `tests/host_layer`, `host_layer_test shaders` (`host_layer_shaders`, skipped without a display): each preset
drawn into a 640x480 window and read back with `host_window_capture`: scanlines darken rows 2-3 of every 4-row line
and leave the columns alike, strength 0 equals none, the CRT's columns are red / green / blue and its curvature
blacks the corners, the LCD grid darkens each cell's last column and row and keeps its inside, ghosting shows half
of a white picture under the next black one, sharp bilinear blends a black / white edge over fewer columns than
linear at 4.27x, the BMP screenshot is the raw grey, a forced build failure draws exactly what none draws. Run it
under Xvfb + Mesa (`DISPLAY=:N`).

Cost (`HOST_LAYER_BENCH_SIZE=1920x1080 HOST_LAYER_BENCH=100 host_layer_test shaders`, ms per present of a 160x120
picture in a 1920x1080 window, Mesa llvmpipe on one CPU thread, `LP_NUM_THREADS=1`; a software rasteriser, so the
difference to none is the shader's own arithmetic): none 13.2, sharp 17.0, scanlines 15.8, CRT 19.5, LCD with
ghosting 23.8. On a GPU the pass writes the same pixels as none; the shaders add 15 to 40 ALU operations and one
or two texture fetches per output pixel, about 0.1 GFLOP per 1080p frame: well under 0.2 ms on an Adreno 740-class
GPU (AYN Thor), to be confirmed on the device.
