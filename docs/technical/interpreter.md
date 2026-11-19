# The x86 interpreter: code the analysis missed (XBA-303)

Static recompilation lifts the code the analysis finds. Until XBA-303 what it missed was not run at all: an indirect
call to an address with no dispatch entry popped its return address and went on (« the title goes on, maybe
wrong »), a thread whose start routine was not lifted never started, a direct call to an address the analysis did not
make a function reached a stub that skipped it, and code made at run time (`exec_outside_image`) was only named.
Each miss cost a Prepare round to find (about 15 minutes on the device for a large title), and the next miss only
showed up once the first was lifted.

Now such code is **interpreted** from the guest CPU state until it reaches lifted code again, and every miss is still
written to the discovery file, with every call the interpreted code makes to another unlifted address: one run names
them all, and the next Prepare lifts them. This is what GBA, PS1, N64 and Xbox 360 (XBA-283) already did; the owner
reopened the « no interpreter, no JIT » design for the Xbox on 2026-11-19.

`RECOMP_INTERP=0` restores the old behaviour (skip and report). `RECOMP_INTERP_TRACE=1` prints every interpreted
instruction and the registers before it.

## Where it is entered

`xbox_InterpResolve(va, kind, ret)` (`src/kernel/xbox_interp.h`) gives a callable `void(void)` for a guest address with
no lifted function: one thunk per address for the whole run, from a pool of 4096 (a plain function bound to its
address on first use). So every place that wants a `recomp_func_t` takes one:

| Miss | Where | Discovery entry |
|---|---|---|
| indirect call / tail jump, in the image's code | `RECOMP_ICALL_DISPATCH_` (recomp_types.h), after the lookups miss | `icall` (with the return address) |
| indirect call / jump to mapped memory outside the image's code | the same macro, before its « not code » skip | `smc` `exec_outside_image` |
| direct call to an address the analysis did not lift | the translator's unresolved-call stubs (`RECOMP_STUB_INTERP`) | `icall` |
| thread start routine | `PsCreateSystemThread(Ex)` (kernel_bridge.c) | `thread` (as before) |
| DPC, APC, timer, interrupt, IRP and notify routines | kernel_bridge.c | `icall` |
| SEH handler the dispatcher calls | `seh_resolve` (xbox_seh.c) | `icall` |
| a call the interpreted code makes to unlifted code | the interpreter (`note_call` hook) | `icall` (`smc` outside the image) |

The macro reaches the runtime through a weak reference (`xbox_InterpIcallMiss`), so a harness without the runtime
links and keeps the skip.

**The entry gate.** A new address is decoded first, up to its first transfer: two zero bytes (`add [eax], al`),
`FF FF`, an undecodable byte, the null page or the kernel thunk range is not code, whatever pointed at it, and keeps
the old skip (and its record). That is what refuses the self-healing test's thread C (zeros), as seed_from_log's
gates refuse it at the next Prepare.

## How it runs

`src/kernel/xbox_interp_core.c` decodes and executes one **activation** over `g_cpu`, compiled against
`templates/runtime/recomp_types.h` as generated code is:

- **Registers**: eax..edi and esp are `g_cpu`'s; ebp is `g_ebp` for the whole activation (so it is published before
  every native call, as lifted code publishes its local, and a fault sees it); the arithmetic flags are `g_eflags`
  (exact at every instruction: the lifter's flags are lazy and per function, which an interpreter cannot follow); DF
  is `g_df`. A pushfd / popfd / sahf / lahf reads and writes the same words the lifted code does.
- **Guest memory** goes through the generated code's accessors (`MEM8`..`MEMD`, `recomp_guest_base`'s barrier):
  every register write before an access is in `g_cpu` when it faults. Locked read-modify-writes are compare-and-swap
  loops (`RECOMP_ATOMIC_CAS*`), xchg with memory an atomic exchange, cmpxchg8b `RECOMP_ATOMIC_CAS64`.
