# Engine layers

One folder per engine, named as the `layer` of its entry in `tools/xbe_ident/engines.json`
(`renderware`, `unreal2`, ...), holding a `layer.json` and optional `.c` files, as the XDK
layers do. Picked when tools.xbe_ident finds the engine in the XBE. Empty for now: no fix
seen so far is true for a whole engine. Rules: `docs/technical/profiles.md`.
