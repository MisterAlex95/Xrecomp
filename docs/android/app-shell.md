# Android app shell

The APK every title starts from (XBA-18): `android/` (Gradle, AGP 8.7.3) plus `android/app/src/main/cpp`.

```bash
docker build -f tools/docker/android.Dockerfile --target apk-out --build-arg XBA_VERSION=$(git rev-parse --short HEAD) \
    --output type=local,dest=out .       # → out/xboxrecomp-shell.apk
adb install -r out/xboxrecomp-shell.apk
adb logcat -s xboxrecomp SDL
```

The CI builds it on every push to `main`. The latest one is always the prerelease **shell-latest** of the repo
(XBA-20): https://git.cf.adserver.fr/Alex/xboxrecomp-android/releases/tag/shell-latest

## How it is put together

| Piece | What |
|---|---|
| `LauncherActivity` | The launcher entry: the game picker ([Choosing the game](#choosing-the-game)), in its own process `:launcher`. With a valid game it starts `ShellActivity` at once and shows nothing. |
| `ShellActivity` | Subclass of SDL's `SDLActivity`, which loads `libSDL2.so` and then `libmain.so`. SDL's Java side is compiled from the pinned sources (`$SDL2_SRC/android-project`), not copied here. Not exported: started by the picker, after it sets `RECOMP_GAME_DIR`. |
| `libxba_import.so` | The picker's ISO import: `xiso_jni.c` around the static `xiso` target (`src/xiso`). Loaded only in `:launcher`. |
| `libmain.so` | `shell_main.c` + the **whole** toolkit (`--whole-archive`) + the title, when one is linked (see [A title in the shell](#a-title-in-the-shell)). Loading it on a device proves that bionic resolves every symbol the runtime uses. Four symbols that a title provides are weak stubs in `shell_main.c`, and so is the title's hook `recomp_title_main`. |
| logcat | `src/platform/android_log.c`: stdout (INFO) and stderr (WARN) go to the tag `xboxrecomp`, one entry per line. `xbox_android_log_init()` is the first call of `SDL_main`. |
| Window | Black, fullscreen, landscape (`sensorLandscape` + `SDL_HINT_ORIENTATIONS`). |
| Back | Trapped (`SDL_HINT_ANDROID_TRAP_BACK_BUTTON`). Once shows a « Press Back again to quit » toast (`QUIT_PROMPT`); twice within 2 s returns from `SDL_main`, and SDLActivity then finishes. With a title, the title's window does the same and ends the process (see [Quit](#quit)). |
| Manifest | minSdk 30, target 35, arm64-v8a only, GLES 3.0 required, `allowNativeHeapPointerTagging="false"`, `isGame`. |
| Per title | `-PxbaTitleDir=…` (the title's project), `-PxbaApplicationId=…`, `-PxbaLabel=…`, `-PxbaVersionCode=…`, `-PxbaXbeSha256=…` (the expected `default.xbe`, [WRONG_GAME](#wrong_game)). The title is linked next to `shell_main.c`, which stays the entry (XBA-52). |
| Assets | `selftest.xbe`: the synthetic XBE of `tools/conformance` (ours, no game content), copied in at build time for the [self-test](#self-test-in-the-app-process). |
| Signing | Debug key generated once per toolchain image, so updates install in place while the image is cached. The release key will be a build secret, never in the repo. |

## Check list on the device (Alex)

1. `adb install -r xboxrecomp-shell.apk`, then `adb logcat -c && adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG`.
2. Launch « xboxrecomp ». Expected: a black screen, fullscreen, landscape. logcat shows
   `xboxrecomp ready (build <commit>, SDL 2.32.10, <w>x<h>)`. Without game data the game picker shows instead
   ([Choosing the game](#choosing-the-game)); push any `default.xbe` to `files/game/` first to reach the black screen.
3. Press Back once. A « Press Back again to quit » toast shows; logcat: `QUIT_PROMPT shown`. The app stays open.
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

## Game data and saves

Right after `recomp.cfg`, the shell resolves two directories (`src/platform/recomp_storage.c`, XBA-23) and hands them to
`xbox_path_init(game_dir, save_dir)`, which maps `D:\` and `\Device\CdRom0\` to the first and the save drives and
partition images to the second:

| What | Where | Why there |
|---|---|---|
| Game data (`default.xbe` and the disc's files) | The folder chosen in the [game picker](#choosing-the-game), else `/sdcard/Android/data/<app id>/files/game/` (where the picker's imports go too), or `RECOMP_GAME_DIR` from `recomp.cfg` | Reachable with `adb push` and a file manager, no permission needed; uninstalling the app removes it. |
| Saves, `Partition0-5.img` | `/data/data/<app id>/files/save/` | Private to the app, kept when it is killed or updated. The cache partition images are sparse: they take no space until written. |

Both are created at start, so the `game/` folder is there to push into. The lookup of `default.xbe` is
case-insensitive, like the kernel's. logcat shows `storage: game=… save=… (default.xbe found|missing)` and the kernel's
`Path init: game=…, save=…, user data=…`.

When `default.xbe` is missing, logcat shows `NO_GAME_DATA: default.xbe not found in …` and, instead of starting
anything (with or without a title linked), the shell opens the game picker naming that folder
(`ShellActivity.openGamePicker`, called from `shell_main.c` over JNI) and finishes. If that call fails it shows the
old « No game data found » box with the folder to copy the disc to. The picker normally catches a missing game before
the shell starts; this is the path for a folder that went away in between, or `RECOMP_GAME_DIR` in `recomp.cfg`
pointing nowhere.

Device check (XBA-23):

1. Start the shell once with nothing pushed. Expected: the game picker, « No game is set up », and logcat
   `picker: shown (no game set up)`; nothing from the runtime, which has not started.
2. Push any file named `default.xbe` (one of our nxdk samples will do):
   `adb push default.xbe /sdcard/Android/data/fr.adserver.xboxrecomp.shell/files/game/`, then start the app again.
   Expected: no picker, logcat `storage: … (default.xbe found)` and `game data: …/game/default.xbe`.
3. `adb shell run-as fr.adserver.xboxrecomp.shell ls -ls files/save` (debug APK). Expected: `TitleData`,
   `UserData`, `Cache`, `SystemData` and `Partition0.img` … `Partition5.img`, the last ones taking about 0 blocks.
4. Kill the app (`adb shell am force-stop fr.adserver.xboxrecomp.shell`), restart it, run step 3 again: the same
   files, still there.
5. Send the logcat lines and the `ls` output.

### Extracting a disc image in the app (XBA-40)

When the player picks an Xbox disc image (XISO), the app extracts it once into the game folder, because the runtime
reads plain files. It does this with `src/xiso` (target `xiso`, header `xiso.h`, libc only), which takes the fd that
the ContentResolver opened and only needs `pread` on it:

- `xiso_probe_fd` reads the volume, checks the whole directory tree and returns the title from `default.xbe`'s
  certificate, the total size, and whether the root holds a `default.xbe`. The app can show these and check the free
  space before it writes anything.
- `xiso_extract_fd(fd, out_dir, progress, user)` writes into `out_dir.partial`. Once every file is written, it
  replaces `out_dir` as a whole (the old one goes to `out_dir.old`, which is then deleted). If the run is cancelled
  (the callback returns nonzero) or fails, `out_dir` is untouched and `.partial` is removed. Progress is reported per
  file and every 4 MB.
- Images: a bare game partition (extract-xiso, xdvdfs) or a redump full-disc image (game partition at 0x18300000).
  If the image is corrupt (truncated, offsets out of range, cycles in the tree, names like `..` or with `/`), the call
  returns `XISO_ECORRUPT` and nothing is written outside `out_dir`. `xiso_strerror` gives a message for each code.

On a desktop, `xiso_extract image.iso outdir` (built by `tests/xiso`, `tools/xiso/README.md`) does the same.

In the app the picker calls it into `files/game.import`, never into `files/game` itself, and swaps the result in
once checked ([Choose disc image (ISO)](#choose-disc-image-iso)).

## Choosing the game

(XBA-54, and the import half of XBA-40.) The runtime reads the game with POSIX calls (`open`, `opendir`, the kernel's
case-insensitive lookup), so it needs a real path, not a `content://` URI. The app's launcher entry is therefore a
small picker, `LauncherActivity` (framework widgets, no AndroidX), in its own process `:launcher`: it never loads
`libSDL2.so` or the runtime, and after a new choice it can end the game's process and start a fresh one (the runtime
maps its memory once per process).

### When it shows

| Case | What happens |
|---|---|
| A remembered folder that holds `default.xbe` (any case) | `ShellActivity` starts at once; the picker shows nothing. |
| Nothing remembered, but `RECOMP_GAME_DIR` in `recomp.cfg`, or `default.xbe` pushed to `files/game/` | Same: the adb workflow still works. |
| Nothing set up | The picker: « No game is set up ». |
| The remembered folder is gone (deleted, SD card out) | The picker, naming the old path (`GAME_FOLDER_GONE`); the previous choice stays until a new one is made. |
| The folder is there but unreadable because All files access was turned off | The picker says so (`GAME_FOLDER_NO_ACCESS`). |
| The native side found no `default.xbe` (`NO_GAME_DATA`) | `shell_main.c` calls `ShellActivity.openGamePicker(dir)`; the picker shows `NO_GAME_DATA` with that folder. |
| Long-press on the app icon, « Change game » | The picker, even with a game set up. A dynamic shortcut (the package name differs per title, which a static `shortcuts.xml` cannot follow), published each time the picker runs, so it appears after the first start. If the game is running, the picker says it will close when another game is chosen; saves are in internal storage and stay. |

logcat (tag `xboxrecomp`): `picker: shown (...)`, then one `picker:` line per step.

### The screen

One screen, landscape or portrait, usable with a gamepad (D-pad focus on the buttons):

- the app's name; the state (« Game: TITLE / Folder: PATH », or why none is usable); a message line for errors;
- **Choose game folder**, **Choose disc image (ISO)**, **Start the game** (enabled when the current game is usable);
- while copying or extracting: a label, a progress bar with « X of Y » and the current file, and **Cancel**. The
  buttons are off, the screen stays on (`FLAG_KEEP_SCREEN_ON`), Back cancels too. The work runs on a background
  thread; the activity handles rotation itself so it survives.

Texts are string resources, English (`res/values`) and French (`res/values-fr`), named by key: `NO_GAME_SET_UP`,
`NO_GAME_DATA`, `NOT_A_GAME_FOLDER`, `NOT_AN_ISO`, `NOT_A_GAME_IMAGE`, `WRONG_GAME`, `NOT_ENOUGH_SPACE`, ...

### Choose game folder

1. Without All files access (`MANAGE_EXTERNAL_STORAGE`), a dialog explains: with it the game is read in place from
   the folder (phone or SD card, no copy, fine for a 4 GB disc, only that folder is read); without it the folder is
   copied into the app's storage (as much free space again, a few minutes, removed with the app). « Allow all files
   access » opens the system's settings page for the app; on return the folder picker opens either way. « Copy
   instead » opens it directly.
2. The system folder picker (`ACTION_OPEN_DOCUMENT_TREE`).
3. **With the permission**, the tree URI is mapped to a path: the external storage provider's `primary:Games/Halo`
   is `/storage/emulated/0/Games/Halo`, `1A2B-3C4D:Halo` is `/storage/1A2B-3C4D/Halo` (SD card), `home:X` is
   `Documents/X`; the downloads provider's `raw:/path` is a path already. No `default.xbe` there: `NOT_A_GAME_FOLDER`,
   and the picker can be opened again. Otherwise the [hash check](#wrong_game), then the folder is remembered as is.
4. **Without the permission** (or a provider that is not a storage volume, or a path that cannot be listed): the tree
   is listed through `DocumentsContract`, `default.xbe` checked (`NOT_A_GAME_FOLDER`) and hashed from the provider
   before anything is copied, the free space checked against the total, then everything is copied to
   `files/game.import/` with progress and Cancel, checked again and swapped in place of `files/game/` (the old
   `game/` becomes `game.old/` and is removed once the new one is in). `files/game` is remembered.

### Choose disc image (ISO)

1. The system file picker (`ACTION_OPEN_DOCUMENT`, any type: providers rarely know a MIME type for `.iso`); a name
   that does not end in `.iso` or `.xiso` gives `NOT_AN_ISO`.
2. A `ParcelFileDescriptor` is opened on it, and its fd goes to `libxba_import.so` (`XisoNative`, JNI in
   `android/app/src/main/cpp/xiso_jni.c`): `xiso_probe_fd` gives the title, the total size of the files and whether
   `default.xbe` is in the root. Not an XISO, or no `default.xbe`: `NOT_A_GAME_IMAGE` with the reason
   (`xiso_strerror`).
3. Free space in the external files dir must be the total plus 64 MB, or `NOT_ENOUGH_SPACE` names both.
4. `xiso_extract_fd` extracts into `files/game.import/` (through `.partial`, renamed at the end) on the background
   thread; its progress callback calls back into Java (at each file start and at most ten times a second), which
   updates the bar and returns Cancel. A cancel or an error leaves `files/game/` untouched.
5. The extracted `default.xbe` is hashed ([WRONG_GAME](#wrong_game)), then `game.import/` is swapped in place of
   `game/` as above and remembered.

Why a separate JNI library rather than `libmain.so`: `libmain.so` needs `libSDL2.so` loaded first and brings the
whole runtime (`--whole-archive` toolkit, the title) into a process that would never run it. `libxba_import.so` is
`xiso_jni.c` plus the static `xiso` target (libc only) and `liblog`.

### WRONG_GAME

A title's APK is made for one disc. `-PxbaXbeSha256=<sha256sum of default.xbe>` bakes the expected hash in (string
resource `xba_xbe_sha256`, 64 hex digits, checked by Gradle); empty, as for the shell and the stub, means no check.
The picker computes the SHA-256 of `default.xbe` of the chosen folder (in place), of the provider's file (before a
copy) or of the extracted tree (after an extraction, before the swap). A mismatch shows `WRONG_GAME` and changes
nothing: the previous choice is kept, `files/game/` is untouched, the staging folder is removed. logcat names the
hash: `picker: ... sha256 <hex>`.

### How the path reaches the runtime

The choice is in the app's SharedPreferences (`game`: `game_dir`, `title`), written with `commit()` before the
game's process starts. `ShellActivity.onCreate` calls `Os.setenv("RECOMP_GAME_DIR", dir)` before
`super.onCreate()`: `SDL_main` runs later on SDLThread, and `recomp_app_start` → `recomp_storage_resolve` reads the
variable (the process environment is libc's, so `getenv` sees it). An environment variable wins over `recomp.cfg`,
so a remembered choice wins over a `RECOMP_GAME_DIR` line there. Saves stay in internal storage whatever the game
folder.

### Device check (XBA-54, XBA-40), for Alex

Use the stub APK (**shell-latest**) and its `stub-default.xbe` as the « game »: a folder holding only it is enough
for the stub title. `adb logcat -c && adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG` in a terminal throughout.

1. Install the APK with nothing configured (`adb uninstall` first). Expected: the picker, « No game is set up »,
   logcat `picker: shown (no game set up)`.
2. Copy `stub-default.xbe` as `default.xbe` into a folder of internal storage (e.g. `Download/stubgame/`).
   « Choose game folder », « Allow all files access », turn it on, come back: the folder picker opens. Pick the folder.
   Expected: a « Starting ... » toast, then the stub title starts (`storage: game=/storage/emulated/0/Download/stubgame
   ... (default.xbe found)`, `stub title: all 5 lifted functions OK`).
3. Kill the app (swipe it from recents) and start it again: it starts directly, no picker.
4. Long-press the app icon: « Change game ». Expected: the picker, « Game: ... / Folder: ... », and the note that the
   running game will close. Pick an empty folder: `NOT_A_GAME_FOLDER`; the picker can be opened again.
5. Repeat step 2 with the folder on the SD card if the Thor has one (expected path `/storage/XXXX-XXXX/...`).
6. Folder gone: rename the folder of step 2 with a file manager, start the app. Expected: the picker naming the old
   path (`GAME_FOLDER_GONE`), no crash.
7. Copy fallback: Settings › Apps › the app › All files access off. « Choose game folder », « Copy instead », pick the
   folder of step 2. Expected: the progress bar, then the game starts from
   `/storage/emulated/0/Android/data/<app id>/files/game`.
8. ISO: make an XISO of our own test data (e.g. `extract-xiso -c stubgame stub.iso`, or any image dumped from your
   own disc), push it to `Download/`. « Choose disc image (ISO) », pick it. Expected: the progress bar with file
   names, the screen stays on, then the game starts from `files/game`. Do it once more and press Cancel halfway:
   « Import canceled », and the previous game still starts.
9. Pick a file that is not an ISO (e.g. a photo): `NOT_AN_ISO`.
10. WRONG_GAME needs an APK built with `-PxbaXbeSha256=` of another XBE (ask for one, or build one): picking the stub
    folder then shows `WRONG_GAME`, and the previous choice still starts.
11. Switch the phone to French: the picker's texts are in French.
12. Send screenshots of the picker (steps 1, 4, 8) and the logcat lines (`picker:`, `import:`, `storage: game=...`).

## Background and resume

(`src/platform/recomp_lifecycle.c`, XBA-24.) On Android SDL already blocks the thread that pumps its events while the
app is in the background, and pauses its audio there. The title's own threads would keep running, so:

- an SDL event watch (in the title's window, `fb_present_sdl.c`, and in the shell) calls `recomp_lifecycle_pause()` on
  `SDL_APP_WILLENTERBACKGROUND` and `recomp_lifecycle_resume()` on `SDL_APP_DIDENTERFOREGROUND`;
- the flip (`xbox_FramebufferWindowPresent`) holds there while paused, on a condition variable: no CPU. The title's
  other threads then stop behind it, as they wait for the GPU;
- the SDL audio device is paused by a listener at once, not at the next flip, and plays its queue on at resume;
- if the GL context was lost (`SDL_RENDER_DEVICE_RESET`), the window remakes its texture from the last frame;
- logcat shows `lifecycle: paused (background)` and `lifecycle: resumed after N ms`.

Guest time is not frozen: after a long pause the title sees the time that passed, as after a long frame. Most titles
clamp their frame time; if one does not, that becomes its own ticket.

Device check (XBA-24), shell part (the in-game part waits for a title, XBA-35):

1. Start the shell, press Home. logcat: `lifecycle: paused (background)`.
2. `adb shell top -b -n 1 | grep xboxrecomp`: under 2 % CPU.
3. Wait 5 minutes, bring the app back. logcat: `lifecycle: resumed after ~300000 ms`; the black screen is back, no
   crash, no tombstone.
4. Do it once more with a notification pulled down and with the screen turned off and on.
5. Back once: the toast; Back again within 2 s: `clean exit`.
6. Send the logcat lines and the `top` line.

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

Device check: `RECOMP_SHELL_CRASH_TEST=1` in `recomp.cfg` makes the shell write to NULL right after the ready line
(shell without a title, or without game data). Expected: the app closes and logcat shows
`RECOMP_SHELL_CRASH_TEST: writing to NULL now`, then `Fatal signal 11 (SIGSEGV)` with a backtrace that goes through
`SDL_main` in `libmain.so`. Remove the line afterwards. The shell no longer waits before the write: the line is there
because the handler drains the log pipes first ([Last lines before a crash](#last-lines-before-a-crash)).

## Self-test in the app process

(XBA-51.) The device test bundle ([device-tests.md](device-tests.md)) runs from `adb shell`: SELinux domain `shell`, no
ART, no libsigchain, an almost empty address space. An app runs as `untrusted_app` under ART, with libsigchain in front
of `sigaction` and hundreds of libraries mapped, so a pass in the bundle does not prove the app. With
`RECOMP_SHELL_SELFTEST=1` in `recomp.cfg`, the shell runs the runtime's risky pieces in its own process, right after
`SDL_Init` and the second fault-handler install, before the window or a title:

| Check | What it proves in the app |
|---|---|
| `layout` | `xbox_MemoryLayoutInit` on `selftest.xbe` (the APK asset, read with `SDL_LoadFile`; the layout takes the bytes, no copy on disk); the base is host-page aligned. |
| `window` | The whole 4 GB guest window is held (`RECOMP_GUEST_WINDOW`, on by default on Android) and `/proc/self/maps` covers all of it. On Android, a refused 4 GB reservation (the layout then falls back to fixed addresses) fails. |
| `mirrors` | A write and a read through each of the 28 RAM mirrors (memfd views, XBA-21), seen at the base, and the other way round. |
| `tiled_alias` | The same between the tiled aperture and the contiguous window (two views of the other memfd). |
| `mmio_handler` | The toolkit's SIGSEGV handler is still in front, and one host page of guest RAM traps (`PAGE_NOACCESS`, routed by `xbox_PosixMmioSetTestWindow` to a test device). |
| `mmio_imm`, `mmio_extend`, `mmio_regoff`, `mmio_writeback`, `mmio_unscaled`, `mmio_pair`, `mmio_simd` | One trapped access of each A64 decoder class through that page, by the real chain: libsigchain, the toolkit's handler, `mmio_decode_a64.h`, the register write-back into the signal frame. `mmio_simd` is `ldr q` / `str q` / `ldr d`: the V register written in the frame's `fpsimd_context` (XBA-45). (On x86-64 Linux: `mmio_mov`, `mmio_extend`, `mmio_immstore`, `mmio_xmm_scalar`, `mmio_simd` = `movups`.) |
| `exec_alloc` | `NtAllocateVirtualMemory` with `PAGE_EXECUTE_READWRITE` succeeds, read / write, and the mapping is not executable. |
| `exec_guest` | `MmSetAddressProtect` with `PAGE_EXECUTE_READ` (read-only) and `PAGE_EXECUTE_READWRITE` on guest RAM (a memfd view) takes effect, without `PROT_EXEC`. |
| `save_dir` | A file created, written, `fsync`ed, read back and removed in the save directory. |
| `reinit` | Shutdown, a second init and shutdown again: a title maps its own layout after the self-test, in the same process. |

Each prints `selftest NAME: PASS` (sometimes with a note) or `selftest NAME: FAIL (reason)`, then
`selftest: N passed, M failed`. The shell then carries on as without the option (its window, or the title). The checks
never fault on purpose: whether an address is mapped is asked of the kernel (a pipe write or read returns `EFAULT`).
A trapped access the decoder cannot service is a crash, as in a title; the fault handler drains logcat first, so the
last `PASS` line says which check came next. The same code is the CMake test `tests/recomp_selftest` (CI on x86-64,
arm64 under qemu-user, and the device bundle), so a check that fails only in the app is an Android difference.
Under qemu-user `mmio_simd` fails (the `mmio_decode_a64` lead, XBA-45): the arm64 job expects `recomp_selftest` to
fail and gates `recomp_selftest_no_simd` (`RECOMP_SELFTEST_SKIP=mmio_simd`).

**No executable guest memory.** The POSIX Win32 shim (`prot_from_page` in `src/platform/win32_compat.c`) no longer maps
`PAGE_EXECUTE*` to `PROT_EXEC`: read / write are kept, `PAGE_EXECUTE` alone becomes `PROT_READ`. Guest code is
recompiled ahead of time and never runs from guest memory, and the runtime has no JIT, while an executable anonymous
or memfd mapping can be refused to an app (SELinux `execmem`). Every guest protection reaches the host through that
function (`VirtualAlloc`, `VirtualProtect`; file-mapping views were never executable), and `VirtualQuery` already
reported every mapping as `PAGE_READWRITE`.

### Device check (XBA-51), for Alex

1. Install the shell APK (**shell-latest**), push a `recomp.cfg` with `RECOMP_SHELL_SELFTEST=1`:
   `printf 'RECOMP_SHELL_SELFTEST=1\n' > recomp.cfg`,
   `adb push recomp.cfg /sdcard/Android/data/fr.adserver.xboxrecomp.shell/files/recomp.cfg`.
2. Start the app, then `adb logcat -d -s xboxrecomp`.
3. Expected: every `selftest` line `PASS` and the summary `selftest: 16 passed, 0 failed`; no tombstone
   (`adb logcat -d -b crash` empty).
4. Run it a second time after a reboot (the address space layout changes).
5. Send the logcat lines (both runs); remove the option (`adb shell rm
   /sdcard/Android/data/fr.adserver.xboxrecomp.shell/files/recomp.cfg`).

## A title in the shell

(XBA-52.) A title does not copy or replace `shell_main.c`. It defines one function, and the shell calls it:

```c
#include "platform/recomp_storage.h"
int recomp_title_main(const recomp_storage *store);   /* store->game_dir, ->save_dir, ->xbe */
```

1. `SDL_main` calls `recomp_app_start()` (`src/platform/recomp_app.c`): logcat, `recomp.cfg`, the Android defaults
   below, storage and `xbox_path_init`, fast cores, the fault handler. A title gets all of them by being linked in.
2. `SDL_Init`, the fault handler checked again, `xboxrecomp ready (build …, SDL …, title)`.
3. When `default.xbe` is found and a title is linked, `xbox_HostWindowMain(title_boot)`: the window on the main
   thread, `recomp_title_main(&store)` on a 64 MB guest thread, the lifecycle watch, Back twice to quit. When the
   guest returns, the window stays on its last frame (`[FBWIN] guest returned (N); Back twice to quit`): an app does
   not vanish on its own.
4. No title linked (the weak `recomp_title_main` is NULL), or no game data: the shell's own black window, as before.

`templates/new-game` is such a title. Built through the app, its `CMakeLists.txt` sees `XBOXRECOMP_SHELL_TITLE` and
makes the object library `xba_title` instead of an executable (the toolkit is the shell's), and its `main.c` defines
`recomp_title_main` instead of `main`, taking the XBE path, `game_dir` and `save_dir` from `store` instead of the
`YOUR_GAME_*` constants. The rest of its boot (memory layout, kernel, bridge, `xbe_entry_point`) is unchanged.

```bash
# the title project: templates/new-game + its generated C in src/recomp/gen (docs/GETTING_STARTED.md)
cd android && gradle assembleDebug -PxbaTitleDir=/path/to/my_xbox_game \
    -PxbaApplicationId=fr.adserver.xboxrecomp.mygame -PxbaLabel="My game"
# or the NDK alone: cmake -S android/app/src/main/cpp -DXBA_TITLE_DIR=/path/to/my_xbox_game (+ the NDK toolchain)
```

### The stub title

`tools/android/stub_title.sh OUT` makes a title from `templates/new-game` and the synthetic XBE of `tools/conformance`
(ours: five leaf functions, no game content), lifted by the pipeline, plus an `xbe_entry_point` that calls the five
and checks what they return. The CI builds its APK (`--target stub-apk` of `tools/docker/android.Dockerfile`) on every
push to `main`, after the shell, and attaches `xboxrecomp-stub-<commit>.apk` and `stub-default.xbe` to the
**shell-latest** prerelease. On Linux the same project builds as the template's executable and prints the same lines
(x86-64, and arm64 under qemu).

### Android defaults

After `recomp.cfg`, for what neither it nor the environment sets (`recomp_app_set_defaults`):

| Variable | Default on Android | Why |
|---|---|---|
| `RECOMP_FB_WINDOW` | `1` | Without it `xbox_HostWindowMain` runs the guest on SDL's thread: no event loop (no pause, no Back) and a 1 MB stack. |
| `RECOMP_PB_EXEC` | `1` | The pushbuffer executor: its flips are what the window shows. |

`RECOMP_HEADLESS=1` still keeps the window off. `RECOMP_FB_WINDOW=0` or `RECOMP_PB_EXEC=0` in `recomp.cfg` turn either
off (the kernel reads `RECOMP_PB_EXEC` by presence, so the `=0` is removed from the environment). logcat:
`defaults: RECOMP_FB_WINDOW=1 RECOMP_PB_EXEC=1 (Android; …)`.

### Quit

Back twice in the title's window calls `recomp_quit(0)` (`src/platform/recomp_quit.c`): the exit hook (the kernel's
missing-file summary), `xboxrecomp: clean exit`, `fflush` of stdout and stderr, the log pump drained (at most
500 ms), then `_exit(0)`. Not `exit()`: the guest's threads are still running, and atexit handlers and destructors must
not run under them. The other atexit reports (`RECOMP_PROFILE`, the ICALL feedback dump, a WAV capture) are desktop
diagnostics and are skipped on Android. On desktop hosts `recomp_quit` is `exit()`, as before.

### Last lines before a crash

stdout and stderr reach logcat through pipes read by two pump threads (`src/platform/android_log.c`); a line still in
a pipe when the process dies is lost. So before the fault handler hands a fault that is not guest MMIO to debuggerd,
it calls `xbox_log_drain(200)`: it waits, at most 200 ms, until both pipes are empty and the pump threads have written
what they read, and ends a half-written line with a `\n` so that it goes out too. That runs in a signal handler, so it
only uses lock-free atomics and plain system calls (`ioctl(FIONREAD)`, `clock_gettime`, `nanosleep`, a 1-byte write
into an empty pipe); it never logs by itself (`__android_log_write` takes locks the crashing thread may hold), it only
waits for the pump threads to do it. Trade-offs: a pump thread stuck behind the crashing thread is not waited for past
200 ms (its lines are lost, the crash is not held up); a crash on a pump thread does not wait for itself; a second
fault during the drain returns at once; a partial stdout line still in stdio's buffer (no `\n`, no `fflush`) is not
flushed, as `fflush` is not async-signal-safe. When the pipes are empty the drain returns at once, so it costs nothing
on the way to a tombstone in the usual case. `tests/mmio_fault_chain_posix` (`drain`) checks it on Linux with a slow
sink.

### Device check (XBA-52), for Alex

1. From **shell-latest**, install the stub APK and push its XBE, then start « xboxrecomp stub »:
   `adb install -r xboxrecomp-stub-<commit>.apk`,
   `adb push stub-default.xbe /sdcard/Android/data/fr.adserver.xboxrecomp.stub/files/game/default.xbe`,
   `adb logcat -c && adb logcat -s xboxrecomp SDL DEBUG AndroidRuntime`. Expected in logcat: `defaults: RECOMP_FB_WINDOW=1
   RECOMP_PB_EXEC=1`, `xboxrecomp ready (… title)`, `starting the title`, the boot lines (`xbox_MemoryLayoutInit:
   complete`, `Starting game...`), five `stub title: …` lines and `stub title: all 5 lifted functions OK`, then
   `[FBWIN] guest returned (0); Back twice to quit`. The screen is black (the stub draws nothing).
2. Home, wait 10 s, come back: `lifecycle: paused (background)`, then `lifecycle: resumed after ~10000 ms`.
3. Back twice: `[FBWIN] Back pressed twice, quitting`, `xboxrecomp: clean exit`, the app closes; `adb logcat -b crash`
   is empty.
4. Crash lines: push a `recomp.cfg` with `RECOMP_SHELL_CRASH_TEST=1` to the **shell** app (no title) and start it: the
   `writing to NULL now` line is in logcat above the `Fatal signal 11` and the backtrace. Remove the file afterwards.
5. Send the logcat lines.
