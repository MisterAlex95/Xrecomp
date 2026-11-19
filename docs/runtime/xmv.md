# XMV movies: the runtime's decoder

> Part of the [Documentation Index](../INDEX.md) · Plane XBA-70, XBA-199, XBA-198

Xbox titles play their movies (`.xmv`) with the XDK's XMV library, which is linked into the title and lifted like
the rest of its code. When that lifted decoder runs, nothing else is needed: Fable's plays once vblanks are paced
(`profiles/titles/4d5300d1/NOTES.md`). For a title whose own decoder cannot run, the runtime carries a portable
decoder (`src/video/xmv/`) and a hook in place of the library's `XMVDecoder_GetNextFrame` (`src/video/xmv_hle.c`),
which Prepare binds by itself when it finds the library's API in the XBE ([below](#binding-at-prepare-xba-199)): no
title layer. Upstream decodes movies on Windows only, and only `.wmv` files through Media Foundation
(`RECOMP_FMV_HOST`); this works on Linux, macOS, Android and Windows alike.

## What is where

| File | What |
|---|---|
| `src/video/xmv/xmv_demux.c` | The XMV container (the C port of `tools/xmv/xmv_demux.py`): header, packet chain, video frames, audio blocks. |
| `src/video/xmv/wmv2_dec.c` | WMV2 (Windows Media Video 8) video: I and P frames into YUV 4:2:0. |
| `src/video/xmv/wmv2_tables.c` | The format's code tables (VLCs, run/level, motion vectors, scans). LGPL-2.1-or-later, from FFmpeg (see below). |
| `src/video/xmv/xbox_adpcm.c` | Xbox ADPCM to 16-bit PCM. |
| `src/video/xmv/xmv_player.c` | Demuxer + decoders behind a pull API (next frame with its time, decoded audio), BT.601 colour conversion. |
| `src/video/xmv/xmv_surface.c` | A picture into a D3D surface's pixels: YUY2, UYVY, (LIN_)A8R8G8B8 / X8R8G8B8, (LIN_)R5G6B5, swizzled or linear. |
| `src/video/xmv_hle.c` | The hook: the movie the title opened, paced on the host clock, written into the title's surface, sound out. |
| `tools/xmv/find_api.py` | Finds `XMVDecoder_GetNextFrame` / `CloseDecoder` / `CreateDecoderForFile` in an XBE by structure (and recorded signatures), with a confidence. |
| `tools/recomp/xmv_bind.py` | What the lifter generates from it (`--xmv-bind`): `recomp_xmv_bind.c`, the hook bound in place of the found functions. |
| `tools/xmv/synth_title.py` | A synthetic movie title of our own with a fake XMV library, for the tests. |
| `src/kernel/kernel_bridge.c` | `NtCreateFile` of a `*.xmv` file tells the hook its name (`xbox_MovieFileOpened`). |
| `tools/xmv/xmv_mux.py` | XMV writer, Xbox ADPCM encoder, the test fixtures (`make-test-set`). |
| `tools/xmv/wmv2_synth.py` | Random valid WMV2 streams for the features FFmpeg's encoder never writes. |
| `tools/xmv/xmv_dump.c` | Decodes a movie to Y4M + WAV with this decoder and says how much of it was handled. |

All of it is plain C99/C11 with no dependency: the decoder builds into `xbox_video` on every host.

## The hook

