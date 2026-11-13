# Playing from the Disc Image

> Part of the [Documentation Index](../INDEX.md)

A title's D:\ can be the disc image itself rather than an extracted folder (XBA-126): no second copy of a 4-8 GB
disc, any format the player has (plain, redump layout, FAT32 split parts, CSO / ZSO / CCI; see
[Xbox Disc Images](../formats/disc-image.md#split-and-compressed-images-xba-126)). Extraction stays available
(`tools.prepare --extract`, `RECOMP_DISC_IMAGE=0`) and a folder works as before.

## The runtime: D:\ over the image

`xbox_path_init(game_dir, ...)` mounts `game_dir` when it is a regular file or `fd:N[,M...]`
(`src/kernel/kernel_disc.c`, header `kernel_disc.h`):

- **One read of the tree.** The XDVDFS directory tree is walked once at mount (`xiso_list`, breadth first) into one
  array of nodes whose children are contiguous and sorted ignoring case. A lookup is a binary search per path
  component; it never touches the image. The log line: `[DISC] mounted <spec>: cso split, 2 part(s), 6900.0 MB, 2311
  entries, tree read in 41 ms`.
- **Reads at the file's offset.** A file read is `xiso_source_pread` at the file's offset in the image: a `pread`
  on the image (or on the right split part), or, for a compressed image, the blocks decoded through a small cache
  (4 MB; a streaming read loads a batch of stored bytes ahead in one `pread`, a random read only its own blocks).
  Thread-safe: titles read from several threads.
- **The same answers as the folder.** The paths stay host paths under the game directory
  (`<image>/media/intro.bik`), so the path table, relative opens (RootDirectory), the missing-file report and the
  logs are unchanged; `xbox_disc_rel` tells `kernel_file.c` which paths are the image's. Opens, reads (with and
  without offsets), positions, sizes, attributes (NORMAL / DIRECTORY), directory searches with patterns and the
  search restart after `STATUS_NO_MORE_FILES`, and the missing-file statuses with their Win32 codes (2 vs 3) match the
  folder backend; `tests/kernel_disc` runs one scenario against a folder and against each image form and requires
  equal transcripts. Times are the volume's time stamp.
- **Read-only, as the DVD.** A write open, a create, a delete, a rename or an end-of-file change on the image is
  `STATUS_ACCESS_DENIED` (logged `on the disc image denied (read-only)`). One deliberate difference with the folder:
  opening a file with `FILE_DIRECTORY_FILE` fails (the folder backend opens the file's directory instead, a rule
  meant for the partition device images). Saves, `E:\CACHE`, Z: and the partition images are not on the disc and
  are written as before. The other rules that map to the game directory (`Partition1\` outside UDATA/TDATA/CACHE,
  `Partition2\`, C:, Y:) read the image too and cannot be written there.
- **The DVD device.** With an image mounted, `NtReadFile` on the bare `\Device\CdRom0` handle reads the game
  partition's bytes, and the SCSI pass-through answers READ(10) / READ(12) (2048-byte sectors of the game partition)
  and READ CAPACITY(10) from the image (`xbox_disc_scsi`). The security sector (READ DVD STRUCTURE) and the MODE SENSE
  authentication page keep their synthetic answers (commit 53e58f2).
- **The XBE.** The template's `load_xbe` reads `game/default.xbe` out of the image when `game` is one
  (`xbox_disc_slurp`, which mounts an image met on the path), and the Android pack loader reads the title ID the same
  way. A secondary image's executable (XBA-82) finds its XBE in the image as well.
- **Windows:** no image support (no `src/xiso` there); the Win32 backend reads folders only.
- `RECOMP_DISC_IMAGE=0` refuses images.

## Desktop: prepare and play from the image

`tools.prepare Game.iso` (or `Game.1.cso`, `Game.part1.iso`, ...) reads `default.xbe` out of the image, and the
disc's other XBEs that `default.xbe` names are copied out alone into `<work>/disc-xbe/` (`tools.triage.disc_xbes`)
and prepared (XBA-82). Nothing else is extracted: `build/game` links to the image (one part of a split image is
enough; the runtime finds the others by name from the link's target). `--extract` unpacks into `<work>/game` as
before. Tested end to end: `tools/compat/test_e2e.py::test_a_multi_image_disc_boots_from_its_image` boots a
two-image synthetic disc stored as a split CSO, including the hand-over to the second image.

## Android: the image read in place

docs/android/app-shell.md « Disc images read in place ». In short: the launcher keeps a picked image where it is when
it can reach it at every start (a path with All files access, or documents whose read permission persists), copies
only its XBEs into a companion folder `files/discs/<TID>/` with `disc.json`, and `ShellActivity` hands the runtime
the image's path or `fd:N[,M]` opened from the documents. The fds are opened again by each game process, so a quick
reboot or a hand-over to another image (both restart the process) find the image too.

## Read speed

`tools/xiso/bench_disc.sh` (`kernel_disc_bench` from `tests/kernel_disc`): the same 64 MB tree (half random, half
compressible text) read through the file layer, warm page cache, so it measures the layer, not the storage. x86-64
host (Docker, 2 CPUs), best of three:

| Source | Kind | Sequential 64 KB reads | Random 4 KB reads |
|---|---|---|---|
| folder | extracted | 9597 MB/s | 0.8 µs |
| bench.iso | plain image | 9248 MB/s | 0.7 µs |
| split.1.iso | 3 parts | 9248 MB/s | 0.7 µs |
| bench_v1.cso | CSO v1 (deflate) | 153 MB/s | 29.7 µs |
| bench_v2.cso | CSO v2 (LZ4, the Xbox layout) | 909 MB/s | 5.3 µs |

A plain or split image costs what the folder costs. A compressed one costs its decoder: LZ4 (what the Xbox tools
write) streams far above a DVD drive's 6-8 MB/s even at a phone's lower speed; deflate is slower (a straightforward
inflate) but still over 20 times a DVD. On the device, the storage (SD card, USB) is the limit; see the check list.

## Device check list (Alex)

`adb logcat -s xboxrecomp`. Use only images dumped from your own discs.

1. A redump image (`.iso`, full layout) on the SD card, All files access on: Choose your game › Disc image, pick it.
   Expected: « Reading the disc image in place… », then Home; no « Extracting »; logcat `image <name>: iso, 1
   part(s), read in place`. Manage › Storage: the row « Disc image, read in place » shows a few MB « in the app » and
   the image's size.
2. Play it: logcat `shell: disc image ... -> /storage/...iso` then `[DISC] mounted ...: iso, 1 part(s), ...`. Note
   the time from Play to the first frame, compared with the same game extracted (if you still have the copy).
3. Play 10 minutes through FMVs and level loads: any stutter not seen with the extracted copy? Note where.
4. Quick reboot from the in-game menu: the game comes back (the image opened again).
5. A split image (`Game.1.iso` + `Game.2.iso` on a FAT32 USB stick, OTG): pick either part with All files access
   on: one game; logcat `iso split, 2 part(s)`. Play, load a level near the end of the disc.
6. All files access off: Disc image, pick **both** parts at once (multi-select in the picker): read in place
   (`fd:` in logcat). Pick only one part: the note « choose every part ». Then reboot the phone and play again: the
   permission persisted.
7. A `.cso` made by the Xbox tools (MakeMHz `ciso`, split `.1.cso`/`.2.cso` if over 4 GB) and a `.cci` (Repackinator),
   if you have them: they import and play like the ISO. If either says « not an Xbox disc image » or a read fails,
   send the logcat lines `xiso probe` and `[DISC]`.
8. Take the SD card out (or rename the image) and open the launcher: Home says the source is gone; put it back:
   playable again.
9. Library folder with an image and a split set: one card each; Import keeps them in place (no extraction, no space
   used).
10. A multi-XBE disc (one whose default.xbe launches another XBE, `tools.triage` lists them): prepare and play from
    the image; the launch of the second XBE restarts into it as with a folder.
11. Storage used: Manage before and after importing a 7 GB image in place: the app's storage grows by a few MB only.
