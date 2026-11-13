# Profiles

What `tools.prepare` adds to the generic title template, in layers: `engines/<engine>/`, `titles/<title id>/`.
No `xdk/` layer ships: the XDK switches, the D3D service and the DMA walk are the toolkit's defaults
(`src/kernel/recomp_xdk_defaults.c`, `src/kernel/recomp_d3d_service.c`). Our code only (no game code, no game data). Rules and keys: `docs/technical/profiles.md`.
