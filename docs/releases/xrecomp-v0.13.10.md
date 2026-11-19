# XRecomp 0.13.10

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

A crashed game's log can be sent from the device, and Xbox titles run missed code through an interpreter instead of
stopping on it.

- **Share the last game's log** (Settings › Advanced). The in-game « Share log » reads the game's own process, so a
  crash took the log with it. The launcher now writes what the game's last run logged, how its process ended (native
  crash, Java exception, not responding, killed…) and the crash's backtrace into one text file, ready to send. It holds
  no game data.
- **Xbox: an x86 interpreter behind the lifted code** (XBA-303). Code the Prepare missed is interpreted (dispatch
  misses, SEH, save states), and one self-healing round collects every miss.
- **Xbox rendering on GLES**: fixed function on the GPU by default, more texture stage modes, vertex data streamed
  through rings, shader programs compiled off the draw path, with a « Shaders 12/304 » badge while they compile.
- **XMV movies** are read from a disc image in place.

**Known problems**

- Warhammer: Battle March (Xbox 360) crashes on the Thor right after the Bandai logo. The cause is not known yet: this
  release is the one to send its log from (play until the crash, then Settings › Advanced › Share the last game's log).

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Executor

- back on the guest's core only for the CPU rasteriser's pool (XBA-305) (0e31d0b2)

### GC cbuild

- -I src for video/gpu_ring.h (XBA-302 build fix) (2c8dcabe)

### GC homebrew job

- copy src/video/gpu_ring.h into the container (XBA-302 CI fix) (59c352e8)

### Interpreter

- x86 decoder and integer core, checked against the CPU (XBA-303) (33c2dcdf)
- x87, SSE, MMX checked; conformance vectors through it (XBA-303) (f090f1e6)
- run missed code from the dispatch miss paths, SEH and save states (XBA-303) (e4c3ef5d, XBA-112, XBA-132)
- one self-healing round finds every miss; docs (XBA-303) (fa511b1d)
- SEH resumes into interpreted frames, from raises, #DE and faults (XBA-303) (d5e1b242)

### Interpreter links

- registered pointers in place of weak references (macOS link fix) (af16451e, XBA-303)

### Launcher

- « Share the last game's log » after a game crashed (308c273c)

### Lifted code held to the CPU

- goldens, differential tests, float compare flags (XBA-317) (c2c3fb03)

### Lifter

- stack slots without volatile and without the g_cpu barrier (XBA-307) (f70e2f1b, XBA-304)
- every consumer of a defined flag answered from its setter (XBA-317) (1bac06d2, XBA-303)

### nv2a

- fixed function on the GPU by default, with its gaps filled (XBA-306) (b70ffb1a)
- texture stage modes 8 and up, bump-env on the CPU, YUV 0x26 / 0x34 (XBA-311) (9731969a)
- lazy visibility reports by default on GLES (XBA-313) (87e12b13)

### nv2a GL

- stream per-draw vertex and index uploads through rings (XBA-302) (4b6363ee)
- compile programs off the draw path, a program list compiled ahead (e8e57925, XBA-310, XBA-314)
- per-program uniform shadow, uniform and hashing counters (XBA-312) (bc3d33ea)

### nv2a_gl_coverage

- reset the vertex arrays before case 12, count batches (XBA-315) (340a8e16)

### prepare

- inline LSE atomics in device-prepared Xbox packs (XBA-313) (9fb5bf11, XBA-277)

### PS1 homebrew job

- ps1_present left to desktop runs (CI fix) (3d517137)

### Rewind

- the capture spacing comes back down from one above the setting (2be6d80c)

### Shader badge

- "Shaders 12/304" at the top left of the picture while shaders compile (8c0c4e50, XBA-314)

### stack switch

- the interpreter is optional, as in xbox_seh.c (XBA-303) (4189179f)

### x360_hle_table

- run x360_hle_test under the cross emulator (CI fix) (0527ebb9)

### Xbox runtime

- an ADPF session for the frame, floating threads off the guest's core (XBA-305) (2a91cc1c)

### XMV

- movies read from a disc image in place; never bind a Play-only GetNextFrame (XBA-309) (f3af7a8b)

### XMV docs

- the Play-only guard's evidence as observed, its cause as presumed (XBA-309) (8eb71b7a)

### XMV hook

- write GetNextFrame's timestamp as wide as the XDK build has it (XBA-316) (27a09f3b)

### Zero-copy present

- the window draws the GL back end's frame texture (XBA-308) (9f46380d)

### Other

- Record indirect-call hits in every build, for tools.compat recall (XBA-304) (998a62b7)
- Interpreter reference weak in recomp_status (XBA-303 CI fix) (9ebbdada)

<!-- release:changes end -->
