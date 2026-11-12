# macOS launcher

## Prepare interface

The launcher drives the desktop prepare through three machine-readable entry points. They are the contract between
`tools/prepare/{__main__,progress,identify}.py` and the launcher's Swift code (`PrepareProtocol`, `TitleState`,
`LibraryScanner`): change them only together. All three run from the toolkit root (`cwd` = the checkout) with the
toolkit's Python, e.g. `python -u -m tools.prepare ...`. Without the new flags the desktop prepare prints exactly
what it printed before. Tests: `tools/prepare/test_desktop_progress.py`, `tools/prepare/test_identify.py`
(synthetic `tools/conformance/test.xbe` and a `tools/xiso/mkxiso.py` image only).

### 1. Identify a source: `python -m tools.prepare.identify SOURCE --json [--xbe-out FILE]`

For the library scan. `SOURCE` is what prepare takes: a `.iso`/`.xiso`, a folder holding `default.xbe`, or a
`default.xbe`. It reads only the XBE header and certificate; for an image only `default.xbe` is read, never the rest
of the disc. `--xbe-out FILE` writes the source's `default.xbe` to `FILE` (temp file + rename, any source kind) so
the launcher can decode the `$$XTIMAGE` art from it. Nothing else is written. About 50 ms per source.

One JSON object on one line, exit 0:

```json
{"source": "/Users/me/Documents/xbox/Burnout 3 - Takedown (Europe)/Burnout 3 - Takedown (Europe) (Fr,De,It).iso",
 "kind": "image", "size": 2966814720, "title_id": "4541005B", "name": "Burnout 3",
 "profile_name": "Burnout 3: Takedown", "version": 3, "region": 4, "disc_number": 0,
 "xbe": "/Users/me/Library/Application Support/xboxrecomp/titles/4541005B/default.xbe", "xbe_size": 4308992,
 "has_title_image": true}
```

| Field | Type | Meaning |
|---|---|---|
| `source` | string | `SOURCE`, absolute. |
| `kind` | string | `image`, `folder` or `xbe`. |
| `size` | int | Bytes: the image file, or `default.xbe` for a folder or XBE source (a scan-cache key with the mtime). |
| `title_id` | string | 8 upper-case hex digits. |
| `name` | string | The certificate's title name (the title ID when it is empty). |
| `profile_name` | string or null | `name` of the title profile (`profiles/titles/<tid>/title.json`), when there is one. Show it in preference to `name`. |
| `version`, `region`, `disc_number` | int | From the certificate. |
| `xbe` | string or null | A readable `default.xbe`: `--xbe-out` when given, else the file in place; null for an image without `--xbe-out`. |
| `xbe_size` | int | Bytes of `default.xbe`. |
| `has_title_image` | bool | The XBE has a `$$XTIMAGE` section. |

Failure: exit 1 and `{"error": {"code": "...", "msg": "..."}}` on one line. Codes: `NO_XBE` (no such path, no
`default.xbe`, not a supported kind), `BAD_IMAGE` (not an XDVDFS image, or it ends inside `default.xbe`: an ISO still
being copied, retry on the next scan), `BAD_XBE` (`default.xbe` does not parse), `NO_SPACE`/`BAD_ARGS` (writing
`--xbe-out` failed).

### 2. Title state: `python -m tools.prepare SOURCE --work DIR --check --json`

Whether the title is prepared and up to date, without doing anything: it writes nothing, not even `DIR` (for an
image whose `DIR/xbe/default.xbe` does not exist yet it reads `default.xbe` into a temporary file and removes it).
Without `--work` the work folder is prepare's default, `<title id>-work` next to `SOURCE`. One JSON object on one
line, exit 0:

```json
{"state": "stale", "reasons": ["toolkit", "discovered"],
 "exe": "/Users/me/Library/Application Support/xboxrecomp/titles/4541005B/work/project/build/burnout3",
 "title": "Burnout 3: Takedown", "title_id": "4541005B", "env_force": {"RECOMP_LSFG": "0"},
 "work": "/Users/me/Library/Application Support/xboxrecomp/titles/4541005B/work"}
```

