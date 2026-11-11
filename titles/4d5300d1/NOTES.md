# Fable: The Lost Chapters — recomp bring-up

Private, never commit to xboxrecomp-android: contains game data and generated code.

| Dir | What |
|---|---|
| `toolkit/` | clone of xboxrecomp-android (main @ 8e50e7e) |
| `game_files/` | ISO unpacked (`tools.xiso unpack`), `default.xbe`, `default_analysis.json` |
| `analysis/` | disasm, func_id, abi outputs + build/run logs |
| `game/` | title project from `templates/new-game`, generated C in `src/recomp/gen/` |

## Pipeline (2026-11-09)
Python 3.12 venv with capstone (system Python is 3.9). Brew: python@3.12, sdl2, libepoxy, cmake, ninja.

```bash
P=python3.12-venv; A=../analysis
$P -m tools.xbe_parser ../game_files/default.xbe --json ../game_files/default_analysis.json
$P -m tools.disasm ../game_files/default.xbe -o $A/disasm          # all code sections, not --text-only (S_* quest code)
$P -m tools.func_id ../game_files/default.xbe --functions $A/disasm/functions.json --strings $A/disasm/strings.json --xrefs $A/disasm/xrefs.json -o $A/func_id
$P -m tools.abi_analysis ../game_files/default.xbe --disasm-dir $A/disasm --func-id-dir $A/func_id --output-dir $A/abi
$P -m tools.recomp ../game_files/default.xbe --all --split 250 --disasm-dir $A/disasm --func-id-dir $A/func_id --abi-dir $A/abi --gen-dir ../game/src/recomp/gen
cd ../game && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
cd build && ln -sfn ../../game_files game && ./fable_recomp
```

39,756 functions, 165 C files (246 MB), build ~1 min on the Mac.

## Toolkit fixes (branch `fix-posix-template-and-cr-regs` in toolkit/, not committed yet)
- `templates/new-game`: Win32 VEH / dbghelp under `_WIN32`, POSIX uses `xbox_PosixInstallMmioFaultHandler()`,
  explicit `recomp_dispatch_init` declaration, Windows libs under `if(WIN32)`. game/src/main.c is now the template's
  with the three Fable constants (old local version: analysis/main.c.local).
- Lifter: `mov` to/from crN/drN -> `xbox_ReadCr/WriteCr/ReadDr/WriteDr` (runtime in kernel_hal.c, declared in
  recomp_types.h). The game-side cr3 shim is gone.
- After regenerating with a newer toolkit, delete `game/src/recomp/gen/recomp_types.h` first: recomp never overwrites it.

## First run (macOS arm64)
Boots through memory layout, kernel bridge (154/154 thunks), CRT start, PsCreateSystemThreadEx into the game thread.
- Heap probe: allocations of 608/304/152/76 MB fail (heap is 45 MB), then it goes on.
- UNIMPL reached: `invlpg` (0x000E0E2C), `cli`/`sidt [esp]`/`sti` (0x000AB213..).
- **SIGSEGV** in `sub_00025BD0+284` reading guest VA 0x74726174 (ASCII "tart": a string used as a pointer).
  Chain: xbe_entry_point → sub_001E76CE → PsCreateSystemThreadEx → sub_001E7636 → sub_001E6A00 → sub_001EE930 →
  sub_001ED670 → sub_001EEAC0 → sub_0004CA24 → sub_000244B0 → sub_00024BE0 → sub_00025BD0.

