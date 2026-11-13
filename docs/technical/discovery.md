# The discovery file

> Part of the [Documentation Index](../INDEX.md)

Static analysis cannot see every function: indirect-call targets, thread start routines, code reached through
computed jumps. A run meets them. The runtime records them in a file in the prepare's work folder, so the next
prepare can seed them instead of a human reading the log (XBA-64; the loop that uses it: XBA-65, XBA-74).

## Where

`$RECOMP_DISCOVERY` (the environment or `recomp.cfg`). Unset, empty or `0`: nothing is written.

| Host | Default |
|---|---|
| Desktop, a project made by `tools.prepare` | `<work>/discovered.jsonl` (CMake `XBA_DISCOVERY_FILE`, read by `main.c`) |
| Android | `<files dir>/work/<TITLEID>/discovered.jsonl`, the Prepare's work folder (`pack_loader.c`) |

## Format

One JSON object per line (JSON Lines). Each entry is appended as it happens (one write, no fsync), so a crash of the process keeps
every entry written before it; a reader skips a torn last line. An entry already in the file (same kind and address, from
this run or an earlier one) is not written again: the file only grows with what is new.

```
{"t":"run","time":"2026-11-11T18:00:00Z"}                    a process wrote entries from here on
{"t":"icall","va":"0x0012ABCD","ret":"0x00123456"}           indirect call to code with no dispatch entry; ret = the caller's next instruction
{"t":"thread","va":"0x00034560","ctx1":"0x00045670"}         thread start routine with no dispatch entry
{"t":"thread_ctx","va":"0x00045670","routine":"0x00034560"}  StartContext1 inside the title's code with no dispatch entry
{"t":"unimpl","va":"0x00056780","insn":"bsf eax, ecx"}       untranslated instruction reached (RECOMP_UNIMPL)
{"t":"d3d","va":"0x0085FDE0","slot":"0x00862288"}            D3D's device found at run time (profiles/xdk/d3d8/d3d8.c)
{"t":"wait_spin","va":"0x0027B91C","ret":"0x00025701","header":"0x00040001","status":"0xC0000001"}
                                                             a thread waited on it 2,000 times in a second (kernel_bridge.c)
{"t":"launch","va":"0x5A1C33E0","path":"d:\\game.xbe"}           the title launched another image (XBA-82); va: a hash of the path
{"t":"stack_switch","va":"0x00023454","to":"0x00023480","why":"..."}
                                                             a guest stack switch the runtime could not carry (XBA-101):
                                                             va = the esp load, to = where its ret went
                                                             (stack-switching.md); or an exception handler's
                                                             resume refused because its frame is on another
                                                             host stack (XBA-112): va = the resume target,
                                                             to = its function (seh-handling.md)
{"t":"stub","ord":136,"name":"KeRemoveQueue","ret":"0x00012345"}
                                                             a kernel route that is still a stub (or has no route) was called;
                                                             keyed by ord, no va (XBA-115, src/kernel/kernel_stubs.h)
{"t":"smc","va":"0x00400000","kind":"exec_outside_image","ret":"0x00011234","where":"heap","bytes":"5589E583EC10C390"}
                                                             an indirect call or jump into mapped guest memory outside
                                                             the image's code (where: heap, stack, image_data,
                                                             contiguous, kernel); skipped as before (XBA-118)
{"t":"smc","va":"0x00012000","kind":"code_write","at":"0x00012345","section":".text","by":"a store","ret":"0x..."}
                                                             the first write into a lifted code page: va = the page,
                                                             at = the address written, by = a store or a system call,
                                                             ret = the word at the guest stack top (XBA-118)
{"t":"exception","va":"0x00012350","code":"0xC0000094","why":"divide by zero"}
                                                             a guest exception nothing handled (XBA-129): a lifted
                                                             divide error that stops the title (kernel_hal.c) or a
                                                             raise no handler took (xbox_seh.c), or a host fault on a guest
                                                             address no guest handler took (XBA-132)
{"t":"d3d_layout","va":"0x1A2B3C4D","lib":"D3D8LTCG","build":5933,"rule":"fence_event","used":"0x00001DCC"}
                                                             the XDK D3D layer met a D3D build its layout table does not
                                                             know and used another build's value for that rule (XBA-105,
                                                             profiles.md « Other XDK builds »); va: a hash of lib, build, rule
```

