# APU DSP: the GP and the EP

The MCPX APU has three processors: the voice processor (VP: 256 voices, decoding, HRTF, mixing into 32 mixbins),
the global processor (GP) and the encode processor (EP). The GP and EP are Motorola DSP56300 cores. DirectSound,
which is XDK code linked into the title (and so recompiled with it), loads an *effects image* into the GP (the
XDK's standard `dsstdfx` image, or the title's own built with `xgpimage`): I3DL2 reverb, the crosstalk stage for
3D voices, and whatever effects the title's FX sends feed. The EP takes the GP's output and makes the digital
output (AC3 / Dolby interleave) or passes PCM.

This runtime emulates the VP (`src/apu/apu_vp.c`, from xemu) and **neither DSP**. What stands in for them
(XBA-122):

| Stage | What | Where | Default |
|---|---|---|---|
| 1 | What the title asks of the GP/EP, in the log and the status file | `apu_dsp_watch.c` | on (costs nothing measurable) |
| 1 | The DSP handshakes DirectSound waits on, each with a bounded answer | `apu_dsp.c` | on |
| 2 | The mixbins folded to stereo by role | `apu_mix.c` | on (`RECOMP_APU_MIXDOWN=routed`) |
| 3 | A native I3DL2-style reverb fed from the I3DL2 send | `apu_mix.c` | **off** (`RECOMP_APU_REVERB`) |
| 4 | EP: PCM stereo pass-through, whatever EP image is loaded | `apu_dsp.c` | on |

## Why no DSP56300 interpreter

**Licence.** The repository is MIT. The xemu code it ships (the APU core, the VP, `apu_dsp.c`, the NV2A register
files: `NOTICE`) is all under the **LGPL-2.1-or-later**, which lets MIT and proprietary code link against it.
xemu's DSP is not all LGPL. In `hw/xbox/mcpx/apu/dsp/` (checked 2026-11-13):

| xemu file | Licence |
|---|---|
| `gp_ep.c` (GP/EP glue), `dsp_dma.c` | LGPL-2.1-or-later |
| `dsp.c` (DSP core wrapper), `interp/dsp_cpu.c` (the DSP56300 interpreter, "adapted from Hatari DSP M56001 emulation", ARAnyM team, Thomas Huth) | **GPL-2.0-or-later** |

The interpreter and the wrapper around it are GPL, inherited from Hatari. Linking them in would put every title
binary and the Android app under the GPL, which is not consistent with how the repository ships the rest of
xemu's code. **Not imported.** The LGPL glue is of no use without a core.

**Cost.** The GP runs its image every 32 samples (1,500 times a second). Writing our own interpreter from
Motorola's public DSP56300 family manual is possible, but the GP's clock and the standard image's real load are
unknown here (no image to measure: we must not use an XDK one). As an order of magnitude: a switch-dispatch
interpreter costs some 10-20 ns per DSP instruction on a desktop core, so each 1,000 instructions per frame cost
10-20 us per frame, 1.5-3 % of a core. An image that spends 30,000 instructions per frame (a reverb plus the
crosstalk stage could) would take 300-600 us of the 667 us frame on x86-64, more on a phone's audio thread.
xemu itself added a JIT for the DSP (`dsp_jit.c`) for that reason. And an interpreter is only as faithful as the
image it runs, which we cannot test without one.

**So the native route (option b):** observe what the title asks, mix by role, and offer a native reverb behind a
switch. If a GP image ever has to run, the faithful path is our own DSP56300 core written from the manual, with a
JIT in mind, measured on images dumped (`RECOMP_APU_DSP_DUMP`) from Alex's own discs, kept outside the repository.

## Stage 1: what a title asks of the DSPs

`apu_dsp_watch.c` records, on the APU frame thread:

- **GP doorbell commands**: every value the doorbell ack cleared (count, distinct values, the most frequent), and
  doorbells left pending because the GP was held in reset (frames, longest run, stalls: a run past 2,250 frames,
  1.5 s, is logged).
- **The effects image**: GP P memory is hashed (FNV-1a 64 over the used words) every 64 frames (43 ms); a new
  content counts once it held still for two checks (a check landing mid-upload is not an image). Size in words,
  hash, distinct images, X/Y non-zero words. X/Y rewritten under the same image counts as a parameter update
  (DirectSound writes the I3DL2 listener and effect settings there).