## Crash 1 fixed: the 608 MB reserve (2026-11-09)
Fable reserves 608 MB (MEM_RESERVE) for its allocator; the runtime clamped it to 38 MB, the allocator walked into the
RAM mirrors and two strings shared storage ("tart" as a pointer). Fix in game/src/main.c: `xbox_SetMapSize(1 GB)` before
`xbox_MemoryLayoutInit` (the runtime's documented answer, same as Half-Life 2). Also: chdir to the binary's folder at
start (Finder launch).

## Bring-up log, 2026-11-10 (all in game/src/main.c unless noted)
1. D3D ring make-space spin (sub_00855260): RECOMP_PB_EXEC=1 + host thread handing device+0x30 (device ptr at
   0x00862288, device 0x0085FDE0, semaphore 0x80966000) to nv2a_pb_set_semaphore_target(). First flip, loads graphics.
2. DirectSound wait (sub_00832BFC, DSOUND section): RECOMP_AC97_READY=1 (emulated APU). Loads shaders, xfrontend.bin.
3. "out of thread stacks" at the 12th worker -> ran inline -> deadlock. Toolkit: RECOMP_THREAD_STACKS (commit on the
   branch, not pushed); Fable sets 32. First draw call (1 draw, 2 flips).
4. Desktop POSIX layout with map > 64 MB loses contiguous-window pages (host malloc trap; guard malloc fault at
   0x80005000 in xbox_ContiguousAllocEx). Worked around with RECOMP_GUEST_WINDOW=1. Toolkit bug, not fixed yet.
All of these are setenv() defaults in main.c, so a double-click runs the same thing.

## Now: everyone waiting
Main thread loops in sub_0003B530 (sleep/poll, via sub_001EB6D8 -> KeDelayExecutionThread); every guest worker waits
on an event (sub_001EB630) or sleeps (sub_005FE220). Next: find what sub_0003B530 polls and which worker should set it.
Logs: analysis/run10.err, analysis/lldb-hang4.txt.

### Hang investigation, 2026-11-10 (continued)
- sub_0003B530 is a loading-screen loop: Sleep(1), progress callback, exits when [ebx+0x54] (done) is set or a timer
  runs out. So it waits for the loader workers, which all wait on events (sub_001EB630) or sleep.
- The loader decompresses d:\...\*.biz into z:\...\*.big with ReadFileEx (APC = NtUserIoApcDispatcher, ordinal
  232, ~360 reads). It stops right after opening d:\Data\misc\xbox\effects.biz (no read follows).
- Ruled out: stale Z: cache (moved to ~/.local/share/xboxrecomp/Cache.old, fresh run stops at the same place);
  deferred file APCs (xbox_SetDeferredIoApc(1), no change, reverted).
- RECOMP_SAVE_DIR moves T:/U: but not Z: (still ~/.local/share/xboxrecomp/Cache) -- toolkit inconsistency to check.
- Next: find which worker should issue the effects.biz reads and what event it waits on (sub_005ED160 job queue?).

### Session 2026-11-10 (later)
- Window: RECOMP_FB_WINDOW=1 + main() goes through xbox_HostWindowMain (window on the macOS main thread). Opens; black.
- The "effects.biz" stall was not one: copies finish. The real loop was the intro movie (lionhead_logo.xmv):
  sub_0003B530 polls XMVDecoder_GetNextFrame (sub_006440AB, XMV section) which never leaves XMV_NOFRAME.
  src/fable_overrides.c replaces it with "XMV_ENDOFFILE" (recomp --exclude-manual src/fable_overrides.c). Movies skipped;
  front-end loads (frontend.big, text.big, shaders.big, frontend.lug), 3 flips.
- Semaphore glue fixed: 0x00862288 sometimes holds a heap object; only accept a semaphore in contiguous memory.
- Wild ICALL to 0x00051120 (inside 0x50F8E..0x5126F per disasm). Seeded it (analysis/seed_functions.json, disasm
  --seed-functions, full pipeline rerun). Now sub_00051120 -> ... -> sub_001D5BF0 -> sub_001D5B00 (vector<8-byte>
  grow) gets a heap pointer as an element count -> operator new(0x41F529C0) -> NULL -> null calls. Suspect 0x51120 is a
  secondary entry of the function at 0x50F80 (start moved from 0x50F8E after seeding) and lifting it alone loses its
  frame. Next: check the disassembly around 0x50F80..0x5126F and who calls/jumps to 0x51120.

### Session 2026-11-10 (third pass)
- 0x51120 is not a function: it is the join point after `call sub_00027360` (stream Read(&local, 4), thiscall ret 8)
  at 0x5111B inside the function at 0x50F80, reached also by jl/jb/jg/ja/jmp. The seed only made the lifter split
  it into tail calls (which look correct). Original failure (pre-seed: function at 0x50F8E, unresolved ICALL to
  0x51120) still unexplained -- compare with a disasm run without the seed.
- The bad element count (0x083EA538, a heap pointer) is read straight from the stream buffer at 0x510FB.
- Cache .big files look sane (BIGB header, index offset ~ file size, readable UTF-16 text): decompressor probably OK.
- Threads: without RECOMP_GUEST_LOCK the wild calls differ run to run; with RECOMP_GUEST_LOCK=1 runs are
  repeatable (7 bad calls, same place) but about half the runs stall at D3D init (0 flips, D3D ring spin).
  Wild call chain with the lock: return addresses 0x00013BBE, 0x0001B138, 0x0037453D under sub_00051120.
- recomp_manual.c now prints the guest stack's code addresses at each new wild indirect call.
Next: decide whether 0x51120 should be one function with 0x50F80 (drop the seed, find why the original ICALL went
there), then follow the stream object (ebx) back to whoever filled its buffer.

### Session 2026-11-10 (fourth pass) -- toolkit updated to main c21ec4c
- toolkit: branch fable-local = origin/main + RECOMP_THREAD_STACKS; branch fix-gap-prologue-switch-arms adds the
  disasm fix (commit 02f3cf8, local, not pushed). game/ ported to the XBA-52 template (recomp_title_main,
  XBOXRECOMP_SHELL_TITLE block): can be built into the APK with -PxbaTitleDir (not tried yet).
- FIXED (toolkit): memmove (0x0057FE40) jump table on n&3 lost 2 of 4 arms: a false function at 0x0057FFB0
  (gap_prologue on a switch arm). Short copies dropped their tail -> text parser 2 bytes out of step -> giant counts.
  After the fix: no more giant allocations; only 1 function removed in the whole title. All 279 indexed switches
  checked against the XBE tables: none short.
- Seed 0x51120 dropped (it is a join point inside 0x50F80); disasm without seed.
- Still: text records occasionally parsed from the wrong buffer. File reads go through one contiguous bounce buffer
  (0x831D2000) and completion routine sub_000AB4C0 copies to [0x9A10A8] (start [0x9A10A0]); the copy is right, but
  the parser's stream base ([ebx+0x18]) sometimes points 8 bytes past the end of the copied block, at a block whose
  first word links to the copied one (freed / reallocated block?). That bad record then yields a huge element
  count -> vector fill from NULL writes zeros over the kernel import table (0x876C94) -> null kernel calls.
  Deferred file APCs (now default in main.c, FABLE_INLINE_APC=1 to disable) do not change it.
- Generated C currently carries temporary traces ([STREAM], [RD], [WSTR]); a regen removes them.

### Session 2026-11-10 (fifth pass)
- toolkit verified on origin/main c21ec4c (nothing missing). Pushed branch disasm-switch-arms (memmove fix only, off
  main) for a PR. Local main aligned to origin/main.
- Text buffer mismatch narrowed: the stream (vtable 0x0090345C) gets its data pointer from a load request
  ([[edi]+0x38], accessor sub_00049380 -> [req+8]); the async reader (0x000AB6xx, completion sub_000AB4C0, globals
  0x9A10A0..0x9A10D8, bounce 0x831D2000) copies into the block just before it (same size, +8 header). Looks like the
  buffer was allocated twice (loader thread vs main thread via sub_00049310 -> sub_000490D0) or the reader's shared
  globals were used by two requests at once.
- RECOMP_CS_MODE: default and guest still show the bad record; single not measurable (every run stalled early).
- With RECOMP_GUEST_LOCK=1, most runs stall at D3D init (0 flips, make-space spin, after names.bin). Not the
  ISR/DPC theory (they do not take the lock). Unexplained.
Next: tag reads and buffer allocations with the host thread id to confirm two threads in the reader; find why
the lock stalls D3D init (RECOMP_GUEST_CPU_TRACE).

## 2026-11-10: TITLE SCREEN
- Root cause of the text-parse corruption: runtime bug, RtlTryEnterCriticalSection used its own host-lock table
  (kernel_bridge.c g_try_cs), separate from Enter/Leave (kernel_rtl.c xbox_cs_shadow). Fable's loader thread
  TryEnters a load request while the main thread Enters it -> both allocate the buffer -> main thread parses the
  empty one. Fix: xbox_RtlTryEnterCriticalSection in kernel_rtl.c on the shared shadow + tests/kernel_try_cs_posix.
  Branch rtl-tryenter-shadow (pushed, off main).
- toolkit for Fable: local branch fable-integration = main + RECOMP_THREAD_STACKS + disasm switch-arm fix + TryEnter.
- Result: no bad calls, ~400-560 flips, ~11k draws per 10 s report, ~1.8 fps. Frame dumps
  (RECOMP_FB_DUMP=prefix) show the Fable: The Lost Chapters title screen (logo, forest, character, copyright).
  Text renders as blocks (font texture issue). Keyboard: Enter = START, Z/X/C/V = A/B/X/Y, arrows = d-pad, WASD stick.
- Open: 1.8 fps; font rendering; RECOMP_GUEST_LOCK D3D-init stall (not needed now: runs fine without the lock).
- FB window: RECOMP_FB_WINDOW must be set before xbox_HostWindowMain (main()), not in game_main. Fixed.
- Text fixed (toolkit, branch nv2a-al8, pushed): the CPU rasteriser had no AL8 (0x01 / LIN 0x1B) texel decode;
  Fable's glyph cache is a 512x512 AL8 texture, every glyph quad took the vertex colour. Test: nv2a_tex_filter AL8
  ramp (12/12 fail without the fix). fable-integration now has it too.

## 2026-11-10: speed + cache progress
- Profile: the software rasteriser is the cost (game code is cheap). Bilinear DXT fetches ~50%, combiners ~35%.
  Thread count: default (5) is best; -mcpu=native: no gain.
- Toolkit branch nv2a-raster-perf (pushed): whole-block DXT decode (tests/dxt_decode), 16k block cache, single-block
  bilinear fast path, combiner tweaks, [GPU] fps over wall time (it used clock() = CPU time). 130 -> 170 flips/10 s.
- Beyond that: a GPU backend for the pushbuffer executor (XBA-27); the nv2a_backend.h interface exists but has no
  implementation and does not carry register-combiner state.
- game/src/main.c: [CACHE] progress thread (biz! header = decompressed size; sums z:\data\*.big sizes), shown in the
  window title and printed. 626 MB in total for Fable.

## 2026-11-10: input
- No input at all: the USB controller model was never started. xbox_OhciInit() is called by nobody (not the template,
  not the runtime) and needs RECOMP_USB=1; the keyboard pad needs RECOMP_KEYBOARD=1. main.c now sets both and calls
  xbox_OhciInit() right after xbox_MemoryLayoutInit. XAPI then enumerates the pad (port 3) and polls it.
- With RECOMP_PAD_PRESS=0x10 (Start pulses) the title leaves the title screen and reaches a loading screen with a tip
  and a progress bar. Toolkit follow-up: the template should call xbox_OhciInit too.

## 2026-11-11: contiguous memory
- `[CONTIG] arena exhausted` after the first world load: Fable frees about 16 MB of contiguous memory per load
  transition (MmFreeContiguousMemory via its allocator 0x000AC000 -> D3D 0x001EC2C0), and the runtime kept every
  freed block unless RECOMP_HEAP_RECLAIM was set. Toolkit fix: contiguous frees are honoured by default
  (RECOMP_CONTIG_RECLAIM, =0 restores the old behaviour); the profile sets nothing. RECOMP_HEAP_RECLAIM (general heap,
  NtFreeVirtualMemory) stays off for Fable.
- 150 s run with Start pressed: 0 `arena exhausted`, last `[CONTIG]` line in use 52524 KB, peak 52524 KB of 65408 KB,
  allocs=181 released=63 free calls=63. The run then stops at the in-world stall in sub_0012F7F0 (~220-250 flips).
- About 26 MB of the window is Fable's own demand-paging pool (XPhysicalAlloc from sub_000AB270, xboot.ini
  PhysicalSize = 26544 KB), which its pager never uses here (sidt/invlpg are no-ops). Harmless at this peak.

