# SEH and Exception Handling

> Part of the [Documentation Index](../INDEX.md)

Structured Exception Handling in statically recompiled Xbox code: how the guest's exception frames are kept, how exceptions are dispatched through them, and our own use of host fault handlers.

## Background: SEH on Xbox

Xbox games are compiled with the MSVC compiler, which uses Windows Structured Exception Handling (SEH) for `__try`/`__except` blocks. The compiler emits calls to CRT helper functions:

- `__SEH_prolog` (VA 0x00244784): Sets up an exception frame on the stack, installs it in the TIB exception chain (fs:[0x00]), and initializes ebp for the calling function.
- `__SEH_epilog` (VA 0x00244797): Tears down the exception frame, restores the previous handler, and cleans up ebp.

In the original binary, these are standard CRT functions that manipulate the thread's exception handler chain. In recompiled code, they are translated just like any other function -- but with a critical twist.

## The ebp Bridge Problem

`__SEH_prolog` sets up the caller's frame pointer (ebp). On real x86:

```asm
; Caller:
push    ebp                 ; save old ebp
call    __SEH_prolog        ; prolog sets ebp = esp - N
; After return, ebp is set by prolog
mov     eax, [ebp+8]        ; access parameter via new ebp
```

In recompiled code, ebp is a local variable in each function (see [register-model.md](register-model.md)). When the caller calls `__SEH_prolog`, it is invoking a separate C function. That function's local `ebp` is independent from the caller's local `ebp`. The prolog cannot modify the caller's local variable.

### Solution: g_seh_ebp

A global bridge variable connects the two functions:

```c
extern uint32_t g_seh_ebp;
```

The flow:

```c
// In __SEH_prolog (sub_00244784):
void sub_00244784(void) {
    uint32_t ebp;
    // ... set up exception frame on Xbox stack ...
    ebp = esp - N;       // compute frame pointer
    g_seh_ebp = ebp;     // write to bridge
}

// In the caller (generated code):
void sub_001234AB(void) {
    uint32_t ebp;
    PUSH32(esp, ebp);           // push old ebp
    // push other SEH setup data...
    PUSH32(esp, 0xDEAD0000);    // dummy return address
    sub_00244784();              // call __SEH_prolog
    ebp = g_seh_ebp;            // READ BACK the computed ebp

    // ... function body using local ebp as frame pointer ...

    g_seh_ebp = ebp;            // write for epilog
    sub_00244797();              // call __SEH_epilog
    POP32(esp, ebp);            // restore old ebp
}
```

This bridge is the only way to communicate ebp values between functions in the recompiled code, because ebp is deliberately kept local to prevent corruption by FPO functions.

## Exception Frame on the Xbox Stack

`__SEH_prolog` installs an exception frame in the TIB exception chain:

```c
// Simplified __SEH_prolog implementation
void __SEH_prolog_impl(void) {
    // The exception frame is on the Xbox stack:
    // [esp+0] = previous exception handler (from TIB fs:[0])
    // [esp+4] = exception handler function pointer
    // [esp+8] = try level (-1 = no active __try block)

    // Read previous handler from fake TIB
    uint32_t prev = MEM32(0x00);  // fs:[0] → MEM32(0x00)

    // Write new frame
    MEM32(esp + 0) = prev;
    MEM32(esp + 4) = handler_va;
    MEM32(esp + 8) = 0xFFFFFFFF;  // try level: -1 (outside any __try)

    // Install in TIB
    MEM32(0x00) = esp;  // fs:[0] = new frame

    // Set up ebp for caller
    ebp = esp + frame_offset;
    g_seh_ebp = ebp;
}
```

These frames are real: the runtime dispatches guest exceptions through them (next section).

## Dispatch, unwind and resume (XBA-112)

The title's exception machinery is guest code and stays guest code: `__SEH_prolog`, the scope tables, `_except_handler3`, `__CxxFrameHandler` and the funclets they call are all lifted. The kernel's part is `src/kernel/xbox_seh.c`:

