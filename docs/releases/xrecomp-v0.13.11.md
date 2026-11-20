# XRecomp 0.13.11

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

An Xbox 360 game that faults now leaves its cause in the log.

- **Xbox 360: no more silent exit.** On a fault the runtime wrote the guest address to a log pipe and quit with
  status 1. The line was lost with the process, and Android recorded a plain exit with no backtrace. The line now
  reaches the log first, and the fault goes on to Android's crash handler: a native crash with its backtrace, which
  « Share the last game's log » (Settings › Advanced) picks up.
- **Releases publish 15 minutes sooner.** The GitHub step waited for the tag to arrive, but compared an annotated
  tag's object with the commit, so it always ran out its full wait.

**Known problems**

- Warhammer: Battle March (Xbox 360) still stops on the Thor a few seconds after the Bandai logo. This release is
  the one that says where: play until it stops, then send the last game's log.

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### X360

- a fault reaches debuggerd with its line in logcat, no silent exit(1) (1ea43b44, XBA-52, XBA-318)

<!-- release:changes end -->
