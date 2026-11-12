# Guest stack switching

> Part of the [Documentation Index](../INDEX.md)

Fibers, coroutines and job schedulers that switch stacks by hand, carried with no title code (XBA-101).

## The problem

Lifted code keeps its return path on the host stack: a guest `call` is a C call, a guest `ret` a C return (see
[register-model.md](register-model.md)). A title that switches stacks by hand breaks that. Fable's loader and scripts
run as fibers through this function:

```asm
cli
push ebx / push ebp / push esi / push edi / push dword fs:[0]
xchg esp, [0x00990A8C]          ; store this side's esp, load the other side's
pop dword fs:[0] / pop edi / pop esi / pop ebp / pop ebx
sti
ret                             ; continues on the other stack
```

Lifted, that `ret` is a C return to the caller on the *old* stack, with the other side's registers: the resumer came
straight back and the fiber never ran. Fable needed a hand-written replacement of this function
(`profiles/titles/4d5300d1/overrides.c`, by address) to boot past its first load. Many engines of the era have a job or
script scheduler built the same way, and XAPI's own `SwitchToFiber` has the same shape (`mov [cur], esp` /
`mov esp, [next]` / `ret 4`).

## One host stack per guest stack

**Lifter** (`tools/recomp/lifter.py`, `stack_switch_sites`). An instruction that loads esp from memory is a switch:

| Switch | Not a switch |
|---|---|
| `xchg esp, [mem]`, `xchg [mem], esp` | `mov esp, ebp`, `lea esp, [ebp-N]` (epilogues) |
| `mov esp, [mem]` with any base but ebp | `mov esp, [ebp-18h]` (the `__except` block restoring its own frame's esp) |
| `mov reg, [mem]` then `mov esp, reg` (no branch between) | arithmetic on esp (`_chkstk`, `_alloca_probe`) |
| | `pop esp` (compilers only emit it to undo their own `push esp`), `xchg esp, reg` |

A function with a switch notes, around the load, the esp it leaves (`_ss_save`), the esp it lands on (`_ss_load`) and
where (`_ss_site`); at entry, the return address it was called with (`_ss_entry`). Its `ret` then calls
`recomp_stack_switch_ret(...)` before the usual `esp += 4; return;`. The prepare log names each one:
`stack switch: sub_0004B440 loads esp from memory at 0x0004B44C (carried at its ret)`.

**Runtime** (`src/kernel/recomp_stack_switch.c`). Each host thread that switches keeps a table of contexts: its own
stack is context 0, every other one owns a host stack (`mmap`, 8 MB by default, committed as it is used, a guard page
below). At the ret of a switching function:

- the side being left is **suspended** inside that call, keyed by the esp it left and the return address it was called
  with, plus the first 64 bytes of its guest stack;
- the guest esp it loaded and the address the ret pops pick the side to run: a suspended context with that esp and that
  return address is **resumed** -- it comes back out of the call it was suspended in, and its own function pops its
  return address as usual;
- otherwise a **new** context starts on a fresh host stack at the address the ret pops (a fiber's entry), with esp past
  it. When that function returns, the context continues where its ret went (the return address it was entered with),
  as the guest would: a coroutine whose body returns into an exit routine that switches away ends that way.

Guest registers need nothing: they are per host thread (`g_cpu`) and the switch stays on the thread, so what the
switching function popped is what the other side sees. ebp, a C local, travels through `g_ebp` / `g_seh_ebp` like at
any ret.

**Twins.** An engine that copies each fiber's stack bytes into one shared area (Fable) parks every fiber at the same
esp, returning to the same yield: both keys match several contexts. The one whose saved 64 bytes match the guest stack
best is resumed -- the engine copied that fiber's bytes back before switching to it. A resume that is not an exact
match says so (`[STACK] N contexts parked at esp ... resuming the closest`).

**What cannot be carried** is the old behaviour (the plain C return), said once in the log and recorded in the
discovery file as a `stack_switch` entry, so the compatibility matrix and the app name the blocker:

| `why` | When |
|---|---|
| `resumes where no context is parked and no function starts` | the ret goes into the middle of a function and no context is parked there (also recorded as an `icall` entry: if it is in fact a function the next prepare seeds it) |
| `no ret after the esp load in the function` | the function leaves by a jump after the switch (lifter: `recomp_stack_switch_unhandled`) |
| `a new stack returned into code with no function` | a new context's chain of returns reached an address with no function (the process stops: there is nothing to return to) |
| `this host has no stack switch` | Windows and 32-bit hosts (no host switch written for them yet) |
| `no host stack for a new context` | `mmap` failed |

Log lines: `[STACK] context N: new guest stack at esp ..., starts at ... (switch at ...; K started)` for the first four
and then every power of two.

## Limits

- **setjmp / longjmp.** A guest `longjmp` is a native `longjmp` to the buffer taken at the guest `setjmp`
  (`recomp_setjmp_slot`, `recomp_guest_longjmp` in `xbox_memory_layout.c`). A native longjmp is only valid on the host
  stack that armed the buffer, so each slot names its context (`recomp_stack_context()`): a jump to a buffer armed on
  another guest stack is refused, with `[STACK] longjmp to jmp_buf ... armed on host stack A from host stack B`, and
  the guest's own longjmp runs instead (as for a buffer with no native counterpart: it moves esp but leaves the C
  frames). A jump only drops the inner buffers of its own context; the others' frames are still alive, parked.
- **SEH resumption must not cross host stacks either.** Resuming into a lifted function after an exception (XBA-112:
  a `_setjmp` anchor per EH frame, `longjmp` to it) is the same native longjmp and has the same limit: an exception
  raised on one guest stack whose handler frame lives on another (a handler registered by the fiber's creator, reached
  through a chain that spans stacks) cannot be resumed natively. Every switch therefore hands the dispatcher the
  context id as its host stack token (`xbox_SehSetHostStack`) and the guest stack's bounds (`xbox_SehSetGuestStack`:
  the side left keeps its own, a new stack in a heap block gets its top, any other is unknown). Each anchor records
  the token it was taken under, and a resume under another one is refused and reported (`[SEH] resume at ... refused:
  its anchor is on another host stack`), the exception then being unhandled. Within one guest stack nothing changes.
  [seh-handling.md « Other stacks »](seh-handling.md).
