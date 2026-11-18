# Feature matrix: every console, every host

XBA-274 (Alex: « je veux que toutes les plateformes soient uniformes en termes de features »). This page says, from
the code, which player-facing feature each console has, at runtime (the C runtime) and in the front ends (the Android
app, the desktop `xrecomp` CLI, the macOS launcher). Each cell names the file that proves it; « UI » means the
Android app (`android/app/src/main/java/fr/adserver/xrecomp/`, written `app/` below).

**yes**: the console has it, runtime and Android UI. **partial**: something is missing (said in the cell). **no**:
missing; **n/a**: it means nothing for that console (the reason is given). Audited on main at 0.13.5
(2026-11-18); each XBA-274 commit that fills a gap updates its cell here.

Consoles: Xbox (`src/`), Game Boy Advance (`consoles/gba`), PlayStation (`consoles/ps1`), Nintendo 64
(`consoles/n64`), GameCube (`consoles/gc`). The spikes (DS, PSP, PS2, Dreamcast, 3DS, Xbox 360, Win32) are not in the
app and not in this matrix.

## Runtime and Android app

| Feature | Xbox | GBA | PS1 | N64 | GameCube |
|---|---|---|---|---|---|
| Internal resolution (upscaling) | yes: `src/gpu/nv2a_gl.c` (`RECOMP_GPU_SCALE`, else `RECOMP_UPSCALE`); UI `app/GameSettings.appliesTo` | n/a: the PPU draws 240 x 160 in software; window scale and shaders instead | yes: `consoles/ps1/runtime/ps1_gpu.c` `ps1_gpu_set_upscale` (shadow VRAM 1/2/4x) | partial: `consoles/n64/runtime/n64_rdp_select.c` `n64_rdp_upscale_env`, the Vulkan RDP only (the software RDP draws native) | yes: `consoles/gc/runtime/gx/gx_gl.c` `gx_gl_attach` (`RECOMP_GC_UPSCALE` / `RECOMP_UPSCALE`, 1..8x, GX renderer); UI `app/ConsoleVideo.applies` (XBA-274: was hidden) |
| Fast-forward | yes: guest clock x2..16, `src/kernel/kernel_hal.c` `xbox_FastForwardPoll`; menu `app/GameMenu` (`fastForwardSupported`) | yes: frames per shown frame, `gba_runtime.c` (`recomp_ff_hold`) | yes: `ps1_runtime.c` | yes: `n64_runtime.c` | yes: `gc_runtime.c` |
| Save states in a run | partial: `src/kernel/xbox_state.c` + `xbox_state_service.c` (`recomp_states_attach "xbox"`); a load needs every guest thread parked at the same call (else ECHAIN); not built on Windows (`kernel_bridge.c` `#ifndef _WIN32`); UI asks the runtime (`app/ShellActivity.statesSupported`) | yes: `gba_state.c` | yes: `ps1_state.c` | yes: `n64_state.c` | yes: `gc_state.c` |
| Across runs (« Resume where I left off ») | partial: `xbox_state_service.c` `xbox_StateResumeAtBoot`, only with resume entries in the generated code (`app/Pack.resume`: PREPARE_AGAIN otherwise) | yes: `recomp_states_resume` (`src/platform/recomp_states.c`), `gba_runtime.c` | yes: `ps1_runtime.c` | yes: `n64_runtime.c` | yes: `gc_runtime.c` |
| partial: on by default, time-based (`xbox_state_service.c`, `recomp_rewind_config("xbox")`), when the title's states work, and it turns itself off when one state (its non-zero RAM pages and devices) does not fit half the ring (`RECOMP_REWIND_MB`, default 64: a title filling its 64 MB needs 128 or 256); hotkey with states (`app/Hotkeys`); a game's own Rewind / Rewind memory rows (`app/GameSettings.appliesTo`, XBA-274: were hidden for Xbox titles) | yes: on by default (`src/platform/recomp_rewind.c`) | yes: on by default | yes: off by default (4-8 MB a capture), a game can turn it on | yes: off by default (up to ~42 MB a capture: MEM1 + ARAM), a game can turn it on |
| Cheats | yes: `src/kernel/xbox_cheats.c` (per vblank); UI `app/Cheats.checkXbox` | yes: `gba_cheats.h` | yes: `ps1_runtime.c` | yes: `n64_runtime.c` | yes: `gc_runtime.c`; UI `app/Cheats.checkGc` |
| Frame generation | yes: `src/video/fb_present_sdl.c` (framegen pacer) | yes: `src/video/host_window.c` `host_window_present_rgba` -> `fg_present` | yes: same path | yes: `host_window_present_rgba_shared` | yes: the GX and GPU-YUV pictures (`host_window_present_texture`) are read back for the pacer while frame generation runs (XBA-274, `host_window.c` `texture_pixels`; ctest `gc_framegen`) |
| Perf line | yes: `fb_present_sdl.c` `perf_overlay_tick`; menu `app/GameMenu` (Item.PERF, every console) | yes: `host_window.c` overlay + `host_perf` | yes | yes | yes |
| Pad remapping | yes: `RECOMP_PAD_MAP` (`src/input/xinput_device.c`), UI `app/PadMapping` | yes: `console_pad` (`RECOMP_GBA_PAD_MAP`), UI `app/ConsoleControls` | yes | yes (+ stick range / dead zone) | yes: runtime `gc_runtime.c` `console_pad_load` + `gc_pad_map.c`; UI `app/ConsoleControls` (XBA-274: the Controls screen used to show the N64's 14 targets over the GameCube's 12, `targetLabel` / `groups` default branch) |
| Rumble | yes: `src/usb/ohci.c` -> `xbox_InputSetState`; `RECOMP_RUMBLE` (`app/RecompConfig`, shared) | n/a: no motor on a GBA pad (rumble carts are cartridge hardware, not emulated) | yes: DualShock, `ps1_runtime.c` | yes: Rumble Pak, `n64_runtime.c`, UI `app/N64Pak` | yes: SI motor, `gc_runtime.c` `rumble()` |
| On-screen (touch) pad | yes: `app/TouchPad` Layout.XBOX | yes: Layout.GBA | yes: Layout.PS1 / PS1_ANALOG | yes: Layout.N64 | yes: Layout.GC (XBA-274: both sticks, the cross, A B X Y, Z, L / R; the Xbox's layout before) |
| Hotkeys (states, slots, fast-forward, rewind) | yes: pad chords and, since XBA-274, the window's keys (`src/usb/usb_gamepad.c` `pad_hotkeys`, keys by HID usage from `src/input/keyboard_sdl.c`; the state ones when the title's states work; not on Windows, no states there); the launcher's Xbox Controls lists every chord (`app/Hotkeys.Action.appliesTo`, XBA-274) | yes: pad + keyboard (`src/platform/recomp_hotkeys.c`, `gba_runtime.c`) | yes (+ F6 next disc) | yes | yes |
| Audio | yes: `src/apu/apu_sdl2.c`; volume `app/GameMenu` (every console) | yes: `gba_audio.c` | yes: `ps1_spu.c` | yes | yes: AI / DSP / AX |
| Shader cache / warm-up | partial: runtime program cache (`nv2a_gl.c`, `RECOMP_GPU_SHADER_CACHE`), no app warm-up | n/a: software PPU | n/a: software GPU | yes: `n64_vk_cache.c`, warm-up `app/ShaderWarmup` | yes: `gx/gx_progcache.c`, warm-up `app/GcShaderWarmup` |
| Texture filtering | yes: `RECOMP_TEX_BILINEAR` / `ANISO` (`nv2a_gl.c`), UI OPT_TEX | n/a: a 2D picture, the window filter (`RECOMP_GBA_FILTER`, default nearest) | partial: the window filter only (software GPU, no texture override) | partial: the window filter only | partial: the window filter only (`RECOMP_GC_FILTER`); no texture override in `gx_tex.c` |
| Aspect / scale / picture shader | yes: `aspect_policy.c` (`RECOMP_ASPECT`), widescreen, `RECOMP_XBOX_SHADER` | yes: `RECOMP_GBA_SCALE`, shaders + LCD ghosting (`src/video/present_shaders.c`); menu Scale / Filter (`app/GameMenu`) | yes (4:3) | yes (4:3, edge crop) | yes (4:3); no widescreen hack |
| Cover art | yes: the XBE's image (`app/TitleArt`) | yes: `app/CoverArt` (libretro-thumbnails download, a captured frame until a cover comes, `ShellActivity.coverShotKey`) | yes | yes | yes (XBA-274: the download was missing, `CoverArt.system` had no GameCube repository; a captured frame only before) |
| Per-game apps / shortcuts | yes: `app/TitleApk.packageName`, `app/TitleShortcut` | yes: « tgba_… » packages (`app/LauncherActivity.consoleAppRow`, any key `TitleApk.normKey` takes) | yes | yes | yes: « tgc_… », the same row (a disc image goes into the app whole) |
| Save export / import, cloud saves | yes: `app/SaveArchive` (UDATA / TDATA), `app/CloudSaves` | yes: `save/<console>-<KEY>/` (`app/Storage.saves`, `SaveArchive` roots: the key's folder for any console but the Xbox) | yes | yes | yes: `save/gc-<KEY>/` (cardA.raw, cardB.raw, states), the same path |
| Per-game settings | yes: `app/GameSettings` | yes | yes | yes (+ Vulkan driver, pak) | yes |
| Prepare on device | yes: `app/PrepareService` (XBE -> pack) | yes: `app/Console.prepares` | yes (+ CHD) | yes | yes (DOL, disc, RVZ / WIA / GCZ / CISO) |

Shared layers (what a new console gets by plugging in, src/platform, xbr_host_core): save-state slots and the
automatic slot (`recomp_states.h`), rewind (`recomp_rewind.h`), cheats (`recomp_cheats.h`), hotkeys
(`recomp_hotkeys.h`), and since XBA-274 the frame loop's controls (`recomp_play.h`: the keys' hotkeys next to the
pads', the fast-forward and rewind holds, console frames per shown frame), which the GBA, PS1, N64 and GameCube
runtimes now call instead of four copies; the window, its shaders, frame generation and the perf overlay
(`src/video/host_window.c`); pad mapping (`src/input/mapping/console_pad`).

## Hosts

| Host | Xbox | GBA | PS1 | N64 | GameCube |
|---|---|---|---|---|---|
| Android app | yes | yes | yes | yes | yes |
| Desktop `xrecomp` CLI (Linux, macOS) | yes | yes (`gba_play`) | yes (`ps1_play`) | yes (`n64_play`) | yes (`gc_play`; XBA-274: `tools/xrecomp/library.py` `_identify_gc`, RVZ / WIA through a found `gc_run`, read once and kept; saves and states in RECOMP_SAVE_DIR, `--missed`) |
| Desktop `xrecomp` CLI (Windows) | yes, without save states / rewind | no (`__main__.py` `console_unsupported`) | no | no | no |
| macOS launcher (`macos/`) | yes: settings, cover art, game apps, save import | no: `LibraryScanner.swift` lists Xbox folders and images only | no | no | no |

The desktop has no settings UI on Linux / Windows (`recomp.cfg`, `xrecomp settings KEY=VALUE`), no save-state, cheat
or rewind UI on any desktop (hotkeys and files only), and no screenshot or fullscreen hotkey.

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
- N64 and GameCube rewind off by default: a capture is 4-8 MB (N64) and up to ~42 MB (GameCube: MEM1 + ARAM)
  against 64 MB of history by default (`src/platform/recomp_rewind.c` `recomp_rewind_config`); on per game or in
  Settings.
- N64 internal resolution with the software RDP: it draws at the console's resolution by design; the Vulkan RDP
  upscales.
- Xbox in-run loads with mismatched thread chains, and Xbox states on Windows: runtime work (XBA-249 follow-ups).
- Xbox keyboard hotkeys on Windows (the GDI window, `src/video/fb_present.c`): Windows builds have no Xbox save
  states yet, so only fast-forward would apply.
- macOS launcher for the other consoles, desktop settings / states UI: front-end work, not started.
