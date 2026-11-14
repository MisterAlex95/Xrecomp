# gba_tools: the GBA static recompiler (XBA-150, XBA-151, XBA-152)

`python3 -m gba_tools.recomp ROM OUTDIR [--discovered FILE] [--report FILE] [--no-comments]`: a GBA ROM to C that
runs on the core in `consoles/gba/runtime` (contract: `gba_recomp.h`, lifted-code ABI: `gba_recomp_rt.h`, overview:
[consoles/README.md](../../README.md)). Pure Python 3; Capstone, when installed, only adds disassembly comments.

| module | role |
|---|---|
| `rom.py` | header (title, game code, maker, complement check, entry branch), SHA-256, cartridge reads |
| `isa.py` | ARMv4T classification, bit for bit the interpreter's (`gba_cpu.c` `arm_execute` / `thumb_execute`) |
| `discover.py` | function discovery, data barriers, table islands |
| `lift.py` | one C function per discovered function |
| `recomp.py` | the CLI: OUTDIR/gba_rc_NNN.c (about 2500 instructions each) + gba_rc_table.c |

## Discovery

Recursive descent from 0x08000000 (ARM). Inside a function: B / Bcc, fall-through, jump tables. New entries: BL
targets, BX to a known constant (bit 0 = Thumb; `bx pc` veneers; libgcc `_call_via_rN`), `mov lr, pc ; bx rN` calls
(the return point stays in the function), the IRQ handler stored to 0x03007FFC, Thumb pointers (bit 0 set) in
literal pools and `adr` targets. Those last ones are weak: kept only when their code decodes cleanly (no undefined
encoding, no torn BL pair, no branch out of the ROM, no data). Jump tables: ARM `ldr pc, [pc, rN, lsl #2]` and
`add pc, pc, rN, lsl #2`, Thumb word tables through `mov pc` / `add pc`, libgcc `__gnu_thumb1_case_{uqi,sqi,uhi,shi,
si}`, each bounded by the `cmp rN, #max` before it (no bound: the table is left to the discovery loop). Literal
pool words and tables are data barriers that no function runs into. Constants are tracked per path (pool loads,
`mov`, `adr`, `add`), enough for veneers and tables.

Final bodies stop at other entries; code two functions reach (a shared tail) becomes its own function, so nothing is
lifted twice. Bodies are cut at 4000 instructions (the rest becomes new functions). The table holds one entry per
contiguous run of a function's instructions, runs cut so they never overlap: the core finds the only function that
can hold a PC as the run at or before it.

Not lifted: code a game copies to IWRAM / EWRAM (always interpreted; no detection), the BIOS. A pointer the static
pass cannot see (a function table built at run time, a computed jump without a bounded table) is run by the
interpreter the first time, recorded by the core (`gba_recomp_missed`, written by gba_play / gba_run `--missed` and
by the app at exit) and lifted by the next build (`--discovered`): `tools/consoles/gba_recomp_build.py --loop N`
iterates until a run finds nothing new. On our test ROMs it converges in one or two rounds.

## Lifting

Each instruction is emitted from its opcode with the interpreter's semantics, quirks included (rotated misaligned
loads, LDRSH on odd addresses, LDM/STM with the base in the list, empty-list and `+12` PC reads, LDRD/STRD as
no-ops): same results, same flags (eager, 0/1 locals), same cycles in the same order (opcode fetch from
`fetch16/32[]`, accesses through the wait-state tables, internal cycles, multiply cycles by operand, pipeline refill
by the target's region). Literal loads are folded (value from the ROM, the access's cycles kept).

Guest registers, flags, `now` and the next event live in C locals; they are synced around each bus call (EWRAM /
IWRAM are accessed inline with the same wait states) and written back at every exit. Before each instruction:
`now >= next_event` leaves for the dispatcher (the interpreter's loop test, so scheduler events and IRQs land on the
same instruction); after each store: IRQ line / HALTCNT. Every instruction has a label reachable from the function's
entry switch, so lifted code resumes anywhere (after an event, or when an IRQ handler returns into it).

Control flow: branches to addresses of the same function are `goto`s; calls, returns, BX and computed jumps return
to the dispatcher with PC set (no C stack growth whatever the game does; a direct-mapped cache makes each dispatch a
probe). SWI, MSR, MRS of SPSR, exception returns (`movs pc` / `subs pc` / `ldm {pc}^`), LDM/STM with the S bit, SWP,
undefined and coprocessor encodings and PC write-backs are handed to the interpreter (`GBA_RC_EXIT_INTERP`), which
runs that one instruction after the dispatcher's usual checks.

## Checking it

- `gba_run ROM --pack LIB --lockstep`: an interpreter-only machine alongside, the whole state (registers, banks,
  cycles, scheduler, IRQ registers, every RAM, save chip, frame) compared after each frame.
- `GBA_RC_VERIFY=1`: every lifted call is replayed by the interpreter from the same state up to the same cycle and
  compared (aborts at the first difference, with the function and the field).
- `--stats` with a pack built `-DGBA_RC_COUNT`: instructions run natively vs interpreted, dispatches, missed.
- `consoles/gba/tests/recomp_fuzz.py`: random ARM / Thumb programs (all data-processing forms, shifts, multiplies,
  every load / store width, LDM / STM, PUSH / POP, conditional branches, MRS / MSR, state switches) under a fast
  timer IRQ, lifted and run in lockstep.
