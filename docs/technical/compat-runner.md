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
| `RECOMP_STATUS` file | flips, picture changes (a hash of a 32x24 grid at each flip), lit, non-silent audio, pad reads, guest threads (`src/kernel/recomp_status.h`) |
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
for 30 s at the end), each with the first untranslated instruction or the last discovery when there is one. Any
blocker but a crash or a launch also names the stub kernel routes the run called, from the discovery file's `stub`
entries: « no flip; calls stub ordinal 136 (KeRemoveQueue) » (XBA-115).

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

## Limits

- `responsive` is a heuristic: an animated screen changes on its own. The press times and the status clock start a
  fraction of a second apart.
- `tools/conformance/test.xbe` is a lifter test image with no entry point: it prepares but does not boot. The tool's
  own tests use a stand-in title (`tools/compat/test_compat.py`) and a synthetic XBE of our own
  (`tools/compat/test_e2e.py`: XAPILIB/D3D8/DSOUND in its library table, one `jmp $` function), which the linux CI
  image prepares, builds against the tree and boots through the runner (`XBA_E2E=1`): the profile layers, the
  template and the runtime linked and run together on every PR.
