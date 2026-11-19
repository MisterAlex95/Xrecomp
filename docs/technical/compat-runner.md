# The compat runner

> Part of the [Documentation Index](../INDEX.md)

`tools.compat` prepares titles from the player's own discs, boots each one headless with a pad script, and says how
far it got, so the « Compatibility » matrix (XBA-73) comes from runs rather than impressions (XBA-62).

```bash
python -m tools.compat run /dumps/*.iso --work-root /scratch/compat --seconds 180 --repeat 3
python -m tools.compat report compat-results.jsonl     # Markdown table, one row per title
```

**Without the title layer** (profile-free WP6): `run --no-title-layer` (and `heal --no-title-layer`) prepares with
`XBA_NO_TITLE_LAYER=1`, in a work folder of its own (`<name>-no-title-layer`, so the two builds never share a stamp, an
executable or a discovery history), and marks its result lines `"title_layer": false`. `report` then adds a « No title
layer » column next to each title's normal stage (in bold when lower: a title layer must never be needed to run); the
matrix lists such runs as rows of their own (`<title> (no title layer)`), not counted as more titles and never taken
for a regression of the normal run. The A/B of the whole matrix:

```bash
python -m tools.compat run /dumps/*.iso --seconds 180
python -m tools.compat run /dumps/*.iso --seconds 180 --no-title-layer
python -m tools.compat report compat-results.jsonl
```

One title at a time, at `nice 10`, each boot stopped by PID after `--seconds`. Run it on a machine of your own
(Linux arm64 first, then x86-64 and macOS); never on the LXC that hosts Gitea and Plane. The results hold numbers,
stage names and the first line of an error: nothing from the game.

## What it reads

| Source | What |
|---|---|
| `RECOMP_STATUS` file | flips, picture changes (a hash of a 32x24 grid at each flip), lit, non-silent audio, pad reads, guest threads, what the renderer could not draw (`unsupported`, XBA-120) (`src/kernel/recomp_status.h`) |
| `discovered.jsonl` | what the analysis missed ([discovery.md](discovery.md)) |
| the run's stderr | a crash line, the first untranslated instruction reached |

The pad: `start` three times from 8 s, then `a`, every 2 s, through `RECOMP_PAD_LIVE`. Each run has its own data home,
`<work>/data-<n>` (`XDG_DATA_HOME`, made empty for the run): saves, the launch data a quick reboot or a hand-over to
another XBE reads back, the partition images. Not the user's `~/.local/share/xboxrecomp`, which carried one run's state
into the next and needed a writable HOME.

The screen: `RECOMP_FB_DUMP` writes a BMP per report; the last one of each run is kept as `<work>/shot-<n>.bmp`
and named in the results (`runs[].screenshot`). It stays in the work folder: it is the game's picture, never part of
the matrix.

## Stages

| Stage | Means |
|---|---|
| `none` | prepare failed, or no executable |
| `prepares` | built; the runtime wrote no status line |
| `boots` | the title did something: a flip, a thread, a pad read, a sound |
| `renders` | a presented frame was not near-black |
| `responsive` | the picture changed within 5 s of a press, and the title reads the pad |
| `sustained` | over the last `--sustain` seconds (default 60): flips in every 10 s, the picture still changing, no exit |

Playable (30 minutes, saves kept) is a person's check. With `--repeat N` the title's stage is the worst run, `best`
the best one, and `failure_rate` the share of runs below the best: a race that loses one boot in four shows.

