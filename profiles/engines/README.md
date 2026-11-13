# Engine layers

One folder per engine, named as the `layer` of its entry in `tools/xbe_ident/engines.json`
(`renderware`, `unreal2`, ...), holding a `layer.json` and optional `.c` files, as the XDK
layers do. Picked when tools.xbe_ident finds the engine in the XBE, or when a title lists it in
`"layers"`. Rules: `docs/technical/profiles.md`.

None is shipped. The last one, `renderware` (DMA walk, 60 fps cap, frame generation off, for
Burnout 3 and Black), was removed in WP4: the DMA walk comes from `xdk/d3d8ltcg`, and the pacing and
frame generation decisions are made at run time (`RECOMP_PACING=auto`, the lock verdict,
`docs/technical/framegen.md`). The mechanism stays for a developer's A/B layer.
