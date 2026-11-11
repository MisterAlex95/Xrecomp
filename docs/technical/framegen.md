# Frame Generation (LSFG)

> Lossless Scaling frame generation on xboxrecomp: a Vulkan compute sidecar
> turns two consecutive frames into an in-between one, and a pacer shows real
> and generated frames at even intervals.
>
> **Read this when:** you work on `src/framegen/`, the presenter's frame
> generation path (`src/video/fb_present_sdl.c`), the launcher's Frame
> generation settings, or a log says `[LSFG] off: ...`.
>
> Part of the [Documentation Index](../INDEX.md)

Built only with `-DXBOXRECOMP_LSFG=ON` (POSIX hosts); the default build and the
upstream tests do not see it. Run-time switches: see
[Environment Variables](../runtime/environment-variables.md#frame-generation).

## Design

Frame generation runs as a **Vulkan compute sidecar**: the GL back end keeps
rendering and the SDL/GLES presenter keeps presenting; a small Vulkan device
(no surface, no swapchain) turns two consecutive frames into the in-between
one(s). Phase 1 is a CPU bridge:

```
flip N (guest frame in memory) -> pacer thread: lsfg_bridge_generate
    upload N into in[N % 2] -> framegen presentContext -> read back G(N-1, N)
-> presenter sink: G due at T + c, N at T + c + P/2 (Pacing below)
```

One upload and one read-back of 1.2 MB per frame at 640x480. Nothing changes in
`src/gpu/`; it works the same with the CPU rasteriser, on Android, macOS
(MoltenVK) and Linux. Zero copy (AHardwareBuffer images shared with the GL back
end) is a later phase.

| Piece | File | Language |
|---|---|---|
| PE resource walker (Lossless.dll RT_RCDATA) | `src/framegen/lsfg_dll.c` | C, libc |
| Shader table, SPIR-V binding renumbering, requirements | `src/framegen/lsfg_spv.c` | C, libc |
| Shader cache (extract, manifest, translate, load) | `src/framegen/lsfg_cache.c` | C, libc |
| DXBC -> SPIR-V over DXVK's compiler | `src/framegen/lsfg_dxbc.cpp`, `dxvk_shim/` | C++17 |
| Vulkan bridge over framegen | `src/framegen/lsfg_bridge.cpp` | C++20 behind `extern "C"` |
| Bridge as the pacer's generator | `src/framegen/lsfg_generator.c` | C |
| Pacer, fake generator, cap rule | `framegen_pacer.c`, `framegen_fake.c`, `framegen_policy.h` | C |
| Listing a DLL's resources (metadata only) | `tools/lsfg/inspect_dll.py` | Python |
| Filling the cache on a desktop | `tools/lsfg/lsfg_translate` | C |
| "Choose Lossless.dll" JNI | `android/app/src/main/cpp/lsfg_jni.c` (`libxba_import.so`), `LsfgNative.java` | C |

### framegen

The LSFG compute chain (mipmaps -> alpha x7 -> beta -> gamma x7 -> delta x3 ->
generate, ~100 dispatches per generated frame, storage images in R8_UNORM,
R8G8B8A8_UNORM and R16G16B16A16_SFLOAT) is the `framegen/` library of
lsfg-vk 1.0 as carried by the Android fork **GameNative/lsfg-vk-android at
commit d33caceb** (MIT, see Licences). We build it unmodified from a checkout
(`XBOXRECOMP_LSFG_VK_SRC`, like `SDL2_SRC`) and call only its public API
(`framegen/public/lsfg_3_1.hpp`, `lsfg_3_1p.hpp`): the fork's
`initializeExternal` (framegen on *our* device and queue) and
`createContextFromImages` (plain VkImages in GENERAL layout), then
`presentContext(id, -1, {})` per frame, `deleteContext`, `finalize`. Inputs are
two images used alternately: framegen's frame counter k reads `in[k % 2]` as the
newest frame. framegen asks for each shader by name (`"alpha[0]"`,
`"p_generate"`, ...) through a loader callback; the bridge's loader reads the
shader cache.

### Bridge

`lsfg_bridge.h`: `lsfg_bridge_open(cfg)`, `lsfg_bridge_generate(frame, pitch,
out[], out_pitch)`, `lsfg_bridge_reset`, `lsfg_bridge_close`, plus `probe`,
`bench`, `get_info`, `last_gen_us`, `save_pipeline_cache`. All calls from one
thread (the pacer's).

- **Vulkan through volk** (dlopen of libvulkan / MoltenVK): nothing links
  libvulkan, so a binary with the bridge starts on a machine without Vulkan
  and `open` returns `LSFG_E_NOLOADER`. On macOS the instance enables
  `VK_KHR_portability_enumeration`, the device `VK_KHR_portability_subset`.
- **Device**: Vulkan 1.1+, a compute queue, `shaderStorageImageExtendedFormats`,
  storage + sampled support for the three formats above; enabled only what the
  device reports: read/write without format, `shaderFloat16`,
  `timelineSemaphore`, `vulkanMemoryModel`, `shaderInt16`.
- **Shader check before Vulkan sees a module**: open loads the whole variant
  from the cache (renumbered and checked against the pass layouts) and compares
  each module's SPIR-V version and capabilities with the device. Translated
  DXBC is SPIR-V 1.6 with VulkanMemoryModel and StorageImageWriteWithoutFormat,
  so it needs Vulkan 1.3 + `vulkanMemoryModel` (Adreno 7xx, MoltenVK 1.3 have
  both); otherwise `LSFG_E_FEATURE` with the missing items.
- **Images**: in[2] and out[multiplier - 1], R8G8B8A8_UNORM, STORAGE | SAMPLED
  | TRANSFER_SRC | TRANSFER_DST, GENERAL. The guest frame is uploaded as is
  (BGRX into RGBA: the swap cancels on the way back); 16 bpp frames are
  expanded by the pacer.
- **Per frame**: upload (barrier compute -> transfer, copy, transfer ->
  compute), `presentContext`, read-back (compute -> transfer, copy, transfer ->
  host) with one fence, bounded 2 s waits (`LSFG_E_TIMEOUT`). The first frame
  after open/reset, and any call with `out == NULL`, only enters the history
  (`LSFG_PRIMED`).
- **VkPipelineCache**: framegen builds its pipelines with no cache; volk's
  dispatch pointers are globals, so while the bridge is open
  `vkCreateComputePipelines` is wrapped to pass ours. Loaded from and saved to
  `<shader dir>/pipeline-<vendor>-<device>-<driver>.bin` (header checked:
  vendor, device, pipelineCacheUUID), saved after the pipelines are built and at
  close.
- **Errors**: every C++ exception is caught at the boundary and becomes an
  `LSFG_E_*` code with a one-line message; after a generate failure the bridge
  stays failed until closed (the pacer falls back, Pacing below).

## Shader cache

Lossless.dll (the user's own copy, from their Steam install of Lossless
Scaling) carries the LSFG shaders as RT_RCDATA resources. The IDs framegen
needs (fixed table, the one lsfg-vk 1.0 used, in `lsfg_spv.c`): 255..279 for
LSFG 3.1, 255, 256 and 280..302 for 3.1 performance, 25 per variant, 48
distinct. Alex's DLL (2026-11-11): 202 RCDATA resources, IDs 101..302, **all
DXBC**; the SPIR-V some builds reportedly carry at ID + 49 (FP16) / + 98 (FP32)
is absent. The cache handles both.

`lsfg_cache.h` describes the layout: `<base>/<key>/` with `<id>.dxbc` (kept, so
a translation can run later without the DLL), `<id>.fp16.spv` / `<id>.fp32.spv`
(embedded SPIR-V), `<id>.spv` (translated), `manifest.txt`, the pipeline cache;
`<base>/current` names the active key (first 16 hex digits of the DLL's
SHA-256). Extraction goes through `<key>.partial` and a rename. The version
check: one variant complete in one form, else "this Lossless.dll version is
not supported". `<base>` is `getFilesDir()/lsfg` on Android (excluded from
backups and device transfers by `res/xml/backup_rules.xml` and
`data_extraction_rules.xml`, refused by `LogProvider`; Android below), the
desktop default otherwise (`RECOMP_LSFG_CACHE`).

**Binding renumbering.** framegen numbers each pass's descriptor bindings
0..N-1 over its declared type list: uniform buffers, samplers, sampled images,
storage images (`framegen/src/core/shadermodule.cpp`, counts in
`framegen/v3.1*_src/shaders/*.cpp`). Compiled HLSL carries register slots
instead. `lsfg_spv_renumber` classifies every resource variable by its SPIR-V
type, sorts each class by (set, binding) and numbers class after class, set 0,
then checks the counts against the pass layout. The cache stores the
translator's output and the bridge renumbers again at load (idempotent).

### DXBC translation

DXBC needs a DXBC -> SPIR-V compiler. We use **DXVK's** (`src/dxbc`, zlib),
as lsfg-vk 1.0 did with its DXVK subset: `lsfg_dxbc.cpp` reads the container,
compiles it with default options and returns the SPIR-V
(`lsfg_translate_fn`, the contract is in `lsfg_cache.h`). Pinned at **DXVK
v2.6.2**: 2.7 moved samplers to a global descriptor heap, which framegen's
fixed per-pass layouts cannot express. DXVK is used from a checkout
(`XBOXRECOMP_LSFG_DXVK_SRC` / `$DXVK_SRC`, with its `include/spirv` submodule),
never vendored. DXVK's `src/dxbc` includes `../dxvk/dxvk_shader.h` (the whole
Vulkan back end), so `lsfg_dxbc.cmake` stages `src/dxbc`, `src/spirv` and
`src/util` into the build tree beside our reduced stand-ins
(`src/framegen/dxvk_shim/dxvk/`: the few DXVK types the compiler uses, a
logger instance, `getEnvVar`; a DXVK derivative under zlib, marked as altered).

