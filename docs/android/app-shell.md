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
| `LauncherActivity` | The launcher entry ([The launcher](#the-launcher)): welcome, game choice and import, home (the game's art and title, one line, a solid Play / Prepare and three quiet pills: Settings, Controls, Manage), in its own process `:launcher`. `Theme.java` and `Widgets.java` draw it, `TitleArt.java` decodes the game's title image, `GameArt`, `GamePalette` and `GameTheme` make the [game theme](#game-theme) from it, `UiFeedback.java` plays its sounds and haptics; `RecompConfig.java` writes its settings to `recomp.cfg`. With a valid game and « Open the game directly next time » on, it starts `ShellActivity` at once and shows nothing. |
| `ShellActivity` | Subclass of SDL's `SDLActivity`, which loads `libSDL2.so` and then `libmain.so`. SDL's Java side is compiled from the pinned sources (`$SDL2_SRC/android-project`), not copied here. Not exported: started by the launcher, after it sets `RECOMP_GAME_DIR`. Holds the [in-game menu](#in-game-menu) (`GameMenu`), a View layer over SDL's surface. |
| `libxba_import.so` | The launcher's ISO import: `xiso_jni.c` around the static `xiso` target (`src/xiso`); and « Choose Lossless.dll » ([Frame generation](#frame-generation)): `lsfg_jni.c` around the shader cache (and the DXBC translator in a `-PxbaLsfg=1` build with DXVK). Loaded only in `:launcher`. |
| `libmain.so` | `shell_main.c` + the **whole** toolkit (`--whole-archive`) + the title, when one is linked (see [A title in the shell](#a-title-in-the-shell)). Loading it on a device proves that bionic resolves every symbol the runtime uses. Four symbols that a title provides are weak stubs in `shell_main.c`, and so is the title's hook `recomp_title_main`. |
| logcat | `src/platform/android_log.c`: stdout (INFO) and stderr (WARN) go to the tag `xboxrecomp`, one entry per line. `xbox_android_log_init()` is the first call of `SDL_main`. |
| Window | Black, fullscreen, landscape (`sensorLandscape` + `SDL_HINT_ORIENTATIONS`). |
| Back | Opens and closes the [in-game menu](#in-game-menu) (ShellActivity takes it before SDL); quitting is in the menu (see [Quit](#quit)). A gamepad's own Back key stays that pad's Select for the game, as before. SDL still traps Back (`SDL_HINT_ANDROID_TRAP_BACK_BUTTON`); its old Back-twice path (`QUIT_PROMPT` toast, twice within 2 s quits) is only reached where nothing above SDL takes Back. |
| Manifest | minSdk 30, target 35, arm64-v8a only, GLES 3.0 required, `allowNativeHeapPointerTagging="false"`, `isGame`. |
| Frame generation | `-PxbaLsfg=1 -PlsfgVkSrc=… -PdxvkSrc=…` ([Frame generation](#frame-generation)); off by default. |
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
3. Press Back once. The in-game menu slides in from the left; logcat: `shell: menu open (Back)`. The app stays open.
4. Quit game, then Quit game again (the second press confirms). logcat: `shell: menu: quit (confirmed)`, then
   `clean exit`, and the app closes.
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
code (`Theme.java` for colours, type, spacing and sizes; `Widgets.java` for the custom views: list rows, pill
buttons, tabs, the Options sheet, the focus highlight, the import ring, status dots, button hints). Everything is laid out on a 1920 x 1080 design grid that `Widgets.Stage` scales to the
window (wider or taller screens get a wider or taller grid, never a stretched one), so it looks the same from 1280 x 720
to 1920 x 1080. The window is fullscreen and immersive (system bars hidden, back with a swipe), drawn under the display
cutout, and the stage keeps its content out of the cutout's insets. System fonts only (`sans-serif`,
`sans-serif-medium`); the look is our own (deep green-black, one vivid green accent, warm off-white text), with no
console or game maker's logo or name in the art.

### Screens

One activity, a small state machine of screens (`LauncherActivity.Screen`) with a back history instead of fragments:

| Screen | What it shows | Reached from |
|---|---|---|
| Library | The [game library](#game-library): a full-width grid of the games (five columns), the focused game's state and play time as one context line at the bottom left; Rescan, Storage, Library folder, Settings and About in the **Options** sheet (Y). | Every start when the library is on, B from a game's Home. |
| Storage | What the app stores per game, each deleted on its own ([Storage](#storage)). | Options (Y) on the library. |
| Welcome | What the app is (a recompiled game, the game not included: bring your own copy), three plain steps, a « Get started » button. | First start, when no game is set up at all. |
| Choose your game (source) | A list: **Game folder** (read in place with All files access, otherwise copied; the value says which), **Disc image (ISO)** (extracted once; the free space in the top bar), **Game library folder**; the focused choice explained on the right. | Welcome, Manage › Change game, Play with no usable game, the « Change game » shortcut, `NO_GAME_DATA`. |
| Read the game in place? | Only without All files access: **Allow all files access** (recommended; opens the system page, then the folder picker) and **Copy into the app** (the folder picker), with the explanation on the right (in place: no copy, fine for a 4 GB disc, only that folder is read; copy: as much free space again, a few minutes, removed with the app). | Game folder. |
| Preparing the game (import) | A plain progress ring (percent), the step (« Checking », « Copying », « Extracting »), « X of Y », the current file, the free space. The screen stays on; B cancels (nothing to focus). | A picked folder or image. |
| Home | No top bar: the game's title image (or a monogram) in the [game theme](#game-theme)'s colours, its title (the XBE certificate's name, else the folder's name), **one line** — when it was played (« Yesterday · 12 h 40 min played », « Not played yet »), or its state when it is not ready (« Needs preparing · once, about 20–30 min », « Prepare failed · CODE », the Prepare's progress, « Queued (n) », « Game data missing » with the folder, …) — and **one row of buttons**: a solid **Play** (or **Prepare**, **Preparing…**, **See what failed**, **Choose a game**) and three quiet pills, **Settings**, **Controls**, **Manage**. Before the first play the note says where the in-game menu is. | A finished import (Play focused), every start when « Start in the game » is off, the « Launcher » shortcut, B from the screens above. |
| Settings | Four tabs (General, Graphics, Audio, Advanced; L1 / R1) of [options](#settings) as « label  value » rows, left / right (or A) to change, the focused option's description on the right. About ends Advanced. | Home, Options. |
| Controls | The player's **remapping** ([Controls and remapping](#controls-and-remapping)): one row per Xbox control (A B X Y, Black, White, the triggers; Back, Start, the stick presses; the D-pad; the two sticks), its value the player's button, changed like a setting; « Reset controls » (also Y). The description column says what the focused row does and draws the handheld with, next to each control, what the game gets from it. | Home. |
| Manage | **Change game** (another folder or image, or the same one again to re-import; **Library** when the [game library](#game-library) is on); **Delete the imported copy** (only when the game is the app's own copy, `files/games/<TID>/` or the older `files/game/`; its size as the value; press A twice within 5 s; saves are kept); the game's pack, Prepare files, caches and saves with their sizes, each deleted on its own ([Storage](#storage)). The technical facts (title ID, prepared date, pack size, folder, source, size, free space) are the first row's description. | Home. |
| About | Version (`-PxbaVersion`), build (`-PxbaVersionCode`), app id, the game check (the first digits of `-PxbaXbeSha256`, or none), a one-line licence summary, and that no game data is included. **X**: Licences. | The end of Settings › Advanced, Options on the library. |
| Licences | XBA-100. Every licence text the APK carries, monospaced, one after the other: `LICENSE`, `NOTICE`, then `LICENSES/*.txt` (copied into the assets `licences/` at build time by `xbaLicencesAsset`). D-pad up / down scroll, L1 / R1 page, a finger scrolls too. | X on About. |

Messages and errors (`NOT_A_GAME_FOLDER`, `WRONG_GAME`, `NOT_ENOUGH_SPACE`, « Import canceled », « Saved », ...) go to
the left of the footer line (the hints are on its right), with a dot coloured by kind; there are no toasts and no dialogs. logcat (tag `xboxrecomp`):
`launcher: shown (...)` naming why, then one `launcher:` line per step (folder, hash, copy, import, game set, play,
settings saved, errors).

### When it shows

| Case | What happens |
|---|---|
| A usable game and « Start in the game » off (the default) | Home, Play focused. |
| A usable game and « Start in the game » on | `ShellActivity` starts at once; nothing shows. |
| The game is running in the background and the app icon is pressed | Back to the game at once, whatever « Start in the game » says. |
| Nothing remembered, but `RECOMP_GAME_DIR` in `recomp.cfg`, or `default.xbe` pushed to `files/game/` | A usable game, as above: the adb workflow still works (source « Set in recomp.cfg » or « Imported copy »). |
| Nothing set up | Welcome. |
| The remembered folder is gone (deleted, SD card out) | Home, « Game data missing » and the old path on its line (`GAME_FOLDER_GONE`), « Choose the game again » focused; the previous choice stays until a new one is made. |
| The folder is there but unreadable because All files access was turned off | Home, « No file access » and why on its line (`GAME_FOLDER_NO_ACCESS`). |
| `default.xbe` there but not the one this APK was built for (files changed since the choice, or set up through `recomp.cfg` / adb) | Home, « Wrong game » on its line, Play dimmed: it says `WRONG_GAME` instead of starting. Checked once per folder in the background, only when the APK has an expected hash. |
| The native side found no `default.xbe` (`NO_GAME_DATA`) | `shell_main.c` calls `ShellActivity.openGamePicker(dir)`; the launcher opens on Choose your game with `NO_GAME_DATA` and that folder in the note; B goes to Home. |
| Long-press on the app icon, « Change game » | Choose your game, even with a game set up; B goes to Home. If the game is running, the note says it will close when another game is chosen; saves are in internal storage and stay. |
| Long-press on the app icon, « Launcher » | Home, even with « Start in the game » on or the game running. |

The two shortcuts are dynamic (the package name differs per title, which a static `shortcuts.xml` cannot follow),
published each time the launcher runs, so they appear after the first start.

A new game, once checked, becomes a record of the [game library](#game-library) and the selected game; the launcher
goes to Home with Play focused and « TITLE is ready » in the note, instead of starting the game by itself. It
becomes the launched game (the preferences ShellActivity reads) and a running other game is ended only when it is
played.

### Controls

| Input | Does |
|---|---|
| D-pad, left stick | Move the focus (Android turns the stick and the hat into D-pad keys). Each screen (and each Settings tab) remembers its focused item. Home's buttons are one row: left / right. |
| A (`BUTTON_A`), D-pad centre, Enter | Select the focused item; on a setting, the next value. |
| B (`BUTTON_B`), Back, Escape | Back one screen; on Home or Welcome, leave the app (on a library game's Home: the library); during an import or a Prepare, Cancel (a Prepare: twice); with the Options sheet open, close it. |
| Left / right on a setting | Previous / next value. |
| L1 / R1 | Settings: previous / next tab (wrapping). |
| Y | Library: the **Options** sheet (Rescan, Storage, Library folder, Settings, About; B or Y closes it, the focus comes back). Settings: the defaults. Preparing: hide it (the Prepare carries on). A failed Prepare: Share log. |
| Start | Home: Play (not shown as a hint). Library: the focused game's primary action. |
| Touch | Tap an item to select it, the arrows of a setting to step it, a button hint to do what it says. After a touch, the first D-pad or A press brings the focus back. |

A and B are taken on release, once, so a held button does not repeat. Dimmed rows and buttons (Play without a usable
game) stay focusable so that pressing them says why. Each screen shows **three hints at most**, on the right of the
footer line (`LauncherActivity.MAX_HINTS`: a fourth is a bug and throws). « Start in the game » is in Settings ›
General only.

### Look, motion and sound

« Calm » (the 2026-11 redesign): fewer things on screen, one way to show the focus, secondary text quiet.

- **Type**: five sizes, two weights (`Theme`): Display 64 (Home's title), Title 40 (a screen's title), Row 34 (a
  row's label, a button), Body 28 (values, lines, descriptions), Small 24 (group labels); nothing smaller (hints 26),
  no Light weight, no spaced capitals. Spacing on an 8-pixel scale (8, 16, 24, 32, 48, 96); 96 px side margins.
- **Colour**: the tokens of `GamePalette` (and the [game theme](#game-theme)) unchanged. All secondary text is
  `TEXT_DIM` (8.5:1 on the backdrop); `TEXT_FAINT` only says « disabled ». The backdrop is the gradient with one soft
  glow (7 %), or, on Home and Preparing only, the game's hero instead; no dot grid, bezel, corner ticks, rules or
  panel outlines. A flat `PANEL` surface only for overlays (the Options sheet, the in-game drawer).
- **Chrome**: a top bar with the screen's title only (Settings adds its tabs, Storage the free space on the right;
  Home and Preparing have none), and one 72 px footer line: the note on the left, at most three hints on the right
  (outlined glyphs, 26 px labels).
- **Focus**: one highlight for the screen (`Widgets.Glide`): a flat `ACCENT` plate at 14 % with a 2.5 px `ACCENT`
  outline (radius 16) around a row or a quiet button, a 4 px ring around a game card's art; the solid Play is its own
  highlight. It glides from the item it leaves to the item it reaches in 150 ms; the focused item grows to 102 %.
  Contrast (stock and every hue, `CalmContrastTest`): `TEXT_DIM` on the plate 4.5:1 or more, `TEXT_BRIGHT` 7:1, the
  outline 3:1 on the backdrop, the ink on Play 4.5:1.
- **Rows**: label left, value right (`ACCENT` when focused), the « < > » arrows of a setting only on the focused row;
  no subtitles: the focused row's description shows as plain text in a quiet column on the right.
- **Screens** fade in and slide in from the side they come from (forward from the right, back from the left), 150 ms;
  a Settings tab slides in from its side.
- **Settings**: a value pulses when it changes (160 ms). A setting row keeps the width of its widest choice for the
  value, and a label too long for what is left ends in « … » (measured with the row's own paint).
- **Sounds and haptics** (`UiFeedback`): five short sounds, made with `soundcraft-cli` from our own synth recipes
  (`tools/android/ui_sounds.sh`, WAVs in `res/raw`, 16-bit 22.05 kHz, about 115 kB in all): **move** (focus or a
  value), **select**, **back**, **error** (an error note) and **done** (a game set up, a Prepare finished), played
  through SoundPool at a low volume, each with a haptic (`performHapticFeedback`: clock tick, keyboard tap, reject,
  confirm; Android's own « touch feedback » setting applies). Settings › General › **Sounds** and **Vibration** turn
  them off (`game` SharedPreferences, `ui_sound`, `ui_haptics`).
- **Preparing screen**: Home's layout with the buttons replaced by the progress, so nothing jumps: « Preparing · step
  5 of 6 · Compile », one 12 px bar, the percentage and the time left, and that the app can be left. When the Prepare
  ends: Home with Play focused, the « done » sound and haptic. After two minutes without input it dims (OLED
  burn-in): a black layer with the percentage and the step in grey, moved a little each minute, and the window's
  brightness lowered; any key or touch wakes it and does nothing else.

### Device check (Calm launcher), for Alex

`adb logcat -s xboxrecomp` throughout; Fable and Burnout 3 in the library, the Thor.

1. Home (Fable): no top bar; the art, the title, one line « <when> · <time> played », a solid Play and three quiet
   pills (Settings, Controls, Manage) in one row; hints A Select, B Library only. Left / right moves along the row
   (the plate glides, Play stays solid). Start still plays. Screenshot.
2. A game that needs preparing (Burnout 3): an amber dot and « Needs preparing · once, about … », the button reads
   Prepare. Start a Prepare: the same layout, the bar moving, « Preparing · step n of 6 · <step> », the time left;
   no row to focus; Y hides it, B twice cancels. Screenshot of each.
3. Settings: the tabs in the top bar, L1 / R1 switch them (and wrap), each tab keeps its focus, no tab scrolls;
   only the focused row shows its arrows; its description on the right, plain. Graphics: Frame cap and Resolution
   dimmed until Renderer is GPU. Advanced ends with Reset all and About. A long French label is cut with « … » and
   never runs under its value. Screenshot of each tab, in English and French.
4. Library: five columns, « Ready » not written under a ready game, the others with a dot and their state
   (« Not imported » grey); the focused game's line at the bottom left. Y opens Options from the right: Rescan,
   Storage (free space), Library folder (its name), Settings, About; D-pad stays in it; B closes it and the focus is
   back on the card. X does nothing any more. Screenshot with and without the sheet.
5. Storage (from Options): « … free » in the top bar, the games' names as small labels, sizes as values, A twice
   deletes (« Press A again » replaces the size); paths in the description. Manage (Home): the title ID, prepared
   date, pack size and folder in the first row's description.
6. In-game menu: a flat sheet flush with the left edge (about 30 % of the screen), the title and « Paused · 0:42
   played », six rows (Quit apart), B Resume; Settings holds Perf overlay, Volume, Bottom screen, Language and Share
   log. Screenshot.
7. Bottom screen (Thor): pure black, the art, the title, one pill (Play, Prepare, Import…) and the battery; on the
   library it follows the focused card, on Settings or Storage only the art shows. While preparing: the big
   percentage, the bar, « <step> · about … left », Hold to cancel. Nothing in the bottom centre.
8. Contrast and comfort: the dim text readable over the hero (Fable) at arm's length; the focused row's value and
   description readable on the plate; say if anything looks too faint or too small at 1080p on the Thor.
9. Send the screenshots (`adb exec-out screencap -p > x.png`) of Home, Preparing, the four Settings tabs, the
   library with and without Options, Storage, the in-game menu and both bottom-screen states.

### Game theme

The launcher takes its colours from the game: Home shows the game's own title image, and the accent (the focus,
values, the Play button, the tabs' underline), its glow and the neutrals' hue come from it, with a faint blurred
copy of the image washing in from the right of the backdrop. The text colours and the **status colours never change**:
`Theme.OK` (ready, done, connected: the stock green), `WARN` (amber), `ERROR` (red). Nothing of a game is in the APK or
the repo: everything is read from the player's own copy and kept in app-private storage.

**Where the image comes from** (`GameArt`, first found): default.xbe's `$$XTIMAGE` section (an XPR0 resource file
holding one texture, usually 128 x 128 DXT1), its `$$XSIMAGE` (the save image), a loose `TitleImage.xbx` then
`SaveImage.xbx` in the game folder (any case; its root first, then a walk at most 3 folders deep, 2000 entries, files
up to 4 MB), then `<internal files>/save/UDATA/<title id>/TitleImage.xbx`, the copy the game writes next to its saves.
Only default.xbe is read (dashupdate.xbe and update.xbe hold dashboard media). XPR0 and the textures are decoded in
Java (`TitleArt`: DXT1 / DXT3 / DXT5, and swizzled or linear A8R8G8B8 / X8R8G8B8 / R5G6B5); formats from public
documentation (xboxdevwiki « Xbe » and « XPR ») and `src/d3d` (format codes, swizzle).

**The colours** (`GamePalette`, pure Java, a 1:1 port of `tools/android/theme_preview/palette.py`): the image is
box-downsampled to 64 x 64 and its pixels taken to OKLCH; greys, near-black and near-white carry no hue. Two 36-bin
hue histograms (« vivid »: chroma squared, dark pixels count less; « area »: pixel count) give the peaks. The accent
takes the most vivid hue, the neutrals the hue covering the most area, the second glow the next peak 40° or more away.
Every token keeps the stock token's OKLCH lightness: the neutrals take a little chroma (at most 0.035) in the game's
hue, the accent keeps 7:1 on `PANEL_HI` (its lightness raised when needed), and an accent within 18° of `ERROR`'s hue
is pushed out of it. `GamePalette.WARN_AVOID_DEG` would do the same around `WARN`'s amber for warm games: 0 (off) for
now, set it to 15 if an orange accent reads like the amber « Needs preparing » chip on the device. A 360-hue sweep
keeps every contrast pair the UI relies on (TEXT on PANEL_HI 7:1, TEXT_DIM 4.5:1, TEXT_FAINT on BG_TOP 3:1, the
accent on its surfaces 7:1, ...). Fable: accent `#A7C2FF` (periwinkle), blue-black neutrals, a rose second glow.

**The hero backdrop** (`GamePalette.bakeHero`): 48 px on its long side, Gaussian-blurred (σ 3 px), then a gain in
linear light so the 95th-percentile luminance is 0.012 and nothing above it: small text keeps its contrast over the
brightest pixel (a render test checks TEXT_FAINT 3:1 and TEXT_DIM 4.5:1). `Widgets.Backdrop` draws it cover-fit,
on Home and Preparing only, gone left of 30 % of the width and in the bottom 40 % (the buttons and hints stay on the
plain gradient), at most 85 % opaque; no glow over it; one bitmap and cached shaders, nothing per frame.

**Cache** (`GameTheme`), per title ID in `<internal files>/art/`: `<TID>.png` (the title image), `<TID>.hero.png`,
`<TID>.theme` (text: `v=<GamePalette.VERSION>`, `xbe=<length>:<mtime>`, `source=`, `hues=`, `hero=`, one
`TOKEN=AARRGGBB` per colour). Baked on the import worker before Home shows, or lazily on a worker for a game imported
before (then the screen is shown again); stale when the version or default.xbe changes. « Delete the imported copy »
deletes it. Each bake logs `launcher: theme <TID>: source xbe:$$XTIMAGE, accent #A7C2FF, hues 265.9/265.9/-, mode
full`, and the launcher logs the same line when it applies a theme (`launcher: theme <TID>: stock, mode off` without).

**Where it applies**: the launcher (applied before any view is built, so no flash of the stock green; when the game or
the setting changes, the stage is built again in the new colours), the bottom screen's panel (it also
shows the title image; the panel's background stays black), and in the game process the in-game menu and the
companion panel (`ShellActivity` reads the cached tokens once at game start: no bitmaps). Without a usable image
(none, or an unknown format) Home shows a monogram (the title's initials on a gradient whose hue comes from the title
ID) and the colours take that hue; a grey image keeps its picture and hero with the title ID's hue. No title ID, no
game: the stock colours.

**Settings › General › Game theme** (only with a game that has a title ID): Art and colours (default), Colours only
(no hero), Off (the stock green), per title in `files/settings/theme_<TID>`; applies at once in the launcher, at the
next game start in the in-game menu.

**Previews on a Mac** (`tools/android/theme_preview/`, Python 3 and Pillow; no art in the repo): `xart.py <xbe> <out>`
extracts a title image from your own XBE, `palette.py <png>` prints the palette and its contrast checks, `mock.py`
draws mock-ups, `golden.py` makes `GamePaletteTest`'s golden vectors. With a game folder of your own,
`gradle testDebugUnitTest -PxbaPreviewGameDir=/path/to/game --tests '*ThemePreviewTest'` renders the real launcher in
its theme to `build/screenshots/preview-*.png` (git-ignored); without the property the test is skipped.

### Device check (game theme), for Alex

`adb logcat -s xboxrecomp` throughout; Burnout 3 already imported, Fable's folder at hand.

1. Open the launcher with Burnout 3: one line `launcher: theme <TID>: source …, accent #…, hues …, mode full` (send
   it, and whether the source is `xbe:$$XTIMAGE`). The first start after the update bakes it on a worker: Home shows
   again in the game's colours within a second.
2. Home: the accent (focus, Play) in the game's colour, a faint wash of the title image on the right of the backdrop,
   no banding in it; a not-ready state's dot keeps its status colour (amber, red).
3. The dim line under the title stays easy to read over the wash.
4. Settings › General › Game theme: Colours only (the wash goes, the colours stay), Off (the stock green at once),
   Art and colours again; the description names the game.
5. Bottom screen (Thor): black background, the title image, Play in the game's colour; it dims after 10 s.
6. Play: the in-game menu in the game's colours, the screenshot flash green.
7. Change game to Fable (import or folder): periwinkle (#A7C2FF) without restarting the app.
8. Force-stop the app and open it again: no flash of the stock green before the game's colours.
9. Manage › Delete the imported copy: no `files/art/<TID>.*` left (`adb shell run-as <app id> ls files/art`),
   unless the game is still in the library folder (the library keeps showing its art).
10. Screenshots of Home with both games (and of a warm-coloured game next to the amber « Needs preparing » chip, if
    one is at hand: the accent is not kept away from the amber yet, `GamePalette.WARN_AVOID_DEG`).

### Play time

« Last played » and « Play time » are kept simple: a session starts when the launcher starts `ShellActivity` (not when
it only brings a running game back) and ends when the launcher shows again with the game no longer running; a session
the launcher never saw end counts for at most 6 hours (`game` SharedPreferences, `last_played`, `play_ms`,
`session_start`).

`LauncherScreensTest` renders every screen and Home state in English and French at 1280 x 720 and 1920 x 1080, each
with its focused row, to `build/screenshots/ui-<state>-<en|fr>-<720|1080>.png` (`--target app-unit-tests-out`);
`TitleArtTest` checks the XBE section, the XPR0 container and the texel decoders on blobs it builds; `GamePaletteTest`
and `GameArtTest` (plain JUnit) the palette against `palette.py`'s golden vectors, the contrast sweep, the hero cap and
the image sources; `GameThemeTest` the cache; `LauncherScreensTest` also renders Home in a blue, a warm and a grey
game's theme (`ui-home-themed-*`) and the Game theme setting (`ui-settings-theme-*`).

### Settings

The Settings screen has four tabs — **General**, **Graphics**, **Audio**, **Advanced** — switched with L1 / R1; it
writes a few `RECOMP_*` options to `recomp.cfg` (the file the runtime already reads,
[RECOMP_* options](#recomp_-options-recompcfg)), and they apply the next time the game starts. Only options that
exist in the runtime and mean something to a player are offered:

| Row | Choices (default first or marked) | Line in `recomp.cfg` |
|---|---|---|
| General › Start in the game | Off (default), On | none: a launcher preference (`game` SharedPreferences, `auto_start`) |
| General › Sounds | On (default), Off | none: `ui_sound` |
| General › Vibration | On (default), Off | none: `ui_haptics` |
| General › Bottom screen (only with a second display) | Panel (default), Black, Off | none: [Dual screen](#dual-screen-ayn-thor), `GameStore.bottomScreen` |
| General › Game theme (only with a game that has a title ID) | Art and colours (default), Colours only, Off | none: [Game theme](#game-theme), `files/settings/theme_<TID>` |
| General › Language | Same as Android (default), English, Français, Deutsch, Español, Italiano, Português, 日本語, 한국어, 中文 | `RECOMP_LANGUAGE=…`; « Same as Android » writes nothing ([Game language](#game-language)) |
| Graphics › Vertical sync | On (default), Off | `RECOMP_PRESENT_VSYNC=0` when off |
| Graphics › Renderer | Auto (default: the title profile's choice, software today), Software, GPU | `RECOMP_GPU=0` / `1`; see [GPU back end](../technical/gpu-backend.md#android) |
| Graphics › Frame cap (dimmed unless Renderer is GPU) | Auto (default: the title profile's choice, 60 unless the profile sets one; Fable: 30), 30, 60, Off | `RECOMP_GPU_FPS_CAP=30` / `60` / `0` |
| Graphics › Resolution (dimmed unless Renderer is GPU) | Auto (default: the title profile's choice, 1× for now), 1× (original), 2×, 3×, Fit the screen | `RECOMP_GPU_SCALE=1` / `2` / `3` / `auto` (Fit: the screen's height / 480, at most 3); see [GPU back end](../technical/gpu-backend.md#internal-resolution) |
| Graphics › Aspect ratio | Auto (default), 4:3 (original), 16:9 | `RECOMP_ASPECT=4:3` / `16:9`. Auto: 16:9 for the frames a title draws wide (a title patch declares them; Fable on Android) with menus and videos kept in their shape, 4:3 otherwise; 16:9 stretches every frame of a title that draws no wide frames; see [GPU back end](../technical/gpu-backend.md#aspect-modes) |
| Graphics › Frame generation (only in a [frame generation](#frame-generation) APK) | Off (default), 2×, 2× performance | `RECOMP_LSFG=1`, `RECOMP_LSFG_MULT=2`, plus `RECOMP_LSFG_PERF=1` for 2× performance |
| Graphics › Choose Lossless.dll (same) | a document picker; the row shows the shaders' state | `RECOMP_LSFG_DIR=<files>/lsfg/<key>` once the shaders are there (not removed by « Reset all ») |
| Graphics › Texture filtering | Sharp (default), Smooth, Anisotropic 8×, Anisotropic 16× | `RECOMP_TEX_BILINEAR=1`, `RECOMP_TEX_ANISO=8` / `16` |
| Audio › 3D audio (HRTF) | On (default), Off | `RECOMP_APU_HRTF=0` when off |
| Audio › Audio buffer | Short (256), Normal (default, 512), Long (1024), Very long (2048) | `RECOMP_AUDIO_BUF_SAMPLES=…` |
| Advanced › Drawing threads | Auto (default), 1, 2, 4, 6, 8 | `RECOMP_RASTER_THREADS=…` |
| Advanced › Fast cores | On (default on Android), Off | `RECOMP_BIG_CORES=0` when off |
| Advanced › Self-test | Off (default), On | `RECOMP_SHELL_SELFTEST=1` |

#### Game language

The console language the runtime reports to the title (`RECOMP_LANGUAGE`, XBA-77, `GameLanguage.java`). First found
wins: the title's own choice (in-game menu › Settings › **Language**, kept per title ID in
`files/settings/language_<TITLEID>`), then the `RECOMP_LANGUAGE` line of `recomp.cfg` (Settings › General › Language),
then the Android language when the Xbox has it, then English. `ShellActivity` sets the variable before the runtime
starts for the first and the third (the environment wins over `recomp.cfg`), and clears it otherwise; logcat says
« shell: game language … ». A title only has the languages on its disc: a US disc is often English whatever is
chosen.

How the file is written (`RecompConfig.java`): it is parsed as `recomp_cfg.c` does (one `KEY=VALUE` per line, blanks
and `#` comments skipped, keys and values trimmed, the first line for a key wins). A change rewrites the key's first
line in place and removes its later duplicates; a default value removes the key's lines (the runtime default then
applies); a new key is appended under the comment « Written by the launcher's Settings screen ». Every other line
(comments, other variables, `RECOMP_GAME_DIR`) is kept as written. The file goes through `recomp.cfg.tmp` and a
rename; no file is created while every option is at its default. A value the screen does not offer (written by hand)
shows as « Custom » and stays until that row is changed. « Reset all » (the end of Advanced, or Y on any tab)
removes the lines of these options only. Frame generation forced off by the game's pack (`env_force` RECOMP_LSFG=0)
stays changeable (it is global) but is dimmed, and its description says, in amber, that it is unavailable for this
title.

Asked for in XBA-41 but left out, because no runtime option backs them on Android: the output scaling and its filter
(the SDL presenter always letterboxes the 640 x 480 frame with nearest-neighbour sampling, `fb_present_sdl.c`;
`RECOMP_WINDOW_SCALE` only sizes a desktop window and `RECOMP_PRESENT_FILTER` is the D3D11 presenter's), an fps cap
for the software renderer (only the GPU renderer has one, `RECOMP_GPU_FPS_CAP`), a stats overlay (no such switch; control remapping has its own screen since XBA-131, [Controls and remapping](#controls-and-remapping)), and vibration (`RECOMP_RUMBLE` only gates the emulated USB
pad, `RECOMP_USB`, a per-title opt-in). Bring-up and debug switches stay out on purpose.

### Controls and remapping

XBA-131. One mapping for every title and every pad, `RECOMP_PAD_MAP` in `recomp.cfg` (`PadMapping`), which
`src/input/xinput_device.c` reads when the game starts (`[INPUT] RECOMP_PAD_MAP: …` in the log). No line is the
usual mapping: SDL's game controller layout by position (south = A as Android names it), L1 = **Black**, R1 =
**White**, L2 / R2 = the triggers, Select = **Back**, the stick presses = the thumbstick buttons. The text is
`control=source` entries for what differs, `left_stick=` / `right_stick=` (`left`, `right`, `none`), e.g.
`a=east,b=south,black=left_trigger,right_stick=none`; the grammar is `xbox_InputMap_PadMapParse`
([environment variables](../runtime/environment-variables.md#input)).

The **Controls** screen (from Home) is a Settings-like list, pad only: the D-pad moves, left / right or A change the
focused row's button (every source in turn: A B X Y, L1 R1 L2 R2, L3 R3, Select, Start, the D-pad's four, each stick's
four directions, Not used), Y or « Reset controls » puts the usual mapping back (the line goes), B goes back. Each
change is written at once (the « Saved » note; a running game takes it at its next start). The top right says how many
controls differ. The description column says where the focused control comes from (and which other control the same
button also gives: allowed, one press then gives both), and draws the handheld with each of the player's buttons and
what the game gets from it.

What it does not touch: the launcher's own navigation and the [in-game menu](#in-game-menu) (Android key events: A
selects, B goes back, Select + Start held opens the menu, whatever the mapping), the companion panel's Back / Start /
Black / White (Xbox buttons already, `input_overlay.h`), the keyboard (`RECOMP_KEYBOARD`, its own bindings) and the
Windows backend. Deadzones stay as before (none added); triggers keep their travel, and a trigger or a stick direction
used as a button counts as pressed from half way.

**Towards the touch pad (XBA-42).** A phone without the Thor's second screen needs an on-screen pad. What it reuses:
`input_overlay.h` (`xbox_InputOverlaySet`, merged into a real pad's state, a stand-in pad 0 without one; the
companion panel already drives it through `CompanionNative.nativeOverlay`), `CompanionPanel`'s pointer handling (each
pointer on its own, release on cancel and detach) and `PanelView`'s drawing, and `PadMapping.Control` with
`ControlsLabels` for the buttons' names. The overlay's buttons are Xbox controls already, so the remapping does not
apply to them (it maps a physical pad's buttons); the overlay needs analog sticks in `nativeOverlay` (today buttons
only) and a layout of its own drawn over the game.

### Device check (Controls, XBA-131), for Alex

1. Home › Controls: the list starts on A = A; the right column draws the Thor with « L1 · Black », « R1 · White »,
   « L2 · LT », « A B X Y ». Only the D-pad and A / B / Y move and change things. Screenshot.
2. On A press right once: A = B, « 1 changed » top right, the note « Saved ». On B press left once: B = A.
   `adb shell cat /sdcard/Android/data/<app id>/files/recomp.cfg` ends with `RECOMP_PAD_MAP=a=east,b=south`.
3. Play: in the game, the right face button confirms and the bottom one goes back (logcat
   `[INPUT] RECOMP_PAD_MAP: a=east,b=south`). The launcher and the in-game menu still use A to select.
4. In-game menu › Controls: « Your mapping » first, then « B · A » and « A · B ».
5. Back in Controls, Black = L2 (A twice on Black): in a game that uses Black, L2 does it, and L2 still pulls the
   left trigger too.
6. Right stick: Not used: the game's camera stick no longer moves. Then Y: everything back, the `RECOMP_PAD_MAP` line
   is gone from recomp.cfg, the game plays as before.
7. With an external pad paired (Bluetooth): the same mapping applies to it.
8. French: the screen in French (« Commandes », « Noir », « Blanc »).

### Frame generation

Lossless Scaling frame generation (LSFG 3.1, [Frame Generation](../technical/framegen.md)) in an APK built with:

```bash
cd android && JAVA_HOME=… SDL2_SRC=… gradle -PxbaLsfg=1 \
    -PlsfgVkSrc=/path/to/lsfg-vk-android \
    -PdxvkSrc=/path/to/dxvk \
    assembleDebug assembleRelease            # + -PxbaTitleDir=… etc. for a title's APK
```

| Gradle property | Environment | What |
|---|---|---|
| `-PxbaLsfg=1` | | Builds `XBOXRECOMP_LSFG` (pacer, presenter path, shader cache) into `libmain.so`. Off by default. |
| `-PlsfgVkSrc=<dir>` | `LSFG_VK_SRC` | GameNative/lsfg-vk-android at d33caceb with its `thirdparty/volk` submodule: the Vulkan bridge. Without it gradle warns and the launcher hides the rows (`R.bool.xba_lsfg`). |
| `-PdxvkSrc=<dir>` | `DXVK_SRC` | DXVK v2.6.2 with `include/spirv`: the DXBC translator in `libxba_import.so`, so the phone translates at import. Without it gradle warns; the phone only extracts, and a cache translated on a desktop (`tools/lsfg/lsfg_translate --dir`) can be pushed into `<files>/lsfg/<key>`. |

In the launcher, Settings › Graphics:

- **Frame generation**: Off / 2× / 2× performance (above). The game then runs at 30 real fps (cap rule) and shows 60;
  the overlay's `LSFG` line gives real / shown fps and generation time. Any failure (no Vulkan, no shaders, too slow
  30 frames in a row) logs `[LSFG] off: <reason>`, shows one toast, and the game goes on with normal frames.
- **Choose Lossless.dll**: `ACTION_OPEN_DOCUMENT`; the picked file's fd goes to `LsfgNative.importDll(fd,
  <files>/lsfg, …)` on the import worker (the ring screen, « Preparing the frame generation shaders… »). The DLL is
  read through the fd only, never copied into the app's storage; its shaders land in `<files>/lsfg/<key>` (key: the
  first 16 hex digits of its SHA-256). The row then says **Shaders ready (LSFG 3.1)**, **This Lossless.dll version is
  not supported**, or **… (translator missing)** (DXBC found, APK built without `-PdxvkSrc`); in the first and last
  case `RECOMP_LSFG_DIR` goes into `recomp.cfg`.
- `<files>/lsfg` is kept out of backups and device transfers (`res/xml/backup_rules.xml`,
  `data_extraction_rules.xml`; `allowBackup` is off anyway) and `LogProvider` refuses any path under it.

Device check: [framegen.md](../technical/framegen.md) and the Device checklist of the frame generation work.

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
3. A on « Get started »: Choose your game, Game folder focused. D-pad down / up moves between the choices;
   B goes back to Welcome; A again.
4. Copy `stub-default.xbe` as `default.xbe` into a folder of internal storage (e.g. `Download/stubgame/`). A on Game
   folder: « Read the game in place? ». A on « Allow all files access », turn it on, come back: the folder picker
   opens. Pick the folder. Expected: Home, title of the stub (or the folder's name), chip « Ready », Play focused, the
   note « … is ready »; logcat `launcher: game set: /storage/emulated/0/Download/stubgame`.
5. A on Play: the stub title starts (`storage: game=/storage/emulated/0/Download/stubgame ... (default.xbe found)`,
   `stub title: all 5 lifted functions OK`), and « The title finished (code 0) » on the black screen.
6. Press Home on the Thor, then the app icon: back to the game at once (logcat `launcher: game running, bringing it
   back`). Back, Quit game twice to quit it, start the app again: Home with Play focused (« Open directly » is off).
7. Settings: L1 / R1 to the Graphics tab, D-pad through the rows, left / right change Vertical sync and Texture
   filtering, the description on the right follows the focus. Then
   `adb shell cat /sdcard/Android/data/fr.adserver.xboxrecomp.shell/files/recomp.cfg`: the comment line and
   `RECOMP_PRESENT_VSYNC=0` / `RECOMP_TEX_BILINEAR=1` (or what you chose). Play: logcat `recomp.cfg: N option(s)
   from …` lists them and `[FBWIN] … vsync off`. Back in Settings, Y: the lines are gone.
8. Hand-written lines survive: `adb shell "echo RECOMP_WAIT_LOG=1 >> /sdcard/Android/data/fr.adserver.xboxrecomp.shell/files/recomp.cfg"`,
   change a setting, `cat` again: `RECOMP_WAIT_LOG=1` is still there.
9. Settings › General › « Start in the game » on; quit and start the app: the game starts directly.
   Long-press the icon › « Launcher »: Home. Turn it off again in Settings.
10. Long-press the app icon › « Change game »: Choose your game, the note says the running game will close (if it
    runs). Pick an empty folder: `NOT_A_GAME_FOLDER` in the note, nothing changed; B goes to Home.
11. Folder gone: rename the folder of step 4 with a file manager, start the app. Expected: Home, « Game data
    missing » and the old path on its line (`GAME_FOLDER_GONE`), no crash; A on « Choose the game again » opens
    Choose your game.
12. Copy fallback: Settings › Apps › the app › All files access off. Game folder › « Copy into the app », pick the
    folder of step 4 (renamed back). Expected: the progress ring, then Home with Play focused, Manage showing
    « Imported copy » and the folder `/storage/emulated/0/Android/data/<app id>/files/game`.
13. ISO: make an XISO of our own test data (e.g. `extract-xiso -c stubgame stub.iso`, or any image dumped from your own
    disc), push it to `Download/`. Disc image (ISO), pick it. Expected: the ring with the percentage, « X of Y », file
    names, the screen stays on, then Home. Do it once more and press B (or Cancel) halfway: « Import canceled » in the
    note on Choose your game, and the previous game still plays. Re-import your own disc's image too (it worked
    before this change).
14. Pick a file that is not an ISO (e.g. a photo): `NOT_AN_ISO` in the note.
15. Manage › Delete the imported copy: the first A shows « Press A again », the second deletes; then Welcome (or
    Home without a game) and « The imported copy was deleted ». Saves: `adb shell run-as fr.adserver.xboxrecomp.shell ls files/save`
    still lists them.
16. `NO_GAME_DATA` (after step 15, so that no game is remembered): `adb shell mkdir /sdcard/Download/empty`, then
    `adb shell "echo RECOMP_GAME_DIR=/sdcard/Download/empty >> /sdcard/Android/data/fr.adserver.xboxrecomp.shell/files/recomp.cfg"`.
    Start the app: Home; Manage says « Set in recomp.cfg ». Play: the native side finds no `default.xbe` and hands over;
    the launcher opens on Choose your game with `NO_GAME_DATA` and the folder in the note. Remove the line afterwards.
17. WRONG_GAME needs an APK built with `-PxbaXbeSha256=` of another XBE (ask for one, or build one): picking the stub
    folder then shows `WRONG_GAME`, and the previous choice still plays.
18. Touch: tap the choices, the buttons and rows, a setting's arrows, the Settings tabs, the hints (B Back). Everything works without the gamepad,
    and the next D-pad press shows the focus again.
19. Switch the Thor to French: every launcher text is in French (screens, settings, notes, shortcuts).
20. Send screenshots of Welcome, Choose your game, Read the game in place?, the import ring, Home, Settings, Manage
    and About, and the logcat lines (`launcher:`, `import:`, `recomp.cfg:`, `storage: game=...`). Say if the A /
    B buttons are swapped on the Thor (its « Xbox » / « Nintendo » layout setting changes the key codes the app sees).

## Game library

One APK, several games. A **library folder** (for example `/sdcard/xbox`) holds disc images (`*.iso`, `*.xiso`) and
extracted game folders (a folder with a `default.xbe`); the launcher lists them, and the player imports the ones he
plays. Games are never run from the image: an image is extracted once into `<external files>/games/<TID>/`, a game
folder read as files is played in place (no copy). Prepare packs, Prepare status, the title image, the game theme,
the language and the saves were already per title ID; the few caches that are not (see below) change hands when the
launched game changes.

### Turning it on

Choose your game › **Game library folder** (third card). With All files access the folder is read as files (the
tree URI resolved to a path, `java.io.File`, images opened with `ParcelFileDescriptor`); without it,
FOLDER_ACCESS offers **Allow all files access** or **Use the file picker** (Storage Access Framework: images are
imported from their document, folders must be copied, chip « Needs copying »). The read permission on the tree is
kept (`takePersistableUriPermission`). Preferences (`game`): `library_tree`, `library_path` (absent in SAF mode),
`library_scanned_at`.

The library is **on** when a library folder is set, or the launcher knows two games or more. Otherwise everything is
as before (Welcome, Home of the one game, Change game): the one-game launcher and its tests are unchanged.

### Scan

`LibraryScanner` on a worker: at most four levels, 5,000 entries; names starting with « . » and `Android/` skipped; a
folder holding `default.xbe` is a game folder (not descended). Each new or changed image gives its `default.xbe` only
(`XisoNative.extractFile`, at most 32 MB) into `files/library/xbe/<TID>-<sha8>/` (art, theme, hash, and the Prepare's
source before an import); an unchanged one (location, size, modification time) is not opened again. The title image
and the game theme are cached there and then (`files/art/<TID>.png`, `<TID>.theme`). Files that are not games (not
an Xbox image, no or a bad `default.xbe`, unreadable) make the dimmed « Other files (n) » line under the grid. The
index is `files/library/index.json` (rebuildable). Rescans: each time the library opens (path mode), Y, a new folder;
in SAF mode only at a cold start and on Y. logcat: `launcher: library: scan N items, M games, T ms (…)`.

### The library screen

The grid (the full width, five columns of 288 px art, scrolling): one card per title ID — title image or monogram,
one-line title, and its state as a dot and a label only when it is not **Ready** (« Not imported » is a neutral
grey dot) — sorted by title. The focused game's state and play time are one context line at the bottom left
(« Ready · Yesterday · 12 h 40 min played »). D-pad between cards (a ring around the art glides to them), **A** the
game's Home, **Start** its primary action without opening Home, **Y** the **Options** sheet — **Rescan**, **Storage**
(the free space), **Library folder** (its name; A chooses another), **Settings**, **About** — and **B** leaves the
app. An empty library shows why (no folder yet, nothing found, the scan's progress) and a **Library folder** button.
The scan's progress, a scan cut at 5,000 entries and the « Other files (n) » line go under the grid. The library is
always in the stock colours; only the backdrop's glow follows the focused card (from its cached `<TID>.theme`).
Opening a game's Home applies its theme; back to the library resets it. The bottom screen shows the focused game:
its art, title and primary action.

States (`GameState`, first that applies): **Running**, **Preparing**, **Queued (n)**, **Importing**, then with the
game's files present **Prepare failed**, **Pack invalid** (the pack was built from another `default.xbe` —
`Pack.xbeSha256` against the record's — or the shell refused it), **Ready**, **Needs preparing**; without them
**Not imported**, **Needs copying**, **No file access**, else **Source gone** (an in-place folder that vanished) or
**Game data missing**. A game with neither files nor source leaves the grid (its pack and saves stay in Storage).
Variants: images with the same title ID are one game; the active one is the record's source, else the most recently
modified.

### Home of a library game

Home shows the **selected** game (`Library.Games.selected`): its folder, state, title ID, title, play time. B goes
back to the library. With its files: as before (Play, Prepare, Preparing, See what failed), plus **Queued (n)** (A
takes it out of the queue) and, when another game runs, Play warns first (« X is running and will close », a second
press within 5 s ends it). Without them: **Import** (the image extracted, or the SAF folder copied, into
`games/<TID>`; a needed Prepare starts right after), **Copy into the app**, **Choose the folder again**, and
**Prepare now**: tools.prepare needs only `default.xbe`, so the cached copy is the Prepare's source and several games
can be prepared (queued) before their gigabytes are imported.

**Selected vs launched.** The preferences `game_dir`, `title` and the new `title_id` keep meaning « the game the
process runs or last ran » (ShellActivity, quick reboot, the game's language read them). Opening another game's Home
writes nothing there; `GameStore.rememberLaunch` runs only in `launchShell()`. The launcher opens on the last
launched game; « Open the last game directly » starts it.

**Caches.** Saves (`UDATA/<tid>`, `TDATA/<tid>`) are per title, and so is Z: (`save/Cache/<tid>`, since K-9; the
stash below still moves `save/Cache` as a whole, which is harmless). Entries of `save/Cache` from before K-9 belong
to the owner: `SaveCaches.migrateLegacyCache` renames them into `Cache/<owner>` when that title is launched (owner
unchanged, or its stash restored), once and without replacing anything, so Fable does not unpack its data again. `save/Cache` (Z: and the cache partitions),
`save/TitleCache` (E:\CACHE), `Partition3-5.img` and `launch_data.bin` are not: `files/library/cache_owner` names their
title, and `launchShell()` calls `SaveCaches.switchTo` (only while no game process runs): the live set moves by rename
into `save/.stash/<owner>/`, the new title's stash comes back, `launch_data.bin` (a quick-reboot hand-off) is
dropped. Nothing is wiped: Fable's unpacked cache does not have to be rebuilt after a game of Burnout 3.

**Play time** is per record (`last_played`, `play_ms`): a session opens at launch (`session_tid`) and closes when the
launcher comes back.

### Storage

Options › Storage on the library: the free space in the top bar, then per game (its name as a small label) what the
app stores — **Imported copy**,
**Pack**, **Prepare files** (`files/work/<TID>`), **Game caches** (live or stashed), **Saves** — with their sizes.
Each is deleted on its own (A twice within 5 s, on a worker, after ending the game that runs from it). Saves have
their own row and a stronger warning; nothing else deletes them. The sizes are the rows' values, the paths are in
the focused row's description. Manage shows the same rows for the selected game.
Only what the app made is ever deleted: never a folder read in place nor anything in the library folder. Deleting
the imported copy keeps the game in the library (import it again); its art and theme go only when the title also
left the library folder.

### Files

| What | Where |
|---|---|
| Records (launcher only; temporary file, then rename) | `files/library/games.json`: `selected`, per title ID `title`, `source`, `data_dir`, `data_kind` (`imported`, `in_place`, `cfg`), `xbe_sha256`, `added_at`, `last_played`, `play_ms` |
| Scan index (rebuildable) | `files/library/index.json` |
| Cached `default.xbe` | `files/library/xbe/<TID>-<sha8>/default.xbe` |
| Imported games | `<external files>/games/<TID>/` (staged in `games/.import`, swapped once checked) |
| Owner of the shared caches, stashes | `files/library/cache_owner`, `files/save/.stash/<TID>/` |

**Migration.** The first start of this version (`Library.migrate`, no file moves): the game a launcher without the
library would run (the chosen folder, else `RECOMP_GAME_DIR`, else `files/game/` with a `default.xbe`) becomes the
first record and the owner of the caches, `title_id` is written; logcat `launcher: library: migrated <TID> (<dir>)`.
`files/game/` stays that game's data and is never an import target again; a later import of the same title into
`games/<TID>` removes it once the new copy is checked (and no game process runs). An older APK still finds its
preferences and `files/game/`. The adb workflow (`RECOMP_GAME_DIR`, pushing to `files/game/`) still works.

### A test library

`tools/android/make_test_library.sh OUT_DIR [COUNT]` builds a synthetic library: images made with
`tools/xiso/mkxiso.py` around the conformance XBE with patched title IDs, names, regions and versions (two versions
of one game), one game folder, one nested image, a non-Xbox `notes.iso`, and a hidden and an `Android/` image that
the scan skips. `adb push OUT_DIR /sdcard/xbox-test`, then choose it as the library folder. The games import in a
second; they do not run (the XBE is not a game).

Tests: `LibraryScreenTest`, `StorageTest` (plain JUnit), `LauncherLibraryTest` (routing, grid with 0 / 1 / 7 / 20
games in en / fr at 720p / 1080p → `build/screenshots/library-*.png`, navigation, theme, launched game written at
Play only, caches switched, not imported, Prepare now, queue, Storage), `LauncherPanelsTest` (`companion-library-*`).

### Device check (game library), for Alex

`adb logcat -s xboxrecomp` throughout. Library folder `/sdcard/xbox` with Burnout 3 and Fable.

1. Install over the current APK and open it: Fable's Home as before (same game, play time, saves). logcat
   `launcher: library: migrated 4D5300D1 (<dir>)` once (not at the next start).
2. Home › Manage › Library is not there yet (one game): « Change game » as before. Choose your game: a third card
   **Game library folder**. Choose `/sdcard/xbox`: the library opens, the grid fills; logcat
   `launcher: library: scan N items, M games, T ms (…)`. Send the line.
3. Burnout 3 shows once (one card even with several images of it, « n versions » in the details), with its title
   image; Fable shows **Ready**; the other files of the folder, if any, on the « Other files (n) » line.
4. D-pad between the cards (the highlight glides, the backdrop's glow takes each game's colour), A on Burnout 3: its
   Home in its colours, chip **Not imported**; B: back to the library in the stock green, the focus on Burnout 3.
5. Y: the rescan is quick (`… 0 opened, N unchanged`). Copy another ISO into `/sdcard/xbox`, Y: it appears.
6. Put a non-Xbox `.iso` (any file renamed) in the folder, Y: « Other files (1) » under the grid, no card.
7. Burnout 3 › Import: the progress ring; Cancel once (back to its Home, nothing left in `games/`), then Import again
   to the end: Home, a Prepare starts by itself (if the APK has the toolchain). `adb shell ls
   /sdcard/Android/data/<app id>/files/games/` shows its title ID.
8. While Burnout 3 prepares, Fable or another game › Prepare: its chip reads **Queued (1)**; A on it takes it out;
   start it again and let it run after the first one.
9. On a game not imported: **Prepare now** — the Prepare runs from the cached `default.xbe` (logcat `prepare
   started for <TID> (…/files/library/xbe/<TID>-…)`); it ends « … is prepared: import it to play ».
10. Play Fable, back to the launcher, play Burnout 3 (Play warns « Fable is running and will close », press again):
    logcat `launcher: game caches switched to <TID>`; back on Fable later, it starts without the long cache rebuild
    (`run-as <app id> ls files/save/.stash` shows the other game's caches).
11. In Fable, Pause › Quit (quick reboot): Fable comes back, not Burnout 3.
12. Library › Options (Y) › Storage lists both games; delete Burnout 3's Prepare files (A twice), then its imported copy:
    the game stays in the grid as **Not imported**, its saves row is still there; your ISO in `/sdcard/xbox` is
    untouched.
13. Rename `/sdcard/xbox` (or remove the SD card) and open the launcher: no crash; games read in place show
    **Source gone**, imported ones still play; rename it back, Y.
14. Bottom screen (Thor): on the library the focused game's art, title and action (Play, Import, …); on a game's
    Home its Play (or Prepare); on Settings and Storage the art alone.
15. In French: the library, the chips (« Non importé », « En attente (1) »), Storage.
16. Turn All files access off (system settings), choose the folder again with **Use the file picker**: the games
    are listed; folders read « À copier » / « Needs copying », images import.

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
dlopens it (« Title packs » below); neither checks `lib_sha256` at each start. A valid pack can still be stale
(XBA-127, [prepare.md « When a pack is stale »](prepare.md#when-a-pack-is-stale)): built from another `default.xbe`
(another revision or region under the same title ID: never run, Home offers **Prepare again**), or lifted by an older
toolkit than the APK's (it still plays; Home keeps Play and offers **Prepare again** beside it with a note).

**The launcher.** With a game ready and the toolchain present:

| Case | Home | Preparing screen |
|---|---|---|
| No valid pack, nothing run | **Prepare** instead of Play, « about 20–30 min » (30–45 below 8 cores, 40–60 under 6 GB of RAM), « Needs preparing · once, about … » with an amber dot. Prepare refuses under 2 GB of free internal space, and warns (a note) under 6 GB of memory. | |
| Running | « Preparing… », the line « Compile · 42 % · about 12 min left » | Home's layout: « Preparing · step 5 of 6 · Compile », one bar, the percentage, the time left, « You can leave the app ». Nothing to focus: B twice cancels; Y hides it and the Prepare carries on. Polls `status.json` twice a second; the screen stays on. |
| Failed | **See what failed**, the line « Prepare failed · CODE » with a red dot | « Preparation failed · CODE » with a red dot, the message (a hint for `KILLED` and `NO_SPACE`), **Retry** and **Share log** (also Y) as buttons. |
| Cancelled, or interrupted (a `running` status older than 15 s with no `:prepare` process) | « Prepare » carries on where it stopped | « Preparation stopped » with an amber dot, why, **Resume** and **Share log**. |
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
2. With the toolchain APK and your game chosen: Home shows **Prepare** and « Needs preparing · once, about
   20–30 min » with an amber dot. Screenshot.
3. A on Prepare. On Android 13+ the notification permission is asked once: allow it. Expected: the Preparing screen,
   « Preparing · Install tools » for a few seconds the first time; then « step n of 6 » with Identify, Analyse,
   Translate, Compile, Pack in turn; the bar, the percentage and the time left move. logcat `prepare: started for <TID> ... N jobs`
   (N = 4 or 5 on the Thor).
4. Pull down the notification shade: « Preparing <title> », the step, the bar, the time left, **Cancel**. Screenshot.
5. Press Y: Home shows « Preparing… » and its line with the step and percent updating. Press Home on the Thor, use another app
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

### Device check (stale packs, XBA-127), for Alex

A toolchain APK and one prepared game (Play on Home).

1. `adb shell run-as <app id> cat files/packs/<TID>/pack.json`: it has `toolkit_sha256` (64 hex digits) once the game
   was prepared with this APK; `unzip -p <apk> assets/prepare/toolkit.zip.id` prints the same value.
2. Another XBE: with the game read in place (or imported), replace its `default.xbe` by another revision or region of
   the same disc (same title ID; keep the original aside). Open the app: within a second Home says « Another version
   of the game · prepare it again » with an amber dot, the primary is **Prepare again**, and the note says saves are
   kept. Screenshot. A on it starts a Prepare (from scratch: logcat `prepare: started`). Before it ends, nothing
   starts the old pack (turn on « Open the game directly », restart the app: Home, no game).
3. Put the original `default.xbe` back: Home shows **Play** again (the old pack matches it again, or the new one does
   once the Prepare ended). Your saves in-game are all there.
4. Older toolkit: install an APK built from a commit whose toolkit tree changed (any change under `tools/recomp/`) over
   the current one, without clearing data. Home: « Prepared by an older version of the app », **Play** first and a
   quiet **Prepare again** next to it, a note about the update's fixes. Play starts the game at once, as before.
   Screenshot.
5. Install an APK whose commit changed only Java (or docs): no « older version » line; Play as before.
6. From step 4, **Prepare again**: a full Prepare runs; when it ends, Home shows Play without the line, and the game
   does not start by itself.

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
  [ Start ]   [Share log] [  Menu  ] [ Quit  ]   [ Black ]
                                     (hold)
               (Perf details)       nothing to touch bottom-centre (palms)
```

- **Back / Start / White / Black**, in the edge columns next to the thumbs: duplicates of Select, Start, R1 and L1
  (`xinput_device.c` maps L1 to Black and R1 to White), there so the player finds them. Each pointer is tracked on its
  own (two thumbs on two buttons work, sliding from one to the next works); everything is released on
  `ACTION_CANCEL`, when the panel is dismissed or detached, and in `ShellActivity.onStop`.
- **Launcher** opens the launcher (the game waits in the background, as with Home). **Menu** opens or closes the
  [in-game menu](#in-game-menu) on the top screen. **Quit** needs a hold of 0.8 s (it fills in red), then ends the game
  (`recomp_quit(0)`). **Screenshot** saves the last frame as
  `<external files>/screenshots/<date>.bmp` (24-bit BMP of the frame shown, `xbox_FramebufferDumpBmp`: 640 x 480,
  or the GPU renderer's own size with an internal resolution above 1×, e.g. 1440 x 1080 at Fit the screen on a
  1080p display) and says « Saved … » in the status strip. **Perf** shows more figures on the panel (draws per frame,
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

`LauncherCompanion` does it in a few calls, wired in `LauncherActivity` like this (the panel's one action is the
main screen's: Play goes through the launcher's own `onPlay`, so a Prepare still comes first):

```java
private LauncherCompanion companion;

// onCreate, after buildStage() (not on the early launchShell() return):
companion = new LauncherCompanion(this, this::onPlay);
companion.libraryActions(() -> libraryPrimary(focusedEntry()));

@Override protected void onStart() { super.onStart(); if (companion != null) companion.start(); }
@Override protected void onStop() { if (companion != null) companion.stop(); super.onStop(); }

// where Home is built (its state, whether its button can run, and its label: Play, Prepare, …):
companion.home(title, statusText, statusColor, !play.dim, play.title);
// on the library, for the focused card:
companion.library(e.title(), chipText, chipColor, art, primaryLabel(e));
// where the Preparing screen is built:
companion.preparing(titleId(), gameTitle(dir));
// on every other screen:
companion.artOnly();
```

Home and the library show one panel (`LauncherPanel`): pure black, the game's art (440 px), its title, one pill
(Play, Prepare, Import…; y 716–844, the bottom centre left empty for the palms) and the battery small in the corner;
no card, status or other shortcuts. The other screens show the art alone. Preparing shows the overall percentage
big, one bar, « Compile · about 12 min left », the title and **Hold to cancel** (it sends `PrepareService`'s cancel
itself); it reads `status.json` once a second. Buttons are borderless pills (the in-game panel's too, rounded).

### Tests and screenshots

`DualScreenTest` (chooser over one, two, private and off displays; decisions; focus mode), `CompanionPanelTest`
(multi-touch, slide, cancel, detach, taps, hold-to-quit, dimming, shift; through a fake sink), `CompanionScreenTest`
(Robolectric `ShadowDisplayManager.addDisplay` / `removeDisplay`: shown, moved, dismissed, Off / Black, `onStop`,
Plan A not focusable and Plan B focusable and forwarding, the launcher's panel switch), `LauncherPanelsTest`. The
panels are rendered at 1240 x 1080 to `build/screenshots/companion-*.png` (`--target app-unit-tests-out`).

### Device check (dual screen), for Alex

On the Thor, with the shell APK of this change (and the stub or a title for steps 5 to 9):

1. Start the app: the launcher on the top screen. Until the launcher wiring lands the bottom screen shows the system's
   own content; after it, the launcher panel (the art, the title, one Play pill). logcat
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

## In-game menu

A drawer that slides in from the left over the game (Eden / BananaNative style) with XRecomp's own options: resume,
settings (with share log), controls, screenshot, back to the launcher, quit. `GameMenu.java` (the drawer and its
input), `MenuTriggers.java` (the edge swipe and the gamepad combo as plain state machines), `MenuNative.java` /
`android/app/src/main/cpp/menu_jni.c` (hold, volume, quit), wired in `ShellActivity`.

```
  ┌──────────────────────┐
  │ <game title>         │   the game's frame, dimmed,
  │ Paused · 0:42 played │   stays behind (held, not redrawn)
  │                      │
  │ Resume               │                        59.9 fps · 16.7 ms
  │ Settings             │                        (performance overlay,
  │ Controls             │                         also with the menu closed)
  │ Screenshot           │
  │ Back to launcher     │
  │                      │
  │ Quit game            │
  │ (A) Select (B) Resume│
  └──────────────────────┘
```

**How it is put together.** A View layer (`FrameLayout`: a dimming scrim, then the launcher's `Widgets.Stage` with the
drawer, the focus `Glide`, the performance overlay and a message line) added over SDL's layout with
`addContentView` in `ShellActivity.onCreate`. SDL's surface is never recreated or resized. The drawer is a flat sheet
flush with the left edge, 560 of the 1920 design pixels (29 %), over a light scrim: the game's title, « Paused · 0:42
played », text-only 88 px rows (no icons, no subtitles; Quit stands apart), the gliding focus, its sounds and haptics
(`UiFeedback`); it slides in in 200 ms (decelerating) while the scrim fades in. The Stage scales it to the screen
less the display cutout, so 720p and 1080p look the same.

**Opens by** (logcat `shell: menu open (<why>)`):

| Trigger | Notes |
|---|---|
| Android Back (the navigation bar, the back gesture, a keyboard) | Opens; Back again (or B) closes. A Back key **from a gamepad** SDL reads (`SDLControllerManager.isDeviceSDLJoystick`) is that pad's Select for the game, as it always was (SDL maps `KEYCODE_BACK` from a pad to its Back button), so it does not open the menu. |
| Guide / Mode (`KEYCODE_BUTTON_MODE`) | The Xbox pad has no such button, so no game uses it: the menu takes it whole. Many handhelds keep it for the system; then it never reaches the app. |
| Select + Start held 0.5 s | Both still reach the game while held (each alone is a game button: Back and Start); when the menu opens, the game gets their release. Xbox games read Back and Start each on its own, so holding both half a second is free. |
| A swipe in from the left edge (top screen) | Down within 3 % of the width from the left edge, then 6 % of the width to the right, mostly sideways. The game sees the touch begin and gets an `ACTION_CANCEL` when the swipe is recognised. With Android's gesture navigation the same swipe may also be the system's back gesture: a Back within 0.6 s of a swipe-open is ignored, so the menu stays open. |
| « Menu » on the companion panel (second screen) | Opens or closes it. |

**While open.**

- **The game is held**: `MenuNative.nativeHold(true)` → `recomp_lifecycle_pause_for(RECOMP_HOLD_MENU)`, the same
  hold as the background ([Background and resume](#background-and-resume)): the flip parks, the title's threads stop
  behind it, the audio device pauses at once. The window keeps showing the last frame, dimmed. Closing calls
  `nativeHold(false)`. The two reasons are kept apart, so Home with the menu open, then back, leaves the game paused
  under the menu.
- **Input goes to the menu only**: `ShellActivity.dispatchKeyEvent` / `dispatchGenericMotionEvent` /
  `dispatchTouchEvent` ask `GameMenu` first, and what it takes never reaches SDL. D-pad, hat and left stick move the
  focus (left / right step a setting), A selects, B or Back goes back (closes from the main page), Start or Guide
  resumes. A, B and Start act on their **release**, and only for a press that went down while the menu was open: the
  release of the key that opened it does nothing, and the key that closes it never reaches the game. Volume keys stay
  the system's. Touch: tap a row, tap the dimmed game to close.
- **No stuck buttons**: when the menu opens it hands SDL the release of every key the game saw go down (`KeyEvent`
  copies, sent straight to `SDLActivity.handleKeyEvent`), a centred event for every joystick that moved (sticks,
  triggers and hat at rest, to SDL's surface), and releases the companion panel's buttons (`input_overlay`: still on,
  nothing held); it releases the panel's again when it closes.

**Items.**

| Item | Does |
|---|---|
| Resume | Closes the menu. |
| Settings | **Perf overlay** On / Off: fps and frame time (`xbox_FramebufferWindowGetStats`, twice a second) in the top right corner, in the menu's layer, not in the game frame; stays with the menu closed. **Volume** 0–100 % in steps of 10, A mutes / unmutes: one gain on the mixed APU output (`apu_volume.c`, the square of the fraction, after the WAV capture), set through JNI. **Bottom screen** Panel / Black / Off (only with a second screen): `GameStore.setBottomScreen` and the companion is refreshed at once. These three apply **now** (the rows say so) and are remembered (`files/settings/perf_overlay`, `game_volume`, `bottom_screen`); the volume is applied again at the next start. **Language** (only when the game's XBE gives a title ID): As in Settings, then the nine languages; this game's own [language](#game-language), applied at the next start. Screen filter and scaling are not offered: the SDL window's are fixed at start (`fb_present_sdl.c`: nearest, logical size 640 x 480). |
| Controls | The usual mapping, compact (plus Back and Select + Start: the menu); with a remapping in `recomp.cfg`, first « Your mapping » and each changed control (your button, the Xbox control). |
| Screenshot | Closes the menu first, then saves the game's last frame as `<external files>/screenshots/<date>.bmp` (`xbox_FramebufferDumpBmp`, the companion panel's), and says « Saved … » at the top of the screen (and on the panel). |
| Settings › Share log | This process's logcat to `files/logs/game.log`, then the share sheet (the companion panel's `LogProvider` path). The menu stays open. |
| Back to launcher | **The game stays alive** (the runtime maps its guest memory once per process: starting it again means a new process), held: the drawer goes at once, the launcher comes up (`ACTION_SHOW_LAUNCHER`), and the menu's hold is let go only in `onStop`, after SDL's pause has put the background hold in place, so no frame runs in between. The launcher's Play brings this task back and the game goes on (`launcher: game running, bringing it back`). |
| Quit game | The first press turns the row into « Press again to quit » (with « Progress since the last save is lost » under it, the only row that says more) for 4 s (moving away cancels it); the second press quits: `nativeQuit` lets the guest go, `sync()`s the saves, then `recomp_quit(0)` ([Quit](#quit)). |

The header shows the game's title (the launcher's choice) and « Paused · h:mm played », the time since the game
started (this process). Hints: A Select, B Resume on the main page, B Back on Settings and Controls.

**Tests and screenshots.** `GameMenuTest` (Robolectric, a fake `GameMenu.Host` standing in for libmain and SDL): Back,
a gamepad's Back, Guide, Select + Start timing (0.4 s no, 0.55 s yes, released early no), the edge swipe and what is
not one, the panel's Menu; nothing reaching the game while open; the releases on open (keys, a centred joystick
event, the overlay) and on close; B acting on release; stick and hat navigation with repeat; Back to launcher holding
until `onStop`; Quit's confirmation and its timeout; the quick settings by pad (live, remembered, applied at start);
the slide; the drawer's width. Rendered over a made-up game frame to `build/screenshots/menu-*.png`: closed, main,
quit armed, quick settings, controls, closed with the overlay and a message; en and fr at 1080p and 720p
(`--target app-unit-tests-out`). C: `tests/recomp_lifecycle` (the reasons) and `tests/apu_volume`.

### Device check (in-game menu), for Alex

With the shell APK of this change and the stub or a title (and the Thor for 5 and 9):

1. **Back**: in the game, press Android Back (the navigation key or the back gesture). The menu slides in from the
   left over the dimmed game, Resume focused. logcat `shell: menu open (Back)`, `lifecycle: paused (menu)`. Back
   again: it slides out, `shell: menu closed`, `lifecycle: resumed after N ms`.
2. **Paused, with sound**: open the menu during a scene with music or motion: the picture freezes and the sound
   stops at once; Resume: both come back where they were. Then open the menu, press Home, wait 10 s, come back: still
   paused under the menu (`lifecycle: still paused (menu)`), Resume continues.
3. **Swipe**: swipe from the very left edge of the top screen to the right: the menu opens (`menu open (swipe)`), and
   it stays open even if the system's back gesture fires with it. Tap the dimmed game: it closes.
4. **Combo and Guide**: hold Select + Start: after about half a second the menu opens (`menu open (Select+Start)`);
   press each alone in the game: the game still gets them. If the pad has a Guide / Home button the app receives,
   it opens the menu (`menu open (Guide)`); say whether the Thor's does. Also say what the Thor's own Back button
   does: opens the menu, or acts as Select in the game (logcat shows which).
5. **Bottom panel**: tap « Menu » on the companion panel: the menu opens on the top screen; tap it again: it closes.
6. **No stuck buttons**: hold A (or a direction, or the right trigger) in the game and open the menu with Back while
   holding; let go inside the menu, close it: nothing is held in the game. Same while holding White on the bottom
   panel. Close the menu with B and with A on Resume: the game does not get that B or A.
7. **Navigation**: D-pad, hat and left stick move the focus (held: it repeats); A selects; B goes back from Settings
   and Controls; Start resumes from anywhere.
8. **Settings**: Perf overlay On: the fps and frame time show in the top right corner, also after Resume
   (compare with the companion's fps). Volume: left / right change the game's volume in steps (audible after
   Resume), A mutes; quit and start again: the volume is remembered. Bottom screen (Thor): Panel / Black / Off switch
   the bottom screen at once.
9. **Screenshot**: Screenshot closes the menu and « Saved <date>.bmp » shows at the top;
   `adb shell ls /sdcard/Android/data/<app id>/files/screenshots`, pull the BMP: the game's frame, without the menu.
10. **Settings › Share log**: the share sheet opens with game.log; cancel it: the menu is still there, the game still paused.
11. **Back to launcher**: the launcher comes up and the game waits (no sound; logcat `paused (menu)` then
    `still paused (background)` and no `resumed` in between). Play: the game is back where it was, running
    (`launcher: game running, bringing it back`).
12. **Quit**: Quit game once: the row asks « Press again to quit »; wait 4 s: it goes back to « Quit game ». Press it
    twice: `shell: menu: quit (confirmed)`, `quit from the in-game menu`, `clean exit`; start the game again: the
    saves are there.
13. **French**: with the device in French, every row, hint and message of the menu is in French. Photos of the menu
    (main page, Settings) at least once, in French.
14. **Notch / 720p**: on a phone with a cutout, the drawer's text stays clear of it; on a 720p screen the menu looks
    the same as at 1080p.

### Device check (game language, XBA-77), for Alex

With a title from a European (multi-language) disc, Fable's PAL disc for instance:

1. **Same as Android**: device in French, Settings › Game › Language on « Comme Android » (the default): start the
   game. logcat `shell: game language fr, title 4D5300D1`; the game's menus are in French.
2. **Settings**: Settings › Game › Language → English; quit the game and start it again: logcat
   `shell: game language en (recomp.cfg)`, menus in English.
3. **This game**: in the game, in-game menu › Settings › Language → Deutsch; quit and start again: logcat
   `shell: game language de`, menus in German (and the Settings line still says English).
4. **Back to Settings**: same row → « Comme les Réglages »; next start: English again.
5. **US disc** (if you have one): choose French: the game stays in English (its disc has nothing else), nothing
   breaks.
6. Say which languages Fable's disc offers, for the profile notes.

## Full screen

The game window hides the status bar and the navigation (gesture pill); a swipe from an edge shows them for a moment
(`SystemBars.java`, XBA-99). SDLActivity hides them with the old `systemUiVisibility` flags, which a focus change
clears: on the AYN Thor the companion Presentation on the second display took the focus, the bars came back and SDL's
surface was 1920x970. `ShellActivity` applies `WindowInsetsController.hide(systemBars())` with
`BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE` and the cutout mode `shortEdges` in `onCreate`, `onResume`, on every focus
gain, and 300 ms after `onStart` (once the companion screen is up).

Device check (Alex, Thor): 1. start a game with the bottom screen on « Panel »: no status bar, no pill, logcat
`SDL: ... 1920x1080`; 2. swipe up from the bottom edge: the pill shows, then hides again; 3. open and close the in-game
menu, then Home and back to the game: still full screen.


Frame times (XBA-29): every 10 s the presenter logs `[FBWIN] frame times over 10 s: N presents, mean X ms (F fps), sd
S ms, worst W ms` (intervals over 250 ms, a pause or a load, are left out). Device check on the Thor's 120 Hz screen:
a title at 30 fps and one at 60 fps each show sd under 2 ms in-game.

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
- the pause has reasons (`RECOMP_HOLD_BACKGROUND`, `RECOMP_HOLD_MENU`: `recomp_lifecycle_pause_for` /
  `resume_for`): the guest stays held while any is set, so the [in-game menu](#in-game-menu) open across a trip to
  the background keeps the game paused when the app comes back (`lifecycle: still paused (menu)`). Listeners (the
  audio device) see the first reason set and the last one cleared. Test: `tests/recomp_lifecycle`.

Guest time is not frozen: after a long pause the title sees the time that passed, as after a long frame. Most titles
clamp their frame time; if one does not, that becomes its own ticket.

Device check (XBA-24), shell part (the in-game part waits for a title, XBA-35):

1. Start the shell, press Home. logcat: `lifecycle: paused (background)`.
2. `adb shell top -b -n 1 | grep xboxrecomp`: under 2 % CPU.
3. Wait 5 minutes, bring the app back. logcat: `lifecycle: resumed after ~300000 ms`; the black screen is back, no
   crash, no tombstone.
4. Do it once more with a notification pulled down and with the screen turned off and on.
5. Back: the in-game menu; Quit game twice: `clean exit`.
6. Send the logcat lines and the `top` line.

## Fast cores (big.LITTLE)

Right after `recomp.cfg`, the shell calls `recomp_host_cpus_init()` (`src/platform/host_cpus.c`, XBA-25). It restricts
the main thread to the cores whose cpufreq maximum is above the slowest cluster's, before any other thread exists, so
every thread inherits that mask: guest threads, the rasteriser pool, SDL's. On the Thor (Snapdragon 8 Gen 2) that means
cpus 3-7 and not the three A510 cores. logcat: `host cpus: runtime on the fast cores 3-7 of 0-7 (...)`.
`RECOMP_BIG_CORES=0` in `recomp.cfg` turns it off. On desktop it is off unless `RECOMP_BIG_CORES=1`.


After a pass through the background (XBA-50): Android moves the app between cpusets (top-app, background), and on
kernels before 6.2 (the Thor runs 5.15) that move overwrites the mask each thread set for itself and does not put it
back. The runtime's threads note their mask when they start (the shim's CreateThread, the raster pool, the guest
threads' one-core pin, the main thread) and `recomp_lifecycle_resume` sets them again
(`recomp_host_cpus_restore`, one logcat line `host cpus: N runtime thread(s) back on their cores after resume`).
Threads that are not ours (ART, binder, SDL's) are left alone.

Device check (Alex, Thor): 1. start a game, note the `host cpus` line; 2. `for t in /proc/PID/task/*; do taskset -p
${t##*/}; done`; 3. Home, 10 s, back; logcat shows the `back on their cores` line and step 2 gives the same masks
as before.

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
   thread, `recomp_title_main(&store)` on a 64 MB guest thread, the lifecycle watch; quitting is the
   [in-game menu](#in-game-menu)'s. When the guest returns, the window stays on its last frame
   (`[FBWIN] guest returned (N); the window stays`): an app does not vanish on its own.
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
   `stub title: all 5 lifted functions OK`, then `[FBWIN] guest returned (0)`. Back, Quit game twice quits;
   `adb logcat -b crash` empty.
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

**Quit game** in the [in-game menu](#in-game-menu) (pressed twice: the second press confirms) lets the guest go
(`recomp_lifecycle_resume_for(RECOMP_HOLD_ALL)`), `sync()`s the saves the title wrote (plain files under
`<internal files>/save`) and calls `recomp_quit(0)` (`menu_jni.c`); the companion panel's Quit (a hold) calls
`recomp_quit(0)` too. Where nothing above SDL takes Back (not the app), Back twice in the title's window does the same.
`recomp_quit(0)` (`src/platform/recomp_quit.c`) runs the exit hook (the kernel's
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

### The title leaving by itself

A title that ends its own run — `HalReturnToFirmware` to the dashboard (XapiBootToDash, an empty launch path; a failed
start-up check such as KOTOR's before the DVD security sector existed), to an image this build cannot start, a
restart refused by the cap, or `HalInitiateShutdown` — goes back to the launcher, which says why. It used to call
`exit()` from the guest thread while the window, SDL and the other guest threads still ran: atexit handlers and
destructors pulled mutexes from under them, `FORTIFY: pthread_mutex_lock called on a destroyed mutex`, SIGABRT, and a
crash report (and a « Prepare again » offer) for a title that had merely quit.

`xbox_title_exit` (`src/kernel/kernel_reboot.c`, [docs/runtime/README.md](../runtime/README.md) « The title leaving »)
calls the exit handler `shell_main.c` sets before the title starts (`exit_via_launcher`). It calls
`ShellActivity.titleExited(why)` through JNI from the guest thread; that starts `LauncherActivity` with
`EXTRA_TITLE_EXITED` (the kernel's reason, English) and `NEW_TASK | CLEAR_TASK`, finishes the activity, and returns
once the UI thread has done so (at most 2 s). The kernel then ends the process with `recomp_quit(0)` (flush, log drain,
`_exit`): no atexit handlers, no tombstone, `ApplicationExitInfo` says the process exited by itself. The launcher waits
up to 2 s for the game's process to be gone (else ends it), so the play session closes and the exit is not taken for a
crash, then opens Home (Library beneath when the library is on) with the note « The game ended: the game returned to
the dashboard (routine 2). »; « Open the game directly » does not start it again. If the JNI call fails, the process
still ends through `recomp_quit`.

Logcat: `[EXIT] the game returned to the dashboard (routine 2)`, `shell: the game exited by itself (…), back to the
launcher`, `xboxrecomp: title exit: launcher opened (…)`, `xboxrecomp: clean exit`, then in `:launcher`
`launcher: shown (game exited: …)`. The quick reboot and the other image of the disc (XBA-82) are handled before any
of this and are unchanged. Test: `LauncherPrepareTest.titleExitShowsWhyOnHome` (Robolectric).

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
   `[FBWIN] guest returned (0); the window stays`. The screen is black (the stub draws nothing), with a « The title
   finished (code 0). Press Back for the menu (Quit). » toast.
2. Home, wait 10 s, come back: `lifecycle: paused (background)`, then `lifecycle: resumed after ~10000 ms`.
3. Back, Quit game, Quit game: `shell: menu: quit (confirmed)`, `xboxrecomp: clean exit`, the app closes;
   `adb logcat -b crash` is empty.
4. Crash lines: push a `recomp.cfg` with `RECOMP_SHELL_CRASH_TEST=1` to the **shell** app (no title) and start it: the
   `writing to NULL now` line is in logcat above the `Fatal signal 11` and the backtrace. Remove the file afterwards.
5. Send the logcat lines.
