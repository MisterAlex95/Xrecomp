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
{"t":"run","time":"2026-11-11T18:00:00Z","v":2}              a process wrote entries from here on; "v":2: it marks taint
                                                             and writes "wild" entries (below; absent before profile-free WP1)
{"t":"icall","va":"0x0012ABCD","ret":"0x00123456"}           indirect call to code with no dispatch entry; ret = the caller's next instruction
{"t":"icall","va":"0x00525000","ret":"0x000CFA7A","tainted":true}
                                                             the same, written after the process went wrong (below)
{"t":"wild","va":"0xFF084E8B","ret":"0x000CFA7A"}            indirect call to an address that is not code (a null or wild
                                                             pointer, outside the image's code); never a seed, it marks
                                                             its call site (ret) as wild and taints the process (profile-free WP1)
{"t":"thread","va":"0x00034560","ctx1":"0x00045670"}         thread start routine with no dispatch entry
{"t":"thread_ctx","va":"0x00045670","routine":"0x00034560"}  StartContext1 inside the title's code with no dispatch entry
{"t":"unimpl","va":"0x00056780","insn":"bsf eax, ecx"}       untranslated instruction reached (RECOMP_UNIMPL)
{"t":"d3d","va":"0x0085FDE0","slot":"0x00862288"}            D3D's device found at run time (src/kernel/recomp_d3d_service.c)
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
{"t":"cut_body","va":"0x001C1208"}
                                                             a lifted body ran off its end (WP2): its last instruction (va)
                                                             is not a terminator and no function starts after it, so the
                                                             analysis cut it short; recomp_cut_body (kernel_hal.c) stops
                                                             the title instead of returning without the epilogue
```

`bytes` is the first 8 bytes at the target of an `exec_outside_image`, as hex. They are code the title made at run time
(decompressed, generated or copied), not bytes of the XBE, and only a short prefix, enough to tell code (a prologue)
from a wild pointer into data; nothing longer is ever written. Rules and the write protection behind `code_write`:
`src/kernel/recomp_smc.h`.

Addresses are guest VAs, 8 hex digits. At most 6,144 distinct entries per file.

### Taint: what a run that went wrong reports is not a seed (profile-free WP1)

A run that went wrong calls through garbage. Fable's corrupted run sent 34 different targets through one call site
(`ret 0x000CFA7A`), most of them `0xFFxxxxxx`; 14 fell in executable bytes that decode, the gates of the time took
them as seeds, 12 of them sat inside an instruction of a real function, and one (`0x001C120A`) cut a function in half:
the next build returned from it without its `pop`s and `ret 8`, and the guest stack was off from then on. That looked
like a profile problem for a day. So:

- The runtime marks the process **tainted** at the first `wild`, `smc` or `exception` entry (even one already in the
  file: the sign is this run's), says so once on stderr (`[DISCOVERY] the run went wrong here (wild 0xFF084E8B): ...`),
  and writes its later `icall`, `thread` and `thread_ctx` entries with `"tainted":true`. They do not count in the
  status line's `seeds`. A tainted entry is keyed apart: a later healthy run that meets the same address writes it
  again, untainted.
- `recomp_icall_fail_log` writes a target outside the image's code (`g_xbox_code_lo`/`hi`) as `wild`, not `icall`;
  `recomp_icall_not_code_log` writes the first 16 non-code targets as `wild` (`templates/new-game/src/recomp_manual.c`).
- The readers (`tools/seed_from_log` `classify_discovery`) apply two rules per run segment, from one `run` line to
  the next. **Call site**: an `icall` whose `ret` also produced a target outside the executable sections in that run
  (a `wild` entry, or an older file's `icall` to a non-code address) is never a seed. **Taint**: entries after the
  first `wild` / non-code `icall` / `smc` / `exception` entry of the segment, or marked `"tainted":true`, are held.
  An address becomes a seed only when some entry of it is clean: a healthy run repeats it.
- And a static gate: an address inside an instruction of a body `functions.json` established (decoded from the
  body's start along its control flow, not the linear sweep) is refused unless a prologue starts there. Bodies that
  start at a run's own seeds do not count, but a real body an earlier seed cut still does.

On the launcher's poisoned Fable folder these rules keep `0x00584074` and `0x005861C5` and refuse all 14 (the call-site
rule alone takes all 14; the instruction gate alone 11). A test that a dword equal to the address occurs in the image
was tried and does not discriminate (8 of the 14 occur by coincidence, one 841 times).

The first `icall`, `thread` or `thread_ctx` entry a process writes is also said on stderr, once: `[DISCOVERY] first
code the analysis missed in this run: icall 0x0012ABCD (skipped; the next Prepare adds it)`. A skipped indirect call
does not crash, the title goes on (maybe wrong): that line, the entry, and the status line's `seeds` (how many such
entries this process wrote, `src/kernel/recomp_status.h`) make the miss visible without a crash (XBA-129).

## Who reads it

- `python -m tools.seed_from_log <work>/discovered.jsonl <xbe> --functions ... --seeds ...` takes the `icall`,
  `thread` and `thread_ctx` addresses through its gates (executable section, decodes as a function body, not inside
  an instruction of a known body, and the call-site and taint rules above: an address is never seeded just because a
  run mentioned it). `wild` entries are not seeds.
- `unimpl` entries are not seeds: they feed the lifter coverage report (XBA-71). Nor are `stack_switch` entries:
  they name a blocker ([stack-switching.md](stack-switching.md)).
- `stub` entries are not seeds either: `tools.compat` adds « calls stub ordinal N (Name) » to the run's blocker
  (XBA-115).
- `smc` entries are not seeds: what they name is not in the XBE as lifted. `tools.compat` and the device grader add
  « code written or run at run time xN: exec_outside_image 0x... (heap, from 0x...), code_write 0x... (.text) » to the
  blocker, whatever the stage, and the result line carries the count (`"smc": N`); the Compatibility page counts the
  titles that have any (XBA-118). An `exec_outside_image` used to be an `icall` entry when it came through the lookup,
  and a seed candidate.
- `cut_body` entries are not seeds: they name an analysis error (a function start that cut the body before it).
  `tools.prepare` already prints the count of trapped bodies after the lift (« cut bodies ... »), so they are known
  before any run.
- An `unimpl` entry also comes from string port I/O (`ins*`, `outs*`) in lifted code (XBA-139): taken for data
  decoded as code, it lifts to `recomp_misdecoded`, which writes the entry and stops the title
  (`RECOMP_MISDECODE_TRAP=0` carries on).
- After the lift, `tools.recomp` prints one line `lift health: {"seh_lost": n, "unresolved_calls": n,
  "unimplemented": n, "misdecoded": n, "cut_bodies": n}` (XBA-139): exception handler targets no function enters,
  call targets left as stubs (each listed once with its first call site: « unresolved call targets (stubbed): »),
  untranslated instructions, string port I/O sites, cut bodies. `tools.prepare` (desktop and Android) shows the
  non-zero counts after the lift and keeps them in the stamp as `lift_health`, which `--check --json` reports, so a
  library check can rank titles by them.
- `d3d_layout` entries are not seeds: they say what to add to `k_layouts` in `src/kernel/recomp_d3d_service.c` for that
  build (XBA-105); `tools.compat` (and the device grader) add « D3D layout guessed for D3D8LTCG 5933:
  fence_event, ... » after a blocker other than a crash or a launch (`tools/compat/blockers.py`).
- `tools.prepare` reads it before analysing (XBA-97, `tools/prepare/discovered.py`): the `icall`, `thread` and
  `thread_ctx` addresses go through the same gates, the ones that pass are kept in `<work>/discovered-seeds.json` and
  handed to the disassembler, the ones a static gate refuses are listed in `<work>/discovered-rejected.json`, the ones
  only a wild site or a tainted stretch reported in `<work>/discovered-held.json` (until a healthy run reports them).
  Every prepare re-gates the whole file, the seeds already kept included (profile-free WP1): a seed an older toolkit took that
  today's gates refuse is dropped and said (`- 001C120A  dropped from discovered-seeds.json: from a wild call site
  ...`, `14 seed(s) dropped: ...`), so a poisoned work folder heals at its next Prepare, on a desktop or a phone,
  without anyone deleting files. Only changes are printed. A seed added or dropped changes the stamp: the next
  prepare analyses and lifts again (all of it for now; only what changed is XBA-65).
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
| `callback_arg` | the routine pushed for `PsCreateSystemThread(Ex)` (start routine and StartContext1), `KeInitializeDpc`, `KeInitializeApc`, `NtQueueApcThread`, `KeInitializeInterrupt`, as `push imm32` or `push reg` after `mov reg, imm32`; and the same pushed for a **wrapper**, a function that pushes an argument of its own there (`push [ebp+n]`, `[esp+n]`, or through a register), as the XDK's `CreateThread` does with `lpStartAddress` (StartContext1), through any number of wrappers (`summary.json` `"callback_wrappers"`): a title's thread routines, the CRT's `_threadstartex` among them, are only ever named that way (GitHub issue 1) | in a gap: seed_from_log's (reads as a function body), on an instruction boundary, a prologue or right after padding; inside a body: a clean split (the instruction before ends a path, no branch or switch arm crosses, a prologue or a closed body) |
| `ctor_vtable` | a constructor's `mov dword ptr [reg(+n)], vtable` with the vtable in a data section: its words that point into code | the same |
| `rtti_vtable` | the methods MSVC RTTI names (`tools/rtti`) | the same |
| `seh_funclet` | an exception frame's handlers and filters (`tools/recomp/seh_frames.py`): a funclet or resume target in the gap after its function extends the function; a funclet elsewhere becomes a function | the same |
| switch tables | a table in `.rdata`/`.data` bounded by the `cmp; ja` before its dispatch, a byte-indexed two-level table, `mov reg, [idx*4 + table]; jmp reg`: the arms stay in their function and are never entries (`engine.py`) | every entry in the dispatch's section |
| `gap_body` (level 2) | bytes in a gap, after padding or a terminator, nothing references (code reached through a computed address) | a closed body in the gap, a balanced stack, callee-saved registers saved before they are written, whole-argument `ret N`, none of what data decodes into, three instructions or a prologue, aligned after padding or a prologue when packed unaligned |
| `packed_table` (level 2, XBA-140) | a packed pointer table in a data section: a run of 3 or more records of one size (4 to 32 bytes) whose same field names code, at any alignment, at least one word off a dword boundary (the older table-word pass reads aligned words only). The MSVC CRT's x87 math dispatch is the case: its handler pointers are packed after variable-length names, its handlers frameless | per word, a run's observed seed's (seed_from_log: executable, reads as a function body) plus a closed CFG that is a compiled body (balanced stack, callee-saved registers saved, whole-argument `ret N`, none of what data decodes into), no prologue asked; where a function starts: in a gap an instruction boundary that is aligned or follows padding or a path's end, inside a body an instruction after a path's end (a clean split makes it a start, otherwise an alias that keeps the body whole). A run (less the refused words at its ends when they name code more than 64 KB from its other routines and those lie within 64 KB of each other: a record header read as a word) is taken when 3 of its words and three quarters of them pass and they name two routines or more. Then the table is completed (WP5): runs sharing a word with a taken run are one table; in a run of it where half the routines named or more are established, and lie within 64 KB of each other, a refused word becomes an alias (never a cut) when it names code within 64 KB of an established entry, on an instruction boundary, not a jump table, padding or measured byte table, with a closed compiled body where a jump into code the sweep decoded counts as an exit. That takes the CRT routines that fall into each other (`fstp st0` then the entry `ret`), open with `or cl, cl`, or jump back into a neighbour, with no seed to anchor the table |

The weak passes that were there before (table words, padding boundaries, starts after a ret, a data section's
switch-table words) take the same body gates under `RECOMP_STATIC_DISCOVERY_STRICT=1` (off by default, wave 5): a
table word that names the middle of a path inside a function (the `mov ebp, esp` after `push ebp`, the `ret` after a
`pop`) is no longer an alias, nor bytes in a gap that close by chance. That keeps the decoys out (the decoy tests run
with it), but on synthetic titles it drops 6-14 % of the starts those passes found, and on a real title one may be
genuine: it waits for the owner's comparison of the function counts on the reference titles. By default static
discovery only adds: `test_strict_off_finds_every_function_the_passes_before_found` pins that every start the
analysis before XBA-137 found is still a start or inside a measured body (the register-dispatched switch's arms,
which the new pass keeps in their function).

**Switches**: `RECOMP_STATIC_DISCOVERY` (`2` default, `1` strong evidence only, `0` as before),
`RECOMP_STATIC_DISCOVERY_CAP` (at most this many percent more functions) and `RECOMP_STATIC_DISCOVERY_STRICT`
(above), or `tools.disasm --static-discovery /
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

Packed tables (`tools/disasm/test_packed_tables.py`, `-s` for the report): a fixture with three packed tables (13-byte
`{name, kind, f1, f2}` records, a name followed by four contiguous pointers, 6-byte `{id, f}` records) naming eleven
frameless routines, x87 code among them, two of them inside another function's body: all eleven are functions at the
first analysis in the five layouts (four before: the ones a word happened to name at an aligned address), no decoy
(a string holding an address, an unaligned run of words into the middle of code, colours whose one-byte-in windows read
as code addresses, an unaligned float array). Its own decoy corpus (the 40 decoy titles plus packed runs of 3 to 8 words
at a random stride and phase into non-entry code bytes, strings, colours and floats at odd offsets): 0 seeded, the
pass adds nothing, 633 runs refused; the corpus above stays at 0 of 7,160.

A CRT math table with no anchor (`crt_math_table_title`, WP5): 32-byte records `{len-prefixed name, flag, u16 id, four
pointers at phase 2}` naming nine x87 helpers, four of them fall-into entries (after `fstp st0`, after a `mov`, opening
with `or cl, cl` and a branch into a neighbour), the others testing a flag in `bl` (`or bl, bl`, not a clobber of
ebx). All nine are entries at the first analysis in the five layouts, strict on and off; a word of the table naming
the middle of an instruction and a second table of the same shape whose words all name mid-path instruction
boundaries add nothing; a run whose passing words name code 64 KB apart (words read across a table of short strings)
keeps its refused ends and stays refused. On the real titles the completion only adds the CRT's math routines: every entry of
Fable's math table is dispatchable with no seed (it used to need eleven), and Burnout 3 and Black gain the same
routines.

Code: `src/kernel/recomp_discovery.c` (the writer, no dependency on the rest of the runtime), callers in
`kernel_bridge.c` (threads) and `templates/new-game/src/recomp_manual.c` (indirect calls, untranslated instructions);
test `tests/recomp_discovery`.
