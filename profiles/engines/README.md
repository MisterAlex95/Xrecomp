# Engine layers

None ships, and `tools/ci/layer_guard.py` fails a shipped one: what an engine needs is a toolkit default, found
from the XBE or decided at run time. The last one, `renderware` (DMA walk, 60 fps cap, frame generation off, for
Burnout 3 and Black), went in WP4: the DMA walk is the toolkit's default (the D3D service), and the pacing and frame
generation decisions are made at run time (`RECOMP_PACING=auto`, the lock verdict, `docs/technical/framegen.md`).

The mechanism stays for a developer's local A/B: a folder named as the `layer` of its entry in
`tools/xbe_ident/engines.json` (`renderware`, `unreal2`, ...), holding a `layer.json` and optional `.c` files, picked
when tools.xbe_ident finds the engine in the XBE. Rules: `docs/technical/profiles.md`.
