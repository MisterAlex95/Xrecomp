# Runtime Implementation Guide

> Part of the [Documentation Index](../INDEX.md)

The recompiled C code needs a host environment to run. This directory documents the four major runtime subsystems you need to build for each game.

## The Four Pillars

### 1. Memory Layout (`xbox_memory.c`)

Reproduce the Xbox's 64 MB address space at the correct virtual addresses. This is the most critical piece — get it wrong and nothing else works.

See: [../technical/memory-layout.md](../technical/memory-layout.md)

Template: [../../templates/runtime/xbox_memory.h](../../templates/runtime/xbox_memory.h)

### 2. Kernel Shim (`xbox_kernel.c`)

Replace the 147 Xbox kernel imports with Win32 equivalents. Most can be stubbed (return STATUS_SUCCESS). The critical ones are memory allocation and file I/O.

See: [../technical/kernel-replacement.md](../technical/kernel-replacement.md)

Template: [../../templates/runtime/kernel_stubs.h](../../templates/runtime/kernel_stubs.h)

### 3. Graphics Translation (`d3d8_to_d3d11.c`)

The Xbox uses a modified Direct3D 8. You need a translation layer to D3D11 (or D3D12, Vulkan, etc.). This is the largest runtime component.

See: [../technical/d3d-translation.md](../technical/d3d-translation.md)

### 4. Input System (`xbox_input.c`)

Map Xbox controller input to Windows XInput. This is usually the simplest piece — Xbox controllers on Windows are nearly 1:1.

## Build System

We recommend CMake with MSVC. Structure your project as static libraries:

```
src/
├── kernel/     → xbox_kernel.lib
├── d3d/        → d3d_translation.lib
├── audio/      → audio_stubs.lib
├── input/      → input_layer.lib
└── game/       → your_game.exe (links all the above + recompiled code)
```

## Game-Specific Components

Beyond the four pillars, each game needs:

- **Manual function overrides** — hand-written replacements for functions that don't work when mechanically translated (physics, rendering orchestrators, hardware-specific code)
- **Asset loaders** — each game/engine has its own file formats for textures, models, levels, audio, etc.
- **Game-specific workarounds** — vtable guards for corrupted pointers, state machine patches, etc.

These go in the game-specific repo, not in the generic xboxrecomp toolkit.

## POSIX hosts (macOS, Linux)

The template's `main.c` is Win32: a vectored handler that calls
`xbox_VehMmioFault` and a `WinMain`. A POSIX build needs the same two pieces
in its own `main()`:

```c
#include <signal.h>
#include <ucontext.h>
#include "kernel/xbox_memory_layout.h"   /* xbox_PosixMmioFault */
#include "video/video_player.h"          /* xbox_HostWindowMain */

/* Trapped hardware registers (APU, AC'97, USB OHCI) fault here. Return when
 * the access was serviced: the program counter has been advanced past it. */
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    if (xbox_PosixMmioFault(uc, (uintptr_t)si->si_addr))
        return;
    signal(sig, SIG_DFL);                /* a real crash: report and die */
}

static int boot_and_run(void)
{
    /* xbox_MemoryLayoutInit, kernel init, the guest entry point, ...,
     * as in the template's WinMain. */
    return 0;
}

int main(void)
{
    struct sigaction sa = { 0 };
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);        /* macOS reports a protected page so */
    /* With RECOMP_FB_WINDOW set this opens the framebuffer window on the
     * main thread (where macOS requires it) and runs boot_and_run on a second
     * one; otherwise it just calls boot_and_run. */
    return xbox_HostWindowMain(boot_and_run);
}
```

`xbox_PosixMmioFault` decodes the faulting instruction on arm64 hosts only;
on x86-64 POSIX it always returns 0, and the traps are not armed there.

## Quick reboot (a title restarting itself)

On a console `XLaunchNewImage` fills the launch data page (kernel export 164,
`LaunchDataPage`) and calls `HalReturnToFirmware(2)`, a quick reboot; the page
survives it and the next image reads it with `XGetLaunchInfo`. A title that
launches its own `default.xbe` is restarting itself: Fable's Pause → Quit →
Confirm goes back to its main menu that way (launch path
`\Device\CdRom0;default.xbe`, type 0, launch data starting `42000100`).

The runtime does the same across a host restart (`src/kernel/kernel_reboot.c`):

1. `bridge_HalReturnToFirmware` (`kernel_bridge.c`), routine 2 with a page
   whose path names the running image (`XeImageFileName`, compared without
   case, `dir;file` joined to `dir\file`): `xbox_quick_reboot` writes
   `<save dir>/launch_data.bin` (magic `XBLD`, version 1, the times of the
   recent restarts, then the 4096-byte page; written aside and renamed) and
   calls the restart handler. Log: `[KERNEL] quick reboot: launch data in …,
   restarting the title (N in the last 30 s)`.