DXVK's compiler emits per-class binding ranges (cbuffers from 800, samplers 816,
SRVs 832, UAVs 1088 for compute), which the renumbering maps to framegen's
order. Measured on Alex's DLL (Mac, 2026-11-11): 48/48 shaders translated,
every output SPIR-V 1.6 with the right resource counts for its pass, 4.7 MB in
all, 0.4 s.

```
cmake -S tools/lsfg -B build/lsfg -DXBOXRECOMP_LSFG_DXVK_SRC=$DXVK_SRC \
      -DXBOXRECOMP_VULKAN_HEADERS=<dir with vulkan/vulkan_core.h>
build/lsfg/lsfg_translate /path/to/your/Lossless.dll       # -> the desktop cache, made current
build/lsfg/lsfg_translate --dir <cache dir pulled from a phone>
```

On Android the translator links into `libxba_import.so` (about 1.3 MB stripped,
arm64) when the APK is built with `-PxbaLsfg=1` and `DXVK_SRC`, so the phone
translates at import (`LsfgNative.hasTranslator()`); without it the app only
extracts, and a cache translated on a desktop can be pushed.

## Build

| Build | What |
|---|---|
| default | nothing of this (`XBOXRECOMP_LSFG` OFF): default build and upstream tests unchanged |
| `-DXBOXRECOMP_LSFG=ON` | `xbox_framegen`: pacer, fake generator, shader-cache helpers (C only) |
| + `-DXBOXRECOMP_LSFG_VK_SRC=<checkout>` (or `$LSFG_VK_SRC`) | + the bridge (C++20), volk, framegen; `XBOXRECOMP_LSFG_BRIDGE=1` on `xbox_framegen`; call `lsfg_bridge_register()` before `framegen_pacer_start()` |
| + `-DXBOXRECOMP_LSFG_DXVK_SRC=<checkout>` (or `$DXVK_SRC`) | + `lsfg_dxbc` (the translator) |
| Android `-PxbaLsfg=1` (+ `-PlsfgVkSrc`, `-PdxvkSrc` or the env vars) | the same in `libmain.so` / `libxba_import.so`; the launcher shows Frame generation only with the bridge (`R.bool.xba_lsfg`) |

