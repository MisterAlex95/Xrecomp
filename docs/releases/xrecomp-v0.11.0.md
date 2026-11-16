# XRecomp 0.11.0

Xbox games run their float maths faster and get save states within a session: Save / Load state in the in-game menu,
quick save / load on the pad, and rewind. **Prepare your Xbox games again**: the generated code changed.

## Faster Xbox code

Measured on our benchmark set (instructions executed, the same results bit for bit):

| | before | 0.11.0 |
|---|---|---|
| x87 float code (x86-64 and arm64) | 100 % | 74 % |
| SSE packed float code (arm64) | 100 % | 70 % |
| Integer, memory, calls | 100 % | 100 % |

- **x87**: within a block of code, the x87 register stack stays in the CPU's own registers instead of memory.
- **SSE on arm64**: packed add, multiply, divide, min / max, compares, square root and the bitwise operations use NEON,
  one instruction where there were six or seven.

These are benchmark numbers, not frame rates on a device: tell us what your games do.

## Xbox save states, within a session

- **Save state / Load state** in the in-game menu, four slots, **quick save / load** with Back + RB / Back + LB,
  and **rewind** (hold Back + LT), as for GBA, PS1 and N64.
- A state loads back only in the **same session** and only when every thread of the game is at the same point as when
  it was saved: otherwise the load is refused, with the reason, and the game goes on untouched. States do not survive
  quitting the game yet, so there is no « Resume where I left off » for Xbox titles (planned for 0.12).
- Rewind switches itself off when a game's state is too large for its memory budget (`RECOMP_REWIND_MB` in
  `recomp.cfg`, 64 MB by default; the log says so).
- How it works and its limits: [xbox-save-states.md](../technical/xbox-save-states.md).

## For developers

- `python -m tools.bench icount` counts the instructions each benchmark executes (arm64 under qemu, x86-64 under
  callgrind), to compare builds.
- `RECOMP_STATE_PROBE=<seconds>` takes a test capture periodically and logs its time and size; the kernel summary's
  `[KERNEL] park:` line says how often a game's threads could be frozen for a state.

## Known problems

- An Xbox load is refused while a thread of the game waits for another thread (common on loading screens): load in
  gameplay, at a point close to where you saved.
- Xbox rewind will often switch itself off on real games with the default budget.
- If float-heavy parts of an Xbox game (camera, physics, HUD numbers) look wrong, tell us: building with
  `-DRECOMP_XMM_NO_NEON` turns the NEON path off for a comparison.
- From 0.10.0: fast-forward cannot speed up an Xbox game that paces itself on its sound buffers; Xbox cheat codes on
  heap addresses may only work for one run.

## Before you install

- Updates 0.10.0 and older, and `xrecomp-latest`, in place (same signing key).
- **Prepare your Xbox games again** (the library shows them « Prepared · out of date »). GBA, PS1 and N64 games stay
  prepared; their save states and every per-game setting are kept.
- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, the Vulkan driver for N64, and `adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG`
  ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach game files.
