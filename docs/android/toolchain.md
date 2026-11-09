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
