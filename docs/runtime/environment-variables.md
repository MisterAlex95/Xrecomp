# Environment Variables

> Every `RECOMP_*` switch the runtime and tools read, what it does, and what it defaults to.
>
> **Read this when:** a title needs a bring-up switch, a log is asking you to set one, or you want to know what a variable you found in a script does.
>
> Part of the [Documentation Index](../INDEX.md)

Almost everything here is **off by default**. A switch that changes behaviour is a per-title opt-in: set it for the title that needs it and leave it unset for the rest, so one title's fix never moves another's output. Debug switches only print or dump; they never change what the title does.

How to read the tables:

- **Default** is what happens with the variable unset. "off" means the variable has to be present (or non-zero, where the entry says so). "on" means it is active until you set `0`.
- **Where** is the source file that reads it, so the code is one search away.
- Values are text. A flag is "set" when the variable exists, unless the entry says `1` or non-zero is required.

On Android these come from `recomp.cfg` in the app's external files dir. The launcher's Settings screen writes a few of
them there for the player (`RECOMP_LANGUAGE`, `RECOMP_PRESENT_VSYNC`, `RECOMP_TEX_BILINEAR`, `RECOMP_TEX_ANISO`, `RECOMP_APU_HRTF`,
`RECOMP_AUDIO_BUF_SAMPLES`, `RECOMP_RASTER_THREADS`, `RECOMP_BIG_CORES`, `RECOMP_SHELL_SELFTEST`) and keeps every
other line as written: see [Settings](../android/app-shell.md#settings).

### Forced by a title

A title profile (`profiles/titles/<title id>/title.json`, or any profile layer: [profiles.md](../technical/profiles.md)) can force options that the title cannot run right without, over
everything else (the shell environment, `recomp.cfg`, the launcher's Settings):

```json
"env_force": {"RECOMP_LSFG": "0"}
```

- **Allowed**: names `RECOMP_[A-Z0-9_]+` (63 characters at most), values strings of printable ASCII (255 at most; a
  JSON number is taken as its text), 16 entries at most. `tools.prepare` refuses anything else (`BAD_PROFILE` on
  Android, an error on the desktop); the runtime checks again and ignores a refused entry with a `[TITLE]` line on
  stderr. The value is set as is: for a switch read by presence, `"0"` still turns it on.
- **Why not `setenv` in the profile**: `xba_title_configure` runs when the guest thread starts, after the window
  and the frame generation pacer have read their options. Burnout 3 (its game logic follows the flip rate) under
  `RECOMP_LSFG=1`'s 30 fps cap runs in slow motion; its profile forces `RECOMP_LSFG=0`.
- **Android**: `tools/prepare` copies it into `pack.json` ([prepare.md](../android/prepare.md#packjson));
  `pack_loader.c` sets it after `recomp.cfg` and before the dlopen (`recomp_pack_apply_env_force`,
  `recomp_cfg_force`). The launcher's frame generation row says « Frame generation unavailable for this title » when
  the selected game's pack forces `RECOMP_LSFG` off.
- **Desktop**: `tools.prepare` writes it to the project's `src/title/env_force.h`; `main()` (templates/new-game)
  sets it first thing.
- Log, one line per option: `[TITLE] 4541005B forces RECOMP_LSFG=0 (title profile)`.

## Bring-up and diagnostics

Start here when a title does not run. See [Step 6: Iterative Debugging](../pipeline/06-debugging.md) for the workflow these belong to.

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_WATCHDOG_SECS=N` | off | Dumps registers, PUT/GET and the guest stack when the title stops making progress for N seconds. | `xbox_memory_layout.c` |
| `RECOMP_WATCH=<va>` | off | Names the guest code that writes a value at an address (a callback slot, a "ready" flag). Wrap the address in `[` `]` to watch the word it points to. | `xbox_memory_layout.c` |
| `RECOMP_WATCH_LEN` | off | Length in bytes of the watched range. | `xbox_memory_layout.c` |
| `RECOMP_WAIT_LOG=1` | off | One `[WAIT]` line for each single-object wait that timed out or lasted 500 ms or more. | `kernel_sync.c` |
| `RECOMP_WAIT_REPORT=0` | on, top 4 | Turns off the per-site wait report: every periodic `[KERNEL] summary` (2 s) is followed by up to N lines `[KERNEL] waits: <object> set xS; from <return address> woke xW of T, blocked M ms` for the wait sites that waited since the last summary, ranked by blocked time. `set` counts KeSetEvent, KePulseEvent, NtSetEvent, NtPulseEvent and device-model sets on the object (absent when nothing set it), `woke` the waits that returned satisfied, `of` all waits from that call site, `blocked` the host time inside them (zero-timeout polls are not timed); totals since start; multi-object waits are filed under their first object and marked `(any of N)` or `(all of N)`. Wakes far below sets on an event two threads wait on means lost wake-ups. `=N` lists the top N (1-16). Fixed tables (256 sites, 128 set objects), lock-free; a full table says how many it dropped. | `kernel_wait_stats.c` |
| `RECOMP_WATCH_DEPTH` | 14 | Frames printed per watch hit; identical consecutive return addresses are folded. | `xbox_memory_layout.c` |
| `RECOMP_WATCH_RAW` | off | Also prints the raw stack frame, to see which object the writer was working on. | `xbox_memory_layout.c` |
| `RECOMP_KERNEL_WATCH=<va>` | off | Reports a guest address whenever a kernel bridge call changes it. | `kernel_bridge.c` |
| `RECOMP_KERNEL_WATCH_ALL` | off | With the above: prints every sample, so a value that was already wrong at the first call shows. | `kernel_bridge.c` |
| `RECOMP_KERNEL_LOG_BUDGET=N` | 200 | Number of kernel-call log lines kept before the log goes quiet. | `kernel_bridge.c` |
| `RECOMP_IRQL_TRACE` | off | Prints the first IRQL transitions; a lower from a level the raise did not set is a calling-convention bug. | `kernel_hal.c` |
| `RECOMP_STATUS=<file>` | off | A headless run's status, one JSON line per period: flips, how often the picture changed, whether it is lit, non-silent audio, pad reads, guest threads, discovery entries. What the compat runner reads (`src/kernel/recomp_status.h` has the format). `RECOMP_STATUS_MS` sets the period (default 1000, at least 50). | `src/kernel/recomp_status.c` |
| `RECOMP_DISCOVERY=<file>` | the prepare's work folder (desktop: set by `tools.prepare`; Android: `<files>/work/<TITLEID>/`) | Where a run records what the static analysis missed: unknown call targets, thread starts, untranslated instructions reached. `0` turns it off. [discovery.md](../technical/discovery.md) | `src/kernel/recomp_discovery.c` |
| `RECOMP_UNIMPL_TRAP=1` | off | Stops at the first untranslated instruction that is actually reached, instead of logging it. | `templates/new-game/src/recomp_manual.c` |
| `RECOMP_STUB_LOG=1` | off | Prints each unresolved stub the first time it runs, with return address and `esp`. Read by the generated stubs file. | `tools/recomp/translator.py` |
| `RECOMP_TRACE_BUDGET=N` | 400000 | Lines of the call trace kept; a larger file is better than a hole. | `recomp_trace.c` |
| `RECOMP_TRACE_PROFILE=N` | 20000000 | Prints the call profile every N calls (and at exit). | `recomp_trace.c` |
| `RECOMP_TRACE_ARGS=N` | off | Adds N stack arguments to each trace line. | `recomp_trace.c` |
| `RECOMP_TRACE_DEREF` | off | Follows pointer arguments one level in the trace. | `recomp_trace.c` |
| `RECOMP_CS_TRACE_CRT[=all]` | off | Traces critical-section activity (CRT sections only, or all). | `kernel_rtl.c` |
| `RECOMP_CS_WATCH=<va>` | off | Traces one critical section by address. | `kernel_rtl.c` |
| `RECOMP_POKE=va[:val][,...]` | off | Holds guest globals at a value. A bring-up probe, not a fix. | `xbox_memory_layout.c` |
| `RECOMP_PEEK=<va>[,...]` | off | Prints guest words at each report, both as a number and as a pointer. | `nv2a_pb_exec.c` |
| `RECOMP_PEEK_CHAIN` | off | Follows a pointer chain level by level and says where a null breaks it. | `nv2a_pb_exec.c` |
| `RECOMP_TRAP_NULL` | off | Protects the null page so a null dereference faults. | `xbox_memory_layout.c` |
| `RECOMP_FORCE_RETURN` | off | Turns on the early return of functions generated with `--force-return`; without it they behave normally. | `xbox_memory_layout.c` |
| `RECOMP_MISSING_FILES` | list | `0` turns the missing-file report off, `all` lists every miss, otherwise the default report. | `kernel_missing.c` |
| `RECOMP_MISSING_FILES_LIST=<path>` | off | Writes the missing-file list to that path on exit. | `kernel_missing.c` |
| `RECOMP_CMDLINE=<text>` | off | Command line handed to the title, as a launcher would (`+map intro`). | `kernel_bridge.c` |
| `RECOMP_SHELL_SELFTEST=1` | off | Android app shell: after `SDL_Init`, runs the self-test in the app process (memory layout on the synthetic XBE asset, the 4 GB window, mirrors, trapped MMIO of each decoder class, execute protections, the save dir), one `selftest NAME: PASS` / `FAIL (reason)` line each and `selftest: N passed, M failed`, then starts as usual. Set it in `recomp.cfg`; `0` is off (docs/android/app-shell.md). | `shell_main.c`, `recomp_selftest.c` |
| `RECOMP_COMPANION_FOCUS=forward` | `none` | Android app, dual screen (AYN Thor): the second-screen panel's window is focusable and forwards the key and motion events it gets to the game, which ignores losing the focus to it (Plan B). `none`: the panel is never focusable (Plan A). Read from `recomp.cfg` by the Java side at each start of the panel (docs/android/app-shell.md « Dual screen »). | `DualScreen.java`, `CompanionScreen.java` |
| `RECOMP_SELFTEST_SKIP=<names>` | off | Self-test checks to skip, separated by commas or spaces (`mmio_simd`); each prints `SKIP`. The arm64 CI job skips `mmio_simd` under qemu-user. | `recomp_selftest.c` |

## Memory and kernel

Switches that change how the kernel layer behaves. Each fixes a class of titles; apart from `RECOMP_CONTIG_RECLAIM` and the per-host defaults noted, none is a default. See [Xbox Kernel Replacement](../technical/kernel-replacement.md) and [Memory Layout Reproduction](../technical/memory-layout.md).

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_LANGUAGE=<code>` | English | The console language the title reads (`XC_LANGUAGE` and the `XC_MAX_OS` block): `en`, `ja`, `de`, `fr`, `es`, `it`, `ko`, `zh`, `pt` (a region suffix such as `fr-CA` is ignored) or the Xbox value `1`..`9`. The title uses it when its disc has that language. A player setting, not a fix: on Android the launcher sets it (Settings › Game › Language, per-title choice in the in-game menu). | `kernel_xbox.c` |
| `RECOMP_SAVE_DIR=<dir>` | off | Root for the title's `UDATA`, `TDATA`, `TitleCache` and utility drive (`Cache/<title id>`, Z:) folders; created if missing. Absolute. Saves left in the game folder are then not used. | `kernel_path.c` |
| `RECOMP_GAME_DIR=<dir>` | `<external files dir>/game` | Android app shell: where the title's data (`default.xbe` and the disc's files) is read from. The launcher sets it from the folder the player chose (XBA-54; it then wins over `recomp.cfg`); otherwise set it in `recomp.cfg`. | `recomp_storage.c` |
| `RECOMP_HEAP_RECLAIM` | off | Gives freed guest heap back: freed heap blocks are split instead of handed over whole, and `NtFreeVirtualMemory` returns heap memory. Contiguous memory is `RECOMP_CONTIG_RECLAIM`. | `xbox_memory_layout.c`, `kernel_bridge.c` |
| `RECOMP_CONTIG_RECLAIM=0` | on | `MmFreeContiguousMemory` gives the block back to the contiguous window, so the next request that fits gets its pages (and possibly its address). `0` keeps every block for good, the behaviour before (when only `RECOMP_HEAP_RECLAIM` freed them): addresses are never seen twice, but a title that frees and reloads textures per scene runs the 64 MB window out (Fable: `[CONTIG] arena exhausted`). The `[CONTIG]` lines count `released=` blocks and `free calls=`. Read once. | `xbox_memory_layout.c` |
| `RECOMP_EXT_VMA` | off | Extended virtual-memory areas above the mirrors. | `guest_vmem.c`, `xbox_memory_layout.c` |
| `RECOMP_GUEST_WINDOW=0/1` | on for Android, off elsewhere | POSIX: hold the whole 4 GB guest window from the start and carve every view out of it, so the contiguous window, the apertures and the tiled alias never land on a taken host range. Implied by `RECOMP_EXT_VMA`. | `xbox_memory_layout.c` |
| `RECOMP_THREAD_STACK_MB=N` | 8 | POSIX: floor, in MB (1 to 1024), for the host stack of every thread the Win32 shim creates for a routine (`CreateThread`, so guest threads from `PsCreateSystemThreadEx`, plus timer and DPC callbacks). Lifted code recurses on the native stack; without a floor bionic gives about 1 MB and an Xbox thread's 64 KB would be the whole stack. A larger request is kept. Logged once as `[win32_compat] thread stack floor`. The guest main thread has its own 64 MB stack (`fb_present_sdl.c`). The shim's threads are detached, so a thread that ends gives its stack back at once; its handle keeps the exit code and the signalled state (XBA-56). | `win32_compat.c` |
| `RECOMP_BIG_CORES=0/1` | on for Android, off elsewhere | big.LITTLE hosts: run the runtime on the cores whose cpufreq maximum is above the slowest cluster's (set once at start by `recomp_host_cpus_init`; every thread inherits it). | `host_cpus.c` |
| `RECOMP_CONTIG_AVOID_IMAGE=1` | off | Keeps contiguous allocations out of the image range. | `xbox_memory_layout.c` |
| `RECOMP_WIN_PLACEHOLDER` | off | Reserves the guest window with placeholders on Windows hosts where fixed bases are taken. | `xbox_memory_layout.c` |
| `RECOMP_WAIT_SPIN=0` | on | Turns off the wait-spin detector: a thread that waits on one object 2,000 times within a second is reported (object, header, status, caller, frames; `wait_spin` in the discovery file), and an event header whose waits fail at once is promoted to a guest-built event, as `RECOMP_TITLE_KEVENTS` does for every object, once something has tried to set it (KeSetEvent, software method 5); `=promote` promotes without that evidence (XBA-93). | `kernel_bridge.c` |
| `RECOMP_TITLE_KEVENTS=0` | on | Turns off host events for events a title builds inline instead of through `KeInitializeEvent` (XAPI, XPP and D3D build theirs that way): a wait on one then fails at once instead of blocking until it is set, as before K-5. Even then, the XDK D3D layer adopts D3D's fence event alone when it finds the device at run time under the DMA walk (`xbox_KeAdoptGuestEvent`), and the wait-spin detector promotes a spinning one. On by default since KOTOR's synchronous USB request returned early on a failed wait and its stack URB was later completed into garbage. | `kernel_bridge.c` |
| `RECOMP_GUEST_LOCK=1` | off | Only one guest thread runs guest code at a time. | `kernel_guest_cpu.c` |
| `RECOMP_GUEST_QUANTUM=ms` | 4 | With the lock: milliseconds before the lock is handed over (0 = at every call). | `kernel_guest_cpu.c` |
| `RECOMP_GUEST_CPU_TRACE[=ms]` | off | With the lock: periodic trace of who holds it. | `kernel_guest_cpu.c` |
| `RECOMP_GUEST_CPUS` | `one` on arm64 Linux and Android, `all` elsewhere | `all`, `one`, or a core number: pin the title's threads to one core. On arm64 this gives the title's threads x86's store order among themselves (XBA-81). | `kernel_thread.c` |
| `RECOMP_PUT_BARRIER=1` | off | Linux/Android: `membarrier` at each new DMA_PUT, so the executor never reads a pushbuffer word the title stored before PUT but another core has not seen yet (arm64). One IPI per kick. | `xbox_memory_layout.c` |
| `RECOMP_CS_MODE=single` | per-section | Backs every critical section with one host lock. | `kernel_rtl.c` |
| `RECOMP_CS_MODE=guest` | per-section | Runs critical sections on the guest's own structure (state at +0x10), falling back to the shadow for ones that don't read as critical sections. | `kernel_rtl.c` |
| `RECOMP_WORKERS` | off | Runs requests of the title's worker threads inline. A bisecting tool for concurrency bugs. | `kernel_bridge.c` |
| `RECOMP_THREAD_STACKS=N` | 8 | Worker thread stacks alive at once (512 KB of guest heap each). Past the limit a new worker runs inline on its creator, which deadlocks a worker that waits for requests. | `xbox_memory_layout.c` |
| `RECOMP_INLINE_APC=1` | off (deferred) | Runs a file request's completion APC (ReadFileEx, WriteFileEx) inline inside NtReadFile/NtWriteFile, the default before XBA-102, instead of at the issuing thread's next alertable user-mode wait (SleepEx, the Nt/Ke waits), which then returns STATUS_USER_APC as on the console. The status block and the event are set at the request either way. A/B switch kept for one release; `xbox_SetDeferredIoApc` wins over it. | `kernel_bridge.c` |
| `RECOMP_ASYNC_IO` | off | Reports a read on an asynchronous handle as still in flight. The data is read synchronously either way. | `kernel_bridge.c` |
| `RECOMP_DISPATCH_GATE=1` | off | The one-CPU gate: guest code runs only at safe points while interrupts are delivered. | `kernel_hal.c` |
| `RECOMP_IRQ_SAFE_POINTS` | on with the gate | `0` disables safe-point delivery under the gate. | `kernel_hal.c` |
| `RECOMP_IRQ_SAFE_FORCE` | off | Forces delivery at a safe point. | `kernel_hal.c` |
| `RECOMP_IRQ_SAFE_MS=N` | 250 | Milliseconds the gate waits for a safe point. | `kernel_hal.c` |
| `RECOMP_DPC_ON_RAISE` | on with the gate | `0` stops DPCs running when IRQL is raised. | `kernel_hal.c` |
| `RECOMP_CPUID_NO_SIMD` | off | Hides SIMD from `cpuid`. | `kernel_hal.c` |
| `RECOMP_AC97_READY=1` | off | Reports the AC'97 codec ready and routes the APU aperture to the emulated APU. | `xbox_memory_layout.c`, `templates/new-game/src/main.c` |
| `RECOMP_AC97_ALIAS=0` | on (POSIX) | With `RECOMP_AC97_READY`: arm the AC'97 bus-master write trap with the old `mprotect` window instead of a read-only shared page plus a writable view. The window lets a second thread's store keep the RR bit, so a channel reset can spin (Fable during movies); for A/B only. | `xbox_memory_layout.c` |
| `RECOMP_DSP_ACK=va[,...]` | off | Guest addresses to acknowledge as the DSP's doorbell. | `xbox_memory_layout.c` |

## Graphics: pushbuffer executor

The executor is itself opt-in: set `RECOMP_PB_EXEC=1` to execute the pushbuffer. See [Pushbuffer Executor](../technical/pushbuffer-executor.md).

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_PB_EXEC` | off (Android: on) | Executes the title's pushbuffer. On Android the app shell sets it unless `recomp.cfg` says `RECOMP_PB_EXEC=0` (docs/android/app-shell.md). | `nv2a_pb_scan.c`, `xbox_memory_layout.c` |
| `RECOMP_PB_SCAN` | off | Read-only survey of the pushbuffer; ranks what is not implemented. | `nv2a_pb_scan.c`, `xbox_memory_layout.c` |
| `RECOMP_PB_WALK=dma` | off (on for XDK 5849 D3D8LTCG titles: `profiles/xdk/d3d8ltcg`, `XBA_D3D_DMA_WALK`) | Walks the pushbuffer as the DMA engine does, which delivers D3D's software-method interrupt. `RECOMP_PB_WALK=scan` turns it off where a layer turned it on. | `nv2a_pb_scan.c` |
| `RECOMP_NV2A_TRACE` | off | Shows whether `DMA_PUT` moves at all. | `xbox_memory_layout.c` |
| `RECOMP_POLL_WAIT=0` | on | The lifter marks loops that only poll one memory word (`test dword [mem], imm ; jne/je` to itself) with `RECOMP_POLL_WAIT`. For D3D KickOff's wait on the PFB flush bit (0x100410 bit 16) the thread then blocks until the ack thread clears the bit (`xbox_Nv2aKickWait`) instead of spinning a host core; other words still spin. `0` keeps the spin for the flush bit too. Takes effect on titles prepared with a toolkit that has the lifter change. | `xbox_memory_layout.c`, `tools/recomp/translator.py` |
| `RECOMP_PB_REPORT_MS=N` | 10000 | Milliseconds between `[GPU]` reports (minimum 100). | `xbox_memory_layout.c` |
| `RECOMP_PB_EXEC_VERBOSE` | off | Per-batch trace. | `nv2a_pb_exec.c` |
| `RECOMP_PB_UNHANDLED_ALL` | top 10 | Lists every method the executor does not model, not only the top ten. | `nv2a_pb_exec.c` |
| `RECOMP_VBLANK[=paced]` | no vblanks | Raises vblanks: a ~50 Hz tick, or `paced` for the 60 Hz schedule. `0` or empty is off. | `kernel_bridge.c` |
| `RECOMP_FLIP_PACING[=edge]` | off | Holds each flip to the vblank count. | `nv2a_pb_exec.c` |
| `RECOMP_FFP_INLINE=<0\|1\|2>` | 1 | Inline (Begin/End) fixed-function batches: `1` transformed by the composite matrix, `0` the old screen-space guess, `2` transformed only when the guess would skip them. A/B switch for one release. | `nv2a_pb_exec.c` |
| `RECOMP_FFP_FLIP_NORMALS` | off | Lights the side of a fixed-function mesh that faces the viewer. Not hardware behaviour. | `nv2a_pb_exec.c` |
| `RECOMP_CLIP_NEAR_Z` | on | `0` turns off the near-plane clip (window z below `SET_CLIP_MIN`). | `nv2a_pb_exec.c` |
| `RECOMP_SWIZZLED_RT` | on | `0` stops rendering into swizzled render targets. | `nv2a_pb_exec.c` |
| `RECOMP_NO_COMBINERS` | off | Skips the register combiners. | `nv2a_pb_exec.c` |
| `RECOMP_NO_VSH` | off | Skips vertex programs. | `nv2a_pb_exec.c` |
| `RECOMP_RASTER_THREADS=N` | auto | Threads drawing (and shading vertex programs) for the software rasteriser, the executor thread included, on Windows and POSIX. Auto: online cores − 4 on desktop; on Android the cores the runtime may run on (the fast cluster under `RECOMP_BIG_CORES`) − 1. | `nv2a_pb_exec.c` |
| `RECOMP_ACK_IDLE_US=N` | 250 | POSIX: with nothing submitted, the NV2A ack thread sleeps in 50 us steps for up to N us (watching PUT and the busy bits) instead of yielding in a loop. While the title kicks through `xbox_Nv2aKickWait` (a title override of D3D's KickOff wait, e.g. Fable's), it instead sleeps until the next kick, for at most N us, and yields only briefly for the DMA_PUT after an ack that released the title. 0 restores the bare yield. | `xbox_memory_layout.c` |
| `RECOMP_RASTER_SPIN_US=N` | 20 | POSIX: how long an idle rasteriser worker watches for the next job before it parks (`0`: park at once). The executor hands batches over back to back, and a wake costs tens of microseconds. | `nv2a_pb_exec.c` |
| `RECOMP_VSH_POOL=0` | on | Shades every vertex-program batch on the executor thread instead of splitting batches of 192+ vertices across the rasteriser's worker threads (same output either way). | `nv2a_pb_exec.c` |
| `RECOMP_FFP_BATCH=0` | on | Lights and transforms fixed-function vertices one by one, re-reading the light registers and attribute layout for each vertex, instead of resolving them once per batch and transforming each unique index once (same output either way; A/B switch). | `nv2a_pb_exec.c` |
| `RECOMP_FFP_POOL=1` | off | Splits fixed-function batches of 192+ unique vertices across the rasteriser's worker threads (same output). Off by default: slower in KOTOR on a busy host. | `nv2a_pb_exec.c` |
| `RECOMP_TEX_BILINEAR=1` | off | CPU rasteriser blends the four nearest texels instead of taking the nearest. | `nv2a_pb_exec.c` |
| `RECOMP_TEX_ANISO=8\|16` | off | Anisotropic filtering on the CPU rasteriser; implies `RECOMP_TEX_BILINEAR`. Slower. | `nv2a_pb_exec.c` |
| `RECOMP_GPU=1` | off | Draw with the GL back end (desktop GL 3.3/4.1 core, GLES 3.0 on Android) instead of the CPU rasteriser; vertex programs run as GLSL vertex shaders (see `RECOMP_GPU_VSH`). Falls back to the CPU rasteriser if no GL context can be made. See [GPU Back End](../technical/gpu-backend.md). | `src/gpu/nv2a_gl.c` |
| `RECOMP_OVERLAY=1` | off | The SDL presenter draws its small debug overlay (FPS, frame time, draws, memory, CPU, battery, temperature, the title's status line) in the top left corner of the game. Off by default everywhere, Android included; the in-game menu's Settings page has its own performance display for normal use. | `src/video/perf_overlay.c` |
| `RECOMP_D3D_CALLBACKS=0` | on | Leaves D3D's InsertCallback software methods (6, 7) unserviced instead of calling the title's routine at DPC level (XBA-94). | `profiles/xdk/d3d8/d3d8.c` |
| `RECOMP_FRAME_LOCK=0` | on | Under frame generation, keeps it on even for a title that asks for every vblank and waits at its 30 fps cap on nearly every frame (otherwise: frame generation off, cap back to 60; XBA-96, framegen.md). | `nv2a_pb_exec.c` |
| `RECOMP_GPU_FPS_CAP=N` | 60 on macOS and Android, off elsewhere | With a back end drawing (`RECOMP_GPU=1`), at most N flips per second of host time: after `FLIP_STALL` the executor sleeps until the frame's slot before it walks on. `0` turns it off. Independent of `RECOMP_FLIP_PACING`. The `[GPU] frame cap` report line gives waits, time slept and how late the sleeps woke. | `nv2a_pb_exec.c` |
| `RECOMP_GPU_PRIORITY=0` | high where offered | GL back end on EGL: do not ask for a high-priority context (`EGL_IMG_context_priority`). | `src/gpu/nv2a_gl_context.c` |
| `RECOMP_GPU_FAKE_RESET=<frame>[,oom]` | off | Test aid: the GL back end acts as if its context were lost (or `GL_OUT_OF_MEMORY` were raised) at that frame, then re-creates its objects. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_CULL=0` | on | GL back end: draw both faces (no back-face culling). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_ZPASS=sync`, `=always`, `=lazy` | deferred | GPU back end: visibility reports (`GET_REPORT`) are written when the GPU's count is in, without waiting (when the walker idles, at the flip and after the present); a report still pending at the head after 300 us of idle calls gets one flush, after 2 ms a wait; past 512 pending the oldest are waited for. `sync` waits at every report; `always` runs no queries and reports 1; `lazy` never flushes or waits from the idle hook and answers 1 (visible) where it would wait. | `nv2a_pb_exec.c` |
| `RECOMP_GPU_VSH=0` | on | GL back end: run vertex programs on the CPU (interpreter, near-plane clipping, triangle expansion) instead of as GLSL vertex shaders; for comparing the two paths. | `src/kernel/nv2a_pb_exec.c` |
| `RECOMP_GPU_VBO=0` | on | GL back end: draw vertex-program batches from cached GL buffers holding the guest's vertex bytes in their own formats (validated by a hash once a frame) instead of attributes gathered on the CPU; `0` gathers every batch (the old path, for comparison). See `docs/technical/gpu-backend.md`. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_S3TC=0` | on where supported | GL back end: decode DXT textures on the CPU instead of uploading them compressed. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_TEX_RECHECK=frame`, `=switch` | per frame, on texture switch for volatile textures, never over GPU targets | GL back end: `frame` hashes a cached texture's guest bytes only once per frame; `switch` restores the old policy (every frame and every texture switch, targets included). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_RT_DIRECT=0` | on | GL back end: never sample a render target's GL texture directly; read the target back into guest memory and upload it as a texture instead (the pre-7d path). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_RT_RELAYOUT_READBACK=1` | off | GL back end: read a render target back when the title switches it between swizzled and linear or between sizes (it is kept on the GPU either way). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_PRESENT=sync` | async | GL back end: read the presented surface back at its own flip instead of one frame later through a pixel pack buffer. Frame dumps always read synchronously. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_PRESENT=host` | through guest memory (host present on by itself at `RECOMP_GPU_SCALE` above 1) | GL back end, SDL window only: hand the presented target to the window at its real GL size (one frame late, through a pack buffer of its own) instead of converting it into guest memory; the guest bytes are then refreshed only when something needs them (or with `RECOMP_GPU_READBACK=present`). Headless runs and frame dumps keep the guest-memory path. See [GPU Back End](../technical/gpu-backend.md#present). | `src/gpu/nv2a_gl.c`, `src/video/fb_present_sdl.c` |
| `RECOMP_GPU_READBACK=all` | presented surface only | GL back end: at every flip also read back every other render target drawn since the last one (for a title whose CPU reads its render targets). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_READBACK=present` | on demand under host present | GL back end with host present: still copy the presented surface into guest memory at every flip, as without host present (for a title whose CPU reads its front buffer). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SCALE=N`, `=auto` | 1 | GL back end: internal resolution, a multiple of the 640x480 frame buffer (2, 3, or a decimal 1-4). `auto`: the window's drawable height / 480 (1080 -> 2.25), at most 3 and within `GL_MAX_TEXTURE_SIZE`; 1 headless. Logged as `[GPUGL] internal scale <n>`. Above 1 the window gets host frames (`RECOMP_GPU_PRESENT=host`). Launcher: Settings › Graphics › Internal resolution. See [GPU Back End](../technical/gpu-backend.md#internal-resolution). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SCALE_RESOLVE=0` | on | With `RECOMP_GPU_SCALE` above 1: a native target sampling a scaled one reads the scaled texture instead of its native-size resolve (comparison aid). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SCALE_SHADOW=1` | off | With `RECOMP_GPU_SCALE` above 1: depth-only targets (shadow maps) scale too. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SCALE_TARGETS=WxH[,WxH]` | none | With `RECOMP_GPU_SCALE` above 1: colour targets of these real sizes scale as well as the 640x480 frame buffer. | `src/gpu/nv2a_gl_scale.c` |
| `RECOMP_GPU_SCALE_DUMP=1` | off | At every dumping flip (`RECOMP_FB_DUMP`), also write the presented target at its real (scaled) size as `<prefix>S<n>.bmp`. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SHADER_CACHE=0` | on where the driver offers a binary format | GL back end: do not load or save linked programs in `<save dir>/shadercache/<title id>/` (`glProgramBinary`); every program is compiled at its first use. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_INVALIDATE=0` | on where supported (GLES 3.0, GL 4.3) | GL back end: do not call `glInvalidateSubFramebuffer` before full clears, nor `glInvalidateFramebuffer` on the scratch depth of a target drawn without a zeta surface when it is left. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SIGDUMP=<file>` | off | GL back end, diagnosis: at the first flip where `<file>` exists, print that frame's batch signature table (`[GPUSIG] <sig> n <batches> tris <triangles> : <description>`, one per signature) and its target runs (`[GPUSEQ]`), then delete the file. See [GPU Back End](../technical/gpu-backend.md#batch-signatures). | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_SIGSKIP=<file>` | off | GL back end, diagnosis: do not draw batches whose signature (hex, as `RECOMP_GPU_SIGDUMP` prints it) is listed in `<file>`; re-read every 16 flips, so it can be edited while the game runs. | `src/gpu/nv2a_gl.c` |
| `RECOMP_GPU_VERBOSE=1` | off | GL back end: log render targets as they are created and a statistics line every 60 frames (default every 600); on EGL, the EGL version, vendor and extension list once. | `src/gpu/nv2a_gl.c`, `nv2a_gl_context.c` |
| `RECOMP_STRIP_ALT_WINDING` | off | Odd triangles of a strip keep the strip's winding, as on hardware. | `nv2a_pb_exec.c` |
| `RECOMP_PB_CONST_FAST=0` | on | Sends every `SET_TRANSFORM_CONSTANT` word through the general dispatcher. | `nv2a_pb_exec.c` |
| `RECOMP_RASTER_TEST` | off | Bring-up aid: draws a magenta triangle after every clear, to prove the rasteriser and presenter work. | `nv2a_pb_exec.c` |
| `RECOMP_FFP_TRACE` | off | Matrix and first vertices of the first fixed-function batches. | `nv2a_pb_exec.c` |
| `RECOMP_VSH_TRACE=N` | 0 | Prints N vertex-program batches. | `nv2a_pb_exec.c` |
| `RECOMP_VSH_DUMP` | off | Vertex-program constant traffic. | `nv2a_vsh_interp.c` |
| `RECOMP_FRAME_TRACE` | off | Traces one frame in detail. | `nv2a_pb_exec.c` |
| `RECOMP_FRAME_TRACE_METHODS` | off | With the above: every method of the traced frame, less the bulk uploads. | `nv2a_pb_exec.c` |
| `RECOMP_TEX_STATE` | off | Texture state per report. | `nv2a_pb_exec.c` |
| `RECOMP_TEX_DUMP=<prefix>` | off | Writes each distinct texture as a BMP, first use only. | `nv2a_pb_exec.c` |
| `RECOMP_TEX_DUMP_EVERY=N` | 0 | Dumps every Nth texture bind instead. | `nv2a_pb_exec.c` |
| `RECOMP_FB_DUMP=<prefix>` | off | Writes a BMP per report: the frame the next flip presents (complete), or, for a title that has not flipped yet or stopped flipping, the draw surface as it is; before the first flip also the first frames that draw geometry. On POSIX hosts, while the video overlay is up, the overlay's picture over the last frame instead (what the screen shows). | `nv2a_pb_exec.c`, `fb_present.c`, `pvideo_posix.c` |
| `RECOMP_FB_DUMP_FLIPS` | off | `1` dumps every flip; any other value names a file whose existence turns dumping on. | `nv2a_pb_exec.c` |
| `RECOMP_FIND_NAN` | off | Scans for NaN matrices at every report. | `nv2a_pb_exec.c` |
| `RECOMP_FIND_QUAD` | off | Scans once for quad vertices. | `nv2a_pb_exec.c` |

## Video output

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_FB_WINDOW` | off (Android: on) | Opens a window showing the frame buffer. On Android the app shell sets it unless `recomp.cfg` says `RECOMP_FB_WINDOW=0`; `RECOMP_HEADLESS=1` keeps it off everywhere. | `fb_present.c`, `fb_present_sdl.c` |
| `RECOMP_FB_VA=<va>` | follows the draw surface | Pins the window to one guest address. | `fb_present.c` |
| `RECOMP_FB_WINDOW_DUMP_EVERY=N` | 0 | With `RECOMP_FB_DUMP`: dumps what the window shows every N frames. | `fb_present.c` |
| `RECOMP_WINDOW_SCALE=1..8` | 2 | Client-area scale. | `fb_present.c`, `fb_present_sdl.c` |
| `RECOMP_FULLSCREEN` | off | Starts fullscreen (Win32 and SDL presenters). | `fb_present.c`, `fb_present_sdl.c` |
| `RECOMP_ASPECT=auto\|4:3\|16:9` | auto | SDL window: per-frame aspect modes. `4:3`: every frame 4:3, as before. `auto`: 4:3 unless the title declares wide 3D frames (`xbox_FramebufferWindowSetWide(1)`, e.g. Fable's widescreen patch); then frames with at least `RECOMP_ASPECT_MIN_TRIS` triangles are stretched to 16:9 (WIDE) and the rest stay 4:3, and letterboxed 16:9 movies are cropped and shown at 16:9 (ZOOM). `16:9`: the same for a title that declares wide frames, otherwise every frame stretched to 16:9; the desktop window opens at 854x480 per `RECOMP_WINDOW_SCALE`. Class changes are logged `[FBWIN] aspect NORMAL -> WIDE (...)`; screenshots (`xbox_FramebufferDumpBmp`) take the shown shape (1440x1080 WIDE -> 1920x1080), `RECOMP_FB_DUMP` stays raw. Titles read it with `xbox_AspectSetting()`. Launcher: Settings › Graphics › Aspect ratio. See [GPU Back End](../technical/gpu-backend.md#aspect-modes). | `src/video/aspect_policy.c`, `fb_present_sdl.c` |
| `RECOMP_ASPECT_MIN_TRIS=N` | 5000 | Triangles a frame needs to be shown at 16:9 when the title declared wide frames (Fable: menus 210-1622, scenes 178,000-375,000); a change of class takes two frames. | `src/video/aspect_policy.c` |
| `RECOMP_ASPECT_ZOOM=0` | on | Turns off ZOOM (letterboxed movies cropped to 16:9) when the aspect is wide. | `src/video/aspect_policy.c` |
| `RECOMP_HEADLESS` | off | No window and a dummy audio driver. | `fb_present_sdl.c`, `apu_sdl2.c` |
| `RECOMP_CLOSE_KEEPS_RUNNING=1` | off | Closing the window only closes it instead of exiting the process. | `fb_present.c` |
| `RECOMP_D3D_PRESENT` | off | Presents through a D3D11 swap chain. | `gpu_present.c` |
| `RECOMP_PRESENT_VSYNC` | on | `0` turns vsync off. | `gpu_present.c`, `fb_present_sdl.c` |
| `RECOMP_PRESENT_FILTER` | default | Sampler filter for the D3D11 present. | `gpu_present.c` |
| `RECOMP_PVIDEO=0` | on | POSIX hosts: turns off the PVIDEO overlay's scan-out (films a title plays through `D3DDevice_UpdateOverlay`, composed over the last frame at 640x480 and published as a host frame each vblank a bank is pending; logged `[PVIDEO] overlay on: ...`). The STOP handshake in the NV2A ack pass stays. | `src/video/pvideo_posix.c` |
| `RECOMP_FMV_HOST` | off | Plays a movie through the host player when the title asks to. A substitute for the title's own output. | `kernel_bridge.c` |
| `RECOMP_FMV_DUMP=<prefix>` | off | Writes two frames of a movie as BMP evidence. | `video_pump.c` |

## Frame generation

Only in a build configured with `-DXBOXRECOMP_LSFG=ON` (POSIX hosts); see [Frame Generation](../technical/framegen.md#pacing). With `RECOMP_LSFG=1` the SDL presenter hands every frame to the pacer and shows real and generated frames at their due times ([Presenter](../technical/framegen.md#presenter)); headless runs count them and log `[LSFG] real ... fps, shown ... fps` every 5 s. On Android the launcher writes `RECOMP_LSFG`, `RECOMP_LSFG_MULT`, `RECOMP_LSFG_PERF` (Settings › Graphics › Frame generation) and `RECOMP_LSFG_DIR` (« Choose Lossless.dll ») to `recomp.cfg`.

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_LSFG=1` | off | Frame generation on. A title profile can force it off (`env_force`, [Forced by a title](#forced-by-a-title)). While on, the effective `RECOMP_GPU_FPS_CAP` is min(cap, display rate / multiplier) (a cap of `0` becomes display rate / multiplier: 30 at 60 Hz, 2x) and `RECOMP_GPU_PRESENT=sync` is forced. Both stay for the run if the pacer later falls back to pass-through. | `src/framegen/framegen_policy.h`, `nv2a_pb_exec.c`, `src/gpu/nv2a_gl.c` |
| `RECOMP_LSFG_MULT=2..4` | 2 | Frames shown per real frame (2: one generated frame between two real ones). | `framegen_policy.h` |
| `RECOMP_LSFG_DISPLAY_HZ=N` | 60 | Display refresh rate used by the cap rule. Unset, the windowed presenter sets it to the display's rate when that is below 60 Hz (a faster display keeps 60). | `framegen_policy.h`, `src/video/fb_present_sdl.c` |
| `RECOMP_LSFG_BUDGET_MS=N` | cap period / multiplier (16.7 ms at 30 fps, 2x) | Generation cost above which a frame counts as over budget; 30 in a row (or 30 failed generations) turn frame generation off: `[LSFG] off: <reason>`. | `src/framegen/framegen_pacer.c` |
| `RECOMP_LSFG_FAKE=1` | off | Use the fake generator (CPU linear blend of the two frames, no Vulkan) instead of the LSFG bridge: headless runs and tests. | `framegen_pacer.c` |
| `RECOMP_LSFG_FAKE_MS=N` | 0 | With the fake generator: sleep N ms per generated pair, to exercise the budget fallback. | `src/framegen/framegen_fake.c` |
| `RECOMP_LSFG_DIR=<dir>` | the current cache of the default base | The LSFG bridge's shader cache directory (`<base>/<key>`, made by `tools/lsfg/lsfg_translate` or the app's "Choose Lossless.dll"). The bridge's VkPipelineCache is kept there too. Bridge builds only (`XBOXRECOMP_LSFG_VK_SRC`). | `src/framegen/lsfg_generator.c`, `lsfg_cache.h` |
| `RECOMP_LSFG_DLL=<path>` | unset | Desktop: your own Lossless.dll, extracted into the default cache base when `RECOMP_LSFG_DIR` is unset. The DXBC shaders still need one `lsfg_translate` run. | `lsfg_generator.c` |
| `RECOMP_LSFG_CACHE=<dir>` | `~/Library/Caches/xboxrecomp/lsfg` (macOS), `$XDG_CACHE_HOME/xboxrecomp/lsfg` or `~/.cache/xboxrecomp/lsfg` | The default cache base. | `lsfg_cache.c` |
| `RECOMP_LSFG_FLOW=0.25..1` | 1 | Flow scale (lower is cheaper and blurrier motion). | `lsfg_generator.c` |
| `RECOMP_LSFG_PERF=1` | off | LSFG 3.1 performance shader set (about 4x cheaper). | `lsfg_generator.c` |
| `RECOMP_LSFG_FP16=auto\|0\|1` | auto | Embedded FP16 SPIR-V when the DLL has it and the device has `shaderFloat16` (translated DXBC is always full precision). | `lsfg_generator.c` |
| `RECOMP_LSFG_GPU=<index>` | best device | Vulkan physical device index for the bridge. | `src/framegen/lsfg_bridge.cpp` |

## Audio

See the [APU](../../src/apu/README.md) and [DirectSound](../../src/audio/README.md) module references.

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_APU_TRACE` | off | Voice statistics and host playback accounting. | `apu_core.c`, `apu_vp.c`, `apu_mmio_hook.c` |
| `RECOMP_AUDIO_HOST` | off | Host playback accounting without the rest of the trace. | `apu_core.c` |
| `RECOMP_APU_RING` | off | Ring monitor: finds a streaming writer that falls behind the play cursor. | `apu_vp.c` |
| `RECOMP_APU_SOLO=<voice>` | off | Mutes every other voice, to find where a sound comes from. | `apu_core.c` |
| `RECOMP_APU_VOICE_DUMP=<voice\|all>` | off | Dumps voices as raw PCM to `RECOMP_APU_VOICE_DUMP_FILE`. | `apu_vp.c` |
| `RECOMP_AUDIO_WAV=<path>` | off | Records exactly what the device is given. | `apu_core.c` |
| `RECOMP_AUDIO_WAV_SECS=N` | unlimited | Caps the recording. | `apu_core.c` |
| `RECOMP_APU_HRTF` | on | `0` turns HRTF for 3D voices off. | `apu_vp.c` |
| `RECOMP_APU_MIXDOWN_ALL` | on | `0` limits the mixdown. | `apu_dsp.c` |
| `RECOMP_APU_DSP_ACK` | auto | DSP doorbell acknowledgement: `auto` derives it, `0`/`off` disables, or a list of addresses. | `apu_dsp.c` |
| `RECOMP_APU_PRIORITY` | on | `0` leaves the APU frame thread at the default priority (otherwise nice -16 on Linux/Android, QoS user-interactive on macOS). | `apu_core.c` |
| `RECOMP_APU_EP_CANARY` | on | Keeps the XDK EP program's canary (`0x00CCCCCC` at EP P:6, VA `0xFE85A018`) while the APU frame thread runs; DirectSound resets the whole APU (10 ms stall) on every `DoWork` that reads anything else. `0` leaves the word alone. | `apu_dsp.c` |
| `RECOMP_AUDIO_BUF_COUNT`, `RECOMP_AUDIO_BUF_SAMPLES` | built-in | Device buffer count and size. | `apu_sdl2.c`, `apu_xaudio2.c` |

## Input

See [xbox_input](../../src/input/README.md).

| Variable | Default | What it does | Where |
|---|---|---|---|
| `RECOMP_KEYBOARD` | off | Keyboard acts as a controller. | `keyboard.c`, `usb_gamepad.c` |
| `RECOMP_MKB` | off | Keyboard-and-mouse layer; tuned by `RECOMP_MKB_SENS`, `_DZ`, `_ACC`, `_FULL`, `_INVERT_Y`, `_SWAP_STICKS` or an `RECOMP_MKB_INI` file. | `mkb_input.c` |
| `RECOMP_KEY_TRACE` | off | Prints the first 40 key-downs. | `keyboard_sdl.c`, `fb_present.c` |
| `RECOMP_INPUT_DIAG` | off | Reports whether the input chain is wired. | `usb_gamepad.c` |
| `RECOMP_RUMBLE` | on | `0` turns rumble off. | `ohci.c` |
| `RECOMP_USB` | off | Enables the USB controller model. The title project calls `xbox_OhciInit()` after the memory layout (the template does). | `ohci.c` |
| `RECOMP_USB_PADS=1..4` | 1 | Number of pads plugged. | `ohci.c` |
| `RECOMP_USB_PORT=0..3` | 2 with four ports | Root port the pad appears on. | `ohci.c` |
| `RECOMP_USB_HC`, `RECOMP_USB_NDP` | model default | Host-controller selection and port count. | `ohci.c` |
| `RECOMP_OHCI_CATCHUP` | on (needs the gate and safe points) | When the controller thread is more than 16 ms overdue and the driver has written ControlListFilled or BulkListFilled, a guest thread walks the lists at its release point and runs the USB interrupt there (log: `[OHCI0] controller thread N ms overdue; lists serviced on the guest thread`). Keeps a starved host thread from letting per-frame control transfers (rumble) pile up past XAPI's descriptor budget (KOTOR, K-8c). `0` restores the controller thread alone; an A/B switch kept for one release. | `ohci.c`, `kernel_hal.c` |
| `RECOMP_USB_TRACE`, `RECOMP_USB_STATS` | off | Controller trace; completions every 5 s. | `ohci.c` |
| `RECOMP_PAD_LIVE=<file>`, `RECOMP_PAD_SCRIPT`, `RECOMP_PAD_PRESS` | off | Test input: a file read live, a script, or a periodic press. | `usb_gamepad.c` |
