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
| `main` | Pure mirror of `upstream/main`. Never commit on it. |
| `android` | Our work: upstream plus the Android host. PRs target it. |

We take from upstream but send nothing back: no PRs or issues on GitHub, every change stays here (Alex, 2026-11-09).
Keep generic fixes in their own commits anyway: they merge more easily with upstream changes.

## Following upstream

```bash
tools/sync_upstream.sh          # fast-forward main, merge it into android
tools/sync_upstream.sh --push   # the same, then push main and android to origin
```

On a conflict the script aborts the merge and lists the files; resolve by hand
(`git merge main` in the android worktree) and commit.

## Linux baseline (CI)

Every push to `main` / `android` and every PR runs `.gitea/workflows/ci.yml`, which builds
`tools/docker/linux.Dockerfile` (target `test`):

1. the toolkit builds (gcc, Debian trixie);
2. `tools/run_c_tests.sh` builds each self-contained C test project under `tests/` and runs its ctest
   (the root build lists only two of them); a skip (exit 77) is fine, a failure fails the run;
3. `pytest tools/` passes (clang on the PATH: the lifter tests compile with it, as the NDK does).

Locally:

```bash
docker build -f tools/docker/linux.Dockerfile --target test .
```

## Legal

Personal builds only: no game code, XBE, disc image or game data in this repo, in CI, or in any release.
Per-title repos (generated C) are separate and private.
