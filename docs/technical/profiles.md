# Profiles in layers

> Part of the [Documentation Index](../INDEX.md)

**A title needs no profile.** It is prepared and run by the toolkit alone: its defaults (the XDK switches, the D3D
service with the DMA walk, run-time pacing and the frame-lock verdict), what the analysis reads from the XBE (static
discovery, packed tables) and what the runs find (the gated discovery loop). A title with no layer is the normal case,
and every reference title runs that way (profile-free WP1-WP6, report `profile-free-report.md`).

What `profiles/` can still add is **optional**: a title layer holds enhancements of one game that cannot be derived (a
16:9 mod of its camera, a faster front end), never something the title needs to boot, draw, play sound, take input
or keep its speed. `tools.prepare` picks the layers, merges them and puts them into the title project (`src/title/`),
on the desktop and on the device alike (the tree ships in the app's `toolkit.zip`). Code:
`tools/prepare/profiles.py`; spec: Plane XBA « Spec · Universal titles », section 3.

## Optional by construction

- **Prepare says what it adds.** A title the toolkit has enhancements for prints
  `title enhancements: titles/<id> (16:9 scenes, front end at the display rate)` (title.json `"enhancements"`).
- **The A/B switch.** `python -m tools.prepare SOURCE --no-title-layer`, or `XBA_NO_TITLE_LAYER=1` in the environment,
  prepares the title as an unknown one: no title layer, nor the layers it names (prepare prints
  `title enhancements left out ...`). The prepare stamps hash the layers, so switching re-lifts. The executable is
  `title_<id>` either way (no shipped title layer sets `"project"`).
- **Launchers.** The macOS launcher (Manage › More › Enhancements) and the Android app (Settings › General ›
  Enhancements, per game) offer the switch for a title the toolkit has enhancements for. Off, their prepares run with
  `--no-title-layer` / `XBA_NO_TITLE_LAYER=1`. `prepare --check --json` and `pack.json` say `"title_layer": "on"` or
  `"off"` for such a title (nothing for another), so a build made the other way is offered for Prepare again (the Mac
  through `--check`'s `profile` reason, the app by comparing pack.json with the switch).
- **The compatibility matrix both ways.** `python -m tools.compat run <sources> --no-title-layer` prepares in a work
  folder of its own (`<name>-no-title-layer`) with the variable set and marks its result lines `"title_layer": false`;
  `tools.compat report` then shows a « No title layer » column next to the normal stage, a lower stage there in bold.
  The stage must not drop: a title layer may never be needed to run.
- **The CI guard.** `tools/ci/layer_guard.py` (run by `tools/ci/test_layer_guard.py` in `pytest tools/`) fails the
  build when a shipped layer carries what a general mechanism covers (below). A justified exception goes into
  `tools/ci/layer_guard_allow.txt` with its reason.

## The layers, lowest first

| Layer | Folder | Picked when |
|---|---|---|
| Toolkit | none: `src/` (runtime) and `tools/` (lifter); in the title project `recomp_xdk_defaults()` and the D3D service (`recomp_d3d_configure` / `recomp_d3d_start`) | always |
| XDK library | `profiles/xdk/<name>/layer.json`: none ships | a title's `"layers"` names it (a developer's A/B, never shipped) |
| Engine | `profiles/engines/<name>/layer.json`: none ships | `tools.xbe_ident` finds the engine whose `layer` (in `tools/xbe_ident/engines.json`) is `<name>` |
| Title | `profiles/titles/<title id>/title.json`: optional enhancements | the title ID matches, unless `--no-title-layer` / `XBA_NO_TITLE_LAYER=1` |

A title's `"layers"` lists engine (or developer XDK) layers to pick even when the XBE does not select them, and prepare
prints `asked for by the title profile, not selected by the XBE`; a shipped title may not use it.
`python -m tools.xbe_ident default.xbe` prints the libraries and engines an XBE was built with.

**A fix goes to the toolkit.** What every XDK title needs because the console works that way is a default; what an
engine needs is found from the XBE or decided at run time; what cannot be computed from the XBE or a run is not a fix
but an enhancement, and only that stays with a title.

## What a shipped layer may hold (tools/ci/layer_guard.py)

| Fails | Why | Instead |
|---|---|---|
| a `profiles/xdk/*/layer.json` or `profiles/engines/*/layer.json` | what they did is the toolkit's default | `src/`, or a developer's local A/B layer |
| a title.json without `"enhancements"` | a layer with nothing to add is deleted (Burnout 3's went in WP6: a name and a state) | the XBE certificate's name, the prepare's state |
| `"layers"` | the XDK layers are defaults, engines are found from the XBE | nothing |
| `"seed_functions"` | static discovery and the gated discovery loop find code (WP1, WP5) | nothing, or an analysis fix |
| `"params"` `XBA_D3D_*` | the D3D service finds the device at run time | `RECOMP_D3D_*` at run time for an A/B |
| `env_default*` / `env_force` of `RECOMP_PB_WALK`, `RECOMP_PB_EXEC`, `RECOMP_GPU_FPS_CAP`, `RECOMP_PACING`, `RECOMP_FLIP_PACING`, `RECOMP_FRAME_LOCK`, `RECOMP_LSFG`, the D3D service's and the five XDK switches | a general mechanism sets or decides them | its default, or the player's `recomp.cfg` |
| a key the guard does not know | a typo, or a new key to review | add it to `KNOWN_KEYS` with the rule it needs |

## The XDK is the toolkit's default

`profiles/xdk/{xapi,dsound,d3d8,d3d8ltcg}` used to be picked from the XBE's library table (by build, by code probe,
by kernel imports). Everything they held is now done for every title by the title project itself, so a title with no
layer at all boots, draws, plays sound and takes input:

- **The switches** (XBA-106). `RECOMP_DISPATCH_GATE=1`, `RECOMP_VBLANK=paced`, `RECOMP_USB=1`, `RECOMP_KEYBOARD=1` and
  `RECOMP_AC97_READY=1` are set by `recomp_xdk_defaults()` (called by `templates/new-game` main after
  `xba_title_configure`) unless something already set them; `RECOMP_PB_EXEC=1` already was. They are the console's
  hardware, not a title's taste: every retail title links XAPI (and with it XPP's USB stack and its DISPATCH-level
  locking), and real hardware always has vblanks, the APU and the OHCI controllers. `RECOMP_XDK_DEFAULTS=0` sets none
  of them (A/B), and each switch reads `0` as off ([environment-variables.md](../runtime/environment-variables.md#xdk-defaults)).
- **The D3D service** (`src/kernel/recomp_d3d_service.c`, the former `profiles/xdk/d3d8/d3d8.c`): D3D's device found
  at run time, its GPU semaphore handed to the executor, its software methods answered (below). main calls
  `recomp_d3d_configure()` right after `recomp_xdk_defaults()` and `recomp_d3d_start()` right before the guest's entry
  point. In a title without D3D it finds no device and stays idle, printing nothing. `RECOMP_D3D_SERVICE=0` leaves it
  out (A/B).
- **The DMA walk.** `recomp_d3d_configure` makes `RECOMP_PB_WALK=dma` the default: D3D8LTCG's fence wait reads
  DMA_GET and the 0x0310 latch (PGRAPH 0x400B10), which only the walk keeps honest (Black froze at the menu and
  Burnout 3 needs it), and the walk delivers software method 5, which the service answers. It was the `xdk/d3d8ltcg`
  layer's `XBA_D3D_DMA_WALK`, picked by the `d3d_latch_wait` probe or build 5849; Fable (D3D8LTCG 5849) had it
  through that build rule. KOTOR (D3D8 5455) moves from the segment scan to the walk: 5 runs each way showed no
  difference (K-7). `RECOMP_PB_WALK=scan` keeps the segment scan (A/B); a layer's or the environment's value wins,
  since the default is set after them. A project whose main never calls `recomp_d3d_configure` keeps the segment scan
  (`nv2a_pb_scan.c`): nothing would answer the software methods the walk makes D3D ask for.

`tools.xbe_ident` still reports the libraries, their builds, the kernel imports and the `d3d_latch_wait` probe
(`ident.json`, the triage), as information. prepare prints:

```
      XDK: D3D8LTCG 5849, DSOUND 5849, XAPILIB 5849; the toolkit's defaults apply (XDK switches, D3D service, DMA walk)
```

A developer can still make `profiles/xdk/<name>/layer.json` for an A/B and name it from a title's `"layers"`; an old
`applies` in it is not read.

## What a layer holds

Every key is optional. The same keys work in every layer; a higher layer's value wins.

| Key | What |
|---|---|
| `name` | what it is |
| `hooks` | a C prefix `p`: the layer's `.c` files define `p_configure(void)` and `p_start(const char *game_dir, const char *save_dir)` |
| `env_default` | `{"RECOMP_*": "value"}`: set before the hooks run unless the environment or `recomp.cfg` already set it |
| `env_default_android`, `env_default_desktop` | the same for one platform; within a layer it wins over `env_default` |
| `env_force` | forced over everything ([environment-variables.md](../runtime/environment-variables.md#forced-by-a-title)) |
| `params` | `{"XBA_*": number}`: `#define`s in `xba_params.h`, for the layers' C (the D3D addresses are no longer params: the service finds them, `RECOMP_D3D_DEVICE_PTR` and friends override them at run time) |
| `seed_functions` | files in the layer for the disassembler (`tools.seed_from_log`'s format) |
| `exclude_manual` | one `.c` file in the layer whose functions replace the generated ones (one in the whole stack) |
| `enhancements` | titles: what the layer adds, a few words each (prepare prints them); a shipped title layer must have one |
| `layers`, `project`, `status` | titles: the layers above (not in a shipped title), the CMake project name (default `title_<id>`), a one-line state |
| `_why` | free text: why each value is there |

## In the title project

`src/title/` gets each layer's `.c` files as `<kind>_<name>__<file>.c` (`titles_4d5300d1__overrides.c`), plus
three generated files (a title with no layer gets no `src/title/`):

- `xba_layers.c` defines the two hooks `main.c` calls (`xba_title_configure` before the memory layout,
  `xba_title_start` right before the guest's entry point): configure sets the merged `env_default`s, then calls each
  layer's `p_configure`, lowest first; start calls each `p_start`.
- `xba_params.h`: the merged `params`.
- `env_force.h` when the stack forces options (Android: `pack.json` `env_force` instead).

`pack.json` records the layers (`"layers"`) and, for a title the toolkit has enhancements for, `"title_layer"`
(`"on"` / `"off"`); the prepare stamps hash every layer's files: changing a layer, or the switch, re-lifts the title.

## The layers today

| Layer | Holds |
|---|---|
| `titles/4d5300d1` | Fable, optional: 16:9 scenes (`overrides.c` entry hooks on the camera and the cutscene bars, `title.c` `fable_aspect_configure`) and the front end at the display rate (`title.c`: its `Sleep(50)` shortened to 16 ms) |

Nothing else ships. Gone with the profile-free work: the XDK layers (WP3: the toolkit's defaults and the D3D
service), `engines/renderware` (WP4: the DMA walk is the default, pacing and frame generation are decided at run time,
[framegen.md](framegen.md#titles-locked-to-the-flip-rate)), both `seed_functions.json` (WP5: packed-table discovery),
Fable's `[CACHE]` thread (WP7: the runtime's « preparing data » indicator), Fable's D3D KickOff wait override
`sub_008556D0` (WP6: the lifter's poll-wait mark does it for every XDK D3D) and Burnout 3's title layer (WP6: a name
and a state only; its history is [burnout3-notes.md](burnout3-notes.md)).

## The D3D service

D3D is linked into each title, so its statics move from XBE to XBE. The service finds the device at run time
(XBA-80): once D3D has made it, the title's image is scanned for the one dword `D` with `[D + 0x24A8] == D` (the
pointer slot, XDK 5849), a pushbuffer ring at `[D + 0x24]..[D + 0x28]` in the contiguous window, and the put pointer
`[D]` inside it. When nothing passes that slot rule (XDK 5455, KOTOR: the slot is `D - 8`), any image dword naming a
`D` that passes the ring and put tests, has its GPU semaphore pointer `[D + 0x30]` in the contiguous window and holds
D3D's vblank/fence KEVENT pair is taken, if there is only one. The fence event's offset is read from the device: the
one pair of KEVENT headers 16 bytes apart (5849 +0x1DCC, 5455 +0x19AC; 5849's is assumed when there is none). A unique
hit is printed (`[D3D] device found at ... fence event +0x...`) and written to the discovery file as
`{"t":"d3d",...}` with the slot actually found; then the semaphore watch and every serviceable software method run on
it. Title-built KEVENTs are guest events by default, so `BlockOnTime` sleeps on the fence event until method 5 sets
it. Method 5 is serviced whenever the device is known, also with `RECOMP_D3D_SW_METHODS` (with the addresses given its
offset is read at the first method 5): left unserviced, that blocking wait never ends (Fable froze in Oakvale that
way). With `RECOMP_TITLE_KEVENTS=0`, the DMA walk still adopts that one event (`xbox_KeAdoptGuestEvent`,
`[D3D] fence event ... set by software method 5`). So a new title on a known XDK build needs no title layer for D3D.

The addresses Fable's and Burnout 3's title layers used to give (`XBA_D3D_DEVICE_PTR`, `XBA_D3D_DEVICE`,
`XBA_D3D_SW_METHODS`) are what the search finds; for an A/B they are run-time overrides now:
`RECOMP_D3D_DEVICE_PTR=0x...`, `RECOMP_D3D_DEVICE=0x...`, `RECOMP_D3D_SW_METHODS=0x...`
([environment-variables.md](../runtime/environment-variables.md#d3d-service)). With an address given, RunPushBuffer's
methods write as D3D's handler does, without the run-time guards. With the device found, those guards check each
write against the node D3D built, not against where the title keeps its data: the return slot in the contiguous
window, the fixup list anywhere in mapped guest memory (bounded: 64 KB an entry, 1 MB a list), each fixup inside the
node's own pushbuffer. A refusal is printed and counted (`[GPU] software methods: ...; fixup lists refused N`):
[pushbuffer-executor.md](pushbuffer-executor.md#d3d-software-methods).

**Other XDK builds (XBA-105).** What cannot be read from the device's structure is looked up in one table in
`recomp_d3d_service.c` (`k_layouts`), keyed by the title's D3D library and build from its XBE header: the pointer
slot's offset, the fence event's offset when the device holds no KEVENT pair, RunPushBuffer's pending count and node
table. Known: D3D8LTCG 5849 (Burnout 3, Black, Fable: slot +0x24A8, fence event +0x1DCC, RunPushBuffer +0x50/+0x264)
and D3D8 5455 (KOTOR: slot -8, +0x19AC). A build the table does not know uses the fallback's value for each such rule
(D3D8LTCG 5849's) and says it once per rule — `[D3D] unknown XDK build 5933 (D3D8LTCG): fence event (no KEVENT pair in
the device) fell back to +0x1DCC (...)` — with a `{"t":"d3d_layout","lib","build","rule","used"}` line in the
discovery file; on such a build a found device's pending count is decremented only when it reads as a count (1-4096).
A run of a title on a new build therefore says what to add to the table. `tests/recomp_d3d_service` builds the service
once per environment: 5849 LTCG found at run time (the walk by default, the fence event adopted), the addresses given
(on D3D8 5849, a build the table leaves out), `RECOMP_PB_WALK=scan`, `RECOMP_D3D_SERVICE=0`, 5455, an unknown LTCG 5933
on 5849's slot rule without an event pair, an unknown D3D8 4627 with its slot and events elsewhere (found by
structure, nothing to fall back on but the ring offsets). What only real titles on other builds can confirm: that the
ring, put and semaphore offsets (+0x24/+0x28/+0/+0x30) hold on every build (they are what finds the device: a build
where they moved is not found at all, and needs `RECOMP_D3D_DEVICE_PTR`); the fence-event pair shape; RunPushBuffer's
offsets beyond 5849; and that the walk suits every D3D8 title (KOTOR is the one tested).

## Adding a title

Nothing to add: `python -m tools.prepare SOURCE` with no layer. The toolkit's defaults, the analysis and the
discovery loop (Prepare again after a run that found code) are the path. When a title does not run, the fix goes to
the toolkit (the runtime, the lifter, the analysis), found by its general rule, never to a title layer.

A title layer is only for an optional enhancement of one game: `profiles/titles/<title id>/` with `title.json`
(`"enhancements"`, `"hooks"`, `"status"`), its `.c` files and a `NOTES.md`. Check that the title still runs with it
left out (`tools.compat run ... --no-title-layer`) and that `python3 tools/ci/layer_guard.py` passes.

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
