# Android toolchain

The image `tools/docker/android.Dockerfile` (XBA-16) holds everything needed to build for Android. Builds use it in
CI and locally, so they are the same everywhere.

```bash
docker build -f tools/docker/android.Dockerfile --target android -t xba-android .
docker run --rm -it -v "$PWD:/src" xba-android bash
```

## Versions

Pinned as `ARG`s at the top of the Dockerfile. Change both places together.

| Component | Version | Notes |
|---|---|---|
| Base | Debian trixie-slim | |
| NDK | r28c (28.2.13676358), clang 19.0.1 | `$ANDROID_NDK_HOME`. The spec asks for r27+. BananaNative uses the same NDK. |
| SDK platforms | android-34, android-35 | minSdk 30 (decision XBA-10), compile/target 35. |
| Build tools | 35.0.0 | |
| Command-line tools | 13114758 | as in Table Factory |
| CMake | 3.31.6 (Debian), Ninja | The SDK's CMake is not installed. |
| Gradle | 8.10.2, in `/opt/gradle` | Pre-installed so that builds do not download a wrapper distribution. |
| JDK | Temurin 17 (17.0.21) | trixie has no OpenJDK 17 package. |
| SDL2 | `release-2.32.10`, sources in `$SDL2_SRC` | `android-project/` is the SDLActivity template. |

The spec said SDL2 2.30.x. We took 2.32.10, the latest SDL2 release: the same API, more Android fixes.

`/etc/xba-toolchain.txt` in the image lists what was actually installed.

## Checks (2026-11-09)

- Cold build: 3 min 35 s. Rebuild with no change: 5 s (from cache).
- Smoke test (CI job `android-image`): SDL2 builds for arm64-v8a, API 30 →
  `ELF 64-bit ARM aarch64, for Android 30, built by NDK r28c`.

## Secrets

The release keystore is never in the image. When an APK is signed, it comes in at build time as a BuildKit secret
(`--secret id=keystore,...`), as in Table Factory.

## The toolkit for Android

```bash
docker build -f tools/docker/android.Dockerfile --target toolkit .
```

This builds every library of the `xboxrecomp` umbrella target for arm64-v8a (API 30), plus `libSDL2.so` from the
pinned sources, and lists them in `/reports/android-artefacts.txt`. It fails if libepoxy, X11 or OpenSSL show up in
the build. What differs from desktop POSIX (XBA-17):

| Where | Android |
|---|---|
| `CMakeLists.txt` | SDL2 from `$SDL2_SRC` as a subproject (shared `libSDL2.so`, which SDLActivity loads). `find_package(SDL2)` only runs when there is no `SDL2::SDL2` target yet. |
| `src/d3d` | GLES 3.0 (`GLESv3`) through `gl_host.h` instead of libepoxy. It compiles, and nothing more: rendering is XBA-27 (e.g. `GL_BGRA` as a vertex attribute size is desktop-only). |
| `src/platform/win32_compat.c` | Thread affinity through `sched_{set,get}affinity` on the bionic tid. `SecureZeroMemory` without `explicit_bzero`. `TerminateThread` logs instead of `pthread_cancel` (temporary, XBA-25). |
| `src/kernel` | No OpenSSL anywhere (desktop too): nothing called it, `kernel_crypto.c` has its own SHA-1. |

`liblog` and `libandroid` are not linked yet: nothing in the toolkit calls them before the app shell (XBA-18).

## CI gating

XBA-53. `.gitea/workflows/ci.yml` runs on the Docker of the host that also serves Gitea and Plane, so what runs when
is chosen to keep that host up (on 2026-11-09, heavy jobs in parallel took Gitea and Plane down).

| When | Jobs | Red when |
|---|---|---|
| Every PR | `linux`, then `android-toolkit` | a Linux test fails; the toolkit does not build with the NDK |
| Push to `main`, or by hand | `linux`, then `linux-arm64`, then `android-image` (publishes `shell-latest`) | a Linux test fails; an arm64 failure that is not expected; the image, the toolkit or the shell APK does not build |
| Monday 02:30 (server time), or by hand with `device_tests` | `linux`, then `device-tests` | a test project that should build for Android does not |

- **`android-toolkit`** builds the `toolkit` target of `android.Dockerfile` (compile only, `JOBS=4`) when the PR
  touches `src/`, `include/`, `android/`, `cmake/`, the root `CMakeLists.txt`, `tools/docker/android.Dockerfile` or
  the workflow. A first step diffs the PR against its merge base and decides; other PRs pass the job in seconds.
  The Android image layers are cached on the host: the compile takes a few minutes.
- **`linux-arm64`**: the tests run under qemu-user to the end whatever fails, the image prints the report, then
  `tools/ci/check_expected.py ctest` compares the JUnit files of ctest with `tools/ci/arm64-expected.txt`. Any failure
  not listed fails the build, and so does a listed test that no longer fails (pass, skip or gone): remove its line,
  in the same PR as the fix. Lines are `<project>/<test>` (`root/<test>` for the root build) or `<project>` for a
  project that does not configure or build.
- **`device-tests`** builds the `device-tests` stage (about 12 minutes) and prints its `build-report.md`;
  `tools/ci/check_expected.py android` compares it with `tools/ci/android-unbuilt.txt`, the projects that are not
  built for Android (Windows-only ones, `input_keyboard_sdl`). Same rule both ways. The bundle stays in the image.
- **One heavy job at a time.** Every job but `linux` takes `tools/ci/heavy_lock.sh` before its Docker build and
  gives it back at the end. The lock is a created, never started container named `xba-ci-heavy` on the host's
  Docker (`docker create --name` is atomic); a job that waits prints who holds it every 5 minutes. A holder whose job
  container is gone (cancelled run) loses the lock at once; one that cannot be identified, after 4 hours.
  Not `concurrency:`: Gitea keeps one running and **one** pending job per group, and a newer job cancels the one
  already waiting, so with a main push chaining two heavy jobs a PR's job would be cancelled routinely. A stuck lock
  can be removed by hand: `docker rm xba-ci-heavy` on the host.
- No tmpfs, `JOBS` at 4 in every image, the `linux` job is not under the lock (it ran on every PR before XBA-53).

To check the expected-failure files by hand:

```bash
python3 tools/ci/check_expected.py ctest --expected tools/ci/arm64-expected.txt --report c-tests.md --junit junit/
python3 tools/ci/check_expected.py android --expected tools/ci/android-unbuilt.txt --report build-report.md
```
