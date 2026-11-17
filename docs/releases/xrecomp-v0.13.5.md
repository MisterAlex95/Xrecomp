# XRecomp 0.13.5

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Kernel

- NtDuplicateObject(NtCurrentThread()) is a real token or a clean failure (6dabd01c)

### Win32 shim

- DuplicateHandle of the current thread is a real handle (0e95bd99)

<!-- release:changes end -->