## 2026-11-11: GPU back end (XBA-27 7a+7b, branch xba-gpu-backend)
- RECOMP_GPU=1 draws with the GL back end (src/gpu, docs/technical/gpu-backend.md); the CPU rasteriser stays default.
- Title screen, Mac, back to back: CPU rasteriser 71-93 ms render per frame (10.5-13.8 fps); GL 2.1-2.6 ms
  (225-278 fps: the "Press START" screen is not capped by the game when the renderer is fast).
- Frames match the CPU path: title 0.34/255 mean diff, AutoSave screen 0.33/255 (progress bar end only).
- Every title-screen batch is a vertex-program batch (15336 of 15336); 3 shader programs cover the front end.
- After Start both paths stop flipping after the AutoSave screen: the pushbuffer stall (WP2/WP3), not the back end.

## 2026-11-11: in-world stall in sub_0012F7F0 fixed (pushbuffer jumps + deferred RunPushBuffer)
- sub_0012F7F0 (inlined D3DDevice_GetVisibilityTestResult) waited for reports the executor never wrote: the segment
  scan stopped at every top-level JUMP, and XDK D3DDevice_RunPushBuffer (0x00855900) jumps from the ring into recorded
  pushbuffers. Toolkit: the scan follows jumps, one walk from the previous PUT to the new one (wraps included), and a
  walk that loops is dropped and counted (docs/technical/pushbuffer-executor.md).
