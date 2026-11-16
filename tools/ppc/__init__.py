"""The PowerPC frontend (XBA-225): decoder, function discovery and lifter to C, console-agnostic.

Written for the GameCube (consoles/gc/tools/gc_tools adds the Gekko's paired singles and quantised loads / stores,
and the DOL format) and meant to be reused by the Xbox 360 (XBA-229: Xenon's VMX / VMX128 and the 64-bit forms would
be another layer on top, XEX2 another image format). Modules:

- isa.py: `Decoder` (32-bit PowerPC of the 750 family), `Insn`; opcodes 4, 56, 57, 60, 61 are left to the layer.
- discover.py: recursive descent from seeds to functions (`Discovery`), console-agnostic image interface.
- lift.py: `FnLifter`, one C function per discovered function in a macro vocabulary the console's runtime header
  defines (consoles/gc/runtime/gc_recomp_rt.h for the GameCube); semantics helpers in
  consoles/gc/runtime/ppc_ops.h, shared with the interpreter.

Tests: python3 -m pytest tools/ppc (synthetic code, no toolchain).
"""
