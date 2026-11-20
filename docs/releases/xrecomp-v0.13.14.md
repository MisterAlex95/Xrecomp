# XRecomp 0.13.14

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### App

- How it works, a four-page onboarding on Prepare and Prepare again (6b69f7f2)
- first start, Help and questions, Good to know bubbles (XBA-326) (14f6bd42)
- Help's drawings, Home's track, cards' new code, the bottom screen's guide (XBA-326) (7b6f3237)
- Translation, live and a game's journal (XBA-326) (6daa980c)
- the network library in the first start and Help (XBA-326, XBA-319) (5a0bd9e8)

### Docs

- How it works in the launcher's screens, its device check (XBA-326) (04ece2e9)

### Mods

- mod.json schema, per-title store and load order, mod-set hash in Xbox save states (MOD-1, XBA-286) (438e9ba2)
- patches before Prepare and the display hint on Xbox (MOD-3, XBA-288) (468accfd)
- none for a disc's other images, the mods' binary in RECOMP_MOD_FOR (XBA-288) (c5ec7eee)

### Network library

- read .torrent files and magnets, list a torrent's games (XBA-320) (ec1d0407)
- torrent games in the library, downloaded on A, then set up (XBA-320, XBA-321) (882cdb55)
- hybrid torrents, cancel by the service, host check against real libtorrent (XBA-321) (eb1c880e)
- after the review: resume data, service-only cancel and remove, safe auto-continue (XBA-321) (bd2c7889)
- show or hide the network's games, a Show pill for local / network (XBA-320) (6f45357b)

### Profiles

- Fable's title layer leaves the toolkit, no title layer ships (XBA-236, XBA-285) (4dfa2b8d, XBA-274)

### Tests

- a completed USB transfer whose interrupt is still pending is served before its wait times out (XBA-327) (93cdff71)

### Xbox states

- a same-run load re-enters threads at other call chains (XBA-294) (f294ae0f, XBA-250)

<!-- release:changes end -->