The blocker is the first of: a crash (signal and fault line), an early exit, no status line, no flip, a hang (no flip
for 30 s at the end), each with the first untranslated instruction or the last discovery when there is one. A
guest stack switch the runtime could not carry (a `stack_switch` discovery entry, XBA-101,
[stack-switching.md](stack-switching.md)) is named after any of them: « hang: ...; stack switch at 0x... to 0x...:
<why> ». Any blocker but a crash or a launch also names the stub kernel routes the run called, from the
discovery file's `stub` entries: « no flip; calls stub ordinal 136 (KeRemoveQueue) » (XBA-115).
A guest divide error no handler took (the runtime's `[#DE]` report, then bugcheck 0x1E) is named after the exit:
« exited with code 30 after 3 s; guest exception 0xC0000094 at 0x00012340: divide by zero »; an unhandled
guest exception (`[SEH] unhandled exception`, after which the title goes on) after any blocker but a crash: « no flip;
guest exception 0xC0000005 at 0x00045678: no guest handler took it » (XBA-110, XBA-112). The discovery file's `exception`
entries (XBA-129: divide errors, unhandled raises, host faults no guest handler took, XBA-132) are named first, after
any blocker. These words (`tools/compat/blockers.py`) are the ones `heal.py` and the app's « needs a fix » use. What the renderer met and could not draw
(the status file's `unsupported`: texture formats, points and lines, volume textures, stage modes, state, by name and
batches; [gpu-backend.md](gpu-backend.md#what-is-not-drawn-xba-120)) is kept in the run's summary (`unsupported`) and
named after any blocker but a crash or a launch, the three most used first: « nothing lit on screen; renderer lacks
texture format 0x2F LU_IMAGE_DEPTH_X8_Y24_FLOAT (40 batches), points (12 batches), +1 more ». The device's library check
grades through the same `classify()` (`tools/compat/device.py`), so its words are these.

## The « Compatibility » page (XBA-73)

```bash
python -m tools.compat report compat-results.jsonl --matrix   # the page as Markdown
python -m tools.compat plane compat-results.jsonl             # agents' host: write it to Plane, a Bug per regression
```

The results file keeps every run (append). The matrix shows, per title, its last run: stage, the change since the run
before (`up from`, `**down from**`, `same`), blocker, XDK build, engine layer, toolkit commit and date; a summary of
how many titles are at each stage; and the regressions. `plane` writes the Plane page « Compatibility » with the
`plane` CLI and opens one Bug per regression not already open (title `Regression: <title> <from> -> <to> (toolkit
<commit>)`, under XBA-60), to bisect between the two toolkit commits.

## On the device: the app's library check (XBA-130)

The app's « Check my library » ([app-shell.md](../android/app-shell.md#library-check-xba-130)) boots each title on the
phone, headed, and grades it with this tool's rules through

```bash
python -m tools.compat grade --out compat-results.jsonl --source TITLE --work WORK \
    --status status.jsonl --log run.log --presses 8000,10000,... --exit none --run-s 180 \
    [--prepare-s N] [--field device=...]...          # or --prepare-failed "CODE: msg", --not-booted "why"
```

(`tools/compat/device.py`; the app runs it with the CPython it embeds for Prepare: one implementation of the grading).
It appends one line with this tool's schema plus the device's fields: `host` (`android`), `title` (the certificate's
name; `source` is the same), `xdk_libraries`, `engines` (names), `device`, `android`, `api`, `soc`, `app_build`,
`renderer` (from the runtime's `[GPUGL]` line), `seconds`, `peak_mem_mb`, `prepare_s`. `--exit` takes how Android said
the process ended: `exit:N`, `signal:N`, `crash[:N]` (the signal from the log's `Fatal signal N` when the system did not
say), `java`, `anr`, `lowmem`, `window` (the game's window closed), and what the check saw: `lost` (the check itself was
restarted during the boot), `away` (the game left the screen), `nostart`. Status lines of a quick reboot (a second
process, its clock from 0 again) are put on one clock before grading.

The run's environment is `device.check_env`: `boot()`'s minus `RECOMP_HEADLESS` and `RECOMP_FB_DUMP` (nothing is
dumped on the device). `report`, `report --matrix` and `plane` read a device file as a desktop one; when a line has
`device`, the tables get a Device column (« AYN Thor, Android 13 »).

## Self-healing rounds and recall (XBA-129)

```bash
python -m tools.compat heal /dumps/title.iso --work-root /scratch/compat --seconds 180 [--max-idle 1]
python -m tools.compat recall /scratch/compat/title /dumps/title/default.xbe [--icall-dump icall.txt] [--json]
```

`heal` is the app's self-healing loop on a desktop (`tools/compat/heal.py`; the app's side:
[prepare.md « Self-healing »](../android/prepare.md#self-healing-xba-74-xba-129)). A round is one `run` (prepare +
boot); another round follows when the boot met code the analysis missed (`icall`, `thread`, `thread_ctx` entries past
what `discovered.jsonl` held before it), crash or not. It stops on:

| Stop | When |
|---|---|
| `nothing new found` | the run met no new code and ran its time |
| `needs a fix: <blocker>` | the run met no new code and stopped, or stopped with what a prepare cannot fix: a guest exception nothing handled, a stack switch the runtime could not carry, an untranslated instruction, code written or run at run time (XBA-118) (named in that order); calls to stub kernel routes are named after the blocker |
| `no progress: the prepare accepted no new seed` | the round's prepare took none of the new addresses (seed_from_log's gates: not code) |
| `no progress: the same blocker twice` | the run stopped on the blocker of the round before (seconds aside) |
| `safety cap: 12 rounds` | `--max-rounds`, a safety net only |

The cap is on rounds without progress (`--max-idle`, default 1: the first such round stops), not on rounds. The
results line is the last round's, with `heal: {rounds, stop, history: [{round, stage, blocker, new_seeds, accepted,
run_seeds, then}]}`; `run_seeds` is the runtime's own count (the status line's `seeds`).

**Recall** (`tools/compat/recall.py`): from a title's whole `discovered.jsonl` history, the targets reached at run time
that the static analysis missed, what the prepares made of them (`seeded`, `rejected` by the gates, `pending`), and
where each one is referenced from in the image, as input for the disassembler:

| Category | The missed address |
|---|---|
| thread start | a `thread` / `thread_ctx` entry |
| vtable | is a word next to another word that points into code (a vtable, a table of function pointers) |
| table word | is a word in the image, alone among non-code words (a dispatch or class-driver table entry) |
| computed jump | appears nowhere in the image: computed at run time |

`inside_known` counts misses inside a function the analysis knows (the sweep decoded through them). The share found
statically needs the targets reached that the analysis did find, which the discovery file does not hold. Every
runtime writes them to `icall_targets.dump` beside it (XBA-304, below), which `recall` reads by default
(`--icall-dump` names another, e.g. a `RECOMP_ICALL_FEEDBACK` build's): the share is then
`found_statically / targets`, else « recall unknown ». `run` adds a `recall` object to a title's line when a run
missed anything, and `report` / `report --matrix` get a Recall column (« recall unknown (3 misses: 3 thread start) »).
Tested on synthetic titles only (`tools/compat/test_heal.py`, and `test_e2e.py::test_the_self_healing_loop`: three
threads found one round at a time, the third refused, four rounds); a real title's numbers are XBA-75.

### Recording hits by default (XBA-138, XBA-304)

Every build records the targets a run reaches that the analysis found (the hits), at no cost on a hit. Since
XBA-277 a hit is a bounds check and a load in the generated dispatch's window (`g_recomp_flat`), which never reaches
`RECOMP_ICALL_OBSERVE`. Recording on the hit path (test-before-set into a seen array, the XBA-138 proposal) would add
a load, a compare and a branch to every hit, a few percent of `w_icall`. Instead the window records itself:

- `recomp_dispatch_init` (generated, `tools/recomp/translator.py`) points the window at `g_hit_table`, a second
  table as large as the flat one that starts empty (calloc: only touched pages are committed).
- A call whose slot is empty takes the miss path as before (trace note, `RECOMP_ICALL_IS_CODE`, manual, generated
  and kernel lookups). Its answer goes through `recomp_icall_install` (`recomp_types.h`, `RECOMP_ICALL_INSTALL_`),
  which puts it in its slot when the VA is a translated function. The value stored is the slow path's own answer
  (the manual override first), so a hit calls what the full table would have. The next call to that target hits.
- So the filled slots are exactly the targets reached that the analysis found. Each target pays one slow call;
  a hit pays nothing. Filled slots survive a later `recomp_dispatch_init` (the window moving).
- `recomp_dispatch_init` hands the table to the runtime through the weak `recomp_icall_hits_register`
  (`src/kernel/icall_feedback.c`). `recomp_icall_hits_dump` writes `icall_targets.dump` beside `discovered.jsonl`,
  in the icall-feedback text format (`VA 1`, a `# window` line giving the range covered). It merges with what the
  file already held, so every run adds its targets, and writes through a temporary file and a rename. It dumps:
  - at a clean exit (atexit);
  - at `HalReturnToFirmware` and at a bug check (`ExitProcess` skips atexit);
  - when the SDL window's loop ends (Android skips atexit);
  - when the app goes to the background (Android may end it there).

  It writes nothing when nothing was reached and no file exists. It is not called from a signal handler: a crash
  keeps the last dump.
