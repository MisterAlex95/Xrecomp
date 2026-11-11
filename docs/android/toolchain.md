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

## The on-device toolchain (embedded Prepare)

The on-device Prepare ([prepare.md](prepare.md)) runs the recompilation pipeline on the device and compiles the
generated C into a title pack, with no PC and no Termux. This is the toolchain the APK carries for it: Python with
capstone, and clang + lld. The scripts are in `tools/android/toolchain/`, the versions and checksums in
`tools/android/toolchain/versions.env` (the one place to change them; the table below follows).

```bash
docker build -f tools/docker/android.Dockerfile --target toolchain-out --output type=local,dest=out/toolchain .
cd android && gradle assembleDebug -PxbaToolchainDir=../out/toolchain       # the shell, with the toolchain
docker build -f tools/docker/android.Dockerfile --target prepare-apk-out --output type=local,dest=out .
```

Without `-PxbaToolchainDir` the APK is the plain shell, unchanged (the app hides Prepare when the toolchain is
absent); the CI targets do not build the toolchain.

**On a macOS host, without Docker.** The scripts also run on macOS (Apple silicon or Intel): `common.sh` takes the
NDK's `darwin-x86_64` prebuilts (universal binaries, native on Apple silicon) and falls back to `shasum -a 256` and
`wc -c` where GNU `sha256sum` / `stat` are missing; the scripts stay parseable by macOS's bash 3.2. What the host
needs: the NDK r28c under `$ANDROID_HOME/ndk/28.2.13676358`, Homebrew `cmake` and `ninja`, and a `python3` ≥ 3.12
first in `PATH` (`/usr/bin/python3` is 3.9: put a `python3` → `python3.12` link ahead of it). The output is the
`toolchain-out` layout:

```bash
export ANDROID_HOME=~/Library/Android/sdk ANDROID_NDK_HOME=~/Library/Android/sdk/ndk/28.2.13676358
export XBA_TC_OUT=$PWD/out/toolchain XBA_TC_WORK=/tmp/tc/work XBA_TC_CACHE=/tmp/tc/cache
JOBS=10 tools/android/toolchain/build_llvm.sh &          # beside the other two
JOBS=4 tools/android/toolchain/build_capstone.sh && JOBS=4 tools/android/toolchain/build_python.sh; wait
XBA_TC_CHECK_WORK=/tmp/tc/check tools/android/toolchain/check.sh out/toolchain
```

Without `qemu-aarch64` (not installed on macOS by default) `check.sh` runs the static checks only (jniLibs: ELF
machine, NEEDED, `libxbacc.so` static; assets: `.sha256`) and says it skipped the rest; on Linux qemu stays
required.
On an M1 Max (10 cores), 2026-11-11: LLVM 11 min (host tablegen 65 s, cross build 10 min) with CPython (4.3 min)
and capstone built beside it; the outputs have the sizes of the table below. The NDK unpacked on macOS's
case-insensitive file system keeps one of each pair of netfilter headers that differ only by case
(`xt_CONNMARK.h` / `xt_connmark.h`, ...), so its `sysroot.zip` lacks those few kernel headers; generated title code
never includes them.

| Component | Version | How |
|---|---|---|
| CPython | 3.13.18 | Its official Android build (`Android/android.py`, PEP 738), NDK r28c, API 30. The prebuilt libffi/bzip2/xz/OpenSSL/SQLite it unpacks (BeeWare's `cpython-android-source-deps`, as CPython's own release) are fetched and checked first. |
| capstone | 5.0.9 | CMake with the NDK, X86 only, shared; its pure-Python (ctypes) bindings in `site-packages`. |
| LLVM | 19.1.7 | clang + lld + llvm-ar as one multicall binary (`LLVM_TOOL_LLVM_DRIVER_BUILD`), AArch64 target only, default target `aarch64-linux-android30`, fully static against bionic (`-static`, libc++ static), stripped. 19.1.x is the release line of the NDK r28c clang (19.0.1). |