- **The EP image** (the canary at P:6 is this runtime's and does not count), and the GPRST/EPRST words.
- **Mixbins**: which bins carried signal (above -100 dBFS) and in how many frames, by class: bed (0-5), XTLK
  (6-9), I3DL2 send (10), FX sends (11-31).

To stderr as `[APU-DSP]` lines, and into the status file (`RECOMP_STATUS`) as the `apu_dsp` object of every line:

```json
"apu_dsp":{"cmds":13,"cmd_kinds":3,"cmd_top":"0x00000101","pending_max":2260,"stalls":1,
           "gp_image":"9cd378fca346977d","gp_words":1500,"gp_images":2,"gp_param_updates":1,"gp_rst":"0x3",
           "ep_image":"6067adc5553666e6","ep_words":1200,"bins":"0x00001487",
           "bed":142,"xtlk":10,"i3dl2":10,"fx":10,"reverb":0,"reverb_bypassed":0}
```

The compat runner (`python3 -m tools.compat`) keeps it in each run's summary, and the report grows a **DSP**
column ("GP image 9cd378fc (1500 words), 13 cmds, sends: reverb+FX, EP image, 1 stall(s)") as soon as one title
reports it. Titles that share the standard image share its hash: grouping by hash separates standard-image titles
from those with their own image.

**What it cannot see.** GP and EP memory is plain RAM in the aperture: `xbox_memory_layout.c` traps only
0x00000-0x2FFFF, because Burnout 3's DirectSound copies DSP memory with `rep movsd` (a host `memcpy`) that
cannot be resumed after a fault. So a title's **reads** of DSP memory (results read back, output levels) are
invisible; only what it wrote is seen. For the same reason the trapped GPRST handler
(`mcpx_apu_dsp_gprst_write`) is reached only by tests: real titles write GPRST into RAM, which the watch reads
(`gp_rst`) but the doorbell ack does not gate on.

`RECOMP_APU_DSP_DUMP=<dir>` writes each distinct GP/EP window (64 KB) for study. It is the title's data: never in
the repository.

## The handshakes

Every wait DirectSound is known to make on a DSP, and what answers it. `tests/apu_dsp_ack`,
`tests/apu_dsp_watch`.

| # | DirectSound waits for | Hardware | Here | Bounded by | Test |
|---|---|---|---|---|---|
| H1 | the GP program to clear the doorbell at GP scratch page 0 + 0x810 | the image clears it | `RECOMP_APU_DSP_ACK=auto` clears it within one frame, wherever the scratch table (GPSADDR) puts the page | one frame | `apu_dsp_ack`, `apu_dsp_watch` H1 |
| H2 | the same, while the GP is held in reset | never answered | not answered, counted per frame, a stall logged at 1.5 s; answered on the first frame the GP runs | the title's own release of the reset | `apu_dsp_ack`, `apu_dsp_watch` H2 |
| H3 | the doorbell before GPSADDR / its first entry is set | (no page to ring) | nothing to clear; answered as soon as the table names a page | the title's own setup | `apu_dsp_ack` |
| H4 | the EP program's canary `0x00CCCCCC` at EP P:6 (otherwise DoWork resets the APU, 10 ms each) | the EP program keeps it | kept every frame, also after the title loads another EP image | one frame | `apu_dsp_watch` H3 |
| H5 | AC3 / Dolby EP image loaded | the EP encodes | output stays the PCM stereo downmix, sample for sample; the EEPROM reports no AC3 (`XC_AUDIO` = 0, `tests/kernel_audio_setting`), so DirectSound should not enable it | n/a | `apu_dsp_watch` H4 |
| H6 | the SSL tick, the front end's idle/trap service | VP/sequencer | `apu_core.c` (XBA-earlier), not DSP | | `apu_irq`, `apu_lifecycle` |
| H7 | results the GP writes back (output levels, effect state) | the image writes them | **not modelled**: the title reads what it last wrote, or zero. Invisible (reads of plain RAM) | a title that spins on such a value would show as a wait-spin (`RECOMP_WAIT_SPIN`) or a hang | none: needs a real title |

## Stage 2: the mix

DirectSound's bin numbering (`DSMIXBIN_*`) is fixed by its API, not by the image: 0-5 the 5.1 bed, 6-9 the
crosstalk (XTLK) front and back pairs carrying HRTF'd 3D voices, 10 the I3DL2 reverb send, 11-30 FX sends 0-19.

| Bin | Routed (default) L / R | `sum` (before) | `two` |
|---|---|---|---|
| 0 front L, 6 XTLK front L | 1 / 0 | 1 / 0 | bin 0 only |
| 1 front R, 7 XTLK front R | 0 / 1 | 0 / 1 | bin 1 only |
| 2 centre | 0.707 / 0.707 | 1 / 0 | - |
| 3 LFE | 0.5 / 0.5 | 0 / 1 | - |
| 4 back L, 8 XTLK back L | 0.707 / 0 | 1 / 0 | - |
| 5 back R, 9 XTLK back R | 0 / 0.707 | 0 / 1 | - |
| 10 I3DL2 send | 0.5 / 0.5 without reverb; 0 / 0 with it (wet only) | 1 / 0 | - |
| 11-31 FX sends | even 1 / 0, odd 0 / 1 (unchanged: the image decides) | same | - |

The bed follows ITU-R BS.775's stereo downmix (centre and surrounds at -3 dB). ITU drops the LFE; here it is
kept at -6 dB, since a voice a title sends only to the LFE would otherwise vanish. No bin is louder than it was
(gl^2 + gr^2 <= 1, checked bin by bin). The VP already applies each voice's volumes and the submix / HRTF headroom
DirectSound programs (`apu_vp.c`, `NV1BA0_PIO_SET_SUBMIX_HEADROOM`); the mixdown adds no gain of its own and clamps
at full scale as before.

Why routed is the default: it fixes the centre (dialogue) and the I3DL2 send landing in the left channel only, and
the LFE in the right only, and the tests show no bin gets louder. `RECOMP_APU_MIXDOWN=sum` is the A/B switch back.

## Stage 3: the native reverb

`RECOMP_APU_REVERB=<preset>` (off by default) runs a feedback delay network (8 lines, Householder feedback, per-line
decay gain from the decay time, a one-pole low-pass per line for the HF decay ratio, two input all-passes for the
diffusion, four early-reflection taps) on the I3DL2 send, driven by the I3DL2 guideline's parameters (room, room
HF, decay time and HF ratio, reflections and their delay, reverb and its delay, diffusion, density, HF reference).
Measured decay times (`tests/apu_downmix`): generic 1.27 s for 1.49, cave 2.92 s for 2.91; room < hallway < cave.

