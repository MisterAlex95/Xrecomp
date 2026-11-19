# XRecomp 0.13.9

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Android

- console_pack keeps the release flags of the code it took over (CLEAN-001) (15baab8c)

### APU

- register masks reaching bit 31 are unsigned (XB-006) (774b8cf4)

### Consoles

- the shared pack loader and SHA-256 in src/platform (CLEAN-001) (ffb28bd8)

### Frame generation

- read GPU pictures back through pack buffers (GC-003) (3ff72333)

### Frame generation read-back

- recreate the buffers after a pause, guard deletes (12e2eccf)

### GameCube

- the pack loader on console_pack (CLEAN-001) (f1421cfe)

### GameCube Prepare

- clean the JNI result text (AND-001) (c50ea804)

### GBA

- present without the extra copy kept for a screenshot (fcdee91f)
- the pack loader on console_pack (CLEAN-001) (431c532b)

### GX

- hash texture contents in four independent lanes (ea1f71cc)
- count the texture bytes hashed per field (62f937d6)
- decode a texture map's registers once per bind (3a8ba43e)
- send a sampler's parameters only when they change (5858ed4f)
- leave out the pixel state a batch does not change (9aaf9654)
- one vertex array per texture coordinate count (371a0bd1)
- report the texture bytes hashed as a 60-field average (ae1935fc)

### Host window

- the shared present takes live picture changes (eab657ba)

### Kernel

- reuse released lazy reservations (audit XB-003) (e52d7d39)

### Kernel DPCs

- a queued guest call is never overwritten (XB-004) (8ea9daa8)

### Kernel timers

- a test entry point for the firing pass (e3cf0e70)
- a periodic timer keeps its schedule (XB-001) (a5362684)
- honour an absolute due time (XB-002) (a3ba87f6)

### Memory layout

- the header says released reservations can be reused (XB-003) (81113972)

### N64

- the pack loader on console_pack (CLEAN-001) (2ba7e4dd)

### N64 RDP

- span accumulators wrap without signed overflow (N64-001) (0cdfac7e)

### PS1

- present without the extra copy kept for a screenshot (d3428744)
- the pack loader on console_pack (CLEAN-001) (58df16ad)

### PS1 GTE

- no left shift of a negative translation (PS1-001) (90850c44)

### Rewind

- document that the spare capture buffer is outside the cap (44e8467e)

### Rtl strings

- no terminator past the caller's buffer (XB-005) (47ceca36)

### Windows

- rename replaces an existing file (WIN-001) (b5264e8a)
- open files in binary mode (WIN-002) (93979841)

### X360

- VdInitializeScalerCommandBuffer writes where the caller says (de56691a)
- the 4 KB physical view starts one page into physical memory (e1ec6a42)
- scratch registers are written back to memory (37e95f3f)
- a wait inside an indirect buffer stops the command processor (959cecab)
- the display interrupt status says the vertical blank occurred (a59aaf7c)
- ExCreateThread's creation flags carry the hardware thread (1085086a)
- the last XAM imports of a network-aware title have an HLE (8076620b)
- predicated packets run only for a selected bin (2d0cb4ab)
- the runtime works in the host's pages, whatever their size (3ec22fbe)

### X360 recompiler

- a jump table is indexed by the value its bound check compared (86a0fa6f)
- leaf functions only a pointer reaches are discovered (e9394a85)
- a jump table without a bound check is read to its end (1ede4a40)
- an address built in two halves is followed down the straight run (a549d9cb)
- a table of addresses in a code section names its leaf functions (6fd93a7d)
- a jump table's bound check may return or branch to the table code (34f3ff21)
- a jump table indexed by a stack slot the bound check loaded (ce7a40fc)
- a jump table without a check takes the bound every path to it passed (a2b348c4)
- a shared bound is shown path by path, across calls (f5a87869)

### X360 renderer

- each sampler keeps its own texture in a multi-texture draw (e52dd0b2)
- DXT colour endpoints have red in the high bits (05cb8b24)
- a resolve that swaps red and blue keeps its host picture (cfb31c33)
- a resolve's host picture reads as its memory does (007ae471)
- a resolve's host picture only stands for a texture read as it was written (ad009d68)
- a texture is read from its fetch constant's first mip level (990590d9)
- a surface that grows takes every attachment off the draw framebuffer (fc399a16)

### X360 shaders

- the pixel parameters are written by constant index (37207832)
- the vertex index survives writes to the rest of r0 (cbcaf21c)
- a pixel shader's inputs are the interpolators it names (d6a7c186)

<!-- release:changes end -->
