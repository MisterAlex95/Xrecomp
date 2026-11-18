# XRecomp 0.13.7

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

GameCube games draw correctly on Adreno phones: the picture is complete, steady and fast.

- **Missing surfaces.** GX's cull mode was the wrong way round, which removed the faces a game wanted to keep: floors
  and walls came out black.
- **A picture that jumped back.** About twelve times a second the window showed an older picture, or black. A game's
  last draw could leave colour writes off, and the window's present now sets the state it depends on.
- **Stray triangles and speed.** Vertex, index and uniform data stream through persistent, fence-protected buffers
  where the driver has `GL_EXT_buffer_storage`. Other drivers keep the previous path.
- **A driver crash at present.** GX textures no longer announce mip levels they do not have, and the window no longer
  samples through the game's last samplers.

Tested on a Galaxy Z Fold8 (Adreno 840) with Prince of Persia: The Two Thrones, at full speed with 1x and close to it
with 2x internal resolution.

**Known problems**

- 4x internal resolution is too heavy for that phone in 3D scenes (about 60 % speed).
- Other GameCube games and other GPUs have not been tested with these changes.

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### GX

- no announced mip chain or stale samplers at present (6e174264)
- cull the faces the hardware culls (ca5874fa)
- switches to diagnose stale XFB pictures on Adreno (d5e5490f)
- fenced persistent buffer rings, flush after XFB copies by default (2c6a21a3)
- the XFB diagnostic's slot fields hold any int (333d336e)

### Window

- the present sets the colour state it depends on (e75b71ad)

<!-- release:changes end -->
