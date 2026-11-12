# Discs with several XBEs

> Part of the [Documentation Index](../INDEX.md)

Some discs start with a launcher image that hands over to another XBE on the same disc (`XLaunchNewImage`: the XDK
writes the path into a contiguous page, stores it in `LaunchDataPage`, then calls `HalReturnToFirmware(2)`). A
recompiled title is one executable per image, so the hand-over is a process exec (XBA-82).

## Desktop (Linux, macOS)

- `tools.prepare` reads the disc's other `.xbe` files that `default.xbe` names in its strings (`tools.triage`'s test),
  prepares each into `<work>/images/<path>/`, and writes `<work>/images.map`, one line per image:
  `<disc path>\t<executable>` (`default.xbe` first). Every image's build bakes the map's path in (`XBA_IMAGE_MAP`,
  read as `RECOMP_IMAGE_MAP`).
- When the title launches a path that is not its own image, the kernel (`xbox_launch_image`, `kernel_reboot.c`) looks
  it up in the map, writes the launch data page where the next start reads it (as a quick reboot does), and execs
  that image's executable in the same process. Not found, or the exec failed: the title exits as before, the launch
  is recorded in the discovery file (`{"t":"launch"}`) and the compat runner names it.
- Tested end to end in CI on a two-image synthetic disc (`tools/compat/test_e2e.py::test_a_launch_of_another_image_hands_over`).

## Not yet

Windows (a `CreateProcess` like the quick reboot's), and Android: one library per image in the pack (format 2), the
shell restarting the activity with the other image, the secondary image prepared on its first launch. Design in Plane
XBA-82.