- Following them needs D3D's software methods 0xC/0xD/0xE (NO_OPERATION with a value): they patch a re-run
  pushbuffer's return JUMP. title.c registers fable_software_method() with nv2a_pb_set_software_method_hook(), a
  re-implementation of sub_00855860 (node table 0x00860044, pending count 0x0085FE30). Without it the walk loops
  (`[PB] runaway segment walk (jump taken twice)`) and the game stalls as before.
- Not serviced (counted in `[GPU] software methods:`): 6 (InsertCallback, ~1 per frame), 8 (PGRAPH RDI, ~30 per
  frame in-world), 9 (register poke). Delivering D3D's own PGRAPH interrupt instead is blocked by
  write-1-to-clear on plain-RAM registers (its DPC never leaves); see the executor doc.
- Needs the contiguous frees (RECOMP_HEAP_RECLAIM=1 for now) to get past the world load at all.
- Side effect: the walk now runs D3D's init commands earlier, so the semaphore hand-over race (a
  BACK_END_WRITE_SEMAPHORE_RELEASE before semaphore_watch has set the target is dropped; main thread stuck in
  sub_00855260, 0 flips) went from 0 of 6 starts to 4 of 8 on 2026-11-11. The pending-release fix for that race
  must land with this.

## 2026-11-11: FPS quick wins (WP5)
- The main thread's 92-100 % "in sub_00855520" was D3D's flush-kick wait sub_008556D0 (inlined into its caller in
  the shared build): it sets PFB 0x100410 bit 16 and spins until the ack thread clears it, i.e. until the executor
  gets through what it is drawing. overrides.c replaces it with a wait that yields, then sleeps 50 us a turn
  (FABLE_D3D_SPIN=1: the old spin). Needs the generated code regenerated (prepare / tools.recomp --exclude-manual).
  Mac title screen 12.3-13.2 -> 17.5-17.7 fps, main thread from 100 % spinning to sleeping.
