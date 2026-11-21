# macOS launcher

XRecomp for the Mac: a native SwiftUI app in [`macos/`](../../macos/) that lists the games in your library, prepares
them with the desktop pipeline (`python -m tools.prepare`) and starts the built title, with per-title settings, saves
and storage. It is a front end to the toolkit, not a copy of it: the app drives the checkout it sits in. Since XBA-278
it lists and plays Game Boy Advance, PlayStation, Nintendo 64 and GameCube games too ([Console games](#console-games)).


- **Build**: `bash macos/scripts/build-app.sh`, then open `macos/build/XRecomp.app` (Finder or `open`).
- **Checks**: `bash macos/scripts/check.sh` (build, `xrecomp-check`, a smoke run). Command Line Tools are enough; no
  Xcode project.
- **Look without a toolkit**: `swift run --package-path macos XRecomp --demo [screen]`.
- **Mac checklist** for a real test: [at the end](#mac-checklist).

## Contents

- [Install](#install)
- [Features](#features)
- [Building and checking](#building-and-checking)
- [Layout of the code](#layout-of-the-code)
- [Toolkit, Python and PATH](#toolkit-python-and-path)
- [Storage](#storage)
- [Prepare, play, stop](#prepare-play-stop)
- [Console games](#console-games)
- [Per-title settings](#per-title-settings)
- [Look](#look)
- [Game apps](#game-apps)
- [Send report](#send-report)
- [Licence, legal, CI](#licence-legal-ci)
- [Prepare interface](#prepare-interface)
- [Mac checklist](#mac-checklist)

## Install

macOS 14 or later. Download `xrecomp-v<version>-macos-arm64.zip` (Apple silicon) from the
[latest release](https://github.com/MisterAlex95/Xrecomp/releases/latest) and unzip it; on an Intel Mac, build it
(below). The app is not notarized, so clear the quarantine flag first ([why](#building-and-checking)):

```bash
xattr -dr com.apple.quarantine XRecomp
open XRecomp/XRecomp.app
```

Keep `XRecomp.app` in its `XRecomp` folder: it drives the toolkit around it. On first start it sets up its own Python
environment (Setup › **Create it**), then prepares and plays your library. To build it from a checkout instead:
`bash macos/scripts/build-app.sh` ([Building and checking](#building-and-checking)).

## Features

The Android launcher's screens ([app-shell.md](../android/app-shell.md)), as a Mac window: a sidebar (Library, the
games, Setup, Default settings, About) and the detail pane.

| Screen | What it does |
|---|---|
| Library | A grid of cards: the title image (decoded from the game's own `default.xbe`) or a monogram, the name, one status line with a dot. Right-click: Settings, Manage, Show source in Finder. Drop a disc image (`.iso`, `.xiso`, `.cso`, `.zso`, `.cci`) or a game folder on the window, or use **Add game…**: its folder joins the library. |
| Home | The blurred title image behind, the title, one status line, a solid **Play** / **Prepare** / **Stop** and three quiet pills: Settings, Controls, Manage. Below: the prepare's progress or failure, a run's problem and log tail (with **Send report**), the facts (title ID, source, size, play time, options the title forces). |
| Preparing | Step *i* of 5 with its name, the overall bar, five step dots, the step's message (`compiling 12/345`), elapsed time, Show log. One prepare at a time, the others queued ("Queued (2)"). Cancel stops the whole process group; the next Prepare resumes. |
| Prepare failed | The error code in words, prepare's message, the last 40 lines of `prepare.log`, Reveal log in Finder, Copy log, **Send report** ([Send report](#send-report)). |
| Settings | Per title, four tabs (General, Graphics, Audio, Advanced) over the [option table](#per-title-settings), saved to the title's `recomp.cfg` at once: each row « As in Default settings (value) » or the title's own value, with a « This game » badge ([Settings per game](#settings-per-game)); Graphics also has [frame generation's Lossless.dll](#frame-generation); Advanced has a free editor for your own lines. **Default settings** (sidebar) is the same screen for every title. |
| Controls | The pad mapping the game gets (L1 = Black, R1 = White, View = Back) and the connected controller's name (GameController). |
| Manage | Per title: the size of the work folder, saves, the Z: cache, logs and the title image (and an extracted disc, only in a work folder made before XBA-126), each deletable after a confirmation and revealable in Finder; "Re-prepare…" (`--force`); "Import saves from command-line runs"; for a title with mods turned on (`--check`'s `title_layer`; mods: [mods.md](../technical/mods.md), store `<storage root>/mods`), an **Enhancements** switch (on by default; off, the title's prepares and checks run with `--no-title-layer` / `XBA_NO_TITLE_LAYER=1`, the mods left out, kept in `state.json` `"enhancements": false`, and a build made the other way shows « Prepare again (enhancements or title profile changed) »); **Create an app for this game** ([Game apps](#game-apps)). |
| Log | The live output of the running game (or prepare), copyable; run logs are kept in `logs/`. |
| Setup | The toolkit found (and from where), every dependency with the command that installs it, the Python environment (with **Create it**), the library folders, the work folder location. Shown first when the toolkit is not found. |
| About | Version, the toolkit's commit, "No game data included", **Send a report about the last game** ([Send report](#send-report)), the licence texts read from the toolkit (`LICENSE`, `NOTICE`, `LICENSES/*.txt`). |

Not in v1: the in-game menu, dual screen and the companion (the game is a separate SDL window; the launcher has
Stop), gamepad navigation of the launcher itself, live fps, the pad mapping editor (`RECOMP_PAD_MAP`,
`RECOMP_<C>_PAD_MAP`: Advanced lines), a save-state and cheat browser (the hotkeys and files do it), console cover
art downloads (a console game shows its monogram, or an `art.png` you put in its title folder).

## Building and checking

```
bash macos/scripts/check.sh          # swift build, swift run xrecomp-check, XRecomp --smoke on a temp library
bash macos/scripts/build-app.sh      # macos/build/XRecomp.app, ad-hoc signed
```

- **No Xcode.** The package builds with the Command Line Tools (Swift 6.x). XCTest and swift-testing are not
  available there, so the checks are an executable, `xrecomp-check`, with a small assert harness
  (`Sources/xrecomp-check/Harness.swift`); `swift run xrecomp-check [filter]` runs the checks whose name contains
  the filter. They build all their inputs (XBE headers, XPR0 textures, fake prepare scripts, `/bin/sh` games) in a
  temporary folder: no game file is ever read.
- **No `@State`.** In the Command Line Tools' SDK SwiftUI's `@State` is a macro whose plugin ships only with Xcode;
  the views use `@Local` (`Sources/XRecomp/Local.swift`), a property wrapper over `@StateObject` with the same use.
- **`XRecomp --smoke [--toolkit DIR] [--library DIR]... [--prepare [--no-build]]`** handles itself before the app
  starts: it resolves the toolkit and Python, scans, prints the library as JSON and exits (0 when the toolkit was
  found). With `--prepare` it runs the app's own prepare path (`PrepareRunner`, then `--check`) on every title found:
  the end-to-end test. A full desktop build of the synthetic `tools/conformance/test.xbe` does not link on macOS
  (`_xbe_entry_point`), so that one is tested with `--no-build`.
- **`XRecomp --smoke --play TID [--seconds N]`** starts a prepared title the way Play does (its `recomp.cfg`, Default
  settings, save folder, run log) and stops it after N seconds (TERM, then KILL, by PID). The environment is
  inherited, so a scripted run can add `RECOMP_HEADLESS=1 SDL_AUDIODRIVER=dummy RECOMP_STATUS=<file>` from the shell;
  the app itself never sets them.
- **`XRecomp --demo [screen]`** shows every screen from mock data, without a toolkit: `library`, `home`,
  `preparing`, `failed`, `running`, `settings`, `controls`, `manage`, `log`, `setup`, `about`, `single` (a game
  app on its game), `single-missing` (a game app whose game left the library). The titles are
  invented and their art is drawn in code. In demo mode (or with `XRECOMP_PRINT_WINDOW=1`) the app prints
  `XRECOMP_WINDOW <id>` for `screencapture -l <id>`.
- **`XRECOMP_HOME=DIR`** moves the whole Application Support tree (tests, a second library).
- **`XRECOMP_SINGLE_TITLE=TID`** runs any launcher (`swift run`, the bare binary) in the single-game mode of a
  [game app](#game-apps).
- **Packaging**: `build-app.sh` runs `swift build -c release`, writes `Contents/Info.plist` from `macos/Info.plist.in`
  (bundle id `fr.adserver.xrecomp.mac`, macOS 14, `XBAToolkitPath` = this checkout), renders `AppIcon.icns` from
  [`docs/images/xrecomp-icon.svg`](../images/xrecomp-icon.svg) (`scripts/mkicon.swift` + `iconutil`) and signs ad hoc
  (`codesign -s -`). Built and run on the same Mac it needs no Developer ID or notarization. The app is not
  sandboxed: it spawns compilers and reads your folders.
- **Published build**: GitHub's `.github/workflows/desktop.yml` runs `check.sh`, then `build-app.sh` with
  `XBA_APP_PORTABLE=1` (no `XBAToolkitPath`: the path of the runner means nothing on your Mac) and
  `XBA_APP_VERSION=<release>`, puts the app inside the toolkit folder (`tools/ci/package_toolkit.sh`, with
  `tools/VERSION` for the commit) and checks that it finds that folder by itself (`toolkit_source` `bundle`) before
  zipping it ([releases](../releases/README.md)). Downloaded, it carries the quarantine flag: without
  `xattr -dr com.apple.quarantine`, Gatekeeper blocks it and App Translocation would run it from a read-only copy
  elsewhere, where the folders above it hold no toolkit (Setup then asks for one).

## Layout of the code

```
macos/
  Package.swift              swift-tools 5.9, macOS 14; no third-party packages
  Sources/XRecompCore/       all logic, Foundation (+ CoreGraphics/ImageIO for art), no SwiftUI
    Paths Settings Toolkit PythonEnv ChildEnvironment DependencyCheck      setup
    PrepareProtocol PrepareRunner PrepareQueue ProcessGroup TitleState     prepare
    LibraryScanner LibraryIndex XbeInfo TitleArt Xpr Dxt Accent            library and art
    GameRunner RecompConfig OptionTable GameSettings Storage PlayTime      play, settings and storage
    SaveImport LsfgCache                                                    saves, frame generation's shader cache
    TitleApp                                                               game apps: names, Info.plist, icon, ICNS
    CrashReport                                                            Send report: scrub, envelope, queue
  Sources/XRecomp/           the SwiftUI app: main (--smoke/--demo), XRecompApp, AppStore, LauncherModel, views
  Sources/xrecomp-check/     the checks
  scripts/                   check.sh, build-app.sh, mkicon.swift
  Info.plist.in
```

The views see a `LauncherModel` (published state) and call a `LauncherActions` protocol; `AppStore` implements it
with Core, `PreviewStore` (`--demo`) with mock data. Swift ports of our own Java: `CrashReport` (CrashReport.java), `RecompConfig` (RecompConfig.java),
`TitleArt`/`Xpr`/`Dxt` (TitleArt.java), `PrepareProtocol` (PrepareProtocol.java and `tools/prepare/progress.py`); their
checks reuse the Java tests' cases, so change both sides together.

## Toolkit, Python and PATH

- **The toolkit is the checkout, side by side, not bundled**: prepare needs the whole tree (tools, templates,
  profiles, `src/`) and stamps titles with its content digests (XBA-134: `toolkit_sha256`, the digest the Android packs
  carry, for the lift; `runtime_sha256` over `src/`, `include/`, `cmake/` for the build; the commit is recorded, not
  compared). It is found, in order, from the Setup choice,
  `$XBA_TOOLKIT`, the `XBAToolkitPath` that `build-app.sh` wrote into `Info.plist`, then the folders above the
  executable. A folder counts when it holds `tools/prepare/__main__.py` and `templates/new-game/`.
- **Python**: the Setup choice, else `<toolkit>/.venv/bin/python3` (`tools/macos/setup.sh` makes one with Python 3.10
  or later), else the app's own venv in Application Support, the first that imports `capstone`, `pefile` and
  `numpy`. Setup's **Create it** makes that venv from the first Python 3.10+ found (Homebrew's `python3.1x` first;
  macOS's `/usr/bin/python3` is 3.9) and pip-installs the three packages.
- **PATH**: an app opened from Finder gets launchd's PATH, without Homebrew. Every child (prepare, the checks, the
  game) gets `/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin` first, then the
  inherited entries; otherwise prepare fails at `cmake` only when started from Finder.
- **Dependencies** (Setup): CMake, Ninja (optional), Clang, Git, `pkg-config` finding `sdl2` (Homebrew's
  `sdl2-compat` is fine) and `epoxy`, and the Python packages. Each missing item shows its command; the app never
  runs `brew`. CMake is required only for a toolkit without a prebuilt runtime (a checkout): when the toolkit has
  `runtime/macos-arm64/runtime.json` (the release zip, XBA-86), prepare builds titles with the compile driver and
  Clang alone (`tools/prepare/desktop_build.py`) and Setup lists CMake as optional (XBA-253). Setup only checks that
  the file is there; prepare still falls back to CMake when the runtime was built from another toolkit tree
  (`runtime_sha256`), which a release zip never is.

## Storage

Everything the app writes is under `~/Library/Application Support/xboxrecomp/` (`$XRECOMP_HOME`):

```
settings.json           toolkit path, library folders, work root, Python, global defaults
library.json            scan cache: source path, size, mtime -> title ID, name, default.xbe
scan/                   default.xbe files identify pulled out of disc images (for their art)
venv/                   only when the toolkit has no usable .venv
defaults/recomp.cfg     Default settings
lsfg-tool/              tools/lsfg built by « Choose Lossless.dll » (lsfg_translate), when the toolkit has no build/lsfg
titles/<TID>/
  work/                 prepare --work: analysis, project, build, stamp.json, discovered.jsonl, prepare.log,
                        launcher-prepare.log (prepare's stdout as the app saw it); an image is read in place,
                        so game/ (an extracted disc) is only in work folders made before XBA-126
  recomp.cfg            the title's settings (the Android file format)
  save/                 RECOMP_SAVE_DIR: UDATA/<tid>, TDATA/<tid>, Cache/<tid> (Z:), the title ID in lower case
                        as the runtime names them (technical/saves.md)
  art.png               the decoded title image
  logs/run-*.log        the last 10 runs
  state.json            source path, last played, play time, runs, last prepare result, enhancements; the keys the
                        xrecomp command line adds (title, console) are kept when the app saves
```

- **Work folders can move** (Setup › Work folders) to another disk: about 2 GB per title of analysis, generated C
  and build. A disc image is read in place (XBA-126: `project/build/game` points at the image, the runtime mounts
  it) and a folder source is only linked (`project/build/game` points at your folder): neither is copied.
- **Deletes never follow a symbolic link**: deleting a work folder removes the link to your game folder, never the
  folder. Your ISOs and folders are only read.
- **Frame generation's shader cache** is not under Application Support: it is the runtime's desktop default,
  `~/Library/Caches/xboxrecomp/lsfg` (`$RECOMP_LSFG_CACHE`), so a command-line run finds it too.
- **Saves are per title** (`RECOMP_SAVE_DIR`), so "Delete saves" is one folder. The partition images and the GL
  shader cache stay in the runtime's default save dir (`~/.local/share/xboxrecomp`). Saves made by command-line runs
  live there too (`UDATA/<tid>`): Manage › Import copies them in once, overwriting nothing.

## Prepare, play, stop

- **Scan**: every library folder (default `~/Documents/xbox`), 3 levels deep, by content: a folder holding
  `default.xbe` (any case) or a disc image the pipeline reads (`.iso`, `.xiso`, `.cso`, `.ciso`, `.zso`, `.cci`, or
  a split image's first part, `Game.iso.1`). Images are identified with `python -m tools.prepare.identify`
  (cached in `library.json` by path, size and mtime); folders are read in Swift. A partial ISO (still being copied)
  is listed as "Not readable yet" and retried at the next scan. One entry per title ID: the first found wins, the
  others are listed under "Also at".
- **State**: `prepare --check --json` per title after each scan, prepare and run: Needs preparing (absent), Ready,
  Prepare again (stale: toolkit updated, profile changed, improvements found while playing, ...), or an error. A
  stale title can still be played.
- **Prepare**: `python -u -m tools.prepare SOURCE --work titles/<TID>/work --progress` from the toolkit, in its own
  process group; the [Prepare interface](#prepare-interface) lines drive the screen. Before a first prepare the free
  space is checked against an estimate of 2 GB (an image is read in place, so its size does not count). **Cancel**: SIGTERM to the group, SIGKILL after
  5 s.
- **Play**: `work/project/build/<name>` (from `XBA_RESULT ok exe=` or `--check`), cwd its build folder, with the
  environment: the fixed PATH, then Default settings, then the title's `recomp.cfg` lines (first line for a key
  wins), then `RECOMP_SAVE_DIR`. The desktop `main.c` does not read `recomp.cfg`; the app passes it as environment.
  Output goes to `logs/run-<time>.log` and the Log screen. One game at a time.
- **Stop**: SIGTERM by PID, SIGKILL after 5 s, never by name. A non-zero exit or a signal shows "Stopped unexpectedly
  (signal N)" with the log's tail. Quitting the app stops a running game and prepare.

## Console games

XBA-278 (Alex: « toutes les plateformes uniformes en termes de features »). The launcher lists, prepares and plays
GBA, PS1, N64 and GameCube games next to the Xbox titles, as the Android launcher does. It reads no ROM header, disc
format or `pack.json` itself: it asks the toolkit's desktop command line ([cli.md](../desktop/cli.md)), which tells
consoles apart the way `tools.prepare` does and shares this data folder, so the app and `xrecomp` never disagree
(`macos/Sources/XRecompCore/ConsoleGames.swift`).

- **Scan**: besides the Xbox images and folders, the files `.gba`, `.z64` / `.v64` / `.n64`, `.cue` / `.chd`,
  `.gcm` / `.gcz` / `.rvz` / `.wia` / `.dol`, a `.bin` / `.img` with no `.cue` next to it (a cue names its own
  tracks), and a `.iso` / `.ciso` that the Xbox identify does not read (a GameCube or PlayStation disc). They go in
  one batch to `python -m tools.xrecomp --home <data folder> identify SOURCE... --json` (console, game ID such as
  `gba-AXVE-1A2B3C4D`, title, how prepared, the player found); the answers are cached in `library.json` like the
  Xbox ones. The card has a console badge (GBA, PS1, N64, GC).
- **State**: the same `identify --json` after each scan, prepare and run. A console game plays before it is
  prepared: « Ready · interpreted » (its runtime interprets it), with a **Prepare** pill next to Play; prepared it is
  « Ready » (native), « Prepare again » when the toolkit changed or a run found code the lift missed. When its player
  (`gba_play`, `ps1_play`, `n64_play`, `gc_play`) is not built the status says so (build the toolkit, Setup).
- **Prepare**: `python -u -m tools.prepare SOURCE --work titles/<ID>/work --pack-dir <data folder>/packs
  --progress`, as `xrecomp prepare` runs it: the same protocol lines and Preparing screen; no 2 GB space check (a
  pack is a few MB) and no enhancements switch.
- **Play**: `python -m tools.xrecomp --home <data folder> play <ID> --print --json` gives the player's command line
  (the player found as `xrecomp` finds it, the source, `--pack` and `--missed` once prepared), its working folder
  (the toolkit, for the PS1 BIOS path) and environment (Default settings, the game's `recomp.cfg`,
  `RECOMP_SAVE_DIR=<data folder>/save`); the app starts it itself like an Xbox title (run log, Stop by PID) with the
  fixed PATH and frame generation's Vulkan library path.
- **Settings**: a game's Settings shows the rows its console reads (`ConfigOption.consoles`, the Android
  `GameSettings.appliesTo`): Rewind and Rewind memory, Controller rumble, Picture size and filter, Console resolution
  (PS1, N64, GameCube), the picture shader, VSync, frame generation, the audio buffer, the N64's controller pak.
  Default settings lists every row.
- **Controls**: the pad's buttons on the console's (`console_pad_defaults`), the hotkeys (fast-forward, rewind,
  quick save / load, slots, fullscreen, screenshot) and the keyboard.
- **Manage**: the work folder, the native pack (`packs/<ID>/`), the game's own saves, memory cards and states
  (`save/<ID>/`, never the shared `save/`), logs, the title image. No Xbox save import.
- **Game apps**: as for an Xbox title (the icon is the monogram unless the title folder has an `art.png`).

Folders: `titles/<ID>/` keeps a console game ID as it is (`gba-AXVE-1A2B3C4D`), as `xrecomp` names it; an Xbox title
ID stays upper case.

## Per-title settings

`recomp.cfg`, as on Android: one `KEY=VALUE` per line, `#` comments, the first line for a key wins; the Settings rows
rewrite their key's first line in place, and lines you wrote yourself stay (Advanced edits them; Reset keeps them).
On Default settings a row's default value removes the line.

| Tab | Option | Key | Choices |
|---|---|---|---|
| General | Language | `RECOMP_LANGUAGE` | System (the Mac's, when the disc has it), en, fr, de, es, it, pt, ja, ko, zh |
| General | Controller rumble | `RECOMP_RUMBLE` | On, Off |
| General | Window | `RECOMP_FULLSCREEN` | Window, Fullscreen |
| General | Window size | `RECOMP_WINDOW_SCALE` | 2x (default), 1x, 3x, 4x |
| Graphics | Renderer | `RECOMP_GPU` | CPU (default), GPU (OpenGL 4.1, opt-in) |
| Graphics | Frame cap | `RECOMP_GPU_FPS_CAP` | Auto (`RECOMP_PACING=auto`: the game's rate, or a steady lower one), 30, 60, Off; GPU only |
| Graphics | Internal resolution | `RECOMP_GPU_SCALE` | 1x, 2x, 3x, Fit; GPU only |
| Graphics | Aspect ratio | `RECOMP_ASPECT` | Auto, 4:3, 16:9 |
| Graphics | Widescreen | `RECOMP_WIDESCREEN` | Off, On (the console's setting: the game draws 16:9 frames) |
| Graphics | 480p | `RECOMP_480P` | Off, On |
| Graphics | VSync | `RECOMP_PRESENT_VSYNC` | On, Off |
| Graphics | Texture filtering | `RECOMP_TEX_BILINEAR`, `RECOMP_TEX_ANISO` | Sharp, Smooth, Anisotropic 8x / 16x |
| Graphics | Picture shader | `RECOMP_SHADER` | None, Sharp, Scanlines, CRT, FXAA, CAS (XBA-203; the LCD grid draws nothing on the Xbox picture) |
| Graphics | Shader strength | `RECOMP_SHADER_STRENGTH` | Default, 25, 50, 75, 100 % |
| Graphics | CRT curvature | `RECOMP_SHADER_CURVATURE` | Off, Slight (30), Strong (70); CRT only |
| Graphics | Frame generation | `RECOMP_LSFG`, `RECOMP_LSFG_MULT`, `RECOMP_LSFG_PERF` | Off, 2x, 2x performance ([below](#frame-generation)) |
| Audio | 3D audio (HRTF) | `RECOMP_APU_HRTF` | On, Off |
| Audio | Audio buffer | `RECOMP_AUDIO_BUF_SAMPLES` | Short, Normal, Long, Longest |
| Advanced | Raster threads | `RECOMP_RASTER_THREADS` | Auto, 1, 2, 4, 6, 8 |

Options a title's profile forces (`env_force`, compiled into the binary, they win) show "Set by the title" and are
locked. `RECOMP_BIG_CORES` and the Android shell switches are left out. Internal resolution stays
`RECOMP_GPU_SCALE` (the GL renderer's); the Xbox `RECOMP_UPSCALE` of XBA-237 gets its row once the runtime reads
it. Every key: [environment-variables.md](../runtime/environment-variables.md).

### Settings per game

The Android launcher's semantics (XBA-188, [app-shell.md](../android/app-shell.md#settings-per-game)), in the
title's own `recomp.cfg` over Default settings (`defaults/recomp.cfg`):

- a key the title's file does not set follows Default settings: the row shows « As in Default settings (value) »;
- `KEY=VALUE` is the title's own value (a « This game » badge on the row);
- `KEY=` (empty) is the runtime's default for this title even when Default settings set another: picking Off, Auto,
  1x... on a title writes it, and the game starts with the variable unset;
- an option of several keys (frame generation) is the title's own when the file sets any of them; the keys it does
  not set keep Default settings' values;
- **Reset to Default settings** removes the option lines and keeps your own.

`GameSettings` does it; `ChildEnvironment.game` unsets a `KEY=` variable (also one inherited or from Default settings).
A file written before keeps working: a line was already the title's own, a missing one already followed Default
settings.

### Frame generation

LSFG ([framegen.md](../technical/framegen.md)) is in every desktop build since XBA-187. It needs two things on the
Mac:

- **The shaders of your own Lossless.dll** (Lossless Scaling, Steam). Settings › Graphics › **Choose
  Lossless.dll…** runs the toolkit's `lsfg_translate` on it (`lsfg_translate -o <base> <dll>`, DXVK's DXBC compiler),
  which fills the runtime's desktop cache `~/Library/Caches/xboxrecomp/lsfg/<key>` and makes it current; the game
  finds it by itself (`RECOMP_LSFG_DIR` stays unset). The tool is the toolkit's `build/lsfg/lsfg_translate` when you
  built it by hand, else the app builds it once into `lsfg-tool/` under Application Support (`cmake -S tools/lsfg -B
  ... -DCMAKE_BUILD_TYPE=Release [-G Ninja]`, `cmake --build ... --target lsfg_translate`; the pinned DXVK archive
  comes from the toolkit's `third_party/lsfg-sources`, else CMake downloads it once). The panel shows the cache's
  state from its `manifest.txt` (LSFG 3.1 and 3.1 performance complete or not), the import's step, its error and its
  log. The DLL is only read; nothing of it leaves the Mac.
- **A Vulkan loader**: MoltenVK (`brew install molten-vk vulkan-loader`). Setup lists it as optional; Settings warns
  when none is found in `/opt/homebrew/lib`, `/usr/local/lib` or `~/lib`. The bridge dlopens it by name, and dyld's
  default fallback misses `/opt/homebrew/lib`, so a game started with `RECOMP_LSFG=1` gets
  `DYLD_FALLBACK_LIBRARY_PATH=~/lib:/opt/homebrew/lib:/usr/local/lib:/usr/lib` unless you set one yourself.

Then **Frame generation** (Off, 2x, 2x performance) on Default settings or per title. A title profile can still
force it off (« Set by the title »).

## Look

The Android launcher's stock theme (`GamePalette.STOCK`, ours): background `#0C110F` to `#060807`, accent `#4FE38C`,
text `#F3EEE4`, dim `#A9AFA6`, warn `#F2BA52`, error `#FF6F61`; dark only, the system font, an 8-pt grid, capsule
buttons (a solid Play, quiet pills), rounded glass panels, the standard Mac window and sidebar. Each game gets an
accent from its title image (the dominant hue, lightened until the Play label keeps 4.5:1 and kept away from the
error red); `xrecomp-check` holds the tokens equal to the Java array and checks the contrast pairs. The full OKLCH
`GamePalette` port is later work.

## Game apps

Manage › **Create an app for this game** makes `<Game title>.app` (a save panel, `~/Applications` by default, made
when missing): an app of its own for the Dock, Launchpad or Spotlight that opens straight on that game.
Only for a prepared game (playable, as on Android): before that the row says « Prepare this game first » and
**Create app…** is dimmed.

- **What it is**: a copy of the running `XRecomp.app` (`Bundle.main.bundleURL`; from `swift run` there is no bundle
  and the app says so) with a rewritten `Info.plist`: `CFBundleName` and `CFBundleDisplayName` = the game title,
  `CFBundleIdentifier` = `fr.adserver.xrecomp.mac.title.<title ID in lower case>`, `CFBundleIconFile` =
  `TitleIcon` (`CFBundleIconName` removed), and three keys of ours: `XBASingleTitle` = the title ID,
  `XBAMainAppPath` and `XBAMainBundleIdentifier` = the XRecomp.app it was made from. `XBAToolkitPath` is kept, or set
  to the toolkit the main app uses right now, so the copy still finds the checkout from outside it. The edit breaks
  the copied signature: the app re-signs it ad hoc (`/usr/bin/codesign --force --deep --sign - <app>`) and shows
  codesign's output if that fails. Launch Services is told of the new bundle (`LSRegisterURL`) and Finder shows it.
- **File name**: the title with « : » as « - », any other `/` or `:` as `-`, control characters and leading dots
  removed, at most 120 characters (`TitleApp.fileName`). Making it again at the same place replaces it; an existing
  app there that is not a game app of ours is never replaced (pick another name), and the app never writes over or
  inside XRecomp.app itself.
- **Icon**: the title image (`$$XTIMAGE` of the game's own `default.xbe`, the cached `art.png`) on a macOS-style
  rounded tile, rendered at 1024 px: the tile is 824 px on the standard icon grid with a soft shadow, its colour the
  average of the image's border (when at least half of the border is opaque) else a deep shade of the game's accent,
  and the image is scaled with high-quality interpolation to 80 % of the tile (title images are often 128 x 128, so
  expect a soft picture). Without a title image: the Library's monogram (the initials on the title ID's hue). The
  `.icns` is written directly (`TitleApp.icns`: « icns », the big-endian total length, then type + length + PNG per
  entry: ic10 1024, ic09 512, ic08 256, ic07 128, ic14 512, ic13 256, ic12 64, ic11 32) into
  `Contents/Resources/TitleIcon.icns`; no `iconutil`.
- **Single-game mode**: when `Info.plist` has `XBASingleTitle` (or `$XRECOMP_SINGLE_TITLE` is set) the launcher lists
  only that title and opens on its Home: no sidebar, no Library, no way back to it; Settings, Controls, Manage and Log
  are there, and Home's « … » menu has Setup, Default settings, About and **Open XRecomp (all games)**. There is no
  « Create an app » inside a game app. The menu bar shows the game's name and a « Game » menu (Home = ⌘1).
- **Same data**: nothing the launcher stores depends on the bundle ID or the app name: the whole tree is
  `~/Library/Application Support/xboxrecomp/` (`AppPaths.standard`), the app has no `UserDefaults` of its own, so a
  game app and XRecomp.app share settings, library, work folders, saves, play time and logs. Only window positions
  (kept by macOS per bundle ID) are separate. Do not prepare or play the same game from both at once: each app runs
  its own processes.
- **When the game leaves the library** (removed, its image moved, a disk not connected) the game app shows « This
  game is not in your library » with **Open XRecomp**, Scan again and Setup. There is nothing to clean up: the app
  only names a title ID. Delete it from Finder when you no longer want it.
- **Updates**: a game app is a copy of the launcher as it was when it was made; after rebuilding XRecomp.app, make the
  game apps again to get the new launcher (they keep working on the shared data meanwhile).
- **Legal**: a game app holds no game code or data, only our launcher, the title's name and ID and an icon drawn from
  the player's own disc at run time. It is fine to keep locally; like the title images, it is not shared or committed.

## Send report

XBA-342, the Android app's « Send report » ([app-shell.md](../android/app-shell.md), search « Rustrak ») on the Mac,
for feature uniformity ([feature-matrix.md](../runtime/feature-matrix.md)): the same Sentry envelope to the same
Rustrak project, the same scrubbing, the same fingerprint. Tap-only: nothing leaves without Send; no account, no
e-mail, no install id.

| Where | What |
|---|---|
| Prepare failed (Home) | **Send report** beside Reveal log and Copy log (a cancelled Prepare has no failure panel). |
| A run that ended badly (Home) | **Send report** beside Show log and Dismiss: that title's newest `logs/run-*.log`. |
| About | **Send a report about the last game**: the newest run log of any title. A game still running: dimmed, « quit it first ». |

The sheet « Send a report » says where it goes (the DSN's host) and shows the report in four lines: the problem; the
game, its ID and console; the Mac's model, macOS, the architecture and `xrecomp@<version>`; the log and its size.
**Send** (Return) posts it off the main thread, **Cancel** (Escape) sends nothing; then « Report sent. Thank you! » or
« Could not send the report: it will leave with the next one ».

`Sources/XRecompCore/CrashReport.swift` (a port of `CrashReport.java`): one envelope (an event and the log as a
`text/plain` attachment, trimmed to 256 KB: its first 32 KB and its end) posted with URLSession. The event: release
`xrecomp@<CFBundleShortVersionString>` (`dev` under `swift run`), environment `release` (an app bundle with a
version) or `debug`, tags `kind` (`prepare` / `game`), `console`, `game_key`, `game`, `device` (`sysctl hw.model`,
« Mac14,2 »), `os` (« macOS 15.1 »), `arch`, plus `prepare_code` or `end`. `end` comes from the run log's last line,
which GameRunner writes (« # ended: signal 11 after 12 s » → `signal 11 (SIGSEGV)`); the fingerprint groups by
console, game, code or end, and a backtrace's first frame when the log has one, never the duration. No breadcrumbs:
the Android app's come from its own event ring; the Mac host keeps none.

DSN: `$XBA_REPORT_DSN` when set (empty: no report buttons), else the Info.plist key `XBAReportDsn`, else the
project's server (the same public DSN as the APK's `xba_report_dsn`).

Scrubbing (the Android patterns, both sides in step with the same test vectors: `CrashReportTest.scrubsHomeFolders`
and `xrecomp-check`'s `report:` checks): the launcher's data folder keeps its names
(`<app:xboxrecomp>/titles/4D530004/…`), any other path in a home folder (`/Users/<name>/…`, `/home/<name>/…`,
`C:\Users\<name>\…`) keeps only its file name (`<path>/Halo.iso`), a bare home folder goes whole (`<path>`); URLs
lose host and login, magnet links, IP and e-mail addresses go; on the Mac the account's short and full names (4
characters or more) become `<user>` too.

Offline or refused (not 2xx): one retry, then the envelope waits in `<data folder>/reports/` (the newest 5) and leaves
after the next report that gets through.

Linux and Windows have no launcher (the `xrecomp` command line and the game window only): no reports there
([feature-matrix.md](../runtime/feature-matrix.md) « Crash reports »).

## Licence, legal, CI

- The Swift code is MIT like the repo; there are no third-party Swift packages and nothing is bundled, so `NOTICE`
  and `LICENSES/` are unchanged. The only asset is the app icon, original art.
- Title images, extracted XBEs and discs, generated C and logs stay in Application Support; nothing of a game enters
  the repo or the app. The one thing that leaves the Mac is a report the player chooses to send ([Send
  report](#send-report)): one scrubbed log, no game data. The checks use synthetic bytes and `tools/conformance/test.xbe` (ours).
- Linux CI is unaffected: `macos/` is not referenced by the root `CMakeLists.txt` and holds no Python.
- Mac CI: Gitea's `.gitea/workflows/macos.yml` builds the toolkit's C libraries with AppleClang on the Mac runner at
  every push to main (not the Swift package). The Swift package is built and checked by GitHub's
  `.github/workflows/desktop.yml` (`check.sh`, then `build-app.sh`) on pushes to main and release tags of the public
  mirror. `check.sh` on a Mac is the gate before a change lands.

## Prepare interface

The launcher drives the desktop prepare through three machine-readable entry points. They are the contract between
`tools/prepare/{__main__,progress,identify}.py` and the launcher's Swift code (`PrepareProtocol`, `TitleState`,
`LibraryScanner`): change them only together. All three run from the toolkit root (`cwd` = the checkout) with the
toolkit's Python, e.g. `python -u -m tools.prepare ...`. Without the new flags the desktop prepare prints exactly
what it printed before. Tests: `tools/prepare/test_desktop_progress.py`, `tools/prepare/test_identify.py`
(synthetic `tools/conformance/test.xbe` and a `tools/xiso/mkxiso.py` image only).

### 1. Identify a source: `python -m tools.prepare.identify SOURCE --json [--xbe-out FILE]`

For the library scan. `SOURCE` is what prepare takes: a disc image (`.iso`/`.xiso`/`.cso`/`.zso`/`.cci`, or a split
part), a folder holding `default.xbe`, or a `default.xbe`. It reads only the XBE header and certificate; for an image only `default.xbe` is read, never the rest
of the disc. `--xbe-out FILE` writes the source's `default.xbe` to `FILE` (temp file + rename, any source kind) so
the launcher can decode the `$$XTIMAGE` art from it. Nothing else is written. About 50 ms per source.

One JSON object on one line, exit 0:

```json
{"source": "/Users/me/Documents/xbox/Burnout 3 - Takedown (Europe)/Burnout 3 - Takedown (Europe) (Fr,De,It).iso",
 "kind": "image", "size": 2966814720, "title_id": "4541005B", "name": "Burnout 3",
 "profile_name": "Burnout 3: Takedown", "version": 3, "region": 4, "disc_number": 0,
 "xbe": "/Users/me/Library/Application Support/xboxrecomp/titles/4541005B/default.xbe", "xbe_size": 4308992,
 "has_title_image": true}
```

| Field | Type | Meaning |
|---|---|---|
| `source` | string | `SOURCE`, absolute. |
| `kind` | string | `image`, `folder` or `xbe`. |
| `size` | int | Bytes: the image file, or `default.xbe` for a folder or XBE source (a scan-cache key with the mtime). |
| `title_id` | string | 8 upper-case hex digits. |
| `name` | string | The certificate's title name (the title ID when it is empty). |
| `profile_name` | string or null | `name` of the title profile (`profiles/titles/<tid>/title.json`), when there is one. Show it in preference to `name`. |
| `version`, `region`, `disc_number` | int | From the certificate. |
| `xbe` | string or null | A readable `default.xbe`: `--xbe-out` when given, else the file in place; null for an image without `--xbe-out`. |
| `xbe_size` | int | Bytes of `default.xbe`. |
| `has_title_image` | bool | The XBE has a `$$XTIMAGE` section. |

Failure: exit 1 and `{"error": {"code": "...", "msg": "..."}}` on one line. Codes: `NO_XBE` (no such path, no
`default.xbe`, not a supported kind), `BAD_IMAGE` (not an XDVDFS image, or it ends inside `default.xbe`: an ISO still
being copied, retry on the next scan), `BAD_XBE` (`default.xbe` does not parse), `NO_SPACE`/`BAD_ARGS` (writing
`--xbe-out` failed).

### 2. Title state: `python -m tools.prepare SOURCE --work DIR --check --json`

Whether the title is prepared and up to date, without doing anything: it writes nothing, not even `DIR` (for an
image whose `DIR/xbe/default.xbe` does not exist yet it reads `default.xbe` into a temporary file and removes it).
Without `--work` the work folder is prepare's default, `<title id>-work` next to `SOURCE`. One JSON object on one
line, exit 0:

```json
{"state": "stale", "reasons": ["toolkit", "discovered"],
 "exe": "/Users/me/Library/Application Support/xboxrecomp/titles/4541005B/work/project/build/burnout3",
 "title": "Burnout 3: Takedown", "title_id": "4541005B", "env_force": {"RECOMP_LSFG": "0"},
 "work": "/Users/me/Library/Application Support/xboxrecomp/titles/4541005B/work"}
```

| Field | Type | Meaning |
|---|---|---|
| `state` | string | `absent` (no executable), `stale` (an executable, but the next prepare would redo something), `ready`. |
| `reasons` | [string] | Empty for `ready`. For `stale`, any of: `toolkit` (the toolkit tree's files changed: `tools/prepare/toolkit.py`'s content digest, so a commit that touches only docs, tests, Java or the runtime's `.c` files is not one; a stamp from before XBA-134 compares the commit), `runtime` (only the runtime the executable links changed: the next prepare keeps the lift and rebuilds, minutes), `profile` (the profile layers' files changed), `discovered` (runs found new addresses in `discovered.jsonl`: show "Improvements found while playing: prepare again"), `xbe` (another `default.xbe`), `not_built` (the lift is newer than the executable: a build failed or was cancelled), `no_stamp` (an executable without `stamp.json`). For `absent`: `["not_built"]` when a prepare started but has not built yet, else `[]`. |
| `exe` | string or null | The executable (`project(...)` of `DIR/project/CMakeLists.txt` in `DIR/project/build`), also when `stale`: the old binary can still be played. |
| `title` | string | The title profile's `name`, else the certificate's title name. |
| `title_id` | string | 8 upper-case hex digits. |
| `env_force` | {string: string} | The options the profile layers force (compiled into the binary, they win over the environment): show those settings as "set by the title". |
| `work` | string | The work folder looked at. |
| `title_layer` | string | Only for a title with mods turned on in the store (docs/technical/mods.md): `on` when they go in, `off` when `--no-title-layer` / `XBA_NO_TITLE_LAYER=1` leaves them out (docs/technical/profiles.md « Optional by construction »). The launcher offers the Enhancements switch for such a title; `reasons` holds `mods` when the store changed since the build. |

Failure: exit 1 and `{"state": "error", "code": "...", "msg": "..."}`: the identify codes above, or `BAD_PROFILE`
(a profile layer is not well formed). `--check` without `--json` prints one human line instead.

### 3. Prepare with progress: `python -u -m tools.prepare SOURCE --work DIR --progress`

The normal desktop prepare (five steps, each skipped when up to date), with protocol lines on stdout between the
human ones. Read stdout line by line: lines starting `XBA_PROGRESS ` or `XBA_RESULT ` drive the UI, every other line
goes to the log pane. Errors (the failed command's log tail) go to stderr. The grammar is the Android one
(`tools/prepare/progress.py`, `parse_line`; docs/android/prepare.md):

```
XBA_PROGRESS step=<i>/5 id=<step id> pct=<0-100> msg=<rest of line>
XBA_RESULT ok exe=<executable>
XBA_RESULT ok project=<project folder>         (only with --no-build)
XBA_RESULT error code=<UPPER_SNAKE> msg=<text>
```

`msg` is always last and takes the rest of the line (spaces and `=` included); other values never hold spaces; a
reader ignores keys it does not know (`eta=` may come later). `step` never goes back, `pct` is per step. Exactly one
`XBA_RESULT` line ends a run.

| Step | `id` | `pct` |
|---|---|---|
| 1 | `identify` | 0 finding `default.xbe`, 30 title read, 100 profile resolved (`msg` = title and ID) |
| 2 | `analyse` | 0, 2 xbe_parser, 5 disasm, 60 func_id, 80 abi_analysis, 100 done (or 100 "up to date") |
| 3 | `project` | 0, 100 |
| 4 | `lift` | 0, 100 (`msg` = C file counts, or "up to date") |
| 5 | `build` | 0, 5 CMake configure, 5-90 from the build's `[i/n]` (Ninja) or `[ nn%]` (make) lines (`msg` = `compiling 12/345`, at most one line per percent or 2 s); with the toolkit's prebuilt runtime (no CMake, `tools/prepare/desktop_build.py`) 5-90 from the compile driver's files done (`msg` = `compiling 12/345`); 90 built, 90-98 extracting the disc (only with `--extract`; by default an image is read in place), 98 the disc's other images, 100 ready |

Error codes and exit status:

| Code | When |
|---|---|
| `NO_XBE` | step 1: no `default.xbe`, or the image is unreadable |
| `BAD_XBE` | step 1: `xbe_parser` failed |
| `BAD_PROFILE` | step 1: a profile layer is not well formed |
| `ANALYSE_FAILED` | step 2: disasm, func_id or abi_analysis failed |
| `LIFT_FAILED` | step 4: `tools.recomp` failed |
| `COMPILE_FAILED` | step 5: CMake configure or build failed, the compile driver's compile or link failed (the message starts with `COMPILE_FAILED:`, `LINK_FAILED:`, `NO_COMPILER:` or `NO_BUILDER:`), or another image's prepare |
| `NO_SPACE` | ENOSPC anywhere |
| `CANCELLED` | SIGTERM or SIGINT: exit 130 |
| `BUSY` | another prepare holds the work folder (`<work>/.prepare.lock`); nothing was changed |
| `INTERNAL` | a bug (traceback on stderr), or step 3 failed |

Exit 0 after `ok`, 1 after an error, 130 after `CANCELLED`. A failed command's `msg` is
`<tool> failed (exit N), see <DIR>/prepare.log`.

**Cancel**: send SIGTERM to the prepare's process group (start it in its own group), SIGKILL after 5 s. On SIGTERM
prepare kills the command it is running, prints `XBA_RESULT error code=CANCELLED msg=cancelled (signal 15)` and exits
130. The stamp and the incremental CMake build make the next prepare resume where it stopped.

Example (synthetic `test.xbe`, human lines shortened):

```
[1/5] finding default.xbe
XBA_PROGRESS step=1/5 id=identify pct=0 msg=finding default.xbe
      SYNTH (title ID 12345678), work folder /tmp/w
XBA_PROGRESS step=1/5 id=identify pct=30 msg=SYNTH (title ID 12345678)
XBA_PROGRESS step=1/5 id=identify pct=100 msg=SYNTH (title ID 12345678)
[2/5] analysing the executable (a few minutes the first time)
XBA_PROGRESS step=2/5 id=analyse pct=0 msg=analysing the executable (a few minutes the first time)
...
[5/5] building for this computer
XBA_PROGRESS step=5/5 id=build pct=0 msg=building for this computer
XBA_PROGRESS step=5/5 id=build pct=5 msg=compiling
XBA_PROGRESS step=5/5 id=build pct=13 msg=compiling 10/104
...
XBA_PROGRESS step=5/5 id=build pct=100 msg=ready
XBA_RESULT ok exe=/tmp/w/project/build/title_12345678
```

## Mac checklist

For Alex, on the Mac (agents cannot click through Finder):

1. `bash macos/scripts/check.sh`. Expected: build OK, all checks pass, the smoke run prints JSON with title ID
   `12345678` and "all green".
2. `bash macos/scripts/build-app.sh`, then open `macos/build/XRecomp.app` **from Finder**, not from Terminal.
3. Setup: the toolkit is found by itself ("Found from the app (build-app.sh)"). Every dependency is green, or shows
   the exact command for what is missing. If no Python with the packages exists yet, press **Create it** (or run
   `tools/macos/setup.sh`), then Check again.
4. Library shows the games in `~/Documents/xbox` with names and title images (Black, Fable, Burnout 3, KOTOR). Add
   `~/Downloads/xboxrecomp/titles` in Setup: no second card for a title already there (the other copy is under
   "Also at" on Home).
5. Prepare Burnout 3. The step and percent advance, also during the build ("compiling i/n"). Press Cancel once
   mid-build, then Prepare again: it resumes without redoing the analysis.
6. When it is Ready, press Play: the game window opens and Play becomes Stop. Check the pad. Press Stop: the game
   closes within 5 s and Log keeps the output.
7. Settings › Graphics: Renderer GPU, Frame cap 30, then Play. Log (or `titles/<TID>/logs/run-*.log`) shows
   `# RECOMP_GPU=1` and `# RECOMP_GPU_FPS_CAP=30`. Add a line in Advanced, Reset to Default settings: the line stays.
8. Save in game, quit, play again: the save is there. Manage shows the sizes; Delete saves removes only that title's.
9. Manage on a title prepared from a **folder** source (`~/Downloads/xboxrecomp/titles/black/game`): delete its work
   folder; the original folder is untouched.
10. `git status` in the checkout shows nothing new besides code. Send screenshots of Library, Home, Preparing and
    Settings, and `~/Library/Application Support/xboxrecomp/titles/<TID>/work/prepare.log` if something failed.

Settings parity (XBA-196), with `macos/build/XRecomp.app` opened from Finder:

19. `swift run --package-path macos XRecomp --demo settings`: Graphics shows the new rows (Widescreen, 480p,
    Picture shader, Shader strength, CRT curvature, Frame generation), each with « As in Default settings (…) »
    first; Internal resolution, Aspect ratio, Picture shader and Frame generation carry « This game »; the
    Lossless.dll panel says « ready: LSFG 3.1 and 3.1 performance ». Send a screenshot.
20. Default settings › Graphics: Picture shader CRT. A title's Settings › Graphics: Picture shader reads « As in
    Default settings (CRT) » and CRT curvature is enabled. Pick None on the title: « This game » appears, its
    `recomp.cfg` has `RECOMP_SHADER=` (Advanced › Reveal recomp.cfg); Play: the run log has no `# RECOMP_SHADER`
    line and the picture has no CRT effect. Pick « As in Default settings »: the line is gone. Reset to Default
    settings clears every « This game ».
21. Setup lists « Vulkan (MoltenVK) for frame generation · optional », green after `brew install molten-vk
    vulkan-loader` and Check again.
22. Settings › Graphics › **Choose Lossless.dll…**, pick your Lossless.dll. First time: « Building the shader
    translator… » (a few minutes; `~/Library/Application Support/xboxrecomp/lsfg-tool/lsfg_translate` appears),
    then « Translating the shaders… », then « Lossless.dll <key>… ready: LSFG 3.1 and 3.1 performance ». Reveal
    cache opens `~/Library/Caches/xboxrecomp/lsfg/<key>`. Show log has the cmake and lsfg_translate output. Pick a
    file that is not the DLL: a red « lsfg_translate: … » line, the earlier cache still ready.
23. Frame generation 2x on a GPU title, Play: the run log shows `# RECOMP_LSFG=1`, `# RECOMP_LSFG_MULT=2`, and after
    the lock verdict (25-50 s) `[LSFG]` lines with « shown » about twice « real »; no `LSFG_E_NOLOADER` /
    « no shader » line. Then 2x performance: `# RECOMP_LSFG_PERF=1`. If the loader is not found, send the run log
    and `ls /opt/homebrew/lib | grep -i vulkan`.
24. Widescreen On + Aspect ratio 16:9 on a game with a widescreen option (Burnout 3): the game offers or uses 16:9.
    480p On, Controller rumble Off: `# RECOMP_480P=1`, `# RECOMP_RUMBLE=0` in the run log, no rumble.

Console games (XBA-278), with `macos/build/XRecomp.app` opened from Finder, after `cmake --build build --target
gba_play ps1_play n64_play gc_play` in the toolkit. Use homebrew or your own dumps, kept outside the checkout:

25. `bash macos/scripts/check.sh`: the `console:` checks pass, including « the real xrecomp identify and play
    request on a synthetic N64 ROM » (not « skipped »). The SwiftUI views were never compiled before this check:
    send the first compile error if there is one.
26. Put a `.gba`, a `.z64`, a PS1 `.cue` + `.bin`, and a GameCube `.iso` or `.rvz` in `~/Documents/xbox` (or drop
    them on the window): each gets a card with its badge (GBA, N64, PS1, GC) and its name, one card for the cue (not
    one per track); the Xbox titles are as before.
27. A console game's Home: « Ready · interpreted (Prepare runs it natively) », **Play** and a **Prepare** pill.
    Play: the game window opens, Stop closes it within 5 s, Log has the output and the run log's first line is
    `# .../gba_play <your ROM>`.
28. Prepare it: the Preparing screen advances through its five steps; then « Ready », and Play runs natively (the
    run log's first line has `--pack .../packs/<ID>/libtitle...`).
29. Settings of a GBA game: Rewind, Rewind memory, Picture size, Picture filter, Picture shader..., no Renderer /
    Widescreen / Language rows; an N64 game also has Controller pak and Console resolution. Set Picture filter
    Smooth, Play: the run log has `# RECOMP_FILTER=linear` and the picture is smooth.
30. In game: Tab fast-forwards, F1 saves a state, Shift+F1 loads it, hold R to rewind (N64 / GameCube with Rewind
    On); Option+Return toggles fullscreen (F11 too, but macOS keeps plain F11 for Show Desktop: Fn+F11 or change that
    shortcut), Fn+F12 writes a screenshot (the log says where).
31. Manage of a console game: Work folder, Native pack, Saves, Logs, Title image; no « Import saves ». Delete Native
    pack: the game is « Ready · interpreted » again. Create an app for it: the app opens on that game and plays it.
32. `xrecomp list` in Terminal shows the same games with the same states; a setting made with `xrecomp settings
    <ID> RECOMP_FILTER=nearest` shows in the app's Settings (« This game »).

Game apps ([Game apps](#game-apps)), with `macos/build/XRecomp.app` opened from Finder:

10b. A game not prepared yet › Manage: « Create an app for this game » says « Prepare this game first » and **Create
    app…** is dimmed.
11. `swift run --package-path macos XRecomp --demo manage`: the « Create an app for this game » row is last in More;
    `--demo single` opens straight on Sample Racer's Home with the « … » menu and no sidebar; `--demo
    single-missing` shows « This game is not in your library ».
12. Burnout 3 › Manage › **Create app…**: the save panel opens in `~/Applications` with « Burnout 3 - Takedown.app »
    (or the XBE's title). Create: no error, Finder shows the new app selected. `codesign --verify --deep --strict
    ~/Applications/<name>.app` prints nothing, and `plutil -p <app>/Contents/Info.plist` shows `XBASingleTitle`,
    the `...mac.title.<tid>` bundle ID and `CFBundleIconFile` `TitleIcon`.
13. Icon: Finder (icon view, Get Info) and the Dock (open it, Options › Keep in Dock) show the game's logo on a
    rounded tile, not XRecomp's icon, sharp enough at Dock size. Send a screenshot.
14. Double-click the game app: it opens straight on Burnout 3's Home (window title and menu bar name = the game, no
    sidebar, no back arrow, « … » menu instead). Play, Settings, Controls, Manage and Log work; Manage has no « Create
    an app ». « … » › Setup / Default settings / About each have a way back to the game.
15. Shared data: change a setting (Settings › Graphics) in the game app, quit it, open XRecomp: the setting is there,
    and the play time and saves made from the game app show in XRecomp (and the other way round).
16. Make an app for a title **without a title image** (or delete Manage › Title image first, then create): the icon
    is the monogram (initials on a coloured tile), not a blank or the XRecomp icon.
17. Make the same game's app again at the same place: it is replaced, no error. Try to save it as an existing
    non-XRecomp app name in `~/Applications`: refused with a clear message, that app untouched.
18. Remove the game from the library (take its folder out of the library folders, or rename its ISO), then open its
    game app: after the scan it says « This game is not in your library »; **Open XRecomp** opens the main app. Put
    the game back, Scan again: the game's Home comes back.

Send report ([Send report](#send-report), XBA-342), with `macos/build/XRecomp.app` opened from Finder:

33. `bash macos/scripts/check.sh`: the nine `report:` checks pass, and the app builds (the report sheet and buttons
    were written without a Mac: send the first compile error if there is one).
34. Crash a title (Stop is a clean end: kill it instead, `kill -SEGV <pid>` from Terminal). Home shows « Stopped
    unexpectedly (signal 11) » with **Send report**. Press it: the sheet shows four lines (« Game: signal 11 (SIGSEGV):
    after N s », the game and its ID, your Mac's model · macOS · arm64 · xrecomp@<version>, `+ run-….log (n KB)`).
    Cancel: nothing sent.
35. Again, Send: « Report sent. Thank you! ». On rustrak.adserver.fr › XRecomp, a new issue tagged `os` macOS,
    `device` your Mac's model, the run log attached: no `/Users/` and no account name in it, the data folder written
    `<app:xboxrecomp>/…`.
36. About › **Send a report about the last game** › Send: the same game's issue gets a second event (same
    fingerprint), not a new issue.
37. Make a Prepare fail (for example on a nearly full disk, or by killing its `cmake` / `ninja` from Terminal mid-build):
    the failed panel has **Send report**; Send: an issue « PrepareFailed » with `prepare_code` and `prepare.log`.
38. Wi-Fi off, send a report: « Could not send … », and `~/Library/Application Support/xboxrecomp/reports/` holds one
    `.envelope`. Wi-Fi on, send another: both arrive, the folder is empty.
