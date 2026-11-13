# Reuniting Burnout 3 with xboxrecomp

> Part of the [Documentation Index](../INDEX.md)

Burnout 3 was the first static recompilation of an original Xbox title with no
prior recompiler to lean on. The core of xboxrecomp was written while getting it
to boot, then extracted into a reusable toolkit and hardened against Halo,
Crimson Skies and Steel Battalion. The two then diverged. This page started as
the plan to bring Burnout 3 back onto the shared toolkit; that has now happened
another way, and the status below supersedes the plan.

## Status (2026-11-11): runs on the shared toolkit

Burnout 3 (PAL Fr/De/It disc, title ID 4541005B, XDK 5849) runs on the shared
toolkit and runtime with no fork: `tools.prepare` on the disc image, the generic
`templates/new-game` project, and (then) a small title profile, `profiles/titles/4541005b/`
(a `title.c` of about 150 lines, one seeded function). Since profile-free WP6 it has no
profile at all: everything it held is a toolkit default or found by the analysis. On the Mac with the GL back
end it goes from boot to a finished race at 60 fps: movies with sound, the title
screen, profile and save, menus, vehicle select, a 2-lap race, results and
auto-save; two 15-minute soaks did not stall. Android has not been tried yet.

What it took, beyond what Fable already needed:

