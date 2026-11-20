# Profiles

A title needs nothing here: the toolkit's defaults (the XDK switches, the D3D service and its DMA walk, run-time
pacing), the analysis and the discovery loop run it, and a title with no layer is the normal case. **No layer ships.**

- `titles/<title id>/`: empty, and kept so by `tools/ci/layer_guard.py` (XBA-236). One game's enhancement that cannot
  be derived (a 16:9 picture, a faster front end) is a player's mod (`docs/technical/mods.md`), kept outside the
  toolkit; the last title layer, Fable's, became one on 2026-11-20. The mechanism stays for a developer's local A/B
  (`tools.prepare --no-title-layer` leaves such a layer, and the mods, out).
- `engines/`, `xdk/`: no layer ships (what they did is the toolkit's default); the mechanism stays for a developer's
  A/B. `xdk/<build>/xmv.json` holds masked signatures of the XDK's XMV API for `tools.xmv.find_api` (data, not a
  layer; written by its `--record`, none yet).

Our code only (no game code, no game data). Rules and keys: `docs/technical/profiles.md`.
