# Releases

XRecomp's own releases, one page of notes each. Not to be confused with [CHANGELOG.md](../../CHANGELOG.md), which is
upstream xboxrecomp's history (its `v0.x` tags are upstream's, not ours): our tags are `xrecomp-v<version>`.

| Release | Notes |
|---|---|
| `xrecomp-v0.13.11` | [xrecomp-v0.13.11.md](xrecomp-v0.13.11.md) |
| `xrecomp-v0.13.10` | [xrecomp-v0.13.10.md](xrecomp-v0.13.10.md) |
| `xrecomp-v0.13.9` | [xrecomp-v0.13.9.md](xrecomp-v0.13.9.md) |
| `xrecomp-v0.13.8` | [xrecomp-v0.13.8.md](xrecomp-v0.13.8.md) |
| `xrecomp-v0.13.7` | [xrecomp-v0.13.7.md](xrecomp-v0.13.7.md) |
| `xrecomp-v0.13.6` | [xrecomp-v0.13.6.md](xrecomp-v0.13.6.md) |
| `xrecomp-v0.13.5` | [xrecomp-v0.13.5.md](xrecomp-v0.13.5.md) |
| `xrecomp-v0.13.4` | [xrecomp-v0.13.4.md](xrecomp-v0.13.4.md) |
| `xrecomp-v0.13.3` | [xrecomp-v0.13.3.md](xrecomp-v0.13.3.md) |
| `xrecomp-v0.13.2` | [xrecomp-v0.13.2.md](xrecomp-v0.13.2.md) |
| `xrecomp-v0.13.1` | [xrecomp-v0.13.1.md](xrecomp-v0.13.1.md) |
| `xrecomp-v0.13.0` | [xrecomp-v0.13.0.md](xrecomp-v0.13.0.md) |
| `xrecomp-v0.12.2` | [xrecomp-v0.12.2.md](xrecomp-v0.12.2.md) |
| `xrecomp-v0.12.1` | [xrecomp-v0.12.1.md](xrecomp-v0.12.1.md) |
| `xrecomp-v0.12.0` | [xrecomp-v0.12.0.md](xrecomp-v0.12.0.md) |
| `xrecomp-v0.11.0` | [xrecomp-v0.11.0.md](xrecomp-v0.11.0.md) |
| `xrecomp-v0.10.0` | [xrecomp-v0.10.0.md](xrecomp-v0.10.0.md) |
| `xrecomp-v0.9.0` | [xrecomp-v0.9.0.md](xrecomp-v0.9.0.md) |
| `xrecomp-v0.8.0` | [xrecomp-v0.8.0.md](xrecomp-v0.8.0.md) |
| `xrecomp-v0.7.0` | [xrecomp-v0.7.0.md](xrecomp-v0.7.0.md) |
| `xrecomp-v0.6.0` | [xrecomp-v0.6.0.md](xrecomp-v0.6.0.md) |
| `xrecomp-v0.5.1` | [xrecomp-v0.5.1.md](xrecomp-v0.5.1.md) |
| `xrecomp-v0.5.0` | [xrecomp-v0.5.0.md](xrecomp-v0.5.0.md) |
| `xrecomp-v0.4.0` | [xrecomp-v0.4.0.md](xrecomp-v0.4.0.md) |
| `xrecomp-v0.3.0` | [xrecomp-v0.3.0.md](xrecomp-v0.3.0.md) |
| `xrecomp-v0.2.0` | [xrecomp-v0.2.0.md](xrecomp-v0.2.0.md) |
| `xrecomp-v0.1.0` | [xrecomp-v0.1.0.md](xrecomp-v0.1.0.md) |