| Problem | Fix |
|---|---|
| black screen: D3D's `BlockOnTime` spinning on the GPU semaphore | the profile hands `[device + 0x30]` to the pushbuffer executor (`nv2a_pb_set_semaphore_target`) |
| front end stalls at `_FEMain.xmv`: the `0x400B10` latch never moved | `RECOMP_PB_WALK=dma` |
| under the DMA walk, `BlockOnTime` sleeps for ever on the fence event | a software-method hook answers method 5 with `xbox_KeSetGuestEvent` (new in `kernel_bridge.c`), `RECOMP_TITLE_KEVENTS=1` |
| no pad: the XID class driver's Init was never lifted | seed `0x00363064`; `tools/disasm` now realigns a table-named prologue the sweep stepped over |
| game in slow motion with frame generation or a 30 fps cap | the game is locked to the flip rate: cap 60, frame generation off (`env_force` in `title.json` until WP4; now the run-time lock verdict of `RECOMP_PACING=auto`, [framegen.md](framegen.md#titles-locked-to-the-flip-rate)) |

Details, addresses and runs: [burnout3-notes.md](burnout3-notes.md).

What did **not** turn out to be needed: the per-title ordinal remap (Phase 2
below), Burnout 3's own runtime libraries, its hand-written overrides, its xemu
device-context snapshot, or the threading-model reconciliation (the shared
kernel's spawned threads run it as they run Fable). The kernel imports are 147,
all resolved by the bridge with the standard ordinals.

The sections below are the original plan, kept for the record, with the
ordinal claim corrected where it appears.

## The one-line finding (original plan)

**The toolkit halves reunited cleanly; the runtime halves did not.** After the
extraction, xboxrecomp's `tools/` and Burnout 3's `tools/` diverged in one
direction (xboxrecomp got every fix, the fork kept one feature — now upstreamed),
but the *runtime libraries* diverged a lot. The plan saw two walls: the two
kernels drive the game loop in incompatible ways (**Phase 1, reconciled**), and
an XDK-specific kernel ordinal table (**Phase 2**). The second wall does not
exist: xboxkrnl's export ordinals are the same for every XDK, see the
correction below.

## What is already done

**Toolkit (`tools/`): reunited.** Burnout 3's fork had exactly one thing
xboxrecomp lacked — `--exclude-manual`, which reads `recomp_manual.c` for the
functions it defines by hand and skips generating their bodies. That is now
upstreamed (`manual_scan.py` + the `--exclude-manual` flag). Proven end to end:
xboxrecomp's full pipeline on Burnout 3's XBE — correct export table, call-target
realignment, transcendentals, real return addresses, flat dispatch — generates
22,893 / 23,010 functions, 0 failed, excluding 117 hand-written overrides and
wrapping 2. **Burnout 3's recompiled code can be produced entirely by
xboxrecomp's tools today.**

The x87 transcendentals were also ported directly into Burnout 3's lifter fork
as an interim fix (its own commit on the `linux-port` branch), which measurably
eliminated its FP exceptions (10 → 0). That is redundant once the toolkit swap
lands, but it is a real correctness fix in the meantime.

## Why the runtime is not a swap

Measured, file by file. Every library diverged substantially — no library is a
clean stale copy:

| library | BO3 vs xboxrecomp | nature of divergence |
|---|---|---|
| `src/kernel` | 17/17 files differ | **architectural** (threading model) |
| `src/d3d` | 9/12 files differ | co-tuned with BO3's nv2a pipeline |
| `src/nv2a` | game-coupled | 2 BO3-only bridge headers |
| `src/audio` | 768 diff-lines | independent evolution |
| `src/input` | 488 diff-lines | independent evolution |

### The hard gate: two opposite threading models

This is the blocker, and it is not staleness — it is two valid designs that each
co-evolved with their game layer.

- **Burnout 3**: `xbox_PsCreateSystemThreadEx` creates a **real Win32 thread**
  (`CreateThread`). Its `main.c` frame-pump drives the game tick on a claimed
  worker-stack slice (`xbox_worker_stack_alloc`, 16 × 256 KB), and its watchdog
  samples the game thread through `xbox_thread_debug_handle`. Recent commits —
  "Make game_frame_pump the only frame driver", "Give the host loop a stack so it
  can drive the game tick" — built this. The game loop *requires* real concurrency.

- **xboxrecomp**: `bridge_PsCreateSystemThreadEx` runs the start routine
  **synchronously** through `RECOMP_ICALL` ("Must run synchronously…"). This is
  what Halo and Crimson need; their loops are not built on a worker frame-pump.

Swapping Burnout 3 onto xboxrecomp's kernel would replace real threads with
synchronous execution and its frame pump would never be driven. Swapping the
other way would break Halo. **Neither kernel is a superset of the other.**

**Correction (2026-11): the ordinal claim was wrong.** An earlier version
said the kernel export ordinals are XDK-version-specific: that 50 of Burnout 3's
147 imports map to different ordinals, that ordinal 49 is
`HalRequestSoftwareInterrupt` for XDK 5849 but `HalReturnToFirmware` for older
XDKs, and that a kernel swap exited because 49 was routed to
`HalReturnToFirmware` during engine setup. The kernel is in the console's flash,
not linked into the title, and its export numbering does not change with the
XDK: `tools.xbe_parser` reads Burnout 3's ordinal 49 as `HalReturnToFirmware`
like any other title's, and `tools/kernel_audit/gen_ordinal_remap.py` gives
identity for all 147 imports (and for Fable's 154, also XDK 5849). The early
exit seen in that swap had another cause, not identified; it does not occur
with the current toolkit.

## The plan

Sequenced so each step is independently testable and nothing regresses a working
title.

**Phase 0 — toolkit. Mostly done.** `--exclude-manual` upstreamed; xboxrecomp's
tools generate Burnout 3's code. To *retire* the `tools/` fork — point Burnout 3's
regen at xboxrecomp's `tools/` and delete its copy — two small loose ends remain,
both low-risk:

- Burnout 3's `gen_dangling_stubs.py` is **redundant** with xboxrecomp's built-in
  `recomp_stubs_unresolved.c` (the translator emits stubs for unresolved call
  targets inline; the realignment fix shrinks that set further). It is dropped,
  not ported.
- Burnout 3's `recomp_types.h` is a stale copy that predates flat dispatch, so it
  lacks the `recomp_dispatch_init` declaration xboxrecomp's generated code
  references. Replace it with xboxrecomp's template (re-applying any Burnout 3
  game-local additions). `merge_names` and `xdk_names.json` already work with
  xboxrecomp's `func_id`; those stay as Burnout 3 config.

Completing Phase 0 is a regen + full rebuild (the giant chunks are slow) and does
not touch the runtime, so it is safe — just not instant.

**Phase 1 — threading reconciliation. Done.** xboxrecomp's kernel now supports
both driving models, defaulting to the one Halo and Crimson Skies use so they are
untouched.

- `XBOX_WORKER_STACK_*` region + `xbox_worker_stack_alloc`/`free` (verbatim from
  the Burnout 3 fork): a guest stack for the host's own thread to call recompiled
  code from, which is what a host-tick-driven title needs to pump its frame tick.
- `xbox_thread_debug_handle` + `g_game_thread`, recorded when the bridge spawns a
  thread, so a host watchdog can sample the game thread.
- `xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN)`: in SPAWN mode every
  `PsCreateSystemThreadEx` is a real thread so the title's entry can return and
  the host can drive. Default is `INLINE` — the first call runs the game in place,
  the historical behavior.

All of it is additive: a default-model title calls none of it, so the region is
unused address space and the code is never reached. Verified: Halo builds and
runs **byte-identically** (same exit, same milestones, same
`render_cameras.c:458`), the allocator passes a standalone correctness check (16
distinct slices, exhaustion, free/reuse, aligned stride), and Burnout 3's exact
`main.c` threading API compiles against xboxrecomp's headers. What remains is
Phase 2 wiring it up under a real swap — including confirming empirically whether
Burnout 3's init thread must genuinely run concurrently (SPAWN) or whether INLINE
suffices.

**Phase 2 — kernel swap. Done another way.** The plan was a per-title ordinal
remap (`xbox_kernel_set_ordinal_remap`, generated by
`tools/kernel_audit/gen_ordinal_remap.py`) followed by chasing why the spawned
init worker did not advance. Neither was needed: the ordinals are standard (see
the correction above; the remap hook stays as a check for a non-standard
title), and the current kernel's threads run Burnout 3's init worker like any
other. What stopped the game on the shared kernel was D3D's GPU synchronisation
and the pad's class driver, as listed under Status.

**Phase 3 — nv2a / d3d. Not needed.** The fork drove its own nv2a through two
local snapshot headers and an xemu-captured device context. On the shared
toolkit the title's own statically linked D3D runs as lifted code, and the
shared pushbuffer executor draws what it submits.

**Phase 4 — audio / input. Not needed.** The shared APU, AC'97 and USB/XID
stacks run the title's DirectSound and XPP as they run Fable's.

## The honest summary

The reconciliation this page planned — keep Burnout 3's runtime, merge it piece
by piece — was overtaken by the shared runtime growing enough to run the title's
own libraries unmodified (the pushbuffer executor, the emulated APU, the USB
stack, title-built kernel events). Burnout 3 now needs what any title needs: a
profile with its switches and a few addresses.

One thing from the plan was wrong and is worth saying plainly: the kernel
ordinals are not XDK-specific (see the correction), so both the earlier
"Burnout 3's kernel is shifted" and the later "two correct tables for two XDKs"
rested on a false premise. The worker-stack and SPAWN threading support that Phase 1
added to the shared kernel stay; nothing in the current profile uses them.