The XDK's `XMVDecoder_GetNextFrame(decoder, surface, &result, &timestamp)` (stdcall, `ret 0x10`) is the call a
title polls while a movie plays; it is where a title whose decoder cannot run hangs (Fable, before vblank pacing:
`XMV_NOFRAME` for ever) and what its old skip override replaced with `XMV_ENDOFFILE`. Prepare binds the runtime's
replacement in its place when it finds the function ([below](#binding-at-prepare-xba-199)); by hand, a developer's
`overrides.c` (`--exclude-manual`, [profiles.md](../technical/profiles.md)) can still do it:

```c
void sub_XXXXXXXX(void)                 /* XMVDecoder_GetNextFrame in this XBE */
{
    eax = xmv_hle_get_next_frame(MEM32(esp + 4), MEM32(esp + 8), MEM32(esp + 12), MEM32(esp + 16));
    esp += 4 + 16;                      /* ret 0x10 */
}
```

and `XMVDecoder_CloseDecoder` can call `xmv_hle_close(decoder)`. What the hook does:

1. **Which movie.** The kernel notes every `.xmv` file the title opens (`NtCreateFile`, host path). A decoder object
   the hook has not seen plays the last one opened. A new open after a movie ended starts a new one on the same
   object. With the game data read in place from a disc image (`kernel_disc.h`, Prepare's default for an ISO),
   that host path names a file inside the image: the hook then streams it through the disc layer (XBA-309; before,
   it could not open it and every movie of such a title went to the title's own decoder).
2. **Pacing.** The first call starts the movie's clock (host monotonic time). Each call returns `XMV_NOFRAME` until
   the next frame's time has come, then decodes it, writes it into `surface` and returns `XMV_NEWFRAME` with the
   frame's time (100 ns units, 64-bit). After the last frame: `XMV_ENDOFFILE`.
3. **The picture.** The surface is the title's (`D3DSurface`: `Data` at +4, `Format` at +0x0C, `Size` at +0x10);
   `Data` is a physical address, seen through the contiguous mapping at 0x80000000. The picture is converted to
   the surface's format (BT.601 studio range, as the Xbox's player) at its top left; the title then shows the
   surface its own way (a textured quad through the GPU back end, or `D3DDevice_UpdateOverlay` through the PVIDEO
   scan-out, both already handled).
4. **Sound.** The first Xbox ADPCM (or PCM) track, decoded as the packets are read, goes out through the host
   layer's audio device (`host_audio`, POSIX hosts), down-mixed to its channels and linearly resampled when the
   device runs at another rate (it keeps the format of its first open). Not through the APU: the title's own
   DirectSound streams for the movie are never fed. Windows: no sound yet.
5. **Fallback.** Bound by Prepare, a movie the runtime would not decode exactly goes to the title's own
   decoder ([the hand-over](#the-hand-over-to-the-titles-own-decoder)). Called directly (a hand-written override,
   which has no own decoder to call), or with `RECOMP_XMV=skip`, no `.xmv` opened or a file that is not XMV: the
   movie ends at once (`XMV_ENDOFFILE`), the old skip override's behaviour. The log says which (`[XMV] ...`), and
   at the end of each movie how many frames were decoded and how many were not.

## Binding at Prepare (XBA-199)

The XMV library is linked into each title, so its functions move from XBE to XBE and no symbol names them. Prepare
finds them by structure, the way the D3D service finds D3D's device at run time, and binds the hook through the
lifter: nothing of any one title is in the toolkit, and no title layer is involved.

**Finding it** (`tools/xmv/find_api.py`, from the XBE alone, a few seconds):

- *The library table*: the XDK build, and an XMV library entry if there is one (a name starting with `XMV`).
- *The sections*: the XDK's libraries keep their code in sections of their own (D3D, DSOUND, XPP...); the XMV
  library's is `XMV`. A candidate inside it scores, one outside it (when there is one) counts against.
- *The movie names*: strings ending in `.xmv` (ANSI or UTF-16) and the functions that use their address: the
  title's movie player.
- *The call graph*, from one linear sweep of the code. Every called function whose every `ret` is `ret 0x10`
  (stdcall, four arguments) is a candidate, scored on its call sites from outside the XMV section (the title calls
  the public API): the site is in a loop, or its function is called from one (`loop`, `loop_up`); a `cmp` with 1 or
  2 (`XMV_NEWFRAME`, `XMV_ENDOFFILE`) right after the call (`result`); the address of a stack local passed
  (`locals`: &result, &timestamp); a `ret 0xC` function called before it (`create`: CreateDecoderForFile, flags,
  name, &decoder) and a `ret 4` function after (`close`: CloseDecoder), in the XMV section when there is one;
  the function, or one next to it in the call graph, uses a movie name (`movie`). More than 8 call sites counts
  against (GetNextFrame is called from one or two places).
- *Recorded signatures*: `profiles/xdk/<build>/xmv.json` (below). A candidate that matches one of the title's build
  gets +0.5 (+0.3 for another build).

The confidence is the best candidate's score (capped at 1), lowered by 0.3 when a second candidate is within 0.15 of
it. **High** is 0.7 or more: an XMV section or a movie name on top of the call shape, or a signature. The shape
alone (0.6 at best) is reported, never bound.

**Played by the library (XBA-309).** Some titles never call `XMVDecoder_GetNextFrame`: they hand the movie to the
library's own `XMVDecoder_Play` (`ret 0xC`), which polls GetNextFrame itself and paces the pictures it shows on a
clock (performance counter since a start time) that the library's GetNextFrame starts at its first frame. Bound
there, the runtime's hook decodes the movie but never starts that clock, and Play presents almost none of the frames
(a retail XDK 5849 title: 157 of 1765). So when the XMV section is known and the best candidate has no call site
outside it, the report says `library_only` and nothing is bound, whatever the score or a signature: the title's own
decoder plays the movie, as it does unbound. `python -m tools.xmv.find_api default.xbe` prints the evidence for
each candidate (`--json` for the report as JSON); Prepare writes the same report to `<work>/analysis/xmv_api.json`.

**Binding it.** With a high confidence, Prepare writes `<work>/analysis/xmv_bind.json` (GetNextFrame and, when
found at the same call site, CloseDecoder; CreateDecoderForFile is reported but never bound: the title's own
decoder object stays, and its open is what tells the hook the file name) and passes it to `tools.recomp
--xmv-bind`. The lifter emits those bodies as `sub_X_gen` (the `--exclude-manual` wrap mechanism) and writes
`recomp_xmv_bind.c` next to the generated code: `sub_X`, which every caller and the dispatch table reach, asks
`xmv_hle_route()` with the decoder object and calls the hook or the title's own body (`sub_X_gen`, then
`xmv_hle_native_result()` with what it answered: [the hand-over](#the-hand-over-to-the-titles-own-decoder));
CloseDecoder calls `xmv_hle_close`, then the title's own close. A function the analysis did not find, or one a developer's
`--exclude-manual` file defines, is not bound. The binding is part of the lift's stamp (desktop `stamp.json`
`xmv_bind`, Android the lift key, with the generated binding's version `xmv_bind.VERSION`): a change re-lifts.

**The switch, `RECOMP_XMV`:**

| Value | At Prepare | At run time, in a bound title |
|---|---|---|
| `auto` (default; also unset, `decode`) | bind when found with high confidence | per movie: the runtime's decoder when it supports everything the movie uses, else the title's own (below) |
| `runtime` | bind as `auto` | the runtime decodes every movie, also those it does not fully support |
| `native` | bind nothing | the title's own (lifted) decoder runs, as if nothing were bound |
| `skip` (or `0`) | bind as `auto` | every movie ends at once (`XMV_ENDOFFILE`) |

Default `auto` binds whenever the API is found, also in a title whose own decoder would run (Fable's does, once
vblanks are paced): on what it supports the runtime's decoder is bit-exact with FFmpeg, and it does not depend on
the title's DirectSound streams or decode threads; a movie it does not support goes to the title's own decoder,
which is what played before the binding. `native` keeps the title's own decoder for every movie (and binds
nothing: the way back if the binding itself is wrong).

## The hand-over to the title's own decoder

Binding the runtime's decoder must not make a working movie worse: X8 intra frames (XBA-198), WMA sound and 5.1
are not decoded exactly, and the title's own decoder may well handle them. So with `RECOMP_XMV=auto`, at the first
`GetNextFrame` of a decoder object, `xmv_hle_route()` opens the movie the title opened and looks at it:

- *the audio tracks*: any WMA track (the runtime would play it silent), any format other than 4-bit Xbox ADPCM or
  16-bit PCM, a 5.1 movie (tracks flagged as its pairs) or a track of more than two channels (the runtime plays
  two);
- *the first frames* (`xmv_player_probe`): the picture header of the first 1,800 (a minute at 30 frames per
  second) for X8 intra frames (an I frame whose `j_type` bit is set, when the sequence header allows X8: FFmpeg's
  encoder and others set that flag on every stream, so the flag alone says nothing), and the first 30 decoded for
  damage: two or more damaged frames, and at least one in sixteen, or a broken packet chain;
- *the file*: none opened, or not one the runtime can open.

Any of these, and the whole movie goes to the title's own decoder: every `GetNextFrame` of that decoder object
calls the title's lifted body, until it is closed or the title opens another movie. The log says why, once:
`[XMV] decoder 00A10000: X8 intra frames (3 in the first 1800 frames; XBA-198): the title's own decoder plays this
movie`, and at its close how many frames the title's decoder gave. The decision is taken before anything of the
movie is shown, so the two decoders never share one: the title's own `CreateDecoderForFile` is never bound, its
decoder object is complete.

What the probe does not see: an X8 frame after the first minute (the runtime then shows the previous picture for
it, counted in `movie ended`), damage further in. Checking a disc's movies beforehand: `xmv_dump` (below) says
how many frames of a movie the runtime decodes.

**The watchdog.** A movie handed over can hang in the title's own decoder (Fable's did, before vblank pacing:
`XMV_NOFRAME` for ever). After each call of the title's body, `xmv_hle_native_result()` reads the result it wrote:
with no `XMV_NEWFRAME` for 10 seconds while the title keeps polling (`RECOMP_XMV_WATCHDOG=seconds`, `0` turns it
off; a second or more between two polls is not counted, a paused movie), the runtime takes the movie back: if the
title's decoder never gave a frame and the runtime can open the file, the runtime plays it from the start (not
exactly, but better than a hang); otherwise the movie ends (`XMV_ENDOFFILE`). Logged: `[XMV] decoder 00A10000: the
title's own decoder gave no frame for 10.0 s while the title polled: the runtime plays the movie instead` (or
`the movie ends`). The XBA-93 wait-spin detection is not used for this: it watches spin loops on guest memory, not
a polled API, and a movie poll loop that calls `GetNextFrame` is exactly what the watchdog sees.

**What the logs say.** Prepare: `XMV: XMVDecoder_GetNextFrame at 0x... and XMVDecoder_CloseDecoder at 0x...
(confidence 0.90): bound to the runtime's decoder`, or `best guess ... too low, not bound`, or `... is called only by
the XMV library itself: the title plays its movies with the library's XMVDecoder_Play ...; not bound`, or `the title has movies
but no XMVDecoder_GetNextFrame was found`, or nothing for a title with no sign of movies. The lift log:
`XMV: bound ...`. Run time, at the first call: `[XMV] XMVDecoder_GetNextFrame at 0001xxxx bound by Prepare (found
with confidence 0.90): the runtime decodes the title's movies it fully supports, the title's own decoder the
others` (or `RECOMP_XMV=native, the title's own decoder runs`); then, per movie, `[XMV] decoder ... plays ...` or
the hand-over's reason.

**Signatures.** Once a title's functions are confirmed (from the report and the lifted code), `python -m
tools.xmv.find_api default.xbe --record get_next_frame=0x... [close_decoder=0x...]` writes their masked signatures
to `profiles/xdk/<build>/xmv.json` (build: the XMV library entry's, else the XDK's; `--build` to choose). The masking
is `tools/fusion/signatures.py`'s: the first 32 bytes with absolute addresses and branch targets wildcarded, plus
the function's size; it refuses a function whose returns do not have the API's shape. That is XDK library code
only, masked, as the signature tooling already stores; no title code. None is recorded yet: they need XBEs dumped
from real discs.

**What only real titles can confirm.** The detector is tested on synthetic titles of our own
(`tools/xmv/synth_title.py`: a fake XMV library with the API's shape, in a section of its own or not, with or
without a movie name, a title that plays through the library's `XMVDecoder_Play`, and a title without movies).
Seen on one retail XBE (XDK 5849, XBA-309): the library's code in a section named `XMV`, an `XMV` entry in the
library table, and a title that only calls `XMVDecoder_Play` (above). Unconfirmed on other XDK builds: that the
XMV library's code sits in a section named `XMV`; whether the library table lists the XMV library; that titles call
`XMVDecoder_GetNextFrame` directly (not through a C++ wrapper of their own, which would then be the candidate, and
still a correct binding when it keeps the stdcall shape); that `CloseDecoder` follows the poll in the same
function. A title where it is wrong can be prepared with `RECOMP_XMV=native` (nothing bound) and its
`find_api` report sent in.

## The format, as implemented

**Container** (little-endian). Header: `+0` next packet size, `+4` header + first packet size, `+8` largest
packet, `+0x0C` "xobX", `+0x10` version (2 or 4), `+0x14` width, `+0x18` height, `+0x1C` duration (ms), `+0x20`
audio track count (16-bit) and 2 bytes of padding, then 12 bytes per track (codec, channels, rate, bits per
sample, flags; codec 0x0069 Xbox ADPCM, 0x0001 PCM, 0x0161 WMA; flags 1/2/4 mark the three stereo pairs of a 5.1
stream). Each packet: the next packet's size; a video word (bits 0..22 video size, 23..30 frame count, 31 "a
sequence header follows"); 4 bytes; one 4-byte word per audio track (bits 0..22 its size; 0 repeats the previous
track's); the video data, then each track's audio. The video size counts 4 bytes per audio track more than there
is. In the video data: the optional 32-bit sequence header, then per frame a 32-bit word (bits 0..16 the frame's
size in 32-bit words minus one, 17..31 the time since the previous frame in ms) and the frame.

**WMV2 in XMV.** The frame is stored as little-endian 32-bit words of the usual most-significant-bit-first
bitstream. The sequence header is packed differently from a WMV file's 4 bytes: bit 0 mspel, 1 loop filter, 2
adaptive block transform, 3 X8 intra allowed, 4 top-left MV flag, 5 per-MB RL table, 6..8 slice count. The first
bit of a frame (the top bit of its first word) is 0 for an I frame.

**Xbox ADPCM.** IMA ADPCM, 36 bytes per channel per block: the first sample (int16) and the step index, then 32
bytes of 4-bit codes, low nibble first, channels interleaved 4 bytes at a time; 65 samples per block. The step
arithmetic is the IMA reference's shift-and-add, as the APU's decoder (`src/apu/apu_state.h`) does; a signed step
index byte is clamped to 0..88 the same way.

## Coverage

| Feature | State |
|---|---|
| Container, all packet layouts the demuxer knows (audio-only packets, duplicate tracks, truncated files) | done |
| Xbox ADPCM, mono / stereo / up to 8 channels; 16-bit PCM | done |
| WMV2 I frames (MS-MPEG4 intra: DC/AC prediction, three RL tables per class, escapes 1-3) | done, bit-exact |
| WMV2 P frames: skip maps (none / MPEG / row / column), inter MB tables, MV prediction with the top-left flag, half-pel and mspel motion compensation, the 8x4 / 4x8 adaptive block transform, intra MBs | done, bit-exact |
| Loop filter, several slices, pictures that are not whole macroblocks | done, bit-exact |
| WMV2 I frames coded with the X8 intra coder (`j_type`) | **not done** (XBA-198): bound, such a movie goes to the title's own decoder; called directly or with `RECOMP_XMV=runtime`, the frame is counted and the previous picture stays |
| 5.1 ADPCM (three stereo tracks) | bound: the title's own decoder; else the front pair is played |
| WMA audio tracks | bound: the title's own decoder; else not played (silence) |
| Binding `XMVDecoder_GetNextFrame` without a title layer | done by structure at Prepare (XBA-199); unconfirmed on retail XBEs, no signature recorded yet |
| Sound on Windows | not done |

"Bit-exact" means: every decoded picture of every fixture is identical, byte for byte, to FFmpeg's decode of the
same file in its bit-exact mode.

## Tests

`tests/xmv_decode` (in `tools/run_c_tests.sh`, no runtime needed):

- **Video against FFmpeg.** `fixtures/*.xmv` are our own synthetic movies: FFmpeg's wmv2 encoder on test
  patterns (I and P frames, intra MBs in P frames, the three cbp tables, both escape-3 codings, a 104x72 picture),
  with the loop filter and slice count switched in the XMV header (they change the reconstruction, not the syntax),
  and three streams from `tools/xmv/wmv2_synth.py` with mspel, the adaptive block transform, every skip map, per-MB
  tables, the top-left MV flag and up to four slices. Each fixture's per-frame CRC-32s of FFmpeg's decode
  (`ffmpeg -flags +bitexact`: FFmpeg's SIMD no-rounding averages are approximate otherwise) are in
  `fixtures/expected.h`; the decoder must match all of them, and again after a rewind. 96% of `wmv2_dec.c`'s lines
  run (the rest are error returns).
- **Audio.** The decoded PCM must be what the ADPCM encoder reconstructed, and every block (and 2000 random ones)
  must decode as the APU's own decoder decodes it.
- Demuxer refusals, a file cut in the middle of a packet, 500 random "frames" (no crash), the colour conversion,
  and every surface format (layout, pitch, swizzle round trip, refusals).

`tests/xmv_hle` (the runtime linked, guest memory set up from `tools/conformance/test.xbe`): a `D3DSurface` in the
contiguous window, the movie noted as the kernel notes it, then `xmv_hle_get_next_frame` polled until the end:
12 `XMV_NEWFRAME` with rising timestamps, `XMV_NOFRAME` in between (paced), each picture in the surface identical to
the player's own decode, `XMV_ENDOFFILE` after; a new open restarts the decoder object; `RECOMP_XMV=skip`, no movie
opened, a non-XMV file and an unknown surface format behave as described above; `xmv_hle_bound` answers the
runtime's decoder by default, with `auto` and `skip`, and the title's own with `native`. With a disc image of our
own (`movie_disc.iso`, written at configure time by `tools/xiso/mkxiso.py` around `basic.xmv`) mounted, the movie
opened inside the image is the runtime's and plays its 12 frames; a name the image does not hold goes to the
title's own decoder.

The hand-over, on `tests/xmv_hle/fixtures` (`python -m tools.xmv.xmv_mux make-fallback-set tests/xmv_hle/fixtures`:
`wmv2_synth.py` streams, no FFmpeg): the probe finds the X8 frame of `x8.xmv` from its picture header, the four
damaged frames of `damaged.xmv`, stops at its limits and rewinds the player; `x8_unused.xmv` (X8 allowed by the
sequence header, never used: its pictures are bit-exact with FFmpeg's) and `basic.xmv` stay the runtime's;
`x8.xmv`, `wma.xmv` (a WMA track), `surround.xmv` (three 5.1 pairs), `damaged.xmv` and a file that is not XMV go to
the title's own decoder, for the whole movie; `RECOMP_XMV=runtime` keeps an X8 movie, `skip` ends it, `native`
never asks; the watchdog takes a hung movie back after `RECOMP_XMV_WATCHDOG` seconds of polls and the runtime plays
it, ends it when the title's decoder had given frames, ignores polls a second apart, and is off with `0`.
`tools/xmv/test_xmv_mux.py` checks that the fixtures are what `make-fallback-set` writes.

The binding (XBA-199):

- `tools/xmv/test_find_api.py`: the synthetic movie title's API found in its `XMV` section (every feature, the
  title's own `ret 0x10` decoy far behind), found without a section through the movie name, the call shape alone
  not bound, a title that plays through the library's `XMVDecoder_Play` never bound (not even with a signature of
  its build), a title without movies binding nothing, an XMV library entry counted, `RECOMP_XMV=native` at
  prepare, a signature recorded on one title finding the API in another where only the call shape is left, a
  wrong-shaped function refused by `--record`, the CLI report, and the generated C.
- `tools/compat/test_e2e.py`: `tools.prepare --no-build` binds the found functions (their bodies `sub_X_gen`,
  `recomp_xmv_bind.c`, no title layer) and redoes the lift without them under `RECOMP_XMV=native`; a title with no
  XMV library gets nothing. With `XBA_E2E=1`, the title is built and run: its own GetNextFrame never gives a frame,
  bound it plays `basic.xmv` (copied as its `D:\intro.xmv`) into its surface, 12 frames decoded, and goes on to its
  `DbgPrint`; the same executable with `RECOMP_XMV=native` stays in its loop; with `x8.xmv` as the movie, it goes
to the title's own decoder, which gives nothing, the watchdog (`RECOMP_XMV_WATCHDOG=1`) gives it back to the
runtime, which plays it, and the title goes on.

Remaking the fixtures (FFmpeg needed only for this): `python -m tools.xmv.xmv_mux make-test-set
tests/xmv_decode/fixtures`. `tools/xmv/test_xmv_mux.py` checks the Python side (mux / demux round trip, ADPCM
encoder) under pytest.

Checking a movie from your own disc (kept outside the repo): build `tests/xmv_decode` and run
`build/xmv_dump movie.xmv out.y4m out.wav`; it prints how many frames were decoded, and its exit status is 3 when
some were not (X8 intra frames, damaged data). Compare with `ffmpeg -flags +bitexact -i movie.xmv -f yuv4mpegpipe
ref.y4m`.

## Why not FFmpeg's libavcodec

`tools/linux/install_deps.sh` lists FFmpeg's development packages, but nothing in the runtime links them, and the
Android build (NDK, the app's prepare on the device) and the macOS launcher have no FFmpeg at all. Linking it would
mean cross-building and shipping libavcodec for Android arm64 and macOS, and keeping it dynamically linked and
replaceable in the app to honour the LGPL (or carrying GPL obligations, for the usual GPL-enabled builds). The
decoder needed here is small (WMV2 is MPEG-4 part 2 with a twist, about 1,500 lines with its tables), so it is
written for the project and has no dependency. Only the code tables, which are data the format fixes, come from
FFmpeg: `wmv2_tables.c` keeps FFmpeg's LGPL-2.1-or-later licence and copyright and is listed in `NOTICE`, like the
APU and NV2A files taken from xemu. The repository (MIT) already carries LGPL files this way.
