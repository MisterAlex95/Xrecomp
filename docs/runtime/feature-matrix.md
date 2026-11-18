# Feature matrix: every console, every host

XBA-274 (Alex: « je veux que toutes les plateformes soient uniformes en termes de features »). This page says, from
the code, which player-facing feature each console has, at runtime (the C runtime) and in the front ends (the Android
app, the desktop `xrecomp` CLI, the macOS launcher). Each cell names the file that proves it; « UI » means the
Android app (`android/app/src/main/java/fr/adserver/xrecomp/`, written `app/` below).

**yes**: the console has it, runtime and Android UI. **partial**: something is missing (said in the cell). **no**:
missing; **n/a**: it means nothing for that console (the reason is given). Audited on main at 0.13.5
(2026-11-18); each XBA-274 commit that fills a gap updates its cell here.

Consoles: Xbox (`src/`), Game Boy Advance (`consoles/gba`), PlayStation (`consoles/ps1`), Nintendo 64
(`consoles/n64`), GameCube (`consoles/gc`), Xbox 360 (`consoles/x360`, XBA-280: in the app; XBA-282: prepared on the
device). The spikes (DS, PSP, PS2, Dreamcast, 3DS, Win32) are not in the app and not in this matrix.

## Runtime and Android app

| Feature | Xbox | GBA | PS1 | N64 | GameCube | Xbox 360 |
|---|---|---|---|---|---|---|
| Internal resolution (upscaling) | yes: `src/gpu/nv2a_gl.c` (`RECOMP_GPU_SCALE`, else `RECOMP_UPSCALE`); UI `app/GameSettings.appliesTo` | n/a: the PPU draws 240 x 160 in software; window scale and shaders instead | yes: `consoles/ps1/runtime/ps1_gpu.c` `ps1_gpu_set_upscale` (shadow VRAM 1/2/4x) | partial: `consoles/n64/runtime/n64_rdp_select.c` `n64_rdp_upscale_env`, the Vulkan RDP only (the software RDP draws native) | yes: `consoles/gc/runtime/gx/gx_gl.c` `gx_gl_attach` (`RECOMP_GC_UPSCALE` / `RECOMP_UPSCALE`, 1..8x, GX renderer); UI `app/ConsoleVideo.applies` (XBA-274: was hidden) | yes: `x360_runtime.c` (`RECOMP_X360_UPSCALE` / `RECOMP_UPSCALE`, 1..8x: every eDRAM surface scaled, resolves keep their host picture, `xenos_gl.c`); UI `app/ConsoleVideo.applies`, `app/GameSettings.appliesTo` (XBA-282) |
| Fast-forward | yes: guest clock x2..16, `src/kernel/kernel_hal.c` `xbox_FastForwardPoll`; menu `app/GameMenu` (`fastForwardSupported`) | yes: frames per shown frame, `gba_runtime.c` (`recomp_ff_hold`) | yes: `ps1_runtime.c` | yes: `n64_runtime.c` | yes: `gc_runtime.c` | yes: `x360_runtime.c` (`recomp_play`) |
| Save states in a run | partial: `src/kernel/xbox_state.c` + `xbox_state_service.c` (`recomp_states_attach "xbox"`); a load needs every guest thread parked at the same call (else ECHAIN); not built on Windows (`kernel_bridge.c` `#ifndef _WIN32`); UI asks the runtime (`app/ShellActivity.statesSupported`) | yes: `gba_state.c` | yes: `ps1_state.c` | yes: `n64_state.c` | yes: `gc_state.c` | yes: `x360_state.c`, `x360_runtime.c` (`recomp_states_attach "x360"`); the eDRAM is not saved (redrawn, as the GameCube's EFB) |
| Across runs (« Resume where I left off ») | partial: `xbox_state_service.c` `xbox_StateResumeAtBoot`, only with resume entries in the generated code (`app/Pack.resume`: PREPARE_AGAIN otherwise) | yes: `recomp_states_resume` (`src/platform/recomp_states.c`), `gba_runtime.c` | yes: `ps1_runtime.c` | yes: `n64_runtime.c` | yes: `gc_runtime.c` | yes: `x360_runtime.c` (`recomp_states_resume`) |
| partial: on by default, time-based (`xbox_state_service.c`, `recomp_rewind_config("xbox")`), when the title's states work, and it turns itself off when one state (its non-zero RAM pages and devices) does not fit half the ring (`RECOMP_REWIND_MB`, default 64: a title filling its 64 MB needs 128 or 256); hotkey with states (`app/Hotkeys`); a game's own Rewind / Rewind memory rows (`app/GameSettings.appliesTo`, XBA-274: were hidden for Xbox titles) | yes: on by default (`src/platform/recomp_rewind.c`) | yes: on by default | yes: on by default since XBA-278 (a fixed-layout 8-9 MB capture, `n64_state_save_rewind`; ~1.5 ms each on the homebrew) | yes: on by default since XBA-278 (a fixed-layout 41 MB capture, `gc_state_save_rewind`; ~4.5 ms each, so one every 5-6 fields: `recomp_rewinder_frame` keeps captures at 1 ms per field on average; 64 MB keeps a few seconds) | yes: on by default (a capture is a whole state: the title's memory, XOR-delta coded) |
| Cheats | yes: `src/kernel/xbox_cheats.c` (per vblank); UI `app/Cheats.checkXbox` | yes: `gba_cheats.h` | yes: `ps1_runtime.c` | yes: `n64_runtime.c` | yes: `gc_runtime.c`; UI `app/Cheats.checkGc` | partial: raw big-endian codes (`RECOMP_CHEAT_X360`, `x360_cheat_read32` / `write32` on allocated memory), data only (lifted code is ahead of time); UI `app/Cheats.checkRaw` (XBA-282: its own shape and hint) |
| Frame generation | yes: `src/video/fb_present_sdl.c` (framegen pacer) | yes: `src/video/host_window.c` `host_window_present_rgba` -> `fg_present` | yes: same path | yes: `host_window_present_rgba_shared` | yes: the GX and GPU-YUV pictures (`host_window_present_texture`) are read back for the pacer while frame generation runs (XBA-274, `host_window.c` `texture_pixels`; ctest `gc_framegen`) | partial: the same `host_window_present_texture` path as the GameCube's; not checked on a device |
| Perf line | yes: `fb_present_sdl.c` `perf_overlay_tick`; menu `app/GameMenu` (Item.PERF, every console) | yes: `host_window.c` overlay + `host_perf` | yes | yes | yes | yes: `x360_runtime.c` (`host_perf`, « x360: perf ») |
| Pad remapping | yes: `RECOMP_PAD_MAP` (`src/input/xinput_device.c`), UI `app/PadMapping` | yes: `console_pad` (`RECOMP_GBA_PAD_MAP`), UI `app/ConsoleControls` | yes | yes (+ stick range / dead zone) | yes: runtime `gc_runtime.c` `console_pad_load` + `gc_pad_map.c`; UI `app/ConsoleControls` (XBA-274: the Controls screen used to show the N64's 14 targets over the GameCube's 12, `targetLabel` / `groups` default branch) | yes: `console_pad` `CONSOLE_PAD_X360` (`RECOMP_X360_PAD_MAP`), `x360_pad_map.c`; UI `app/ConsoleControls` |
| Rumble | yes: `src/usb/ohci.c` -> `xbox_InputSetState`; `RECOMP_RUMBLE` (`app/RecompConfig`, shared) | n/a: no motor on a GBA pad (rumble carts are cartridge hardware, not emulated) | yes: DualShock, `ps1_runtime.c` | yes: Rumble Pak, `n64_runtime.c`, UI `app/N64Pak` | yes: SI motor, `gc_runtime.c` `rumble()` | yes: `XamInputSetState`'s motors (`x360_input.c`) to `host_pad_rumble` (`x360_runtime.c` `rumble()`) |
| On-screen (touch) pad | yes: `app/TouchPad` Layout.XBOX | yes: Layout.GBA | yes: Layout.PS1 / PS1_ANALOG | yes: Layout.N64 | yes: Layout.GC (XBA-274: both sticks, the cross, A B X Y, Z, L / R; the Xbox's layout before) | partial: the Xbox layout (`app/TouchPad` default) |
| Hotkeys (states, slots, fast-forward, rewind) | yes: pad chords and, since XBA-274, the window's keys (`src/usb/usb_gamepad.c` `pad_hotkeys`, keys by HID usage from `src/input/keyboard_sdl.c`; the state ones when the title's states work; not on Windows, no states there); the launcher's Xbox Controls lists every chord (`app/Hotkeys.Action.appliesTo`, XBA-274) | yes: pad + keyboard (`src/platform/recomp_hotkeys.c`, `gba_runtime.c`) | yes (+ F6 next disc) | yes | yes | yes: pad + keyboard (`recomp_play`) |
| Audio | yes: `src/apu/apu_sdl2.c`; volume `app/GameMenu` (every console) | yes: `gba_audio.c` | yes: `ps1_spu.c` | yes | yes: AI / DSP / AX | partial: the render driver's mix to `host_audio` (`x360_xma.c` `x360_audio_pull`); XMA voices are silent (no WMA Pro decoder: `x360_xma_set_backend`, Alex's decision on XBA-280) |
| Shader cache / warm-up | partial: runtime program cache (`nv2a_gl.c`, `RECOMP_GPU_SHADER_CACHE`), no app warm-up | n/a: software PPU | n/a: software GPU | yes: `n64_vk_cache.c`, warm-up `app/ShaderWarmup` | yes: `gx/gx_progcache.c`, warm-up `app/GcShaderWarmup` | partial: translated at Prepare time on the device (XBA-282, `tools/prepare/x360.py`: the shaders the runs met, dumped to `work/x360-<KEY>/shaders`, go into the pack's `shaders/`; `app/Pack.newShaders` offers Prepare again); a shader first met in a run is skipped until then; memory export shaders run where GLES 3.1 has vertex storage blocks (checked at run time, `xenos_gl.c` `memexport_caps`), skipped elsewhere |
| Texture filtering | yes: `RECOMP_TEX_BILINEAR` / `ANISO` (`nv2a_gl.c`), UI OPT_TEX | n/a: a 2D picture, the window filter (`RECOMP_GBA_FILTER`, default nearest) | partial: the window filter only (software GPU, no texture override) | partial: the window filter only | partial: the window filter only (`RECOMP_GC_FILTER`); no texture override in `gx_tex.c` | yes: the window filter (`RECOMP_X360_FILTER`) and the texture filter shared with the Xbox (`RECOMP_TEX_BILINEAR` / `RECOMP_TEX_ANISO`, `xenos_gl.c` `texture_options`; UI OPT_TEX, XBA-282) |
| Aspect / scale / picture shader | yes: `aspect_policy.c` (`RECOMP_ASPECT`), widescreen, `RECOMP_XBOX_SHADER` | yes: `RECOMP_GBA_SCALE`, shaders + LCD ghosting (`src/video/present_shaders.c`); menu Scale / Filter (`app/GameMenu`) | yes (4:3) | yes (4:3, edge crop) | yes (4:3); no widescreen hack | yes (16:9, `RECOMP_X360_SCALE`) |
| Cover art | yes: the XBE's image (`app/TitleArt`) | yes: `app/CoverArt` (libretro-thumbnails download, a captured frame until a cover comes, `ShellActivity.coverShotKey`) | yes | yes | yes (XBA-274: the download was missing, `CoverArt.system` had no GameCube repository; a captured frame only before) | partial: the XEX's own title image (its XDBF resource, written into the pack by `tools/prepare/x360.py`, `app/CoverArt.packArtFile`, XBA-282), else a captured frame; no download repository |
| Per-game apps / shortcuts | yes: `app/TitleApk.packageName`, `app/TitleShortcut` | yes: « tgba_… » packages (`app/LauncherActivity.consoleAppRow`, any key `TitleApk.normKey` takes) | yes | yes | yes: « tgc_… », the same row (a disc image goes into the app whole) | not checked (`app/TitleApk` takes any key) |
| Save export / import, cloud saves | yes: `app/SaveArchive` (UDATA / TDATA), `app/CloudSaves` | yes: `save/<console>-<KEY>/` (`app/Storage.saves`, `SaveArchive` roots: the key's folder for any console but the Xbox) | yes | yes | yes: `save/gc-<KEY>/` (cardA.raw, cardB.raw, states), the same path | yes: `save/x360-<KEY>/` holds the states and, since XBA-282, the title's own saves (`data/`: hdd:, cache:; `x360_runtime_start` moves an older `work/x360-<KEY>/data` there) |
| Per-game settings | yes: `app/GameSettings` | yes | yes | yes (+ Vulkan driver, pak) | yes | yes: Controls, scale / filter, internal resolution, texture filter, rewind, shaders (XBA-282) |
| Prepare on device | yes: `app/PrepareService` (XBE -> pack) | yes: `app/Console.prepares` | yes (+ CHD) | yes | yes (DOL, disc, RVZ / WIA / GCZ / CISO) | yes: `app/Console.prepares` (XBA-282: an XEX or a disc image, with the player's key file, `app/X360Key`; `tools/prepare/x360.py`); a desktop pack still works (`x360_tools.bringup --app-pack`) |

Shared layers (what a new console gets by plugging in, src/platform, xbr_host_core): save-state slots and the
automatic slot (`recomp_states.h`), rewind (`recomp_rewind.h`), cheats (`recomp_cheats.h`), hotkeys
(`recomp_hotkeys.h`), and since XBA-274 the frame loop's controls (`recomp_play.h`: the keys' hotkeys next to the
pads', the fast-forward and rewind holds, console frames per shown frame), which the GBA, PS1, N64 and GameCube
runtimes now call instead of four copies; the window, its shaders, frame generation and the perf overlay
(`src/video/host_window.c`); pad mapping (`src/input/mapping/console_pad`).

## Hosts

| Host | Xbox | GBA | PS1 | N64 | GameCube | Xbox 360 |
|---|---|---|---|---|---|---|
| Android app | yes | yes | yes | yes | yes | yes: Prepare on the device (XBA-282) or a desktop pack |
| Desktop `xrecomp` CLI (Linux, macOS) | yes | yes (`gba_play`) | yes (`ps1_play`) | yes (`n64_play`) | yes (`gc_play`; XBA-274: `tools/xrecomp/library.py` `_identify_gc`, RVZ / WIA through a found `gc_run`, read once and kept; saves and states in RECOMP_SAVE_DIR, `--missed`) | partial: `x360_play` alone (Linux; not in `xrecomp` yet) |
| Desktop `xrecomp` CLI (Windows) | yes, without save states / rewind | no (`__main__.py` `console_unsupported`) | no | no | no | no |
| macOS launcher (`macos/`) | yes: settings, cover art, game apps, save import | yes (XBA-278: `macos/Sources/XRecompCore/ConsoleGames.swift`; listed, prepared and played through `xrecomp identify` / `play --print --json`; settings rows per console, Controls, Manage, game apps; box art: no download, the monogram) | yes (a cue's tracks once) | yes (+ controller pak row) | yes (a GameCube `.iso` after the Xbox identify) | no |

The desktop has no settings UI on Linux / Windows (`recomp.cfg`, `xrecomp settings KEY=VALUE`), no save-state, cheat
or rewind UI on any desktop (hotkeys and files only). Every desktop game window (the console runtimes' `host_window.c`,
the Xbox SDL presenter, the Xbox Windows presenter) has F11 / Alt+Enter for fullscreen and F12 for a screenshot
(XBA-278, `src/platform/recomp_window_keys.h`; `RECOMP_FULLSCREEN=1` starts fullscreen, `RECOMP_SCREENSHOT_DIR`
moves the screenshots from `$RECOMP_SAVE_DIR/screenshots`); on Android the window is always fullscreen.

## Gaps, in the order they are filled (XBA-274)

1. GameCube controls in the app: the Controls screen (`app/ConsoleControls`). Done.
2. GameCube internal resolution in the app (`app/ConsoleVideo.applies`, `app/GameSettings.appliesTo`). Done.
3. Xbox rewind in a game's settings (`app/GameSettings.appliesTo`). Done.
4. GameCube in the desktop `xrecomp` CLI (`tools/xrecomp`, `gc_play --missed`, `RECOMP_SAVE_DIR`). Done.
5. GameCube frame generation on the texture present path (`src/video/host_window.c`). Done.
6. Every hotkey in an Xbox title's Controls (`app/Hotkeys.Action.appliesTo`). Done.
7. The frame loop's controls shared by the console runtimes (`src/platform/recomp_play.h`). Done.
8. A GameCube on-screen pad (`app/TouchPad` Layout.GC). Done.
9. The desktop keys' hotkeys for Xbox titles (`src/usb/usb_gamepad.c`, `src/input/keyboard.c`). Done.
10. GameCube cover art download (`app/CoverArt.system`). Done.

Left, with the reason:

- GBA internal resolution and rumble: n/a (above).
- N64 and GameCube rewind: on by default since XBA-278. The ring's coding buffer was a worst case (1.125 times a
  capture: with a 41 MB GameCube capture and the newest, 64 MB held no history), it is now as large as the deltas;
  a capture goes in by a swap of buffers and is saved straight into the last one's buffer (no size walk); the N64 and
  GameCube captures store every page (a page turning zero shifted the packed layout and the XOR delta covered the
  whole state); costly captures are spaced out. Measured (`tests/recomp_rewind` « big », `gc_run` / `n64_run
  --rewind` on the homebrew): GameCube 41 MB, 4.5-5 ms a capture, one every 5-6 fields, ~30 held in 64 MB with a
  picture rewritten every field; N64 9 MB, 1.5 ms, every 4 frames. A desktop CPU; the Thor is to be measured
  (device check).
- N64 internal resolution with the software RDP: it draws at the console's resolution by design; the Vulkan RDP
  upscales.
- Xbox in-run loads with mismatched thread chains, and Xbox states on Windows: runtime work (XBA-249 follow-ups).
- Xbox keyboard hotkeys on Windows (the GDI window, `src/video/fb_present.c`): Windows builds have no Xbox save
  states yet, so only fast-forward would apply.
- macOS launcher for the other consoles: done (XBA-278); its console games have no box art download yet (the
  monogram). A desktop settings / states UI on Linux and Windows: front-end work, not started.
