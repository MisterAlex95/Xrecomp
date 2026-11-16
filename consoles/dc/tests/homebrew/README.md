# Dreamcast homebrew corpus (XBA-227)

Our own test programs for the Dreamcast spike, so it can be tested headless without a commercial disc, a BIOS or a
flash dump.

| Program | What it exercises | Output |
|---|---|---|
| `hello` | KallistiOS boot (no maple, no CD-ROM), dbgio on the SCIF, a 640x480 RGB565 framebuffer with KOS's minifont, an SH-4 check, a second thread, VBlank interrupts against the TMU timer, `thd_sleep`, the return to the BIOS | `hello.elf`, `hello.bin` |

`hello`'s serial lines starting with `hello: ` are `consoles/dc/tests/hello_serial.txt`, its framebuffer when it
prints `hello: done` is `consoles/dc/tests/hello_fb.png`. `SH-4: ok` means each check gave the value the hardware
gives (bit n of `SH-4: FAIL xxxx` = check n in `hello/main.c`). Two of them record KallistiOS behaviour rather than
the obvious maths: `mat_transform` stores x / w, y / w and 1 / w (so the third component is 1), and KOS 2.1's
`fisin` / `ficos` pass the float's bits to FSCA (the test uses `fsincos` instead).

`hello.bin` is the same program as a raw binary for 0x8C010000 (what a disc's 1ST_READ.BIN holds once unscrambled):
`dc_run` and `dc_tools.recomp` take it as well as the ELF (no symbols: the lifter scans the image for code pointers).

## Build

```sh
tools/consoles/build_homebrew.sh dc [OUTDIR]     # default OUTDIR: build/homebrew -> build/homebrew/dc/
```

The image `tools/docker/dc-homebrew.Dockerfile` is KallistiOS's own toolchain image (`kallistios/dc-kos-toolchain`,
tag `14.2-stable`, pinned by digest; about 430 MB to download, 1.55 GB on disk); `make` runs inside it without
network. Outputs (`.elf`, `.bin`, `SHA256SUMS`) are never committed.

## Toolchain and licences

| Component | Licence |
|---|---|
| sh-elf GCC 14.2 / binutils | GPLv3 (tools only) |
| libgcc (linked) | GPLv3 with the GCC runtime library exception |
| newlib (linked) | BSD-style licences |
| KallistiOS 2.1 (linked: libkallisti, its minifont) | KOS License, "new" BSD (`/opt/toolchains/dc/kos/doc/LICENSE.md`) |

The expected picture holds glyphs of KallistiOS's minifont ("Naomi", Lawrence Sebald, KOS License).
