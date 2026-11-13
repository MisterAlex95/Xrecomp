# Android port (fork)

This repository is a fork of [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) that adds
**Android arm64** as a host. The maintainers plan in a private tracker (Plane project **XBA**: the `XBA-nn` in
commits and docs); public bug reports go to this repository's issues.

## Branches and remotes

| Remote | URL |
|---|---|
| `origin` | `https://git.cf.adserver.fr/Alex/xboxrecomp-android.git` (this fork; maintainers push over SSH) |
| `upstream` | `https://github.com/sp00nznet/xboxrecomp` |

| Branch | Role |
|---|---|
| `main` | Our work: upstream plus the Android host. PRs target it; merged as soon as CI is green. |

Upstream is followed through the `upstream` remote (`upstream/main`), merged into `main` by the script below.
Until 2026-11-09 our work lived on `android` with `main` as a mirror; that branch is gone.

We take from upstream but send nothing back: no PRs or issues on GitHub, every change stays here (Alex, 2026-11-09).
Keep generic fixes in their own commits anyway: they merge more easily with upstream changes.

## Following upstream

```bash
tools/sync_upstream.sh          # merge upstream/main into main
tools/sync_upstream.sh --push   # the same, then push main to origin
```

On a conflict the script aborts the merge and lists the files; resolve by hand
(`git merge upstream/main` in the main worktree) and commit.

## Linux baseline (CI)

Every push to `main` and every PR runs `.gitea/workflows/ci.yml`, which builds
`tools/docker/linux.Dockerfile` (target `test`):

1. the toolkit builds (gcc, Debian trixie);
2. `tools/run_c_tests.sh` builds each self-contained C test project under `tests/` and runs its ctest
   (the root build lists only two of them); a skip (exit 77) is fine, a failure fails the run;
3. `pytest tools/` passes (clang on the PATH: the lifter tests compile with it, as the NDK does).

On pushes to `main` (and by hand), a second job cross-compiles everything for arm64 and runs the tests under qemu-user
(`tools/docker/linux-arm64.Dockerfile`); it is red on any failure that is not listed in `tools/ci/arm64-expected.txt`
(see [baseline-arm64.md](baseline-arm64.md)). PRs that touch the toolkit also compile it with the NDK. The whole
gating, Android jobs included: [toolchain.md « CI gating »](toolchain.md#ci-gating).

Locally:

```bash
docker build -f tools/docker/linux.Dockerfile --target test .
```

## Legal

No game code, XBE, disc image or game data in this repo, in CI, or in any release: the app builds a game's pack on
the player's device from the player's disc. Per-title repos (generated C) are separate and private. The licences the
APK carries (SDL2, CPython, capstone, LLVM, the NDK sysroot) are listed in NOTICE « The Android app » and shown by the
app (About › Licences, from `LICENSE`, `NOTICE` and `LICENSES/*.txt`, copied into the assets at build time).

## Android toolchain

The pinned image `tools/docker/android.Dockerfile` (NDK, SDK, Gradle, JDK, SDL2 sources): see [toolchain.md](toolchain.md).
The toolchain the APK can carry for the embedded Prepare (Python + capstone, clang + lld for arm64, `-PxbaToolchainDir`):
[toolchain.md « The on-device toolchain »](toolchain.md#the-on-device-toolchain-embedded-prepare).

## App shell

The APK every title starts from (SDLActivity + `libmain.so` + logcat): see [app-shell.md](app-shell.md).

## GPU renderer

The default (XBA-119): the GL back end draws with OpenGL ES 3 on a headless EGL context instead of the CPU
rasteriser, unless launcher › Settings › Graphics › Renderer says Software (`RECOMP_GPU=0`) or the title's profile
forces it off; a device without GLES 3.0 falls back to the CPU rasteriser by itself; `RECOMP_GPU_FPS_CAP` (Frame cap, default 60, or the title profile's: 30 for Fable) keeps menus from running at 200+ fps;
`RECOMP_GPU_SCALE` (Internal resolution: 1×, 2×, 3×, Fit the screen; default 1×) draws the 3D picture above 640×480
and hands it to the window at that size.
Context, GLES details and the device check lists:
[gpu-backend.md « Android »](../technical/gpu-backend.md#android).

## Prepare on the device

A title pack (`libtitle.so` + `pack.json`) built on the device from the player's disc, `tools.prepare --android`:
see [prepare.md](prepare.md).

## Tests on a device

The C tests as Android executables, run over adb: see [device-tests.md](device-tests.md).