Downloads: the [GitHub releases](https://github.com/MisterAlex95/Xrecomp/releases). Besides the tagged releases,
`xrecomp-latest` is a rolling prerelease rebuilt from every push to `main`: newer, less tested, debuggable.

## What a release contains

- `xrecomp-v<version>.apk`: the XRecomp Android app with the on-device Prepare. arm64-v8a, Android 11+.
  Release-signed (the same key as `xrecomp-latest`, so either updates the other in place) and **not debuggable**.
- `xrecomp-v<version>.apk.sha256`.
- `xrecomp-v<version>-macos-arm64.zip`: the Mac app (Apple silicon, macOS 14+) inside the toolkit folder it drives.
  Ad-hoc signed, not notarized: run `xattr -dr com.apple.quarantine <the XRecomp folder>` once before opening it,
  otherwise macOS blocks it or runs it from a translocated copy that cannot see its toolkit (Setup then asks for it).
- `xrecomp-v<version>-linux-x86_64.tar.gz` and `xrecomp-v<version>-linux-arm64.tar.gz`: the toolkit for Linux
  (README « Linux »), each built and smoke-tested on its own architecture (GitHub's Ubuntu 24.04 runners, XBA-87).
  Up to 0.12 there was one `-linux.tar.gz`, x86-64 only.
- `xrecomp-v<version>-windows-x86_64.zip` (**experimental**, XBA-89): the toolkit for Windows x86-64 with
  `xrecomp.cmd`, Xbox games only (the console runtimes are POSIX-only). Needs Python 3 and Visual Studio's Build
  Tools (docs/desktop/windows.md). Left out of a release when its CI job fails; the other files are not held back.
- The **prebuilt runtime** of the desktop folders (XBA-86): `runtime/macos-arm64/` in the Mac zip,
  `runtime/linux-x86_64/` and `runtime/linux-arm64/` in the Linux archives, `runtime/windows-x86_64/` (MSVC) in the
  Windows zip (`tools/prepare/runtime.py`), so prepare builds a title with a C compiler alone, no CMake
  (`tools/prepare/desktop_build.py`). On Linux and macOS its `bin/` has the console players (`gba_play`, `ps1_play`,
  `n64_play`) that `./xrecomp play` starts (XBA-87); Windows has none.
- A `.sha256` next to each.
- **Frame generation** (LSFG, XBA-187) in every one of them: the APK has the Vulkan bridge in `libmain.so` and the
  shader translator in `libxba_import.so` (Settings › Graphics › Frame generation, « Choose Lossless.dll »); the
  desktop folders carry its pinned sources (`third_party/lsfg-sources`), which a game build compiles in by default.
  Lossless.dll itself is never shipped: the player picks their own copy, from their Lossless Scaling install.
  Licences: NOTICE « Frame generation (LSFG) », `LICENSES/MIT-lsfg-vk.txt`, `LICENSES/Zlib-DXVK.txt`.
- No game, no game data, no generated C: you prepare your own games on your device or computer.

`xrecomp-latest` carries the same files, named `xrecomp-<commit>…`. The desktop files are built on GitHub's runners
by `.github/workflows/desktop.yml` (each one is this repository's tree at the commit, `tools/ci/package_toolkit.sh`,
plus `XRecomp.app` for the Mac) and attached once Gitea has published the release of that commit. The Windows zip is
experimental: built and smoke-tested (a synthetic XBE prepared and started) on GitHub's Windows runner, no game played
on Windows yet.

Every XRecomp APK we publish (releases and `xrecomp-latest`) is signed with the same certificate, SHA-256:

```
BC:F2:04:46:E6:FE:38:6F:3C:FA:A1:B8:75:B2:5E:99:7A:77:0A:E6:1A:B2:48:68:DD:4C:9C:E3:18:F8:39:FD
```

Check a download with `apksigner verify --print-certs xrecomp-v<version>.apk` (Android SDK build-tools): its
`Signer #1 certificate SHA-256 digest` must be the same, lower case without colons. Any other signer is not ours.

The Mac app and the toolkit can also be built from the source at the tag (README « Get it »).

## Making a release (maintainers)

Releases are cut through a **release PR**, in the spirit of semantic-release / release-please
(`tools/release/release.py`, `.gitea/workflows/release-pr.yml`):

1. **Commits say how far the version moves**, with a trailer in the last paragraph of the message (the subject keeps
   our `Area: sentence` style):

   ```
   Recomp: the x87 pass keeps locals in registers

   Longer explanation.

   Release: minor
   Co-Authored-By: ...
   ```

   Git reads trailers in the last paragraph only: `Release:` goes in the same block as `Co-Authored-By:`, with no
   blank line between them (`git log -1 --format='%(trailers:key=Release)'` shows whether it was seen).

   `major`, `minor`, `patch` or `none`; no trailer counts as `patch` (upstream's commits included). The next version
   is the last `xrecomp-v*` tag bumped by the largest trailer since it. A finished roadmap milestone (Plane vX.Y) that
   no commit marked: push `git commit --allow-empty -m "Release: v0.14 milestone is done" -m "Release: minor"`.
   A trailer only pushes the version up; to take a bump back (the roadmap goes from 0.13 to 1.0, say), a commit with
   `Release-As: <version>` sets the next version outright (the newest one since the last tag wins).
2. **After every push to `main`**, `release-pr.yml` keeps one open PR « Release: XRecomp <version> » (branch
   `release/next`). It adds `docs/releases/xrecomp-v<version>.md`, whose « Changes » section is generated from the
   commits grouped by area, and its row in the table above. Write the highlights (what changed, what works, known
   problems) above « Changes » on that branch: they are kept when main moves on, even when the version changes; the
   generated section is rewritten each time.
3. **Merge the PR into `main`.** `.gitea/workflows/release.yml` sees that the push added a notes file whose tag does
   not exist, creates the tag `xrecomp-v<version>` on the merge commit, waits for main's CI to have cached the
   on-device toolchain of that commit (`toolchain-<key>`, up to 2 h), builds the APK (release key, not debuggable,
   version name `<version>+<commit>`) and publishes the release on Gitea and on GitHub with the notes. It stops
   without publishing when the notes file, the release key or `GH_RELEASE_TOKEN` is missing
   ([toolchain.md « Signing »](../android/toolchain.md#signing) lists the secrets).
4. GitHub's `Desktop` workflow (`.github/workflows/desktop.yml`) runs on the tag when the mirror pushes it, waits
   for that release, then attaches the macOS zip and the two Linux archives and appends a « Desktop » section to the
   notes. A re-run of Gitea's release job recreates the release without them: re-run the Desktop run of the tag.
5. Check the GitHub release page: the APK, the three desktop files, their checksums, the notes ending with
   `signed with: release key`.

By hand, without the PR, still works: write `docs/releases/xrecomp-v<version>.md` and its table row, merge them,
and either let `release.yml` tag that merge (the notes file must be added by the last commit of the push) or tag a
green commit yourself: `git tag -a xrecomp-v<version> -m "XRecomp <version>" && git push origin xrecomp-v<version>`.
`python3 tools/release/release.py next` prints the version the commits ask for.

Versions follow [semantic versioning](https://semver.org) loosely while we are below 1.0: the minor number goes up
with a release that adds something, the patch number with a release that only fixes.
