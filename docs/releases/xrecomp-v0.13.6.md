# XRecomp 0.13.6

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Android

- Xbox 360 titles in the app with a desktop-made pack (XBA-280) (340722ef)
- Xbox 360 games without a pack go back to the launcher with a message (XBA-280) (7e94f5b7)
- the Xbox 360 Home on 4:3 and 1:1 screens: the Key file pill folds to its icon (XBA-282) (c76292ac)

### Android packs

- lock-prefixed instructions inline, no outline atomics (XBA-277) (129a5811)

### App

- GameCube Controls screen with its own buttons and sticks (XBA-274) (5dd34827)
- GameCube internal resolution, Xbox rewind per game (XBA-274) (922c23a2, XBA-249)
- every hotkey in an Xbox title's Controls (XBA-274) (6d755b81, XBA-249)
- a GameCube on-screen pad of its own (XBA-274) (c12ce20d)
- download GameCube cover art too (XBA-274) (bdaae7b4)
- Rewind's default row says Auto, the Xbox 360 stays off (XBA-278) (82efdbc5)

### App, docs

- Xbox rewind's memory limit said; matrix cells proven (XBA-274) (89fd90c9)

### bench

- a clear error for a layout that was not built (XBA-273) (25965095)
- w_lock, lock-prefixed read-modify-writes on shared words (XBA-277) (8629a8a4)
- qprof, executed instructions and a flat profile of a whole program under qemu-user (XBA-277) (fdc49304)

### Docs

- feature matrix, every console on every host (XBA-274) (b26c8cdf)
- feature matrix, the gaps filled so far and the reasons for the rest (XBA-274) (0333e5a2)
- lifted code speed in a pack, measured (XBA-273) (abfd050e)
- recomp_play in the host layer's map (XBA-274) (85844a01)
- the Xbox 360 section of consoles/README.md for XBA-270 (f8d0edc5)
- indirect calls and locked instructions in a pack, measured (XBA-277) (3a04fd39)
- consoles/README.md's Xbox 360 section brought up to date (XBA-270) (b1c1a67f)
- locked instructions as fetch-and-ops, measured (XBA-277) (6a954159)
- GBA / PS1 / N64 packs, where a frame goes and what XBA-277 changed (60049f04)
- every console pack shows stale after XBA-277 (the toolkit digest covers tools/ and the headers) (f36a7d9d)
- consoles/README.md -- the Xbox 360 shader translator and first renderer (XBA-270) (87f6ef24)
- consoles/README.md -- wrap two Xbox 360 lines (XBA-270) (e8d94e29)
- consoles/README.md -- the Xbox 360 renderer as it stands, --audio, the new tests (XBA-270) (faf97c78)
- consoles/README.md -- wrap a line (XBA-270) (f55312f1)
- GBA / PS1 / N64 / GameCube speed 3, where a frame goes now (XBA-279) (e55a7eab)
- Xbox 360 player, eDRAM model, XMA, the app (XBA-280) (38bc4b8d)
- Xbox 360 runs go on past missed code; Prepare cost of a large title (XBA-283) (11411eb1)
- Xbox 360 files by address, reservations, host threads (XBA-283) (55294869)
- GBA speed 4 (XBA-281) (5cc29878)
- the Xbox 360 is prepared on the device, no retail title run yet (4b31954c)

### Every game window

- F11 / Alt+Enter fullscreen, F12 screenshot (XBA-278) (5456bb8d)

### Frame generation for GPU pictures

- the GameCube's GX path (XBA-274) (3a6f7e2d)

### GameCube

- the fetch fast path with libogc's IBATs, write-gather stores straight to the FIFO (XBA-279) (db61d2b3)
- the last BAT translation of each kind is remembered for its 128 KB block (XBA-279) (e35591e8)

### GameCube sound

- silence is resampled for free (XBA-279) (3671de63)

### GBA

- the prefetch buffer's opcode fetch at the head without the full model (XBA-277) (53f8f378)
- code the game copies to IWRAM is lifted, checked against its bytes at every call (XBA-279) (048bf5dd)
- the cartridge's discovery is as before with IWRAM code around (XBA-279) (7d79d5ef)
- lifted code's RAM stores no longer leave the interpreter's RAM blocks stale (XBA-281) (bb346ad1)
- instructions handed to the interpreter run in one loop (XBA-281) (0b500de6)
- segment starts are resume points, MRS SPSR and MSR are lifted (XBA-281) (ff1476a1)

