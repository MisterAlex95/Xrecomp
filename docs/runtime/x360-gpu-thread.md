# Xbox 360: the GPU in two halves, and the road to a GPU thread (WP-22)

> Part of the [Documentation Index](../INDEX.md)

Status: **stage 1 done** (the seam, same thread, runs byte-identical). **Stage 2 landed, off** (section 5.0: the run half
on the renderer's thread while the machine runs on a CPU worker; `RECOMP_X360_GPU_THREAD=1` turns it on in the player,
off by default awaiting a device check). Stages 3-4 and the rest of 5.1 below are designs, not code.
Source: `consoles/x360/runtime/x360_gpu.c` (the seam), `xenos_cp.c` (the command processor), `xenos_gl.c` (the
renderer), `x360_state.c` (states), `x360_runtime.c` (the frame loop).

Why: on Adventure Time the guest (with the runtime) costs 9-12 ms per swap and the command processor plus GL 7.2 ms,
one after the other on a single host thread, against a 16.7 ms budget (round-3 performance report). Running the two
on separate threads overlaps them.

## 1. The two halves

| Half | Thread (stage 1 / stage 2+) | Owns |
|---|---|---|
| **post** | guest / guest (the machine's CPU thread) | the register page at 0x7FC80000 (vertical counter, display status, `CP_RB_WPTR` as the title wrote it), `last_wptr`, the vertical blank (`next_vblank`, `vblanks`), the interrupt context (`irq_callback`, `irq_user`, `irq_thread`, `irq_pending`, `irq_cpus`, `raise_irq`), the Vd* calls |
| **run** | guest / GL (the SDL thread) | `xenos_cp` (registers, ring and indirect-buffer positions, `stalled`, shaders, bin mask, counters), the renderer (`xenos_gl`: surfaces, copies, lazy resolves, frame texture), `--dump-shaders` state, `cp_ns` / `cp_cpu_ns` |

`x360_gpu_poll` (called by the scheduler between time slices, `x360_thread.c`) is now:

```
gpu_post(x, g, now);     // registers; post the write pointer when the title moved it, or the run half stalled
x360_gpu_run(x);         // stage 1: the run half, right here
gpu_post_irq(x, g, now); // take the run half's interrupts, vertical blank, raise_irq
```

The order of everything guest-visible is the order before the split, which is why stage 1 changes nothing.

## 2. The seam (`struct gpu_link`, `x360_gpu.c`)

| Item | Direction | Meaning |
|---|---|---|
| `link.post` (u64) | post → run, release / acquire | `(posts made << 32) \| write pointer`. One word, so the run half never pairs a count with another post's pointer. |
| `link.taken` (u32) | run → post, release | the post number the run half has run to; equal to `post >> 32` when idle. `x360_gpu_drain` waits on it (stage 2). |
| `link.stalled` (u32) | run → post | the command processor stopped at an unmet `WAIT_REG_MEM`; the post side posts the same pointer again at its next poll (before the split: `\|\| g->cp.stalled`). |
| `link.irq` (u32) | run → post, `fetch_or` / `exchange` | `PM4_INTERRUPT`: `LINK_IRQ_SRC1` and one bit per CPU of its mask (`LINK_IRQ_SRC0` reserved). `gpu_take_irq` turns it into `irq_pending` / `irq_cpus`, which `raise_irq` consumes. The run half never creates or restarts a guest thread. |
| guest memory | run → guest | word stores through `phys_write` (`cp.write32`) and the renderer's direct resolve stores into `cp.wmem`. Plain stores, as a bus master's; a release fence in `xenos_cp_set_wptr` orders a run's stores before its read-pointer write-back. |
| `x360_gpu_drain(x, flush_resolves)` | post → run, synchronous | everything posted run to its end, the lazy resolves written to memory if asked, the run half's interrupts queued on the post side. Called by `x360_state_save`, `state_load` (both in `x360_state.c`, at their start, never between sections) and `x360_render_flush`. **Only on the run half's thread** (below), or on the CPU worker during a stage-2 job (the GL thread runs it, the worker waits). |
| `gpu_hold` / `gpu_release` | post side brackets | a Vd* call that changes run-half state: drain first; after `VdInitializeRingBuffer` the seam is re-synchronised (`link_sync`: what was posted for the old ring dropped, `stalled` copied). Used by `VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`. |
| `link_sync` after a load | `x360_gpu_state` | `stalled` copied from the loaded command processor; a post is made pending again when the loaded `cp.wptr` is behind the loaded `last_wptr` (a state saved where the drain could not run), else nothing is pending. |
| `x360_gpu_set_deferred(x, on)` | host knob | `x360_gpu_poll` posts without running; `x360_gpu_run` / `_drain` run. Off by default; stage 2's hook and the seam test's. |

The link is host state, not in save states.

**The run half's thread.** The thread that attached the renderer (`x360_set_renderer_ex`: the GL context is current
there), else the first thread that ran packets. `x360_gpu_drain` (hence `x360_state_save`, `state_load`,
`x360_render_flush`) called on any other thread runs no packet and flushes nothing: the app's menu thread and its
auto-save thread have no GL context, and draws or resolves there would crash or raise GL errors. Posted work stays
posted for the run half; a state saved there has the command processor behind its write pointer (consistent: what
the hardware state is while the GPU lags), and loading it posts the rest again (`link_sync`); lazy resolves stay
pending (the state goes without the last 2 frames' resolves, as before WP-22: the title resolves them again). In
stage 1 nothing is ever left posted after a poll (the run follows the post; a stalled `WAIT_REG_MEM` is re-posted
and re-run at the next poll, not left pending), so this only matters with the deferred knob and from stage 2 on.
Checked by `x360_hle_test` (`test_gpu`, end): a deferred post, a state saved and `x360_render_flush` called on another
thread (nothing runs), a drain on the owner thread (it runs), that state loaded (posted again, run by the next drain).

## 3. Inventory: where the run half meets guest-visible state

Every place, before the split, where command-processor or renderer code read or wrote something the title can see,
or the guest side touched run-half state, and how it goes now.

| Where | What | Kind | Now |
|---|---|---|---|
| `xenos_cp_set_wptr` end | read pointer to `rptr_writeback` | store | seam store path (+ release fence before it) |
| `MEM_WRITE`, `REG_TO_MEM`, `COND_WRITE` (memory target) | words into memory | store | seam store path |
| `EVENT_WRITE_SHD` | fence value (`cp->counter` = swaps, or the packet's) | store | seam store path; this is what Direct3D's fences and `BlockUntilIdle` wait on |
| `EVENT_WRITE_EXT` | fake screen extents | store | seam store path |
| `EVENT_WRITE_ZPD` | occlusion query block: reads the 0xFFFFFEED end marker, writes counts | load + store | seam store path; the marker is written by the CPU *before* the packet is posted, so the post's release / acquire orders it |
| scratch register write-back (`xenos_cp_write_reg`) | `SCRATCH_REG0..7` to `SCRATCH_ADDR` | store | seam store path (Direct3D passes values to its interrupt handler this way) |
| `WAIT_REG_MEM`, `COND_WRITE` poll | a register (`cp->regs`, run state) or a memory word | load | stays a run-half read; an unmet wait sets `link.stalled`, the post side re-posts |
| `PM4_INTERRUPT` | the graphics interrupt callback, per CPU | callback | `link.irq`, turned into `raise_irq` by the post side |
| `VIZ_QUERY` | `PA_SC_VIZ_QUERY_STATUS_*` | `cp->regs` only | not guest-visible (titles read the register page, which this does not mirror) |
| `XE_SWAP` | `swap_hook` = `xenos_gl_swap` (front buffer decoded from memory, frame texture, frame files), `cp->swaps`, `cp->counter` | run state + GL | run half; **no guest callback**: `VdSwap` only writes the token words into the ring, on the guest thread, before the title moves the write pointer |
| resolves (`xenos_gl.c` `resolve_write`, `gpu_store`) | eager: pixels into memory | store (bulk, `cp.wmem`) | seam store path |
| lazy resolves (`flush_copy`) | pixels written later: on a GPU read of that memory, a GPU write over it, eviction, 2 swaps after, `xenos_gl_flush_resolves` | store | inside the run half; from outside only through `x360_gpu_drain(x, 1)` (`x360_render_flush`) |
| memory export (`memexport_end`) | stream-out into memory | store | seam store path |
| textures, vertices, indices, indirect buffers, `IM_LOAD`, `LOAD_ALU_CONSTANT`, ring packets | guest memory | load | run-half reads at run time (see 5.1, "Ordering and memory", for what stage 2 changes) |
| `shader_loaded` (`--dump-shaders`) | files on disk | host | run half |
| `VdInitializeRingBuffer` | `xenos_cp_ring_init`, `last_wptr`, `CP_RB_WPTR` | guest → run state | `gpu_hold` / `gpu_release` around the ring init |
| `VdEnableRingBufferRPtrWriteBack` | `cp.rptr_writeback` | guest → run state | `gpu_hold` / `gpu_release` |
| post side's retry test | `g->cp.stalled` | run state read | `link.stalled` |
| `x360_gpu_state`, `x360_gpu_bin_state` / `_reset` | the command processor in a state | run state | drained by `x360_state_save` / `state_load` at their start; `link_sync` after a load |
| `x360_state_save` / `state_load` | MEM must hold the run half's stores | | `x360_gpu_drain(x, 0)` at entry (rewind uses the same calls) |
| `x360_render_flush` (`state_save_op` on the machine thread, `x360_run --save-state`) | lazy resolves into memory | GL | `x360_gpu_drain(x, 1)`: nothing on a thread other than the run half's |
| `x360_render_reset` (end of a state load) | renderer starts over | GL | already a flag (`reset_pending`) the renderer acts on at its next draw / swap / flush |
| WP-19 swap counters (`host_swaps`, `long_swaps`, `last_swap_vt`) | swaps per second and swaps > 20 ms of virtual time | post state | the run half publishes `cp.swaps` in `link.swaps`; the post side counts the difference at its poll with the guest's clock (stage 1: the same poll as before); a load resets `last_swap_vt` and re-bases the count (`link_sync`) |
| `x360_gpu_stats`, `x360_render_perf_get`, `x360_gpu_opcode_histogram` | counters | run state read | read as they stand (racy in stage 2: make them relaxed atomics, or read after a drain) |

Nothing in the run half reads `x360_now`, a guest thread, or the kernel. The renderer's own GL entry points called by
the frame loop (`x360_render_end_frame`, `_texture`, `_frame`, `_programs`, `_stats`) are run-half (GL thread) calls.

## 4. Stage 1 (done)

Same thread, `x360_gpu_run` called right after the post. Checked on the Adventure Time 6 s scene: frames and end
state byte-identical to `perf/ref-wp9` (eager and lazy), and to `perf/ref-main` with `RECOMP_X360_NOOP_DRAWS=keep`.

## 5. Stage 2: the threads

### 5.0 What landed (`x360_gpu_overlap`, `x360_gpu.c`): off, awaiting a device check

Off by default (`RECOMP_X360_GPU_THREAD=1` turns it on), as `RECOMP_X360_DIRECT_PRESENT`: the gain is not measured on
a phone yet, an unpaced run loses virtual time to a slower GPU (below), and frames differ from stage 1's.

A smaller shape than 5.1's: the frame loop keeps its thread, its GL context and everything it did between
`x360_run_until` calls; only the call itself moves.

- `x360_gpu_overlap(x, job, arg)` (`x360_core.h`) hands `job(x, arg)` to a CPU worker (made at the first call, 16 MB
  stack, parked between jobs) and serves the run half on the calling thread until the job ends. The player
  (`x360_runtime.c`) runs each frame's `x360_run_until` that way; `x360_run --swaps N` (frames of 1/60 s, as the player)
  or a plain `x360_run` with `RECOMP_X360_GPU_THREAD=1`.
- Serving (GL thread, `gpu_overlap.mu` held only between runs): a post (`link.post` ahead of `link.taken`) and not held:
  `x360_gpu_run` with the lock released (`running`); then a flush request (lazy resolves); then acknowledge drain
  requests; then, the job done, return. Posted work can be left over at the end of a job: the next job, an inline drain
  (rewind, states, `x360_render_flush` on this thread) or `x360_gpu_run` runs it.
- Post side on the worker (`overlap_remote`): `x360_gpu_poll` posts and wakes the GL thread instead of running;
  `x360_gpu_drain` asks the GL thread and waits (then retries a stalled wait that may hold, up to 3 posts: the wait may
  have read the word just before the guest's store and the poll skipped the retry while that run lasted);
  `gpu_hold` = drain + `held` (+ waits for `!running`), `gpu_release` clears it; the stalled-retry test
  (`stall_retry_useless`) reads the command processor only under the lock with the GL thread idle, and calls a retry
  useless while it is busy (that run looks at the wait again).
- Back-pressure: `VdSwap` counts swaps issued (`swaps_issued`, re-based on `cp.swaps` by `link_sync`); after a post,
  while more than 2 are not run, the worker waits for the GL thread, unless it is stalled on a wait or has nothing
  posted left (no deadlock on a wait that needs the guest).
- Not overlapped (the job runs inline): the run half belongs to another thread, the GPU is deferred, host threads
  (`x->host`), or the worker cannot be made. The worker joins the host's thread bookkeeping
  (`x360_gpu_set_worker_hook`: ADPF session, fast cores, priority) and is joined by `x360_gpu_free`.
- `x360_gpu_cpu_ns` (taken out of the perf line's guest time) counts only runs on the guest's own thread.
- `RECOMP_X360_GPU_LOCKSTEP=1` (checks): every post waits for its run; results are those of stage 1.

States and rewind: the frame loop takes them between jobs, on its own thread (the run half's), so `x360_state_save` /
`state_load` drain inline as in stage 1 (section 6's "full drain" plan; rewind's per-frame drain gives back only the
tail of the frame's GPU work). The menu and auto-save threads take `s_mu`, which the frame loop holds for the whole job,
so they never see a job running; their saves keep the stage-1 rule (posted work left posted, posted again on load),
which is now common rather than deferred-only.

Checks (Linux, llvmpipe): `x360_hle_test` (`test_gpu`'s stress: writes, fences, stalled waits opened later, VdSwap
back-pressure, drains and `gpu_hold` from the worker; ThreadSanitizer-clean with `setarch -R`), `x360_render` (draws
posted from the worker, drawn on the GL thread), the x360 ctest set, and a retail stretch: `RECOMP_X360_GPU_LOCKSTEP=1`
gives stage 1's frames and states byte for byte; the default gives frames that differ only in animated elements
(trees, units, banners: the guest sees the GPU later in virtual time, section 8), and two such runs are identical
to each other here. Wall time on that stretch is GPU-bound on llvmpipe (about 270 ms of GPU and 50 ms of guest per
swap): 60 swaps in 17.8-20.0 s before, 16.8-19.0 s after (4 runs each), a gain within this shared host's noise. Not
measured on a phone yet.

Virtual time. The perf line's 58-59 swaps per virtual second (60 in stage 1) on those frame-by-frame runs is one
frame of pipeline, not lost time: a swap is counted when the GL thread runs it, up to one job (frame) after the guest
posted it, so a run stopped at its N-th swap holds one frame more (30 swaps: 30 / 31 frames x 60 = 58.1; 60 swaps:
59.0; 34: 58.3, as measured), and the guest's work per swap is the same (2.00 M dispatches per swap in both modes).
Back-pressure does not cost virtual time: the worker sleeps on a condition variable, executing nothing. Virtual time
is really lost in one case: an unpaced single job (`x360_run` with `=1` and no `--swaps`), where every guest thread
waiting lets the clock jump to the next deadline at once while the GPU takes host time, so Direct3D's frame throttle
sees the GPU many virtual frames late (2 swaps in 1.5 G instructions against 34). The player is paced (one job per
1/60 s of virtual time, 60 Hz of host time) and does not do that; a fix for the harness would be the scheduler waiting
for the GL thread's posted work before such a jump.

Left from 5.1 / later stages: the GPU runs only while a job runs (it idles while the frame loop does audio, present
and pacing, so overlap is within a frame); no partial hold for rewind (section 6); present on swap (section 7).

### 5.1 The original design

#### Who runs where

The GL context stays where it is: the SDL thread (`x360_play` / the app's machine thread) becomes the **GPU thread**.
The machine moves to a new **CPU thread**: the `x360_run_until` step loop, audio pull, cheats, rewind frames, pacing.
The SDL thread keeps the window pump, the GPU loop and the present.

- `x360_gpu_poll`: `gpu_post`; then, instead of `x360_gpu_run`, wake the GPU thread (a futex / condition variable,
  signalled only when `gpu_post` made a post); then `gpu_post_irq`.
- GPU thread loop: wait until `link.post >> 32 != link.taken` or a request is set; take the GPU mutex; `x360_gpu_run`;
  serve requests (flush resolves); release; broadcast "drained".
- `x360_gpu_drain`: on the GPU thread itself, run inline (as now: `state_save_op` from a hotkey on the SDL thread).
  From the CPU thread (rewind, states taken between steps): set the request bits (flush resolves), wake, wait for `taken == post >> 32` and the requests acknowledged,
  then `gpu_take_irq`. It is only correct while the post side posts nothing new: callers hold `s_mu` with the CPU
  thread outside `x360_run_until`, which is already so for states and rewind. This replaces stage 1's "skip off the
  owner thread" for any caller that can wait (the CPU thread, and then also the menu and auto-save threads, whose
  saves would then get the lazy resolves too); the skip stays for a caller that must not block on the GL thread.
- `gpu_hold` = drain + take the GPU mutex; `gpu_release` = `link_sync` + unlock.
- Stalls: the GPU thread never spins on a `WAIT_REG_MEM`; it sleeps until the next post. The post side re-posts at
  every poll while `link.stalled` (469 polls per swap here); if that wakes it too often, re-post only when the guest
  executed a store since (or every N polls).
- **Back-pressure** at two queued swaps: `VdSwap` (post side) counts swaps issued (host-only counter); the run half
  publishes `cp.swaps` after each run (`link.swaps_done`). `gpu_post` blocks on "drained" while
  `issued - done > 2`. Without it the CPU thread runs ahead by an unbounded number of frames (until the ring fills and
  Direct3D waits on the read pointer, which it would, but with seconds of latency).
- Host-threads mode (`x->host`, experimental): the poll already runs under `h->mu`; nothing changes.
- `x360_run` (headless harness): keep it synchronous (the flag off) for reference runs; the flag can create a GPU
  thread there too (EGL / ANGLE context made current on that thread after `x360_set_renderer`) for timing.

#### Ordering and memory

- The guest's ring stores happen before it stores `CP_RB_WPTR`; the post reads that and publishes `link.post` with
  release; `x360_gpu_run` loads it with acquire. Packets and what they point at are visible to the run half.
- The run half's stores happen before the release fence in `xenos_cp_set_wptr` and before the read-pointer write-back;
  `link.irq` is set with release and taken with acquire, so an interrupt handler sees the scratch / fence values
  written before the `PM4_INTERRUPT`.
- Guest memory is shared like a bus: plain loads and stores on both sides (as the host-threads mode already does for
  guest CPUs). The guest's own `lwsync` / `eieio` and the edges above are the only ordering, as on hardware.
- **Late reads**: the run half reads textures, vertices, indices and indirect buffers later than in stage 1, up to the
  queue depth (two swaps) behind the CPU. That is what real hardware does, and Direct3D guards buffer reuse with fences
  (`EVENT_WRITE_SHD`, the read pointer), but a title that rewrites a buffer it has handed to the GPU without a fence
  (it worked on Xenia-like synchronous emulators) draws the new contents. Watch for this first.
- Lazy resolves: their delayed writes happen on the GPU thread; a CPU read of a resolve's memory before the flush sees
  old bytes, as today (lazy is per title for that reason).

## 6. Stage 3: states and rewind

- Save (`state_save_op`, the automatic slot, `x360_run --save-state`): under `s_mu`, CPU thread parked between steps;
  `x360_state_save` drains (waits for the GPU thread to finish all posted work, and with `x360_render_flush` first,
  flushes lazy resolves on the GL thread). The state is then that of stage 1: frames and memory complete.
- Load (`state_load_op`, rewind step): drain, load, `link_sync`, `x360_render_reset` (the flag, picked up on the GL
  thread). Posted work from before the load is finished, then overwritten, never run against the loaded state.
- **Rewind takes a state every frame**: a full drain each frame puts CPU and GPU back in lockstep once per frame and
  gives back most of the overlap. Plan: a *partial* hold for rewind: take the GPU mutex (waiting only for the run in
  progress, not for the queue), save the command processor as it stands plus the posted write pointer. The state then
  has the GPU behind the CPU, which is consistent (it is what the hardware state is at any instant). To load it, the
  post side must re-post what the GPU had not taken: save `cp.wptr` (run half) apart from `last_wptr` (post half),
  and in `link_sync` after a load make a post pending when `cp.rptr != last_wptr`. Today `x360_gpu_state` saves
  `last_wptr` and `cp.wptr`; with full drains they are always equal, so the field order need not change, only the
  re-post rule. Runs are whole `xenos_cp_set_wptr` calls (up to a frame of packets): to bound the wait, let the run
  half release the mutex every N packets (its position is already resumable: `cp.rptr`, `cp.ib`).
- Thumbnails (`x360_render_frame`): a GL read-back, GL thread only (already only off-screen).

## 7. Stage 4: present on swap

The GPU thread presents right after the run that executed an `XE_SWAP` (the swap hook sets a flag; the GPU loop calls
`x360_render_end_frame`, `host_window_present_texture`, swap buffers), instead of the 60 Hz host step presenting
`frame_tex` whether or not the guest swapped. With back-pressure at two swaps, the guest's vblank-locked swaps pace the
display; if no swap arrives for 1.25 frames the last frame is presented again (the window stays live). Pairs with
WP-23 (`ANativeWindow_setFrameRate(60)`). The judder of a host step holding 0 or 2 swaps disappears.

## 8. Determinism: the trade-off

Stage 1 is deterministic: a run is a function of the inputs, frames and states compare byte for byte, input scripts
(`--pad-script`, play movies) replay.

From stage 2 on, the guest sees the GPU's results (the read pointer, fence words, interrupts, resolved memory) after a
number of guest instructions that depends on host timing. Direct3D's spin-wait (`xe_fn_82C955D8` here) spins a
different count, interrupts land at different virtual times, the scheduler's choices diverge: frames have the same
content but runs are not reproducible, and a pad script drifts. Consequences and options:

- References, regressions and pad-script checks run with `RECOMP_X360_GPU_THREAD=0` (= stage 1; `x360_run` is
  stage 1 unless `=1`). Stage 2 is off by default in the player too (`=1`) until the device numbers; the plan is to
  switch it on
  when the phone numbers justify it, per title (`k_title_env`) or for all.
- States and rewind stay correct (consistent snapshots), only replay after a load is not bit-exact.
- A deterministic variant is possible on top of the same seam ("delayed commit"): the run half's guest-visible word
  stores (everything on the seam's store path but the bulk resolves) and its interrupts go into a log tagged with the
  post number instead of into memory; the post side applies post *k*'s log at the first poll at or after the virtual
  time post *k* was made plus a fixed latency (one vertical blank), waiting for the GPU thread if it is late.
  Guest-visible effects then depend only on virtual time. Costs: resolves must be deferred too (lazy resolves almost
  are: write them at commit), and a GPU slower than the fixed latency stalls the CPU. Worth trying only if
  non-reproducible runs turn out to hurt (debugging, netplay, movie replays).
