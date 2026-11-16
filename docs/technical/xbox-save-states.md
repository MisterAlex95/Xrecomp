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
  switches (XBA-101: Fable is out); no thread spinning on guest memory without kernel calls, except in a poll loop
  the lifter marks or behind a back edge lifted with `--back-edge-yield` (XBA-251, below); built through the
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
| `xbox_ParkHostWaitEnter / Leave` | The runtime helpers lifted code calls that wait on the host (XBA-251, below): `xbox_GuestPollWait`, `xbox_Nv2aKickWait`, `xbox_Halt`, `xbox_GuestCpuPreempt` |

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
the capture, XBA-245), the native-call thunk-table path (`kernel_thunks.c`), and a spin loop the lifter did not mark
(anything but `test [mem], imm; jcc itself`, without `--back-edge-yield`): a thread there counts as busy.

### Host waits inside lifted code (XBA-251)

A thread can also wait in the middle of a lifted function, with no kernel call, in a runtime helper the lifter emits
a call to. Every such helper (the lifter and `recomp_types.h` were searched for calls that block) is a park point:

| Helper | Emitted for | Site kind | Blocks |
|---|---|---|---|
| `xbox_GuestPollWait(word, mask, while_set, va)` | `RECOMP_POLL_WAIT`, the back edge of a loop that only polls one word (`_mark_poll_loops`) | `XBOX_PARK_SITE_POLL` (returns at once) or `_KICK` (the PFB flush word: waits for the NV2A ack thread) | Kick only |
| `xbox_Nv2aKickWait(reg, mask)` | `xbox_GuestPollWait` on the PFB flush word, or a title override | `XBOX_PARK_SITE_KICK` (site VA 0 from an override) | Yes |
| `xbox_Halt(va)` | `hlt` | `XBOX_PARK_SITE_HALT` | 1 ms |
| `xbox_GuestCpuPreempt(va)` | `RECOMP_BACK_EDGE` with the preempt flag set (`--back-edge-yield`, guest CPU lock on) | `XBOX_PARK_SITE_PREEMPT` | Until the guest CPU is free |

The others the lifter emits (`recomp_lookup`, port I/O, cpuid, control registers, SEH anchors, the stack switch
helpers, `xbox_GuestInterruptFlag`) do not wait on another thread, or only for a lock held for a moment.

Each is entered like a top-level kernel call (one kernel-call level; nested ones, the kick wait inside the poll
wait, count once; one inside a kernel call leaves the call the park point): Enter records the registers (`g_cpu`,
as at the site) with the site, the helper runs, Leave records them again as `returned` and blocks while a freeze
holds the thread. No helper changes guest state a restore would have to replay: calling it again is harmless (the
poll loop tests its word again, the kick sets a bit already set, a hlt or a preempt just waits). So a record means
« in host wait K at VA » (re-issue the helper) or « returned from host wait K at VA » (resume in the lifted code right
after the call). `xbox_GuestPollWait` parks even when it returns at once: a thread spinning in a marked poll loop is
held at its next turn, which takes it out of « a thread that never parks ».

The `ebp` caveat is the same as for kernel calls: inside lifted code `ebp` is a C local, so the recorded `r_ebp` is
not the thread's until XBA-247. The site's VA is the poll loop's `jcc`, the `hlt`, or the back edge's source; the
chain signatures and the call-site unwind table (XBA-247, XBA-250) need these sites as well as the kernel calls.

The park line names one:

```
[KERNEL] park: 6 guest threads, all parked 69.6% of 1250 ms (...); now 0 running, 0 waiting, 3 at a host wait (poll loop at 0x0001A100)
```

A thread spinning in a poll loop moves between parked and busy at every turn, so the all-parked window count grows
with it (thousands per second in the test): the share stays meaningful, the window count and mean do not.

