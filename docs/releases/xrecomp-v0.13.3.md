# XRecomp 0.13.3

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

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
