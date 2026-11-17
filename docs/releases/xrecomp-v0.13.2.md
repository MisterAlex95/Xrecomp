# XRecomp 0.13.2

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### CI

- xrecomp-latest no longer waits for linux-arm64; the APK chain takes the heavy-job lock first (XBA-269) (04781f62)

### GameCube tests

- the helper scripts run cross-built binaries under the emulator (1e4a471c)

### Kernel bridge

- NtQueryVirtualMemory ends at the top of user space (e1486c87)

### Kernel park

- a freeze is reached on the read that saw every thread parked (faf84236)

### macOS

- the NDS / PSP / Dreamcast / PS2 players link their core, not only -force_load it (5f23f9df)

### Release

- a release PR picks the next version from commit trailers (4fc731ef)

### Tests

- kernel_park waits for the state it checks instead of sleeping a fixed time (14785cb3)

<!-- release:changes end -->
