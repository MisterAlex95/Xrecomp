# xrecomp: the desktop command line

One entry point for a desktop toolkit (XBA-90; the decision in XBA-85: on Linux the command line first, the Mac keeps
its app). `xrecomp` prepares and plays your own games, Xbox and GBA / PS1 / N64 / GameCube, with the files in your user folder:

```bash
./xrecomp prepare ~/games/Fable.iso          # or a disc folder, a default.xbe, a .gba, a .cue / .chd, a .z64
./xrecomp list                               # the library: Prepared, Prepared · out of date, Not prepared
./xrecomp play fable                         # by title, title ID or source path
./xrecomp settings fable RECOMP_ASPECT=16:9  # this game only; without a game: Default settings
```

It is a small Python program in the toolkit (`tools/xrecomp`, started by the `xrecomp` script at the toolkit's root,
or `python3 -m tools.xrecomp`), with no dependency beyond what `tools.prepare` needs. Link it onto your PATH to run
it from anywhere: `ln -s "$PWD/xrecomp" ~/.local/bin/xrecomp`.

## Install

Linux x86-64 or arm64 (macOS works too; its app is in [launcher.md](../macos/launcher.md)). Download
`xrecomp-v<version>-linux-x86_64.tar.gz` (or `-linux-arm64.tar.gz`) from the
[latest release](https://github.com/MisterAlex95/Xrecomp/releases/latest), or use a git checkout, then in its
`XRecomp` folder:

```bash
bash tools/linux/install_deps.sh            # apt / pacman / dnf  (macOS: bash tools/macos/setup.sh)
python3 -m pip install capstone pefile numpy
./xrecomp prepare /path/to/game.iso         # or a disc folder, a .gba, a PS1 .cue / .chd, a .z64
./xrecomp list                              # your library: Prepared, Prepared · out of date, Not prepared
./xrecomp play game                         # by title, title ID or path
./xrecomp settings game RECOMP_GPU=1        # per game; without a game: Default settings
```

Games are read where they are; work folders, settings and saves go to `~/.local/share/xrecomp` ([Storage](#storage)).
`python3 -m tools.prepare` stays underneath (`--check`, `--help`).

Each release archive carries the runtime prebuilt for its architecture (`runtime/linux-x86_64/` or
`runtime/linux-arm64/`, built on Ubuntu 24.04): prepare then compiles and links the title with clang (or the system's
`cc`) alone, no CMake or Ninja. A `toolchain/bin/clang` next to the toolkit is used first when there is one, else
`--cc` / `$XBA_CC`, else the system's. The GBA / PS1 / N64 players `./xrecomp play` starts are in the same folder
(`bin/`). A git checkout builds with CMake as before; `python3 -m tools.prepare.runtime` makes the prebuilt runtime
and the players once where CMake is, and `--builder driver|cmake` picks the route.

## Commands

| Command | Does |
|---|---|
| `xrecomp prepare SOURCE` | `python -m tools.prepare SOURCE --work <title>/work` (consoles: `--pack-dir <root>/packs`), its progress shown as `[2/5] analyse 40 % ...`. SOURCE may also be a game of the library (its ID or title): prepare it again. `--force` redoes every step, `--no-build` stops an Xbox prepare after the lift, `--no-title-layer` / `--title-layer` leaves the title's optional enhancements (`profiles/titles/<id>`) out or puts them back (remembered per title, the Mac app's Manage › Enhancements), `--print` shows the command without running it, arguments after `--` go to `tools.prepare` |
| `xrecomp play GAME` | Starts the game with its settings ([below](#play)). `--print` shows the working directory, the environment changes and the command line without running them (`--print --json`: argv, cwd and the whole environment, what the macOS launcher starts, XBA-278); arguments after `--` go to the game or the console player (`-- --frames 600`) |
| `xrecomp identify SOURCE...` | What each source is (console, ID, key, title) and how prepared, with a console game's player (null when not built) and its save and pack folders. `--json` for the macOS launcher (XBA-278) and scripts |
| `xrecomp list [FOLDER...]` | The titles xrecomp knows (prepared or played once) and the games found in the library folders and in `FOLDER`s, 3 levels deep, each with its state. `--json` for scripts |
| `xrecomp settings [GAME] [KEY=VALUE...]` | Shows or changes Default settings, or a game's ([below](#settings)). `--unset KEY` removes a key's line, `--reset` every key line |
| `xrecomp folders [add\|remove FOLDER]` | The library folders `list` scans (settings.json `library_folders`; on a Mac the app's, default `~/Documents/xbox`) |
| `xrecomp where [GAME]` | The data folder, or a game's folders (work, settings, saves, logs, pack, discovered file) |

GAME is a source path, a title ID in any case (`12345678`, `gba-AXVE-1A2B3C4D`) or a part of a title that names one
game only. `--home DIR` (before the command) or `$XRECOMP_HOME` moves the data folder.

## The library and its states

The same words as the Android app's chips (app-shell.md):

| State | Xbox | GBA / PS1 / N64 / GameCube |
|---|---|---|
| **Prepared** | `tools.prepare --check`: ready | a valid pack of this toolkit: runs natively |
| **Prepared · out of date** | `--check`: stale (toolkit, runtime, profile, enhancements, the game's files, code found while playing, a build made outside xrecomp); the old executable still plays | the pack was lifted by another toolkit (`toolkit_sha256`); it still loads |
| **Not prepared** | no executable: prepare it to play | no valid pack: plays in the interpreter |
| **Source gone** / **Not readable** | the recorded source moved, or cannot be read (an ISO still being copied) | the same |

A GBA / PS1 / N64 / GameCube game whose runs found code its pack did not cover keeps « Prepared » and gets the Android Home's
note, « N new addresses found · prepare again ». The console is told the way `tools.prepare` tells it (an N64 ROM's
first word, a GBA header, a PS1 disc or EXE, else Xbox), by the same modules. One entry per game: a disc found twice
is listed once, a known title's recorded source first.

## Storage

The platform's user data folder, `$XRECOMP_HOME` first:

| Host | Folder |
|---|---|
| Linux and other Unix | `$XDG_DATA_HOME/xrecomp` (`~/.local/share/xrecomp`), where `gba_play` / `ps1_play` / `n64_play` and `python -m tools.prepare game.gba` already keep saves and packs |
| macOS | `~/Library/Application Support/xboxrecomp`: the XRecomp app's ([launcher.md « Storage »](../macos/launcher.md#storage)); the app and the command line share titles, settings and saves, and each keeps the keys the other writes in `settings.json` and a title's `state.json` (the app has no use for `title` and `console` but saves them back) |
| Windows | `%APPDATA%\xrecomp` |

```
settings.json           library folders, work root, Python (the Mac app's file; keys xrecomp does not use are kept)
defaults/recomp.cfg     Default settings (also copied to settings.json global_defaults, as the Mac app does)
titles/<ID>/
  work/                 tools.prepare --work (settings.json work_root moves it: <work_root>/<ID>)
  recomp.cfg            the game's settings
  save/                 an Xbox title's RECOMP_SAVE_DIR (UDATA, TDATA, Z: cache)
  logs/run-*.log        the last 10 runs (the command line, the environment changes, the output)
  state.json            source path, title, console, play time, runs, last played, last prepare, enhancements
packs/<console>-<KEY>/  GBA / PS1 / N64 / GameCube title packs
save/<console>-<KEY>/   GBA / PS1 / N64 / GameCube saves, memory cards and save states
```

`<ID>` is the Xbox title ID or `<console>-<KEY>`, the Android app's library key. **Your games are read where they
are**: an Xbox image is read in place (XBA-126; the build's `game` link points at it), a disc folder is linked, never
copied. The console prepares keep only small copies they lift from (the GBA ROM, a PS1 disc's boot EXE). Nothing is
ever uploaded.

## Play

- **Xbox**: the executable the prepare built (`work/project/build/<name>`), working directory its build folder. The
  desktop `main.c` does not read `recomp.cfg`: as the Mac app does, xrecomp passes the settings as environment
  variables. Code a run finds that the analysis missed goes to `work/discovered.jsonl` (the prepare bakes that path
  into the build); the title then turns « out of date » and the next prepare lifts it (self-healing, XBA-74).
- **GBA / PS1 / N64 / GameCube**: `<console>_play SOURCE`, with `--pack <root>/packs/<console>-<KEY>/libtitle.so --missed
  <work>/discovered.txt` when the game has a valid pack (the next prepare lifts what the run missed), else in the
  interpreter. Working directory: the toolkit (where `ps1_play` looks for `build/ps1-bios/openbios.bin`). The player
  is `$XRECOMP_<CONSOLE>_PLAY`, else the one on the PATH, else the newest in a build folder of the toolkit
  (`build*/consoles/<c>/runtime/`, the root build; `build*/<c>/<c>_runtime/`, a console's own), else the one a
  release archive carries with its prebuilt runtime (`runtime/<host>/bin/`, XBA-87). In a checkout without a build:
  `cmake -S . -B build && cmake --build build --target gba_play` (or `ps1_play`, `n64_play`, `gc_play`).

The environment: xrecomp's own, then Default settings, then the game's lines (the first line for a key wins), then
`RECOMP_SAVE_DIR`. `KEY=` (empty) unsets the variable. PATH and RECOMP_SAVE_DIR are never taken from a settings file.
Ctrl+C stops the game by its PID (SIGTERM, then SIGKILL after 5 s). Every key:
[environment-variables.md](../runtime/environment-variables.md).

## Settings

`recomp.cfg` as the launchers write it (Android `RecompConfig` / `GameSettings`, XBA-188; the Mac's
`RecompConfig.swift` / `GameSettings.swift`): one `KEY=VALUE` per line, `#` comments, the first line for a key wins;
a change rewrites the key's first line in place and keeps every other line.

```bash
xrecomp settings RECOMP_GPU=1                    # Default settings, for every game
xrecomp settings RECOMP_GPU=                     # Default settings: back to the runtime's default (the line goes)
xrecomp settings fable RECOMP_GPU=               # this game: the runtime's default even though Default settings say 1
xrecomp settings fable RECOMP_GPU_FPS_CAP=30     # this game's own value
xrecomp settings fable --unset RECOMP_GPU        # this game follows Default settings again
xrecomp settings fable --reset                   # every key line of the game removed, comments kept
xrecomp settings fable                           # each key with where its value comes from
```

A game's file over Default settings: a key the game's file does not set follows Default settings; `KEY=VALUE` is the
game's own; `KEY=` is the runtime's default for this game. An Xbox title's profile can force options (`env_force`,
compiled in, they win): `settings GAME` shows them as « set by the title ».

## Notes

- **Prepare's public entry**: xrecomp runs `python -m tools.prepare` and nothing below it, so either compile route
  works (CMake today, the compile driver of XBA-86 later).
- **Python**: `$XBA_PYTHON`, else settings.json `python_path`, else the toolkit's `.venv`, else the Python running
  xrecomp; it needs what `tools.prepare` needs (`capstone`, `pefile`, `numpy`; [Install](#install)).
- **The toolkit digest** leaves `tools/xrecomp` out (`tools/prepare/toolkit_tree.json`): a change to the command line
  never makes a pack stale.
- **Windows** (experimental, [windows.md](windows.md)): `xrecomp.cmd` in the Windows zip, Xbox titles only, built
  by the compile driver against the MSVC-built runtime. The console runtimes are POSIX-only: `prepare` and `play`
  refuse GBA, PS1, N64 and GameCube games there and say so.
- Tests: `python3 -m pytest tools/xrecomp` (a temporary home; the synthetic conformance XBE, homebrew-shaped
  synthetic ROMs, a GBA pack built with the fake lifter and the host's `cc`).
