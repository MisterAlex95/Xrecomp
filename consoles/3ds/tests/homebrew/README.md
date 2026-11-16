# 3DS homebrew corpus (XBA-228)

Our own test programs for the 3DS spike, so it can be tested headless without a commercial title, system files or
keys.

| Program | What it exercises | Output |
|---|---|---|
| `hello` | libctru's start-up (heaps from the resource limits, srv, APT's wake-up, HID, FS, GSP's interrupt thread), its console on the top screen, an ARMv6K + VFPv2 check, the usual main loop for 30 frames | `hello.3dsx` |

`hello`'s top screen after 60 frames is `consoles/3ds/tests/hello_top.png`: `ARMv6K + VFPv2: ok` means each of REV /
REV16 / REVSH, SXTB / UXTB (rotated) / SXTH / UXTAH, LDREX / STREX (and a STREX failing after CLREX), UADD8 and SEL
through the GE flags, USAT / SSAT, PKHBT, a single-precision multiply-add, a double divide, VSQRT, conversions and a
compare gave the value the ARM ARM gives; otherwise `ARMv6K: FAIL` and a mask of the failed checks (bit n = check n
in `hello/main.c`). `done (keys 00000000)`: no key was held (HID's shared memory stays zero).

## Build

```sh
tools/consoles/build_homebrew.sh 3ds [OUTDIR]     # default OUTDIR: build/homebrew -> build/homebrew/3ds/
```

The image `tools/docker/3ds-homebrew.Dockerfile` is the GBA and NDS corpora's devkitARM image (same tag and digest):
it already holds libctru and 3dstools. `make` runs inside it without network. Outputs (`.3dsx`, `.elf`, `SHA256SUMS`)
are never committed. The build is reproducible: the same `.3dsx` bytes on every run.

## Toolchain and licences

| Component | Licence |
|---|---|
| devkitARM GCC / binutils (`devkitpro/devkitarm:20260610`, pinned by digest) | GPLv3 (tools only) |
| newlib (linked) | BSD-style licences |
| libctru 2.7.0 (linked) | zlib |
| 3dsxtool (3dstools) | GPLv3 (a tool only) |

The expected picture holds libctru's default console font (zlib).