- `recall` reads that file from the work folder by default (`--icall-dump` names another), so a title's line gets a
  share instead of « recall unknown ». A missing or empty dump keeps the recall unknown, never 0 %.
- Switches: `RECOMP_ICALL_RECORD=0` (read at the first `recomp_dispatch_init`) publishes the whole table as the
  window again and records nothing, the A/B switch. `RECOMP_ICALL_DUMP=<path>` writes the dump elsewhere; `=0` writes
  none. Without a discovery file (`RECOMP_DISCOVERY` unset) there is nowhere to write by default.
- A `RECOMP_ICALL_FEEDBACK` build keeps its own instrument: every call goes through the miss path,
  `RECOMP_ICALL_OBSERVE` and the per-site records, and `icall_targets.dump` / `icall_sites.dump` in the working
  directory.

**Measured** (`python -m tools.bench icount`, scale 0.1, instructions executed under qemu-user less a run at scale 0,
origin/main -> XBA-304 on the same corpus; x86-64 clang 19 exe layout, arm64 gcc 14 cross (Debian
aarch64-linux-gnu) exe and so layouts):

| Workload | x86-64 | arm64 exe | arm64 so |
|---|---|---|---|
| w_int, w_mem, w_rep, w_bytes, w_x87, w_sse, w_lock | 0 % | 0 % | 0 % (±11, scale-0 jitter) |
| w_icall (an indirect call in a loop) | 9,250,488 -> 9,250,662 (+0.002 %) | 11,900,382 -> 11,900,556 (+0.001 %) | 13,700,373 -> 13,700,560 (+0.001 %) |
| w_vcall (calls into a function that makes an indirect call) | 19,450,451 -> 19,450,625 (+0.001 %) | 26,500,319 -> 26,500,487 (+0.001 %) | 30,700,298 -> 30,700,470 (+0.001 %) |

