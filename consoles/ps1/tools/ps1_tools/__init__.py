"""The PlayStation static recompiler (XBA-146, XBA-160, XBA-161): PS-X EXE / ISO 9660 input, MIPS I decoding, function
discovery, lifting to C. `python3 -m ps1_tools.recomp INPUT OUTDIR [--discovered FILE]`; see ../README.md and
consoles/ps1/runtime/ps1_recomp.h (the contract).
"""

CONSOLE = "ps1"
