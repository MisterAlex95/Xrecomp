# XRecomp 0.18.0

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Docs

- GBA prefetch model, cheaper (XBA-295) (52537460)
- N64 resume points inside a block measured and not taken, what paid instead (XBA-295) (1d8e4121)
- the prefetch-on fuzz test (XBA-295) (7ea9f92e)
- GBA speed 6 (handed instructions from blocks, rc_pf_fetch_n's front) (41769876)
- the N64 frame around lifted code, profiled whole, and what paid (XBA-295) (8550650b)
- N64 profile split under load (rdpq, polling) and the RDP's share (XBA-295) (58da776f)

### GameCube audio

- the 48 kHz mix without a call per frame (c615c33f)

### GBA

- one prefetch call for a run of plain instructions' fetches (XBA-295) (ca2ef0d9)
- rc_pf_fetch_n charges the rest of a run at once once the buffer is empty (XBA-295) (afebd720)
- a segment that is one run checks the next event with its exact cycles (XBA-295) (941347b2)
- literal loads from the function's region join a run (XBA-295) (d3a83e60)
- exact event checks for more segments, a lean rc_pf_fetch_n, refills in one call (XBA-295) (ebf4ecae)
- rc_pf_fetch_n's short path covers fetches at the buffer's head after a gap (XBA-295) (f3b7cfbf)
- prefetch-on fuzzing, and save states with a running buffer of no read cycles refused (XBA-295) (0b14f068)
- instructions handed over in the cartridge run from the predecoded blocks (e777de48)
- rc_pf_fetch_n's two frequent cases in a front without a frame (31f2bacb)

### GX

- unaligned pipe writes inside a 32-byte line take the fast path (c4f03e65)

### Lifter speed 6

- fistp without a libm call, math functions without errno (9fbecab7)
- fist's limits in the round-24 test, at Prepare's flags; numbers in prepare.md (4e51cfc2)

### N64

- n64_run --stats counts the hand-offs to the interpreter (XBA-295) (b12b48d4)
- MFC0 / DMFC0 run in lifted code, through the core's COP0 read (XBA-295) (cfbd3013)
- an inline store tests one range, the MI repeat mode folded into it (XBA-295) (bc0bbf70)
- the AI resampler skips its sums while the DAC holds silence (XBA-295) (0156f16b)
- a dispatcher hit on a line checks only its pages' write counters (XBA-295) (7776dd79)

### OHCI

- advance the frame clock under the list lock (XBA-329) (eee88fda, XBA-327)

### PS1

- instructions handed to the interpreter run in one loop (XBA-295) (0e7a68e9)
- ps1_cpu_step_handed declared in ps1_recomp.c, so packs stay current (XBA-295) (0f9a7965)

### PS1 HLE

- a TCB's registers in two block copies, the EvCB scan straight from RAM (XBA-295) (fed2d1cb)

### PS1 SPU

- idle voices and a silent reverb history cost nothing per sample (XBA-295) (a74759e0)

### Release

- the console speed night makes a minor release (d81b6737, XBA-295, XBA-329)

### usb_ohci_walker frame_race

- skip when the sides never overlap, no total past the owed cap (3592827f)

<!-- release:changes end -->
