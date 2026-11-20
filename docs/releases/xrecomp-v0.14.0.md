# XRecomp 0.14.0

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

The network library takes zipped games.

- **.zip games in torrents.** A torrent that shares one .zip per disc (a Redump set, say) listed no game. Each .zip
  is now a game of the console you chose for the source. Once downloaded it is unpacked into a folder beside it
  (« Unpacking · 42 % » on its card), the .zip is deleted, and the game is imported and prepared as usual. Checked
  on the AYN Thor with an Xbox 360 set.
- **A source with no game says why.** Instead of « 0 games », the source shows what the torrent holds
  (« No game (238 .7z, 2 .txt) »).

**Known problems**

- Unpacking needs room for the .zip and the game at the same time (about twice the game's size); it waits with
  « Not enough space » until there is.
- .7z and .rar archives, and extracted game folders in a torrent, are not listed yet.

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Network library

- .zip games in torrents, unpacked once downloaded (XBA-331) (634a417c)

<!-- release:changes end -->
