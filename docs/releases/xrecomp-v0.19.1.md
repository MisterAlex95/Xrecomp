# XRecomp 0.19.1

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### GC

- address translation's common cases inline into gc_load / gc_store (XBA-295) (aabaa680)
- the dispatcher's common case inline, the probe walk out of line (a0fd2c03)
- silent AI spans mix to silence in one go (555616c4)
- gc_store's common case keeps nothing across a call; pipe shorts and bytes at once (2858f3a6)

### x360

- import calls find their thunk by a direct table, not a binary search (8fcb64fe)
- count idle sweeps on the perf line; cheaper per-turn polls and sweep (b68111a1)
- sync / lwsync / eieio are a compiler barrier when guest threads share one host thread (d76123ff)

### X360 depth resolves

- depth and stencil packed on the GPU, one read-back (WP-38) (505b5cbc)
- packed path self-checked against the split one, flush first (e9304b24)
- resolve_write checks the packed words; RECOMP_X360_DEPTH_RESOLVE documented (4ca501a6)
- the self-check needs its stencil pattern back; stex cleared whole (cd64a4f1)

### x360 VMX

- vperm, the halfword packs and the byte / halfword merges on NEON / SSE2 (05a6a41f)

<!-- release:changes end -->
