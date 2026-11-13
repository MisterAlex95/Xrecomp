# Profiles in layers

> Part of the [Documentation Index](../INDEX.md)

A title's switches and fixes beyond the generic template live in `profiles/`, in layers. `tools.prepare` picks the
layers from the XBE, merges them and puts them into the title project (`src/title/`), on the desktop and on the
device alike (the tree ships in the app's `toolkit.zip`). Code: `tools/prepare/profiles.py`; spec: Plane XBA « Spec ·
Universal titles », section 3.

## The layers, lowest first

| Layer | Folder | Picked when |
|---|---|---|
| Toolkit | none: `src/` (runtime) and `tools/` (lifter); `recomp_xdk_defaults()` in the title project | always |
| XDK library | `profiles/xdk/<name>/layer.json` | the XBE's library table has one of its `applies.libraries`, at a build in `applies.builds` (no `builds`: any), and every code probe in `applies.probes` matched (`tools.xbe_ident`, XBA-105) or the build is in `applies.or_builds` (the builds known to need the layer); or the table names none of them and the XBE imports `applies.imports.min` of the kernel ordinals in `applies.imports.ordinals` (XBA-106) |
| Engine | `profiles/engines/<name>/layer.json` | `tools.xbe_ident` finds the engine whose `layer` (in `tools/xbe_ident/engines.json`) is `<name>` |
| Title | `profiles/titles/<title id>/title.json` | the title ID matches |

A title's `"layers"` lists the XDK and engine layers it was proven with: they are picked even when the XBE does not
select them, and prepare prints `asked for by the title profile, not selected by the XBE` (a sign `applies` or the
detection needs a fix). `python -m tools.xbe_ident default.xbe` prints the layers an XBE selects.

**A fix goes to the lowest layer where it is true.** What every XDK title needs because the console works that way
goes to an XDK layer (or the toolkit); what one title's own code needs stays with the title.

## When the library table is odd (XBA-106)

The XDK layers are picked from the XBE's library table; a table that is stripped, renamed or from a build nobody
listed used to leave a title with none of them: no DISPATCH gate, no vblanks, no pad, no sound. Two things now stand
in:

- **The switches are toolkit defaults.** `RECOMP_DISPATCH_GATE=1`, `RECOMP_VBLANK=paced`, `RECOMP_USB=1`,
  `RECOMP_KEYBOARD=1` and `RECOMP_AC97_READY=1` are set by the title project itself (`recomp_xdk_defaults()`, called by
  `templates/new-game` main after `xba_title_configure`) unless something already set them; `RECOMP_PB_EXEC=1` already
  was. They are the console's hardware, not a title's taste: every retail title links XAPI (and with it XPP's USB
  stack and its DISPATCH-level locking), and real hardware always has vblanks, the APU and the OHCI controllers. The
  XDK layers keep their identical `env_default`s, so a title they match merges exactly as before (tested against the
  four reference titles in `tools/prepare/test_profiles.py`). `RECOMP_XDK_DEFAULTS=0` sets none of them (A/B), and each
  switch reads `0` as off ([environment-variables.md](../runtime/environment-variables.md#xdk-defaults)).
- **An XDK layer can be picked by its kernel imports.** `applies.imports` lists kernel ordinals typical of the library
  (D3D8: `AvSetDisplayMode`, `AvSendTVEncoderOption`, `Av{Get,Set}SavedDataAddress`, `MmClaimGpuInstanceMemory`, two
  of them; XAPI: `XeImageFileName`, `XeLoadSection`, `XeUnloadSection`, `IoCreateSymbolicLink`,
  `ExQueryNonVolatileSetting`, three; DSOUND: `MmLockUnlockBufferPages`, `MmGetPhysicalAddress`,
  `HalGetInterruptVector`, `KeConnectInterrupt`, all four). When the library table names none of the layer's libraries
  and the XBE imports enough of them, the layer is picked and prepare warns. This matters for `xdk/d3d8`, whose code
  (`d3d8.c`: the device found at run time, the software methods, the GPU semaphore) the env defaults cannot replace;
  in a title without D3D it finds no device and stays idle. A layer whose library is named at a build it does not
  take (`xdk/d3d8ltcg`, 5849 only) is not picked by imports. The lists are a first guess from what each library does
  on the console, to check on the library triage (XBA-63).

prepare prints which XDK layers matched and how:

```
      XDK layers: xdk/d3d8 (kernel imports), xdk/dsound (DSOUND 5849), xdk/xapi (XAPILIB 5849)
      warning: xdk/d3d8: the library table names none of D3D8, D3D8LTCG, but the XBE imports AvSetDisplayMode, ...: picked by its kernel imports (a stripped or renamed library table?)
```

or `XDK layers: none matched; the runtime's XDK defaults apply (recomp_xdk_defaults)`.

## What a layer holds

Every key is optional. The same keys work in every layer; a higher layer's value wins.

| Key | What |
|---|---|
| `name` | what it is |
| `applies` | XDK layers: `{"libraries": ["D3D8", "D3D8LTCG"], "builds": [5558, 5849], "imports": {"ordinals": [3, 2, 1, 4, 168], "min": 2}, "probes": ["d3d_latch_wait"], "or_builds": [5849, 5849]}`; a probe is what the title's linked code does, found by `tools.xbe_ident` (`"probes"` in its answer), for a choice that depends on that rather than on a build number; `or_builds` keeps the builds a layer was picked for and tested with when a probe joins it, so a scan that misses cannot drop them |
| `hooks` | a C prefix `p`: the layer's `.c` files define `p_configure(void)` and `p_start(const char *game_dir, const char *save_dir)` |
| `env_default` | `{"RECOMP_*": "value"}`: set before the hooks run unless the environment or `recomp.cfg` already set it |
| `env_default_android`, `env_default_desktop` | the same for one platform; within a layer it wins over `env_default` |
| `env_force` | forced over everything ([environment-variables.md](../runtime/environment-variables.md#forced-by-a-title)) |
| `params` | `{"XBA_*": number}`: `#define`s in `xba_params.h`, for a lower layer's C (a title gives the addresses an XDK layer needs) |
| `seed_functions` | files in the layer for the disassembler (`tools.seed_from_log`'s format) |
| `exclude_manual` | one `.c` file in the layer whose functions replace the generated ones (one in the whole stack) |
| `layers`, `project`, `status` | titles: the layers above, the CMake project name, a one-line state |
| `_why` | free text: why each value is there |

## In the title project

`src/title/` gets each layer's `.c` files as `<kind>_<name>__<file>.c` (`xdk_d3d8__d3d8.c`,
`titles_4d5300d1__overrides.c`), plus three generated files:

- `xba_layers.c` defines the two hooks `main.c` calls (`xba_title_configure` before the memory layout,
  `xba_title_start` right before the guest's entry point): configure sets the merged `env_default`s, then calls each
  layer's `p_configure`, lowest first; start calls each `p_start`.
- `xba_params.h`: the merged `params`.
- `env_force.h` when the stack forces options (Android: `pack.json` `env_force` instead).

`pack.json` records the layers (`"layers"`), and the prepare stamps hash every layer's files: changing a layer
re-lifts the titles that use it.

## The layers today

| Layer | Holds | Seen on |
|---|---|---|
| `xdk/xapi` (XAPILIB, XPP) | the one-CPU DISPATCH gate for XPP's USB stack, paced vblanks, the pad (USB model, keyboard) | Fable, Burnout 3 |
| `xdk/dsound` (DSOUND) | the AC'97 codec ready (`DirectSoundCreate` waits for it) | Fable, Burnout 3 |
| `xdk/d3d8` (D3D8, D3D8LTCG) | the pushbuffer executor, paced vblanks; `d3d8.c`: D3D's GPU semaphore watch and its software methods (5: the fence event; 0xC-0xE: RunPushBuffer's return JUMP), with `XBA_D3D_DEVICE_PTR`, `XBA_D3D_DEVICE`, `XBA_D3D_SW_METHODS`, `XBA_D3D_DMA_WALK`, or the device found at run time | Fable, Burnout 3 |
| `xdk/d3d8ltcg` (D3D8LTCG with the `d3d_latch_wait` probe, any build) | `XBA_D3D_DMA_WALK` 1: the DMA walk, which a D3D8LTCG fence wait that spins on the 0x0310 latch needs (DMA_GET and PGRAPH 0x400B10); with it `d3d8.c` adopts the fence event once it finds the device at run time. Picked by the probe since XBA-105 (it was builds 5849-5849) | Black, Burnout 3 |
| `engines/renderware` | Criterion's RenderWare on D3D8LTCG: the DMA walk, title-built KEVENTs, 60 fps cap, frame generation off | Burnout 3 (Black, XBA-92) |
| `titles/4d5300d1` | Fable: map size, deferred I/O APCs, front-end sleep, 16:9, cache progress, fibers (`overrides.c`) | |
| `titles/4541005b` | Burnout 3: D3D addresses, the XID seed, flip pacing on the software rasteriser | |

D3D is linked into each title, so its statics move from XBE to XBE. A title layer can give them as `params`; without
them, `xdk/d3d8` finds the device at run time (XBA-80): once D3D has made it, the title's image is scanned for the one
dword `D` with `[D + 0x24A8] == D` (the pointer slot, XDK 5849), a pushbuffer ring at `[D + 0x24]..[D + 0x28]` in the
contiguous window, and the put pointer `[D]` inside it. When nothing passes that slot rule (XDK 5455, KOTOR: the slot is
`D - 8`), any image dword naming a `D` that passes the ring and put tests, has its GPU semaphore pointer `[D + 0x30]` in
the contiguous window and holds D3D's vblank/fence KEVENT pair is taken, if there is only one. The fence event's offset
is read from the device: the one pair of KEVENT headers 16 bytes apart (5849 +0x1DCC, 5455 +0x19AC; 5849's is assumed
when there is none). A unique hit is printed (`[D3D] device found at ... fence event +0x...`) and written to the
discovery file as `{"t":"d3d",...}` with the slot actually found; then the semaphore watch and every serviceable
software method run on it. Title-built KEVENTs are guest events by default, so `BlockOnTime` sleeps on the fence event
until method 5 sets it. Method 5 is serviced whenever the device is known, also with a title's `XBA_D3D_SW_METHODS`
(with the profile's addresses its offset is read at the first method 5): left unserviced, that blocking wait never
ends (Fable froze in Oakvale that way). With `RECOMP_TITLE_KEVENTS=0`, the DMA walk (`xdk/d3d8ltcg`, or `RECOMP_PB_WALK=dma`) still
adopts that one event (`xbox_KeAdoptGuestEvent`, `[D3D] fence event ... set by software method 5`). So a new
title on a known XDK build needs no title layer for D3D.

**Other XDK builds (XBA-105).** What cannot be read from the device's structure is looked up in one table in
`d3d8.c` (`k_layouts`), keyed by the title's D3D library and build from its XBE header: the pointer slot's offset, the
fence event's offset when the device holds no KEVENT pair, RunPushBuffer's pending count and node table. Known:
D3D8LTCG 5849 (Burnout 3, Black: slot +0x24A8, fence event +0x1DCC) and D3D8 5455 (KOTOR: slot -8, +0x19AC). A build the
table does not know uses the fallback's value for each such rule (5849's slot and fence event, Fable's RunPushBuffer
offsets +0x50/+0x264) and says it once per rule — `[D3D] unknown XDK build 5933 (D3D8LTCG): fence event (no KEVENT pair in
the device) fell back to +0x1DCC (...)` — with a `{"t":"d3d_layout","lib","build","rule","used"}` line in the discovery
file; on such a build a found device's pending count is decremented only when it reads as a count (1-4096). A run of a
title on a new build therefore says what to add to the table. `tests/xdk_d3d8_layer` has one synthetic device per
layout: 5849 LTCG, 5455, Fable's params, an unknown LTCG 5933 on 5849's slot rule without an event pair, an unknown
D3D8 4627 with its slot and events elsewhere (found by structure, nothing to fall back on but the ring offsets).
What only real titles on other builds can confirm: that the ring, put and semaphore offsets (+0x24/+0x28/+0/+0x30)
hold on every build (they are what finds the device: a build where they moved is not found at all, and needs params);
the fence-event pair shape; RunPushBuffer's offsets beyond Fable; and that `d3d_latch_wait` matches the far-behind probe
of every D3D8LTCG build that needs the walk (and no D3D8 that does not).

## Adding a title

1. `python -m tools.prepare` with no title layer: the XDK layers apply on their own.
2. A fix that one title needs: `profiles/titles/<title id>/` (`title.json`, `title.c` with `hooks`, `NOTES.md`).
3. Once a second title needs the same fix, move it down to the XDK or engine layer and make the title-specific part
   (addresses) a `param`.

## No per-title code outside profiles/titles (XBA-109)

A title ID, a game's name or one game's guest address belongs in `profiles/titles/<title id>/`, never in `src/`,
`include/`, `templates/`, `tools/` or `android/`. `tools/ci/title_guard.py` checks it in the linux CI job (through
`tools/ci/test_title_guard.py`, part of `pytest tools/`); run it by hand with `python3 tools/ci/title_guard.py`.

It reads **code tokens**, not prose. Comments are history and stay free: « Fable does X, hence this generic path »
explains why generic code exists. Identifiers and string literals may carry none of the three. A Python docstring is
prose for names, but not for title IDs and addresses (it is printed by `--help` and holds copy-paste usage examples).

- **Retail title ID**: 8 hex digits whose two high bytes are upper-case letters (the publisher code, `4D53` « MS ») and
  whose low word is below `0x1000`. `5842xxxx` (« XB ») is ours: tests and the synthetic library use it.
- **Game name**: a short list of distinctive names in the guard, matched on whole words (`isFableHook` and
  `BURNOUT3_X` match, `Halogen` does not).
- **Game address**: a `0x` value or `sub_XXXXXXXX` that a title's profile names (its `.json` values, the `sub_`
  functions of its `.c`), plus a few known former defaults.

Not scanned: Markdown, tests (`tools/**/test_*.py`, `android/**/src/test/**`), `docs/`, `profiles/`, `tests/`. A real
exception goes into `tools/ci/title_guard_allow.txt` with its reason (today: upstream's reference section tables,
kept byte-identical for `tools/sync_upstream.sh`, and a French word that is also a game's name).