What it is not:

- **Not the title's parameters.** DirectSound computes the I3DL2 listener and writes it into the image's state in
  GP X memory, at offsets the image defines. Without an image there is no layout to read: the preset is fixed for
  the whole run. A cave and a corridor sound the same.
- **Not the standard image's sound.** Its own reverb (and its levels) are unknown here.
- **Custom images**: a title with its own image may use the I3DL2 send differently. The runtime cannot tell a
  standard image from a custom one without a reference hash; `RECOMP_APU_REVERB_IMAGES` restricts the reverb to
  listed hashes (from the library report) and counts the frames it bypassed.

Kept off by default until A/B dumps from real titles say it helps.

**Cost** (one 32-sample frame, `tests/apu_downmix`, RelWithDebInfo):

| Host | Routed mixdown | Mixdown + reverb | Share of one core (1,500 frames/s) |
|---|---|---|---|
| x86-64 (this CI host, under Docker) | 0.83 us | 1.60 us | 0.24 % |
| arm64 under qemu-user on the same host (emulated, ~20x slower: not a phone) | 17.6 us | 36.6 us | 5.5 % |
| AYN Thor (estimate) | ~1 us | ~2-3 us | ~0.5 % of a big core |

The phone estimate: the loop is scalar float work (about 40 multiply-adds and 12 memory reads per sample), which a
Cortex-A7x/X core runs at roughly desktop speed per clock; at 2-3x the x86-64 time it stays under 1 % of a core, 0.5 %
of the frame budget. The device check below measures it.

## Stage 4: the EP

The EP is not emulated: the output is the stereo downmix as PCM whatever the title does with the EP. The EEPROM
reports stereo without AC3/DTS (`XC_AUDIO` = 0), so DirectSound should not load an encoder; if a title loads an EP
image anyway, the watch reports it (`ep_image`), the canary is kept over it and the output is unchanged
(`apu_dsp_watch` H4). There is no AC3 path that could go silent or stall.

## Check list for Alex (Mac, then the Thor)

Titles: Fable 4D5300D1, Burnout 3 4541005B, Black 45410083, KOTOR 4C410003.

1. For each title, a 2-minute run with `RECOMP_STATUS=status.jsonl RECOMP_AUDIO_WAV=routed.wav`, then the same
   with `RECOMP_APU_MIXDOWN=sum` into `sum.wav`. Send the status file's last `apu_dsp` object and the
   `[APU-DSP]` lines of each log (image hash and size, commands, stalls, bins).
2. Listen to both WAVs: dialogue (centre) should now be in both ears, not only the left; nothing louder or clipped
   in `routed.wav`. Note any sound present in `sum.wav` and missing in `routed.wav`.
3. Compare the four titles' `gp_image` hashes: same hash = the standard image. Any title with its own image?
4. `RECOMP_APU_REVERB=cave` on Fable (a cave) and Black or Burnout 3 (a tunnel): reverb audible, no crackle, no
   runaway tail (it must die out within a few seconds of silence). Then `stonecorridor`, `hallway`. Is a fixed preset
   better or worse than none?
5. With the reverb on, `RECOMP_APU_REVERB_IMAGES=<the standard hash from 3>`: titles with another image keep no
   reverb (`reverb_bypassed` > 0 in the status file).
6. Any `pending ... frames` / `stalls` > 0 in a log: which title, when (menu, loading, in game)? That is a title
   waiting on a DSP that cannot answer.
7. On the Thor, the same titles: no new crackle against the build before; the audio thread's cost with and without
   `RECOMP_APU_REVERB=generic` (`RECOMP_APU_TRACE=1` utilisation lines, or simpleperf on the APU thread).
8. Optionally, `RECOMP_APU_DSP_DUMP=<dir outside the repo>` on one title, to keep the standard image for a future
   decode of its parameter layout (which would let the reverb follow the title's I3DL2 settings).
