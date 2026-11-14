# Releases

XRecomp's own releases, one page of notes each. Not to be confused with [CHANGELOG.md](../../CHANGELOG.md), which is
upstream xboxrecomp's history (its `v0.x` tags are upstream's, not ours): our tags are `xrecomp-v<version>`.

| Release | Notes |
|---|---|
| `xrecomp-v0.1.0` | [xrecomp-v0.1.0.md](xrecomp-v0.1.0.md) |

Downloads: the [GitHub releases](https://github.com/MisterAlex95/Xrecomp/releases). Besides the tagged releases,
`xrecomp-latest` is a rolling prerelease rebuilt from every push to `main`: newer, less tested, debuggable.

## What a release contains

- `xrecomp-v<version>.apk`: the XRecomp Android app with the on-device Prepare. arm64-v8a, Android 11+.
  Release-signed (the same key as `xrecomp-latest`, so either updates the other in place) and **not debuggable**.
- `xrecomp-v<version>.apk.sha256`.
- No game, no game data, no generated C: you prepare your own games on your device.

The Mac app and the command-line toolkit are built from the source at the tag (README « Get it »).

## Making a release (maintainers)

1. Pick a commit of `main` whose CI run is green (its on-device toolchain is then cached as `toolchain-<key>`).
2. Write `docs/releases/xrecomp-v<version>.md` (what changed, what works, known problems) and add it to the table
   above; merge that into `main` and let CI go green again.
3. Tag and push: `git tag -a xrecomp-v<version> -m "XRecomp <version>" && git push origin xrecomp-v<version>`.
4. `.gitea/workflows/release.yml` builds the APK (release key, not debuggable, version name
   `<version>+<commit>`) and publishes the release on Gitea and on GitHub with the notes. It stops without
   publishing when the notes file, the release key or `GH_RELEASE_TOKEN` is missing
   ([toolchain.md « Signing »](../android/toolchain.md#signing) lists the secrets).
5. Check the GitHub release page: the APK, its checksum, the notes ending with `signed with: release key`.

Versions follow [semantic versioning](https://semver.org) loosely while we are below 1.0: the minor number goes up
with a release that adds something, the patch number with a release that only fixes.
