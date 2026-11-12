# Engine layers

One folder per engine, named as the `layer` of its entry in `tools/xbe_ident/engines.json`
(`renderware`, `unreal2`, ...), holding a `layer.json` and optional `.c` files, as the XDK
layers do. Picked when tools.xbe_ident finds the engine in the XBE, or when a title lists it in
`"layers"`. Rules: `docs/technical/profiles.md`.

| Layer | Holds | Seen on |
|---|---|---|
| `renderware` | DMA walk, title-built KEVENTs, 60 fps cap, frame generation off | Burnout 3 (Black next, XBA-92) |
