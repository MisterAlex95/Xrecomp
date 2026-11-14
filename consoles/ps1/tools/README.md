# ps1_tools: the PS1 static recompiler (XBA-160, XBA-161, XBA-162)

`python3 -m ps1_tools.recomp INPUT OUTDIR [--discovered FILE] [--report FILE] [--no-comments] [--resume-all]`: a
PlayStation program to C that runs on the core in `consoles/ps1/runtime` (contract: `ps1_recomp.h`, lifted-code
ABI: `ps1_recomp_rt.h`, overview: [consoles/README.md](../../README.md)). INPUT is a PS-X EXE or a disc image
(`.cue` / `.bin` / `.iso` / `.chd`). Pure Python 3, standard library only (the MIPS decoder is ours), so it runs in the
app's embedded CPython.

| module | role |
|---|---|
| `exe.py` | PS-X EXE header; ISO 9660 on cooked (2048) and raw (2352, mode 1 / mode 2 form 1) images, `.cue` sheets; `SYSTEM.CNF` `BOOT=` (else `PSX.EXE`) |
| `chd.py` | MAME CHD v5 CD images: Huffman-coded map, `cdzl` / `cdlz` hunks (zlib, lzma), `cdzs` on Python 3.14+ (compression.zstd), FLAC hunks (audio) refused; only the hunks read are decompressed. `python3 -m ps1_tools.chd info X.chd`; `write` makes test images in chdman's shape (XBA-168) |
| `isa.py` | MIPS I decoder: class, registers read, plain and delayed destinations, targets; `conflicts()` (load delay rule) |
| `discover.py` | images (the EXE, recorded RAM code), function discovery, jump tables, BIOS call stubs, record merging |
| `lift.py` | one C function per discovered function: blocks, event checks, memory accesses |
| `recomp.py` | the CLI: OUTDIR/ps1_rc_AAAAAAAA.c (1000-4000 instructions each, « C files ») + ps1_rc_table.c |

## Code lives in RAM

Unlike a cartridge, PS1 code is loaded into RAM: the boot EXE, then overlays read from the disc over each other at
run time. So a table entry names its code by **address and content**: start, length and the FNV-1a 64 hash of its
words (`ps1_recomp_hash`). The core keeps a write counter per 1 KB RAM page (CPU and lifted stores, DMA, the EXE
load bump it); an entry is hashed again only when a page it covers was written since it was last checked, and runs
only while the hash matches. Overlays linked at the same address are separate entries: the matching one runs,
never stale native code (`ps1_rc_hb_overlay` checks it: overlay B replaces A at 0x80100000).

The interpreter's misses are recorded with their bytes: `ADDR HASH HEXBYTES` per line (a 2 KB window from the
address as it was in RAM then; `ps1_run --missed FILE`). `recomp.py` merges overlapping records with equal bytes
into blobs, finds functions in each blob, and lifts them from those bytes; a function whose bytes equal the EXE's
becomes an EXE function. An address recorded with 8 different contents is code the program generates at run time
(ps1-tests' GTE test writes each command into a stub): neither the runtime nor the tools go on with it, it stays
interpreted. The BIOS stays interpreted: nothing below 64 KB of RAM, and nothing before the CPU first reaches the
boot EXE's entry (the table's `boot_pc`: the OpenBIOS shell runs from RAM at 0x80030000 before that).

## Discovery

Per image, from the seeds (EXE entry, recorded addresses): a linear walk per function collects JAL targets, J
targets out of the function (tail calls), code addresses built in registers (`lui rX, hi` ... `addiu / ori rY, rX,
lo`, tracked through the walk) and jump tables (`lui` / `addiu` / `addu` / `lw` / `jr`, bounded by the `sltiu`
before them, else up to 512 entries while they point into the image); then words of the image that point at a
plausible function start (`addiu sp, sp, -N`, or right after a `jr ra` and its delay slot), jump table words
excluded. A function ends after the delay slot of an unconditional transfer (J, JR, `b`) when no branch of the
function points further, at the next known start, or at a reserved encoding; never between a branch and its slot.
Conditional branches out of a function are not seeds. Bodies longer than 1000 instructions are cut into pieces (one C
function each, so no single function overwhelms the compiler), never between a branch and its delay slot.

BIOS call stubs (`li t2, 0xA0|0xB0|0xC0 ; jr t2 ; li t1, N`, Psy-Q's and PSn00bSDK's idiom) are named
`bios_A0_3C` etc. and kept as ordinary calls: the lifted stub jumps to the BIOS table, which the interpreter runs
(the core's BIOS hooks see the jump as before). Psy-Q library signatures (naming / skipping known library functions
by byte patterns) are not done: everything found is lifted; the stubs are the only library code recognised.

## Lifting

Each instruction gets the interpreter's semantics (`ps1_cpu.c`), cycle for cycle: the fetch cost of the function's
segment (KSEG1: 5, else 1); LW-class loads (LW / LWL / LWR / LWC2) with the load cost of their target, LB / LH /
stores without, as the interpreter; main RAM and the scratchpad inline (with the interpreter's address decode
quirks). GTE commands, MFC2 / CFC2 / MTC2 / CTC2 / LWC2 / SWC2 call `ps1_rc_gte_*` (SR.CU2 checked). The scheduler
is tested where the interpreter's loop would stop, with one check per segment (below).

