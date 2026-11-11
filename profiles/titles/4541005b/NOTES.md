# Burnout 3: Takedown (4541005B) — title profile notes

PAL Fr/De/It disc, XDK 5849, D3D8LTCG / DSOUND / XMV / XPP linked in. Entry point `0x001D2B27`, kernel thunk
table `0x0036BAC0` (147 imports, all resolved by the bridge; `gen_ordinal_remap` gives identity: no ordinal remap
needed). No game data, XBE or generated code in this repository.

Result (Mac, GL back end, 2026-11-11): boot to a finished race at 60 fps — movies with sound, French title screen,
profile and save, menus, vehicle select, training video, 2-lap race, results, auto-save. Two 15-minute soaks: no
stall.

## What the profile does

Since XBA-66 most of it comes from the XDK layers (`profiles/xdk/xapi`, `dsound`, `d3d8`; title.json `"layers"`), with
D3D's addresses in title.json `"params"`; `title.c` keeps the flip pacing only. Rules: `docs/technical/profiles.md`.

| Setting | Why |
|---|---|
| `RECOMP_PB_EXEC=1`, `RECOMP_PB_WALK=dma` | D3D's fence wait reads `DMA_GET` and the `0x0310` latch (PGRAPH `0x400B10`); the segment scan never latches it and the front end stalls at `_FEMain.xmv` (~87 s) |
| semaphore watch (XDK D3D layer, `XBA_D3D_DEVICE_PTR`) | hands device+0x30 to `nv2a_pb_set_semaphore_target`; without it a black screen, BlockOnTime spinning at `0x00351BA3` |
| software-method hook, type 5 → `xbox_KeSetGuestEvent(device + 0x1DCC)`, `RECOMP_TITLE_KEVENTS=1` (XDK D3D layer, `XBA_D3D_SW_METHODS` 0x20) | BlockOnTime's far-behind path patches method 5 and waits without timeout on that event, which D3D built itself |
| `RECOMP_AC97_READY=1` | without: hang after the USB start, no flip |
| `RECOMP_VBLANK=paced` | without: one flip (XMV clock, swap counters) |
| `RECOMP_DISPATCH_GATE=1`, `RECOMP_USB=1`, `RECOMP_KEYBOARD=1` | same XDK USB stack as Fable; gate off prints `[IRQ] gate off: N DPCs at DISPATCH` (3–13 in 335 s) |
| `RECOMP_GPU_FPS_CAP=60`; `RECOMP_FLIP_PACING=1` on the desktop software rasteriser | the game is locked to the flip rate |
| title.json `"env_force": {"RECOMP_LSFG": "0"}` | frame generation 2x on a 60 Hz panel caps at 30 → slow motion; the shell applies it before the window opens |
| seed `0x00363064` | the XID class driver's Init, see below |
| `XBA_D3D_FENCE=mirror` (was `BO3_FENCE`) | A/B fallback: segment scan, `xbox_Nv2aMirrorFence(0x0035FE48, 0x2C, 0x30)`, no hook, no watch |

Not needed: `xbox_SetMapSize`, `RECOMP_GUEST_WINDOW`, `RECOMP_THREAD_STACKS`, deferred I/O APCs, delay overrides.

## Addresses

| What | Address |
|---|---|
| D3D device pointer → device (static in the D3D section) | `[0x0035FE48]` → `0x0035D9A0` |
| fences submitted / semaphore pointer / fence KEVENT | device + `0x2C` / `0x30` / `0x1DCC` |
| GPU semaphore (contiguous memory) | `0x803E6000` |
| BlockOnTime | `sub_00351A70` (spin `0x00351BA3`, wait `0x00351BB4`) |
| far-behind probe (compares `0x400B10` with the semaphore) | `sub_00351A00`, loop at `0x00351A10` |
| XID class driver Init | `0x00363064` |
| class-driver tables | `0x00362DF4`..`0x00362E0C`; Init pointers at `0x00362ECC` / `0x00362EE4` |
| XID AddDevice (fails `0x80000100` without Init) | `0x00364F40`, state `[0x0041AAF8]` |

These are this build's; `docs/technical/d3d8ltcg-device-context.md` lists another D3D8LTCG build (PAL is +0x300).

## The pad: a function only a table reaches

XPP's section starts with data; the linear sweep decoded `00 55 8B` at `0x00363063`, one byte early, so the XID
Init at `0x00363064` (reached only through the class-driver table words) was never lifted, `[0x0041AAF8]` stayed 0
and AddDevice failed. The seed fixes it (~200 reports/s).

## Frame lock

Cap 30: 98 → 137 km/h in 5 s, against 2.5 s at 60. Cap 0: 1,450–3,470 flips/s. `RECOMP_FLIP_PACING=1` holds exactly
60 without the cap.

## Runs (335 s, scripted to a race with `race.pad`)

| Run | Configuration | Result |
|---|---|---|
| r5a | DMA walk + semaphore, no hook | stall at `_FEMain.xmv` |
| B, G, H, W1 | DMA walk + method-5 hook + title KEVENTS (the profile) | race at 59–60 fps, 2,107–5,907 method-5 serviced |
| S1 | the same, 900 s soak | finished, auto-save, no zero-fps report |
| A, S2 | segment scan + `0x0310` latch hack + hook (+ gate, 900 s) | race / finished (not shipped) |
| D | segment scan + latch, no hook | race, but 184 method-5 unserviced (latent hang) |
| C | `BO3_FENCE=mirror` (now `XBA_D3D_FENCE=mirror`) | race, no toolkit change |

Verify (desktop): `tools.prepare <iso> --work <dir> --force` (the stamp does not see a profile-only change), then
in `project/build`:
`SDL_AUDIODRIVER=dummy RECOMP_FB_WINDOW=0 RECOMP_GPU=1 RECOMP_SAVE_DIR=<fresh> RECOMP_PAD_SCRIPT=@race.pad
RECOMP_PB_REPORT_MS=5000 timeout -s KILL 335 ./burnout3`. Pass: the `[D3D] device 0x0035D9A0, GPU semaphore at
0x803E6000` line, no `Failed to resolve`, no ` 0.00 fps`, 59–60 fps in the race, `software methods: N serviced by
the title` with N > 0. The pad scripts assume a fresh save folder:

```
22000:start,26000:start,30000:start,34000:start,45000:a,52000:up,53500:a,61000:a,69000:a,77000:a,85000:a,93000:a,101000:a,109000:a,270000:rt:28000,300000:rt:28000
```

## Open

- The DMA walk has not run on Android yet (Fable uses the segment scan).
- Software methods 8 and 9 are not serviced (no visible effect so far).
- The widescreen flag alone hangs D3D init; `ExQueryNonVolatileSetting` 0x103 is unhandled.