2. Kernel init (`kernel_data_init`) takes the file back: the page goes into
   contiguous memory and export 164 points at it, the file is deleted (it is
   one-shot, like the console's page). Log: `Launch data: quick reboot page at
   0x…, type=… titleid=… path='…'`. It wins over `RECOMP_CMDLINE`, which the
   restarted process still has in its environment.
3. The cap: past 3 restarts in 30 s (`XBOX_RESTART_MAX`,
   `XBOX_RESTART_WINDOW_S`; the times travel in the file) the title exits as
   before (`quick reboot refused`), so a title that reboots into itself at
   once cannot loop.
4. Another image of the disc goes through `xbox_launch_image` (XBA-82,
   [docs/technical/multi-xbe.md](../technical/multi-xbe.md)). An empty path
   (XapiBootToDash: the dashboard), no page, any other routine, and whatever
   could not be handed over end the run (« The title leaving », below).

`xbox_SetRestartHandler(fn)` (`kernel.h`) sets how the process restarts. The
default, on desktop hosts:

- macOS / Linux: `execv` of the running binary (`_NSGetExecutablePath`,
  `/proc/self/exe`) with the same arguments and environment. Every descriptor
  past stderr is marked close-on-exec first (partition images, caches, audio,
  window): their writes are already the kernel's, so the new process reads
  them back. stdout and stderr carry on into the same files, and a pending
  `alarm` survives the exec, so a timed headless run stays timed.
- Windows: `CreateProcessW` with the same command line, no inherited handles,
  then `ExitProcess(0)`.

Android has no default; the app shell sets one ([docs/android/app-shell.md](../android/app-shell.md),
« Quick reboot »). When the handler returns, the restart did not happen: the
launch data file is removed and the title exits as before.

With `RECOMP_PAD_LIVE` the restarted process reads the pad file from the start
again; a driver script truncates it when it sees the `quick reboot:` line.

Measured on the Mac with Fable (AutoSave loaded, Pause → Quit → Confirm, three
times in a row): each restart came back to the main menu with « Default -
Continue Game » selected, without the « Press START » screen, the logos or the
intro (no `.xmv` opened after the first boot), within 15 s of the quit on a
loaded machine; the save loaded again from that menu each time.

`tests/kernel_reboot`: the file round trip, `xbox_launch_data_take` (once,
then deleted; a corrupt file deleted too), the path match, the cap, and on
POSIX the default handler end to end (a forked child re-executes the test,
which takes the page back and checks that a descriptor opened before the exec
is closed).

## The title leaving

`HalReturnToFirmware` with nothing to hand over to, and
`HalInitiateShutdown`, end the run through `xbox_title_exit(routine, why)`
(`kernel_reboot.c`). The bridge words the reason from the launch data page
(`xbox_exit_reason`):

| Case | Reason |
|---|---|
| empty launch path, or no page | `the game returned to the dashboard (routine N)` |
| empty path, XapiBootToDash's reason (first launch-data dword) non-zero | `… (routine N, dashboard reason R)` |
| another image this build has no target for | `the game asked to start <path>, which this build cannot start (routine N)` |
| `HalInitiateShutdown` | `the game shut the console down` |

A refused or failed restart into the running image reads as the dashboard.
`xbox_title_exit` prints `[EXIT] <reason>`, calls the exit handler
(`xbox_SetExitHandler`, `kernel.h`; the Android shell's opens the launcher
with the reason, [docs/android/app-shell.md](../android/app-shell.md) « The
title leaving by itself »), then ends the process: on Android with
`recomp_quit(0)` (exit hook, flush, log drain, `_exit`), because the guest's
other threads, SDL's and the GPU executor are still running and `exit()`'s
atexit handlers and destructors would pull their mutexes away (it aborted with
`FORTIFY: pthread_mutex_lock called on a destroyed mutex`); elsewhere with
`ExitProcess(0)`, as before (status 0). Desktop hosts set no handler.
`tests/kernel_reboot` checks the wording and, on POSIX, that a forked child's
handler gets the reason and the process then exits with status 0.

## DVD security sector

XAPI start-up of a title whose certificate has bit 0 set in the dword at
+0x1D8 (only certificates longer than 0x1D8 bytes have that field; KOTOR's is
0x1EC bytes, value 3) reads the disc's security sector with
`IOCTL_SCSI_PASS_THROUGH_DIRECT`, READ DVD STRUCTURE (SCSI 0xAD, 1636 bytes: a
4-byte header and the 1632-byte sector), and returns to the dashboard
(`HalReturnToFirmware(2)`, empty launch path) unless it matches the
certificate. There is no disc, so `bridge_NtDeviceIoControlFile`
(`bridge_dvd_security_sector`, `kernel_bridge.c`) synthesises what the
title's check asks for and nothing else (zeros elsewhere): the certificate's
time stamp (cert +4) at +0x4AB; at +0x4CF the SHA-1 of the dword 0x4CB
followed by the 0x4CB bytes at +4; the signature at +0x4E3 is accepted by
`XcVerifyPKCS1Signature`, which answers TRUE. The certificate is the one the
XBE header at 0x00010118 points to. Other commands, a shorter buffer, or a
certificate without the bit get a zeroed buffer as before. Log:
`[FILE] DVD SCSI command 0xAD, 1636 bytes -> security sector for this title's
certificate`. Measured on the Mac with KOTOR (folder with only its
`default.xbe`): the start-up check passes and the title goes on to open
`d:\DATAXBOX\…`; Black issues no DVD SCSI command. `tests/kernel_dvd_scsi`
covers the sector (time stamp, digest against a hashlib value, zeros
elsewhere, nothing past the buffer) and the zeroed cases.