`bytes` is the first 8 bytes at the target of an `exec_outside_image`, as hex. They are code the title made at run time
(decompressed, generated or copied), not bytes of the XBE, and only a short prefix, enough to tell code (a prologue)
from a wild pointer into data; nothing longer is ever written. Rules and the write protection behind `code_write`:
`src/kernel/recomp_smc.h`.

Addresses are guest VAs, 8 hex digits. At most 6,144 distinct entries per file.

The first `icall`, `thread` or `thread_ctx` entry a process writes is also said on stderr, once: `[DISCOVERY] first
code the analysis missed in this run: icall 0x0012ABCD (skipped; the next Prepare adds it)`. A skipped indirect call
does not crash, the title goes on (maybe wrong): that line, the entry, and the status line's `seeds` (how many such
entries this process wrote, `src/kernel/recomp_status.h`) make the miss visible without a crash (XBA-129).

## Who reads it

- `python -m tools.seed_from_log <work>/discovered.jsonl <xbe> --functions ... --seeds ...` takes the `icall`,
  `thread` and `thread_ctx` addresses through its gates (executable section, decodes as a function body: an address
  is never seeded just because a run mentioned it).
- `unimpl` entries are not seeds: they feed the lifter coverage report (XBA-71). Nor are `stack_switch` entries:
  they name a blocker ([stack-switching.md](stack-switching.md)).
- `stub` entries are not seeds either: `tools.compat` adds « calls stub ordinal N (Name) » to the run's blocker
  (XBA-115).
- `smc` entries are not seeds: what they name is not in the XBE as lifted. `tools.compat` and the device grader add
  « code written or run at run time xN: exec_outside_image 0x... (heap, from 0x...), code_write 0x... (.text) » to the
  blocker, whatever the stage, and the result line carries the count (`"smc": N`); the Compatibility page counts the
  titles that have any (XBA-118). An `exec_outside_image` used to be an `icall` entry when it came through the lookup,
  and a seed candidate.
- `d3d_layout` entries are not seeds: they say what to add to `k_layouts` in `profiles/xdk/d3d8/d3d8.c` for that
  build (XBA-105); `tools.compat` (and the device grader) add « D3D layout guessed for D3D8LTCG 5933:
  fence_event, ... » after a blocker other than a crash or a launch (`tools/compat/blockers.py`).
- `tools.prepare` reads it before analysing (XBA-97, `tools/prepare/discovered.py`): new `icall`, `thread` and
  `thread_ctx` addresses go through the same gates, the ones that pass are kept in `<work>/discovered-seeds.json` and
  handed to the disassembler, the others in `<work>/discovered-rejected.json` so they are not tried again. A new seed
  changes the stamp: the next prepare analyses and lifts again (all of it for now; only what changed is XBA-65).
- The Android app, after every run (XBA-74, XBA-129, [prepare.md « Self-healing »](../android/prepare.md)): new
  `icall`, `thread` or `thread_ctx` entries since the last Prepare → a Prepare that takes them in (at once and a
  relaunch after a crash, in the background after a run that ended well); a crash with `exception`, `stack_switch` or
  `unimpl` entries, or with nothing new → the title needs a fix, named.
