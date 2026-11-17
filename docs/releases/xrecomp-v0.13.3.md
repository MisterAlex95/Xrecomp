# XRecomp 0.13.3

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

A fix release.

- **Threads started through a library's CreateThread** are now found by the analysis. A title that hands its thread
  routine to the XDK's CreateThread (which forwards it to the kernel) could lose that routine: the thread ended at
  once and the game later waited for its work for ever. Reported on GitHub (issue 1) as a black screen after the
  title screen in Sonic Heroes. Not yet confirmed on that game: after updating, press **Prepare again** on the title
  (Home says it was prepared by an older version), and play once more if the first run still stops (the run records
  what the analysis missed for the next Prepare).
- The log now says so when a thread's routine is in the title's code but missing from the pack.

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### CI

- the linux image runs pytest on pytest-xdist workers (41b7abf5)

### GitHub/Gitea

- an issue form for game reports (935ea9f9)

### Prepare

- cmake --build honours CMAKE_BUILD_PARALLEL_LEVEL (91a4db19)

### Static discovery

- a thread routine handed to a CreateThread wrapper is a function (55e7cfa7)

### Template

- an unresolved indirect call is logged at 1, 10, 100 ... hits (66a4fdc7)

### Tests

- test_config's fallback check no longer depends on running first (5d4002a0)

### tools.recomp

- the summary goes beside --gen-dir, not into the toolkit (9ec26fef)

<!-- release:changes end -->
