# Profiles

Optional enhancements for a title, on top of the generic title template. A title needs nothing here: the toolkit's
defaults (the XDK switches, the D3D service and its DMA walk, run-time pacing), the analysis and the discovery loop
run it, and a title with no layer is the normal case.

- `titles/<title id>/`: one game's enhancements that cannot be derived (a 16:9 mod, a faster front end). Left out
  with `tools.prepare --no-title-layer`, `XBA_NO_TITLE_LAYER=1`, or the launcher's / app's « Enhancements » switch;
  the title still runs. Today: `titles/4d5300d1` only.
- `engines/`, `xdk/`: no layer ships (what they did is the toolkit's default); the mechanism stays for a developer's
  A/B. `xdk/<build>/xmv.json` holds masked signatures of the XDK's XMV API for `tools.xmv.find_api` (data, not a
  layer; written by its `--record`, none yet).

`tools/ci/layer_guard.py` keeps it that way in CI. Our code only (no game code, no game data). Rules and keys:
`docs/technical/profiles.md`.