- Front end Sleep(50) at 0x0020BD93 / 0x0020C3C5 -> 16 ms (FABLE_FRONTEND_SLEEP_MS, kernel delay override).
- Executor: vertex programs shaded on the raster pool; RECOMP_ACK_IDLE_US (off until the scan follows jumps).
- Seen during these runs: 0-flip starts (D3D waiting in sub_00855260, GPU semaphore behind) in roughly 1 run in 4
  on a loaded Mac -- the init/semaphore race of the investigation (section 4.1), more frequent than its 2 in 14.

## 2026-11-11: movies and sound on (cycle 3 WP-1..3, branch xba-fable-movies)
- Movies: the lifted XMV decoder works; its clock is the vblank count at device+0x1DE8
  (D3DDevice_GetDisplayFieldStatus, sub_0084F650), which only moves with vblanks. title.c now defaults
  RECOMP_VBLANK=paced and the sub_006440AB "end of file" override is gone. **Regenerate the C after pulling**
  (sub_006440AB needs its body back): tools.prepare, or
  `python -m tools.recomp <default.xbe> --all --split 250 --game-name "Fable: The Lost Chapters"
  --disasm-dir <analysis>/disasm --func-id-dir <analysis>/func_id --abi-dir <analysis>/abi
  --gen-dir <project>/src/recomp/gen --exclude-manual titles/4d5300d1/overrides.c` (~4.5 min on the Mac).
  Against the previous tree only sub_006440AB's body, its call sites and the chunk boundaries change.
