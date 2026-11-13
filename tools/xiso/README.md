# tools/xiso: Xbox disc images (XDVDFS)

Two readers and a generator. None of them downloads, holds or needs a real disc. Use only images you dumped from
your own discs, and keep those outside the repository.

| What | For |
|---|---|
| `python3 -m tools.xiso ls / get / unpack` (`xdvdfs.py`) | The pipeline's first step: getting `default.xbe` out of an image ([docs/formats/disc-image.md](../../docs/formats/disc-image.md)). |
| `xiso_extract image.iso outdir` (C, `xiso_extract.c`) | Extracting a whole image on Linux with the library the Android app uses (`src/xiso`, XBA-40). |
| `mkxiso.py SRC_DIR OUT.iso` | Building synthetic images for the tests. |
| `source.py` | XBA-126: the image's bytes whatever holds them, split parts (`Game.1.iso`, `.part1`, `.iso.001`) or compressed (CSO, ZSO, CCI); `xdvdfs.py` reads through it, so every command above takes them. The Python twin of `src/xiso/xiso_source.c`. |
| `mkcso.py IN.iso OUT --format cso1\|cso2\|cso2-framed\|cso2-maxcso\|zso\|cci` / `--split BYTES` | Compressing or splitting a synthetic image the way the usual tools do, for the tests. |
| `bench_disc.sh BUILD_DIR WORK_DIR` | Read throughput of D: through the runtime's file layer: folder vs image vs split vs CSO ([docs/technical/disc-images.md](../../docs/technical/disc-images.md)). |

## xiso_extract

```sh
cmake -S tests/xiso -B build/xiso && cmake --build build/xiso
build/xiso/xiso_extract "My Game.iso" game/
```

It prints the title from `default.xbe` and a progress line. `game/` is replaced as a whole on success: the files go
to `game.partial` first, so a failed or interrupted run leaves `game/` as it was. It reads a bare game partition
(extract-xiso / xdvdfs output) or a redump full-disc image (game partition at 0x18300000). The API is in
`src/xiso/xiso.h`.

## mkxiso.py

```sh
python3 tools/xiso/mkxiso.py SRC_DIR out.iso                     # bare image
python3 tools/xiso/mkxiso.py SRC_DIR out.iso --base 0x18300000   # redump layout, sparse
```

Standard library only. Each directory becomes a balanced binary tree of entries, root first, with no entry crossing
a sector. `--rename OLD=NEW`, `--corrupt bad-magic|sector-range|self-loop|dir-cycle` and `--truncate BYTES` make
the broken images the tests feed to the readers. `tests/xiso/make_fixtures.py` builds the C test's images (at CMake
configure time), and `test_mkxiso.py` reads images back with `xdvdfs.py`.