The ~170 instructions are each target's first call through the miss path, once per run. The macro's install
returns the function it was given, so the miss and the hit still end in one shared call. A first version that
called the install and then the target had the hit path jump over it: +1 instruction per call, +2.2 % on `w_icall`
(x86-64).

**Every generated pack is stale**: titles must be prepared again. A pack generated before XBA-304 never registers its
table: it runs as before, writes no dump, and its recall stays unknown. The generated dispatch and `recomp_types.h`
change together, since the macros call `recomp_icall_install`, which the generated dispatch defines. Memory: the
hit table doubles the flat table's virtual size (8 bytes per byte of code span); pages are committed only where
targets are reached. Windows hosts (no weak symbols in the generated code) record nothing.

Each run keeps the status file's `apu_dsp` object (what the title asked of the APU's DSPs, XBA-122:
[APU DSP](apu-dsp.md)); once one title has it, `report` gets a DSP column (« GP image 9cd378fc (1500 words), 13 cmds,
sends: reverb+FX, 1 stall(s) »). Titles sharing a GP image hash share DirectSound's effects image.

## Limits

- `responsive` is a heuristic: an animated screen changes on its own. The press times and the status clock start a
  fraction of a second apart.
- `tools/conformance/test.xbe` is a lifter test image with no entry point: it prepares but does not boot. The tool's
  own tests use a stand-in title (`tools/compat/test_compat.py`) and a synthetic XBE of our own
  (`tools/compat/test_e2e.py`: XAPILIB/D3D8/DSOUND in its library table, one `jmp $` function), which the linux CI
  image prepares, builds against the tree and boots through the runner (`XBA_E2E=1`): the profile layers, the
  template and the runtime linked and run together on every PR.
