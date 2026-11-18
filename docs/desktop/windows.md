# Windows (experimental)

The Windows x86-64 download (XBA-89): `xrecomp-v<version>-windows-x86_64.zip` on each release, built and
smoke-tested on GitHub's Windows runner by `.github/workflows/desktop.yml`. **Xbox games only**, through upstream's
Windows back ends (Win32, D3D11, XInput, XAudio2). Experimental: no one in this fork has played a game on Windows yet.

## What is in the zip

- The toolkit (this repository's tree at the commit, `tools/ci/package_toolkit.sh`).
- `runtime/windows-x86_64/`: the runtime libraries prebuilt with MSVC (`tools/prepare/runtime.py`), so a prepare
  compiles and links the title with a C compiler alone, no CMake (`tools/prepare/desktop_build.py`).
- `xrecomp.cmd`: the command line of [cli.md](cli.md) (`prepare`, `play`, `list`, `settings`, `where`).

No game, no game data, no generated C.

## Set up

1. **Python 3.9+** from python.org (tick « Add python.exe to PATH »), then `python -m pip install capstone pefile numpy`.
2. **A C compiler**: Visual Studio 2022's **Build Tools** with the « Desktop development with C++ » workload (cl, link
   and the Windows SDK). Optional: its « C++ Clang tools for Windows » component, or LLVM, for clang-cl, which is used
   first when it is on PATH. XBA-85: the system's compiler during 0.x, an embedded clang for 1.0.
3. Unzip anywhere, open a terminal in the `XRecomp` folder:

```bat
xrecomp.cmd prepare C:\Games\MyGame\default.xbe   :: or a disc image or an extracted disc folder
xrecomp.cmd list
xrecomp.cmd play mygame
```

`xrecomp.cmd` opens a Visual Studio developer environment for the run (vswhere, then `vcvars64.bat`) when the terminal
is not one already. Work folders, settings and saves go to `%APPDATA%\xrecomp`.

The compile driver picks, in order: `--cc` / `%XBA_CC%`, `toolchain\bin\clang-cl.exe` in the toolkit, clang-cl on
PATH (with lld-link), MSVC's cl (with the link.exe beside it). The title links against the MSVC-built runtime with the
same C runtime (`/MD`).

## Limits

- **Console games (GBA, PS1, N64, GameCube) play in the interpreter only** (XBA-278, [below](#console-games)): their
  players are not in the zip (it is built with MSVC) but cross-build with MinGW-w64; `xrecomp prepare` refuses a
  console game's native pack there and says so (the pack build and loader are the POSIX hosts').
- No frame generation (LSFG is POSIX-only), no GL render back end: D3D11.
- A disc image read in place, or a game folder, is linked into the build folder as `game`: a symbolic link when
  Windows allows one (Developer Mode), else a directory junction (a folder) or a hard link (an image on the same
  drive). Otherwise prepare stops and suggests Developer Mode or `--extract`.

## Console games

XBA-278: the host layer (`src/host`) and the GBA, PS1, N64 and GameCube runtimes build for Windows with MinGW-w64.
Their few POSIX calls go through `src/platform/win32_posix/` (force-included `xbr_posix_win32.h`: setenv, localtime_r,
pread, sysconf...; a `dlfcn.h` over LoadLibrary), with SDL2's MinGW development package:

```
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DXBOXRECOMP_BUILD_TESTS=OFF \
      -DXBR_CONSOLE_GBA=ON -DXBR_CONSOLE_PS1=ON -DXBR_CONSOLE_N64=ON -DXBR_CONSOLE_GC=ON \
      -DSDL2_DIR=<SDL2-2.30.x>/x86_64-w64-mingw32/lib/cmake/SDL2
cmake --build build-win --target gba_play ps1_play n64_play gc_play gc_run
```

Put the players (and `SDL2.dll`, plus MinGW's `libwinpthread-1.dll` unless linked statically) on the PATH, or name
them with `XRECOMP_GBA_PLAY` and the like; `xrecomp play` then runs a console game in the interpreter, with its
saves, states, rewind, cheats and hotkeys. Without a console option the build is the Xbox one, as before (MSVC
included: these targets are only defined when a console is asked for). Not done: the native packs (prepare), MSVC
for the console runtimes (winpthreads), frame generation; the players were cross-built and started under wine on
Linux only, never on a Windows PC.

## What CI checks, and what it does not

The `windows` job of `.github/workflows/desktop.yml`, on GitHub's `windows-2025` runner:

1. builds `runtime/windows-x86_64` with MSVC (`-DCMAKE_C_COMPILER=cl`, Ninja); when cl fails it warns and builds it
   with clang-cl instead;
2. from the zip, prepares the synthetic retail XBE (`tools/conformance/mkxbe.py --retail`) with the compile driver,
   a failing `cmake` first on PATH, and starts the executable from its build folder until it prints « XBE loaded »;
3. runs `xrecomp.cmd` (`prepare --no-build`, `list`) and checks that a GBA ROM is refused;
4. prepares the same title with `--cc cl` (informative: its failure does not fail the job).

The job's failure leaves the Windows zip out of the release; the macOS and Linux files are attached regardless.
No retail game has been prepared or played on Windows by this fork.

## Checking the Windows route from Linux

`tools/windows/mingw_check.sh` cross-builds the runtime with MinGW-w64 (`cmake/mingw-w64-x86_64.cmake`) into a
`windows-x86_64` runtime (`XBA_HOST=windows-x86_64` makes `tools/prepare/runtime.py` and the prepare use that host),
prepares the synthetic XBE with `--cc x86_64-w64-mingw32-gcc`, and with wine starts it until « XBE loaded ». Its
header has the Docker command (the xba-deps image plus `gcc-mingw-w64-x86-64`). That checks the libraries, the
manifest, the driver and the Win32 start-up; MSVC, clang-cl and the D3D11 window are only checked on the runner.
