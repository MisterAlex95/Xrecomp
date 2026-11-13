# The compat runner

> Part of the [Documentation Index](../INDEX.md)

`tools.compat` prepares titles from the player's own discs, boots each one headless with a pad script, and says how
far it got, so the « Compatibility » matrix (XBA-73) comes from runs rather than impressions (XBA-62).

```bash
python -m tools.compat run /dumps/*.iso --work-root /scratch/compat --seconds 180 --repeat 3
python -m tools.compat report compat-results.jsonl     # Markdown table, one row per title
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

The pad: `start` three times from 8 s, then `a`, every 2 s, through `RECOMP_PAD_LIVE`.

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
<why> (XBA-101) ». Any blocker but a crash or a launch also names the stub kernel routes the run called, from the
discovery file's `stub` entries: « no flip; calls stub ordinal 136 (KeRemoveQueue) » (XBA-115).
A guest divide error no handler took (the runtime's `[#DE]` report, then bugcheck 0x1E) is named after the exit:
« exited with code 30 after 3 s; divide error at 0x00012340: divide by zero (no guest handler took it) »; an unhandled
guest exception (`[SEH] unhandled exception`, after which the title goes on) after any blocker but a crash: « no flip;
unhandled guest exception 0xC0000005 at 0x00045678 » (XBA-110, XBA-112). What the renderer met and could not draw
(the status file's `unsupported`: texture formats, points and lines, volume textures, stage modes, state, by name and
batches; [gpu-backend.md](gpu-backend.md#what-is-not-drawn-xba-120)) is kept in the run's summary (`unsupported`) and
named after any blocker but a crash or a launch, the three most used first: « nothing lit on screen; renderer lacks
texture format 0x26 LU_IMAGE_A8CR8CB8Y8 (40 batches), points (12 batches), +1 more ». The device's library check
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
| `needs a fix: <blocker>` | the run met no new code and stopped, or stopped with what a prepare cannot fix: a guest exception nothing handled, a stack switch the runtime could not carry, an untranslated instruction (named in that order); calls to stub kernel routes are named after the blocker |
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
statically needs the targets reached that the analysis did find, which the discovery file does not hold: given an
icall-feedback dump (a runtime built with `RECOMP_ICALL_FEEDBACK`, `tools/recomp/icall_feedback.py`) it is
`found_statically / targets`, else « recall unknown ». `run` adds a `recall` object to a title's line when a run
missed anything, and `report` / `report --matrix` get a Recall column (« recall unknown (3 misses: 3 thread start) »).
Tested on synthetic titles only (`tools/compat/test_heal.py`, and `test_e2e.py::test_the_self_healing_loop`: three
threads found one round at a time, the third refused, four rounds); a real title's numbers are XBA-75.

## Limits

- `responsive` is a heuristic: an animated screen changes on its own. The press times and the status clock start a
  fraction of a second apart.
- `tools/conformance/test.xbe` is a lifter test image with no entry point: it prepares but does not boot. The tool's
  own tests use a stand-in title (`tools/compat/test_compat.py`) and a synthetic XBE of our own
  (`tools/compat/test_e2e.py`: XAPILIB/D3D8/DSOUND in its library table, one `jmp $` function), which the linux CI
  image prepares, builds against the tree and boots through the runner (`XBA_E2E=1`): the profile layers, the
  template and the runtime linked and run together on every PR.
