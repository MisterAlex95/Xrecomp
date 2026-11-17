# XRecomp 0.13.4

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

A fix release.

- **Titles that reserve their own memory above RAM now start.** A title whose allocator looks for free space with
  VirtualQuery and then reserves exactly there (Yu-Gi-Oh! The Dawn of Destiny, Intrinsic Alchemy engine) never found
  a region big enough: free space was reported page by page. Its memory pool stayed empty and it stayed on a black
  screen before its first frame. Free space is now reported as one region and a reservation at the asked address is
  honoured.
- **Functions after a call that never returns** (`call exit; int3`, `call _CxxThrowException; int3`) are no longer
  swallowed by the function before them. A first run after a fresh Prepare no longer stops on them.
- **KeBugCheck stops the game** with one clear line (and, on Android, a message) instead of returning into it, which
  looped millions of times and then crashed.

After updating, press **Prepare again** on an affected title (Home says it was prepared by an older version).

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Kernel

- a reservation at the title's address above RAM, by default (4a5e26b9)
- KeBugCheck stops the title instead of returning to it (b7544d1c)

### Static discovery

- a call padded with int3 never returns, and ends the body (b0eac087)

<!-- release:changes end -->