| Field | Type | Meaning |
|---|---|---|
| `state` | string | `absent` (no executable), `stale` (an executable, but the next prepare would redo something), `ready`. |
| `reasons` | [string] | Empty for `ready`. For `stale`, any of: `toolkit` (another toolkit commit), `profile` (the profile layers' files changed), `discovered` (runs found new addresses in `discovered.jsonl`: show "Improvements found while playing: prepare again"), `xbe` (another `default.xbe`), `not_built` (the lift is newer than the executable: a build failed or was cancelled), `no_stamp` (an executable without `stamp.json`). For `absent`: `["not_built"]` when a prepare started but has not built yet, else `[]`. |
| `exe` | string or null | The executable (`project(...)` of `DIR/project/CMakeLists.txt` in `DIR/project/build`), also when `stale`: the old binary can still be played. |
| `title` | string | The title profile's `name`, else the certificate's title name. |
| `title_id` | string | 8 upper-case hex digits. |
| `env_force` | {string: string} | The options the profile layers force (compiled into the binary, they win over the environment): show those settings as "set by the title". |
| `work` | string | The work folder looked at. |

Failure: exit 1 and `{"state": "error", "code": "...", "msg": "..."}`: the identify codes above, or `BAD_PROFILE`
(a profile layer is not well formed). `--check` without `--json` prints one human line instead.

### 3. Prepare with progress: `python -u -m tools.prepare SOURCE --work DIR --progress`

The normal desktop prepare (five steps, each skipped when up to date), with protocol lines on stdout between the
human ones. Read stdout line by line: lines starting `XBA_PROGRESS ` or `XBA_RESULT ` drive the UI, every other line
goes to the log pane. Errors (the failed command's log tail) go to stderr. The grammar is the Android one
(`tools/prepare/progress.py`, `parse_line`; docs/android/prepare.md):

```
XBA_PROGRESS step=<i>/5 id=<step id> pct=<0-100> msg=<rest of line>
XBA_RESULT ok exe=<executable>
XBA_RESULT ok project=<project folder>         (only with --no-build)
XBA_RESULT error code=<UPPER_SNAKE> msg=<text>
```

`msg` is always last and takes the rest of the line (spaces and `=` included); other values never hold spaces; a
reader ignores keys it does not know (`eta=` may come later). `step` never goes back, `pct` is per step. Exactly one
`XBA_RESULT` line ends a run.

| Step | `id` | `pct` |
|---|---|---|
| 1 | `identify` | 0 finding `default.xbe`, 30 title read, 100 profile resolved (`msg` = title and ID) |
| 2 | `analyse` | 0, 2 xbe_parser, 5 disasm, 60 func_id, 80 abi_analysis, 100 done (or 100 "up to date") |
| 3 | `project` | 0, 100 |
| 4 | `lift` | 0, 100 (`msg` = C file counts, or "up to date") |
| 5 | `build` | 0, 5 CMake configure, 5-90 from the build's `[i/n]` (Ninja) or `[ nn%]` (make) lines (`msg` = `compiling 12/345`, at most one line per percent or 2 s), 90 built, 90-98 extracting the disc (image sources), 98 the disc's other images, 100 ready |

Error codes and exit status:

| Code | When |
|---|---|
| `NO_XBE` | step 1: no `default.xbe`, or the image is unreadable |
| `BAD_XBE` | step 1: `xbe_parser` failed |
| `BAD_PROFILE` | step 1: a profile layer is not well formed |
| `ANALYSE_FAILED` | step 2: disasm, func_id or abi_analysis failed |
| `LIFT_FAILED` | step 4: `tools.recomp` failed |
| `COMPILE_FAILED` | step 5: CMake configure or build failed, or another image's prepare |
| `NO_SPACE` | ENOSPC anywhere |
| `CANCELLED` | SIGTERM or SIGINT: exit 130 |
| `INTERNAL` | a bug (traceback on stderr), or step 3 failed |

Exit 0 after `ok`, 1 after an error, 130 after `CANCELLED`. A failed command's `msg` is
`<tool> failed (exit N), see <DIR>/prepare.log`.

**Cancel**: send SIGTERM to the prepare's process group (start it in its own group), SIGKILL after 5 s. On SIGTERM
prepare kills the command it is running, prints `XBA_RESULT error code=CANCELLED msg=cancelled (signal 15)` and exits
130. The stamp and the incremental CMake build make the next prepare resume where it stopped.

Example (synthetic `test.xbe`, human lines shortened):

```
[1/5] finding default.xbe
XBA_PROGRESS step=1/5 id=identify pct=0 msg=finding default.xbe
      SYNTH (title ID 12345678), work folder /tmp/w
XBA_PROGRESS step=1/5 id=identify pct=30 msg=SYNTH (title ID 12345678)
XBA_PROGRESS step=1/5 id=identify pct=100 msg=SYNTH (title ID 12345678)
[2/5] analysing the executable (a few minutes the first time)
XBA_PROGRESS step=2/5 id=analyse pct=0 msg=analysing the executable (a few minutes the first time)
...
[5/5] building for this computer
XBA_PROGRESS step=5/5 id=build pct=0 msg=building for this computer
XBA_PROGRESS step=5/5 id=build pct=5 msg=compiling
XBA_PROGRESS step=5/5 id=build pct=13 msg=compiling 10/104
...
XBA_PROGRESS step=5/5 id=build pct=100 msg=ready
XBA_RESULT ok exe=/tmp/w/project/build/title_12345678
```
