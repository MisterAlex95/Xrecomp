# XRecomp 0.13.5

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

A fix release.

- **Save states work again in titles whose main thread keeps a handle to itself** (Yu-Gi-Oh! The Dawn of Destiny):
  every capture was refused (« handle ...: names no live host object »), so rewind turned itself off, the in-game
  Save state failed and the game never offered « Resume ». A duplicate of the current thread is now a real handle to
  that thread on every thread, the main one included.
- The kernel never stores a pseudo handle in its handle table, and the save-state log names such a case as a runtime
  bug if it ever comes back.

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Kernel

- NtDuplicateObject(NtCurrentThread()) is a real token or a clean failure (6dabd01c)

### Xbox save states

- a pseudo handle in the handle table is named as a runtime bug (409e2f44)

### Win32 shim

- DuplicateHandle of the current thread is a real handle (0e95bd99)

<!-- release:changes end -->
