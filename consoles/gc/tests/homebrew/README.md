# GameCube homebrew corpus (XBA-225, XBA-262)

Our own test programs for the GameCube spike, so it can be tested headless without a disc, the IPL or any other
Nintendo code.

| Program | What it exercises | Output |
|---|---|---|
| `hello` | libogc's boot (crt0, the tuxedo kernel's threads and interrupts, SRAM / RTC over EXI, the DSP bootstrap and ARAM DMA), `VIDEO_Init` and `console_init` over the XFB, a PowerPC check, a Gekko paired-single and quantisation check, VI retrace interrupts (`VIDEO_WaitVSync`) | `hello.dol` |
| `gxtri` | GX geometry (XBA-262): a Gouraud triangle, a perspective cube from indexed s16 / RGB565 arrays with back faces culled and the Z buffer, quads crossing in depth, a fan and a strip with per-vertex matrix indices, wide lines and points | `gxtri.dol` |
| `gxtex` | every texture format (gxtexconv-encoded from our pictures at build time by `gxtex/mktex.py`), CI14x2 with an RGB5A3 palette, mipmaps, repeat / mirror / clamp, alpha blending | `gxtex.dol` |
| `gxtev` | twelve TEV setups: konst colours and fractions, bias, registers, scale and subtract, the R8 compare, swap tables, alpha test, three and sixteen stages, a modulated texture | `gxtev.dol` |
| `gxind` | indirect textures: a constant offset, a wave, two stages adding the previous coordinate | `gxind.dol` |
| `gxcopy` | EFB copies to textures in ten formats drawn back, pokes and peeks, the copy's bytes in RAM | `gxcopy.dol` |
| `gxfog` | perspective linear and exponential fog, alpha / additive / subtractive / multiplicative blending, Z GREATER, logic ops | `gxfog.dol` |
| `asnd` | libogc's ASND library and its DSP microcode (XBA-263): five one-second scenes of looped tones (mono 16-bit left, mono 8-bit at 12 kHz right, stereo 16-bit, two voices mixed, silence), timed by VI fields; `report` (magic `XBAASND1`) | `asnd.dol` |

`hello`'s XFB after 120 fields is `consoles/gc/tests/hello_xfb.png`: `PowerPC: ok` means each of rlwinm, rlwimi,
mulhw, mulhwu, divw, cntlzw, addc / adde, srawi's carry, cmpw / cmplw / crxor / mfcr, fmadd, fctiwz, fsel, fdivs and
fnabs gave the value the PowerPC manual gives; `Gekko: ok` the same for ps_add, ps_mul, ps_madd, ps_merge10,
ps_sum0, ps_muls1, ps_neg, ps_cmpo0, ps_sel and psq_l / psq_st (f32, u8 with a load scale, s16 with a store scale and
saturation, W = 1). Otherwise the line says `FAIL` and a mask of the failed checks (bit n = check n in
`hello/main.c`).

The GX programs (XBA-262) share `common/gxdemo.h`, draw the same scene every frame and check pixels themselves by EFB
peeks: a green square at the bottom left when every check passed, a red one and a row of white marks (one per failed
check) otherwise. Their pictures are `consoles/gc/tests/gx_ref/<prog>.png` (`run_gx.sh`, `GX_UPDATE=1` rewrites one).

## Build

```sh
tools/consoles/build_homebrew.sh gc [OUTDIR]     # default OUTDIR: build/homebrew -> build/homebrew/gc/
```

The image `tools/docker/gc-homebrew.Dockerfile` is devkitPro's devkitPPC image (pinned by tag and digest; about
660 MB compressed, 2.6 GB on disk); `make` runs inside it without network. Outputs (`.dol`, `.elf`, `SHA256SUMS`) are
never committed.

## Toolchain and licences

| Component | Licence |
|---|---|
| devkitPPC r51 GCC 16.2 / binutils 2.47 (`devkitpro/devkitppc:20261016`, pinned by digest) | GPLv3 (tools only) |
| newlib (linked) | BSD-style licences |
| libogc 3.2.1 (linked) | zlib-style (`/opt/devkitpro/libogc/LICENSE`; its threading code carries the RTEMS notice there) |
| elf2dol (gamecube-tools) | GPL (a tool only) |

The expected picture holds libogc's console font (same licence as libogc).

`asnd`'s 48 kHz output (`gc_run --wav`) is compared scene by scene with a reference synthesised by
`consoles/gc/tests/audio_check.py` (per channel: each tone's level, the RMS, what is left once the tones are taken out),
ctest `gc_asnd` and `gc_play_asnd` (through gc_runtime and the host audio path). libasnd (BSD-3-Clause, Hermes) is
linked from libogc.