### GC

- the window finds GX's XFB copy when the VI starts a line into it (28b99529)
- the inline MEM1 path under the later SDKs' split BATs (22c328f5)

### GC boot

- the IPL's default rfi handlers at 0x300 / 0x800 / 0xC00 (efb96c39)

### GC packs

- one code span per lifted function; a stale span is not rehashed (b943c161)
- a polled flag in RAM is an idle loop (52cddccb)

### GX

- vertex, index and uniform uploads stream through rings (867b8603)
- streamed buffers are mapped without invalidating the range (2f5dc00e)

### Host

- the frame loop's controls shared by every console runtime (XBA-274) (8c54bc0a)

### host_window

- no GPU read-back once the frame generation pacer fell back (XBA-274) (90a5aef4)

### Indirect calls

- the flat table's window is the code range, a hit is called at once (XBA-277) (d9495d44)

### Kernel heap

- a request takes only what it needs of a freed block (29b15212)

### Licences

- Xenia's BSD text travels with the app; the matrix's rewind row (891f9764, XBA-280)

### Lifter

- 32-bit locked add / sub / inc / dec / or / and / xor / not as one fetch-and-op (XBA-277) (0b3738b3)

### macOS launcher

- GBA, PS1, N64 and GameCube games listed, prepared and played (XBA-278) (9516a57b)
- a console game's app opens on its game, Play reads only its file (XBA-278) (f6bd9241)

### N64

- a run-time bench of our own, and one compare per inline store (XBA-277) (c060bd12)
- the inline stores' range test without the store limit (XBA-277) (d192f973)
- lifted code compiles at -O2: the big functions' loops keep the runtime's values in registers (XBA-279) (623eab4a)

### NOTICE

- the Xbox 360 references (Xenia, XenonRecomp, MS-PATCH) for XBA-270 (0bb32071)
- Xenia's tiled addressing credited as derived code, its licence in LICENSES (XBA-270) (7e32b49f)

### Prepare on the device for the Xbox 360

- XEX or disc + the player's key file, shaders translated at Prepare time (XBA-282) (845e732c)

### Prepare test

- a GBA pack's table says ABI 5 (XBA-279) (e97f56d6)

### PS1

- the HLE BIOS reads and writes guest words in one go (XBA-277) (d62e197d)
- the HLE kernel's exception path runs natively, exactly as the interpreter (XBA-279) (d8b22a53)
- the HLE handler's words are compared again only after a write into their pages (XBA-279) (79a1e43b)
- the native kernel walk only starts in sequence (XBA-279) (cb3253db)

### PS1 rewind cap test

- a cap that still drops captures now the coding buffer is small (XBA-278) (996a4301)

### PS1 SPU

- an idle voice costs a test per sample (XBA-277) (36bb002e)

### Rewind on for the N64 and the GameCube

- captures that fit, keep their layout and pay their way (XBA-278) (6d434340)

### TLS_SLOT_APP bind keeps x30 on the stack

- leaves save no link register (XBA-273) (537076d5)

### Windows

- the GBA, PS1, N64 and GameCube players, console games in xrecomp play (XBA-278) (c6440ccf)

### X360

