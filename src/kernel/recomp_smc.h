/* Code written or run at run time (XBA-118): detect it, report it.
 *
 * Static recompilation runs the C lifted from the XBE's code as it was on
 * disc. Two things it cannot see, and that no fallback here runs (no
 * interpreter, no JIT: docs/pipeline/04-lifting.md):
 *
 *   exec_outside_image  an indirect call or jump to an address outside the
 *                       image's code: heap, stack, the image's data, the
 *                       contiguous window (decompressed or generated code).
 *                       The macros skip it, as before; xbox_SmcExecOutside
 *                       names it.
 *   code_write          a write into a page of a lifted code section: patched
 *                       thunks, copy protection, code unpacked in place. The
 *                       lifted C still runs the old bytes.
 *
 * Each is written once to the discovery file (recomp_discovery.h):
 *   {"t":"smc","kind":"exec_outside_image","va":"0x...","ret":"0x...","where":"heap","bytes":"5589E5..."}
 *   {"t":"smc","kind":"code_write","va":"0x<page>","at":"0x<address>","section":".text","by":"guest"}
 * and printed once ([SMC] ...). tools.compat names them in the blocker.
 * "bytes" is the first 8 bytes at the target, as hex: code the title made at
 * run time, a short prefix only, to tell code from a wild pointer.
 *
 * The code pages: after the loader is done (xbox_SmcArm, right before the
 * guest's entry point), every host page lying wholly inside the initialised
 * part of an executable, non-writable section and touching no other section
 * is made read-only in the guest view. The first write to one faults;
 * xbox_SmcFault (an early, separate test at the top of the POSIX fault
 * handler, xbox_memory_layout.c) records it, makes the page writable again
 * and the write is retried: nothing is paid after the first write per page.
 * Left writable, so not seen: the pages a code section shares with data
 * (section edges, host pages larger than 4 KB), sections marked writable as
 * well as executable (RECOMP_SMC_WX=1 protects them too), BSS tails, writes
 * through another view of the same memory (the RAM mirrors). Host writes into
 * guest memory by a system call (read() into a code page) cannot fault into
 * the handler: xbox_SmcHostWrite opens the pages first (NtReadFile does).
 *
 * RECOMP_SMC=0 protects nothing (A/B); exec_outside_image is still named.
 * POSIX only (Linux, macOS, Android); nothing is protected on Windows. */
#ifndef XBOXRECOMP_RECOMP_SMC_H
#define XBOXRECOMP_RECOMP_SMC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The loader: forget the previous image (and open its pages), then each
 * section as it is loaded (flags: the XBE section flags; raw: its initialised
 * size). */
void xbox_SmcReset(void);
void xbox_SmcNoteSection(uint32_t va, uint32_t vsize, uint32_t raw, uint32_t flags, const char *name);

/* Write-protect the code pages. Right before the guest's entry point, once
 * the fault handler is installed. Returns the number of host pages protected
 * (0 when off, on Windows, or with no fault handler). */
int xbox_SmcArm(void);

/* The fault handler's early test: 1 when host address `fault` is in a
 * protected code page (it is recorded and opened: retry the access). */
int xbox_SmcFault(uintptr_t fault);

/* Before the runtime writes [p, p + len) of guest memory from a system call
 * (read() into a guest buffer): opens any protected code page in it, recording
 * a code_write by "host". */
void xbox_SmcHostWrite(const void *p, size_t len);

/* An indirect call or jump to `target` that is not in the image's code,
 * from a call whose return address is `ret` (0: unknown). 1 when it is
 * mapped guest memory (heap, stack, image data, contiguous window) and was
 * named as exec_outside_image; 0 for a wild pointer (null page, unmapped). */
int xbox_SmcExecOutside(uint32_t target, uint32_t ret);

/* 1 when [va, va + len) touches a section that is executable and not
 * writable (lifted code: writing it changes nothing the lifted C runs, and
 * its pages may be write-protected). The cheats skip those (xbox_cheats.h). */
int xbox_SmcInCode(uint32_t va, uint32_t len);

/* Records made by this process (both kinds) and protected pages left. */
unsigned xbox_SmcCount(void);
unsigned xbox_SmcProtectedPages(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_SMC_H */
