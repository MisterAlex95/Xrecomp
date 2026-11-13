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
statically needs the targets reached that the analysis did find, which the discovery file does not hold: given an
icall-feedback dump (a runtime built with `RECOMP_ICALL_FEEDBACK`, `tools/recomp/icall_feedback.py`) it is
`found_statically / targets`, else « recall unknown ». `run` adds a `recall` object to a title's line when a run
missed anything, and `report` / `report --matrix` get a Recall column (« recall unknown (3 misses: 3 thread start) »).
Tested on synthetic titles only (`tools/compat/test_heal.py`, and `test_e2e.py::test_the_self_healing_loop`: three
threads found one round at a time, the third refused, four rounds); a real title's numbers are XBA-75.

### Recording hits by default (XBA-138): measured, not done

A default build does not record hits because `RECOMP_ICALL_OBSERVE` sits in every `RECOMP_ICALL*` macro, the hottest
path of the generated code. What it would cost, from `tools/compat/icall_record_bench.c` (the macro's work: trace
ring, global count, range check, direct-map lookup, call), x86-64 container on the shared host, gcc -O2, 40 M
calls, two runs:

| Variant | 1 thread, 64 targets | 1 thread, 4096 targets | 2 threads, 64 targets | 2 threads, 4096 targets |
|---|---|---|---|---|
| no record (today's default) | 2.5 ns | 2.7–2.9 ns | 21–27 ns | 21–25 ns |
| `\|=` on every call (the `RECOMP_ICALL_FEEDBACK` build) | 2.8 ns | 3.7 ns | 41–43 ns | 40–50 ns |
| test-before-set (a store the first time only) | 2.8 ns | 3.6 ns | 15 ns | 19 ns |

One thread pays a load of the 8 MiB seen array per call (+0.3 ns when it stays cached, +0.8 ns over 4096 scattered
targets). With two threads the instrumented build's `|=` dirties a shared cache line on every call and doubles the
cost; test-before-set writes nothing once a target is known and stays within the noise of today's build, whose own
shared `g_icall_count++` dominates. Even at a million indirect calls a second, +1 ns is 0.1 % of a core.

So the cheap variant is: the seen array and `RECOMP_ICALL_OBSERVE` in every build, as test-before-set on resolved
targets only (the per-site cache stays opt-in), dumped next to `discovered.jsonl` at exit and on the crash path,
with an A/B switch, and `recall` reading that dump from the work folder instead of asking for `--icall-dump`. Not
done here: it changes `templates/runtime/recomp_types.h` and `src/kernel/icall_feedback.c` (the runtime), and has
to be measured on a real title first (a frame-time A/B on the Thor). Thread starts need no hot-path hook (the kernel
sees each one).

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
