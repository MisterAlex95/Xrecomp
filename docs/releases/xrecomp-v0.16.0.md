# XRecomp 0.16.0

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

Reports: you can tell us what goes wrong, with or without a crash, and you choose what leaves your device.

- **Report a problem.** Most bugs in a recompiled game do not crash: a black screen, broken textures, no sound.
  In the game menu (and Settings › Advanced), one line of text and the game's log go to our tracker. You can add a
  screenshot: it is off by default and you see it before sending. There is no name or e-mail field.
- **The game's last steps** go with every report, so we can replay what happened: save or load a state, a cheat
  turned on, the menu, a controller plugged in. Only fixed event names and numbers are sent, never a game, save or
  file name.
- **Send reports automatically** (Settings › Advanced › Privacy), **off by default**. Turn it on and crash reports
  leave by themselves on Wi-Fi, each game run counts toward the crash-free rate of the version, and the Prepare
  sends how long each of its stages took on your device. While it is on, a random id that you can reset stands for
  your install; turning it off deletes the id and anything still waiting. A « Good to know » bubble offers it once
  after a crash.
- **On the Mac too.** The macOS launcher sends a report about the last game or a failed Prepare, like the app
  (Linux and Windows have no launcher).
- Every report has storage paths, home folders, addresses and links removed before it leaves. No game data is
  ever sent.
- **Xbox 360:** XMA audio through the APU, fewer clipped pictures, and an HLE that reads a bad address stops the
  title, not the app.
- **Launcher:** game art is cached and decoded off the UI thread, so the library scrolls more smoothly.

**Known problems**

- The Mac launcher's report screens are new and have only been built by CI, not tried by hand yet.
- The screenshot is taken while the menu holds the game: on some renderers there may be no frame (the box then
  unticks itself).

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### CI

- a push to main builds its APK at once and cancels the run it supersedes (87e7895a)
- infra monitors in Rustrak (XBA-343): host disk and the weekly run (0344a969)

### Disasm

- the gap sweep steps over a known entry by its decode (SpikeOut Story black screen, GitHub #3) (1caa501a)

### docs

- teaser video (MP4 + GIF) for the README (XBA-344) (1941f847)
- what each Prepare timing span holds (XBA-341) (7976b2d2)
- teaser without the Prepare screen (XBA-344) (de60c832)
- teaser v2, motion cut with music and a dual-screen handheld (XBA-344) (31bcc32f)

### Docs

- Report a problem and the breadcrumb allow-list (XBA-339, XBA-340) (82fc3e5a)

### Launcher

- a cover found updates its card, one cover worker per process (64235673)
- the library polls' model work off the main thread (0e17a0f0)
- game art through a bounded process-wide cache, decoded off the UI thread at card size (d536c564)

### macOS launcher

- Send report, like the Android app (XBA-342) (d69797e5)

### macOS reports

- one game key form on every path; matrix wording (XBA-342) (70b4f3fd)

### Merge fixes

- one release()/environment() pair in CrashReport, Privacy after Report a problem (fea28c0d)

### Prepare

- timings per stage as a performance transaction, opt-in (XBA-341) (43fe0b93)

### README

- public showcase before the announcement (XBA-344) (78658e72)
- the teaser under the hook, the GIF linking to the 1080p MP4 (XBA-344) (16f9e94c)
- the teaser's caption matches the new cut (24 s, no Prepare screen; XBA-344) (a134726b)

### Reports

- shared client plumbing (XBA-338): breadcrumbs, consent gate, feedback, session and transaction items (7719f0c1)
- opt-in automatic reports and release health (XBA-336) (07ed3ad8)
- scrub desktop home folders too (XBA-342) (4eae4ff7)
- count a launch once, per-title Prepare dedupe, privacy sheet test and shorter text (XBA-336) (f58226c1)
- the Rustrak wave (XBA-336, XBA-339 to XBA-342) makes a minor release (b5287c43)

### Send report

- a failed Prepare or the last game's run, on the player's tap (XBA-335) (00343e12)

### x360

- a depth surface the draw does not use no longer clips the picture (b3a65321)
- an HLE that touches an unmapped guest address stops the title, not the process (WP8) (4eee044c)
- x360_run and the x360 tests build and run on macOS (1ac52c6f)
- the audio client callback receives a pointer to its argument (3327c6b2)
- XMA through the APU registers (107e15a5)
- a PM4_INTERRUPT handler runs as the CPU the packet names (9e19e8d5)
- RtlNtStatusToDosError translates the statuses the HLE returns (198bb80c)
- a spin-wait's priority hint ends the time slice and charges virtual time (ba9ce516)
- --dump-shaders remembers what it has written (no file system call per shader load) (0de5d6c3)
- computed-index vertex buffers cached, no allocation per draw (WP-5) (bd23a56b)

### X360

- shader constants are uploaded only when they changed (7b6f64bc)

### x360 perf line

- GPU, resolve read / write, draw counters; X360_ANGLE_DIR loader for the Mac (WP-0) (6af6386b)

### x360 resolves

- a byte path into guest memory, reused read-back buffers (WP-1) (819693c2)

### x360 toolkit

- jump tables whose code holds a nop are resolved (1788540b)
- twi / tdi / tw / td lifted as their test (d4e8ab40)

### Other

- Report a problem and game breadcrumbs (XBA-339, XBA-340): call sites, menu page, launcher sheet (2637a58c)

<!-- release:changes end -->
