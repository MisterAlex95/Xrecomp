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
| `iwram.py` | code the game copies to IWRAM, lifted from the ROM's bytes (« IWRAM code ») |
| `recomp.py` | the CLI: OUTDIR/gba_rc_AAAAAAAA{a,t}.c (1000-4000 instructions, see « Files »), gba_rc_iw_*.c + gba_rc_table.c |

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

Not lifted: code a game copies to EWRAM or makes at run time, the BIOS (code copied from the ROM to IWRAM is, see
below). A pointer the static
pass cannot see (a function table built at run time, a computed jump without a bounded table) is run by the
interpreter the first time, recorded by the core (`gba_recomp_missed`, written by gba_play / gba_run `--missed` and
by the app at exit) and lifted by the next build (`--discovered`): `tools/consoles/gba_recomp_build.py --loop N`
iterates until a run finds nothing new. On our test ROMs it converges in one or two rounds.

## IWRAM code (XBA-279)

Games run their hottest code from IWRAM (the IRQ handler, mixers, rasterisers), copied there from the ROM by crt0 or
by the game. The core stops where execution enters IWRAM code the pack does not hold (from the BIOS's IRQ entry, from
the cartridge) and records that address, plus where the cartridge holds the same bytes (the first ROM word, at the
same offset modulo 4, from which the next 32 bytes match IWRAM's), as a delta record in the discovered list:
`0xE0000000 | (ROM address - IWRAM address - 0x04FF8000) / 4` (`gba_recomp.h` `GBA_RC_IW_DELTA_ENCODE`; never a code
address, so the other consumers of the list ignore it). `iwram.py` explores each recorded IWRAM address in the ROM at
the first recorded distance under which it decodes cleanly (gba_tools' discovery on an `IwramImage`), and lifts it
with gba_tools' lifter and two overrides: the open-bus latch comes from IWRAM at run time (`RC_IW_AOB` / `RC_IW_TOB`,
IWRAM's own pattern), and the function enters with `RC_ENTER_IW(T, LO, HI)`, the bytes it was lifted from. PC-relative
literals in IWRAM are loaded at run time (only cartridge literals fold). The functions go to `gba_rc_iw_*.c`; the table
(ABI 5) lists their spans with the bytes. The cartridge's functions, files and names are the same with or without.

At run time IWRAM is also data and stack, written by lifted code without the bus, so a span is checked at every call
into its function (a memcmp of the function's size) and a store of the function's own into its bytes stops it after
that instruction (the next check leaves; the dispatcher looks at the bytes again). Code that IWRAM got elsewhere
(generated, decompressed), or that the game patched, stays interpreted. One variant per address: when a game copies
different code to the same IWRAM addresses, the first that decodes cleanly is lifted and the others are interpreted.

## Lifting

Each instruction is emitted from its opcode with the interpreter's semantics, quirks included (rotated misaligned
loads, LDRSH on odd addresses, LDM/STM with the base in the list, empty-list and `+12` PC reads, LDRD/STRD as
no-ops): same results, same flags (eager, 0/1 locals), same cycles in the same order (opcode fetch from
`fetch16/32[]`, accesses through the wait-state tables, internal cycles, multiply cycles by operand, pipeline refill
by the target's region). Literal loads are folded (value from the ROM, the access's cycles kept).

Guest registers, flags, `now` and the next event live in C locals; they are synced around each bus call (EWRAM /
IWRAM are accessed inline with the same wait states; an LDM / STM / PUSH / POP checks its whole run once) and written
back at every exit.

**Blocks and event checks.** The interpreter tests `now < next_event` before every instruction; lifted code must stop
at the same instructions (scheduler events, IRQs) without paying a test, a C label and an exit per instruction, which
is what made the compile slow (XBA-151). A function is cut into blocks: a block starts at a resume point (the entry,
a local jump target, what follows a call or anything else that leaves, the first instruction after an interpreted
one, and every `MAX_BLOCK` = 12 instructions); only those are labels of the entry switch. Inside a block, a *segment*
runs up to the next memory access: its instructions cost static cycles (fetch, internal and multiply cycles, folded
literal loads), so one test at its start, `now + those cycles >= next event`, says whether the event falls inside
(`RC_STOP` at the block's start, also the exact test for the block's first instruction; `RC_CHECK` after each memory
access, whose own cycles, and whatever a bus call did to the clock or the next event, are seen by it). When it does,
lifted code returns `GBA_RC_EXIT_INTERP` with the number of instructions left in the block (`rc_hint`): the
interpreter runs them, the event lands on the same instruction as without a pack, and the next block is native again.
Entered at an instruction inside a block (an IRQ handler returning there), a function answers the same way from a
small table. Only bus writes can raise the IRQ line or halt the CPU: their slow path drops the local next event to 0,
so the next test stops there. Measured on a synthetic 200k-instruction ROM: 5x less compile time for aarch64 than one
label per instruction, the same run time (docs/android/prepare.md « GBA packs »).

**Runs of opcode fetches (XBA-295).** With WAITCNT's prefetch buffer on (games turn it on), every opcode fetch from
the cartridge goes through the buffer's model (`gba_recomp_rt.h` rc_pf_*), which was a third of a frame. Where
instructions in a row do nothing to the clock but static internal cycles (no memory access, multiply, jump or exit),
their bodies cannot tell when the next fetches happen: `RC_IN(A, N, X, S)` charges the fetches of such a *run*, plus
the next instruction's, in one call (`rc_pf_fetch_n`), each at the cycle it would have had (X: 4 bits of internal
cycles per instruction). A literal folded from the function's own cartridge region joins the run (S: a bit per
instruction): its stop of the buffer is computed there, its body keeps the load's wait states. `rc_pf_fetch_n` works
in closed form: fetches back to back take an opcode a cycle from the buffer while it has some (it gains one per
sequential read), then each waits for the opcode being read. A run never crosses a label, and its result is exactly N
calls of `rc_pf_fetch` (`consoles/gba/tests/test_prefetch.c`, random states). A segment that is one run checks the next
event with the run's exact cycles once the bound (the most a fetch can cost, for each) fails (`RC_STOP_IN` /
`RC_CHECK_IN`); a segment of plain instructions, literals and conditional jumps does the same on a copy of the buffer's
state (`RC_STOP_X` / `RC_CHECK_X`), as if no jump were taken (a jump taken leaves earlier). Fewer segments go to the
interpreter for an event that falls after them. The NDS lifter keeps all of it off (`seg_resume`).

