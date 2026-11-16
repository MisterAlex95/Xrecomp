# Xbox save states

> Part of the [Documentation Index](../INDEX.md)

How statically recompiled Xbox titles can get save states, « Resume where I left off » and rewind (XBA-244), the
features the console runtimes have had since 0.8.0 (XBA-182, XBA-194) and the Xbox lacks.

## Verdict

- **Not feasible in general with the code we generate today.** A recompiled thread keeps part of its state in host
  stack frames (its return path and its `ebp`), which nothing can serialise. A console-style « snapshot the CPU
  anywhere » does not exist for us.
- **Feasible for a defined class of titles, in stages**, with a safe-point snapshot: freeze every guest thread at a
  kernel call (prototype done: `src/kernel/kernel_park.c`), serialise guest memory, register sets, kernel objects and
  devices, and restore by putting threads back *at the same call chains*:
  1. **In-session** (rewind, quick save / load within one run): restore in place when every thread is parked at the
     same chain as in the state. Needs one lifter change (`ebp` kept out of host frames across calls).
  2. **Across runs** (save slots on disk, « Resume where I left off »): rebuild the host stacks in a fresh process by
     re-entering lifted functions at their call sites (resume entries, as Emscripten's Asyncify rewinds). A larger
     lifter change.
- **Process-level snapshots (fork, CRIU) are rejected**: not available on Android, and they cannot carry the GPU
  context, the audio device or the JVM the app runs in.
- **The class of titles**: every guest thread regularly enters a kernel wait or call (the usual XDK title: D3D's
  present and vblank waits, DirectSound's worker, loaders waiting on events); no guest fibers or hand-made stack
  switches (XBA-101: Fable is out); no thread spinning on guest memory without kernel calls; built through the
  kernel bridge (not the native-call thunk table). A title outside the class says so (« save states unavailable:
  <reason> ») instead of offering a state that cannot load.

## Why the Xbox is different

The GBA, PS1 and N64 runtimes interpret or recompile one CPU whose whole state is a register file plus memory; they
take a state between frames, on their own frame thread (`recomp_states.h`). An Xbox title is recompiled to C
(`docs/technical/register-model.md`):

| Part of a guest thread | Where it lives | Serialisable? |
|---|---|---|
| eax..edi, esp, eflags image, x87 stack, SSE, MMX | `RecompCpu g_cpu`, one per host thread (`recomp_types.h`) | Yes (about 300 bytes) |
| Guest stack (arguments, saved registers, **return addresses**) | Guest RAM | Yes |
| The return path itself | The **host** stack: a guest `call` is a C call, a guest `ret` a C return | No |
| `ebp` | A C local in every lifted function (FPO code uses it as a scratch register); across a call it exists only in the caller's host frame | No |
| Flag snapshots (`_fa`, `_fb`, ...) | C locals, never live across a call | Not needed |
| SEH anchors (`setjmp` per EH frame, XBA-112) | Host-side tables pointing into the host stack | No (must be re-taken) |
| Fibers (XBA-101) | One extra host stack per guest stack | No |
| Wait in progress | A host wait inside a kernel bridge | Only as « in call N from return address R » |

So the only places where a thread's state is fully known are its **kernel boundaries**: between the guest's `call`
into the kernel thunk and its return, the registers are in `g_cpu`, the guest stack is in guest memory, and the call
(ordinal, guest return address) names where the thread resumes. What remains in host frames is the chain of lifted C
calls that led there and each frame's `ebp`.

Around the threads, state that also lives on the host side:

| Component | Host-side state | Plan |
|---|---|---|
| Kernel objects | Events, semaphores, mutants as host Win32 objects (win32_compat on POSIX) behind guest handle tokens; KE shadows; guest-built events (`ke_guest_event`); KQUEUE / I/O completion bookkeeping (`kernel_ksync.c`); timers and the DPC queue (`kernel_timer_thread`); APC queues | Readable / recreatable objects (XBA-246) |
| Files | Host fds, offsets, the title's path for each handle (`kernel_file.c`, `kernel_path.c`) | Reopen by path + offset (XBA-246) |
| Memory manager | `xbox_HeapAlloc` cursor and free lists, contiguous page map, `guest_vmem` page tracker | Serialise (XBA-245) |
| NV2A | Register aperture (16 MB at 0xFD000000, host memory), pushbuffer walker's GET, executor state, GL render targets not yet in guest memory, texture / shader caches | Registers and walker serialised, targets read back, caches dropped (XBA-248) |
| APU | MCPX aperture (APU registers, VP, GP/EP memories), VP host state (`apu_vp.c`), mixer, host audio ring | Serialised; ring cleared (XBA-248) |
| USB | OHCI registers, device models | Serialised; pads re-attach (XBA-248) |
| Guest CPU lock, IRQL, one-CPU gate | `kernel_guest_cpu.c`, `kernel_hal.c` | Not saved: a state is only taken with every thread at PASSIVE_LEVEL |

## Options studied

### (a) Safe-point snapshot

Freeze every guest thread at a kernel boundary, serialise guest memory, registers, kernel objects and devices, and on
load recreate threads that re-enter their waits. What breaks, and the answer to each:

| Problem | Answer |
|---|---|
| A thread in the middle of lifted code | The freeze holds each thread at its next kernel call; a thread that never makes one defeats the freeze (measured below) and the title is outside the class |
| The host stack below a parked thread | In-session: it is still there, and equivalent when the thread is parked at the same chain (same call sites, same esp at each frame). Across runs: rebuilt by resume entries |
| `ebp` in host frames | The lifter passes it through a per-thread shadow stack across calls (XBA-247), so it is in serialisable memory |
| A thread parked inside a guest routine a bridge runs (an APC inside an alertable wait) | Not a safe point: a bridge's host frame sits between its guest frames. The detector only counts the thread's top-level kernel calls |
| A wait that already returned (the semaphore count is consumed) when the freeze holds the thread | The park record keeps the call's status; a restore returns it instead of waiting again |
| Handles | Guest handle values are restored as they were; the objects behind them are recreated (XBA-246) |
| DPCs and ISRs (timer thread, device models) | Host threads that run guest callbacks briefly; quiesced by holding the one-CPU gate during the capture |
| Time | Time-outs and timers are stored relative to the guest clock, which continues from its saved value |
| GPU and audio on the host | Rebuilt from guest memory and the device models' registers; render targets read back first |

### (b) Process-level snapshot

| Variant | Why not |
|---|---|
| `fork()` | Only the calling thread exists in the child: every other guest thread, the timer thread, the GPU and audio threads are gone. On Android, forking the app process (ART) is unsupported |
| CRIU / DMTCP | CRIU needs root or CAP_CHECKPOINT_RESTORE and ptrace of the target: impossible for an Android app (SELinux, no root). On Linux desktop it works only without a GPU context (EGL / GL state lives in the driver and the kernel), without an open audio device, and restores into the same binary and libraries. The Android app also has the JVM in the process |
| A self-checkpoint of the whole address space | Same problems: the host heap is full of pointers into driver-owned objects (GL, AAudio, file descriptors). Not restorable |

Rejected for every host.

### (c) Hybrid: safe points + chain matching + re-entry (chosen)

(a) for the state, with two ways to have the host stacks right:

- **In place** (same process): a load is accepted only when every guest thread is parked at the same chain
  signature as in the state. Rewind fits this almost always (captures a few seconds apart, the same threads in the
  same loops). A quick load within a level usually fits too: a title's main thread presents from the same main loop
  whatever the level, its audio thread waits in the same worker loop. Otherwise the load is refused with a reason.
- **Re-entry** (fresh process): resume entries in lifted code rebuild each chain. Needed for slots that survive the
  process and for « Resume where I left off ».

### (d) The title's own save points

« Resume » at the title's own saves needs nothing: XAPI saves already persist per title (`saves.md`). It is not a
save state (no mid-level resume, no rewind) and is what players have today.

## Measurements

The prototype (below) on a game-shaped workload, headless on Linux x86-64, through the real kernel bridge and the
synthetic XBE `tools/conformance/test.xbe` (`tests/kernel_park`). We have no runnable nxdk title in the repository,
so the threads are host routines run as guest threads: a « render » thread working 4 ms and sleeping 12 ms
(KeDelayExecutionThread), an « audio » thread working 0.3 ms every 5 ms, a « loader » sleeping 50 ms then working
8 ms without a kernel call.

| Measure | Lock off | Guest CPU lock on |
|---|---|---|
| Time all three threads parked at once | 61 % of 2 s | 58 % |
| All-parked windows | about 370 in 2 s (mean 3.3 ms, longest 5-6 ms) | about 345 |
| Freeze (20 at odd moments), time to reach every thread | 1.3 ms mean, 6.5 ms worst | 1.3 ms mean, 9.3 ms worst |
| A thread spinning without kernel calls | freeze times out (100 ms), names that thread | same |
| Guest window | 16,384 pages (64 MB): 396 resident (1.5 MB), 13 not zero | same |

What it says: with threads that make kernel calls, a freeze is reached within a frame and costs nothing while it is
not requested; the natural all-parked windows are frequent, but a freeze does not need them (it holds threads at
their next call). The guest RAM figure is the synthetic XBE's and is meaningless for a title: a real title allocates
most of its 64 MB, so a state carries up to 64 MB of RAM before compression (128 MB for the titles that ask for it).
Real titles' figures are XBA-252: the periodic kernel summary now prints them.

## The prototype: the park detector (`src/kernel/kernel_park.c`)

Always on; one atomic add when a thread changes between parked and busy, a few thread-local updates per kernel call.

| Hook | Where |
|---|---|
| `xbox_ParkThreadStart / End` | Around a guest thread's routine: `bridge_run_thread_inline` (workers) and the INLINE main thread in `bridge_PsCreateSystemThreadEx` |
| `xbox_ParkKernelEnter / Leave` | `kernel_thunk_dispatch`, around every bridge; Leave after the guest CPU is taken back, so a thread queued for the guest CPU is still parked |
| `xbox_ParkCallbackEnter / Leave` | `xbox_GuestCpuEnterCallback / LeaveCallback` (guest code a bridge runs) |
| Depth snapshot / restore | Carried in `xbox_SehRuntimeSnapshot / Restore`, so an SEH resume or a guest longjmp over bridge frames leaves the counts right |

A thread is **parked** when it is exactly one kernel call deeper than where its routine started and in no further
callback. `xbox_ParkFreeze(timeout, &stats)` sets a flag and waits until no registered thread is busy; a thread
going busy adds itself first and then looks at the flag (both sequentially consistent), so once the freezer has seen
none busy, none can run guest code until `xbox_ParkThaw`. The stats name each thread's state (running, in a call,
waiting, held), its ordinal and guest return address, whether it is held at DISPATCH_LEVEL, and its kernel call
count. `xbox_ParkGuestPages` counts resident and non-zero guest pages (mincore on POSIX).

The periodic kernel summary (every 2 s) prints one line:

```
[KERNEL] park: 3 guest threads, all parked 61.0% of 2000 ms (376 windows, longest so far 5.9 ms); now 0 running, 3 waiting
```

Not covered by the prototype: the timer thread's DPCs and the device models' ISRs (quiesced by the one-CPU gate in
the capture, XBA-245), the native-call thunk-table path (`kernel_thunks.c`), and host waits made from inside lifted
code (`xbox_GuestPollWait`, NV2A kick waits): a thread blocked there counts as busy (XBA-251).

## Data format

A state is a slot file of `recomp_states` (`XRSLOT` header, console `xbox`, game key = title ID, thumbnail), whose
blob is:

```
"XBXS" u32 version
identity: title ID, XBE SHA-1, build ID of the generated code (hash of the lifted sources), RAM size
sections, each: u32 tag, u32 version, u64 length, payload
  RAM    page bitmap (non-zero pages), then the pages (compressed; rewind codes XOR deltas, recomp_rewind.h)
  CPU    per guest thread: guest thread id, KTHREAD VA, TIB VA, guest stack bounds, RecompCpu, ebp shadow stack,
         park record (ordinal, guest return address, status when the call already returned), chain signature
         (return VA and esp per frame, start routine)
  MEM    heap allocator, contiguous page map, guest_vmem
  KOBJ   kernel objects and the handle table, guest clock, timer and DPC queues, APC queues
  FILE   open files and directories (title path, host path, offset, access)
  NV2A   register file, pushbuffer GET / PUT, DMA objects, executor state (render targets already read back to RAM)
  APU    MCPX aperture, VP state
  USB    OHCI registers, device models
```

A state loads only into the binary that saved it (the build ID): call-site addresses and the lifted functions'
frames are part of the chain signatures. A rebuilt title (a new prepare) cannot load older states; the menu says so
(`RECOMP_STATE_EVERSION`).

Sizes: RAM dominates (up to 64 MB raw, zero pages skipped); everything else is tens of KB. Capture cost on a phone:
a 64 MB copy takes about 5-10 ms; for rewind, pages are compared with the previous capture and only changed pages kept.

## Device sections (XBA-248)

The NV2A, APU and USB models serialise themselves into byte buffers; the state container (XBA-245) only frames
and stores them. Interface: `src/kernel/xbox_dev_state.h` (header-only buffer, run-length coding of 32-bit words,
section framing `u32 tag, u32 version, u64 length, payload`, the section table `xbox_DevStateSections`).

With every guest thread frozen (`xbox_ParkFreeze`):

1. `xbox_DevStateFreeze()` holds the device threads between two passes. The NV2A poll thread finishes its walk,
   resolves pending visibility reports, has the GPU back end read every render target back into guest memory
   (back-end hook `state(NV2A_BACKEND_STATE_FLUSH)`, run on that thread, where the GL context is current), then
   waits. The APU frame thread idles (`state_hold`, whatever plays). The OHCI controller thread stands still.
2. Capture (RAM first, then each section's `capture`) or restore (RAM first, then each `restore`).
3. `xbox_DevStateThaw(loaded)`. After a load the NV2A poll thread drops every back-end cache
   (`NV2A_BACKEND_STATE_DROP`: targets, depth, textures, programs, vertex buffers rebuild from guest memory) and
   takes PUT / GET from the restored registers; the flip hold, the frame cap's schedule and the pacing alignment
   start again; `recomp_states_note_load` resets frame generation's history.

| Section | Saved | Rebuilt or cleared |
|---|---|---|
| NV2A | The 16 MB register aperture (PGRAPH, PFIFO, PCRTC, PVIDEO, RAMIN with the DMA objects), run-length coded; the DMA walker's GET, CALL return and cut-off method; the executor's decoded state (surface, vertex attributes, texture stages, combiners, the Kelvin register file, the semaphore target); the vertex program memory, constants and load cursors; the CPU rasteriser's host depth buffers | GPU objects (dropped, re-created from guest memory; programs reload from the disk cache); visibility queries (resolved before the capture); depth held only by the GPU (a frame after a load starts with it cleared) |
| APU | The APU, GP and EP register files; the sequencer's sample count; per voice the resampler's samples and position, the filters, the HRTF history and coefficient ramps, each bin's last gain; SSLs, HRTF table, submix headrooms, title-held voice locks; the GP / EP DSP memories (MCPX aperture 0x30000-0x7FFFF); the doorbell ack's state; the software mixer's play positions | The device queue (`xa2_clear`), the monitor buffer (a fresh EP period), the pacing clock, the reverb's delay lines |
| USB | Both OHCI register files with the read counts the frame number derives from; each pad's control pipe, address, configuration and rumble; plugged pads, hotplug bookkeeping, addresses of pads pulled out, a list kick not walked yet | The pads re-attach to the host's current pads at the controller's next hotplug decision; the rumble is re-applied to whatever pad is there |

Sizes on the synthetic XBE: NV2A, APU (absent) and USB together 6.6 KB, captured in 3 ms (most of it the scan of
the 16 MB aperture); the APU model with one voice playing 50 KB (the per-voice block, run-length coded).

Tests: `tests/nv2a_ffp_backend` (`nv2a-state`: the executor's state captured, overwritten, restored, and a clear
and two batches reach the back end exactly as without the round trip), `tests/nv2a_gl_backend` (case 9: a target
drawn only on the GPU, flushed, captured, restored and dropped, presents the same pixels; without the flush it
does not), `tests/apu_state` (a pitched, fading voice mixed from a restored model equals the uninterrupted run
sample for sample; RAM alone does not), `tests/apu_lifecycle` (the section of a live instance with the frame
thread held), `tests/usb_ohci_posix` (`usb_ohci_posix_state`: an enumerated pad's report and registers after a
reset and a restore; every section through `xbox_DevStateFreeze` / `CaptureAll` / `RestoreAll` / `Thaw` with
the real poll thread).

Not covered: the AC'97 page and the NIC (a state taken during a network session resumes with the link down), the
HLE DirectSound layer's host buffers (`src/audio`: restored only within the run that owns them), the legacy
xemu-derived `src/nv2a` model (Windows MMIO path, not run by recompiled titles).

## Limits

- **Titles outside the class** (above) never offer states. The status file and the app name the reason: fibers or
  stack-switch contexts, a thread that never parks (with its last call), the native-call path.
- **In-session loads refuse when a chain differs** (a thread at another call site, created or ended since the
  state). Across-runs states (resume entries) lift this.
- **A frozen thread at DISPATCH_LEVEL** (holding the one-CPU gate) makes the capture wait for another freeze.
- **States are tied to one build** of the generated code.
- **Network** (the NIC model, system link) is not restored: a state taken during a network session resumes with the
  link down.
- **Cost**: the park hooks are always on and measured negligible; the lifter's ebp shadow stack (XBA-247) and resume
  entries (XBA-250) have budgets in their tickets (2 % frame time).

## Tickets

| Ticket | What | Needs |
|---|---|---|
| XBA-244 | This design, the park detector and its measurements | |
| XBA-245 | Guest state capture (RAM, registers, memory manager) and the state format | |
| XBA-246 | Kernel objects with readable, restorable state | |
| XBA-247 | Lifter: ebp shadow stack across calls, call-site unwind table, chain signatures | |
| XBA-248 | NV2A, APU and USB capture and restore | |
| XBA-249 | In-session restore: rewind, quick save / load within a run, attach to `recomp_states` | 245, 246, 247, 248 |
| XBA-250 | Resume entries in lifted code: states across runs, « Resume where I left off » | 247, 249 |
| XBA-251 | Park points at host waits inside lifted code (guest poll waits, NV2A kick waits) | |
| XBA-252 | Park statistics on our titles (Linux arm64, device) | |
