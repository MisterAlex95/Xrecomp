# XRecomp

Console games statically recompiled to native code: original Xbox today, GBA / PS1 / N64 under way (`consoles/`).
Fork of [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) (remote `upstream`, our work on `main`) that adds
**Android arm64** as a host (NDK build, OpenGL ES 3.0, the XRecomp app with on-device Prepare), a macOS launcher
(`macos/`) and a console-agnostic host layer (`src/host`). Hosts: Android, macOS, Linux (x86-64 / arm64), Windows.

**Vision, roadmap, decisions, spec: Plane project XBA** (`plane page show -p XBA "Vision"`, « Roadmap »,
« Spec · Android port », « Decisions taken »). Work queue: `plane next -p XBA`. Organisation (agents, states):
Plane ORG « Organisation du travail ».

## Language
Work language English (code, comments, commits, Plane). Chat with Alex in French.

## Rules
- **Legal**: never commit, download or store game code, XBEs, disc images or game data. Test with our own XBEs (nxdk)
  or data Alex dumped from his own discs, kept outside the repo. Per-title repos (generated C) stay private.
- Upstream tests stay green: a change must keep them passing on Linux (CI: `tools/docker/linux.Dockerfile`). We take
  from upstream (`tools/sync_upstream.sh`) but send nothing back: no PRs or issues on upstream's repository
  (Alex, 2026-11-09).
- **Public**: Gitea is the dev home; it push-mirrors everything to the public GitHub repo MisterAlex95/Xrecomp
  (2026-11-14), which overwrites GitHub's branches: never merge on GitHub. Everything committed is public: no
  private hosts, IPs, credentials or personal data. Releases: `docs/releases/README.md`.
- A title runs on Linux arm64 before we debug it on Android.
- Agents cannot test on a device: label « Device » + a numbered check list for Alex.

## Upstream landmarks
- Libraries in `src/` (kernel, d3d, nv2a, apu, audio, input, usb, video, platform); the repo builds libraries only, a game
  project from `templates/new-game/` builds the executable.
- Pipeline in `tools/` (Python): XBE parse → disasm → func_id → abi_analysis → recomp. Guide: `docs/GETTING_STARTED.md`.
- Build: `cmake -S . -B build && cmake --build build`; tests are CMake executables under `tests/` (exit 77 = skipped).
- Headless runs: always `timeout`, kill by PID.
