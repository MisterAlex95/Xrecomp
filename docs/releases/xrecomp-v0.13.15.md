# XRecomp 0.13.15

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

A fix for the network library.

- **Torrents with deep folders.** A torrent only listed the games at its top or one folder down; games five or six
  folders deep (a library sorted by console, region, then game) were left out and the source showed no games. Every
  game a torrent holds is now listed, and found again once downloaded. Sources already added list their games
  without being added again.

**Known problems**

- Games inside archives (.zip, .7z, .rar) and extracted game folders (`default.xbe`) in a torrent are not listed yet.
- A source with no game it can list is still added with « 0 games », without saying why.

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Network library

- games deep in a torrent are listed (XBA-320) (c530e1cd)

<!-- release:changes end -->