- **Memory accesses.** An access whose address is a known IO register (the base built by LUI, then ADDIU / ORI, in
  the function, plus the offset, outside RAM and the scratchpad: `lift.io_accesses`, a heuristic for speed only) goes
  through the core (`ps1_rc_lw`, `ps1_rc_read`, `ps1_rc_write`; SWL / SWR always: `ps1_rc_swlr`), the IRQ line
  checked after it. Any other access is a RAM access: main RAM and the scratchpad inline, anything else (IO after
  all, an address or bus error, a store into the running function's own code) handed to the interpreter with the
  rest of its block (`RC_HAND`, nothing of the instruction done): one exit per access and no call into the core, so
  its cycles are static.
- **Blocks and event checks.** A function is cut into blocks: its entry, branch and jump targets, the instruction
  after a transfer's delay slot (return points), after an instruction always handed to the interpreter, and every 12
  instructions. Only block starts are resume points (a case of the entry switch). A segment runs from a block's start,
  or from after a dynamic instruction (an access through the core, a GTE command), to the next dynamic one or the
  block's end; its check, `RC_SEG`, compares `now` plus the static cycles of its instructions before the last (fetch,
  and 3 per RAM load: an upper bound) with the next event: due now, the function stops there (the interpreter's own
  stop); due inside the segment, the interpreter runs the rest of the block (`PS1_RC_EXIT_INTERP` with `rc_hint` =
  how many instructions), so events land on the same instruction as in the interpreter. Entered inside a block (an
  interrupt returns there), the function answers the same from a table (`mid_`). An IO access that left an interrupt
  pending sets the local next event to 0: the check after it stops. This replaced a label, a scheduler test and an
  exit per instruction, which made the C 3-5 times slower to compile (docs/android/prepare.md « PS1 packs »).
- **Registers.** Guest registers live in locals: the ones the function writes are loaded at entry and written back
  at every exit, the ones it only reads are `const` (the core never changes them), the others are not there.

- **Load delay slots.** A load's value lands after the next instruction. When that instruction neither reads the
  register nor loads into it (`isa.conflicts`; LWL / LWR forward the value in flight, as the interpreter), the
  value is committed at once: nothing can tell (an exception or an interrupt before that instruction lands it
  too). Otherwise the function stops with the load in flight (`ld_reg` / `ld_val`) and the interpreter runs that
  instruction. A load in a delay slot is checked against the branch target's first instruction (same function) or
  left to the dispatcher, which applies the same rule to the instruction it finds in RAM.
- **Branch delay slots** run inline after the branch; the condition and the link are computed first. A slot handed
  to the interpreter (a fault) leaves the interpreter's delay state (`next_delay`, `next_pc`, `next_taken`). A branch
  in a delay slot is handed to the interpreter.
- **Exceptions**: overflow (ADD / ADDI / SUB), address errors (alignment), bus errors (checked before the access,
  `ps1_bus_fault`, or the access handed over), SYSCALL, BREAK, reserved encodings, coprocessor unusable, and all of
  COP0 (MFC0, MTC0, RFE) are handed to the interpreter with nothing of the instruction done (`PS1_RC_EXIT_INTERP`):
  it raises them exactly.
- **Self-modifying code**: a store into the running function's own code is handed to the interpreter (a RAM
  access), or stops the function after it (through the core); the dispatcher's content check then sees the change.
- **Control flow**: branches inside the function are `goto`s; calls, returns, JR / JALR and jumps out of the function
  return to the dispatcher (bounded C stack). `--resume-all` (debugging) makes every instruction a block of its own.

## C files

Functions in address order, a file ending after a function whose identity (start, hash) hashes to 0 mod 8 once it
holds 1000 instructions, or at 4000 (`recomp.chunks_of`), named after its first function (`ps1_rc_80012340.c`;
code recorded in RAM: `ps1_rc_80100000_1a2b3c4d.c`). Functions the discovery loop adds change the file they land in
and seldom the next one: Prepare's object cache (and `ps1_recomp_build.py`'s) recompiles only those.

## Checking it

- `ps1_run INPUT --pack LIB --lockstep`: an interpreter-only twin alongside, the whole state (registers with the load
  in flight applied, PC and delay state, HI / LO, COP0, cycles, scheduler, IRQ / DMA / timer registers, GTE, RAM,
  scratchpad, VRAM) compared after every frame. `--lockstep-cycles N` compares every N cycles (1: about every
  instruction) and prints both PCs at the first difference; `PS1_RC_ENTRIES=LO-HI` attaches only those table entries
  (bisecting a divergence).
- `tools/consoles/ps1_recomp_build.py INPUT WORKDIR --loop N --ps1-run PATH --run-args "--bios X ..."`: lift, build
  with the host cc (Prepare's flags, -O1; an object cache keyed by content), run, lift what was recorded, until a
  round records nothing new; `report.json` has the rounds, functions, files compiled, native share and compile time
  per 1000 instructions.
- `consoles/ps1/tests/bench`: a synthetic game-sized program (`build_exe.sh`: `gen_exe.py`'s C, ~1100 functions of
  game-like code, ~220k instructions, built by PSn00bSDK) and `compile_time.py` (CPU time of every compile with
  Prepare's flags, host or NDK clang); never a game.
- ctest `ps1_rc_*` (`consoles/ps1/tests`): our homebrew (EXE fast boot, and the overlay test from its disc) and
  JaCzekanski's ps1-tests, each through the discovery loop and then in lockstep; `ps1_rc_fuzz` (`recomp_fuzz.py`):
  random MIPS code (every ALU form, HI / LO, loads and stores of every width, load-use hazards, loads in delay
  slots, linking branches, JAL / JALR, GTE) under a timer IRQ every 37-2000 cycles.
- pytest `test_recomp.py`: decoder, ISO 9660 on cooked and raw images, discovery on hand-encoded code (calls,
  pointers, a jump table, a BIOS stub), overlays at one address, generated code, the CLI and C syntax, blocks /
  segment checks / IO accesses / register locals, resume points, files that stay the same when functions are found.
