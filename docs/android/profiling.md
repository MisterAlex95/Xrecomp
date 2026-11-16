# Profiling on the device

XBA-36. Two tools to see where a game's time goes on the device: a CPU profile with a flame graph
(`tools/android/profile.sh`, the NDK's simpleperf), and the frame times in the performance line every runtime logs.

## The frame times in the log

Every runtime (the Xbox presenter, the GBA / PS1 / N64 loops) logs its performance line every 10 s and once over the
run ([HOST_LAYER.md « Performance line »](../../src/platform/HOST_LAYER.md#performance-line-xba-243)). Its last field
is the frame time: the mean length of a host frame (one flip on the Xbox, one loop turn elsewhere), top to top, so
its work **and** its sleep (the pacing the player sees), and the 99th percentile of those lengths:

```
xbox: perf speed ? (29.9 fps); render 3.1 present 0.6 idle 0.0 guest 29.7 ms/frame; worst 41 ms, 290 > 16.7 ms,
3 > 33.3 ms of 299 frames; shown 29.9 fps, busy 33.4 ms/frame; frame time mean 33.4 ms, p99 38.1 ms
```

- **fps**: `speed` (guest frames per second) and `shown` (pictures handed to the window per second);
- **frame time mean / p99**: from a histogram of 0.1 ms buckets up to 40 ms, then 2 ms buckets up to 200 ms; the p99
  is its bucket's upper edge (so the GBA's steady 16.74 ms frames read « p99 16.8 »; above 200 ms reads 200);
- **worst** and the slow-frame counts stay as they were: the work of a host frame (its sleep left out), against the
  budget.

The N64's own longer line gets the same field at its end (`...; core 6 @ 1.90 GHz; frame time mean 16.7 ms, p99 17.1
ms`). On Android the lines go to logcat:

```bash
adb logcat -s xboxrecomp | grep ': perf '
```

The ticket asked for every 5 s; the line kept its 10 s period (XBA-243), and `profile.sh` saves the lines logged
during a recording in `frames.log`.

## A CPU profile: `profile.sh`

```bash
export ANDROID_NDK_HOME=~/Android/Sdk/ndk/28.2.13676358     # any NDK with simpleperf/ (r23 and later)
tools/android/profile.sh 30                                  # the game running on the device
```

It records 30 s of the game's process and writes `~/xrecomp-profiles/<app id>-<date>/`:

| File | What |
|---|---|
| `flamegraph.html` | The flame graph of the game's process (simpleperf's inferno). Open it in a browser. |
| `profile.html` | simpleperf's `report_html.py`: functions by self / total time, call graphs, a flame graph per thread. |
| `hot.txt` | The hottest functions (self time, from 0.2 %), each lifted function followed by its guest address (`sub_0004F1C0  [xbox 0x0004F1C0]`). |
| `guest_symbols.tsv` | Every lifted function of the title pack: offset in `libtitle.so`, size, name, console, guest address. |
| `frames.log` | The performance lines logged during the recording. |
| `perf.data`, `binary_cache/`, `libs/` | The samples and the libraries they need (for `report.py`, `annotate.py`, another `report_html.py` run). |

The folder holds the title's compiled code (`libtitle.so`): keep it to yourself, never commit or attach it. The flame
graph and `hot.txt` only carry function names and addresses.

Options: `--serial` (several devices), `--package` (default `fr.adserver.xrecomp.shell`, the XRecomp app; a
per-title APK or the plain shell `fr.adserver.xrecomp.dev` has its own), `--out`, `--freq` (samples per second,
default 1000), `--dwarf` (DWARF unwinding instead of frame pointers), `--lib DIR` (unstripped libraries, see below),
`--no-report`. Needs `adb` and `python3` on the host; nothing is downloaded, everything comes from the NDK.

### Which builds can be profiled

| Build | Profile |
|---|---|
| `xrecomp-latest` (rolling prerelease, every push to `main`) | Yes: debuggable (`-PxbaDebuggable=1`), so simpleperf runs in the app's context through `run-as`. |
| A local debug build, `shell-latest` | Yes, with its own `--package`. |
| A release `xrecomp-v*` | No on a normal device: not debuggable, `run-as` refuses and so does simpleperf. Only on a rooted device (`su`; the script falls back to it). Install `xrecomp-latest` over it to profile: same key, it updates in place. |

### How it works

1. `pidof <package>`: the game runs in the app's main process (the launcher, Prepare and the warm-up have their own
   processes, `<package>:launcher` and so on), so the game must be running when the script starts.
2. `run-as <package> cat /proc/<pid>/maps`: the libraries mapped from the app's private storage, i.e. the title pack
   (`files/packs/<title>/libtitle.so`); they are pulled with `run-as … cat` (binary_cache_builder.py cannot read the
   app's storage).
3. The NDK's `simpleperf` for the device's ABI goes to `/data/local/tmp`, then `simpleperf record -e task-clock -f
   1000 --call-graph fp --duration 30 --app <package>` (all the app's processes; the reports keep the game's pid).
4. `binary_cache_builder.py` pulls the APK's and the system's libraries and takes the pulled pack (matched by build id:
   Prepare links with `--build-id=sha1`); then `report_html.py`, `inferno` and `simpleperf report` for `hot.txt`.

### Lifted functions are named

Every generated function is named after its guest address: `sub_0001ABCD` on the Xbox (`sub_0001ABCD_gen` for the
body behind a wrapper), `gba_rc_08000123`, `n64_rc_80001234` (`n64_rc_f_…`), `ps1_rc_80010000`. Prepare compiles with
`-g0` (no debug info) but links `libtitle.so` with ld.lld and **no** `-s`, so its `.symtab` keeps every name, and
nothing strips the pack afterwards (it is built on the device, not packaged by Gradle): the flame graph names lifted
functions with no side file. Functions with another name (hand-written overrides, names `func_id` found) get their
address from the title's dispatch table, read from the Prepare work folder (`files/prepare/<title>/…_dispatch.c`) when
it is still there. A pack that has lost its `.symtab` is reported (« stripped »); its functions then show as
`libtitle.so[+offset]`.

`libmain.so` (the toolkit: the GPU back end, the kernel, the audio) comes from the APK, where Gradle strips its
`.symtab`: its exported functions keep their names (the title links against them), its static ones show as offsets.
For full names pass the unstripped `libmain.so` of the **same** build with `--lib` (a local build of the commit:
`android/app/build/intermediates/merged_native_libs/<variant>/out/lib/arm64-v8a/`); a different build is ignored (its
build id does not match).

### Call graphs and frame pointers

Android's arm64 compiler keeps frame pointers by default (clang `-mframe-pointer=non-leaf` for
`aarch64-linux-android`, checked on NDK r28's clang), and Prepare compiles for that target, so packs need no
`-fno-omit-frame-pointer` and no profiling build: `--call-graph fp` costs almost nothing and works on every pack. A
leaf function keeps no frame record, so in a sample taken inside one its direct caller can be missing from the stack;
`--dwarf` unwinds from the `.eh_frame` instead (exact, heavier: 64 KB of stack copied per sample, so lower `--freq`).

### Troubleshooting

- « not debuggable »: a release APK. Install `xrecomp-latest`.
- « not running »: start the game first (the Home screen alone is the launcher process).
- `simpleperf record failed` with a `perf_event` error: the device's kernel refuses (rare on Android 11+ for a
  debuggable app); try `adb shell setprop security.perf_harden 0` on a rooted device.
- An empty flame graph for `libtitle.so`: check `guest_symbols.tsv` exists; « no library from the app's storage » means
  no pack was loaded (an unprepared title runs on the interpreter path inside `libmain.so`).
