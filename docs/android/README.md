# Android port (fork)

This repository is a private fork of [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) that adds
**Android arm64** as a host. Plan, decisions and tickets: Plane project **XBA**.

## Branches and remotes

| Remote | URL |
|---|---|
| `origin` | `ssh://git@git.cf.adserver.fr:22222/Alex/xboxrecomp-android.git` (this fork, private) |
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

Personal builds only: no game code, XBE, disc image or game data in this repo, in CI, or in any release.
Per-title repos (generated C) are separate and private.

## Android toolchain

The pinned image `tools/docker/android.Dockerfile` (NDK, SDK, Gradle, JDK, SDL2 sources): see [toolchain.md](toolchain.md).

## App shell

The APK every title starts from (SDLActivity + `libmain.so` + logcat): see [app-shell.md](app-shell.md).

## Tests on a device

The C tests as Android executables, run over adb: see [device-tests.md](device-tests.md).