Sources:

```
git clone https://github.com/GameNative/lsfg-vk-android && git -C lsfg-vk-android checkout d33caceb
git -C lsfg-vk-android submodule update --init thirdparty/volk     # volk be3dbd49, the fork's own pin
git clone --branch v2.6.2 --depth 1 https://github.com/doitsujin/dxvk && \
    git -C dxvk submodule update --init --depth 1 include/spirv
```

Only `framegen/` and `thirdparty/volk` of the fork are compiled; its
pe-parse, dxbc, toml11 submodules and its layer are not used. Vulkan headers:
the NDK sysroot on Android; elsewhere `XBOXRECOMP_VULKAN_HEADERS` (a directory
holding `vulkan/vulkan_core.h`: Vulkan-Headers' `include`, or the NDK's
`sysroot/usr/include`, whose `vulkan/` and `vk_video/` alone are staged into
the build tree) or a Vulkan SDK. Android: `VK_USE_PLATFORM_ANDROID_KHR` (volk's
public define) for framegen's AHardwareBuffer paths; libc++ static
(`c++_static`) in `libmain.so` and `libxba_import.so`; SDL2's hidapi brings its
own static libc++ into `libSDL2.so`, which is fine as no C++ object crosses
the library boundary. Measured arm64 Release sizes, stripped: bridge +
framegen + volk 1.6 MB, translator 1.3 MB; neither needs `libvulkan.so`.

