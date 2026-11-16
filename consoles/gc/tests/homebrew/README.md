# GameCube homebrew corpus (XBA-225)

Our own test programs for the GameCube spike, so it can be tested headless without a disc, the IPL or any other
Nintendo code.

| Program | What it exercises | Output |
|---|---|---|
| `hello` | libogc's boot (crt0, the tuxedo kernel's threads and interrupts, SRAM / RTC over EXI, the DSP bootstrap and ARAM DMA), `VIDEO_Init` and `console_init` over the XFB, a PowerPC check, a Gekko paired-single and quantisation check, VI retrace interrupts (`VIDEO_WaitVSync`) | `hello.dol` |

`hello`'s XFB after 120 fields is `consoles/gc/tests/hello_xfb.png`: `PowerPC: ok` means each of rlwinm, rlwimi,
mulhw, mulhwu, divw, cntlzw, addc / adde, srawi's carry, cmpw / cmplw / crxor / mfcr, fmadd, fctiwz, fsel, fdivs and
fnabs gave the value the PowerPC manual gives; `Gekko: ok` the same for ps_add, ps_mul, ps_madd, ps_merge10,
ps_sum0, ps_muls1, ps_neg, ps_cmpo0, ps_sel and psq_l / psq_st (f32, u8 with a load scale, s16 with a store scale and
saturation, W = 1). Otherwise the line says `FAIL` and a mask of the failed checks (bit n = check n in
`hello/main.c`).

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