- **Integer instructions** are written by hand (`exec`, `exec_0f`), with every flag. **x87, SSE, MMX and the system
  instructions** (cpuid, rdtsc, in / out, mov crN / drN, lgdt, hlt, cli, sti, int) are the lifter's own statements:
  `tools/recomp/gen_interp.py` lifts each encoding with sentinel operands (modrm reg `xmm2` / `mm2` / `edx`, rm
  `xmm6` / `mm6` / `esi`, memory `[ebx + 0x12345678]`, imm8 `0xA5`), renames them to the decoded instruction's
  fields, lifts it again with other registers and another immediate and requires the same text (else it expands the
  encoding into concrete cases: every register pair, or every immediate, as cmpps's predicate needs). The operands are
  copies written back when they changed, so a statement written for two registers is right when they are the same
  one. The compares that talk to the lifter's lazy flags get a hook (`_fca` / `_fcb` of comiss, `_fa` of fcomi);
  fcmov is by hand. `xbox_interp_gen.inc` is checked in and must be current (`test_the_generated_file_is_current`).
- **Calls and returns.** A call to an address with a lifted, manual or kernel function calls it (a C call, after the
  return address is pushed, as lifted code calls); anything else is interpreted in the same activation. A `ret` that
  pops past the activation's own return address (esp above its entry esp) returns to the lifted caller. A jump into
  a lifted entry is a tail call: when it returns, the activation returns if the jump came from its own code (as a
  lifted `RECOMP_ITAIL` returns), else the interpreted caller goes on. An SEH prolog / epilog helper hands its caller
  a new frame in `g_seh_ebp`: taken when the callee moved esp the way those helpers do.
- **Loops.** A taken backward branch is a back edge: every 256th asks the guest CPU's quantum
  (`xbox_GuestCpuPreempt`, as `RECOMP_BACK_EDGE`); a jcc back onto a `cmp [m], 0` / `test [m], imm` right before it
  is a poll loop and waits as `RECOMP_POLL_WAIT` does.
- **An instruction it cannot run** (one the lifter does not lift either, ud2, 16-bit addressing): one line
  (`[INTERP] cannot run the instruction at ...`), an `unimpl` discovery entry (`"insn":"interp: 0F 0B"`), and the
  call returns as a skipped one did (its return address popped, eax 0).

## Boundaries

- **The frame shadow stack (XBA-247).** An activation records a frame on entry as a lifted function that may park
  does (its entry esp, the published ebp) and puts the top back at its end.
- **SEH.** A guest exception raised under interpreted frames walks the same chain: handlers, filters, funclets and
  unwinds the analysis missed are interpreted (`seh_resolve`), a lifted handler resumes into a lifted anchor as
  before. A resume *into* interpreted code (a handler's `jmp` to an `__except` block of an interpreted frame, a guest
  longjmp) arrives at the indirect-call miss path with esp above the native call the activation was making:
  `xbox_InterpResumeAt` longjmps back to that activation (each takes a native setjmp at entry), puts the frame top
  back and goes on interpreting at the target. Activations a longjmp went over are found dead by what decides it
  outside their memory (their host frame below the caller's, or their shadow-stack frame gone) and dropped.
- **Faults.** A host fault on a guest address inside interpreted code becomes the same guest
  `STATUS_ACCESS_VIOLATION` (xbox_seh_fault.c), and the record's address and the context's Eip are the faulting
  instruction's (lifted code has 0 there). A handler that continues execution gets the access run again, as in
  lifted code; its CONTEXT's registers are not applied (as in lifted code).
- **Save states.** A thread parked inside an activation (a kernel call or a host wait made by interpreted code, or by
  lifted code it called) has no lifted frame a resume could re-enter, and the interpreter's position is in its host
  frames: the park record carries the activation count (`XboxParkRecord.interp`) and a capture **refuses**, « guest
  thread N is in code the interpreter runs (...); the next Prepare lifts it » (retry later). States are taken at
  lifted boundaries only; after the next Prepare the code is lifted and saves work again.
- **Guest threads and the guest lock.** Interpreted code runs on the guest thread that reached it, holding the guest
  CPU as lifted code does; its kernel calls let it go as theirs do. Each thread has its own activations.
- **KeBugCheck** and unhandled exceptions end the process as from lifted code; the discovery entries are already
  written (one append per entry).

## Visibility

- The status line (`RECOMP_STATUS`) gets `interp_insns` (instructions interpreted so far) and `interp_entries`
  (addresses resolved to thunks) once there are any: a hot interpreted path is a steep `interp_insns` between two
  lines.
- The log says each new address once (`[INTERP] no lifted code at 0x... (indirect call target, return address 0x...):
  interpreted`), the first 256, and, each time the total doubles past 10 million, the three hottest entries
  (`[INTERP] N instructions interpreted so far (E entries); hottest: 0x... (n), ...`).

## Tests

- `tools/recomp/test_interp_diff.py`: 300 random integer sequences x 8 states run on a real CPU (`data/
  interp_golden.json`, linux/386 container) against the interpreter on any host; the ones the lifter gets right,
  interpreter against the lifted C; 200 random SSE / MMX / x87 sequences against the lifted C; a CF mutation must be
  caught; fresh sequences on the CPU with `XBOXRECOMP_PYTEST_DOCKER=1`.
- `tools/conformance`: a fourth pass runs the 7,662 vectors' snippets through the interpreter against the CPU.
- `tests/seh_dispatch` (`seh_dispatch_interp_test`): every SEH scenario of XBA-112 / XBA-132 with five of the
  fixture's functions unlifted (an EH3 frame between two others, both frame handlers, a C++ frame and its unwind
  stub): 78 checks, the same results as the lifted run.
- `tools/compat/test_e2e.py::test_the_self_healing_loop` (XBA_E2E=1): three thread starts found in one run where it
  took three rounds (`..._without_the_interpreter` keeps the old loop under `RECOMP_INTERP=0`).

## Limits

- A thread parked inside interpreted code refuses a save state (above).
- Code written over lifted code at run time (`code_write`) still runs the lifted copy: the interpreter only takes
  addresses with no lifted function.
- Stack switches inside interpreted code (a fiber the interpreted code switches to) are not carried: the activation
  ends at its own return address by esp.
- 16-bit addressing (0x67), far calls and jumps, iret, task and segment instructions are not run (reported as above);
  SSE2 forms the lifter does not lift either (the Xbox's CPU is a Pentium III) are not run.
- 4096 distinct missed addresses per run; the ones after are skipped (said once).
- MSVC builds of the runtime keep the old behaviour (the core is GNU C).
- The interpreter is a fallback: the next Prepare is what makes the code fast.
