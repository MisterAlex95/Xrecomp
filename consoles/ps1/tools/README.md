# ps1_tools: the PS1 static recompiler (XBA-160, XBA-161, XBA-162)

`python3 -m ps1_tools.recomp INPUT OUTDIR [--discovered FILE] [--report FILE] [--no-comments] [--sparse-labels]`: a
PlayStation program to C that runs on the core in `consoles/ps1/runtime` (contract: `ps1_recomp.h`, lifted-code
ABI: `ps1_recomp_rt.h`, overview: [consoles/README.md](../../README.md)). INPUT is a PS-X EXE or a disc image
(`.cue` / `.bin` / `.iso`). Pure Python 3, standard library only (the MIPS decoder is ours), so it runs in the
app's embedded CPython.

| module | role |
|---|---|
| `exe.py` | PS-X EXE header; ISO 9660 on cooked (2048) and raw (2352, mode 1 / mode 2 form 1) images, `.cue` sheets; `SYSTEM.CNF` `BOOT=` (else `PSX.EXE`) |
| `isa.py` | MIPS I decoder: class, registers read, plain and delayed destinations, targets; `conflicts()` (load delay rule) |
| `discover.py` | images (the EXE, recorded RAM code), function discovery, jump tables, BIOS call stubs, record merging |
| `lift.py` | one C function per discovered function |
| `recomp.py` | the CLI: OUTDIR/ps1_rc_NNN.c (about 3000 instructions each) + ps1_rc_table.c |

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
segment (KSEG1: 5, else 1) and the scheduler test before every instruction; LW-class loads (LW / LWL / LWR / LWC2)
with the load cost of their target, LB / LH / stores without, as the interpreter; RAM and scratchpad inline (with
the interpreter's address decode quirks), everything else through the core (`ps1_rc_lw`, `ps1_rc_read`,
`ps1_rc_write`, `ps1_rc_swlr`), the IRQ line checked after such an access. GTE commands, MFC2 / CFC2 / MTC2 / CTC2 /
LWC2 / SWC2 call `ps1_rc_gte_*` (SR.CU2 checked).

- **Load delay slots.** A load's value lands after the next instruction. When that instruction neither reads the
  register nor loads into it (`isa.conflicts`; LWL / LWR forward the value in flight, as the interpreter), the
  value is committed at once: nothing can tell (an exception or an interrupt before that instruction lands it
  too). Otherwise the function stops with the load in flight (`ld_reg` / `ld_val`) and the interpreter runs that
  instruction. A load in a delay slot is checked against the branch target's first instruction (same function) or
  left to the dispatcher, which applies the same rule to the instruction it finds in RAM.
- **Branch delay slots** run inline after the branch; the condition and the link are computed first. A stop in front
  of a slot (scheduler) leaves the interpreter's delay state (`next_delay`, `next_pc`, `next_taken`). A branch in a
  delay slot is handed to the interpreter.
- **Exceptions**: overflow (ADD / ADDI / SUB), address errors (alignment), bus errors (checked before the access,
  `ps1_bus_fault`), SYSCALL, BREAK, reserved encodings, coprocessor unusable, and all of COP0 (MFC0, MTC0, RFE) are
  handed to the interpreter with nothing of the instruction done (`PS1_RC_EXIT_INTERP`): it raises them exactly.
- **Self-modifying code**: a store into the running function's own code stops it after the store; the dispatcher's
  content check then sees the change.
- **Control flow**: branches inside the function are `goto`s; calls, returns, JR / JALR and jumps out of the function
  return to the dispatcher (bounded C stack). Every instruction is a resume point (`--sparse-labels`: only branch
  targets, return addresses and the instructions after accesses / hand-overs, ~10 % faster to compile).

## Checking it

- `ps1_run INPUT --pack LIB --lockstep`: an interpreter-only twin alongside, the whole state (registers with the load
  in flight applied, PC and delay state, HI / LO, COP0, cycles, scheduler, IRQ / DMA / timer registers, GTE, RAM,
  scratchpad, VRAM) compared after every frame. `--lockstep-cycles N` compares every N cycles (1: about every
  instruction) and prints both PCs at the first difference; `PS1_RC_ENTRIES=LO-HI` attaches only those table entries
  (bisecting a divergence).
- `tools/consoles/ps1_recomp_build.py INPUT WORKDIR --loop N --ps1-run PATH --run-args "--bios X ..."`: lift, build
  with the host cc (-O1), run, lift what was recorded, until a round records nothing new; `report.json` has the
  rounds, functions, native share and compile time per 1000 instructions.
- ctest `ps1_rc_*` (`consoles/ps1/tests`): our homebrew (EXE fast boot, and the overlay test from its disc) and
  JaCzekanski's ps1-tests, each through the discovery loop and then in lockstep; `ps1_rc_fuzz` (`recomp_fuzz.py`):
  random MIPS code (every ALU form, HI / LO, loads and stores of every width, load-use hazards, loads in delay
  slots, linking branches, JAL / JALR, GTE) under a timer IRQ every 37-2000 cycles.
- pytest `test_recomp.py`: decoder, ISO 9660 on cooked and raw images, discovery on hand-encoded code (calls,
  pointers, a jump table, a BIOS stub), overlays at one address, generated code, the CLI and C syntax.
