# XRecomp 0.13.4

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Kernel

- a reservation at the title's address above RAM, by default (4a5e26b9)
- KeBugCheck stops the title instead of returning to it (b7544d1c)

### Static discovery

- a call padded with int3 never returns, and ends the body (b0eac087)

<!-- release:changes end -->