- **Dispatch.** `RtlRaiseException`, `ExRaiseException` and `ExRaiseStatus` (and `xbox_RaiseGuestException()`, the entry point for runtime-detected faults such as XBA-110's divide helper) build an `EXCEPTION_RECORD` and an Xbox `CONTEXT` below the guest esp and walk the chain at `fs:[0]`, calling each handler as `handler(record, frame, context, dispatcher_context)`. `ExceptionContinueExecution` puts the context's integer registers back and returns from the raise; `ExceptionContinueSearch` goes on; `ExceptionNestedException` and a raise inside a handler set `EXCEPTION_NESTED_CALL` as NT does. An unhandled exception is logged and the raise returns, as before dispatch existed. `RECOMP_SEH_DISPATCH=0` turns dispatch off.
- **Unwind.** `RtlUnwind` (`xbox_SehUnwind`) pops the frames above the target and calls each handler with `EXCEPTION_UNWINDING`, so `__finally` blocks and destructors run.
- **Every record is checked** before it is followed: 4-byte aligned, on the current thread's guest stack (`xbox_SehSetGuestStack`, set when a guest thread starts; a worker stack slice taken with `xbox_worker_stack_alloc` names its own exact bounds until it is freed, XBA-132) and above its esp, and climbing. A broken chain ends the walk; the exception is then unhandled, and said so.

### Funclets and resume targets in lifted code

A handler enters its function in places the function's own control flow never reaches: it *calls* funclets (an `__except` filter, a `__finally`, a catch block, a destructor action) and *jumps* to resume targets (an `__except` block, a catch continuation). The lifter reads the frame each function registers and the compiler's tables behind it (`tools/recomp/seh_frames.py`): the scope table pushed before `__SEH_prolog` or in an inline EH3 frame, and the FuncInfo behind a C++ `__ehhandler` stub. Those blocks are kept and labelled instead of being dropped as unreachable.

- **Funclets** are entries like any function: the function is emitted once more as `<name>__seh(target)`, entered with a `goto` at the funclet, and each funclet gets a `void sub_<va>(void)` in the dispatch table. A handler's indirect call and the function's own direct call to its `__finally` both reach it.
- **Resume targets** need the C frames between the raise and the function to be left for real (each lifted function is a C function). A function with somewhere to resume takes a native `_setjmp` "anchor" right after it registers its frame (`recomp_seh_anchor`, keyed by the registration record and the function's ebp). A handler's jump to a resume target misses every lookup and reaches `recomp_lookup_kernel`, whose fallback (`xbox_SehLookup`) finds the live anchor and longjmps back to it with the target's index; the function goes to the label. Only functions with an `__except` or a catch take an anchor; `--no-seh-anchors` leaves them out, and a resume is then refused at run time.

The anchor costs about 6 ns per call of such a function on a Ryzen AI MAX+ 395 (4 ns for the frame setup without it); `tests/seh_dispatch` builds `seh_anchor_bench` to measure it elsewhere. `_setjmp` rather than `setjmp`, so bionic does not add a `sigprocmask` system call.

### Other stacks: refuse, never jump

A guest stack switch (fibers, XBA-101) gives each guest stack its own host stack, and a longjmp must never cross host stacks. So:

- the chain walk only follows records on the **current guest stack**: a record on another one ends the walk, is reported (`[SEH] dispatch: frame ... is on another guest stack`) and the exception is unhandled, without calling that frame's handler;
- a resume only lands on an anchor taken under the **current host stack token** (`xbox_SehSetHostStack`) and whose frame is still on the chain. Otherwise it is refused and reported (`[SEH] resume at ... refused: its anchor is on another host stack`), the handler returns into the dispatcher and the raise is unhandled. What the handler already did (its global unwind) stays done.

Both come from the switch itself: `recomp_stack_switch_ret` (`src/kernel/recomp_stack_switch.c`) hands the dispatcher, on every switch, the context id it switches to as the host stack token and that guest stack's bounds. The side it leaves keeps its bounds (the thread's own, for the thread's stack) and gets them back when it is resumed. A new guest stack's top is known when the stack is a heap block (`xbox_HeapAlloc`): records above it are on another stack (its bottom need not be known, records below esp are dead). Otherwise its bounds are unknown and the walk falls back to its window of 1 MB above esp, so a handler whose frame is on another stack may be called, but its resume is still refused by the token. `tests/seh_dispatch` raises and resumes on a switched-to stack, and raises on a fiber whose handler frames are on the stack that switched to it, both ways.

### Divide errors

A lifted `div`/`idiv` whose divisor is zero or whose quotient does not fit calls `recomp_divide_error` (XBA-110, `src/kernel/kernel_hal.c`) with no register changed. When the thread has a guest handler chain it raises `STATUS_INTEGER_DIVIDE_BY_ZERO` or `STATUS_INTEGER_OVERFLOW` there through `xbox_RaiseGuestException`, the exception address being the `div`. A handler that resumes at its `__except` block never comes back. One that continues execution does, and the lifted code goes on after the `div` (the CPU would retry it with the registers the handler left; said once in the log). An empty chain, a raise nothing handles, or `RECOMP_SEH_DISPATCH=0` is the console's fatal path: a `[#DE]` report naming the instruction, then `KeBugCheckEx(0x1E, status, va)`.

### setjmp / longjmp

A guest `longjmp` to a buffer the lifted `setjmp` paired with a native one is a native `longjmp` (`recomp_guest_longjmp`, `src/kernel/xbox_memory_layout.c`), which replaces the CRT's `longjmp` body. That body also unwinds the exception frames it leaves: `RtlUnwind` up to the registration `setjmp` saved at `+0x18`. The runtime now does that part before the jump (when dispatch is on, and only if that registration is on the chain or is its end), so their `__finally` blocks and destructors run and `fs:[0]` is the `setjmp`-time head again. Without it, `fs:[0]` kept naming dead frames below esp, which the dispatcher rejects as a broken chain, and the next exception went unhandled.

Then the try levels inside the `setjmp` caller's own frame, for a `longjmp` out of a `__try` of the same function (XBA-132, `xbox_SehLongjmpUnwind`), as the CRT's `longjmp` does after its global unwind: a `_setjmp3` buffer (cookie `'VC20'` at `+0x20`) names its own unwind function at `+0x24`, which the compiler passes to `_setjmp3` for a function with an exception frame (`_seh_longjmp_unwind` for EH3, `__CxxLongjmpUnwind` for C++); it is guest code and the runtime calls it, stdcall with the buffer, ebp being the saved one. An old buffer (no cookie) gets `_local_unwind2(registration, TryLevel at +0x1C)` done natively: each level above the saved one is lowered to its enclosing level and its `__finally` runs with the frame's ebp.

The guest `setjmp` slots and the SEH anchors are separate per-thread tables (keyed by guest `jmp_buf` address and by registration record) and never share an entry. Both carry the host stack they were taken on, and neither jumps across host stacks.

`tests/seh_dispatch` covers all of this on lifted code of our own (fixture.s, regenerated by `tools/recomp/seh_fixture.py`) on x86-64 and arm64.

### Host faults on guest addresses (XBA-132)

Titles fault on purpose: a guard page whose filter commits it and continues, `IsBadReadPtr`-style probes, a `__try` around a pointer they do not trust. On the console those are access violations dispatched through the thread's chain. Here they arrive as SIGSEGV/SIGBUS at the toolkit's handler (`xbox_PosixInstallMmioFaultHandler`); trapped MMIO is served first, then `xbox_SehPosixFault` (`src/kernel/xbox_seh_fault.c`) takes any other address in the guest window when the thread has dispatcher state and a non-empty chain:

1. **No dispatch in the signal handler.** It runs on the alternate signal stack (bionic's is small) and the guest registers may still be in host registers. The handler saves the faulting host context (integer registers, the FP/SIMD state: the x86-64 XSAVE area, the arm64 `fpsimd_context` and SVE record, the signal mask), points the context at a trampoline with the stack pointer just below the interrupted one on the thread's own stack, and returns.
2. **The trampoline** (`seh_fault_tramp`, an ordinary C frame of the faulting thread) raises `STATUS_ACCESS_VIOLATION` with `ExceptionInformation = { 0 read / 1 write / 8 execute, guest address }` (from the page-fault error code on x86-64, the ESR on arm64, the instruction's L bit when qemu-user gives no ESR).
3. **Resume**: a handler that resumes at an `__except` or a catch longjmps to its anchor, above the faulting frame on the same host stack, as for any raise; the trampoline goes with the frames below the anchor.
4. **Continue execution**: the trampoline reads a PROT_NONE page of ours; the handler recognises that fault and copies the saved context over the new one, so sigreturn lands on the faulting instruction with every register as it was, and the access runs again (a guard page the filter committed now succeeds; one it did not faults again and is dispatched again, as on the console). Registers the handler changed in the CONTEXT are not applied (said once).
5. **Nothing handled it**: the same restore, with the next fault at that pc and address marked to go down the chain. The access faults again and ends as before this existed: the previous handler with the original context (ART's on Android, then debuggerd's tombstone with the original pc and registers), or the default action.

Faults inside a dispatch nest 3 deep (past that they go down the chain); a resume puts back the depth its anchor was taken at. Declined at once, down the chain: `RECOMP_SEH_FAULTS=0` or `RECOMP_SEH_DISPATCH=0`, a host without the per-architecture part (implemented: Linux x86-64, Linux/Android arm64; not Windows, whose VEH path is unchanged; not macOS by default: its arm64 context code has never been compiled or run, so a default build leaves it out and every fault goes down the chain, said once: `[SEH] host faults as guest exceptions: not built on macOS ...`; `-DXBR_SEH_FAULTS_DARWIN=1` in the C flags builds it, XBA-138), an empty chain, a fault inside one of the runtime's own bridges (`kernel_thunk_dispatch` marks the thread for the length of every kernel call, a guest routine a bridge runs is lifted code again: `xbox_SehRuntimeEnter`, `xbox_GuestCpuEnterCallback`; a resume or guest longjmp puts back the state of the frame it lands in), a fault on the alternate signal stack, FP state that cannot be carried back (an XSAVE area over 4 KB, streaming SVE / SME state).

**What the handlers see.** Lifted code keeps the guest registers in `g_cpu` (thread-local memory) and ebp in a C local, and the compiler may hold any of them in host registers between two accesses to that memory. Every guest access may alias the registers: `MEM8`/`MEM32` by the C aliasing rules, `MEM16`/`SMEM16`/`SMEM64`/`MEMF`/`MEMD` through `may_alias` types (`recomp_types.h`, XBA-138; before, gcc and clang sank or merged an `esp -= 8` past a `MEM16` load). So at a faulting **load** every earlier register write is in memory: eax..edi, esp and ebp (the frame published by `mov ebp, esp`) are exact. At a faulting **store**, of any width, a register written before it and overwritten after it without being read in between may hold its older value: C assumes a store cannot trap, so the first write is dropped as dead. Registers read after the store (esp across a push) are exact; esp can be too high by a dropped allocation (`sub esp; store; mov esp, ebp`), so the dispatch builds its frames 8 KB below the published esp (MSVC calls `_chkstk` for any frame of a page or more, so a pending allocation is under 4 KB). `tests/seh_fault_regs` pins both, at -O2 with the real macros. EFlags is the last published image and Eip is 0, as is `ExceptionAddress`: lifted code keeps no guest pc. `_except_handler3` and `__CxxFrameHandler` need none of it: they take the frame from the registration record and the try level from the frame, and the faulting address is in the record.

`tests/seh_dispatch` covers a read and a write fault resumed at `__except` (with the registers checked), a guard page committed by the filter (load and store run again), an unhandled fault reaching the previous handler once with its address, the default action killing the process with SIGSEGV, and `RECOMP_SEH_FAULTS=0`, on x86-64 and arm64 (qemu, whose frames carry SVE data).

### Exception tables: EH3 and the VC7 C++ FuncInfo only

The original Xbox toolchains are Visual C++ 7.0 and 7.1 (XDK compilers). EH4 (`_except_handler4`, scope tables XORed with the security cookie) first shipped with Visual C++ 8.0 (2005), and the C++ FuncInfo magics VC7.x emits are `0x19930520` and `0x19930521` (`0x19930522` is VC8's, also accepted). A retail XBE has no EH4 frames, so `seh_frames.py` does not read them.

## VEH: Our Own Exception Handling

We use Windows Vectored Exception Handlers (VEH) for our own purposes, separate from the game's SEH:

### NV2A Register Access

The NV2A GPU has memory-mapped registers at 0xFD000000+. Game code and XDK libraries read/write these addresses directly. These pages are not mapped by default, so access causes an access violation. The VEH allocates pages on demand:

```c
LONG WINAPI xbox_veh_handler(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t fault_addr = info->ExceptionRecord->ExceptionInformation[1];

    // NV2A registers: 0xFD000000 - 0xFEFFFFFF
    if (fault_addr >= 0xFD000000 && fault_addr < 0xFF000000) {
        uintptr_t page = fault_addr & ~0xFFF;
        if (VirtualAlloc((LPVOID)page, 4096,
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            return EXCEPTION_CONTINUE_EXECUTION;  // retry the instruction
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;  // not our problem
}

// Install during init:
AddVectoredExceptionHandler(1, xbox_veh_handler);  // first handler
```

### DO NOT Handle Division by Zero

Early in development, we added DIV0 handling to the VEH:

```c
// WRONG - do not do this:
if (info->ExceptionRecord->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO) {
    // Skip the faulting instruction
    info->ContextRecord->Eip += instruction_length;
    info->ContextRecord->Eax = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
```

This interfered with the game's own SEH recovery. Several functions in the game intentionally divide by values that can be zero, relying on their `__except` handler to catch the fault and take an alternate code path. When our VEH intercepts the DIV0 first, the game's SEH handler never fires, and the function continues with a bogus result instead of taking the recovery path.

**Rule**: Only handle exceptions in VEH that cannot possibly be handled by the game's SEH (like NV2A register faults, which are entirely our invention).

## Corrupted Vtables and Missing SEH Protection

On the real Xbox, when the game calls through a corrupted vtable pointer, the access violation is caught by an SEH handler up the call chain, and execution continues at the `__except` block. The game was designed to tolerate these faults.

In recompiled code, these vtable calls go through RECOMP_ICALL, which:
1. Reads the vtable pointer from Xbox memory (may be garbage).
2. Reads the function pointer from the vtable (access violation if the vtable address is unmapped).
3. Calls the function (if the function address is garbage, dispatch fails).

Problem: Step 2 can crash in native code (reading from an unmapped address), and our VEH does not handle this because it is in the [0x400000, 0xFE000000) range where we do not allocate pages.

### Solutions

**Centralized ICALL early-out** (preferred): Check the VA range before dereferencing:

```c
if (_va >= 0x00400000 && _va < 0xFE000000) {
    g_esp += 4; eax = 0; break;  // garbage VA, skip
}
```

**Per-function guards** (for specific known-bad call sites):

```c
// Before a vtable ICALL in sub_001B4170:
uint32_t vtable = MEM32(esi);
uint32_t method = MEM32(vtable + 0x10);
if (method >= 0x00400000 && method < 0xFE000000) {
    goto skip_this_call;  // corrupted vtable, skip
}
PUSH32(esp, 0);
RECOMP_ICALL(method);
```

**Guest SEH** (XBA-112, XBA-132): a host fault on a guest address is raised as a guest access violation, so the title's own `__except` handles these the way it does on the console (Linux x86-64 and Linux/Android arm64 hosts, not macOS yet; see « Host faults on guest addresses »).

## The Exception Handler Chain

The fake TIB at address 0x00 maintains an exception handler chain:

```
MEM32(0x00) = pointer to top-most exception frame
                  |
                  v
            [frame N]
            prev → [frame N-1]
                        prev → [frame N-2]
                                    prev → 0xFFFFFFFF (end of chain)
```

`__SEH_prolog` pushes new frames. `__SEH_epilog` pops them. The dispatcher and `RtlUnwind` walk it (see above). The TIB is per thread (`xbox_AllocThreadTib`), so each guest thread has its own chain.

## Summary

| Mechanism | Purpose | Status |
|-----------|---------|--------|
| g_seh_ebp bridge | Communicate ebp between __SEH_prolog and callers | Working |
| Fake TIB at VA 0x0 | Exception chain anchor, stack bounds, TLS | Working |
| VEH for NV2A pages | Allocate GPU register pages on demand | Working |
| VEH for DIV0 | DO NOT DO THIS -- interferes with game's SEH | Removed |
| ICALL range check | Skip garbage vtable pointers | Working (centralized + per-function) |
| Guest SEH dispatch | Raise, walk fs:[0], call lifted handlers, unwind | Working (XBA-112) |
| Resume anchors | __except / catch continuation across C frames | Working; `--no-seh-anchors` |
| Stack switches | token and guest stack bounds on every switch, no cross-stack resume | Working (XBA-101 x XBA-112) |
| Divide errors | #DE raised as STATUS_INTEGER_DIVIDE_BY_ZERO / _OVERFLOW | Working (XBA-110 x XBA-112) |
| Guest longjmp | unwinds the SEH frames it leaves, then the setjmp caller's try levels | Working (XBA-112, XBA-132) |
| Host faults as guest exceptions | Access violation on a guest address | Working on POSIX hosts (XBA-132); `RECOMP_SEH_FAULTS=0` |
| Worker stack slices | exact guest stack bounds while a thread runs on one | Working (XBA-132) |

The key lesson: in a static recompilation, you inherit the game's exception handling strategy but cannot use the same mechanism (real SEH on a real x86 stack). You must find alternative approaches -- VEH for hardware faults, range checks for corrupted pointers, and bridge variables for inter-function communication.
