# XRecomp 0.19.0

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### docs

- x360 GPU thread, unpaced results from boot and repeats (95b05a8a)

### Release

- the X360 performance night 2 makes a minor release (2e6ad6d3, XBA-345)

### x360

- MmQueryStatistics reads the physical heap's page count instead of scanning 128 Ki page flags (10d67fa6)
- GPU thread stage 2 (WP-22), the command processor on the renderer's thread while the machine runs (9b8d50d5)
- GPU thread stage 2, the scheduler waits for the GPU before skipping time; hold waits for a run; doc (b1551b31)
- GPU thread stage 2, signed back-pressure count; no scheduler wait for the GPU (e2231dd8)
- GPU thread stage 2 off by default (RECOMP_X360_GPU_THREAD=1), awaiting a device check (c74f5719)
- a cheaper cooperative scheduler turn (978e2191)
- lazy resolves by default for Warhammer: Battle March (WP-2) (60f7618e)
- the GPU thread's unpaced runs wait for posted work instead of running ahead (WP-22 stage 2) (29f8d27e)
- a comment within 120 columns (f838f47b)

### x360 homebrew runner

- copy src/input, which x360_pad_map and so x360_run link since WP-25 (ed6c12b1)

### x360 renderer

- 16-bit resolve formats written the direct way, stencil read back four bits at a time (WP-36) (e128dff4)
- packed vertex formats fetched straight from memory, one loop per format and swap mode (WP-36) (22a05e3b)
- 2D-tiled texture rows read 16 bytes of blocks at a time (WP-36) (3e999cc3)
- texture rows decoded with the format chosen once, half floats from a table (WP-36) (b182c52d)
- texture bytes read a dword at a time from memory (WP-36) (a325b7db)
- 8_8_8_8 vertex components from a table; the packed vertex macro on lines of its own (WP-36) (77277625)
- read_stencil's comment says four bits a read-back (WP-36) (b4c358ac)
- resolve rows addressed by tile, a whole tile row of texels at a time (WP-36) (2f763965)
- memory hashed in 8 scalar lanes (WP-36) (cc6f52dd)
- 16-bit resolve rows addressed by tile, whole dwords stored directly (WP-36) (c1df26d1)
- depth resolves stored straight into memory, one loop per swap mode (WP-36) (692b6614)
- direct_rows' inner loop indented, tables made with tiled2d_split (WP-36) (a1d064cf)
- 16-bit and half-float vertex components from tables (WP-36) (ca9e88dc)

### x360 tests

- the homebrew and arm64 scripts build x360_run with x360_pad_map again (f61ffbb9)

### x360 VMX helpers

- host-order lanes and vector ops instead of per-element shifts and masks (1de6c81f)

<!-- release:changes end -->