- With movies on, DirectSound hung in its AC'97 channel reset (sub_00832BFC) when two threads reset channels
  at once: the POSIX trap opened the page for everyone. Toolkit: read-only shared page + writable view
  (RECOMP_AC97_ALIAS=0 for the old window).
- DirectSound reset the whole APU on every DoWork (10 ms stall each) because the EP canary at 0xFE85A018 read 0.
  Toolkit keeps 0x00CCCCCC there (RECOMP_APU_EP_CANARY). Reset counter at 0x834934: 0 with it, ~1,300 per 80 s
  of movies without it.
- Silence outside the movies: the sound/music buffers live in the 608 MB reserve at 0x2xxxxxxx and the APU
  folded their addresses to 28 bits (read zeros). Toolkit fix in mcpx_apu_phys; title screen music
  (LOOKOUT_POINT.wma), Oakvale.wma and the world are audible (WAV rms 500-3,800 per 10 s window, was 0).
- Mac, 10 no-input runs of 80 s: lionhead_logo, Microsoft_Logo, intro_comp in order every time; movie fps
  29.8-30.4 when the machine was not saturated (20-27 under load average ~20); resets 0.
  1 of the 10 stalled in intro_comp at 1,280 flips: the kernel timer thread never came back from the USB
  driver's DPC (sub_00875EA1 -> sub_00875B3C -> sub_00875A48 loop, 930/930 samples; OHCI0 WDH interrupts),
  so no other DPC ran, the vblank count stopped and the decoder with it. That is the OHCI done-queue race
  (cycle 3, 5a), not the movies; the t1 stall at 272 flips was probably the same (it was not sampled).
- Continue -> Load Game -> AutoSave plays dream_sequence_comp.xmv (not skippable) and reaches Oakvale.
- RECOMP_GPU=1 in Oakvale after the AutoSave: 32.6-32.8 fps with this branch, 33.0-33.2 with the previous
  profile and tree (movies skipped), 32.6-33.4 with the previous one plus RECOMP_VBLANK=paced: paced vblanks cost
  nothing measurable in-world.
- Open: in 2 of 3 GPU runs with movies on, pad input stopped for good right as the title screen appeared after
  the intro was skipped (the guest stopped polling the interrupt endpoint: no more pad reports; in one of them the
  attract movie then stalled at 0 fps). Not seen in 2 baseline GPU runs nor in 3 CPU-rasteriser drives. Same
  symptoms as the OHCI race (5a) and the DPC stall above; not root-caused here.

## 2026-11-11: pad input dies (branch xba-fable-input)
- Root cause of the "pad input stops for good" above and of the intro_comp stall: not the OHCI walker. XAPI's USB
  stack (XPP section 0x0086E480-0x00876C3C) uses KeRaiseIrqlToDpcLevel as its only lock. XInputSetState (~30/s,
  sub_0007F4C0 -> sub_008701C5 -> sub_0087249E) raises and programs a control transfer that takes 4 TDs
  (sub_008766DD, alloc sub_00876613: eax = [0x9A614C]; [0x9A614C] = [eax+0x14]); the USB DPC sub_00875EA1 frees
  TDs (sub_00875341), completes URBs and re-arms the pad's interrupt URB ~200/s on the kernel timer thread. The
  ~167 TDs are shared by the control and interrupt EDs. Without the one-CPU gate kernel_drain_dpcs never waits for
  a raised guest thread (and a raise never waits for a running DPC): ~5-7 overlaps/s, TDs queued twice or while on
  the free list, the free list cycling. The end state is the DPC looping in sub_00875A48 (failed-TD path) for ever:
  no more DPCs (vblank count stops, movie stalls) and the driver ISR sub_008757CD left MIE masked, so the OHCI model
  went quiet without a warning.
- Fix: title.c defaults RECOMP_DISPATCH_GATE=1 (one-CPU DISPATCH gate, kernel_hal.c), which orders DPCs against
  raised threads. Needs RECOMP_VBLANK=paced (already the default). RECOMP_DISPATCH_GATE=0 for the old behaviour.
  Investigation numbers (Mac): gate off, driven scenario (A, A, A, Start, A, A) 3 of 30 runs lost input 40-50 s in;
  allocator window widened 50 us (debug build) 6 of 6; gate on, driven 0 of 36, widened 0 of 13. fps gate on vs off
  at the same time: identical (30.0/30.0, 7.7/7.7 under 40 busy loops).