- **One host thread.** Contexts never leave the thread they were made on: lifted code caches the address of the
  thread's `g_cpu` across calls (one TLS lookup per function), so a frame resumed on another thread would run on the
  first thread's registers. An engine that resumes a fiber on another thread than the one that last ran it gets a new
  context there (and the parked one is never resumed); none of the Xbox titles we know does it.
- **Contexts are not freed while the thread lives.** The guest never says a fiber is dead; a context is parked until it
  is resumed. At `RECOMP_STACK_SWITCH_MAX` (default 1024) contexts the one parked longest is dropped and its host stack
  reused for the new one (`[STACK] ... dropping the one parked longest`); resuming a dropped one later is the
  « resumes where no context is parked » blocker. A thread that ends gives its stacks back.
- **Host stack size** is fixed per context (`RECOMP_STACK_SWITCH_STACK_MB`): deep recursion on a fiber overflows into
  the guard page (SIGSEGV), where on the thread's own stack it would have had `RECOMP_THREAD_STACK_MB`.

## Switches

| Variable | Default | What it does |
|---|---|---|
| `RECOMP_STACK_SWITCH=0` | on | The behaviour before XBA-101: the switch's ret is a plain C return. Read once. |
| `RECOMP_STACK_SWITCH_STACK_MB=N` | 8 | Host stack of each context, 1 to 256 MB, reserved and committed as used. |
| `RECOMP_STACK_SWITCH_MAX=N` | 1024 | Contexts per thread before the one parked longest is dropped (4 to 65536). |

## Tests

- `tests/stack_switch`: the runtime against guest code in the shape the lifter emits -- two coroutines (10,000
  rounds), copy-stack twins (Fable's shape), a body returning into an exit routine, a switch with nowhere to go
  (recorded), another host thread; and `RECOMP_STACK_SWITCH=0`. x86-64 and arm64 (qemu).
- `tools/recomp/test_stack_switch.py`: what is a switch and what is not, what the lifter emits.
- `tools/compat/test_e2e.py::test_two_coroutines_switch_stacks` (`XBA_E2E=1`): a synthetic XBE of ours with two
  coroutines, through the whole pipeline -- the prepare marks the switch, the runtime starts the second coroutine, the
  first sees it run between each of its 1,000 switches and leaves; with `RECOMP_STACK_SWITCH=0` it spins.
