# macOS launcher

XRecomp for the Mac: a native SwiftUI app in [`macos/`](../../macos/) that lists the games in your library, prepares
them with the desktop pipeline (`python -m tools.prepare`) and starts the built title, with per-title settings, saves
and storage. It is a front end to the toolkit, not a copy of it: the app drives the checkout it sits in.

- **Build**: `bash macos/scripts/build-app.sh`, then open `macos/build/XRecomp.app` (Finder or `open`).
- **Checks**: `bash macos/scripts/check.sh` (build, `xrecomp-check`, a smoke run). Command Line Tools are enough; no
  Xcode project.
- **Look without a toolkit**: `swift run --package-path macos XRecomp --demo [screen]`.
- **Mac checklist** for a real test: [at the end](#mac-checklist).

## Contents

- [Features](#features)
- [Building and checking](#building-and-checking)
- [Layout of the code](#layout-of-the-code)
- [Toolkit, Python and PATH](#toolkit-python-and-path)
- [Storage](#storage)
- [Prepare, play, stop](#prepare-play-stop)
- [Per-title settings](#per-title-settings)
- [Look](#look)
- [Licence, legal, CI](#licence-legal-ci)
- [Prepare interface](#prepare-interface)
- [Mac checklist](#mac-checklist)

## Features

The Android launcher's screens ([app-shell.md](../android/app-shell.md)), as a Mac window: a sidebar (Library, the
games, Setup, Default settings, About) and the detail pane.

| Screen | What it does |
|---|---|
| Library | A grid of cards: the title image (decoded from the game's own `default.xbe`) or a monogram, the name, one status line with a dot. Right-click: Settings, Manage, Show source in Finder. Drop an `.iso` or a game folder on the window, or use **Add game…**: its folder joins the library. |
| Home | The blurred title image behind, the title, one status line, a solid **Play** / **Prepare** / **Stop** and three quiet pills: Settings, Controls, Manage. Below: the prepare's progress or failure, a run's problem and log tail, the facts (title ID, source, size, play time, options the title forces). |
| Preparing | Step *i* of 5 with its name, the overall bar, five step dots, the step's message (`compiling 12/345`), elapsed time, Show log. One prepare at a time, the others queued ("Queued (2)"). Cancel stops the whole process group; the next Prepare resumes. |
| Prepare failed | The error code in words, prepare's message, the last 40 lines of `prepare.log`, Reveal log in Finder, Copy log. |
| Settings | Per title, four tabs (General, Graphics, Audio, Advanced) over the [option table](#per-title-settings), saved to the title's `recomp.cfg` at once; Advanced has a free editor for your own lines. **Default settings** (sidebar) is the same for every title. |
| Controls | The pad mapping the game gets (L1 = Black, R1 = White, View = Back) and the connected controller's name (GameController). |
| Manage | Per title: the size of the work folder, the extracted disc, saves, the Z: cache, logs and the title image, each deletable after a confirmation and revealable in Finder; "Re-prepare…" (`--force`); "Import saves from command-line runs". |
| Log | The live output of the running game (or prepare), copyable; run logs are kept in `logs/`. |
| Setup | The toolkit found (and from where), every dependency with the command that installs it, the Python environment (with **Create it**), the library folders, the work folder location. Shown first when the toolkit is not found. |
| About | Version, the toolkit's commit, "No game data included", the licence texts read from the toolkit (`LICENSE`, `NOTICE`, `LICENSES/*.txt`). |

Not in v1: the in-game menu, dual screen and the companion (the game is a separate SDL window; the launcher has
Stop), frame generation, gamepad navigation of the launcher itself, live fps.

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
  `preparing`, `failed`, `running`, `settings`, `controls`, `manage`, `log`, `setup`, `about`. The titles are
  invented and their art is drawn in code. In demo mode (or with `XRECOMP_PRINT_WINDOW=1`) the app prints
  `XRECOMP_WINDOW <id>` for `screencapture -l <id>`.
- **`XRECOMP_HOME=DIR`** moves the whole Application Support tree (tests, a second library).
- **Packaging**: `build-app.sh` runs `swift build -c release`, writes `Contents/Info.plist` from `macos/Info.plist.in`
  (bundle id `fr.adserver.xboxrecomp.mac`, macOS 14, `XBAToolkitPath` = this checkout), renders `AppIcon.icns` from
  [`docs/images/xrecomp-icon.svg`](../images/xrecomp-icon.svg) (`scripts/mkicon.swift` + `iconutil`) and signs ad hoc
  (`codesign -s -`). Built and run on the same Mac it needs no Developer ID or notarization. The app is not
  sandboxed: it spawns compilers and reads your folders.

## Layout of the code

```
macos/
  Package.swift              swift-tools 5.9, macOS 14; no third-party packages
  Sources/XRecompCore/       all logic, Foundation (+ CoreGraphics/ImageIO for art), no SwiftUI
    Paths Settings Toolkit PythonEnv ChildEnvironment DependencyCheck      setup
    PrepareProtocol PrepareRunner ProcessGroup TitleState                  prepare
    LibraryScanner LibraryIndex XbeInfo TitleArt Xpr Dxt Accent            library and art
    GameRunner RecompConfig OptionTable Storage PlayTime SaveImport        play and storage
  Sources/XRecomp/           the SwiftUI app: main (--smoke/--demo), XRecompApp, AppStore, LauncherModel, views
  Sources/xrecomp-check/     the checks
  scripts/                   check.sh, build-app.sh, mkicon.swift
  Info.plist.in
```

The views see a `LauncherModel` (published state) and call a `LauncherActions` protocol; `AppStore` implements it
with Core, `PreviewStore` (`--demo`) with mock data. Swift ports of our own Java: `RecompConfig` (RecompConfig.java),
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
- **Dependencies** (Setup): CMake, Ninja (optional), Clang, Git, `pkg-config` finding `sdl2` (Homebrew's `sdl2-compat`
  is fine) and `epoxy`, and the Python packages. Each missing item shows its command; the app never runs `brew`.

## Storage

Everything the app writes is under `~/Library/Application Support/xboxrecomp/` (`$XRECOMP_HOME`):

```
settings.json           toolkit path, library folders, work root, Python, global defaults
library.json            scan cache: source path, size, mtime -> title ID, name, default.xbe
scan/                   default.xbe files identify pulled out of disc images (for their art)
venv/                   only when the toolkit has no usable .venv
defaults/recomp.cfg     Default settings
titles/<TID>/
  work/                 prepare --work: analysis, project, build, stamp.json, discovered.jsonl, prepare.log,
                        launcher-prepare.log (prepare's stdout as the app saw it); game/ = the extracted disc
  recomp.cfg            the title's settings (the Android file format)
  save/                 RECOMP_SAVE_DIR: UDATA/<TID>, TDATA/<TID>, Cache/<TID> (Z:)
  art.png               the decoded title image
  logs/run-*.log        the last 10 runs
  state.json            source path, last played, play time, runs, last prepare result
```

- **Work folders can move** (Setup › Work folders) to another disk: an ISO source is extracted into `work/game`,
  several GB per title. A folder source is only linked (`project/build/game` points at your folder).
- **Deletes never follow a symbolic link**: deleting a work folder removes the link to your game folder, never the
  folder. Your ISOs and folders are only read.
- **Saves are per title** (`RECOMP_SAVE_DIR`), so "Delete saves" is one folder. The partition images and the GL
  shader cache stay in the runtime's default save dir (`~/.local/share/xboxrecomp`). Saves made by command-line runs
  live there too (`UDATA/<TID>`): Manage › Import copies them in once, overwriting nothing.

## Prepare, play, stop

- **Scan**: every library folder (default `~/Documents/xbox`), 3 levels deep, by content: a folder holding
  `default.xbe` (any case) or an `.iso`/`.xiso`. Images are identified with `python -m tools.prepare.identify`
  (cached in `library.json` by path, size and mtime); folders are read in Swift. A partial ISO (still being copied)
  is listed as "Not readable yet" and retried at the next scan. One entry per title ID: the first found wins, the
  others are listed under "Also at".
- **State**: `prepare --check --json` per title after each scan, prepare and run: Needs preparing (absent), Ready,
  Prepare again (stale: toolkit updated, profile changed, improvements found while playing, ...), or an error. A
  stale title can still be played.
- **Prepare**: `python -u -m tools.prepare SOURCE --work titles/<TID>/work --progress` from the toolkit, in its own
  process group; the [Prepare interface](#prepare-interface) lines drive the screen. Before a first prepare the free
  space is checked against an estimate (the image's size + 2 GB). **Cancel**: SIGTERM to the group, SIGKILL after
  5 s.
- **Play**: `work/project/build/<name>` (from `XBA_RESULT ok exe=` or `--check`), cwd its build folder, with the
  environment: the fixed PATH, then Default settings, then the title's `recomp.cfg` lines (first line for a key
  wins), then `RECOMP_SAVE_DIR`. The desktop `main.c` does not read `recomp.cfg`; the app passes it as environment.
  Output goes to `logs/run-<time>.log` and the Log screen. One game at a time.
- **Stop**: SIGTERM by PID, SIGKILL after 5 s, never by name. A non-zero exit or a signal shows "Stopped unexpectedly
  (signal N)" with the log's tail. Quitting the app stops a running game and prepare.

## Per-title settings

`recomp.cfg`, as on Android: one `KEY=VALUE` per line, `#` comments, the first line for a key wins; the Settings rows
rewrite their key's first line in place, a default value removes the line, and lines you wrote yourself stay
(Advanced edits them; Reset to defaults keeps them).

| Tab | Option | Key | Choices |
|---|---|---|---|
| General | Language | `RECOMP_LANGUAGE` | System (the Mac's, when the disc has it), en, fr, de, es, it, pt, ja, ko, zh |
| General | Window | `RECOMP_FULLSCREEN` | Window, Fullscreen |
| General | Window size | `RECOMP_WINDOW_SCALE` | 2x (default), 1x, 3x, 4x |
| Graphics | Renderer | `RECOMP_GPU` | CPU (default), GPU (OpenGL 4.1, opt-in) |
| Graphics | Frame cap | `RECOMP_GPU_FPS_CAP` | Auto (`RECOMP_PACING=auto`: the game's rate, or a steady lower one), 30, 60, Off; GPU only |
| Graphics | Internal resolution | `RECOMP_GPU_SCALE` | 1x, 2x, 3x, Fit; GPU only |
| Graphics | Aspect ratio | `RECOMP_ASPECT` | Auto, 4:3, 16:9 |
| Graphics | VSync | `RECOMP_PRESENT_VSYNC` | On, Off |
| Graphics | Texture filtering | `RECOMP_TEX_BILINEAR`, `RECOMP_TEX_ANISO` | Sharp, Smooth, Anisotropic 8x / 16x |
| Audio | 3D audio (HRTF) | `RECOMP_APU_HRTF` | On, Off |
| Audio | Audio buffer | `RECOMP_AUDIO_BUF_SAMPLES` | Short, Normal, Long, Longest |
| Advanced | Raster threads | `RECOMP_RASTER_THREADS` | Auto, 1, 2, 4, 6, 8 |

Options a title's profile forces (`env_force`, compiled into the binary, they win) show "Set by the title" and are
locked. Frame generation, `RECOMP_BIG_CORES` and the Android shell switches are left out. Every key:
[environment-variables.md](../runtime/environment-variables.md).

## Look

The Android launcher's stock theme (`GamePalette.STOCK`, ours): background `#0C110F` to `#060807`, accent `#4FE38C`,
text `#F3EEE4`, dim `#A9AFA6`, warn `#F2BA52`, error `#FF6F61`; dark only, the system font, an 8-pt grid, capsule
buttons (a solid Play, quiet pills), rounded glass panels, the standard Mac window and sidebar. Each game gets an
accent from its title image (the dominant hue, lightened until the Play label keeps 4.5:1 and kept away from the
error red); `xrecomp-check` holds the tokens equal to the Java array and checks the contrast pairs. The full OKLCH
`GamePalette` port is later work.

## Licence, legal, CI

- The Swift code is MIT like the repo; there are no third-party Swift packages and nothing is bundled, so `NOTICE`
  and `LICENSES/` are unchanged. The only asset is the app icon, original art.
- Title images, extracted XBEs and discs, generated C and logs stay in Application Support; nothing of a game enters
  the repo or the app. The checks use synthetic bytes and `tools/conformance/test.xbe` (ours).
- Linux CI is unaffected: `macos/` is not referenced by the root `CMakeLists.txt` and holds no Python. There is no
  macOS runner; `check.sh` is the local gate.

## Prepare interface

The launcher drives the desktop prepare through three machine-readable entry points. They are the contract between
`tools/prepare/{__main__,progress,identify}.py` and the launcher's Swift code (`PrepareProtocol`, `TitleState`,
`LibraryScanner`): change them only together. All three run from the toolkit root (`cwd` = the checkout) with the
toolkit's Python, e.g. `python -u -m tools.prepare ...`. Without the new flags the desktop prepare prints exactly
what it printed before. Tests: `tools/prepare/test_desktop_progress.py`, `tools/prepare/test_identify.py`
(synthetic `tools/conformance/test.xbe` and a `tools/xiso/mkxiso.py` image only).

### 1. Identify a source: `python -m tools.prepare.identify SOURCE --json [--xbe-out FILE]`

For the library scan. `SOURCE` is what prepare takes: a `.iso`/`.xiso`, a folder holding `default.xbe`, or a
`default.xbe`. It reads only the XBE header and certificate; for an image only `default.xbe` is read, never the rest
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
| 5 | `build` | 0, 5 CMake configure, 5-90 from the build's `[i/n]` (Ninja) or `[ nn%]` (make) lines (`msg` = `compiling 12/345`, at most one line per percent or 2 s), 90 built, 90-98 extracting the disc (image sources), 98 the disc's other images, 100 ready |

Error codes and exit status:

| Code | When |
|---|---|
| `NO_XBE` | step 1: no `default.xbe`, or the image is unreadable |
| `BAD_XBE` | step 1: `xbe_parser` failed |
| `BAD_PROFILE` | step 1: a profile layer is not well formed |
| `ANALYSE_FAILED` | step 2: disasm, func_id or abi_analysis failed |
| `LIFT_FAILED` | step 4: `tools.recomp` failed |
| `COMPILE_FAILED` | step 5: CMake configure or build failed, or another image's prepare |
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
   `# RECOMP_GPU=1` and `# RECOMP_GPU_FPS_CAP=30`. Add a line in Advanced, Reset to defaults: the line stays.
8. Save in game, quit, play again: the save is there. Manage shows the sizes; Delete saves removes only that title's.
9. Manage on a title prepared from a **folder** source (`~/Downloads/xboxrecomp/titles/black/game`): delete its work
   folder; the original folder is untouched.
10. `git status` in the checkout shows nothing new besides code. Send screenshots of Library, Home, Preparing and
    Settings, and `~/Library/Application Support/xboxrecomp/titles/<TID>/work/prepare.log` if something failed.