- Toolkit: the OHCI model now raises FrameNumberOverflow (the driver keeps the high word of its 32-bit frame count
  at ext+0x418 from the FNO handler 0x00875801; without it the count wrapped at 65536). It warns when MIE stays
  masked > 2 s with a pad plugged (naming the DPC running on the timer thread), and with the gate off the IRQ
  report counts DPCs that started while a guest thread was raised.
- Verified on the Mac with the profile default (gate on), RECOMP_GPU=1, 5 runs in parallel: driven scenario 70 s
  30 of 30 kept input (6/6 pad commands seen, ~22,000 pad reports each, no [OHCI0] WARNING, no stall). Debug build
  with the TD allocator window widened 50 us: gate on 10 of 10 kept input; RECOMP_DISPATCH_GATE=0 3 of 5 lost it
  (no report for 5 s warning, reports frozen at 4,700-6,700), the other 2 kept it at ~120 reports/s against ~400/s
  gate on; every gate-off run printed the "[IRQ] gate off: N DPC(s) started while a guest thread was at
  DISPATCH_LEVEL" line. RECOMP_PAD_PRESS=0x10, 150 s: 10 of 10 played lionhead_logo, Microsoft_Logo, intro_comp,
  dream_sequence_comp in order, no 0.00 fps, no warning. In Oakvale, 2 gate-on and 2 gate-off runs at once:
  28.1-29.5 fps both ways.

## 2026-11-11: kick wait as events, visibility reports without waiting (cycle 4 WP-1, WP-2)
- sub_008556D0 now calls xbox_Nv2aKickWait: the main thread sleeps on a condition the ack thread signals when it
  clears the PFB flush bit; the idle ack thread sleeps on the kick (50 us steps, each ended by a kick) instead of
  Sleep(0) polling. FABLE_D3D_SPIN=1 keeps the spin. Answering the kick after every draw batch inside the segment
  scan was tried and dropped: the title ran ahead, took D3D's far-behind path into the 0x400B10 latch loop
  (sub_008551F0, not latched by the segment scan) and froze after 1115 flips.
- Reports: resolved at the idle hook / flip / after the present without waiting; a head report stuck 300 us gets a
  glFlush, 2 ms a wait. RECOMP_GPU_ZPASS=lazy answers 1 instead of waiting (Android candidate).
- Mac, Oakvale tutorial prompt after the AutoSave, back to back on xba-pack-loader + these two commits, load 4-7:
  fps (steady 5 s reports) 37.4-39.4 -> 54.6-56.2 (4 runs); render 13.6 -> 13.1-13.6 ms/frame; executor report
  resolve 25-28.5 % -> 0.4-1.0 %, yield poll 5.2-5.5 % -> 0.7-2.4 %; 9-34 waits per 5 s (~0.03-0.12 a frame),
  idle hook 0.6-0.8 ms/frame; sub_0012F7F0 <= 0.1 % of the main thread, which sleeps in the kick wait (no yield or
  nanosleep under sub_008556D0). Title 60 fps at the cap, process CPU 24 % -> 18-20 %. 5 runs of 160 s, no stall.
