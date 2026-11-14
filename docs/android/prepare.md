# Prepare on the device: the title pack

The app prepares a title from the player's own disc, BananaNative style: pick the disc, tap **Prepare**. The app
runs `tools.prepare` with an embedded CPython (identify, analyse, lift: the same Python pipeline as on a desktop),
compiles the generated C with an embedded clang + lld (LLVM 19, one multicall binary `libxbacc.so`) into a **title
pack** (`libtitle.so` + `pack.json`), and dlopens it next to the shell's `libmain.so`.

This page is the contract between the three parts. Field names, flags and line formats here are frozen: change them
only together with the app (service, loader) and the toolchain.

| Part | Owner | Code |
|---|---|---|
| `tools.prepare --android`, compile driver, pack writer | this page | `tools/prepare/{android,compile,pack,progress}.py` |
| Toolchain: CPython, `libxbacc.so` (clang, ld.lld), sysroot, builtins | embedded-prepare | assets + `jniLibs` |
| Service that runs prepare, `status.json`, pack loader | prepare-app | `android/app` |

Nothing from the game leaves the device: the pack is built from the player's disc, on the player's device.

## Where packs live

```
<files dir>/packs/<TITLEID>/libtitle.so
<files dir>/packs/<TITLEID>/pack.json
```

`<files dir>` is the app's **internal** storage (`Context.getFilesDir()`). External storage
(`/sdcard/Android/data/<app>/files`) is mounted `noexec`: dlopen of a library there fails. An external `packs/` folder
is only an import/export drop: the app copies a pack in from there after checking `lib_sha256`, never loads it in
place (planned; not implemented yet, see « The loader »). A game's own app (app-shell.md « A game's own app ») has
its own private storage: it takes XRecomp's pack over instead of preparing again
([Handing a pack to a game's own app](#handing-a-pack-to-a-games-own-app)).

`TITLEID` is the XBE's title ID in 8 upper-case hex digits (`4D5300D1`). A Game Boy Advance game's pack is
`packs/gba-<KEY>/` ([GBA packs](#gba-packs-xba-157-xba-158)).

**Atomic publish.** A pack is built in `packs/<TITLEID>.partial/`; `pack.json` is written last (temp file, fsync,
rename), then the directory is renamed to `packs/<TITLEID>`. An existing pack is first renamed to
`packs/<TITLEID>.old` and removed after the switch (POSIX cannot rename a directory over a non-empty one); the next
prepare puts `.old` back if a kill fell between the two renames. So a reader sees a whole pack or none:

- the loader only looks at `packs/<TITLEID>` (8 hex digits, no suffix) that has a `pack.json`;
- `*.partial` and `*.old` belong to prepare: the app ignores them and may delete them when no prepare runs.

## pack.json

```json
{"format": 1, "title_id": "4D5300D1", "title": "...", "xbe_sha256": "...", "abi": "arm64-v8a", "shell_abi": 1,
 "toolkit": "<commit>", "toolkit_sha256": "...", "lib": "libtitle.so", "lib_sha256": "...", "lib_size": 123456,
 "entry": "recomp_title_main", "cc": "clang 19.0.1 -O1", "built_at": "2026-11-10T21:10:35Z",
 "env_force": {"RECOMP_LSFG": "0"}}
```

| Field | Type | Meaning |
|---|---|---|
| `format` | int | 1. A loader refuses a format it does not know. |
| `title_id` | string | 8 upper-case hex digits, = the directory name. |
| `title` | string | The XBE's title name, for display. |
| `xbe_sha256` | hex | The XBE the pack was built from. The app compares it with the game folder's `default.xbe` before a start ([When a pack is stale](#when-a-pack-is-stale)). |
| `abi` | string | `arm64-v8a`. |
| `shell_abi` | int | The shell ABI the pack was linked against. The shell exports its own integer; **the loader refuses a mismatch** (the app then offers to prepare again). |
| `toolkit` | string | The toolkit commit (`git rev-parse HEAD`, or `tools/VERSION` in the assets), `unknown` if neither. Informative: the commit of the run that last relinked the pack. |
| `toolkit_sha256` | hex | The toolkit digest (`tools/prepare/toolkit.py`) the pack was lifted with (XBA-127). The app compares it with its own ([When a pack is stale](#when-a-pack-is-stale)). Packs from before it lack it. |
| `lib` | string | `libtitle.so`, relative to the pack directory. |
| `lib_sha256`, `lib_size` | hex, int | Of `lib`. The loader checks both before dlopen. |
| `entry` | string | `recomp_title_main`: `int recomp_title_main(const recomp_storage *)` (templates/new-game/src/main.c). |
| `cc` | string | Compiler and level, informative (`clang 19.0.1 -O1`). |
| `built_at` | string | ISO 8601, UTC (`Z`). |
| `env_force` | object | Optional; only when the title profile declares it. `title.json`'s `"env_force"`, checked and copied: `RECOMP_[A-Z0-9_]+` names to strings (printable ASCII), 16 at most. The loader sets each one over `recomp.cfg` before the dlopen (log `[TITLE] <TID> forces KEY=VALUE (title profile)`); the launcher shows a forced `RECOMP_LSFG=0` on the frame generation row. Not a validity check: a pack without it, or with entries the loader refuses, still loads. See [Forced by a title](../runtime/environment-variables.md#forced-by-a-title). |

| `title_layer` | string | Optional (profile-free WP6): only for a title the toolkit has optional enhancements for (`profiles/titles/<id>`). `on`: they went in; `off`: they were left out (the app's Settings › General › Enhancements was off for the title, so its Prepare had `XBA_NO_TITLE_LAYER=1`). When it differs from the switch, Home says « Enhancements changed · prepare again » and offers Prepare again next to Play, as for an older toolkit. The row is shown only for a pack that has the field. |

| `images` | object | Optional (XBA-82): the disc's other images that `default.xbe` names, prepared from the game folder (one sub-run each in `<work>/images/<path>/`, shown inside step 4 as « game.xbe: ... », at most 8), disc path to library: `{"game.xbe": "libimage-1.so"}`. An image whose analysis or compile fails is left out (« left out » line): default.xbe still runs. Shells from before ignore it. See [multi-xbe.md](../technical/multi-xbe.md#android). |

| `prepare` | object | Optional (XBA-128): what the Prepare that wrote this pack cost, measured as it ran (`tools/prepare/metrics.py`). See below. Informative: no check reads it. |

Readers ignore fields they do not know. `tools/prepare/pack.py` (`read`, `check`) is the reference for the checks.

**`prepare`** (written by the run that links the pack; a run that finds the pack up to date leaves it as it was):

```json
"prepare": {"total_s": 372.4, "steps_s": {"identify": 2.1, "analyse": 61.0, "lift": 48.2, "compile": 255.0,
            "images": 0.0, "pack": 6.1}, "skipped": ["parse", "disasm"], "jobs": 4, "opt": "2", "files": 163,
            "compiled": 163, "c_mb": 256.0, "obj_mb": 149.0, "pack_mb": 61.2, "cpus": 8,
            "peak_rss_mb": {"max": 589, "p90": 302, "median": 163}, "tools_peak_mb": 900, "held_s": 40,
            "thermal": {"samples": 74, "max": 3, "severe_s": 95, "jobs_min": 1, "temp_c_max": 41.5,
                        "curve": [[0, 1], [60, 2], [120, 3]]}}
```

| Key | Meaning |
|---|---|
| `total_s`, `steps_s` | Wall time of this run, and of each step. A resumed run counts only itself; `skipped` names the stamps it found done (`parse`, `disasm`, `func_id`, `abi`, `lift`, `link`). |
| `jobs`, `opt`, `files`, `compiled` | The compile: jobs started with, `-O`, C files, how many compiled now (the rest came from the object cache). |
| `c_mb`, `obj_mb`, `pack_mb`, `cpus` | Sizes of the C, the objects, the libraries in the pack; the CPU count. |
| `peak_rss_mb` | Each compile's own peak resident set (`wait4`'s `ru_maxrss`, one per clang): max, p90, median. Absent where the host has no `wait4`. |
| `tools_peak_mb` | The largest pipeline tool (disasm, lift: `RUSAGE_CHILDREN` before the compile). |
| `held_s` | Wall time the device log held every compile (`jobs` 0: critical heat, or a background Prepare while the game ran). |
| `thermal` | From the device log (below): samples, the hottest status, seconds at severe or above, the fewest jobs allowed, the battery's hottest temperature, up to 24 points of the curve `[seconds into the run, status]`. Absent without a device log (a host). |

The launcher shows it in Manage (« Preparation took 6 min 12 s · 4 jobs · -O2 », « Memory per compile job », « Hottest
during it »; `PrepareFacts`), and the library check adds it to each title's report line (`prepare_s` when the check did
not time a Prepare itself, `prepare_peak_mb`, `pack_mb`, `prepare_jobs`, `prepare_opt`, `prepare_thermal_max`,
`prepare_temp_c_max`, `prepare_steps_s`; `peak_mem_mb` stays the game process's own peak).

### What a pack exports

`libtitle.so` is linked with `-Bsymbolic` (the title's calls to itself bind inside it) and `--no-undefined` against
`libmain.so`, so every symbol it uses is exported by the shell or by libc/libm/libdl/liblog. Its dynamic symbols
include `recomp_title_main` and the symbols the shell defines as weak forwarders (`recomp_lookup`,
`recomp_lookup_manual`, and per title `g_fe_cursor`, `fe_menu_get_pb_state`). The toolkit inside `libmain.so` calls
`recomp_lookup` (thread start, APCs); a library dlopened later does not override `libmain.so`'s own weak definition,
so the loader fetches these with `dlsym` and the forwarders call through them (« The loader » below).

## The command

```
python -m tools.prepare SOURCE --android --work DIR --pack-dir DIR --cc PATH [--cc-prefix-arg clang]
       --sysroot DIR --shell-lib PATH [--jobs N] [--opt 2] [--shell-abi N]
       [--ld PATH [--ld-prefix-arg ARG]] [--builtins PATH] [--python PATH]
       [--extra-src FILE.c]... [--include-dir DIR]... [--device-log FILE] [--force]
```

| Flag | Device value |
|---|---|
| `SOURCE` | The game folder (holds `default.xbe`; the app's ISO import extracted it), or a `default.xbe`, or an image (`.iso`/`.xiso`: only `default.xbe` is read out). |
| `--work DIR` | Per title, internal storage, e.g. `<files dir>/prepare/<TITLEID>`. Holds the copy of the XBE, the analysis, the C project, the object cache, `stamp.json`, `prepare.log`, and `discovered.jsonl`, which the title's runs write ([discovery.md](../technical/discovery.md)). Keep it between runs: it is what makes a rerun cheap. Several hundred MB for a large title (the C alone is ~250 MB for one). |
| `--pack-dir DIR` | `<files dir>/packs` |
| `--cc PATH` | `<nativeLibraryDir>/libxbacc.so` |
| `--cc-prefix-arg clang` | The multicall applet. With a prefix arg, the linker is `PATH ld.lld` unless `--ld` says otherwise. On a host: the NDK's `clang` and no prefix (ld.lld is taken next to it). |
| `--sysroot DIR` | The NDK-layout sysroot from the assets (`usr/include`, `usr/lib/aarch64-linux-android/30/{crtbegin_so.o,crtend_so.o,libc.so,...}`), plus `lib/clang/19/`: the embedded clang's resource directory (headers, builtins, libunwind), which it cannot find by itself; `-resource-dir` then points there ([toolchain.md](toolchain.md#the-on-device-toolchain-embedded-prepare)). |
| `--builtins PATH` | `libclang_rt.builtins-aarch64-android.a`; default: from `<sysroot>/lib/clang/<v>/lib/linux/` when the sysroot carries it, else asked from the compiler (`-print-libgcc-file-name`). |
| `--shell-lib PATH` | `<nativeLibraryDir>/libmain.so`: the pack links against it (DT_NEEDED `libmain.so`). |
| `--shell-abi N` | The integer the shell exports (default 1), written to `pack.json`. |
| `--jobs N` | Parallel compiles; 0 or absent = `min(5, (MemAvailable − 3 GB) / 1 GB)`, at least 1, at most the CPU count (1 GB per job: see Measurements). |
| `--opt N` | clang `-O` level, default 2 (see Measurements). |
| `--python PATH` | The interpreter for the pipeline steps; default `$XBA_PYTHON`, else `sys.executable`. |
| `--extra-src`, `--include-dir` | Development: an extra C file in the pack, an extra `-I`. |
| `--device-log FILE` | `<work>/device.jsonl`, the app's device log ([below](#the-device-log-xba-128)): compiles allowed now, thermal samples. Absent: `--jobs` all the way. |
| `--force` | Redo every step (the object cache still applies). |

The steps, each one subprocess (`python -m tools.<tool>`) or a pool of compiler processes, never the
`multiprocessing` module (no `sem_open` on Android):

| # | id | Does | Stamp |
|---|---|---|---|
| 1 | `identify` | finds `default.xbe`, copies it to `<work>/xbe/`, `xbe_parser` → title ID, title profile; `xbe_ident` → XDK version, engines, profile layers in `<work>/ident.json` (shown in `msg`; never fails the step) | `parse` |
| 2 | `analyse` | `disasm`, `func_id`, `abi_analysis` | `disasm`, `func_id`, `abi` |
| 3 | `lift` | the project from `templates/new-game` + the profile layers (`profiles/`, [profiles.md](../technical/profiles.md)), then `tools.recomp` | `lift` (= profile digest) |
| 4 | `compile` | every C file to an object, largest first, cached | the object cache |
| 5 | `pack` | `ld.lld` → `packs/<TID>.partial/libtitle.so`, `pack.json`, publish | `link` |

Stamps live in `<work>/stamp.json` with the XBE's sha256 and the toolkit digest (`toolkit_sha256`, not the commit:
an app update that changed only Java or the shell keeps the steps done; XBA-127): another XBE or another toolkit
starts over; a redone step drops the stamps after it. Objects are written to a temp name and renamed. So a run killed
at any point (SIGKILL included) loses at most the sub-step or the objects in flight.

### Progress lines

On stdout, one line per event (everything else, the tools' own output included, goes to `<work>/prepare.log`):

```
XBA_PROGRESS step=<i>/<n> id=<identify|analyse|lift|compile|pack> pct=<0-100> [eta=<s>] msg=<rest of line>
XBA_RESULT ok pack=<dir>
XBA_RESULT error code=<UPPER_SNAKE> msg=<text>
```

- `msg` is last and greedy: it takes the rest of the line, spaces and `=` included. Other values hold no spaces.
- Unknown keys are ignored (new ones may appear before `msg`). `eta` (seconds) is optional.
- `pct` is per step. For `analyse` and `lift` it is a time-based guess in v1 (the tools print no counts): it moves
  towards the end of the sub-step and jumps when it ends. For `compile` it is files done / total; `eta` comes from the
  bytes compiled so far.
- The last line is exactly one `XBA_RESULT`. `pack=` is the published directory.
- `tools/prepare/progress.py` has the writer and a reference reader (`parse_line`).

Example (the conformance stub):

```
XBA_PROGRESS step=1/5 id=identify pct=100 msg=SYNTH (title ID 12345678), XDK 5849, no known engine
XBA_PROGRESS step=2/5 id=analyse pct=55 msg=disasm done in 0 s
XBA_PROGRESS step=4/5 id=compile pct=40 eta=12 msg=2/5 files (2 compiled)
XBA_PROGRESS step=5/5 id=pack pct=100 msg=pack 12345678, 19272 bytes
XBA_RESULT ok pack=/data/user/0/<app>/files/packs/12345678
```

| Error code | When |
|---|---|
| `BAD_ARGS` | a required flag is missing (`--work`, `--pack-dir`, `--cc`, `--sysroot`, `--shell-lib`) |
| `NO_XBE` | no `default.xbe` in the source |
| `NO_ROM`, `BAD_ROM` | a GBA ROM that cannot be read, or is empty or over 32 MB ([GBA packs](#gba-packs-xba-157-xba-158)) |
| `BAD_XBE` | `xbe_parser` cannot read it |
| `ANALYSE_FAILED`, `LIFT_FAILED` | a pipeline tool failed; details in `prepare.log` |
| `COMPILE_FAILED` | a C file did not compile; `msg` = file and first error |
| `LINK_FAILED` | ld.lld failed; `msg` = the first undefined symbols, if that is why |
| `PACK_FAILED` | writing or publishing the pack failed |
| `NO_SPACE` | ENOSPC anywhere |
| `CANCELLED` | SIGTERM or SIGINT |
| `BAD_PROFILE` | the title profile's `title.json` has an `env_force` entry that is not allowed |
| `INTERNAL` | anything else (a bug); the traceback is in `prepare.log` |

Exit status: 0 ok, 1 error, 130 cancelled. On SIGTERM/SIGINT prepare kills its children, prints
`XBA_RESULT error code=CANCELLED`, and exits 130; stamps and finished objects stay, the published pack is untouched.
The app should still start prepare in its own process group and signal the group, so that a SIGKILL leaves nothing
behind either.

## Compile and link

The same driver runs on the device and on hosts/CI (with the NDK's clang): no CMake. Per file:

```
<cc> [clang] --target=aarch64-linux-android30 --sysroot=<sysroot> -O<opt> -g0 -fPIC -std=gnu11 -w
     -DXBOXRECOMP_SHELL_TITLE=1 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DNDEBUG [-DXBA_TITLE_PROFILE=1]
     -I<project>/src -I<project>/src/recomp -I<project>/src/recomp/gen
     -I<toolkit>/include -I<toolkit>/src -I<toolkit>/src/{kernel,platform,audio,apu,nv2a,d3d,input,usb,video,selftest}
     -c <file.c> -o <work>/obj/<key>.o
```

`WIN32_LEAN_AND_MEAN`, `NOMINMAX` and `NDEBUG` are what the CMake build gives the same files (the template's
definitions, a Release-type build). Sources: `src/main.c`, `src/recomp_manual.c`, `src/title/*.c` (the profile;
`XBA_TITLE_PROFILE` when there is one), `src/extra/*.c`, `src/recomp/gen/*.c`.

**Object cache.** `key = sha256(source path, source bytes, flags, compiler id, headers digest)`; compiler id = the
`--version` line + the binary's size and mtime; headers digest = every `.h` under the include directories. A second
run with nothing changed compiles 0 files; a new lift rewrites `recomp_funcs.h` and so recompiles everything. After a
successful link, objects not used by it are deleted (storage).

**Link**, ld.lld directly, what the NDK's `clang -shared` does plus our options:

```
ld.lld -EL --fix-cortex-a53-843419 -z now -z relro -z max-page-size=16384 -z noexecstack --hash-style=gnu
       --eh-frame-hdr --build-id=sha1 -m aarch64linux -shared -soname libtitle.so -Bsymbolic --no-undefined
       -o libtitle.so <sysroot>/usr/lib/aarch64-linux-android/30/crtbegin_so.o <objects>
       -L<api libdir> -L<libdir> -L<shell dir> -l:libmain.so -lm -ldl -llog -lc <builtins> [libunwind.a] -ldl
       <sysroot>/usr/lib/aarch64-linux-android/30/crtend_so.o
```

`max-page-size=16384`: 16 KB-page devices (Android 15+). `--no-undefined` turns a symbol the shell does not export
into a link error on the device rather than a dlopen failure at start.

## The device log (XBA-128)

While a Prepare runs, `PrepareMonitor` (in the `:prepare` process) appends a line to `<work>/device.jsonl` every 5 s
(the file is new for each Prepare):

```
{"t": 1731456000, "thermal": 3, "jobs": 1, "temp_c": 41.5}
```

`t` epoch seconds, `thermal` PowerManager's thermal status (0 none … 6 shutdown), `jobs` the compiles allowed from now
on, `temp_c` the battery's temperature, `game: true` when a background Prepare holds for the game. tools.prepare reads
the last whole line before each compile starts (`metrics.jobs_now`, never above `--jobs`); running compiles are never
killed. `DeviceGuard` (shared with the library check, XBA-130) decides:

| Thermal status | Compiles at once |
|---|---|
| none, light, moderate (0–2) | all the jobs (`--jobs`) |
| severe (3) | 1 |
| critical and above (4–6) | 0: held, the running ones finish, none starts until it cools down |
| any, a background Prepare while the game runs | 0 |

So a hot device keeps going slowly instead of being stopped. The library check no longer cancels a Prepare when the
device is hot: it pauses (nothing new starts after it) and lets the Prepare drop its own jobs.

## When the Prepare's process dies (XBA-128)

What a kill costs, step by step: identify, analyse (`disasm`, `func_id`, `abi`) and link have their stamps, so a
finished sub-step is never redone; the compile keeps every finished object (cache, temp name + rename), so only the
files in flight are lost; **the lift is one step**: a kill during it redoes the whole lift (it writes all the C at
once; on the real title of « Measurements » the lift is under a minute of a 10–30 min Prepare).

The lift stays one step on purpose (XBA-138). `tools.recomp` translates every function in memory first and writes
the C only at the end (`translator.py`: `recomp_funcs.h`, then the chunks, through `write_if_changed`), so a kill
almost always lands before the first file and a per-file resume would save only the write phase, seconds of the
lift's ~50 s (48.2 s of 372 s in the `pack.json` example above: 13 % of the Prepare, at most one lift lost per kill).
Resuming the translation itself would mean checkpointing the translator's whole-program state (labels, indirect-call
tables, the header that lists every function), for less than a minute. And nothing is lost downstream: a redone lift
writes the same bytes, and the object cache is keyed by content, so the compile that follows finds every object it
had. What the lift does guarantee: its stamp is dropped before it empties `gen/` and written only after
`tools.recomp` succeeds, so a half-written `gen/` is never taken as a finished lift, even when a later run comes back
to the stamp's earlier key (the « Enhancements » switch turned off, then on again;
`test_android_killed_lift_never_passes_as_up_to_date`). The desktop Prepare lifts into `gen.new` and syncs, same
guarantee.

The app's side:

- The service returns `START_REDELIVER_INTENT`: when the system kills `:prepare` (low memory, the phantom process
  killer), Android starts it again with the same intent, and the Prepare goes on from its stamps. `status.json` counts
  `resumes`; the log of the killed attempt is kept and appended to (« resumed after the Prepare's process died »).
  After `MAX_RESUMES` (3) the title is failed with `KILLED`. A redelivered start of a Prepare that is not `running`
  any more does nothing. If the system does not let it go to the foreground then, it stops, and the launcher shows the
  Prepare as interrupted (« Resume », as before).
- tools.prepare runs under `setsid`, out of the app's process group: a kill of the app's process can leave it and its
  compilers running. Its group id is written to `<files>/prepare/<TID>/pgid` when it starts; the next Prepare of the
  title ends that group before anything else (`PrepareRunner.killOrphans`).
- A killed compiler (not the whole process) is the runner's retry with half the jobs, as before.

## The quick first build (XBA-128): a switch, off

With `<files>/prepare/quick-first` present (`adb shell run-as fr.adserver.xrecomp.shell touch
files/prepare/quick-first`), a Prepare that is not a background one builds at `-O1`; when its pack is published, an
`-O2` rebuild of the same title is queued **in the background** (Play is not blocked; it holds while the game runs).
Off by default: on the x86 host, `-O1` saves 4–6 % of the compile and costs a second full compile afterwards (see
« Measurements »). To be kept only if the device's numbers say otherwise.

## What the app provides

**Environment** of the prepare process (set by the service):

| Variable | Value |
|---|---|
| `PYTHONHOME` | the CPython stdlib in the assets (extracted to internal storage) |
| `PYTHONPYCACHEPREFIX` | a writable cache dir (`<cache dir>/pycache`): the assets are read-only |
| `TMPDIR` | `<cache dir>/tmp` (clang and Python temp files) |
| `HOME` | `<files dir>` |
| `LIBCAPSTONE_PATH` | the directory holding `libcapstone.so` (the capstone binding loads it from there) |
| `XBA_PYTHON` | optional: the interpreter for the step subprocesses, if `sys.executable` is not one |

The working directory does not matter; `python -m tools.prepare` needs the toolkit root on `sys.path`
(`PYTHONPATH=<toolkit root>`).

**Toolkit tree** in the assets (read-only): `tools/` (Python, plus the data files the tools read: `tools/recomp/data/`, `tools/xbe_ident/engines.json`), `include/`, the headers of `src/` (`src/**/*.h`),
`templates/new-game/`, `templates/runtime/` (the `recomp_types.h` the lifter copies next to the generated C), `profiles/`, `tools/VERSION` (one line: the commit; written by the asset build, git-ignored), and `tools/TOOLKIT_SHA256` (the toolkit digest, written by the asset build). The patterns are in
`tools/prepare/toolkit_tree.json`, read by both `android/app/build.gradle` (the zip) and `tools/prepare/toolkit.py`
(the digest on a host).

**status.json**: the service writes it from the progress lines for the UI and for a restart after the app was killed.
Its format belongs to the app (prepare-app branch, `android/app`), not to this page; prepare never reads it.
One Prepare runs at a time: a start for another title while one runs waits in `files/prepare/queue.json` (that
title's status says `queued`) and starts when the running one ends, whatever its outcome (`PrepareService`).

## When a pack is stale

A valid pack (the checks above) can still be the wrong one (XBA-127). The launcher (`PackFreshness`) tells two cases
apart:

| Case | How it is seen | What the player gets |
|---|---|---|
| **Another XBE**: the folder now holds another revision or region of the disc under the same title ID | `xbe_sha256` ≠ SHA-256 of the game folder's `default.xbe` | The pack is **never run**. Home: « Another version of the game · prepare it again », the primary is **Prepare again**, a note says saves are kept. « Open the game directly » does not start it either. |
| **Older toolkit**: the app was updated and its toolkit changed since the pack was lifted | `toolkit_sha256` ≠ the APK's digest (`assets/prepare/toolkit.zip.id`), or no `toolkit_sha256` at all | The pack **still plays** (its `shell_abi` matches, or it would not be valid). Home: « Prepared by an older version of the app », **Play** stays the primary and a quiet **Prepare again** sits next to it, with a note (about 20–30 min, saves kept). Never queued by itself: a Prepare blocks Play while it runs. |

When both hold, another XBE wins. An unknown side is not a mismatch: a pack.json without `xbe_sha256` (or one that is not
64 hex digits), an APK without the toolchain or without `toolkit.zip.id`. Saves are never touched in either case
(they live in `files/save/`, the Prepare writes only `work/` and `packs/`); the old pack stays until the new one replaces
it (atomic publish).

**The XBE hash.** `default.xbe` only (a few MB; game data is never hashed), once per change of the file: the hash is
kept for the process by path, size and mtime. Home hashes it on a worker thread and redraws when it does not match;
Play and « Open the game directly » hash it on the spot when Home has not yet. The record's `xbe_sha256` (taken at
import) is the fallback before that. The native loader does not compare it.

**What « the toolkit changed » means.** Not the commit: a new APK whose commit changed only Java, the docs, the tests
or the shell's `.c` files would otherwise send every player back to a 10–30 min Prepare for nothing. The toolkit digest
is SHA-256 over the **toolkit tree** (above: the pipeline's Python, `tools/recomp/data`, the profiles, the title
template, `include/` and the headers of `src/`), each file as `path NUL size NUL bytes` in path order; this is what
ends up in a pack's bytes besides the player's XBE. What the shell's `.c` files change reaches packs without a Prepare
(they call into `libmain.so`), and a change of that contract bumps `shell_abi`, which makes the pack invalid instead.
The tree also holds Python that is not on the lift path (`tools/compat`, `tools/diff`, …): a change there asks for a
Prepare that was not strictly needed; acceptable, and the tree stays one simple rule. The compiler (`libxbacc.so`, the
sysroot) is not in it: a new clang does not make an old pack wrong.

`android/app/build.gradle` computes the digest when it zips the toolkit and writes it into the zip
(`tools/TOOLKIT_SHA256`) and beside it (`prepare/toolkit.zip.id`, which the launcher reads without opening the zip).
`tools.prepare` reads `tools/TOOLKIT_SHA256` when there is one (the device), else computes the same digest from the
checkout (`tools/prepare/toolkit.py`; `pytest tools/prepare/test_toolkit.py` checks the selection), writes it into
`pack.json` and keys its stamps on it.

## Handing a pack to a game's own app (XBA-176)

A game's own app (Manage › Create an app, [app-shell.md](app-shell.md#a-games-own-app)) is XRecomp with one game and
its own private storage. Instead of preparing its game again (20–30 min), it takes XRecomp's pack over in seconds.
Code: `PackHandover` (both sides), `PackHandoverActivity` and `PackProvider` (XRecomp's side), `LauncherActivity`
(the game's app's Home). Tests: `PackHandoverTest`, `LauncherPackHandoverTest`.

**When the game's app asks.** At its launcher's start, once per process, when it has no valid pack for its title
(manifest meta-data `TITLE_ID`) or a valid one lifted by another toolkit than its own (`toolkit_sha256` against
`prepare/toolkit.zip.id`, [When a pack is stale](#when-a-pack-is-stale)); not while its own Prepare runs or waits,
nor while its game runs. On its way into the game it also asks when its game's files are not usable yet: XRecomp
then says where they are.
A stale pack of its own that XRecomp did not replace: it asks again only after XRecomp was updated (its
`lastUpdateTime`, kept in the `handover` preferences). « Open the game directly » or a shortcut that starts a
playable game skips the launcher, and so the question, until the launcher shows.

**The exchange.**

1. The game's app starts `<PARENT>/fr.adserver.xrecomp.PackHandoverActivity` (meta-data `PARENT`, XRecomp's
   package; component class names stay `fr.adserver.xrecomp.*` in a game's app) with `startActivityForResult`,
   extra `fr.adserver.xrecomp.extra.HANDOVER_TITLE_ID` = its title ID. Its manifest's `<queries>` names XRecomp's
   package (`TitleApk` leaves `<package>` as it is, so in the copy it names the parent).
2. XRecomp's activity (translucent, no view, `noHistory`, `excludeFromRecents`, in `:launcher`) checks the caller and
   finishes at once with its answer: the player sees nothing of XRecomp.
3. Accepted: `RESULT_OK`, extras `HANDOVER_FILES` (file names, `pack.json` first, then `lib`, then the `images`),
   `HANDOVER_SIZES`, one URI per file in the `ClipData` (`content://<XRecomp's package>.packs/<TITLEID>/<file>`) and
   `FLAG_GRANT_READ_URI_PERMISSION`. Refused: `RESULT_CANCELED` with `HANDOVER_REFUSAL` = the reason. To a caller
   that passed the checks (accepted, or refused with `NO_PACK` only), also where XRecomp reads the game from:
   `HANDOVER_GAME_KIND` (`FOLDER`, `IMAGE`, `NOT_SHARED`) and `HANDOVER_GAME_PATH` (an absolute path; none for
   `NOT_SHARED`: XRecomp's imported copy, an image reached by document grant, a path in XRecomp's own storage), so
   the game's app reads the same files in place ([app-shell.md « Straight into the game »](app-shell.md#straight-into-the-game)).
4. The game's app copies (below) on a worker thread, its progress on its one screen (« Copying the prepared game from
   XRecomp… 42 % »), then its game starts. Opened through the « Launcher » shortcut, Home shows the copy instead
   (a dimmed « Copying… »; Play and Prepare wait), then Play.

**Security model.** XRecomp hands its pack only to a caller that Android names and that it can check:

| Check | How | Refusal |
|---|---|---|
| XRecomp is not itself a game's app | its own meta-data has no `TITLE_ID` | `NOT_A_PARENT` |
| The caller is known | `getCallingPackage()`: set only for `startActivityForResult`; `adb shell am start` or a plain `startActivity` gives none | `NO_CALLER` |
| The title ID is one | 1–8 hex digits | `BAD_TITLE` |
| The caller is that title's game app | package = `fr.adserver.xrecomp.t<titleid>` (lower case) for the title ID it asks for | `WRONG_PACKAGE` |
| XRecomp ever made a game app | the AndroidKeyStore holds `xba_title_apps` (`TitleApp.existingCert`, which never makes it) | `NO_KEY` |
| The caller is signed by that key | `GET_SIGNING_CERTIFICATES`: one signer, its certificate byte-for-byte that key's certificate | `WRONG_CERT` |
| There is a whole pack | `Pack.check` = `VALID`, every file `pack.json` names present | `NO_PACK` |

Only the game apps XRecomp signed itself pass (the key never leaves its keystore), each for its own title. The pack's
files are read through `PackProvider` (`${applicationId}.packs`), **not exported**, `grantUriPermissions`: only the URIs
the activity granted, one per file, are readable, read-only, by that caller, while its activity lives. The provider
serves only `<TITLEID>/<name>` where the title ID is 8 upper-case hex digits and the name is `pack.json` or a file it
names (`lib`, `images`), inside that pack directory. Its authority is package-derived, so `TitleApk` moves it to
`fr.adserver.xrecomp.t<tid>.packs` in a game's app: no clash with XRecomp's (and a game's app serves nothing, as
it refuses with `NOT_A_PARENT`). The game's app trusts the package its meta-data names as its parent: it was made by
it and installs only next to it.

**The copy** (`PackHandover.take`, the checks of `Pack.check` and `tools/prepare/pack.py`):

1. `pack.json` read (1 MB at most) and checked: `format` 1, `title_id` the app's, `abi` `arm64-v8a`, `shell_abi` the
   app's, `lib` a plain name that was offered, `lib_size` > 0, `lib_sha256` a SHA-256, each `images` name offered.
   Else **UNFIT** (another version of the app made it). `xbe_sha256` against the disc the app is locked to (meta-data
   `XBE_SHA256`): else **OTHER_XBE**.
2. Freshness: a valid pack of the app's own is replaced only by one lifted with the app's own toolkit, and only when
   its own is not (else **NOT_NEWER**: kept, nothing copied). With no pack of its own, the app takes XRecomp's even
   from another toolkit (same `shell_abi`: it loads and plays), and Home offers Prepare again with the usual
   « Prepared by an older version of the app » note — better than 20–30 min before the first play.
3. Free space: the files' sizes plus 32 MB on internal storage, else **NO_SPACE** (the note gives the size).
4. Copy into `packs/<TITLEID>.partial/` (only what `pack.json` names: `lib`, then the `images`), each file synced; the
   library hashed while it is copied: its size must be `lib_size` and its SHA-256 `lib_sha256`, an image's size the
   offered one, else **DAMAGED** and the `.partial` is removed. `pack.json` written last (`pack.json.tmp`, sync,
   rename).
5. Publish as `pack.py` does: a leftover `.old` put back or removed first (`recover`), an existing pack renamed to
   `<TITLEID>.old`, `.partial` renamed to `<TITLEID>`, `.old` removed; `Pack.check` must then say `VALID`.
6. The pack's `title_layer` (`on` / `off`) becomes the app's Enhancements switch for the title (XRecomp's choice comes
   with its pack; else Home would offer to prepare again at once).

**Falling back.** Nothing is lost: when the game's app gets no pack, Home is the usual « Needs preparing » with
Prepare, and a note says why — XRecomp is not installed, has not prepared this game, refused (the reason), its pack
does not fit this version of the app, comes from another disc, there is not enough space, or the copy failed (open the
app again to retry). A game's app with a valid pack of its own says nothing and keeps playing it.

logcat (`xboxrecomp`): in the game's app `launcher: pack handover: asking <parent> for <TID> (no pack here)`, `…
offers [pack.json, libtitle.so] , <n> bytes`, `launcher: pack <TID> published from the handover`, `launcher: pack
handover <TID>: COPIED`, or `… refused: <REASON>` / `… is not installed` / `…: UNFIT (shell_abi 2, this app is 1)`; in
XRecomp `launcher: pack <TID> handed over to <package>` or `launcher: pack handover refused to <package> (<TID>):
<REASON>`.

## Self-healing (XBA-74, XBA-129)

A run writes what the static analysis missed to `<work>/discovered.jsonl`
([discovery.md](../technical/discovery.md)) and the next Prepare seeds the analysis with it. The app closes the loop
by itself, with no question asked (`SelfHeal.java`, state in `<work>/selfheal.json`):

- PrepareService records the file's length when a Prepare begins (`offset`): what the runs add after it is new.
- The launcher records each launch (`launched_at`). Back on Home with no game process, it reads the system's record
  of how the game's process last ended (`ActivityManager.getHistoricalProcessExitReasons`, the main process only).
  An exit before the launch, or already looked at (`handled`), changes nothing.
- **A crash** (`REASON_CRASH`, `REASON_CRASH_NATIVE`, `REASON_ANR`) with new `icall`, `thread` or `thread_ctx`
  entries: a round at once, the Preparing screen with a note (« The game crashed on code the preparation had missed:
  preparing it again with it (round N), then the game starts by itself. B stops it; Settings › General ›
  Self-healing turns this off. »), then the game starts again when the pack is ready.
- **A run that ended well** with such entries (a skipped indirect call does not crash: the title goes on, maybe
  wrong; the runtime says the first one in its log and counts them in the status line's `seeds`): a round **in the
  background**. The Prepare runs with `background: true` in `status.json` (and in the queue): Home keeps **Play**
  as the primary (the valid pack plays until the new one replaces it, atomic publish), its line says « Improving in
  the background · Compile · 68 % », a note explains, and a quiet **Stop** cancels it. Nothing starts by itself
  afterwards; the next start uses the new pack. PrepareService ends a background round, the launcher the others.
- **What a Prepare cannot fix**: a crash with a guest exception nothing handled (`exception`), a guest stack switch
  the runtime could not carry (`stack_switch`) or an untranslated instruction (`unimpl`), named in that order, even
  when the run also found new code; or a crash with nothing new (« crash (…), nothing new found », with « ; calls stub
  ordinal N (Name) » when the run called stub kernel routes). Home says « Crashed · needs a fix », the blocker in the
  note. No round.
- **The cap is on rounds without progress**, not on rounds. A round's progress is `discovered-seeds.json` growing
  (the seeds tools.prepare took through seed_from_log's gates). A crash round whose Prepare took nothing stops the
  loop at once (its pack is the same: no relaunch; « needs a fix », « the Prepare could not use what the run found
  (N addresses, none of them code) »). `MAX_IDLE_ROUNDS` (2) such rounds in a row and new finds no longer start a
  round by themselves until a run ends well with nothing new. On the device a crash is known only by the system's
  words for it, so « the same blocker twice » is a crash that brought nothing new: the discovery file never writes
  an address twice, a repeat of the same miss adds nothing. 12 rounds in a row is a safety net only.
- **The player sees and stops it**: the notes, Stop (background) or B / Cancel (Preparing screen), and **Settings ›
  General › Self-healing** (on by default, only with the toolchain). Off: a crash with new code offers **Prepare
  again** as the primary (the button of XBA-74); a good run with new code offers a quiet **Prepare again** next to
  Play (a background round). A stopped round starts nothing.
- A crash round whose Prepare ended while the launcher was away is counted when Home next shows (and relaunches the
  game if it took new code in).

The same rules on a desktop: `python -m tools.compat heal` ([compat-runner.md](../technical/compat-runner.md#self-healing-rounds-and-recall-xba-129)),
which `tools/compat/test_e2e.py::test_the_self_healing_loop` runs on a synthetic title (three rounds with progress,
the fourth without: stop). Tests on the app side: `SelfHealTest`, `LauncherSelfHealTest` (screenshots
`prepare-11` … `prepare-15`).

## Measurements

One real title's generated C (Alex's own disc, kept outside the repository; numbers only): 163 files, 256 MB of C
(`--split 250`), the largest 5.1 MB. NDK r28c clang 19 for `aarch64-linux-android30`, `-g0`, the flags above;
3 jobs in a container limited to 3 CPUs on the x86-64 host (Ryzen AI Max+ 395, Zen 5), 2026-11-10.

| | -O1 | -O2 |
|---|---|---|
| Wall time, 3 jobs | 240 s | 254 s |
| CPU time, all files (sum of per-file wall) | 717 s | 759 s |
| Per file: median / largest | 3.2 s / 23.5 s | 3.6 s / 23.2 s |
| Peak RSS per job: median / p90 / max | 163 / 304 / 597 MB | 163 / 302 / 589 MB |
| Objects, total | 150.5 MB | 149.0 MB |

**`--opt`: default 2.** -O2 costs 6 % more compile time than -O1 and the same memory, for the code the game then
runs all the time. -O1 stays an option for a low-end device.

**-O1 then -O2 (XBA-128), measured again through the Prepare's own driver** (`compile_all` with its new per-job
`wait4` peak RSS), on the generated C of a synthetic title of our own (40,000 small functions in a chain: 164 files,
77.4 MB of C, the largest 2.0 MB), NDK r28c clang 19, 3 jobs in a container limited to 3 CPUs on the same host,
2026-11-13:

| | -O1 | -O2 |
|---|---|---|
| Wall time, 3 jobs | 43.1 s | 44.8 s |
| Peak RSS per job: median / max | 86 / 121 MB | 86 / 120 MB |
| Objects | 24.8 MB | 24.8 MB |

Same story as the real title: -O1 is 4 % (synthetic) to 6 % (real) faster to first play and needs the same memory,
and the background -O2 rebuild then costs a whole compile again (time, heat, battery). Not the default; the switch
above is there to measure it on the Thor.

**Jobs: 1 GB per job instead of 1.5.** The worst file peaks at 0.6 GB, 90 % of them under 0.3 GB, and the files are
compiled largest first, so the big ones never run all at once. `min(5, (MemAvailable − 3 GB) / 1 GB)` gives a
12 GB device with ~7 GB available 4 jobs (1.5 GB would give 2). The 3 GB reserve stays: the app's UI, the system,
and room for a title whose largest file is twice this one.

**On a Thor (Snapdragon 8 Gen 2: 1× X3 3.2 GHz, 4× A715/A710 2.8 GHz, 3× A510), a rough estimate.** Per thread,
clang runs at about 0.6× a Zen 5 core on the X3 and 0.4× on an A715/A710 (published SPECint-class ratios; clang is
branchy integer code). Five jobs land on the X3 and the four middle cores: ~2.2 Zen 5 cores of throughput, so 760 s
of CPU at -O2 is ~6 min, plus 20–30 % for sustained clocks under thermal limits: **7–8 min** for the compile with
the NDK's clang. The embedded clang is a plain build of LLVM 19 (the NDK's is PGO + BOLT + LTO, ~20–30 % faster):
count **8–10 min**. With 2 jobs (the 1.5 GB formula on a 12 GB device) it would be 15–20 min. The largest file alone
takes ~1 min on a middle core; it starts first. To be measured on the device (« Device » check list).

## GBA packs (XBA-157, XBA-158)

A Game Boy Advance game plays in the interpreter with no pack (consoles/README.md); a pack makes it run natively. The
same command, the same compile driver and the same publish build it from the player's ROM: `tools/prepare/gba.py`,
taken whenever SOURCE is a `.gba` (or any file with a GBA cartridge header). Contract of the lifted code:
`consoles/gba/runtime/gba_recomp.h`.

```
<files dir>/packs/gba-<KEY>/libtitle.so
<files dir>/packs/gba-<KEY>/pack.json
<files dir>/work/gba-<KEY>/                  --work: rom.gba, gen/, obj/, stamp.json, prepare.log, discovered.txt
```

`KEY` is the title key (`recomp_pack_rom_identify`, `gba_runtime_key`, `Console.identify`): game code + CRC32 of the
whole ROM (`AXVE-1A2B3C4D`), so two dumps of one game have two packs. The app's Prepare goes by the library key
`gba-<KEY>`: `status.json` in `<files>/prepare/gba-<KEY>/`, the queue, `Pack.check`.

**The steps** (same progress lines and ids, `n` = 5):

| # | id | Does | Stamp |
|---|---|---|---|
| 1 | `identify` | reads the ROM (32 MB at most: `BAD_ROM`; unreadable: `NO_ROM`), title key, SHA-256, header title; copies it to `<work>/rom.gba` | `rom` + `toolkit_sha256` (another ROM or toolkit starts over) |
| 2 | `analyse` | the addresses earlier runs had to interpret: `<work>/discovered.txt` (plus `--discovered FILE`), merged into `<work>/discovered-seeds.txt` | (their digest, in the lift's key) |
| 3 | `lift` | `python -m gba_tools.recomp <work>/rom.gba <work>/gen.new [--discovered seeds]`, then `gen.new` → `gen` (the stamp is dropped first: a half-written lift never passes) | `lift` |
| 4 | `compile` | each `gen/*.c` with the flags below, the object cache of « Compile and link » | the object cache |
| 5 | `pack` | `libtitle.so`, `pack.json`, atomic publish as `packs/gba-<KEY>` (`.partial`, `.old`, same rules) | `link` |

Compile flags: `--target=aarch64-linux-android30 --sysroot=<sysroot> [-resource-dir=…] -O<opt> -g0 -fPIC -std=gnu11 -w
-DNDEBUG -I<work>/gen -I<toolkit>/consoles/gba/runtime`. The link is the Xbox pack's (`ld.lld … -shared -soname
libtitle.so -Bsymbolic --no-undefined … -l:libmain.so`): every core function the lifted code calls must be exported by
`libmain.so`, which links the whole GBA core (`--whole-archive gba_core`, android/app/src/main/cpp/CMakeLists.txt).

The toolkit tree ships what the lift and the compile need: `consoles/gba/tools/**/*.py` (the lifter, `gba_tools`; on
`PYTHONPATH` for step 3) and `consoles/gba/runtime/*.h` (`gba_recomp.h`, `gba_core.h`, the lifted code's own header);
both count in the toolkit digest. `XBA_GBA_RECOMP` (a module name) and `XBA_GBA_TOOLS_DIR` replace the lifter, for the
tests (`tools/prepare/tests/fake_gba_recomp.py`, a table with the right ABI and `rom_sha256`).

**pack.json** of a GBA pack (format 1; the same reader rules: unknown fields ignored):

```json
{"format": 1, "console": "gba", "title_key": "AXVE-1A2B3C4D", "title": "POCKET QUEST", "rom_sha256": "...",
 "abi": "arm64-v8a", "shell_abi": 1, "toolkit": "<commit>", "toolkit_sha256": "...", "lib": "libtitle.so",
 "lib_sha256": "...", "lib_size": 123456, "entry": "gba_recomp_title_table", "cc": "clang 19.0.1 -O2",
 "built_at": "2026-11-14T21:10:35Z", "discovered": 12, "prepare": {...}}
```

| Field | Meaning |
|---|---|
| `console`, `title_key` | `gba` and the title key, in place of `title_id` (XBA-147). |
| `title` | The header's 12-character title (else the file name), for display. |
| `rom_sha256` | The ROM it was built from. The runtime compares it with the ROM it plays, and with the table's own `rom_sha256`. |
| `entry` | `gba_recomp_title_table`: `const gba_recomp_table *gba_recomp_title_table(void)`. |
| `discovered` | How many discovered addresses the lift was given. The app compares it with the file's count to offer Prepare again (below). |

The other fields are the Xbox pack's. `tools/prepare/gba.py` (`read`, `check`) is the reference.

**The loader.** `pack_loader.c` (`xba_pack_load_rom`) never refuses to start a GBA game:

1. `packs/gba-<KEY>/pack.json` missing → `PACK_NONE gba <KEY> (not prepared, the interpreter runs it)`.
2. `recomp_pack_find_key` refuses it (format, console, title_key, abi, shell_abi = `xba_shell_abi`, lib, lib_size) →
   `PACK_STALE gba <KEY> (<reason>), running interpreted`.
3. Else `gba_runtime_start(rom, <pack>/libtitle.so, key)`; the runtime (`consoles/gba/runtime/gba_pack.c`) checks
   again with what only it knows: `rom_sha256` = SHA-256 of the ROM it read, `lib_sha256` (hashed at each start: a GBA
   pack is a few MB), `entry`; then `dlopen(RTLD_NOW | RTLD_LOCAL)`, the table, its `abi` (`gba_attach_recomp`) and its
   `rom_sha256`. `PACK_LOADED gba <KEY> (<N> functions)`, or `PACK_STALE gba <KEY> (<why>), running interpreted`.

On a desktop, `gba_runtime_opts.pack_lib` does the same (a library with no `pack.json` beside it: the table's checks
only); `gba_play` exports the core (`gba_core_exported`, `ENABLE_EXPORTS` + the whole archive).

**The discovery loop.** At exit (also the in-game menu's Quit, through the exit hook) the runtime merges
`gba_recomp_missed` (the ROM addresses the interpreter had to start running that the table did not cover, bit 0 =
Thumb) into `<files>/work/gba-<KEY>/discovered.txt`: one `0x%08X` per line, sorted, no repeats, at most 65536 lines,
written through a temporary file; nothing new, nothing written. Log: `gba: <N> address(es) the pack did not cover, <A>
new, in <path>`. The next Prepare runs with `--work <files>/work/gba-<KEY>`, finds the file, and the lift takes them
(`--discovered`). Home compares the file's count with the pack's `discovered`: more → « Prepared · N new addresses
found · prepare again », Play stays first (app-shell.md « Game Boy Advance games »).

**On a desktop** (Linux x86-64 or arm64, macOS): `python3 -m tools.prepare game.gba` builds a pack for `gba_play` with
the host's `cc` (or `--cc`, `$CC`): the same steps, `-shared -fPIC -Wl,-Bsymbolic`, the core left undefined (resolved
against `gba_play` at load), `"abi"` the host's (`x86_64`, `arm64-v8a`). Work and packs default to
`$XDG_DATA_HOME/xrecomp/work/gba-<KEY>` and `…/packs/gba-<KEY>` (beside `gba_play`'s saves); `--work`, `--pack-dir`,
`--jobs`, `--opt`, `--force`, `--discovered` as above. With `--android` and the NDK it is the device's build (« Checking
on a host »). Tests: `python3 -m pytest tools/prepare/test_gba_prepare.py` (the NDK case runs where
`ANDROID_NDK_HOME` is set), `consoles/gba/tests` `gba_pack`, `tests/recomp_pack`.

## The loader

The app shell's side (`android/app/src/main/cpp/pack_loader.{c,h}`, `src/platform/recomp_pack.{c,h}`; details and
logcat lines in [app-shell.md](app-shell.md) « Title packs »):

1. Only when no title is linked into `libmain.so` and `default.xbe` is found. Title ID from the XBE's certificate.
2. `<files dir>/packs/<TITLEID>/pack.json`; absent → `PACK_MISSING <TID>`, the shell goes on as before.
3. Checks: required fields, `format` 1, `title_id`, `abi` (`arm64-v8a`), `shell_abi` = `libmain.so`'s exported
   `xba_shell_abi` (1), `lib` a plain name, present, `lib_size` bytes. **`lib_sha256` is not checked at load**
   (private storage, atomic publish by prepare which hashed it; ~1 s per start for a large pack); the import drop,
   when it exists, checks it. Failure → `PACK_INVALID <reason>`.
4. `dlopen(lib, RTLD_NOW | RTLD_LOCAL)`; `dlsym` of `entry` (required), `recomp_lookup`, `recomp_lookup_manual`,
   `fe_menu_get_pb_state`, `g_fe_cursor`, each kept only if it lies in the pack (`dladdr`; `dlsym` on a handle also
   searches `libmain.so`). Failure → `PACK_DLOPEN <dlerror>`.
   Then `env_force`, if any: each option set over `recomp.cfg` (`recomp_pack_apply_env_force`).
5. The pointers go behind `libmain.so`'s weak forwarders; the title runs from `entry` on the guest thread, as a linked
   title does. On `PACK_INVALID` / `PACK_DLOPEN` the shell hands back to the launcher, which deletes the pack and
   offers Prepare again.

`tests/recomp_pack` checks it on Linux with a fake shell and fake packs linked the way `compile.py` links.

## Checking on a host

In `xba-android:local` (plus `python3-capstone`), with the real `libmain.so` from `android/app/src/main/cpp`
(`cmake -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a
-DANDROID_PLATFORM=30 --target main`) and the stub title's entry (`xbe_entry.c` out of
`tools/android/stub_title.sh`):

```bash
B=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64
python3 -m tools.prepare tools/conformance/test.xbe --android --work /tmp/aw --pack-dir /tmp/packs \
    --cc $B/bin/clang --sysroot $B/sysroot --shell-lib build/libmain.so --extra-src xbe_entry.c
$B/bin/llvm-readelf -d /tmp/packs/12345678/libtitle.so | grep NEEDED      # libmain.so, libm, libdl, liblog, libc
```

Checked on 2026-11-10: NEEDED `libmain.so`; every undefined symbol of `libtitle.so` (45 at -O2, 25 of them from the
toolkit) is exported by the real `libmain.so` or libc/libm/libdl/liblog; `recomp_title_main`, `xbe_entry_point`,
`recomp_lookup` exported; a second run compiles 0 files and keeps the pack; SIGTERM mid-compile exits 130 with
`CANCELLED`, leaves no compiler running and the previous pack in place, and the rerun compiles only the files
that were missing. Tests: `python -m pytest tools/prepare`.
