# Android app shell

The APK every title starts from (XBA-18): `android/` (Gradle, AGP 8.7.3) plus `android/app/src/main/cpp`.

```bash
docker build -f tools/docker/android.Dockerfile --target apk-out --build-arg XBA_VERSION=$(git rev-parse --short HEAD) \
    --output type=local,dest=out .       # → out/xboxrecomp-shell.apk
adb install -r out/xboxrecomp-shell.apk
adb logcat -s xboxrecomp SDL
```

## How it is put together

| Piece | What |
|---|---|
| `ShellActivity` | Subclass of SDL's `SDLActivity`, which loads `libSDL2.so` and then `libmain.so`. SDL's Java side is compiled from the pinned sources (`$SDL2_SRC/android-project`), not copied here. |
| `libmain.so` | `shell_main.c` + the **whole** toolkit (`--whole-archive`). Loading it on a device proves that bionic resolves every symbol the runtime uses. Four symbols that a title provides are weak stubs in `shell_main.c`. |
| logcat | `src/platform/android_log.c`: stdout (INFO) and stderr (WARN) go to the tag `xboxrecomp`, one entry per line. `xbox_android_log_init()` is the first call of `SDL_main`. |
| Window | Black, fullscreen, landscape (`sensorLandscape` + `SDL_HINT_ORIENTATIONS`). |
| Back | Trapped (`SDL_HINT_ANDROID_TRAP_BACK_BUTTON`). Pressing it twice within 2 s returns from `SDL_main`, and SDLActivity then finishes. |
| Manifest | minSdk 30, target 35, arm64-v8a only, GLES 3.0 required, `allowNativeHeapPointerTagging="false"`, `isGame`. |
| Per title | `-PxbaApplicationId=…`, `-PxbaLabel=…`, `-PxbaVersionCode=…`. A title replaces `shell_main.c` with its own entry (XBA-33). |
| Signing | Debug key generated once per toolchain image, so updates install in place while the image is cached. The release key will be a build secret, never in the repo. |

## Check list on the device (Alex)

1. `adb install -r xboxrecomp-shell.apk`, then `adb logcat -c && adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG`.
2. Launch « xboxrecomp ». Expected: a black screen, fullscreen, landscape. logcat shows
   `xboxrecomp ready (build <commit>, SDL 2.32.10, <w>x<h>)`.
3. Press Back once. logcat: `press Back again to quit`. The app stays open.
4. Press Back again within 2 s. logcat: `Back pressed twice, quitting`, then `clean exit`, and the app closes.
5. Check there is no tombstone: `adb shell ls /data/tombstones` (root) or no `DEBUG` / `F libc` lines in logcat.
6. Send a screenshot (`adb exec-out screencap -p > shell.png`) and the logcat lines.

## RECOMP_* options (recomp.cfg)

There is no shell environment on Android. The shell therefore reads `recomp.cfg` from the app's external files dir
before anything starts, and puts each line in the environment (`src/platform/recomp_cfg.c`, XBA-26):

```bash
printf 'RECOMP_WAIT_LOG=1\n# comments and blank lines are fine\n' > recomp.cfg
adb push recomp.cfg /sdcard/Android/data/fr.adserver.xboxrecomp.shell/files/recomp.cfg
```

logcat then shows one line: `recomp.cfg: 1 option(s) from …: RECOMP_WAIT_LOG=1`. Without the file it shows
`recomp.cfg: none (read from … when present)`, which is not an error. A variable that is already in the environment
wins over the file. The list of switches: `docs/runtime/environment-variables.md`.

## Fast cores (big.LITTLE)

Right after `recomp.cfg`, the shell calls `recomp_host_cpus_init()` (`src/platform/host_cpus.c`, XBA-25). It restricts
the main thread to the cores whose cpufreq maximum is above the slowest cluster's, before any other thread exists, so
every thread inherits that mask: guest threads, the rasteriser pool, SDL's. On the Thor (Snapdragon 8 Gen 2) that means
cpus 3-7 and not the three A510 cores. logcat: `host cpus: runtime on the fast cores 3-7 of 0-7 (...)`.
`RECOMP_BIG_CORES=0` in `recomp.cfg` turns it off. On desktop it is off unless `RECOMP_BIG_CORES=1`.

## Fault handler (trapped MMIO and real crashes)

Before SDL starts, the shell calls `xbox_PosixInstallMmioFaultHandler()` (XBA-22), and calls it again after
`SDL_Init`. The handler emulates guest accesses to trapped hardware registers (APU, AC'97, USB OHCI). Every other
fault goes to the handler that was there before, debuggerd (and ART in front of it). When the previous action is the
default one, the handler restores it and the instruction faults again, so a real crash gives the usual tombstone at
the right place. If something replaced the handler in the meantime, the second call puts it back in front and chains
to the newcomer.

Device check: `RECOMP_SHELL_CRASH_TEST=1` in `recomp.cfg` makes the shell write to NULL right after the ready line.
Expected: the app closes and logcat shows `Fatal signal 11 (SIGSEGV)` with a backtrace that goes through `SDL_main`
in `libmain.so`. Remove the line afterwards.