A project that adds the toolkit with `add_subdirectory` enables CXX itself
when `XBOXRECOMP_LSFG` is on (`templates/new-game` does).

## Pacing

The pacer (`src/framegen/framegen_pacer.c`, C, libc + pthreads) sits between
the presenter and the generator. The presenter hands it every real frame at its
flip (`framegen_pacer_submit`, CLOCK_MONOTONIC flip time); the pacer's thread
asks the generator for the in-between frame(s) and hands real and generated
frames back to the presenter's sink (`FramegenSink.show`) with the time each is
due on screen. The presenter keeps a timed FIFO and shows each entry at its due
time (Presenter below).

### Interfaces

- **Generator vtable** (`FramegenGenerator` in `framegen_pacer.h`): `open(w, h,
  multiplier)`, `generate(frame, pitch, out, out_pitch)`, optional `reset`,
  `close`; all called from the pacer thread. The generator keeps the previous
  frame itself. `generate` with `out == NULL` only pushes the frame into the
  history (first frame, skipped pairs); otherwise it writes `multiplier - 1`
  frames (G at t = k / multiplier) and returns that count, 0 if it had no
  previous frame, < 0 on failure. `open` failing fills a one-line reason. This
  maps 1:1 onto `lsfg_bridge_open` / `lsfg_bridge_generate` /
  `lsfg_bridge_close`; the bridge registers itself with
  `framegen_pacer_set_generator`. No Vulkan in the pacer.
- **Fake generator** (`framegen_fake.c`, `framegen_fake_generator`): CPU linear
  blend, `(prev * (M - k) + cur * k) / M` per byte. Used by
  `tests/framegen_pacer` and, under `RECOMP_LSFG_FAKE=1`, by headless runs.
- **Pixels**: everything the sink receives is 32 bpp BGRX, packed.
  `framegen_pacer_submit_format` takes `FRAMEGEN_FMT_BGRX8888` (the guest's
  X8R8G8B8, desktop GL read-backs), `RGBX8888` (GLES read-backs), `R5G6B5` and
  `X1R5G5B5`, up to 8192 x 8192, and converts at submit (the copy it makes
  anyway), so the generator and the presenter's texture see one format.
  `framegen_pacer_submit(bpp)` is the 4 = BGRX / 2 = R5G6B5 shorthand.
- **Statistics**: `framegen_pacer_stats` gives real fps, shown fps (x 100) and
  mean generation cost (us), latched about once a second, for the performance
  overlay; `framegen_pacer_counters` gives the raw counters (tests, logs).

### Rules

For real frame N flipped at T, multiplier M (2 by default):

