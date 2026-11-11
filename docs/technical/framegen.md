# Frame Generation (LSFG)

> Lossless Scaling frame generation on xboxrecomp: a Vulkan compute sidecar
> turns two consecutive frames into an in-between one, and a pacer shows real
> and generated frames at even intervals.
>
> **Read this when:** you work on `src/framegen/`, the presenter's frame
> generation path, or a log says `[LSFG] off: ...`.
>
> Part of the [Documentation Index](../INDEX.md)

Built only with `-DXBOXRECOMP_LSFG=ON` (POSIX hosts); the default build and the
upstream tests do not see it. Run-time switches: see
[Environment Variables](../runtime/environment-variables.md#frame-generation).

## Pacing

The pacer (`src/framegen/framegen_pacer.c`, C, libc + pthreads) sits between
the presenter and the generator. The presenter hands it every real frame at its
flip (`framegen_pacer_submit`, CLOCK_MONOTONIC flip time); the pacer's thread
asks the generator for the in-between frame(s) and hands real and generated
frames back to the presenter's sink (`FramegenSink.show`) with the time each is
due on screen. The presenter keeps a timed FIFO and shows each entry at its due
time (presenter package, not wired yet).

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
- **Pixels**: everything the sink receives is 32 bpp BGRX, packed. 16 bpp
  R5G6B5 frames are expanded at submit, so the generator and the presenter's
  texture see one format.
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
  `RECOMP_LSFG_DISPLAY_HZ` (default 60) until the presenter reports it. Log:
  `[GPU] back end frame cap 30 fps for frame generation (was 60)`.
- **Present mode**: the GL back end (`nv2a_gl.c`) forces
  `RECOMP_GPU_PRESENT=sync`, since the asynchronous read-back delivers frame N
  at flip N+1 and the pacer times frames by their own flip. Log:
  `[GPUGL] present read-back sync (frame generation)`.
- `RECOMP_FLIP_PACING` (the guest vblank flip hold) is independent.

Both are read once at start-up; if the pacer falls back later, the cap and the
sync read-back stay for the run (30 fps is the Android profile's cap anyway).

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
