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
place.

`TITLEID` is the XBE's title ID in 8 upper-case hex digits (`4D5300D1`).

**Atomic publish.** A pack is built in `packs/<TITLEID>.partial/`; `pack.json` is written last (temp file, fsync,
rename), then the directory is renamed to `packs/<TITLEID>`. An existing pack is first renamed to
`packs/<TITLEID>.old` and removed after the switch (POSIX cannot rename a directory over a non-empty one); the next
prepare puts `.old` back if a kill fell between the two renames. So a reader sees a whole pack or none:

- the loader only looks at `packs/<TITLEID>` (8 hex digits, no suffix) that has a `pack.json`;
- `*.partial` and `*.old` belong to prepare: the app ignores them and may delete them when no prepare runs.

## pack.json

```json
{"format": 1, "title_id": "4D5300D1", "title": "...", "xbe_sha256": "...", "abi": "arm64-v8a", "shell_abi": 1,
 "toolkit": "<commit>", "lib": "libtitle.so", "lib_sha256": "...", "lib_size": 123456,
 "entry": "recomp_title_main", "cc": "clang 19.0.1 -O1", "built_at": "2026-11-10T21:10:35Z"}
```

| Field | Type | Meaning |
|---|---|---|
| `format` | int | 1. A loader refuses a format it does not know. |
| `title_id` | string | 8 upper-case hex digits, = the directory name. |
| `title` | string | The XBE's title name, for display. |
| `xbe_sha256` | hex | The XBE the pack was built from. The app compares it with the game folder's `default.xbe`. |
| `abi` | string | `arm64-v8a`. |
| `shell_abi` | int | The shell ABI the pack was linked against. The shell exports its own integer; **the loader refuses a mismatch** (the app then offers to prepare again). |
| `toolkit` | string | The toolkit commit (`git rev-parse HEAD`, or `tools/VERSION` in the assets), `unknown` if neither. |
| `lib` | string | `libtitle.so`, relative to the pack directory. |
| `lib_sha256`, `lib_size` | hex, int | Of `lib`. The loader checks both before dlopen. |
| `entry` | string | `recomp_title_main`: `int recomp_title_main(const recomp_storage *)` (templates/new-game/src/main.c). |
| `cc` | string | Compiler and level, informative (`clang 19.0.1 -O1`). |
| `built_at` | string | ISO 8601, UTC (`Z`). |

Readers ignore fields they do not know. `tools/prepare/pack.py` (`read`, `check`) is the reference for the checks.

### What a pack exports

