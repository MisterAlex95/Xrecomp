# XRecomp 0.12.0

Xbox save states now survive quitting the game, so « Resume where I left off » works for Xbox titles too. On the
desktop, Prepare no longer needs CMake, an `xrecomp` command line drives everything, and there are Linux arm64 and
(experimental) Windows downloads. **Prepare your Xbox games again**: the generated code changed.

## Xbox: states across sessions

- A state saved in one session loads in the next: the four slots, and **« Resume where I left off »** (in-game
  menu › Settings, on by default). Leaving the game saves the automatic slot; its Home then shows **Resume** and
  **Start**, as for the consoles.
- This works for titles whose threads can all be re-entered where they stopped. A state that cannot (a game thread
  inside an exception handler, fibers, a few hand-written code paths) is not offered: the automatic slot is not
  written and the log says why; the game then starts from the beginning.
- Loads within a session now compare each thread's whole chain of calls, not only where it stopped, so a state is
  never loaded into a thread that reached the same point another way.
- The cost is paid only by functions that can reach the system: our benchmarks run the same number of instructions
  as in 0.11.0; the generated code grows by about 8 %.
- How it works: [xbox-save-states.md](../technical/xbox-save-states.md).

## Desktop

- **No CMake needed**: on Linux and macOS, Prepare compiles a game with the system's clang (or cc) against the
  runtime prebuilt in the download.
- **`xrecomp` command line** (`./xrecomp prepare | play | list | settings`, [cli.md](../desktop/cli.md)) for Xbox,
  GBA, PS1 and N64 games, with your games and saves in your user folder (shared with XRecomp.app on macOS).
- **Downloads**: `-linux-x86_64.tar.gz` and **`-linux-arm64.tar.gz`** (they replace `-linux.tar.gz`), each with the
  GBA / PS1 / N64 players; the macOS zip carries them too. **Windows x86-64** (`-windows-x86_64.zip`, `xrecomp.cmd`),
  experimental and for Xbox games only ([windows.md](../desktop/windows.md)): attached only when its build passes.

## Known problems

- No Xbox title has been resumed after a restart on a device yet: tell us what yours do (the log's `[STATE]` lines
  say why a state is refused).
- An Xbox load within a session is still refused while a thread waits for another (loading screens); a session state
  is refused when a frame's scratch value changed since it was saved, which happens more often with rewind.
- The Windows download has never been tried on a Windows PC by us; the Linux arm64 one only on emulated arm64.
- The Mac app's Setup still lists CMake as needed; it is optional now.
- From 0.11.0: Xbox rewind often switches itself off on real games with the default budget (`RECOMP_REWIND_MB`).

## Before you install

- Updates 0.11.0 and older, and `xrecomp-latest`, in place (same signing key).
- **Prepare your Xbox games again** (« Prepared · out of date »). Xbox states saved with 0.11.0 do not load in 0.12.0.
  GBA, PS1 and N64 games stay prepared; their save states and every per-game setting are kept.
- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, the Vulkan driver for N64, and `adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG`
  ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach game files.
