# chdman fixtures (XBA-192)

Real `chdman createcd` output, read by `ps1_identify_test` (test_identify.c, `CHDMAN_DISCS`) on every run: the title
key (path and `fd:N`), the track layout and the sectors of each disc. They are made from the test's own synthetic
ISO 9660 « PLAYSTATION » volume (24 sectors, SYSTEM.CNF only) and a tone of our own. No game data.

| CHD | Source sheet | chdman |
|---|---|---|
| single, single_cdlz, single_cdzl, single_cdzs | one MODE2/2352 track | default codecs, `-c cdlz`, `-c cdzl`, `-c cdzs` |
| iso2048 | one MODE1/2048 track | default |
| audio, audio_cdzs | data + CD-DA, the audio track's 2-second pregap stored (INDEX 00 / 01, redump's shape) | default, `-c cdzs,cdfl` |
| pregap | data + CD-DA, `PREGAP 00:02:00` (not stored) | default |
| t1pregap | track 1 with `PREGAP 00:02:00`, then CD-DA | default |
| t1index0 | track 1 with INDEX 00 at 00:00:00 and INDEX 01 at 00:02:00 (lead-in stored), then CD-DA | default |

Made with chdman 0.276 (Debian trixie `mame-tools`). To make them again, from a build of consoles/ps1/tests:

```sh
ps1_identify_test --make OUTDIR      # chdman on the PATH, or CHDMAN=/path/to/chdman
cp OUTDIR/*.chd consoles/ps1/tests/chdman/
```

`--make` checks each CHD with `chdman verify` and leaves out one chdman cannot read back.
