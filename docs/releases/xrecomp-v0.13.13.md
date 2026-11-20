# XRecomp 0.13.13

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

Xbox 360 games run faster and look more like the console, and a crash now comes with readable symbols.

- **Xbox 360: faster.** After each frame the renderer checked every cached texture for changes by decoding it again.
  It now hashes the texture's memory directly, about 100 times faster per texture on a desktop. Warhammer: Battle
  March ran at 20 frames/s on the Thor; tell us what « perf speed » says now.
- **Xbox 360: closer to the console.** Fixes from a review against Xenia:
  - occlusion and visibility queries now complete, so a game that waits on them no longer stalls;
  - more than 256 shader programs work, with no geometry missing in long sessions;
  - depth bias works: decals and shadows no longer flicker;
  - each render target keeps its own blending and colour mask;
  - gamma targets and gamma textures show the right brightness;
  - two more texture formats (8_A, 8_B);
  - "base map" textures read the right mip level.
- **Crash symbols.** Each release now has an `-symbols.tar.gz` file next to the APK, and `tools/symbolize.sh` names
  the functions in a crash log taken with « Share the last game's log ».
- **Leaving a game no longer freezes the app.** The automatic save on leaving now runs in the background.
- **Safer emulation.** Fixed reads past the end of a buffer in the PS1 sound chip, the Dreamcast and 3DS screens,
  GameCube copies to RAM, and in the Xbox 360 loader for damaged XEX files and save states.

**Known problems**

- Most of the Xbox 360 changes have only been tested on desktop GPUs (Mesa). Report anything that looks wrong on the
  Thor, with the last game's log.

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Android

- the automatic save on leaving the game runs off the UI thread (70b08cf2, XBA-318)

### Consoles

- no reads past the end of a buffer in PS1 SPU, DC, 3DS and GC picture code (e8e2014c, XBA-318)

### Releases

- crash symbols next to the APK, and tools/symbolize.sh to read a tombstone with them (812d8748, XBA-318)

### X360

- a damaged XEX or save state is refused, not read past its buffers (f4283a3a)
- textures checked with a fast hash of their memory, read directly, not decoded a dword at a time (2b6b8652, XBA-318)
- occlusion queries complete, EVENT_WRITE_ZPD reports passed samples (aa7a9fe8)
- VIZ_QUERY posts its event and reports the query visible (fa1a0f51)
- no limit on shader programs, found through a hash table (d19ea693)
- an 8_8_8_8_GAMMA picture is shown with its gamma-encoded values (fd11d2b9)
- a texture fetched with the gamma sign is sampled linear (sRGB upload) (4be4b2ec)
- the frame texture's format is kept per renderer (67261eb9)
- the guest's depth bias reaches GL (polygon offset) (476de39b)
- each render target gets its own blend state and colour mask (a15427df)
- no draw lost to blending a 32-bit float target without EXT_float_blend (dc9b1fd2)
- the mip filter "base map" never samples smaller levels (d0a95b7c)
- a "base map" texture is read at the fetch constant's first level, not level 0 (dc607f6c)

<!-- release:changes end -->