`libtitle.so` is linked with `-Bsymbolic` (the title's calls to itself bind inside it) and `--no-undefined` against
`libmain.so`, so every symbol it uses is exported by the shell or by libc/libm/libdl/liblog. Its dynamic symbols
include `recomp_title_main` and the symbols the shell defines as weak stand-ins today (`recomp_lookup`,
`recomp_lookup_manual`, and per title `g_fe_cursor`, `fe_menu_get_pb_state`). **For the loader:** the toolkit inside
`libmain.so` calls `recomp_lookup` (thread start, APCs); a library dlopened later does not override `libmain.so`'s
own weak definition, so the loader has to fetch these with `dlsym` and hand them to the runtime (a hook it installs),
not rely on symbol interposition.

## The command

```
python -m tools.prepare SOURCE --android --work DIR --pack-dir DIR --cc PATH [--cc-prefix-arg clang]
       --sysroot DIR --shell-lib PATH [--jobs N] [--opt 1] [--shell-abi N]
       [--ld PATH [--ld-prefix-arg ARG]] [--builtins PATH] [--python PATH]
       [--extra-src FILE.c]... [--include-dir DIR]... [--force]
```

| Flag | Device value |
|---|---|
| `SOURCE` | The game folder (holds `default.xbe`; the app's ISO import extracted it), or a `default.xbe`, or an image (`.iso`/`.xiso`: only `default.xbe` is read out). |
| `--work DIR` | Per title, internal storage, e.g. `<files dir>/prepare/<TITLEID>`. Holds the copy of the XBE, the analysis, the C project, the object cache, `stamp.json`, `prepare.log`. Keep it between runs: it is what makes a rerun cheap. Several hundred MB for a large title (the C alone is ~250 MB for one). |
| `--pack-dir DIR` | `<files dir>/packs` |
| `--cc PATH` | `<nativeLibraryDir>/libxbacc.so` |
| `--cc-prefix-arg clang` | The multicall applet. With a prefix arg, the linker is `PATH ld.lld` unless `--ld` says otherwise. On a host: the NDK's `clang` and no prefix (ld.lld is taken next to it). |
| `--sysroot DIR` | The NDK-layout sysroot from the assets (`usr/include`, `usr/lib/aarch64-linux-android/30/{crtbegin_so.o,crtend_so.o,libc.so,...}`). |
| `--builtins PATH` | `libclang_rt.builtins-aarch64-android.a`; default: asked from the compiler (`-print-libgcc-file-name`). |
| `--shell-lib PATH` | `<nativeLibraryDir>/libmain.so`: the pack links against it (DT_NEEDED `libmain.so`). |
| `--shell-abi N` | The integer the shell exports (default 1), written to `pack.json`. |
| `--jobs N` | Parallel compiles; 0 or absent = `min(5, (MemAvailable − 3 GB) / 1.5 GB)`, at least 1, at most the CPU count. |
| `--opt N` | clang `-O` level, default 1 (see Measurements). |
| `--python PATH` | The interpreter for the pipeline steps; default `$XBA_PYTHON`, else `sys.executable`. |
| `--extra-src`, `--include-dir` | Development: an extra C file in the pack, an extra `-I`. |
| `--force` | Redo every step (the object cache still applies). |

The steps, each one subprocess (`python -m tools.<tool>`) or a pool of compiler processes, never the
`multiprocessing` module (no `sem_open` on Android):

| # | id | Does | Stamp |
|---|---|---|---|
| 1 | `identify` | finds `default.xbe`, copies it to `<work>/xbe/`, `xbe_parser` → title ID, title profile | `parse` |
| 2 | `analyse` | `disasm`, `func_id`, `abi_analysis` | `disasm`, `func_id`, `abi` |
| 3 | `lift` | the project from `templates/new-game` + `titles/<title id>/`, then `tools.recomp` | `lift` (= profile digest) |
| 4 | `compile` | every C file to an object, largest first, cached | the object cache |
| 5 | `pack` | `ld.lld` → `packs/<TID>.partial/libtitle.so`, `pack.json`, publish | `link` |

Stamps live in `<work>/stamp.json` with the XBE's sha256 and the toolkit commit: another XBE or another toolkit
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
XBA_PROGRESS step=1/5 id=identify pct=100 msg=SYNTH (title ID 12345678)
XBA_PROGRESS step=2/5 id=analyse pct=55 msg=disasm done in 0 s
XBA_PROGRESS step=4/5 id=compile pct=40 eta=12 msg=2/5 files (2 compiled)
XBA_PROGRESS step=5/5 id=pack pct=100 msg=pack 12345678, 19272 bytes
XBA_RESULT ok pack=/data/user/0/<app>/files/packs/12345678
```

| Error code | When |
|---|---|
| `BAD_ARGS` | a required flag is missing (`--work`, `--pack-dir`, `--cc`, `--sysroot`, `--shell-lib`) |
| `NO_XBE` | no `default.xbe` in the source |
| `BAD_XBE` | `xbe_parser` cannot read it |
| `ANALYSE_FAILED`, `LIFT_FAILED` | a pipeline tool failed; details in `prepare.log` |
| `COMPILE_FAILED` | a C file did not compile; `msg` = file and first error |
| `LINK_FAILED` | ld.lld failed; `msg` = the first undefined symbols, if that is why |
| `PACK_FAILED` | writing or publishing the pack failed |
| `NO_SPACE` | ENOSPC anywhere |
| `CANCELLED` | SIGTERM or SIGINT |
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

**Toolkit tree** in the assets (read-only): `tools/` (Python), `include/`, the headers of `src/` (`src/**/*.h`),
`templates/new-game/`, `titles/`, and `tools/VERSION` (one line: the commit; written by the asset build, git-ignored).

**status.json**: the service writes it from the progress lines for the UI and for a restart after the app was killed.
Its format belongs to the app (prepare-app branch, `android/app`), not to this page; prepare never reads it.

## Measurements

<!-- T4: filled from a run on a real title's generated C (numbers only). -->

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

Checked on 2026-11-10: NEEDED `libmain.so`; every undefined symbol of `libtitle.so` (46, 25 of them from the
toolkit) is exported by the real `libmain.so` or libc/libm/libdl/liblog; `recomp_title_main`, `xbe_entry_point`,
`recomp_lookup` exported; a second run compiles 0 files and keeps the pack; SIGTERM mid-compile exits 130 with
`CANCELLED`, leaves no compiler running and the previous pack in place, and the rerun compiles only the files
that were missing. Tests: `python -m pytest tools/prepare`.
