"""The Nintendo 64 pipeline (XBA-169 / XBA-170): ROM parse and boot (rom.py), MIPS III decoder (isa.py), function
discovery without symbols (discover.py, dma.py), the lifter to C (lift.py) and the CLI that writes a title pack's
sources (recomp.py: `python3 -m n64_tools.recomp ROM OUTDIR [--discovered FILE]`). Pure Python (it runs in the app's
embedded CPython). See consoles/n64/tools/README.md and consoles/n64/runtime/n64_recomp.h (the contract).
"""

CONSOLE = "n64"
