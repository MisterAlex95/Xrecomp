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
| `LauncherActivity` | The launcher entry ([The launcher](#the-launcher)): welcome, game choice and import, home (the game's hero with Play / Prepare, then Settings, Controls, Game data, About), in its own process `:launcher`. `Theme.java` and `Widgets.java` draw it, `TitleArt.java` decodes the game's title image, `UiFeedback.java` plays its sounds and haptics; `RecompConfig.java` writes its settings to `recomp.cfg`. With a valid game and « Open the game directly next time » on, it starts `ShellActivity` at once and shows nothing. |
| `ShellActivity` | Subclass of SDL's `SDLActivity`, which loads `libSDL2.so` and then `libmain.so`. SDL's Java side is compiled from the pinned sources (`$SDL2_SRC/android-project`), not copied here. Not exported: started by the launcher, after it sets `RECOMP_GAME_DIR`. |
| `libxba_import.so` | The launcher's ISO import: `xiso_jni.c` around the static `xiso` target (`src/xiso`). Loaded only in `:launcher`. |
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
2. Launch « XRecomp ». Expected: a black screen, fullscreen, landscape. logcat shows
   `xboxrecomp ready (build <commit>, SDL 2.32.10, <w>x<h>)`. Without game data the launcher's welcome screen shows
   instead ([The launcher](#the-launcher)); push any `default.xbe` to `files/game/` first and press Play on the
   launcher's home: the shell then shows a « Game data found … This APK has no recompiled game in it » message box
   (`NO_TITLE`, logcat `(no title linked)`); OK, and the black screen stays.
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
wins over the file. The list of switches: `docs/runtime/environment-variables.md`. The launcher's Settings screen
writes a few of them into the same file and keeps the lines written by hand ([Settings](#settings)).

## Game data and saves

Right after `recomp.cfg`, the shell resolves two directories (`src/platform/recomp_storage.c`, XBA-23) and hands them to
`xbox_path_init(game_dir, save_dir)`, which maps `D:\` and `\Device\CdRom0\` to the first and the save drives and
partition images to the second:

| What | Where | Why there |
|---|---|---|
| Game data (`default.xbe` and the disc's files) | The folder chosen in the [launcher](#choosing-the-game), else `/sdcard/Android/data/<app id>/files/game/` (where the launcher's imports go too), or `RECOMP_GAME_DIR` from `recomp.cfg` | Reachable with `adb push` and a file manager, no permission needed; uninstalling the app removes it. |
| Saves, `Partition0-5.img` | `/data/data/<app id>/files/save/` | Private to the app, kept when it is killed or updated. The cache partition images are sparse: they take no space until written. |

Both are created at start, so the `game/` folder is there to push into. The lookup of `default.xbe` is
case-insensitive, like the kernel's. logcat shows `storage: game=… save=… (default.xbe found|missing)` and the kernel's
`Path init: game=…, save=…, user data=…`.

When `default.xbe` is missing, logcat shows `NO_GAME_DATA: default.xbe not found in …` and, instead of starting
anything (with or without a title linked), the shell opens the launcher on its game choice, naming that folder
(`ShellActivity.openGamePicker`, called from `shell_main.c` over JNI) and finishes. If that call fails it shows the
old « No game data found » box with the folder to copy the disc to. The launcher normally catches a missing game before
the shell starts; this is the path for a folder that went away in between, or `RECOMP_GAME_DIR` in `recomp.cfg`
pointing nowhere.

Device check (XBA-23):

1. Start the shell once with nothing pushed. Expected: the launcher's welcome screen, and logcat
   `launcher: shown (no game set up)`; nothing from the runtime, which has not started.
2. Push any file named `default.xbe` (one of our nxdk samples will do):
   `adb push default.xbe /sdcard/Android/data/fr.adserver.xboxrecomp.shell/files/game/`, then start the app again.
   Expected: the launcher's home, chip « Ready »; Play: logcat `storage: … (default.xbe found)` and
   `game data: …/game/default.xbe`.
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

In the app the launcher calls it into `files/game.import`, never into `files/game` itself, and swaps the result in
once checked ([Choose disc image (ISO)](#choose-disc-image-iso)).

## The launcher

(XBA-41, around the game picker of XBA-54 and the import of XBA-40.) The app's launcher entry is
`LauncherActivity`, in its own process `:launcher`: it never loads `libSDL2.so` or the runtime, and after a new choice
it can end the game's process and start a fresh one (the runtime maps its memory once per process). It is made for a
gamepad first, in landscape, and works by touch too.

How it is built: framework `Activity` and `View`s only (no AndroidX, no Material, no new Gradle dependency), drawn in
code (`Theme.java` for colours, type and sizes; `Widgets.java` for the custom views: rows, cards, progress ring,
status chip, button hints). Everything is laid out on a 1920 x 1080 design grid that `Widgets.Stage` scales to the
window (wider or taller screens get a wider or taller grid, never a stretched one), so it looks the same from 1280 x 720
to 1920 x 1080. The window is fullscreen and immersive (system bars hidden, back with a swipe), drawn under the display
cutout, and the stage keeps its content out of the cutout's insets. System fonts only (`sans-serif`,
`sans-serif-medium`); the look is our own (deep green-black, one vivid green accent, warm off-white text), with no
console or game maker's logo or name in the art.

### Screens

One activity, a small state machine of screens (`LauncherActivity.Screen`) with a back history instead of fragments:

| Screen | What it shows | Reached from |
|---|---|---|
| Welcome | What the app is (a recompiled game, the game not included: bring your own copy), three steps, « Get started ». | First start, when no game is set up at all. |
| Choose your game (source) | Two large cards: **Game folder** (the extracted disc; read in place with All files access, otherwise copied; a badge says which) and **Disc image (ISO)** (extracted once; a badge gives the free space). | Welcome, Game data › Change game, Play with no usable game, the « Change game » shortcut, `NO_GAME_DATA`. |
| Read the game in place? | Only without All files access: the explanation (in place: no copy, fine for a 4 GB disc, only that folder is read; copy: as much free space again, a few minutes, removed with the app) and two cards, **Allow all files access** (recommended; opens the system page, then the folder picker) and **Copy into the app** (the folder picker). | Game folder. |
| Preparing the game (import) | A progress ring (percent, ticks), the step (« Checking », « Copying », « Extracting »), « X of Y », the current file, the free space, **Cancel**. The screen stays on; B cancels too. | A picked folder or image. |
| Home | A console-style home (the top bar only says « Home », so the title shows once). The hero: the game's [title image](#title-image) (or a monogram), its title ID, its title (the XBE certificate's name, else the folder's name), a status chip (**Ready**, **Needs preparing**, **Preparing**, **Prepare failed**, **Game data missing**, **No file access**, **Wrong game**, **No game**; plus **Running** when the game runs in the background), one lead line, a few facts and one big button. Facts: **Last played**, **Play time**, then **Prepared** (pack.json's `built_at`) and **Pack size** when a pack is there, else the game's size; while a Prepare is needed: how long it takes, the free internal storage, the memory; while it runs: a bar and « Compile · 42 % · about 12 min left »; after a failure: the code. The big button: **Play**, **Prepare**, **Preparing…** (opens its screen), **See what failed**, or **Choose a game** / **Choose the game again**. Under the hero, a row of four smaller tiles: **Settings**, **Controls**, **Game data**, **About**. | A finished import (Play focused), every start when « Open directly » is off, the « Launcher » shortcut, B from the screens above. |
| Settings | The [options](#settings) as rows, left / right (or A) to change, a detail panel with what the option does, its choices and its line in `recomp.cfg`. | Home. |
| Controls | A drawn handheld with, next to each control, the Xbox controller button the game gets, and the same as a table: A B X Y as they are, L1 = **Black**, R1 = **White**, L2 / R2 = the triggers, Start = Start, Select = **Back**, D-pad and sticks as they are, pressing a stick = its thumbstick button (`src/input/xinput_device.c`, SDL's game controller mapping), and Android's Back pressed twice quits the game (`shell_main.c`, [Quit](#quit)). | Home. |
| Game data | The raw folder path (only here, not on Home), source (read in place, imported copy, set in `recomp.cfg`), size (counted in the background), free space; **Change game** (another folder or image, or the same one again to re-import); **Delete the imported copy** (only when the game is the app's own `files/game/`; press A twice within 5 s; saves are kept; then the choice is forgotten). | Home. |
| About | Version (`-PxbaVersion`), build (`-PxbaVersionCode`), app id, the game check (the first digits of `-PxbaXbeSha256`, or none), licences (xboxrecomp MIT, SDL2 zlib), and that no game data is included. | Home. |

Messages and errors (`NOT_A_GAME_FOLDER`, `WRONG_GAME`, `NOT_ENOUGH_SPACE`, « Import canceled », « Saved », ...) go to
a note line above the button hints, coloured by kind; there are no toasts and no dialogs. logcat (tag `xboxrecomp`):
`launcher: shown (...)` naming why, then one `launcher:` line per step (folder, hash, copy, import, game set, play,
settings saved, errors).

### When it shows

| Case | What happens |
|---|---|
| A usable game and « Open the game directly next time » off (the default) | Home, Play focused. |
| A usable game and « Open directly » on | `ShellActivity` starts at once; nothing shows. |
| The game is running in the background and the app icon is pressed | Back to the game at once, whatever « Open directly » says. |
| Nothing remembered, but `RECOMP_GAME_DIR` in `recomp.cfg`, or `default.xbe` pushed to `files/game/` | A usable game, as above: the adb workflow still works (source « Set in recomp.cfg » or « Imported copy »). |
| Nothing set up | Welcome. |
| The remembered folder is gone (deleted, SD card out) | Home, chip « Game data missing », the note naming the old path (`GAME_FOLDER_GONE`), Game data focused; the previous choice stays until a new one is made. |
| The folder is there but unreadable because All files access was turned off | Home, chip « No file access », the note says so (`GAME_FOLDER_NO_ACCESS`). |
| `default.xbe` there but not the one this APK was built for (files changed since the choice, or set up through `recomp.cfg` / adb) | Home, chip « Wrong game »: Play says `WRONG_GAME` instead of starting. Checked once per folder in the background, only when the APK has an expected hash. |
| The native side found no `default.xbe` (`NO_GAME_DATA`) | `shell_main.c` calls `ShellActivity.openGamePicker(dir)`; the launcher opens on Choose your game with `NO_GAME_DATA` and that folder in the note; B goes to Home. |
| Long-press on the app icon, « Change game » | Choose your game, even with a game set up; B goes to Home. If the game is running, the note says it will close when another game is chosen; saves are in internal storage and stay. |
| Long-press on the app icon, « Launcher » | Home, even with « Open directly » on or the game running. |

The two shortcuts are dynamic (the package name differs per title, which a static `shortcuts.xml` cannot follow),
published each time the launcher runs, so they appear after the first start.

A new game, once checked, is remembered and the running game (if any) is ended then, as before; the launcher goes to
Home with Play focused and « TITLE is ready » in the note, instead of starting the game by itself.

### Controls

| Input | Does |
|---|---|
| D-pad, left stick | Move the focus (Android turns the stick and the hat into D-pad keys). Each screen remembers its focused item. |
| A (`BUTTON_A`), D-pad centre, Enter | Select the focused item; on a setting, the next value. |
| B (`BUTTON_B`), Back, Escape | Back one screen; on Home or Welcome, leave the app; during an import, Cancel. |
| Left / right on a setting | Previous / next value. |
| L1 / R1 | Settings: previous / next section. |
| Y | Settings: restore the defaults. |
| Start | Home: Play. |
| Touch | Tap an item to select it, the arrows of a setting to step it, a button hint to do what it says. After a touch, the first D-pad or A press brings the focus back. |

A and B are taken on release, once, so a held button does not repeat. Dimmed rows (Play without a usable game) stay
focusable so that pressing them says why. « Open the game directly next time » is in Settings only (the X toggle on
Home is gone).

### Look, motion and sound

- **Focus**: one highlight for the screen (`Widgets.Glide`: a green outline with a soft glow, a light gradient and an
  accent bar on the left) glides from the row it leaves to the row it reaches in 150 ms; the focused row's title turns
  brighter and the row grows to 102 %.
- **Screens** fade in and slide in from the side they come from (forward from the right, back from the left), 150 ms.
- **Settings**: a value pulses when it changes (160 ms). A setting row keeps the width of its widest choice for the
  value, and a title too long for what is left ends in « … » (measured with the row's own paint); the detail panel
  shows it in full.
- **Sounds and haptics** (`UiFeedback`): five short sounds, made with `soundcraft-cli` from our own synth recipes
  (`tools/android/ui_sounds.sh`, WAVs in `res/raw`, 16-bit 22.05 kHz, about 115 kB in all): **move** (focus or a
  value), **select**, **back**, **error** (an error note) and **done** (a game set up, a Prepare finished), played
  through SoundPool at a low volume, each with a haptic (`performHapticFeedback`: clock tick, keyboard tap, reject,
  confirm; Android's own « touch feedback » setting applies). Settings › Launcher › **Sounds** and **Vibration** turn
  them off (`game` SharedPreferences, `ui_sound`, `ui_haptics`).
- **Preparing screen**: one big overall percentage in the ring, the step and its own percentage smaller under it, the
  time left prominent in the panel. When the Prepare ends: Home with Play focused, the « done » sound and haptic. After
  two minutes without input it dims (OLED burn-in): a black layer with the percentage and the step in grey, moved a
  little each minute, and the window's brightness lowered; any key or touch wakes it and does nothing else.

### Title image

Home shows the game's own title image: the XBE's `$$XTIMAGE` section, an XPR0 resource file holding one texture
(usually 128 x 128 DXT1), decoded in Java on the device (`TitleArt`: DXT1 / DXT3 / DXT5, and swizzled or linear
A8R8G8B8 / X8R8G8B8 / R5G6B5) and cached as `<internal files>/art/<TITLEID>.png`. Formats from public documentation
(xboxdevwiki « Xbe » and « XPR ») and `src/d3d` (format codes, swizzle). The backdrop's glow takes the image's
colour. An XBE without one (or in a format not read) gets a monogram: the title's initials on a gradient whose hue
comes from the title ID. Nothing of a game is in the APK or the repo; the unit test builds its XBE and XPR0 itself.

### Play time

« Last played » and « Play time » are kept simple: a session starts when the launcher starts `ShellActivity` (not when
it only brings a running game back) and ends when the launcher shows again with the game no longer running; a session
the launcher never saw end counts for at most 6 hours (`game` SharedPreferences, `last_played`, `play_ms`,
`session_start`).

`LauncherScreensTest` renders every screen and Home state in English and French at 1280 x 720 and 1920 x 1080, each
with its focused row, to `build/screenshots/ui-<state>-<en|fr>-<720|1080>.png` (`--target app-unit-tests-out`);
`TitleArtTest` checks the XBE section, the XPR0 container and the texel decoders on blobs it builds.

### Settings

The Settings screen writes a few `RECOMP_*` options to `recomp.cfg` (the file the runtime already reads,
[RECOMP_* options](#recomp_-options-recompcfg)), and they apply the next time the game starts. Only options that
exist in the runtime and mean something to a player are offered:

| Row | Choices (default first or marked) | Line in `recomp.cfg` |
|---|---|---|
| Open the game directly next time | Off (default), On | none: a launcher preference (`game` SharedPreferences, `auto_start`) |
| Sounds | On (default), Off | none: `ui_sound` |
| Vibration | On (default), Off | none: `ui_haptics` |
| Bottom screen (only with a second display) | Panel (default), Black, Off | none: [Dual screen](#dual-screen-ayn-thor), `GameStore.bottomScreen` |
| Graphics › Vertical sync | On (default), Off | `RECOMP_PRESENT_VSYNC=0` when off |
| Graphics › Renderer | Auto (default: the title profile's choice, software today), Software, GPU | `RECOMP_GPU=0` / `1`; see [GPU back end](../technical/gpu-backend.md#android) |
| Graphics › Frame cap (GPU renderer) | Auto (default: the title profile's choice, 60 unless the profile sets one; Fable: 30), 30, 60, Off | `RECOMP_GPU_FPS_CAP=30` / `60` / `0` |
| Graphics › Texture filtering | Sharp (default), Smooth, Anisotropic 8×, Anisotropic 16× | `RECOMP_TEX_BILINEAR=1`, `RECOMP_TEX_ANISO=8` / `16` |
| Audio › 3D audio (HRTF) | On (default), Off | `RECOMP_APU_HRTF=0` when off |
| Audio › Audio buffer | Short (256), Normal (default, 512), Long (1024), Very long (2048) | `RECOMP_AUDIO_BUF_SAMPLES=…` |
| Advanced › Drawing threads | Auto (default), 1, 2, 4, 6, 8 | `RECOMP_RASTER_THREADS=…` |
| Advanced › Fast cores only | On (default on Android), Off | `RECOMP_BIG_CORES=0` when off |
| Advanced › Self-test at start | Off (default), On | `RECOMP_SHELL_SELFTEST=1` |

How the file is written (`RecompConfig.java`): it is parsed as `recomp_cfg.c` does (one `KEY=VALUE` per line, blanks
and `#` comments skipped, keys and values trimmed, the first line for a key wins). A change rewrites the key's first
line in place and removes its later duplicates; a default value removes the key's lines (the runtime default then
applies); a new key is appended under the comment « Written by the launcher's Settings screen ». Every other line
(comments, other variables, `RECOMP_GAME_DIR`) is kept as written. The file goes through `recomp.cfg.tmp` and a
rename; no file is created while every option is at its default. A value the screen does not offer (written by hand)
shows as « Custom » and stays until that row is changed. « Restore defaults » (or Y) removes the lines of these
options only.

Asked for in XBA-41 but left out, because no runtime option backs them on Android: the output scaling and its filter
(the SDL presenter always letterboxes the 640 x 480 frame with nearest-neighbour sampling, `fb_present_sdl.c`;
`RECOMP_WINDOW_SCALE` only sizes a desktop window and `RECOMP_PRESENT_FILTER` is the D3D11 presenter's), an fps cap
for the software renderer (only the GPU renderer has one, `RECOMP_GPU_FPS_CAP`), control remapping and a stats overlay (no such switch), and vibration (`RECOMP_RUMBLE` only gates the emulated USB
pad, `RECOMP_USB`, a per-title opt-in). Bring-up and debug switches stay out on purpose.

## Choosing the game

(XBA-54, and the import half of XBA-40.) The runtime reads the game with POSIX calls (`open`, `opendir`, the kernel's
case-insensitive lookup), so it needs a real path, not a `content://` URI. The launcher's source screens therefore
end with a folder path the runtime can open: the folder itself, or the app's `files/game/` after a copy or an
extraction.

### Choose game folder

1. Without All files access (`MANAGE_EXTERNAL_STORAGE`), the « Read the game in place? » screen explains the choice:
   with it the game is read in place from the folder (phone or SD card, no copy, fine for a 4 GB disc, only that folder
   is read); without it the folder is copied into the app's storage (as much free space again, a few minutes, removed
   with the app). « Allow all files access » opens the system's settings page for the app; on return the folder
   picker opens either way. « Copy into the app » opens it directly. With the permission already granted, the folder
   picker opens straight from the Game folder card.
2. The system folder picker (`ACTION_OPEN_DOCUMENT_TREE`).
3. **With the permission**, the tree URI is mapped to a path: the external storage provider's `primary:Games/Halo`
   is `/storage/emulated/0/Games/Halo`, `1A2B-3C4D:Halo` is `/storage/1A2B-3C4D/Halo` (SD card), `home:X` is
   `Documents/X`; the downloads provider's `raw:/path` is a path already. No `default.xbe` there: `NOT_A_GAME_FOLDER`
   in the note, and the card can be chosen again. Otherwise the [hash check](#wrong_game) (the import screen shows
   « Checking the game »), then the folder is remembered as is.
4. **Without the permission** (or a provider that is not a storage volume, or a path that cannot be listed): the tree
   is listed through `DocumentsContract`, `default.xbe` checked (`NOT_A_GAME_FOLDER`) and hashed from the provider
   before anything is copied, the free space checked against the total, then everything is copied to
   `files/game.import/` with progress and Cancel, checked again and swapped in place of `files/game/` (the old
   `game/` becomes `game.old/` and is removed once the new one is in). `files/game` is remembered.

An error or a Cancel ends on Choose your game with the reason in the note; the previous game is untouched.

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
   updates the ring and returns Cancel. A cancel or an error leaves `files/game/` untouched.
5. The extracted `default.xbe` is hashed ([WRONG_GAME](#wrong_game)), then `game.import/` is swapped in place of
   `game/` as above and remembered.

Why a separate JNI library rather than `libmain.so`: `libmain.so` needs `libSDL2.so` loaded first and brings the
whole runtime (`--whole-archive` toolkit, the title) into a process that would never run it. `libxba_import.so` is
`xiso_jni.c` plus the static `xiso` target (libc only) and `liblog`.

### WRONG_GAME

A title's APK is made for one disc. `-PxbaXbeSha256=<sha256sum of default.xbe>` bakes the expected hash in (string
resource `xba_xbe_sha256`, 64 hex digits, checked by Gradle); empty, as for the shell and the stub, means no check.
The launcher computes the SHA-256 of `default.xbe` of the chosen folder (in place), of the provider's file (before a
copy) or of the extracted tree (after an extraction, before the swap). A mismatch shows `WRONG_GAME` and changes
nothing: the previous choice is kept, `files/game/` is untouched, the staging folder is removed. Home checks the
current folder once more in the background (chip « Wrong game »). logcat names the hash:
`launcher: ... sha256 <hex>`.

### How the path reaches the runtime

The choice is in the app's SharedPreferences (`game`: `game_dir`, `title`), written with `commit()` before the
game's process starts. `ShellActivity.onCreate` calls `Os.setenv("RECOMP_GAME_DIR", dir)` before
`super.onCreate()`: `SDL_main` runs later on SDLThread, and `recomp_app_start` → `recomp_storage_resolve` reads the
variable (the process environment is libc's, so `getenv` sees it). An environment variable wins over `recomp.cfg`,
so a remembered choice wins over a `RECOMP_GAME_DIR` line there. Saves stay in internal storage whatever the game
folder.

### Device check (XBA-41, with XBA-54 and XBA-40), for Alex

Use the stub APK (**shell-latest**) and its `stub-default.xbe` as the « game »: a folder holding only it is enough
for the stub title. `adb logcat -c && adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG` in a terminal throughout.
Use the Thor's gamepad for everything unless a step says touch.

1. Install the APK with nothing configured (`adb uninstall` first). Expected: the Welcome screen, landscape,
   fullscreen (no status or navigation bar), nothing hidden under the camera cutout, « Get started » focused with a
   green outline; logcat `launcher: shown (no game set up)`.
2. Look at the layout: texts not cut, nothing overlapping, the button hints at the bottom right (A Start, B Quit).
   Send a screenshot (`adb exec-out screencap -p > welcome.png`).
3. A on « Get started »: Choose your game, the Game folder card focused. D-pad down / up moves between the two cards;
   B goes back to Welcome; A again.
4. Copy `stub-default.xbe` as `default.xbe` into a folder of internal storage (e.g. `Download/stubgame/`). A on Game
   folder: « Read the game in place? ». A on « Allow all files access », turn it on, come back: the folder picker
   opens. Pick the folder. Expected: Home, title of the stub (or the folder's name), chip « Ready », Play focused, the
   note « … is ready »; logcat `launcher: game set: /storage/emulated/0/Download/stubgame`.
5. A on Play: the stub title starts (`storage: game=/storage/emulated/0/Download/stubgame ... (default.xbe found)`,
   `stub title: all 5 lifted functions OK`), and « The title finished (code 0) » on the black screen.
6. Press Home on the Thor, then the app icon: back to the game at once (logcat `launcher: game running, bringing it
   back`). Back twice to quit it, start the app again: Home with Play focused (« Open directly » is off).
7. Settings: D-pad through the rows (the list scrolls), left / right change Vertical sync and Texture filtering, L1 /
   R1 jump between sections, the right panel follows the focus. Then
   `adb shell cat /sdcard/Android/data/fr.adserver.xboxrecomp.shell/files/recomp.cfg`: the comment line and
   `RECOMP_PRESENT_VSYNC=0` / `RECOMP_TEX_BILINEAR=1` (or what you chose). Play: logcat `recomp.cfg: N option(s)
   from …` lists them and `[FBWIN] … vsync off`. Back in Settings, Y: the lines are gone.
8. Hand-written lines survive: `adb shell "echo RECOMP_WAIT_LOG=1 >> /sdcard/Android/data/fr.adserver.xboxrecomp.shell/files/recomp.cfg"`,
   change a setting, `cat` again: `RECOMP_WAIT_LOG=1` is still there.
9. Settings › « Open the game directly next time » on; quit and start the app: the game starts directly.
   Long-press the icon › « Launcher »: Home. Turn it off again in Settings.
10. Long-press the app icon › « Change game »: Choose your game, the note says the running game will close (if it
    runs). Pick an empty folder: `NOT_A_GAME_FOLDER` in the note, nothing changed; B goes to Home.
11. Folder gone: rename the folder of step 4 with a file manager, start the app. Expected: Home, chip « Game data
    missing », the note naming the old path (`GAME_FOLDER_GONE`), no crash; A on Play opens Choose your game.
12. Copy fallback: Settings › Apps › the app › All files access off. Game folder › « Copy into the app », pick the
    folder of step 4 (renamed back). Expected: the progress ring, then Home with Play focused, Game data showing
    « Imported copy » and the folder `/storage/emulated/0/Android/data/<app id>/files/game`.
13. ISO: make an XISO of our own test data (e.g. `extract-xiso -c stubgame stub.iso`, or any image dumped from your own
    disc), push it to `Download/`. Disc image (ISO), pick it. Expected: the ring with the percentage, « X of Y », file
    names, the screen stays on, then Home. Do it once more and press B (or Cancel) halfway: « Import canceled » in the
    note on Choose your game, and the previous game still plays. Re-import your own disc's image too (it worked
    before this change).
14. Pick a file that is not an ISO (e.g. a photo): `NOT_AN_ISO` in the note.
15. Game data › Delete the imported copy: the first A shows « Press A again », the second deletes; then Welcome (or
    Home without a game) and « The imported copy was deleted ». Saves: `adb shell run-as fr.adserver.xboxrecomp.shell ls files/save`
    still lists them.
16. `NO_GAME_DATA` (after step 15, so that no game is remembered): `adb shell mkdir /sdcard/Download/empty`, then
    `adb shell "echo RECOMP_GAME_DIR=/sdcard/Download/empty >> /sdcard/Android/data/fr.adserver.xboxrecomp.shell/files/recomp.cfg"`.
    Start the app: Home, source « Set in recomp.cfg ». Play: the native side finds no `default.xbe` and hands over;
    the launcher opens on Choose your game with `NO_GAME_DATA` and the folder in the note. Remove the line afterwards.
17. WRONG_GAME needs an APK built with `-PxbaXbeSha256=` of another XBE (ask for one, or build one): picking the stub
    folder then shows `WRONG_GAME`, and the previous choice still plays.
18. Touch: tap the cards, the menu rows, a setting's arrows, the hints (B Back). Everything works without the gamepad,
    and the next D-pad press shows the focus again.
19. Switch the Thor to French: every launcher text is in French (screens, settings, notes, shortcuts).
20. Send screenshots of Welcome, Choose your game, Read the game in place?, the import ring, Home, Settings, Game
    data and About, and the logcat lines (`launcher:`, `import:`, `recomp.cfg:`, `storage: game=...`). Say if the A /
    B buttons are swapped on the Thor (its « Xbox » / « Nintendo » layout setting changes the key codes the app sees).

## Prepare on the device (the app side)

The on-device Prepare turns the player's game into a title pack (`<files dir>/packs/<TITLEID>/{libtitle.so,
pack.json}`); the contract (command, progress lines, error codes, pack.json) is
[prepare.md](prepare.md). This section is the app's half: `PrepareService`, `PrepareRunner`, `PrepareProtocol`,
`Pack`, `Toolchain`, `LogProvider` and the launcher's Prepare screens.

**Hidden without the toolchain.** The feature shows only when the APK carries `libxbapython.so` and `libxbacc.so` in
`nativeLibraryDir` and the three tool zips in its assets. Without them (the shell as built today) the launcher behaves
exactly as before. Native libraries are extracted at install (`jniLibs.useLegacyPackaging = true`), so they can be
exec'd and `libmain.so` can be handed to the linker.

**Tool assets** (`Toolchain`): `assets/prepare/python-stdlib.zip`, `sysroot.zip`, `toolkit.zip` (any zip in
`prepare/` or the root whose name contains `stdlib`, `sysroot`, `toolkit` also works), each optionally with a sidecar
`<zip>.sha256` (checked before extracting; a mismatch fails with `ASSET_HASH`). Each is extracted once into
`<files dir>/python-<v>`, `sysroot-<v>`, `toolkit-<v>`, where `<v>` is the first 12 hex digits of the zip's SHA-256,
through a `.tmp` directory renamed when complete (`.xba-complete` holds the hash); other versions are deleted. The
hashes are cached per APK install (`lastUpdateTime`), so a second start opens nothing. This is the service's
« Install tools » phase (status `id=install`, `step=0`), on its worker thread.

**The service** (`PrepareService`, process `:prepare`, foreground type `specialUse`: `dataSync` is capped at 6 h a day
on Android 15). The launcher starts it with the game folder and the title ID (from `default.xbe`'s certificate,
`cert + 0x08`). It takes a partial wake lock (6 h at most), installs the tools, then runs, in its own process group
(`setsid`):

```
<nativeLibraryDir>/libxbapython.so -m tools.prepare <game dir> --android --work <files>/work/<TID>
    --pack-dir <files>/packs --cc <nativeLibraryDir>/libxbacc.so --cc-prefix-arg clang --sysroot <files>/sysroot-<v>
    --shell-lib <nativeLibraryDir>/libmain.so --jobs N --opt 2 --shell-abi 1
cwd <files>/toolkit-<v>; PYTHONHOME=<files>/python-<v> PYTHONPATH=<files>/toolkit-<v>
PYTHONPYCACHEPREFIX=<cache>/pycache TMPDIR=<cache>/tmp HOME=<files> LIBCAPSTONE_PATH=LD_LIBRARY_PATH=<nativeLibraryDir>
PYTHONUNBUFFERED=1 XBA_PYTHON=<nativeLibraryDir>/libxbapython.so
```

`N` = (MemAvailable − 3 GB) / 1 GB, between 1 and min(5, cores). One Prepare at a time (a second start is ignored).
The notification shows the step, a progress bar, the time left and **Cancel**. Cancel (notification or launcher)
sends SIGTERM to the group, then SIGKILL 5 s later. A child killed by a signal it did not ask for (Android's
phantom-process killer, low memory) runs once more with half the jobs; a second kill fails with `KILLED`. After an
« ok », the pack is checked (`Pack.check`); a pack that does not pass fails with `PACK_INVALID`. Starting again after
any end runs the same command: tools.prepare's stamps skip what was done.

**Files**, per title in `<files dir>/prepare/<TID>/`:

- `status.json`, written atomically (temp file, rename) at most every 250 ms and at each step change:
  `state` (`running`, `done`, `failed`, `cancelled`), `step`, `n`, `id` (`install`, `identify`, `analyse`, `lift`,
  `compile`, `pack`), `pct` (per step), `eta` (s, −1 = unknown), `msg`, `code`, `started_at`, `updated_at` (ms since
  the epoch), `log`, plus `jobs`, `attempt`, `title_id`.
- `prepare.log`: the command's whole stdout and stderr, one header per attempt, the outcome, and at the end the tail
  (2 MB) of tools.prepare's own `<work>/prepare.log`. « Share log » sends it through `LogProvider`
  (`content://<app id>.logs/prepare/<TID>/prepare.log`, read-only grant).

Codes the app sets itself: `KILLED`, `CANCELLED`, `SPAWN_FAILED`, `NO_RESULT` (exit without a result line),
`PACK_INVALID`, `INSTALL_FAILED`, `NO_TOOLS`, `ASSET_HASH`, `START_FAILED`; the others come from tools.prepare.

**A pack is valid** when `pack.json` parses, `format` is 1, `title_id` is the game's, `shell_abi` is the app's
`Pack.SHELL_ABI` (1), and `lib` exists with `lib_size` bytes. The native loader checks the same and `abi`, then
dlopens it (« Title packs » below); neither checks `lib_sha256` at each start. `xbe_sha256` is not
compared yet (hashing `default.xbe` at each Home is slow on a picked folder): another revision of a disc with the
same title ID keeps the old pack until it is prepared again.

**The launcher.** With a game ready and the toolchain present:

| Case | Home | Preparing screen |
|---|---|---|
| No valid pack, nothing run | **Prepare** instead of Play, « about 20–30 min » (30–45 below 8 cores, 40–60 under 6 GB of RAM), chip « Needs preparing »; the detail panel shows the free internal space (red under 2 GB: Prepare refuses) and the memory (amber under 6 GB: a warning only). | |
| Running | « Preparing… » with the step, percent and time left, chip « Preparing » | The step list (Install tools, Identify, Analyse, Translate, Compile, Pack) with the current one lit, the ring (overall percent), the message and the time left. B twice (or Cancel twice) cancels; Y goes back to the menu and the Prepare carries on. Polls `status.json` twice a second; the screen stays on. |
| Failed | « Prepare » with a « Prepare failed » badge, chip « Prepare failed » | The code and message (a hint for `KILLED` and `NO_SPACE`), the log's path, **Retry**, **Share log** (also Y). |
| Cancelled, or interrupted (a `running` status older than 15 s with no `:prepare` process) | « Prepare » carries on where it stopped | **Resume**, **Share log**. |
| Pack valid | **Play**, as before | Done: back to Home, « … is ready ». |

The app opens on the Preparing screen while a Prepare runs or after one failed. « Open the game directly » does not
skip a Prepare that is needed. On Android 13+ the first Prepare asks for the notification permission (refused: it
runs anyway, without the notification's progress).

**Tests** (JVM, Robolectric 4.14.1, `android/app/src/test`): the line parser, `status.json`, every pack check,
the asset install, the runner against scripted fake commands (progress → done, an error, a hang → cancel with
SIGKILL, a kill → retry with half the jobs), the service, and the launcher's states rendered to
`android/app/build/screenshots/prepare-*.png`:

```
cd android && gradle testDebugUnitTest -x externalNativeBuildDebug     # in xba-android:local, the repo mounted
docker build -f tools/docker/android.Dockerfile --target app-unit-tests .                 # the same, as CI runs it
```

### Device check (Prepare), for Alex

Needs an APK with the toolchain (the embedded-prepare branch merged) and the pack loader; until then steps 1 and 13.

1. The shell APK as built today (no toolchain): the launcher is unchanged: Home shows **Play**, no « Needs
   preparing » chip, no Prepare notification channel in the app's settings.
2. With the toolchain APK and your game chosen: Home shows **Prepare**, « about 20–30 min », chip « Needs
   preparing »; the right panel shows the free internal space and the memory (Thor: about 11 GB). Screenshot.
3. A on Prepare. On Android 13+ the notification permission is asked once: allow it. Expected: the Preparing screen,
   « Install tools » lit with a percentage for a few seconds the first time; then Identify, Analyse, Translate,
   Compile, Pack in turn; the ring's percent and the time left move. logcat `prepare: started for <TID> ... N jobs`
   (N = 4 or 5 on the Thor).
4. Pull down the notification shade: « Preparing <title> », the step, the bar, the time left, **Cancel**. Screenshot.
5. Press Y: Home shows « Preparing… » with the step and percent updating. Press Home on the Thor, use another app
   for a minute, turn the screen off for a few minutes: the notification keeps moving; back in the app, the Preparing
   screen shows the current step.
6. B once: « Press again » on Cancel; B again: within a few seconds « Preparation stopped », **Resume** and **Share
   log**. `adb shell ps -A | grep -E 'xbacc|xbapython'`: nothing left.
7. Resume: it starts again and skips the steps already done (Identify / Analyse / Translate are quick, Compile
   compiles only what was missing). Let it finish: Home with **Play** and « … is ready »; the notification says
   « Ready ». Write down the whole time and the compile time (« Measurements » in prepare.md asks for them).
8. Play: the game starts from the pack (`PACK_LOADED`, « Title packs » below). Quit, start the app again: Home
   with Play, no Prepare.
9. Second start of a Prepare (e.g. after `adb shell run-as <app id> rm -r files/packs`): no « Install tools » phase
   this time (the tools are installed once per APK version); `files/` holds one `python-*`, `sysroot-*`,
   `toolkit-*` each.
10. Failure: `adb shell run-as <app id> rm -r files/packs`, start a Prepare and, during Compile,
    `adb shell run-as <app id> sh -c 'kill -9 $(pidof libxbapython.so)'`. Expected: it restarts by itself with half
    the jobs (logcat `prepare: killed by signal 9, retrying with N jobs`). Kill it again: « Preparation failed »,
    code `KILLED` with the hint; Share log opens the share sheet with `prepare-<TID>.log`; send it to yourself and
    check it holds the command, the progress lines and the end of tools.prepare's log.
11. Interrupted: during a Prepare, `adb shell am kill <app id>` is not enough (foreground); use
    `adb shell run-as <app id> kill -9 $(adb shell pidof <app id>:prepare)`. Open the app after 20 s: Home says
    « Interrupted », Prepare resumes.
12. Low space: fill internal storage under 2 GB free, A on Prepare: the note says how much is needed and free, and
    nothing starts.
13. Switch the Thor to French: Prepare, Preparing and Failed screens, the notification and the note in French.

## Dual screen (AYN Thor)

The AYN Thor has two screens: the top one (6", 1920 x 1080, 120 Hz) is the main display, the bottom one (3.92",
1240 x 1080, touch) is a second `Display`. The launcher and the game stay on the display they start on (the top one);
a **companion panel** goes on the other one. On a single-screen device nothing of this runs.

### How it is put together

- `DualScreen.java`: the decisions, pure functions over plain display records (JUnit-tested over fake lists).
  `choose()`: a display of `DisplayManager.getDisplays(DISPLAY_CATEGORY_PRESENTATION)` other than ours, not private,
  not off; failing that, any other public display that is on. `decide()`: show, move or dismiss from the setting, the
  chosen display and the one shown on. Every display is logged once per change (`<tag>: displays:` then one line
  each: id, name, flags, size, refresh, state).
- `CompanionScreen.java`: one `android.app.Presentation` per process, shown from the activity's `onStart`, dismissed
  in `onStop`, with a `DisplayListener` for displays that come, go or change state. Pure black window. ShellActivity
  owns one (tag `shell`, the in-game panel); `LauncherCompanion` owns the launcher's (tag `launcher`).
- The panels are plain Views drawn on a 1240 x 1080 design grid scaled to fit (`PanelView`): `CompanionPanel` in the
  game, `LauncherPanel` and `PreparingPanel` in the launcher. No animation: a 1 Hz tick (stats, battery, thermal,
  dimming) that redraws only when something changed, and redraws while a hold button is held (0.8 s at most), so
  the second window costs the game next to nothing.
- Burn-in (AMOLED): pure black background; after 10 s without a touch the panel dims to faint outlines (the
  Preparing panel: after 30 s, to the percentage alone); the whole layout moves by a few pixels each minute. The touch
  that wakes a dimmed panel presses pad buttons but fires no action.

### The setting « Bottom screen »

`GameStore.bottomScreen(ctx)` / `GameStore.setBottomScreen(ctx, GameStore.BottomScreen.PANEL | BLACK | OFF)`:
the panel (default), a black screen (an empty black Presentation, hides what the system shows there), or nothing.
Stored in `<files>/settings/bottom_screen` rather than in the game SharedPreferences: the launcher writes it while
the game's process may be alive, and SharedPreferences caches per process; each process reads the file at its next
`onStart`. The Settings row is the launcher's (another change): show it only when
`DualScreen.available(activity, displayId)` is true, and call `setBottomScreen` on change.

### In the game

```
  fps · frame time              battery                  thermal
  [ Back  ]   [Launcher] [Screenshot] [ Perf ]   [ White ]
  [ Start ]   [Share log] [   Quit (hold)    ]   [ Black ]
               (Perf details)       nothing to touch bottom-centre (palms)
```

- **Back / Start / White / Black**, in the edge columns next to the thumbs: duplicates of Select, Start, R1 and L1
  (`xinput_device.c` maps L1 to Black and R1 to White), there so the player finds them. Each pointer is tracked on its
  own (two thumbs on two buttons work, sliding from one to the next works); everything is released on
  `ACTION_CANCEL`, when the panel is dismissed or detached, and in `ShellActivity.onStop`.
- **Launcher** opens the launcher (the game waits in the background, as with Home). **Quit** needs a hold of 0.8 s
  (it fills in red), then ends the game as Back twice does (`recomp_quit(0)`). **Screenshot** saves the last frame as
  `<external files>/screenshots/<date>.bmp` (24-bit BMP, 640 x 480: `xbox_FramebufferDumpBmp`, now implemented for
  the SDL window) and says « Saved … » in the status strip. **Perf** shows more figures on the panel (draws per frame,
  battery temperature); it does not draw over the game. **Share log** dumps this process's logcat (the runtime's
  stdout and stderr go there) to `<files>/logs/game.log` and opens the share sheet (`LogProvider`,
  `content://<app id>.logs/game/game.log`).
- Status strip, once a second: the fps the window computes for its title bar (`xbox_FramebufferWindowGetStats`) and
  the matching frame time, the battery (`BatteryManager`), the thermal status (`PowerManager.getCurrentThermalStatus`:
  Cool, Warm, Hot…, coloured from moderate on).

The buttons reach the pad through the touch overlay (`src/input/input_overlay.c`):
`xbox_InputOverlaySet(port, pad)` stores a whole pad under a mutex; the SDL backend's `xbox_InputGetState` merges it
into port 0 (`xbox_InputMap_PadMerge`, after the keyboard), and with no gamepad attached it stands in for one
(success on port 0 while the panel is up). Each change bumps the packet number. JNI:
`android/app/src/main/cpp/companion_jni.c` (`CompanionNative`: `nativeOverlay`, `nativeOverlayOff`, `nativeStats`,
`nativeScreenshot`, `nativeQuit`). Test: `tests/input_overlay` (Linux, fake SDL: merge over a pad, release, no pad,
keyboard stand-in, a concurrent writer).

### Focus: Plan A and Plan B

A second window must not take the gamepad away from SDLActivity: on Android, keys go to the focused window, and
touching a focusable window on another display can move the focus there.

- **Plan A** (default): the Presentation is `FLAG_NOT_FOCUSABLE` (set before `show()`). Touch still works; the focus
  never leaves the game.
- **Plan B**: the panel's window is focusable and hands every key and motion event it receives to ShellActivity
  (`dispatchKeyEvent` / `dispatchGenericMotionEvent`, which SDL's surface handles as usual); ShellActivity ignores
  losing the focus while the panel is up (logcat `shell: focus went to the companion panel, ignored`). Switch it on
  without a rebuild with `RECOMP_COMPANION_FOCUS=forward` in `recomp.cfg` (wins), or `GameStore.setCompanionFocusPref`.
  The launcher's panel always uses Plan A.

logcat at each show: `shell: companion panel on display 2 "…" … (not focusable)` or `(focusable, keys forwarded)`.

### In the launcher (wiring)

`LauncherCompanion` does it in a few calls, wired in `LauncherActivity` like this (Play goes through the launcher's own
`onPlay`, so a Prepare still comes first; the Home call passes the hero's chip):

```java
private LauncherCompanion companion;

// onCreate, after buildStage() (not on the early launchShell() return):
companion = new LauncherCompanion(this, new LauncherPanel.Actions() {
    @Override public void play() { onPlay(); }
    @Override public void settings() { go(Screen.SETTINGS); }
    @Override public void changeGame() { go(Screen.SOURCE); }
});

@Override protected void onStart() { super.onStart(); if (companion != null) companion.start(); }
@Override protected void onStop() { if (companion != null) companion.stop(); super.onStop(); }

// where Home is built (the chip's text and colour, Play allowed or not):
companion.home(title, status.label(), status.color(), st == State.READY && !preparing);
// where the Preparing screen is built:
companion.preparing(titleId(), gameTitle(dir));
```

Home shows an identity card (title, status) and three big shortcuts: Play, Settings, Change game. Preparing shows the
step list, the overall percentage and time left, the last four lines of `prepare.log`, battery and thermal, and
**Cancel** on a hold (it sends `PrepareService`'s cancel itself); it reads `status.json` and the log once a second.

### Tests and screenshots

`DualScreenTest` (chooser over one, two, private and off displays; decisions; focus mode), `CompanionPanelTest`
(multi-touch, slide, cancel, detach, taps, hold-to-quit, dimming, shift; through a fake sink), `CompanionScreenTest`
(Robolectric `ShadowDisplayManager.addDisplay` / `removeDisplay`: shown, moved, dismissed, Off / Black, `onStop`,
Plan A not focusable and Plan B focusable and forwarding, the launcher's panel switch), `LauncherPanelsTest`. The
panels are rendered at 1240 x 1080 to `build/screenshots/companion-*.png` (`--target app-unit-tests-out`).

### Device check (dual screen), for Alex

On the Thor, with the shell APK of this change (and the stub or a title for steps 5 to 9):

1. Start the app: the launcher on the top screen. Until the launcher wiring lands the bottom screen shows the system's
   own content; after it, the launcher panel (title card, Play / Settings / Change game). logcat
   `launcher: displays:` lists both screens: send those lines (id, name, flags, size, refresh).
2. Start the game: the bottom screen turns to the in-game panel (black first, no white flash). logcat
   `shell: companion panel on display N … (not focusable)`. Photo of both screens.
3. **Focus, Plan A**: while playing with the built-in pad, touch the bottom screen anywhere (an empty spot, then
   Perf): the pad keeps driving the game, no pause, no `onWindowFocusChanged(): false` in logcat.
4. **Focus, Plan B**: `RECOMP_COMPANION_FOCUS=forward` in recomp.cfg, restart the game. Repeat 3: the pad still drives
   the game; logcat may show `focus went to the companion panel, ignored`. Say which plan behaves better (keys lost,
   doubled, a pause, the sticks).
5. **Buttons**: hold White on the panel (the game sees R1's action), then Start; both thumbs at once (White + Back).
   Slide a thumb from Back to Start. Same results as the physical L1 / R1 / Select / Start.
6. **Stuck keys**: hold White, and while holding press Home (or pull the notification shade, or let the screen turn
   off). Come back: White is not held. Same with two fingers on the panel and a palm landing on the screen.
7. **fps**: logcat prints `shell: fps N, M draws, companion shown|off` every 10 s. In the same scene, a minute with
   the panel (Perf on, so it redraws every second), then set Bottom screen to Off
   (`adb shell run-as <app id> sh -c 'mkdir -p files/settings; echo OFF > files/settings/bottom_screen'`, Home and
   back to the game) and a minute again. Expected: no measurable difference. Send both sets of lines.
8. **Dimming**: leave the panel 10 s: it dims to faint outlines; touch it: it wakes, and that first touch does not
   fire Launcher or Screenshot. Watch for a minute: the layout moves by a few pixels.
9. **Actions**: Screenshot (the strip says « Saved … »; `adb shell ls /sdcard/Android/data/<app id>/files/screenshots`,
   pull the BMP), Share log (share sheet with game.log), Perf, Launcher (the launcher comes up on top, the game
   waits), Quit: a short tap does nothing, a 1 s hold quits (`clean exit`).
10. **Off and Black**: `echo BLACK` / `echo OFF` into `files/settings/bottom_screen` as in 7 (`echo PANEL` to come
    back): Black keeps the bottom screen black, Off leaves the system's content there, nothing else changes.
11. **Single screen**: on a one-screen device (a phone): no panel, no error, logcat `displays:` lists one display.

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
4. No title linked (the weak `recomp_title_main` is NULL) and `default.xbe` found: the title pack for its title ID
   (« Title packs » below), run the same way.
5. No title and no pack, or no game data: the shell's own black window, as before.

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

### Title packs (the loader)

The one app runs any title from the pack the on-device Prepare built for it
(`<internal files dir>/packs/<TITLEID>/{libtitle.so, pack.json}`, [prepare.md](prepare.md)). Code:
`android/app/src/main/cpp/pack_loader.{c,h}` (forwarders, `xba_shell_abi`, `xba_pack_load`) and
`src/platform/recomp_pack.{c,h}` (title ID, `pack.json`, checks, dlopen; no Android in it).

**When.** In `SDL_main`, after `SDL_Init` and the self-test, when no title is linked into `libmain.so`
(`recomp_title_main` is NULL) and `default.xbe` was found: the title ID from its certificate (base at `0x104`,
certificate address at `0x118`, title ID at certificate + `0x08`), then `<SDL_AndroidGetInternalStoragePath()>/packs/
<TITLEID>/pack.json`.

**Checks** (the same as `tools/prepare/pack.py` `check` and the launcher's `Pack.check`, plus `abi`): every required
field, `format` 1, `title_id` = the XBE's, `abi` = `arm64-v8a`, `shell_abi` = the exported `xba_shell_abi`, `lib` a
plain file name, present, of `lib_size` bytes. **Not `lib_sha256`**: the pack sits in the app's private storage,
published atomically by prepare which hashed it, and hashing a 100–250 MB library costs about a second at every
start on a phone. `lib_size` catches a truncated file; an import from outside (below) is where the hash is checked.

**dlopen** with `RTLD_NOW | RTLD_LOCAL`. The pack has `DT_NEEDED libmain.so` (already loaded by SDLActivity: bionic
matches it by soname) and binds its calls to itself (`-Bsymbolic`), so nothing later needs its symbols in the global
scope (`RTLD_GLOBAL` is not needed); `RTLD_NOW` fails a missing symbol at start, not mid-game. The app namespace lets
`libmain.so` dlopen from the app's data directory. Then `dlsym` of `entry` (`recomp_title_main`, required),
`recomp_lookup`, `recomp_lookup_manual`, `fe_menu_get_pb_state`, `g_fe_cursor` (optional). `dlsym` on a handle also
searches the pack's dependencies, so a name the pack does not define comes back as `libmain.so`'s own forwarder:
each result is kept only if `dladdr` puts it inside the pack (else the forwarder would call itself).

**Routing: weak forwarders, strong titles.** The toolkit inside `libmain.so` calls `recomp_lookup` and
`recomp_lookup_manual` (thread start, APCs, DPCs, the USB and APU callbacks) and the pushbuffer replay calls
`fe_menu_get_pb_state` and reads `g_fe_cursor`. Those references were bound when `libmain.so` was loaded; a library
dlopened later cannot take them over. So `pack_loader.c` defines the four as **weak forwarders**:

```c
__attribute__((weak)) recomp_func_t recomp_lookup(uint32_t va) { return s_lookup ? s_lookup(va) : 0; }
```

and `xba_pack_load` sets `s_lookup` & co. from the pack before the title starts (NULL = the old stand-in result,
0). `g_fe_cursor` is data: `libmain.so`'s copy is refreshed from the pack's in each `fe_menu_get_pb_state` call
(the replay asks for the menu state, then reads the cursor, in the same frame). A title linked in statically
(`-PxbaTitleDir`, the stub APK) defines these **strong**: the static link takes them over the weak forwarders, and the
loader never runs. The stub APK's CI check (`llvm-nm`, `T recomp_title_main`) still holds.

**`xba_shell_abi`**: `const int` = 1, exported by `libmain.so` (`llvm-nm -D libmain.so | grep xba_shell_abi`). It
equals `Pack.SHELL_ABI` and the `--shell-abi` the app passes to prepare; bump all of them (and `tools/prepare/pack.py`
`SHELL_ABI`) together when what a pack links against in `libmain.so` changes incompatibly. A pack built against
another value is refused (`PACK_INVALID shell_abi …`) and the launcher offers Prepare again.

**logcat** (stdout, tag `xboxrecomp`):

| Line | Then |
|---|---|
| `PACK_LOADED <TID> <path> (<bytes> bytes, "<title>", shell_abi 1; exports: …)` | `xboxrecomp ready (… title)`, the title runs as a linked one would. |
| `PACK_MISSING <TID> (no …/packs/<TID>/pack.json)` | As before: the black window and the NO_TITLE message. (The launcher normally shows Prepare before Play gets here.) |
| `PACK_INVALID <reason>` | `ShellActivity.openLauncherBadPack(reason)`: the launcher opens with `EXTRA_BAD_PACK`, deletes `packs/<TID>`, and Home shows **Prepare** with « The prepared game could not be loaded (…). Prepare it again. » Its own lighter check passed the pack, hence this round trip. A message box if the JNI call fails. |
| `PACK_DLOPEN <dlerror>` | The same. |

NO_GAME_DATA is unchanged (no `default.xbe`: the game picker).

**Import drop: later.** prepare.md describes `<external files dir>/packs/<TID>/` as an import drop (copied in after
checking `lib_sha256`, never loaded in place: external storage is `noexec`). Not done yet: the copy of a 100+ MB
library belongs on a worker thread with progress in the launcher, not in `SDL_main`; for now a pack is made on the
device (or pushed with `adb shell run-as`, below).

**Tests.** `tests/recomp_pack` (Linux CI, and the device-tests bundle): the title ID, `pack.json` parsing (escapes,
unknown and nested fields, wrong types, truncation), every check, and the routing with three small libraries built
like the app's: a fake shell (`pack_loader.c` + `recomp_pack.c` + a stand-in for the toolkit calling
`recomp_lookup` & co. from inside it), a pack linked `-Bsymbolic --no-undefined` against it, a pack with its entry
only (the `dladdr` guard), and a shell with a title linked in (strong symbols win). `PackTest.drop` and
`LauncherPrepareTest.badPackFromShellOffersPrepareAgain` (Robolectric) cover the launcher side.

### Device check (title packs), for Alex

Needs the shell APK from this branch. Steps 1–5 need no toolchain in the APK (a pack made on a host, pushed); with the
toolchain, step 6.

1. Shell APK, no pack, your game chosen: logcat `PACK_MISSING <TID> (no /data/user/0/fr.adserver.xboxrecomp/files/
   packs/<TID>/pack.json)`, then the NO_TITLE message as before. `adb logcat -s xboxrecomp`.
2. A pack for the stub XBE made on a host (prepare.md « Checking on a host », with `--shell-lib` = the `libmain.so`
   out of **this** APK: `unzip -p xboxrecomp-shell.apk lib/arm64-v8a/libmain.so > libmain.so`). Push the stub's
   `default.xbe` as the game (`/sdcard/Android/data/fr.adserver.xboxrecomp/files/game/`), then the pack:
   `adb push packs/12345678 /data/local/tmp/` and
   `adb shell run-as fr.adserver.xboxrecomp sh -c 'mkdir -p files/packs && cp -r /data/local/tmp/12345678 files/packs/'`.
3. Start the app, Play. Expected: `PACK_LOADED 12345678 …/files/packs/12345678/libtitle.so (… bytes, "…",
   shell_abi 1; exports: recomp_lookup …)`, `xboxrecomp ready (… title)`, the five `stub title: …` lines and
   `stub title: all 5 lifted functions OK`, then `[FBWIN] guest returned (0)`. Back twice quits; `adb logcat -b crash`
   empty.
4. Unloadable pack, same size (the launcher's check passes it):
   `adb shell run-as fr.adserver.xboxrecomp dd if=/dev/zero of=files/packs/12345678/libtitle.so bs=64 count=1 conv=notrunc`
   (zeroes the ELF header). Play: `PACK_DLOPEN …` (bionic's « not a valid ELF » or similar), the launcher comes back
   with the red note « The prepared game could not be loaded (…). Prepare it again. » and Home shows **Prepare**
   (with the toolchain) or **Play**; `files/packs/12345678` is gone (`run-as … ls files/packs`). Screenshot.
5. Wrong ABI: edit `pack.json` to `"shell_abi": 2` (the launcher then shows Prepare: `Pack.check` refuses it too).
   Then set it back to 1 and `"abi": "x86_64"`: Play → `PACK_INVALID abi x86_64, this shell is arm64-v8a`, back to the
   launcher.
6. With the toolchain APK: Prepare your game, Play: `PACK_LOADED <TID>`, the game boots as the per-title APK did
   (same logcat up to the first frames). Note the time from tapping Play to `PACK_LOADED` (dlopen of the real pack,
   ~150 MB, with `RTLD_NOW`).
7. The stub APK (static title) from the same commit: unchanged (no `PACK_` line at all), the « Device check
   (XBA-52) » steps 1 and 3.
8. Send the logcat lines.

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

### Quick reboot

A title that quick-reboots into its own `default.xbe` (`HalReturnToFirmware(2)` after `XLaunchNewImage`; Fable's
Pause → Quit → Confirm, which goes back to its main menu) is restarted, not ended
([docs/runtime/README.md](../runtime/README.md), « Quick reboot »). The kernel writes the launch data to
`<internal files dir>/save/launch_data.bin` and calls the restart handler, which `shell_main.c` sets before the title
starts (`restart_via_launcher`): it calls `ShellActivity.restartGame` through JNI from the guest thread, which sends
`LauncherActivity` the explicit intent `fr.adserver.xboxrecomp.RESTART_GAME`. The launcher, in its own `:launcher`
process, does what it already does after a new game choice: `endRunningGame()` (`Process.killProcess` on the main
process, waiting at most 2 s), then starts `ShellActivity` with `NEW_TASK | CLEAR_TASK` in a new main process, and
finishes without showing anything; the play session stays open. A new process is needed because the runtime maps its
memory once per process. The new process's kernel takes the launch data back, and the title starts where it asked to
(Fable: its main menu, no logos). The native side waits to be ended; if it is still alive after 10 s it exits by itself
(`recomp_quit(0)`), leaving the launch data for the next start. If the JNI call fails, the title exits as before.
Logcat: `[KERNEL] quick reboot: launch data in …`, `xboxrecomp: quick reboot: launcher asked to restart the game`,
`shell: quick reboot, asking the launcher to restart the game`, `launcher: the game asked for a restart (quick
reboot)`, `launcher: ending the running game (pid …)`, then the new process's `Launch data: quick reboot page at …`.
Past 3 restarts in 30 s the kernel refuses and the title exits.

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

1. From **shell-latest**, install the stub APK and push its XBE, then start « XRecomp stub »:
   `adb install -r xboxrecomp-stub-<commit>.apk`,
   `adb push stub-default.xbe /sdcard/Android/data/fr.adserver.xboxrecomp.stub/files/game/default.xbe`,
   `adb logcat -c && adb logcat -s xboxrecomp SDL DEBUG AndroidRuntime`. Expected in logcat: `defaults: RECOMP_FB_WINDOW=1
   RECOMP_PB_EXEC=1`, `xboxrecomp ready (… title)`, `starting the title`, the boot lines (`xbox_MemoryLayoutInit:
   complete`, `Starting game...`), five `stub title: …` lines and `stub title: all 5 lifted functions OK`, then
   `[FBWIN] guest returned (0); Back twice to quit`. The screen is black (the stub draws nothing), with a « The title finished (code 0). Press Back twice to quit. »
   toast.
2. Home, wait 10 s, come back: `lifecycle: paused (background)`, then `lifecycle: resumed after ~10000 ms`.
3. Back twice: `[FBWIN] Back pressed twice, quitting`, `xboxrecomp: clean exit`, the app closes; `adb logcat -b crash`
   is empty.
4. Crash lines: push a `recomp.cfg` with `RECOMP_SHELL_CRASH_TEST=1` to the **shell** app (no title) and start it: the
   `writing to NULL now` line is in logcat above the `Fatal signal 11` and the backtrace. Remove the file afterwards.
5. Send the logcat lines.
