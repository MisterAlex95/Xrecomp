# XRecomp 0.15.0

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

The network library gets easier to browse, and adding more torrents works.

- **Versions stacked.** The versions of one game in a torrent (other regions, languages, discs) are one card. The
  version shown first is one that downloads, else the one that suits your language. Its sheet shows « This version »
  and lists the others. Tags read from the names (« USA », « En, Fr », « Disc 1 ») are shown.
- **Covers before download.** Games on the network get their cover by file name (libretro-thumbnails, Xbox and
  Xbox 360 included) instead of the cloud.
- **Downloading** in the Show pill: the games queued, downloading or unpacking.
- **More than one torrent.** In Options › Network sources, « Add a .torrent file » was squeezed to « … » by its
  hint and could not be found; its label shows whole now. While a download ran, the library refreshed every 2 s and
  closed the console choice of a new torrent before it could be picked: refreshes now wait for the open sheet.
- **Xbox 360: a bad state no longer kills the game.** A damaged save state is refused and the game goes on; an
  automatic resume that crashed is set aside, so the next start is a fresh one.

**Known problems**

- Covers are matched by file name: a name that differs from libretro-thumbnails' shows the cloud.
- .7z and .rar archives, and extracted game folders in a torrent, are not listed yet.

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Launcher

- a background refresh waits for the open sheet (torrent console choice) (b905bef3)

### Network library

- versions stacked, tags shown, covers by name, Downloading view (XBA-332) (b09c0086)

### Network sources

- the add rows show their whole label (4f9f7161)

### x360

- damaged states refused safely, a resume that crashed is set aside, start stages logged (315b6511)
- undo only for states from files; set-aside slots and the resume marker stay on the device (f5c65e58)

<!-- release:changes end -->
