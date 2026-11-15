# XMV movies: the runtime's decoder

> Part of the [Documentation Index](../INDEX.md) · Plane XBA-70

Xbox titles play their movies (`.xmv`) with the XDK's XMV library, which is linked into the title and lifted like
the rest of its code. When that lifted decoder runs, nothing else is needed: Fable's plays once vblanks are paced
(`profiles/titles/4d5300d1/NOTES.md`). For a title whose own decoder cannot run, the runtime carries a portable
decoder (`src/video/xmv/`) and a hook a title layer binds in place of the library's `XMVDecoder_GetNextFrame`
(`src/video/xmv_hle.c`). Upstream decodes movies on Windows only, and only `.wmv` files through Media Foundation
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
| `src/kernel/kernel_bridge.c` | `NtCreateFile` of a `*.xmv` file tells the hook its name (`xbox_MovieFileOpened`). |
| `tools/xmv/xmv_mux.py` | XMV writer, Xbox ADPCM encoder, the test fixtures (`make-test-set`). |
| `tools/xmv/wmv2_synth.py` | Random valid WMV2 streams for the features FFmpeg's encoder never writes. |
| `tools/xmv/xmv_dump.c` | Decodes a movie to Y4M + WAV with this decoder and says how much of it was handled. |

All of it is plain C99/C11 with no dependency: the decoder builds into `xbox_video` on every host.

## The hook

The XDK's `XMVDecoder_GetNextFrame(decoder, surface, &result, &timestamp)` (stdcall, `ret 0x10`) is the call a
title polls while a movie plays; it is where a title whose decoder cannot run hangs (Fable, before vblank pacing:
`XMV_NOFRAME` for ever) and what its old skip override replaced with `XMV_ENDOFFILE`. The runtime's replacement is
bound the same way, from a title layer's `overrides.c` (`--exclude-manual`, [profiles.md](../technical/profiles.md)):

```c
void sub_XXXXXXXX(void)                 /* XMVDecoder_GetNextFrame in this XBE */
{
    eax = xmv_hle_get_next_frame(MEM32(esp + 4), MEM32(esp + 8), MEM32(esp + 12), MEM32(esp + 16));
    esp += 4 + 16;                      /* ret 0x10 */
}
```

and `XMVDecoder_CloseDecoder`, if the title layer finds it, can call `xmv_hle_close(decoder)`. What the hook does:

1. **Which movie.** The kernel notes every `.xmv` file the title opens (`NtCreateFile`, host path). A decoder object
   the hook has not seen plays the last one opened. A new open after a movie ended starts a new one on the same
   object.
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
5. **Fallback.** `RECOMP_XMV=skip`, no `.xmv` opened, a file that is not XMV: the movie ends at once
   (`XMV_ENDOFFILE`), the old skip override's behaviour. The log says which (`[XMV] ...`), and at the end of each
   movie how many frames were decoded and how many were not.

Not done: finding `XMVDecoder_GetNextFrame` in an arbitrary XBE. The XMV library varies across XDK builds and
there is no signature for it yet, so a title layer names the function (from the lifted code: the one the movie loop
polls, in the XBE's `XMV` section). A signature-based binding is the follow-up (XBA-199).

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
| WMV2 I frames coded with the X8 intra coder (`j_type`) | **not done**: the frame is counted and the previous picture stays (follow-up XBA-198) |
| 5.1 ADPCM (three stereo tracks) | the front pair is played |
| WMA audio tracks | not played (silence) |
| Binding `XMVDecoder_GetNextFrame` without a title layer | not done (follow-up XBA-199) |
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
opened, a non-XMV file and an unknown surface format behave as described above.

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