What the APK gets (the names are those of [prepare.md](prepare.md) and the app's `Toolchain.java`):

| In the APK | What | Size |
|---|---|---|
| `lib/arm64-v8a/libxbapython.so` | the Python executable: `xbapython.c`, `Py_BytesMain` around libpython (CPython's Android build is meant for embedding); `DT_RUNPATH $ORIGIN` | 4.5 KB |
| `lib/arm64-v8a/libpython3.13.so` | libpython | 5.2 MB |
| `lib/arm64-v8a/libcapstone.so` | capstone, X86 only | 2.3 MB |
| `lib/arm64-v8a/libxbacc.so` | clang + ld.lld + llvm-ar, static | 80 MB (35 MB compressed in the APK) |
| `assets/prepare/python-stdlib.zip` | PYTHONHOME: `lib/python3.13/`, the standard library without test, idlelib, tkinter, turtledemo, ensurepip, pydoc_data, venv, sqlite3 and without the modules that need OpenSSL or SQLite (`_ssl`, `_hashlib`, `_sqlite3`: hashlib uses its built-in SHA-2), with `lib-dynload/` and `site-packages/capstone/` | 4.0 MB (14 MB extracted) |
| `assets/prepare/sysroot.zip` | the `--sysroot`: the NDK layout (`usr/include`: the generic and aarch64 headers, no libc++; `usr/lib/aarch64-linux-android/30/`: crt objects and the libc, libm, libdl, liblog, libz, libandroid stubs), plus `lib/clang/19/`: this clang's resource headers (`stddef.h`, `arm_neon.h`, ...), the NDK's `lib/linux/libclang_rt.builtins-aarch64-android.a` and `lib/linux/aarch64/libunwind.a` | 3.5 MB (28 MB extracted) |
| `assets/prepare/toolkit.zip` | made by Gradle (`xbaToolkitAsset`) from this repository at build time: `tools/**/*.py` (no tests, caches, per-game output, Docker/CI/Android scripts) and `tools/recomp/data/`, `templates/new-game/`, `templates/runtime/`, `profiles/`, `include/`, `src/**/*.h`, and `tools/VERSION` (the commit; without git, as in the Docker builds, `files-<sha256 of the files>`) | 0.65 MB (2.1 MB extracted, 196 files) |
| `assets/prepare/*.zip.sha256` | `<hex>  <name>`, checked by the app before it extracts | |

The pipeline imports nothing outside the standard library but capstone (`grep` of `tools/{xiso,xbe_parser,disasm,
func_id,abi_analysis,recomp,prepare}`; unicorn, pefile and numpy are only in tools the Prepare does not run).

APK size (debug, 2026-11-10): the plain shell 3.2 MB, with the toolchain 47.2 MB, so **+44 MB** (35 MB of it is
libxbacc.so, compressed). Installed: 92 MB of extracted jniLibs, 44 MB of extracted assets.

### On the device

The app side is in [app-shell.md](app-shell.md) and the command in [prepare.md](prepare.md). What the toolchain
needs from it:

- Android 10+ apps (target API 29+) may only exec files from `nativeLibraryDir`: the two executables are jniLibs and
  the APK is built with `packaging.jniLibs.useLegacyPackaging` (`extractNativeLibs=true`, checked in the APK).
- `libxbapython.so -m tools.prepare ...` with `PYTHONHOME` = the extracted `python-stdlib.zip`, `PYTHONPATH` = the
  toolkit, `LIBCAPSTONE_PATH` = `nativeLibraryDir`. `sys.executable` is `libxbapython.so`, so the pipeline's child
  steps run the same way (`XBA_PYTHON` says it too).
- `--cc <nativeLibraryDir>/libxbacc.so --cc-prefix-arg clang --sysroot <extracted sysroot.zip>`: the tool is the
  first argument (`libxbacc.so clang ...`, `libxbacc.so ld.lld ...`) and the link is done by ld.lld directly, so
  clang never has to find a linker by name. The embedded clang would look for its resource directory next to its
  binary (in `nativeLibraryDir`), so `tools/prepare/compile.py` passes `-resource-dir=<sysroot>/lib/clang/19`
  and takes the builtins from there when the sysroot carries them (the NDK's sysroot does not: nothing changes on
  a host).

### Checks without a device

There is no emulator on the CI host (no KVM). `tools/android/toolchain/check.sh` (run by the `toolchain` stage):

- every jniLib is an aarch64 ELF that needs only `libc`, `libm`, `libdl`, `liblog`, `libz` or a library we ship;
  `libxbacc.so` needs nothing (static); the zips match their `.sha256`;
- `libxbacc.so` runs under qemu-aarch64 as is: it compiles a C file against the extracted `sysroot.zip` and ld.lld
  links it as a shared object and as an executable;
- `tools.prepare --android` builds the stub title's pack (`tools/conformance/test.xbe`, our synthetic XBE, no game
  content, with `xbe_entry.c` from `tools/android/stub_title.sh`) with the shipped compiler and sysroot, the app's
  flags, against the shell's real `libmain.so` (built there with the NDK): `libtitle.so` needs `libmain.so` and
  exports `recomp_title_main`, `xbe_entry_point`, `sub_00011000`;
- with `XBA_TC_ANDROID_ROOT` (made by `tools/android/toolchain/android_root.sh`: Android 13's `linker64` and
  bionic from Google's emulator system image, through sdkmanager; 3 MB kept, about 1.3 GB downloaded), the dynamic
  parts too: `libxbapython.so` under `qemu-aarch64 -L`, capstone disassembles, and the shipped Python runs the
  whole `tools.prepare --android` above with the app's environment; the executable clang linked runs. The
  `toolchain` stage does not download the image (it runs the pipeline with the image's Python).

qemu-user cannot exec another aarch64 binary without binfmt: `check.sh` reaches the compiler and the step
interpreter through small host scripts that start qemu (`--cc` and `XBA_PYTHON` point at them). On the device
they are the jniLibs themselves.

Run by hand on 2026-11-10 (`XBA_TC_ANDROID_ROOT` set), all green:

| Check | Result |
|---|---|
| jniLibs | `libxbacc.so` static; `libpython3.13.so` needs libc/libm/libdl/liblog; `libxbapython.so` libpython + libc/libdl; `libcapstone.so` libc/libm/libdl |
| clang under qemu | `clang version 19.1.7`, target `aarch64-unknown-linux-android30`; a C file compiled and linked twice in 2 s |
| Python under qemu + bionic 13 | `3.13.18 android aarch64`, `sys.executable` = `libxbapython.so`; capstone 5.0 disassembles; `tools.prepare --help` |
| Executable built by clang | runs under bionic, prints the expected result |
| `tools.prepare --android`, shipped Python + clang | `XBA_RESULT ok` in 15 s under qemu (5 files at -O2, 2 jobs); `libtitle.so`: AArch64 DYN, NEEDED libmain/libm/libdl/liblog/libc, the three symbols; `pack.json` with `"cc": "clang 19.1.7 -O2"` |

Build times on the CI host (6 jobs): LLVM 20 min (host tablegen 80 s, cross build 19 min), CPython 1.5 min (2.5 min
at 3 jobs), capstone seconds. Disk: about 9 GB of LLVM build tree during the build (removed by the stage).

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
