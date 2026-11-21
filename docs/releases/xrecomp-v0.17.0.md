# XRecomp 0.17.0

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### docs

- RECOMP_X360_NOOP_DRAWS (WP-9) (45b7506c)

### Release

- the X360 performance night makes a minor release (86f005fe)

### x360

- MSAA off by default for Adventure Time (WP-11 item 5) (58b218f5)
- lifted functions call each other from C (WP-16) (ba635f5e)
- perf counters off the per-turn path (WP-31) (d0fc4996)
- a cheap scheduler turn (WP-32) (b1faf615)

### X360

- rectangle lists read back by default, the GPU fourth corner opt-in (590ec0f4)

### x360 dispatcher

- fast path for lifted code first (WP-20) (eb4aede1)

### x360 GPU

- x360_gpu_poll split into a post half and a run half behind an explicit seam (WP-22 stage 1) (95d484ac)
- x360_gpu_drain runs the command processor only on the run half's own thread (WP-22 stage 1) (c1f82365)

### x360 perf

- device counters, a second perf line (WP-19) (62fcaaa8)

### x360 perf line

- CPU time versus waiting, no-op draws, passes, hashed bytes (WP-15) (4914a015)

### x360 Prepare

- packs for 4 KB-page devices leave out view4k_skew (WP-33) (b2af6b06)

### x360 rectangle lists

- fourth corner on the GPU, no read-back (WP-10) (5577a1eb)

### x360 renderer

- skip draws that cannot write (WP-9) (27237e85)
- resolve-copy textures keep their texture object when size and format match (WP-11 item 3) (41e0f646)
- glCheckFramebufferStatus only when a draw FBO attachment changed (WP-11 item 4) (8177386d)
- shader CRCs cached, large resolve copies checked sparsely (WP-12) (aafea02c)
- stale eDRAM rows of the surfaces being bound cleared on the draw FBO (WP-11 item 1) (1d6adf7e)
- resolve clears on the draw FBO when it has the surface attached (WP-11 item 2) (2814d23d)
- invalidate attachments overwritten whole (WP-17, part 1) (c0506fe9)
- resolve clears wait for the next pass that draws there (WP-17, part 2) (7ae4932a)
- no depth attachment for passes that do not use depth (WP-17, part 3) (4d211866)
- sample and show resolve pictures in place (WP-18) (eb2e9ed8)
- sampler objects for texture sampling state (WP-24, item 2) (89567e7e)
- texture entries found by hash, 1024 of them, returned to bind_textures (WP-24, item 1) (f9831799)
- render state set only when it changes (WP-24, item 3) (054e8e4a)
- showing the front buffer's copy in place is opt-in (RECOMP_X360_DIRECT_PRESENT=1) (e79bf685)
- PA_SC_WINDOW_OFFSET moves the window scissor and the resolve rectangle (WP-26) (017d9205)
- the viewport's z scale and offset through the depth range (WP-27) (8065f786)
- a clear through an MSAA view of 1x tiles goes to the 1x surfaces; unused depth untouched by default (WP-28) (b2a36a7f)
- start-up self-test of the rectangle lists' capture (WP-29 step 1) (74cf88b9)
- rectangle lists capture 60 components, not 64 (WP-35 step 1) (304aa5fc)
- indexed rectangle lists below GLES 3.2, one point draw per index (WP-35 step 2) (cfc3e9bf)

### x360 resolves

- write-behind (lazy) resolves, on for Adventure Time (WP-2) (a019c269)

### Other

- x360_run applies the per-title defaults like the app (WP-25) (4b2dcbbf)

<!-- release:changes end -->