| Rule | What |
|---|---|
| Period | P = moving average of the last 8 flip intervals, clamped to [cap period, 2 x cap period]. A slow frame moves P with it. |
| Due times | Generated frame k of G(N-1, N) at T + c + (k - 1) P / M, N at T + c + (M - 1) P / M, c = this pair's measured generation cost. For 2x: G at T + c, N at T + c + P/2. |
| Skip | An interval beyond 2 x cap period (stall, loading screen, pause without a reset): no generation for that pair, N due at T, the interval is not averaged; N still enters the generator's history. |
| Drop | When an entry is handed over more than P after its due time: a generated one is dropped; a real one only if a newer real frame is already queued (a real frame is never dropped for nothing). The input queue holds 2 frames; a third pushes the oldest out. |
| Reset | `framegen_pacer_reset()` (lifecycle pause / resume) drops queued frames and forgets the timing and generator history: the next frame only primes, so no frame from before the pause is blended with one after it. |
| Fallback | 30 consecutive generations failed or over budget (budget = cap period / M, 16.7 ms at 30 fps 2x; `RECOMP_LSFG_BUDGET_MS`), or `open` failing: one `[LSFG] off: <reason>` line, then pass-through (every frame due at its flip). `framegen_pacer_active()` returns 0 so the presenter can go back to its own path. One good frame restarts the count. |

Cap period = 1 / effective cap (below). Due times are host CLOCK_MONOTONIC
nanoseconds.

### Cap and present mode

`src/framegen/framegen_policy.h` (header only, so the executor and the GL back
end need not link the framegen library; included only in an
`XBOXRECOMP_LSFG` build, applied only under `RECOMP_LSFG=1`):

- **Frame cap**: the executor's `gpu_fps_cap()` (`nv2a_pb_exec.c`) passes its
  value through `framegen_policy_fps_cap`: effective cap = min(cap, display
  rate / multiplier), and a cap of 0 (off) becomes display rate / multiplier.
  60 Hz at 2x gives 30 real fps for 60 shown. Display rate:
  `RECOMP_LSFG_DISPLAY_HZ` (default 60). The windowed presenter reads the
  display's mode before the guest starts and passes a rate **below** 60 Hz on
  (a 50 Hz display caps at 25); at 120 Hz it keeps 60, since a cap of 60 real
  fps would put a title that runs at 30 (most do) over the budget and past the
  skip threshold every other frame, while 60 shown fps sit evenly on 120 Hz.
  Log: `[LSFG] display 120 Hz; pacing for 60 Hz`. Cap log:
  `[GPU] back end frame cap 30 fps for frame generation (was 60)`.
- **Present mode**: the GL back end (`nv2a_gl.c`) forces
  `RECOMP_GPU_PRESENT=sync`, since the asynchronous read-back delivers frame N
  at flip N+1 and the pacer times frames by their own flip. Log:
  `[GPUGL] present read-back sync (frame generation)`.
- `RECOMP_FLIP_PACING` (the guest vblank flip hold) is independent.

