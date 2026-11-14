# Discs with several XBEs

> Part of the [Documentation Index](../INDEX.md)

Some discs start with a launcher image that hands over to another XBE on the same disc (`XLaunchNewImage`: the XDK
writes the path into a contiguous page, stores it in `LaunchDataPage`, then calls `HalReturnToFirmware(2)`). A
recompiled title is one executable per image, so the hand-over is a process exec (XBA-82).

## Desktop (Linux, macOS, Windows)

- `tools.prepare` reads the disc's other `.xbe` files that `default.xbe` names in its strings (`tools.triage`'s test;
  from a folder, or straight from a disc image, plain, split or compressed: only those XBEs are copied out, into
  `<work>/disc-xbe/`, XBA-126),
  prepares each into `<work>/images/<path>/`, and writes `<work>/images.map`, one line per image:
  `<disc path>\t<executable>` (`default.xbe` first). Every image's build bakes the map's path in (`XBA_IMAGE_MAP`,
  read as `RECOMP_IMAGE_MAP`).
- When the title launches a path that is not its own image, the kernel (`xbox_launch_image`, `kernel_reboot.c`) looks
  it up in the map, writes the launch data page where the next start reads it (as a quick reboot does), and execs
  that image's executable in the same process (Windows: `CreateProcess`, then this process ends, as the quick reboot
  does; compiled by a MinGW-w64 cross build, [below](#windows), never run). Not found, or the exec failed: the title exits as before, the launch
  is recorded in the discovery file (`{"t":"launch"}`) and the compat runner names it.
- Tested end to end in CI on a two-image synthetic disc (`tools/compat/test_e2e.py::test_a_launch_of_another_image_hands_over`),
  and on the same disc as a split CSO image booted in place (`test_a_multi_image_disc_boots_from_its_image`,
  [disc-images.md](disc-images.md)).

## The running image's name

`XeImageFileName` is `\Device\CdRom0\default.xbe`, or `\Device\CdRom0\<path>` when `RECOMP_IMAGE=<path>` names
another image (`xbox_image_file_name`, at most 47 characters). A secondary image's desktop build bakes it in
(`XBA_IMAGE_NAME`, from `tools.prepare --image-name`), Android's pack loader sets it: a launch back to `default.xbe`
is then a hand-over, and a launch of the running image a quick reboot. A launch path keeps its directory
(`\Device\CdRom0\sub;game.xbe` is `sub/game.xbe`).

## Android

- `pack.json` may carry `"images": {"<disc path>": "<library>", ...}` next to `lib` (default.xbe's), each library in
  the pack folder. Shells from before ignore the key: they run default.xbe as before.
- At start the loader (`pack_loader.c`) writes `<files>/work/<TID>/images.map` (`<disc path>\t<disc path>`) and sets
  `RECOMP_IMAGE_MAP`. When `<work>/image.next` names an image, it is taken (deleted), that image's library is loaded
  instead of `lib`, `RECOMP_IMAGE` is set and the title boots that image's XBE (next to default.xbe). Logcat:
  `PACK_IMAGE <path> <lib>`, or `PACK_IMAGE_MISSING <path> (...)` and default.xbe runs.
- A launch of another image: the kernel finds it in the map, writes the launch data page and calls the image handler
  the shell installs (`xbox_SetImageHandler`): it writes `image.next` and restarts the game process the way a quick
  reboot does (the launcher ends it and starts the game activity again). A quick reboot of a secondary image writes
  `image.next` too, so the same image starts again.
- The on-device Prepare names the other images up front and builds each one on its first launch: `pack.json` gives
  it library `""` until then; its launch writes `<work>/image.pending` and the launcher offers « Prepare <image> »,
  then starts the game again in that image with its launch data
  ([prepare.md « Images »](../android/prepare.md#images-xba-82)).
- The on-device Prepare finds the other images in a game folder, in a disc image source (plain, split or compressed,
  the named XBEs copied out) and in the companion folder of an image read in place, which holds every XBE of the
  image (XBA-126, [disc-images.md](disc-images.md)). Only a lone cached `default.xbe` (a library game prepared before
  it is imported) has no others.

## Windows

The `CreateProcess` hand-over compiles in a MinGW-w64 cross build of the libraries
(`docker build -f tools/docker/windows-mingw.Dockerfile .`); it has not run on Windows, and no CI job builds it yet.