Control flow: branches to addresses of the same function are `goto`s; calls, returns, BX and computed jumps return
to the dispatcher with PC set (no C stack growth whatever the game does; a direct-mapped cache makes each dispatch a
probe). SWI, MSR, MRS of SPSR, exception returns (`movs pc` / `subs pc` / `ldm {pc}^`), LDM/STM with the S bit, SWP,
undefined and coprocessor encodings and PC write-backs are handed to the interpreter (`GBA_RC_EXIT_INTERP`), which
runs that one instruction after the dispatcher's usual checks.

**Files.** Functions go to files in address order; a file ends after a function whose entry hashes to 0 mod 8 once
it holds 1000 instructions (or at 4000), and is named after its first function. A function the discovery loop adds
or splits changes the file it lands in (rarely the next one too), not the others: Prepare's object cache recompiles
that file only. Compile flags (Prepare, `gba_recomp_build.py`): `-O1` (-O2 runs this code no faster) and
`-fno-tree-slp-vectorize` (nothing to vectorize, and the SLP vectorizer was the costliest pass on big functions).
`consoles/gba/tests/bench/`: `build_rom.sh` builds a synthetic ROM of game-like Thumb code (`gen_rom.py`, ~200k
instructions), `compile_time.py` measures the compile of any ROM's lift.

## Checking it

- `gba_run ROM --pack LIB --lockstep`: an interpreter-only machine alongside, the whole state (registers, banks,
  cycles, scheduler, IRQ registers, every RAM, save chip, frame) compared after each frame.
- `GBA_RC_VERIFY=1`: every lifted call is replayed by the interpreter from the same state up to the same cycle and
  compared (aborts at the first difference, with the function and the field).
- `--stats` with a pack built `-DGBA_RC_COUNT`: instructions run natively vs interpreted, dispatches, missed.
- `consoles/gba/tests/recomp_fuzz.py`: random ARM / Thumb programs (all data-processing forms, shifts, multiplies,
  every load / store width, LDM / STM, PUSH / POP, conditional branches, MRS / MSR, state switches) under a fast
  timer IRQ, lifted and run in lockstep.
