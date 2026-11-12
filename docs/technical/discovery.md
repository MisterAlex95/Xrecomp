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

One JSON object per line (JSON Lines). Each entry is appended and synced as it happens, so a crash keeps every
entry written before it; a reader skips a torn last line. An entry already in the file (same kind and address, from
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
```

Addresses are guest VAs, 8 hex digits. At most 6,144 distinct entries per file.

## Who reads it

- `python -m tools.seed_from_log <work>/discovered.jsonl <xbe> --functions ... --seeds ...` takes the `icall`,
  `thread` and `thread_ctx` addresses through its gates (executable section, decodes as a function body: an address
  is never seeded just because a run mentioned it).
- `unimpl` entries are not seeds: they feed the lifter coverage report (XBA-71).
- `tools.prepare` reads it before analysing (XBA-97, `tools/prepare/discovered.py`): new `icall`, `thread` and
  `thread_ctx` addresses go through the same gates, the ones that pass are kept in `<work>/discovered-seeds.json` and
  handed to the disassembler, the others in `<work>/discovered-rejected.json` so they are not tried again. A new seed
  changes the stamp: the next prepare analyses and lifts again (all of it for now; only what changed is XBA-65).

Code: `src/kernel/recomp_discovery.c` (the writer, no dependency on the rest of the runtime), callers in
`kernel_bridge.c` (threads) and `templates/new-game/src/recomp_manual.c` (indirect calls, untranslated instructions);
test `tests/recomp_discovery`.