Both are read once at start-up; if the pacer falls back later, the cap and the
sync read-back stay for the run (30 fps is the Android profile's cap anyway).

**Titles locked to the flip rate.** A title whose game logic advances once per
flip (Burnout 3) runs in slow motion under the 30 fps cap. Its profile forces
frame generation off with `"env_force": {"RECOMP_LSFG": "0"}` in
`titles/<title id>/title.json`: `xba_title_configure` is too late for it (the
presenter and the pacer read `RECOMP_LSFG` before the guest starts), so the
option travels in `pack.json` on Android (set by the pack loader after
`recomp.cfg`) and in the generated `src/title/env_force.h` on the desktop (set
by `main()`). Log: `[TITLE] 4541005B forces RECOMP_LSFG=0 (title profile)`.
See [Environment variables](../runtime/environment-variables.md#forced-by-a-title).

### Tests

`tests/framegen_pacer` (C, POSIX, built on its own like the other test
projects, no `XBOXRECOMP_LSFG` needed): fake generator plus a fake clock
(cost 5 ms per pair unless a case changes it), scripted flips:

- steady 30 fps, 41 frames: G due T + 5 ms, N due T + 5 ms + 16.67 ms, blends
  exact; stats 30.00 real fps, 59.7 shown (1 s window), 5000 us;
- a 50 ms frame after eight 33.3 ms ones: P = 35.42 ms, N at T + 22.71 ms;
- 70 ms gap: skipped, N due at T, not averaged; exactly 2 x cap period still
  generates;
- frame picked up 40 ms late: G dropped, N shown; 60 ms late with nothing
  newer: N still shown; threaded run with a stuck sink: 1 input pushed out,
  2 generated and 1 real frame dropped, newest real frame shown;
- 29 over-budget frames, a good one, 30 more: off after the 30th in a row,
  then pass-through; 30 failed generations; open failure;
- pause: 2 s gap without reset skipped; with `framegen_pacer_reset()` the next
  frame primes, the one after blends only post-pause frames, P back to the cap
  period;
- 3x multiplier (due times P/3 apart, blends 1/3 and 2/3), R5G6B5 input,
  the cap rule (30/60/0 at 60 Hz 2x -> 30, 144 Hz -> 72), and
  `framegen_pacer_start` from the environment with `RECOMP_LSFG_FAKE=1`.

Clean under ThreadSanitizer and UBSan (30 runs).

## Presenter

`src/video/fb_present_sdl.c`, built with `XBOXRECOMP_LSFG` (the root
CMakeLists defines it on `xbox_video` and links `xbox_framegen`), active under
`RECOMP_LSFG=1`:

- **Start**: `xbox_HostWindowMain` calls `lsfg_bridge_register()` when the
  bridge is built (`XBOXRECOMP_LSFG_BRIDGE`), then `framegen_pacer_start()`,
  which picks the fake generator under `RECOMP_LSFG_FAKE=1` and otherwise the
  registered one (none: `[LSFG] off: no generator in this build`). Windowed:
  after the window exists, before the guest starts. Headless
  (`RECOMP_FB_WINDOW=0`): before the guest, with a sink that only counts.
  The generator opens at the first frame, on the pacer thread (device and
  pipelines: 10-170 ms with the pipeline cache, about 2 s cold for the
  performance set); frames that arrive meanwhile replace each other in the
  input queue.
- **Feeding**: both present paths hand their frame to
  `framegen_pacer_submit_format()` instead of a slot while
  `framegen_pacer_active()`: the guest flip (640x480, X8R8G8B8, R5G6B5 or
  X1R5G5B5 as `xbox_FramebufferWindowSetFormat` says) and the GL back end's
  host frames (`xbox_FramebufferWindowPresentHost`, BGRX on desktop GL, RGBX on
  GLES, at the internal resolution). The flip time is `CLOCK_MONOTONIC` at the
  call: the read-back is synchronous under frame generation. A size change
  (guest frames, then host frames at `RECOMP_GPU_SCALE`) closes and reopens
  the generator at the new size; the frame after primes it.
- **Timed FIFO**: the sink (pacer thread) copies each frame into a ring of 4
  entries (pixels, size, due time; a full ring loses its oldest entry) and
  wakes the main loop. The loop sleeps in `SDL_WaitEventTimeout` until the next
  event or the next due time (whole ms, the last ms counts as due), takes the
  newest due entry (older due ones are skipped: "late"), uploads it as an
  XRGB8888 texture and presents. With vsync on, the present itself waits for
  the display.
- **Aspect modes** ([GPU back end](gpu-backend.md#aspect-modes)): each real
  frame is classed (4:3, 16:9 stretch, letterbox crop) when it is submitted;
  each FIFO entry, real or generated, takes the latest class when it reaches
  the sink, and the main loop shows and screenshots it in that shape.
- **Overlay**: `perf_overlay_set_framegen()` adds a line
  `LSFG <real> / <shown> FPS  GEN <ms> MS` (`LSFG OFF` after a fallback),
  drawn after the picture on every shown frame, never generated. The title
  bar's FPS stays the real (flip) rate.
- **Lifecycle**: a listener (`recomp_lifecycle_listen`) calls
  `framegen_pacer_reset()` and clears the FIFO on pause and on resume.
- **Fallback**: once `framegen_pacer_active()` is 0, the next flip goes to the
  slots again; the main loop logs `[FBWIN] frame generation off (<reason>);
  normal present` and on Android shows one toast.
- **Logs**: `[LSFG] real / shown fps, generation ms, frames ...` every 5 s
  (headless: every 5 s of due time, and at exit; windowed: plus the window's
  presented / FIFO-full / late counts) and when the window closes.
- **Screenshot** (`xbox_FramebufferDumpBmp`, the companion's): the frame on
  screen, the FIFO's last shown entry.
- Closing the window stops the pacer (and closes the bridge) before SDL goes.

Verification on the Mac (M1 Max, MoltenVK from an app bundle through
`DYLD_LIBRARY_PATH`, other jobs running; Fable, `RECOMP_GPU=1 RECOMP_LSFG=1
RECOMP_LSFG_PERF=1 RECOMP_LSFG_DIR=<Alex's translated cache>`, intro and title):

| Run | Real fps | Shown fps | Generation | Notes |
|---|---|---|---|---|
| headless, 60 s | 29.9-30.3 | 59.6-60.5 | 10.0-10.5 ms | 0 failed; 118 inputs replaced while the device opened |
| windowed, 45 s | 30.0-30.1 | 59.7-60.4 | 10.0-11.0 ms | window presented 2318 of 2336 handed over (17 late), FIFO never full |
| headless, `RECOMP_LSFG_FAKE=1` | 30.0 | 60.0 | 0.85 ms | |
| wrong `RECOMP_LSFG_DIR` | - | - | - | `[LSFG] off: lsfg: <dir>: no shader`, the game goes on |

Generation here includes upload and read-back on a loaded machine (the
bridge benchmark alone: 4.4 ms). Before the display-rate rule above, a 120 Hz
Mac display gave a cap of 60: shown 44-48 fps, half the pairs skipped.

## Android

- **Build**: `-PxbaLsfg=1 -PlsfgVkSrc=<lsfg-vk-android checkout> -PdxvkSrc=<dxvk
  v2.6.2>` (or `LSFG_VK_SRC` / `DXVK_SRC`) is a supported configuration; see
  [App shell](../android/app-shell.md#frame-generation) for the full command.
  Without `lsfgVkSrc` gradle warns and the launcher hides the rows; without
  `dxvkSrc` the phone only extracts (a desktop-translated cache can be pushed
  into the directory).
- **Launcher**: Settings › Graphics › **Frame generation**: Off (default) /
  2× / 2× performance writes `RECOMP_LSFG`, `RECOMP_LSFG_MULT`,
  `RECOMP_LSFG_PERF` to `recomp.cfg`. **Choose Lossless.dll** below it opens
  `ACTION_OPEN_DOCUMENT`; the picked document's fd goes to
  `LsfgNative.importDll(fd, filesDir/lsfg, ...)` on a worker thread (the DLL is
  read through the fd into memory, never copied into the app's storage), and
  the row then says "Shaders ready (LSFG 3.1)", "This Lossless.dll version is
  not supported" or "translator missing". When shaders are usable (or DXBC is
  there for a later translation) `RECOMP_LSFG_DIR=<filesDir>/lsfg/<key>` goes
  into `recomp.cfg`. Strings in English and French. When the selected game's
  pack forces `RECOMP_LSFG` off (`pack.json` `env_force`, from its title
  profile), the row stays (the setting is global) but is dimmed, and its
  description (shown when it is focused) says « Frame generation unavailable
  for this title » and why (`Pack.forcesOff`).
- **Storage**: `files/lsfg` is excluded from backups (`allowBackup="false"`,
  plus `fullBackupContent` and `dataExtractionRules`, which also cover a
  device-to-device transfer) and never served by `LogProvider`.

## Measurements

Mac (Apple M1 Max, MoltenVK 1.3 from an existing app bundle via
`DYLD_LIBRARY_PATH`, nothing installed; other jobs running), Alex's DLL
translated, 2x, `tests/lsfg_bridge`:

| | 640x480 | 1280x960 | 1920x1080 |
|---|---|---|---|
| LSFG 3.1, generate (GPU) | 17.8 ms | 40.4 ms | 62.1 ms |
| LSFG 3.1 performance | 4.4 ms | 8.0 ms | 11.5 ms |

Round trip (upload + generate + read-back) adds 0.1-0.6 ms. Pipelines: 150 ms
cold, 9 ms with the pipeline cache (1.6 MB); the performance set 1.9 s cold.
Moving square (48 px, 48 px per frame): generated centre within 0.5 px of the
midpoint every step. MoltenVK numbers say little about Adreno: the device
benchmark is Device checklist step 3.

## Licences

| Component | Licence | How it is used |
|---|---|---|
| framegen (lsfg-vk 1.0, GameNative/lsfg-vk-android d33caceb, `framegen/`) | MIT, (c) 2025 lsfg-vk | compiled from a checkout, unmodified; in binaries (NOTICE) |
| volk (zeux/volk be3dbd49, the fork's submodule) | MIT, (c) 2018-2025 Arseny Kapoulkine | same |
| DXVK v2.6.2 (`src/dxbc`, `src/spirv`, `src/util`) | zlib, (c) 2017 Philip Rebohle and others | compiled from a checkout; `src/framegen/dxvk_shim/` is a DXVK derivative in this repo (zlib, altered) |
| SPIRV-Headers (DXVK's `include/spirv`) | MIT-style (Khronos) | headers only |
| Vulkan headers (NDK / Vulkan-Headers) | Apache-2.0 / MIT | headers only |
| MoltenVK (macOS) | Apache-2.0 | user-installed or already present, never bundled |

**Not used:** lsfg-vk 2.x (CC BY-NC-ND 4.0: no derivatives), the fork's code
outside `framegen/` and `thirdparty/volk` (its layer, extractor, translator
glue), pe-parse, PancakeTAS/dxbc, toml11, and LSFG-Android-Application (GPLv3
with extra terms: read for nothing, copy nothing).

## Legal rules

- **Lossless.dll is the user's.** Never download, commit, bundle (APK, CI
  artefact, Docker image) or upload it, its shaders, or anything derived from
  it (extracted DXBC, translated SPIR-V, pipeline caches). It stays where the
  user put it; the app reads it once through the picker's fd and keeps only
  the shader cache in its private storage.
- Logs and tools print metadata only: IDs, sizes, kinds, counts, hashes'
  first 16 hex digits. Never shader contents.
- Tests use synthetic inputs built by the tests (`tests/lsfg_dll`,
  `tests/lsfg_spv`, `tools/lsfg/test_inspect_dll.py`); `tests/lsfg_bridge`
  skips (exit 77) unless the developer points it at their own cache or DLL.
- Code comes only from our repository, the MIT `framegen/` tree (and volk) of
  the pinned fork, and DXVK (zlib) as described above.

## Tests

| Test | What | Needs |
|---|---|---|
| `tests/lsfg_dll` | PE walker on synthetic PE32/PE32+ (all truncations, 15 malformations), SHA-256 vectors, cache import / reuse / translate with a fake translator, unsupported DLL, damaged cache file | nothing (C, POSIX) |
| `tests/lsfg_spv` | renumbering order, sets, idempotence, layout mismatch, unsupported resources, corrupt modules, capabilities, every table layout | nothing |
| `tools/lsfg/test_inspect_dll.py` | inspect_dll on synthetic PE | pytest |
| `tests/lsfg_bridge` | argument checks; moving square (generated square between the inputs), history-only and reset, pipeline cache reuse; `--bench [frames]` | `LSFG_VK_SRC` at build time; a Vulkan driver and `RECOMP_LSFG_DIR` (or `RECOMP_LSFG_DLL` + the translator built in) at run time, else exit 77 |
| `tests/framegen_pacer` | pacing (below) | nothing |

