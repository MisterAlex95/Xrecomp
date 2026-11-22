# XRecomp 0.19.1

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### App

- a queued Prepare is not reported as a failed one (85517597)
- a Prepare queued behind another title still waits on the Preparing screen (57521ca2)

### disasm

- range and section lookups by bisection; recomp: lite prefilter (a7954e51)

### Docs

- N64 RSP profile, the wait-loop skip, threaded dispatch and SSE2 VU, measured (e30be508)
- game performance reports (app-shell, profiling, host layer), the privacy text mentions frame rates (7f500672)
- x360 GPR loads and stores at their floor, measured (9aebf728)

### docs/android/prepare.md

- object cache keys, plain.json, Python stage timings (ff3cbba6)
- early compiles running at the lift's end (a07163f0)
- overlap measurements, lifter peaks, the warm cache (714d0039)

### Game menu

- Benchmark, 60 s from the newest save state (or from boot), its result sent only on Send (d104b045)

### Game performance

- a run summary from the perf counters, sent as a « game » transaction (c32a884f)
- one summary per game process, its key noted at the start (03b90369)

### GC

- address translation's common cases inline into gc_load / gc_store (XBA-295) (aabaa680)
- the dispatcher's common case inline, the probe walk out of line (a0fd2c03)
- silent AI spans mix to silence in one go (555616c4)
- gc_store's common case keeps nothing across a call; pipe shorts and bytes at once (2858f3a6)

### GX

- the pipe's common write in line in gc_store; commands parsed in place, TLUT loads at once (4020f7d5)
- each VAT's vertex format decoded once, read per attribute present (b4ca6315)

### N64 RSP

- loops that only wait run their iterations at once (db60b6b2)
- threaded dispatch, no per-instruction r[0] or single-step checks (6a1bc621)
- SSE2 vector unit on x86-64 (b64e805f)

### Network library

- take .torrent files up to 64 MB, and show why one is refused in a sheet (7acb252c)

### Prepare

- compile the lift's finished files while the lifter runs (2d3d58ab)

### Prepare object cache

- keys survive an app update (bea2efd0)

### Prepare overlap

- pack.json early / lift_peak_mb, docs (516ddffe, XBA-341)
- do not drain the early compiles before compile_all (33d7b985)
- no early compile for what the object cache has (5ffa3eec)
- keys.json keeps the digests of the last plan (df20f7ab)

### tools.recomp --split-bytes

- write the chunks as they fill up (0950fb93)

### x360

- import calls find their thunk by a direct table, not a binary search (8fcb64fe)
- count idle sweeps on the perf line; cheaper per-turn polls and sweep (b68111a1)
- sync / lwsync / eieio are a compiler barrier when guest threads share one host thread (d76123ff)
- lifted GPR loads and stores without the NULL test, the base in a prologue local, the index opaque (2314fe7e)
- guest memory is separate storage from the context and the machine (clang); xe_ptr's index opaque too (de49935d)

### X360 depth resolves

- depth and stencil packed on the GPU, one read-back (WP-38) (505b5cbc)
- packed path self-checked against the split one, flush first (e9304b24)
- resolve_write checks the packed words; RECOMP_X360_DEPTH_RESOLVE documented (4ca501a6)
- the self-check needs its stencil pattern back; stex cleared whole (cd64a4f1)

### x360 identify

- decrypt in parts, keep the plain XEX across Prepares (23a08194)

### x360 lift

- leaders found once, direct code lookups, lazy helper scan (9a0ce049)

### x360 VMX

- vperm, the halfword packs and the byte / halfword merges on NEON / SSE2 (05a6a41f)
- float NaNs by the AltiVec rule, independent of the compiler and -O level (469786f3)
- the float helpers on NEON / SSE2, the NaN fix-up inline (298e81a6)

### x360_mirror

- the vector case keeps vectors in locals and stores around a load, so it fails without the opaque index (a13061f4)

### x360_vmx_ref

- the float digest is checked, not only printed (e324ea55)

### Other

- Host paths up to PATH_MAX on POSIX, refused rather than cut when too long (76a620a7)

<!-- release:changes end -->