Tests: `tests/kernel_park` (« Host waits »: a thread in a marked poll loop on a guest word, one kicking the PFB flush
through `xbox_GuestPollWait`, one halting, the loops' back edges calling `xbox_GuestCpuPreempt` as lifted code with
`--back-edge-yield` does; 20 freezes reach all of them with their sites, nothing runs while frozen, they run again
after the thaw; the hooks' nesting) and `tests/xbox_state` (« Host waits »: a capture with a poll thread and a kick
thread writes them as host-wait records, the reader refuses an unknown site or a host wait with an ordinal), both
also with the guest CPU lock on (there the kick thread is often held at its preempt).

## Kernel objects (`src/kernel/kernel_kobj_state.c`, XBA-246)

`xbox_KobjSave` / `xbox_KobjLoad` serialise the kernel's host side into one self-describing blob ("XKOB", a
version, then records `u16 type, u16 version, u32 length, payload`; unknown types are skipped), the KOBJ section of
the container. `xbox_KobjCheck` lists what keeps a save from being taken, each with a reason, so the menu can say
why. `xbox_KobjReset` empties everything, as a fresh kernel has it.

| Object | Read | Made again |
|---|---|---|
| Event, semaphore, mutant, waitable timer | `w32_state_read`: type, signal state, count / limit, owner thread and recursion, remaining time and period, without consuming anything | `w32_state_make` in that state (timer re-armed with the time it had left) |
| Thread | Host thread id, KTHREAD, exited, exit status, suspend count, priority | Ended: an exited thread object (signalled, its status). Running: in session, bound to the same thread still running (by host thread id); across runs, refused (XBA-250) |
| File, directory | Host path, `open` mode, position, pending FATX overwrite; disc image files: path in the image, position, search state | Reopened and positioned (`xbox_FileStateReopen`) |
| I/O completion port | Slot, generation, concurrency counts, queued packets in order | Same slot and generation (threads' queue associations stay valid), packets queued again |
| Handle table | Every token: its object, the guest thread object or `ObCreateObject` body behind it | Same token values; one object shared by several tokens (`NtDuplicateObject`) stays one object |
| KE shadows | Guest KEVENT / KMUTANT / KSEMAPHORE / KTIMER / thread object -> host object, guest-built events | Same |
| Kernel timers, DPC queue | Timer, DPC, time left on the guest clock, period; queued DPCs in order | Re-armed relative to the guest clock; queue refilled |
| Other tables | Thread alert state, critical-section semaphores, `ObCreateObject` objects, connected interrupts, shutdown and thread notification routines, the APC pool, asynchronous file handles, inserted KAPC count | Same |

The guest structures (KEVENT headers, KTHREAD, APC lists, KQUEUE, device queues) are guest RAM and come with the RAM
section. What refuses a save: a set on its way to a blocked thread (retry), a host directory search in progress, a
host object kind we do not carry (file mappings), and, for another run, a running thread, an owned mutant or a held
host critical section. A held host critical section in session is recorded and must be held by the same thread, as
deep, when the state loads. A load refuses, changing nothing, while a thread is blocked on an event it would tear
down: the same-run load (XBA-249) waits until every thread is back from its kernel call first. POSIX hosts only: on Windows the objects are
real Win32 objects and the save is refused. Test: `tests/kernel_kobj_state`.

## Data format (XBA-245: `src/kernel/xbox_state.h`)

A state is a slot file of `recomp_states` (`XRSLOT` header, console `xbox`, game key = the title ID as `%08X`,
thumbnail), whose blob is an **XBXS container of tagged sections**. Little-endian throughout (every host we build
for is). POSIX hosts (Linux, macOS, Android): `recomp_states` is not built for Windows.

```
header (128 bytes)
  0    "XBXS"
  4    u32 format version (XBOX_STATE_VERSION, 1)
  8    u32 header size (128)
  12   u32 section count
  16   u32 title ID (certificate)
  20   u8[20] SHA-1 of the XBE's headers (base to SizeOfHeaders: they carry every section's own SHA-1)
  40   u8 build ID length, 44 u8[32] build ID of the binary (ELF NT_GNU_BUILD_ID of the module the runtime is
       linked into, the executable or the app's libmain.so; Mach-O LC_UUID; xbox_StateSetBuildId overrides)
  76   char[16] toolkit version ("0.14.0", from CMake)
  92   u32 RAM size, 96 u32 mapped size, 100 u32 contiguous window size, 104 u32 sizeof(RecompCpu)
  108  u64 Unix time of the capture (ms)
  120  u64 hash of bytes 0..119
sections, in registration order, each:
  u32 tag, u32 version, u64 length, u64 hash of the payload (xbox_StateHash, 64-bit, four lanes), payload
"XBXS" u32 version
identity: title ID, XBE SHA-1, build ID of the generated code (hash of the lifted sources), RAM size
sections, each: u32 tag, u32 version, u64 length, payload
  RAM    page bitmap (non-zero pages), then the pages (compressed; rewind codes XOR deltas, recomp_rewind.h)
  CPU    per guest thread: guest thread id, KTHREAD VA, TIB VA, guest stack bounds, RecompCpu, ebp shadow stack,
         park record (ordinal, guest return address, status when the call already returned), chain signature
         (return VA and esp per frame, start routine)
  MEM    heap allocator, contiguous page map, guest_vmem
  KOBJ   kernel objects and the handle table, open files and directories, guest clock, timer and DPC queues
         (kernel_kobj_state.h; files are in it because handles name them like any other object)
  NV2A   register file, pushbuffer GET / PUT, DMA objects, executor state (render targets already read back to RAM)
  APU    MCPX aperture, VP state
  USB    OHCI registers, device models
```

The sections this ticket writes (built in, always first; since XBA-249 `KOBJ`, `NV2A`, `APU ` and `USB ` follow them,
built in too, so a state has seven sections):

| Tag | Version | Payload |
|---|---|---|
| `CPU ` | 3 | u32 thread count, u32 sizeof(RecompCpu), u64 run id (XBA-249); per guest thread a 68-byte head (park slot, park state, ordinal, guest return address, returned, status, dispatch, host tid, u64 kernel calls, TIB, KTHREAD, guest stack base and limit from the KTHREAD, esp at the park point (the call's return address slot, XBA-249), guest thread id, park site) then the raw RecompCpu |
| `MEM ` | 1 | `xbox_MemStateSave`: heap cursor, alloc count and block table, worker stack count; the contiguous window's counters, page map, block lengths (sparse) and RAM-name map; the lazy reservations; guest_vmem's page table as runs |
| `RAM ` | 1 | u32 range count; per range u32 kind, u32 guest base, u32 pages, a bitmap of the pages stored, then those pages (4 KB each) |

Not in the design's first draft: the ebp shadow stack and the chain signature in `CPU ` (XBA-247 adds them and bumps
the section's version), compression.

**Park records, two kinds** (`CPU ` v2, XBA-251; v3 since XBA-249 adds the run id and the esp at the park point;
older versions are refused, `EVERSION`). The head's last word,
the park site (`XboxParkRecord.site`, `XboxStateThread.site`, `XBOX_PARK_SITE_*` in `kernel_park.h`), says which:

| Site | Kind | ordinal | guest return address | Resume |
|---|---|---|---|---|
| 0 `KERNEL` | Top-level kernel call | The call (never 0) | The call's return address (at esp while in the call) | Not returned: re-issue the call. Returned: eax the status, continue at the return address |
| 1 `POLL`, 2 `KICK`, 3 `HALT`, 4 `PREEMPT` | Host wait inside lifted code (above) | 0 | The site's guest VA (0: a title override's kick, no site) | Not returned: call the helper again. Returned: continue in the lifted code after the call (the status word is meaningless) |

`xbox_StateCpuThreads` refuses a site of 5 or more and a host-wait record with an ordinal. The record API is
unchanged otherwise (the field is appended to both structs): a load (XBA-249) that only knows kernel calls tells a
host wait by `site != XBOX_PARK_SITE_KERNEL` (or ordinal 0) and, in session, treats it like a kernel-call park at the
same chain; across runs (XBA-250) the resume entry is the helper's call site.

**Ranges** (`xbox_StateGuestRanges`): the RAM view `[0, mapped size)`, the contiguous window up to its high-water
mark (its own storage, not RAM: the KTHREADs of spawned threads live there), the lazy reservations (POSIX,
`[cursor, top)` above the RAM view) and the pages `RECOMP_EXT_VMA` has committed. The mirrors and the tiled alias are
views of these and are left out. All-zero pages are not stored (the bitmap says so).

**No compressor**: there is none in the tree (the console runtimes skip zero pages the same way, `n64_state.c`), and
a real title's state is dominated by non-zero RAM. Rewind (XBA-249) codes XOR deltas on top (`recomp_rewind.h`); the
capture already hashes every page and counts the pages changed since the previous capture (the `[STATE]` line).

### Adding a section (XBA-246, XBA-248)

```c
static int kobj_save(XboxStateWriter *w, const XboxStateContext *ctx, void *user);   /* every guest thread frozen */
static int kobj_check(const uint8_t *p, size_t n, uint32_t version, void *user);     /* validate, restore nothing */
static int kobj_load(const uint8_t *p, size_t n, uint32_t version, void *user);      /* XBA-249: put it back */
static const XboxStateSection kobj = { XBOX_STATE_TAG_KOBJ, 1, "kobj", kobj_save, kobj_check, NULL, kobj_load };
xbox_StateRegister(&kobj);   /* once, from the component's init, before the first capture */
```

- The tags are reserved in `xbox_state.h`: `KOBJ`, `FILE` (XBA-246), `NV2A`, `APU `, `USB ` (XBA-248). A section is
  written after the ones registered before it; `xbox_StateRegister` with the same tag replaces it.
- `save` writes with `xbox_StatePut32 / 64 / Put` or `xbox_StateReserve` (keep offsets, not pointers: the buffer
  grows). It runs with every guest thread frozen and the dispatch gate held when it is on; it may run more than once
  per capture (below), so it must not change state. `ctx` gives the frozen threads (`xbox_StateContextThread`: park
  record, registers, KTHREAD, guest stack).
- `check` parses its payload with `XboxStateReader` / `xbox_StateGet32 ...` and returns `XBOX_STATE_OK`,
  `_ECORRUPT` or `_EVERSION`. The container has already checked the section's length and hash.
- `load` (XBA-249) puts a checked payload back, every guest thread frozen and the device threads held; on failure it
  returns an error with a reason (`xbox_StateSectionReason`) and the load puts the state of before back.
- Bump the section's version when its payload changes.

### Reading

`xbox_StateCheck` (and `xbox_StateReadSlot`, which reads the slot file first) checks, in order: magic and format
version; the header hash; the identity against the running title (another title or XBE: `EGAME`; another build ID,
toolkit, memory size or RecompCpu size: `EVERSION`); every section's bounds and hash (`ECORRUPT`), that this build
registers it with the same version (`EVERSION`: a state from a build with other sections), its check callback; that
every registered section is present; no trailing bytes. Nothing is restored (`xbox_StateLoad` does, below).
`xbox_StateParseHeader` reads
the header alone, for a menu listing slots of any title.

A state loads only into the binary that saved it (the build ID): call-site addresses and the lifted functions'
frames are part of the chain signatures. A rebuilt title (a new prepare) cannot load older states; the menu says so
(`RECOMP_STATE_EVERSION`).

## The capture (XBA-245)

`xbox_StateCapture(timeout, &blob, &n, &stats)` from any host thread that is not a guest thread:

1. **Freeze** (`xbox_ParkFreeze`): every guest thread at its next kernel boundary.
2. **Settle**: a thread frozen inside a non-blocking call is still running that call's bridge; wait (50 ms at most)
   until it has returned and is held at the call's end.
3. **Refuse** what a state cannot hold: a thread held at DISPATCH_LEVEL, threads the park detector could not track
   (more than 64), the dispatch gate held by someone else (50 ms). Then take the dispatch gate (when
   `RECOMP_DISPATCH_GATE` is on), so no ISR or DPC runs guest code meanwhile.
4. **Capture**, then check the park records again. Each guest thread keeps a record of its current top-level kernel
   call (`kernel_park.h`): Enter writes the registers as the guest made the call (esp at the return address), Leave
   rewrites them as the call left them (eax the status, esp past the arguments) and marks it returned. A wait that
   completes during the capture rewrites its record (and maybe guest memory), so the capture is taken again; that
   thread is held now, so at most one attempt per thread: threads + 3 attempts at most.
5. Thaw.

A frozen thread is therefore recorded either « in call N from R, registers at the call » (a restore re-issues the
call, overwriting its partial effects on its outputs) or « returned from N with status S, registers as the call left
them » (it resumes at R). In the measurements below most threads are « returned »: their sleeps end during the freeze.

Not quiesced: with the dispatch gate off (the default), the timer thread still updates the guest clock words in the
kernel data page during a capture, and device models' host threads may write guest memory. The clock is XBA-246's
(`KOBJ`), the devices XBA-248's; the dirty-page test counts one such page between two captures in one freeze.

`RECOMP_STATE_PROBE=<seconds>` (diagnostics) captures every that many seconds on a host thread and logs the line
below; with `RECOMP_STATE_PROBE_DIR=<dir>` it also writes `<dir>/probe.st` and reads it back:

```
[STATE] capture: 3 guest threads, RAM 15 of 16386 pages stored (0.1 MB, 3 changed since the last capture) in 2 ranges, blob 0.09 MB; 5.19 ms (freeze 0.30, settle 0.00, sections 4.89, 1 attempt)
[STATE] probe: /tmp/x/probe.st written in 2.5 ms (ok), read back and checked in 0.1 ms: ok
```

### Measurements

`tests/xbox_state` on Linux x86-64 (Docker `xba-deps`), the synthetic XBE and the three game-shaped threads of
`tests/kernel_park`:

| Measure | Lock and gate off | Guest CPU lock and dispatch gate on |
|---|---|---|
| Capture, 10 at odd moments (freeze included) | 7.2 ms mean, 9.6 ms worst | 5.7 ms mean, 11.6 ms worst |
| First capture of the run (page-hash tables allocated) | 30-43 ms, 2 attempts | 30 ms, 2 attempts |
| Blob | 0.09 MB: CPU 1.2 KB, MEM 33 KB, RAM 62 KB (15 of 16,386 pages) | same |
| Slot file write / read and check | 2.5 ms / 0.1 ms | |

The time is the scan of the 64 MB window (16,384 pages read to find the non-zero ones), whatever the title holds;
copying and hashing the stored pages adds to it with a real title. **A real title on Linux: not measured yet** (the
agents have no title; `RECOMP_STATE_PROBE=5 RECOMP_STATE_PROBE_DIR=/tmp` on one gives the figures, to be added here).
Expected from the sizes: most of 64 MB non-zero, so a blob of 40-64 MB and a capture of a few tens of ms.

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

## Same-run load (XBA-249)

`xbox_StateLoad(blob, n, timeout, &stats)` (and `xbox_StateLoadSlot`), from any host thread that is not a guest
thread, one capture or load at a time:

1. **Check** the blob (`xbox_StateCheck`: title, build, sections) and that it was taken **in this run** (the `CPU `
   section's run id, random per process): a state from an earlier run refuses (`ECHAIN`, « saved in another run »)
   until resume entries (XBA-250).
2. **Freeze** every guest thread (`xbox_ParkFreeze`), then **settle**: each thread in a kernel call must be back from
   it (its bridge returned: held at the call's end, or queued for the guest CPU behind a frozen thread,
   `xbox_ParkKernelReturned`). A wait in progress is given the timeout (500 ms from the menu) to end: sleeps and
   vblank waits do; a wait only another guest thread can end does not, and the load refuses naming it. A thread at a
   host wait inside lifted code (XBA-251) may still be waiting: the helper touches nothing a load changes. A thread
   created since the state, or one that ended since, refuses at once.
3. **Where each thread stands** (`chain_matches` in `xbox_state.c`): the same park point as in the state, the same
   kernel call (ordinal, guest return address) or a host wait at the same guest VA (a poll loop's back edge is also
   where the guest CPU lock preempts: either kind matches), with esp at the same place when it was entered. The host
   frames below the thread are then assumed to be the state's: a title's threads loop through the same chains. This
   is the hook XBA-247 replaces with the chain signature (every frame's return address and esp, the ebp shadow stack);
   until then a thread that reaches the same call through another chain with the same esp would load wrongly (not
   seen in the test titles' shape; a known limit). Any difference: `ECHAIN` (`RECOMP_STATE_EREFUSED` for the menu)
   with the reason, nothing changed.
4. **Pre-checks**: every saved guest page is mapped now; the memory manager's `RECOMP_EXT_VMA` page table is what it
   was (commits are not redone yet).
5. **Backup**: a capture of now (the same freeze), put back if a section fails part way: never a partial load.
6. With the dispatch gate and the device threads held (`xbox_DevStateFreeze`): **MEM** (`xbox_MemStateLoad`: the heap's
   block table and cursor, thread stack counts, the contiguous window's page map, the lazy reservations), **RAM**
   (every page of every saved range copied or zeroed; the page hashes start again), **KOBJ** (`xbox_KobjLoad`, in
   session: objects made again under their handles, running threads bound by host thread id, the guest clock),
   **NV2A**, **APU**, **USB**, any other registered section; then each thread's **registers**
   (`xbox_ParkRestoreThread`): a « returned » record's registers are what the thread sees when its call returns; an
   « in the call » record makes the call again from the registers the guest made it with (the restored guest stack
   holds its arguments): `kernel_thunk_dispatch` loops on `xbox_ParkTakeReissue`, a host-wait helper on
   `xbox_ParkHostWaitLeave`'s answer.
7. `xbox_DevStateThaw(1)` (GPU caches dropped, frame generation's history restarted), thaw.

One line per load:

```
[STATE] load: 4 guest threads back at their calls (2 made again), 5 RAM pages written; 20.32 ms (freeze 0.00, settle 4.79, backup 6.51, sections 9.03)
[STATE] load refused: the game is not at a point this state can be loaded at (guest thread 1008 is in kernel call 99 from 0x00011068, in call 99 from 0x00011060 in the state)
```

The capture holds the device threads too since XBA-249 (the NV2A has read its render targets back into guest memory
before RAM is written), and a device thread that is not running answers at once (`xbox_Nv2aPollHold` -1,
`xbox_OhciStateHold` 0) instead of after 2 s.

### The front end (`src/kernel/xbox_state_service.c`)

`xbox_kernel_bridge_init` starts the service: once the title's paths are set it attaches to `recomp_states` as console
`xbox`, slots in `<save dir>/xbox-<TITLE ID>/state<N>.st` (no thumbnail yet), so the Android in-game menu's Save state
/ Load state, the quick save / load pad chords (Back + RB / Back + LB by default), the slot hotkeys and the rewind
hold reach the Xbox capture and load like a console's. The pad's hotkeys come from the OHCI controller thread, which
a capture holds, so they run on the service's own thread. A game held by the menu or the background is let go for
the length of the capture or load (threads waiting on the GPU behind a parked flip never reach a kernel call
otherwise). `xbox_StateAvailable(why)` says whether states can be offered now (`MenuNative.nativeStatesAvailable`,
`nativeStatesReason`): attached with a save folder, guest threads running, no stack switching (fibers), no untracked
threads. The app shows Save state / Load state for an Xbox title when it is true, but no « Resume where I left off »
and no automatic slot (states do not survive the run). A refused load shows « The game is not at a point where this
state can be loaded »; a capture not possible now « A state cannot be saved at this point of the game ».

**Rewind**: the service captures every 500 ms (`RECOMP_REWIND_MS`; at least 20 times the last capture's cost, so it
never takes more than 5 % of the time) into `recomp_rewind`'s XOR-delta ring of XBXS blobs, from the first pad report
on, while the game is not held; holding the rewind chord (Back + LT) loads the newest capture every 125 ms and drops
it. It turns itself off, saying why, when a blob does not fit half the ring (`RECOMP_REWIND_MB`, default 64) or three
captures in a row fail (a capture's freeze waits 20 ms at most, so a thread that never parks costs that, three
times). `RECOMP_REWIND=0`: off. A load from a slot clears the ring.

### Measurements

`tests/xbox_state` (« Same-run load »), Linux x86-64 in Docker `xba-deps`, the synthetic XBE and four game-shaped
threads:

| Measure | Lock and gate off | Guest CPU lock and dispatch gate on |
|---|---|---|
| Load (capture then load, 10 in a row) | 30-32 ms mean, 40 ms worst | 35 ms mean, 45 ms worst |
| of which: settle (waits ending) / backup capture / sections | 5-50 / 6.5 / 7-9 ms | 40 / 6.4 / 7-9 ms |
| Rewind capture / delta coding | 12 ms / 0.02 ms | 9.6 ms / 0.02 ms |
| Ring for 8 captures (blob 0.12 MB) | 0.26 MB | 0.26 MB |
| A step back (load) | 30-60 ms | 30-43 ms |

The settle time is the threads' own sleeps ending (the loader sleeps 50 ms). On the synthetic XBE rewind costs
little, so it is on; a real title's blob is 40-64 MB and its capture tens of ms (XBA-252 measures): the adaptive
interval keeps it under 5 % and the budget check turns it off rather than hold a ring of one capture.

Tests: `tests/xbox_state` « Same-run load »: a state captured, then guest RAM, a kernel object (an event created,
another closed), the heap and an NV2A register changed; the load puts all back, and every thread returns from its
call with the state's registers; refused, with nothing changed: a thread parked at another call site, a thread
created since, one ended since, a state of another run; ten round trips; rewind's ring and four steps back; the
front end (attached as `xbox`, the menu's slot 2 saved and loaded, a refused load answered `RECOMP_STATE_EREFUSED`,
the quick save chord run on the service's thread); « Host waits »: a state with threads at host waits loads. App:
`GameMenuTest.xboxStateRows`.

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
| XBA-245 | Guest state capture (RAM, registers, memory manager) and the state format: done (`xbox_state.h`, `tests/xbox_state`) | |
| XBA-246 | Kernel objects with readable, restorable state | |
| XBA-247 | Lifter: ebp shadow stack across calls, call-site unwind table, chain signatures | |
| XBA-248 | NV2A, APU and USB capture and restore | |
| XBA-249 | In-session restore: rewind, quick save / load within a run, attach to `recomp_states`: done (`xbox_StateLoad`, `xbox_state_service.c`; the chain check is the innermost park point until XBA-247) | 245, 246, 248 (247: the chain signature) |
| XBA-250 | Resume entries in lifted code: states across runs, « Resume where I left off » | 247, 249 |
| XBA-251 | Park points at host waits inside lifted code (guest poll waits, NV2A kick waits, hlt, preempts): done (`CPU ` v2) | |
| XBA-252 | Park statistics on our titles (Linux arm64, device) | |