- LZX (normal) XEX2 decompression with independent test vectors (XBA-270) (0dc955db)
- MSVC save/restore helpers, bctr jump tables, resume-point table (XBA-270) (335888a1)
- the rest of VMX / VMX128 lifted, non-Java mode (XBA-270) (958ce63d)
- x360_runtime and x360_run reach main() of a synthetic title (XBA-270) (bd2c3b27)
- a C test title built with devkitPPC, arm64 run under qemu (XBA-270) (cc9b7650)
- the title's files: disc image or directory, data directory (XBA-270) (d5cc6da0)
- the homebrew job runs the Xbox 360 test titles (XBA-270) (26b972c8)
- HLE checked against Xenia's kernel, a direct HLE test, fault reports (XBA-270) (86d96887)
- helper shapes against XenonRecomp's patterns, HLE coverage of a title's imports (XBA-270) (85801b4f)
- a first slice of xam.xex and more kernel exports (XBA-270) (de6ffe8f)
- the Xenos command processor (PM4 decode / trace) and the Vd* calls (XBA-270) (1bd62ad8, XBA-229)
- one reused interrupt context, the time stamp bundle, kept shader words, strict imports (XBA-270) (76ba8e59)
- the discovery loop tested: ctest x360_missed (XBA-270) (b248aeb7)
- python3 -m x360_tools.bringup, one command from a dump to a recompiled run (XBA-270) (a18e26e5)
- Xenos shader microcode decode / disassembly, shader dumps, PM4 opcode counts (XBA-270) (57d4ff3e)
- save states (XBA-270) (bae6db8e)
- the kernel's crypto exports, KeSetAffinityThread's CPU byte (XBA-270) (0d5138a6)
- instruction-level differential test of the lifter's integer and FP (XBA-270) (dd93227c)
- titles run in 32-bit mode: CR0 and XER[CA] from the low word, CTR's low word (XBA-270) (1beba715)
- mtmsrd lifted; a thread with MSR[EE] clear is not preempted (XBA-270) (902d5dac)
- the virtual clock and XeCryptRandom in save states, a virtual time limit (XBA-270) (9a3645ea)
- vupkhsb128 / vupklsb128 with VA = 0x60 are the halfword unpacks (XBA-270) (07e44c09)
- the rest of a shipped title's imports: strings, I/O stubs, audio driver, XMA, content, UI, profile, net (XBA-270) (7c72f977)
- differential test of loads, stores, CR logic and CTR branches (XBA-270) (b3111e1c)
- ctest x360_bringup, an explicit x360_run for test_bringup, Mesa in NOTICE (XBA-270) (fc311fcc)
- run_hello_arm64.sh also runs x360_hle_test and xenos_cp_test under qemu (XBA-270) (75e909de)
- controllers (set by the host or a timed script) and cheats' memory (XBA-270) (9f459de8)
- pads in their own save-state section, covered by x360_hle and x360_state (XBA-270) (dfadcbc1)
- Xenos microcode to GLSL ES 3.00, checked on Mesa llvmpipe (XBA-270) (e49197bc, XBA-229)
- bringup dumps the title's shaders and translates them to GLSL ES (XBA-270) (caaddb98)
- shader translator control flow (loops, calls, jumps) and relative addressing (XBA-270) (af2050f3)
- shader translator cube, cube / offset / LOD / gradient texture fetches (XBA-270) (efe48cba)
- shader constants in std140 blocks, pixel shaders run on Mesa, translator test in CI (XBA-270) (1743b5c5)
- first pixels -- a GLES 3.0 renderer for the command processor's draws (XBA-270) (39d82d43)
- renderer render state -- blending, depth / stencil, culling, scissor, colour mask, quad lists (XBA-270) (fc539508)
- renderer textures -- linear and tiled 2D, 8 / 16 / 32-bit and DXT formats (XBA-270) (75bca206)
- renderer resolves -- render target copies to tiled textures, eDRAM clears (XBA-270) (6b2ab48d)
- renderer rectangle lists through transform feedback (XBA-270) (077dd10f)
- bringup ends with a run that draws with the translated shaders (XBA-270) (9b78aa89)
- the alpha test, in the translated pixel shaders (XBA-270) (92718554)
- renderer vertex formats -- packed, 16-bit, half-float and 32-bit integer (XBA-270) (60249e17)
- pixel shader parameters (PsParamGen: position and face) (XBA-270) (ee6a9031)
- mini vertex fetches inherit the fetch constant and stride of the full fetch (XBA-270) (a6403d24)
- renderer index offset and primitive restart (XBA-270) (767268d4)
- --audio FILE.wav, the frames titles submit to the render driver (XBA-270) (a154d59e)
- four render targets (oC0-oC3), resolves from any of them (XBA-270) (2a7b8c40)
- renderer 3D and cube textures; linear rows follow the guest layout (XBA-270) (d6cda4d7)
- frames written only up to --frame-limit (default 30); arm64 script builds the renderer (XBA-270) (0817ca10)
- frames from the front buffer VdSwap names; renderer tested through the machine (XBA-270) (91eabaf0)
- the guest viewport in the vertex shader -- screen-space draws, window offset, half pixel (XBA-270) (31a92d90)
- RB_COLOR_CLEAR read as the raw eDRAM value (X in the low byte) (XBA-270) (f16908e4)
- PPM frames tested, renderer on arm64, exp_adjust without UB, Xenia's licence next to its code (XBA-270) (0f8c6795)
- wrap a comment line (XBA-270) (11b7c865)
- eDRAM model, float / 2_10_10_10 targets, resolve and texture formats, mips, computed-index vertex fetch, internal resolution (XBA-280) (7cf739d4)
- XMA contexts and the APU registers, the audio mix to the host, vibration (XBA-280) (2419e9c4)
- x360_runtime and the windowed x360_play on the host layer (XBA-280) (7c486a00)
- bringup --app-pack writes an app pack; the arm64 script builds x360_xma.c (XBA-280) (978dceb5)
- windowed present tested under Xvfb; rewind off by default; deferred renderer reset; shader cache stamp (XBA-280) (1c62996c)
- depth resolves to 24_8 (XBA-280) (ad21f8b2)
- structured exceptions in lifted code: RtlRaiseException dispatch, RtlUnwind, __C_specific_handler (XBA-282) (32051ca5)
- memory export in the shader translator (GLSL ES 3.10, a storage buffer log) (XBA-282) (6a9a304e)
- memory export in the renderer, behind a GLES 3.1 capability check, with a clean fallback (XBA-282) (a9564e8d)
- the shader translations follow the toolkit; memory export retried for vertex shaders only; docs (XBA-282) (55bf7168)
- runs go on past missed code: an interpreter fallback records every miss in one run (XBA-283) (07b797bb)
- the interpreter runs VMX / VMX128 too, generated from the lifter; the self-healing loop test (XBA-283) (55aab952)
- interpreted code counts once in the virtual clock; a loaded state resumes interpreted threads without a new miss (XBA-283) (7e04055f)
- generated C split by size (1 MB a file) for Prepare's clang jobs; the large-title benchmark (XBA-283) (1de0d5d9)
- generated files by guest address (64 KB of code each) so a second Prepare recompiles only what changed; misses outside the image are not recorded (XBA-283) (e43ca047)
- real reservations: stwcx. / stdcx. are a compare-and-swap against what lwarx / ldarx read (XBA-283) (89768fa7)
- guest threads on host threads behind a switch (x360_run --host-threads, experimental) (XBA-283) (3369b993)
- sync / lwsync / eieio are a host fence in lifted and interpreted code (XBA-283) (c02ce2de)
- host threads yield the lock when an interpreted slice ends; recomp clears an earlier lift's files (XBA-283) (8ca3795e)
- differential test of the generated vector interpreter against lifted code (XBA-283) (f84963aa)
- random-word differential test of the integer interpreter against lifted code (XBA-284) (e03c979c)
- OE forms set OV / SO in lifted and interpreted code (XBA-284) (938061d9)
- the resume table in parts past 32,768 resume points, one per MB of code (XBA-284) (3e78002a)
- test_interp_int accepts either NaN only for fma forms with NaNs in both frA and frC (XBA-284) (efca3b70)

### X360 tests

- the renderer tests run their binaries under the cross build's emulator (080742ff, XBA-280)

### x87 precision control 24

- range test on the bits (XBA-273) (b73b6ec2)

### Xbox

- the desktop keys' hotkeys, as on the other consoles (XBA-274) (7492ddb9)

### Xbox 360 parity in the app

- internal resolution, texture filter, cover art, save data export, cheats shape (XBA-282) (b9978dd1)

### xiso

- probe the Xbox 360 XGD3 game partition base 0x02080000 (XBA-270) (8034be75)
- also probe Xenia's 0xFB20 / 0x20600 Xbox 360 offsets; NOTICE credits ucode.h and the disc offsets (XBA-270) (59a6a43c)

### xrecomp

- GameCube games on the desktop (XBA-274) (fec021fc)
- RVZ / WIA GameCube images read once, gc_run found like the players (XBA-274) (ee5b6b6f)

### Other

- Lifted code reaches g_cpu through bionic's TLS_SLOT_APP on Android arm64 (XBA-273) (9cd0dc71)
- Tests for the TLS_SLOT_APP guest CPU (XBA-273) (d1e18114)
- x87 round to nearest with roundeven on arm64 (XBA-273) (9921100d)

<!-- release:changes end -->