- `tools.compat heal` (the same loop on a desktop) and `tools.compat recall` (the share of run-time targets the static
  analysis found, the misses' categories): [compat-runner.md](compat-runner.md#self-healing-rounds-and-recall-xba-129).

The whole loop runs in CI on a synthetic title of our own (`tools/compat/test_e2e.py::test_the_self_healing_loop`,
through `tools.compat heal`): its thread routines are invisible to the static analysis (their addresses are
computed); each run records the next one as a `thread` entry, the next prepare seeds it and rebuilds, and the run
after starts it; the third is not code, the gates refuse it, and the loop stops after that round without progress.

## Static discovery at the first Prepare (XBA-137)

Every entry above costs the player a crash or a wrong behaviour, then a re-prepare of minutes on a phone. So the
analysis looks, before any run, for what a run would reveal (`tools/disasm/discovery.py`, run by
`tools/disasm/functions.py`):

| Pass (`functions.json` method) | Evidence | Gates |
|---|---|---|
| noreturn kernel calls | `call [HalReturnToFirmware / KeBugCheck(Ex) / PsTerminateSystemThread]` ends a body, so the function packed after it (the CRT's `_threadstartex` after `_endthreadex`) is no longer swallowed | - |
| `callback_arg` | the routine pushed for `PsCreateSystemThread(Ex)` (start routine and StartContext1), `KeInitializeDpc`, `KeInitializeApc`, `NtQueueApcThread`, `KeInitializeInterrupt`, as `push imm32` or `push reg` after `mov reg, imm32` | in a gap: seed_from_log's (reads as a function body), on an instruction boundary, a prologue or right after padding; inside a body: a clean split (the instruction before ends a path, no branch or switch arm crosses, a prologue or a closed body) |
| `ctor_vtable` | a constructor's `mov dword ptr [reg(+n)], vtable` with the vtable in a data section: its words that point into code | the same |
| `rtti_vtable` | the methods MSVC RTTI names (`tools/rtti`) | the same |
| `seh_funclet` | an exception frame's handlers and filters (`tools/recomp/seh_frames.py`): a funclet or resume target in the gap after its function extends the function; a funclet elsewhere becomes a function | the same |
| switch tables | a table in `.rdata`/`.data` bounded by the `cmp; ja` before its dispatch, a byte-indexed two-level table, `mov reg, [idx*4 + table]; jmp reg`: the arms stay in their function and are never entries (`engine.py`) | every entry in the dispatch's section |
| `gap_body` (level 2) | bytes in a gap, after padding or a terminator, nothing references (code reached through a computed address) | a closed body in the gap, a balanced stack, callee-saved registers saved before they are written, whole-argument `ret N`, none of what data decodes into, three instructions or a prologue, aligned after padding or a prologue when packed unaligned |

The weak passes that were there before (table words, padding boundaries) take the same body gates at level 1 and up:
a table word that names the middle of a path inside a function (the `mov ebp, esp` after `push ebp`, the `ret` after a
`pop`) is no longer an alias, nor bytes in a gap that close by chance.

**Switches**: `RECOMP_STATIC_DISCOVERY` (`2` default, `1` strong evidence only, `0` as before) and
`RECOMP_STATIC_DISCOVERY_CAP` (at most this many percent more functions), or `tools.disasm --static-discovery /
--discovery-cap` ([environment-variables.md](../runtime/environment-variables.md)). Off their default they change
the prepare's stamp.

**What each pass found** is in `analysis/disasm/summary.json` (`"discovery": {"level", "cap", "capped", "added":
{pass: n}, "refused": {why: n}}`), on the Prepare's analysis line (`disassembled in 12 s: 22140 functions, static
discovery +45 (callback_arg 3, gap_body 30, ...)`) and in `pack.json`'s `"analysis"` (`functions`, `by_method`,
`static_discovery`), for the owner's library report.

**Measured** on synthetic titles of our own (`tools/disasm/test_static_discovery.py`, run with `-s` for the report):
every target of the corpus (vtable methods, callbacks, a thread routine after a noreturn call, gap code, code after
`jmp $`, switch arms, an `__except` block) found at the first analysis in five layouts (int3, nop and zero padding,
packed); 0 decoys seeded out of 7,160 decoy addresses over 40 decoy titles (words naming every non-entry byte of their
code, floats, colours, strings, offsets near the image base); with static discovery off, the passes before it made 55
of them functions or aliases, in 34 of the 40 titles. On the corpus the function count goes down 6 to 14 % and the
bytes lifted move between -19 % and +1 %: aliases of decoys and of switch arms go, the missing targets come. End to end, `tools/compat/test_e2e.py::test_static_discovery_needs_no_round` boots a title whose run
reaches only such code: one round, nothing to seed, `tools.compat recall` says `no miss`.

Code: `src/kernel/recomp_discovery.c` (the writer, no dependency on the rest of the runtime), callers in
`kernel_bridge.c` (threads) and `templates/new-game/src/recomp_manual.c` (indirect calls, untranslated instructions);
test `tests/recomp_discovery`.