- Before the rebase (base 8fdcf01): the loading screen ran at 3-17 fps in 3 of ~16 runs with WP-1 (30 in the others
  and in every base run): two with an earlier single 250 us event wait (one then froze at 0 fps with the main thread
  in the loader's Sleep loop, sub_0003B530), one with the 50 us steps at load average 33. Not seen after the rebase
  (7 runs). Watch for it.
- RECOMP_GPU_ZPASS=lazy, same spot: 56.2-56.4 fps, 0 flushes, 117-582 reports answered lazily per 5 s, but still
  18-22 single-report waits per 5 s: the 512-pending cap (zpass_queue), which lazy does not change. On a tiler that
  is still a mid-frame wait; answer the cap lazily too if the device shows it.

## 2026-11-11: 16:9 scenes (aspect WP-2, branch xba-aspect-fable)
- Fable ignores XC_VIDEO_FLAGS_WIDESCREEN/HDTV and always renders 640x480 (0x0003EB43 mov eax,0x280;
  0x0003EB7B mov ecx,0x1E0 -> device creation sub_0003EEB0; config globals 0x912B64/68 read once, then ignored).
  XGetVideoFlags is sub_00593E13; its two callers only test PAL60 (0x40).
- Camera setup sub_00132D00 (from every scene pass via sub_00131FC0): w,h = viewport; p = [0x009A1188]; if p and
  float[p+8] > 0 and float[p+0xC] > 0: w,h = those; aspect = w/h -> cam+0xD0; unless byte[param+0x54] (explicit
  fov): t = ((0.75 - h/w)*0.5 + 1)*fov*0.5, xs = cot(t) -> cam+0xD4, ys = xs*aspect -> cam+0xD8 (folded into the
  view matrix). [0x009A1188] stays 0 in normal play; writers sub_00125ED0, sub_00125A20 (a level reload can reset
  it); the camera is its only reader. fov 60: 4:3 hfov 60 / vfov 46.8; 16:9 hfov 65.6 / vfov 39.9.
- Cutscene bars: sub_003C0070 calls thiscall sub_0023EB70(0.5, 0.5, 16/9) at 0x003C01A4 (ret 12), target aspect
  stored at this+0x8C (the object is [[0x00988C60]+0x28]); the bars are drawn over a full-frame render (picture
  rows 53-420). 4/3 there: no bars, subtitles and Skip prompt unchanged.
  The AutoSave cutscene reaches it through sub_00303170 (a tail jmp to sub_0023EB70, called at 0x004B7CB8), not
  from 0x003C01A4, so the hook tests the value, not the caller.
- Movies: story XMVs 640x360 letterboxed on rows 61-420 (pure black bars); logos 640x480; GUI clips 256x256.
- overrides.c entry hooks (need the generated code regenerated with this overrides.c, --exclude-manual):
  sub_00132D00_enter points [0x009A1188] at a zeroed 64-byte guest blob (xbox_HeapAlloc(64,16), once) when it is
  0 and writes 16.0/9.0 at +8/+0xC of whatever it points to, before every camera setup, so a reload that resets
  the pointer is covered on the next pass; sub_0023EB70_enter replaces a third argument in (1.7, 1.8) at
  [esp+0xC] (entry, before its push ecx) with 4/3. Both do nothing unless g_fable_aspect_wide.
- title.c fable_aspect_configure (from xba_title_configure): wide when xbox_AspectSetting() is 2 (16:9), or 0
  (auto) on Android; desktop auto stays 4:3. Wide sets g_fable_aspect_wide and calls
  xbox_FramebufferWindowSetWide(1); "[ASPECT] 16:9 scenes" is printed. Both functions belong to src/video
  (src/video/aspect_policy.c, declared in video_player.h, every configuration).
- The profile compiles into the Android pack too: there auto means 16:9 scenes.
- Regenerate after changing the hooks (the generated sub_00132D00 and sub_0023EB70 call them):
  `python -m tools.recomp <xbe> --all --split 250 ... --exclude-manual titles/4d5300d1/overrides.c`
  (tools.prepare passes the profile's exclude_manual); only recomp_0020.c and recomp_0036.c change.
- Mac, RECOMP_GPU=1, headless pad scripts, RECOMP_ASPECT=16:9: "[ASPECT] camera display size 16 x 9 at <blob>
  (ours)" at the first scene, "[ASPECT] cutscene bars 16:9 -> 4:3" at the AutoSave cutscene; gameplay drawn
  anamorphic, cutscene full frame with subtitles and Skip Scene, story XMVs unchanged (letterboxed), menus
  unchanged. A pause-menu Load (a quick reboot of the title) re-applies both. Auto/4:3: no [ASPECT] line,
  cutscene bars and proportions as before.
- With the presenter (windowed, RECOMP_FB_WINDOW=1, RECOMP_ASPECT=16:9): [FBWIN] NORMAL on logos and menus, ZOOM
  (rows 60-419) on the story movies, WIDE on the cutscene and gameplay (217,000-231,000 triangles).
- Cost: headless, gameplay 57-60 fps either way (paced at 60). Windowed, back to back, the first ~40 s in Oakvale:
  hooks off (presenter wide only) 45.9 -> 59.5-59.9 fps; hooks on 43.5 -> 46-52 fps, still climbing (~4 % more
  triangles at the start; the wider view brings in more of the town). Menus are 40 fps windowed with 16:9 with or
  without the hooks (60 with auto): the presenter's, not the camera's.
