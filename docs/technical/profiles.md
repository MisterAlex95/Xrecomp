# Profiles in layers

> Part of the [Documentation Index](../INDEX.md)

A title's switches and fixes beyond the generic template live in `profiles/`, in layers. `tools.prepare` picks the
layers from the XBE, merges them and puts them into the title project (`src/title/`), on the desktop and on the
device alike (the tree ships in the app's `toolkit.zip`). Code: `tools/prepare/profiles.py`; spec: Plane XBA « Spec ·
Universal titles », section 3.

## The layers, lowest first

| Layer | Folder | Picked when |
|---|---|---|
| Toolkit | none: `src/` (runtime) and `tools/` (lifter) | always |
| XDK library | `profiles/xdk/<name>/layer.json` | the XBE's library table has one of its `applies.libraries`, at a build in `applies.builds` (no `builds`: any) |
| Engine | `profiles/engines/<name>/layer.json` | `tools.xbe_ident` finds the engine whose `layer` (in `tools/xbe_ident/engines.json`) is `<name>` |
| Title | `profiles/titles/<title id>/title.json` | the title ID matches |

A title's `"layers"` lists the XDK and engine layers it was proven with: they are picked even when the XBE does not
select them, and prepare prints `asked for by the title profile, not selected by the XBE` (a sign `applies` or the
detection needs a fix). `python -m tools.xbe_ident default.xbe` prints the layers an XBE selects.

**A fix goes to the lowest layer where it is true.** What every XDK title needs because the console works that way
goes to an XDK layer (or the toolkit); what one title's own code needs stays with the title.

## What a layer holds

Every key is optional. The same keys work in every layer; a higher layer's value wins.

| Key | What |
|---|---|
| `name` | what it is |
| `applies` | XDK layers: `{"libraries": ["D3D8", "D3D8LTCG"], "builds": [5558, 5849]}` |
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
| `xdk/d3d8ltcg` (D3D8LTCG 5849) | `XBA_D3D_DMA_WALK` 1: the DMA walk, which D3D8LTCG's fence wait needs (DMA_GET and the 0x0310 latch); with it `d3d8.c` adopts the fence event (device + 0x1DCC) once it finds the device at run time | Black, Burnout 3 |
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
until method 5 sets it; with `RECOMP_TITLE_KEVENTS=0`, the DMA walk (`xdk/d3d8ltcg`, or `RECOMP_PB_WALK=dma`) still
adopts that one event (`xbox_KeAdoptGuestEvent`, `[D3D] fence event ... set by software method 5`). So a new
title on a known XDK build needs no title layer for D3D.

## Adding a title

1. `python -m tools.prepare` with no title layer: the XDK layers apply on their own.
2. A fix that one title needs: `profiles/titles/<title id>/` (`title.json`, `title.c` with `hooks`, `NOTES.md`).
3. Once a second title needs the same fix, move it down to the XDK or engine layer and make the title-specific part
   (addresses) a `param`.
